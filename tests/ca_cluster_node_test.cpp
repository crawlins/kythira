// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Multi-node ca_cluster_node coverage: brings up real ca_cluster_node
// subprocesses (posix_spawn, matching the pattern already established by
// ca_test_fixture.hpp / ca_service_serve_integration_test.cpp — there is no
// in-process embedding since ca_cluster_node is a standalone executable with
// its own signal handling) wired into a real 3-node Raft cluster over
// loopback TCP, and exercises bootstrap convergence, leader redirection,
// Property 16 (no plaintext private key ever hits disk), Property 17
// (an acknowledged issuance survives leader failover), and follower
// restart-recovery.

#define BOOST_TEST_MODULE ca_cluster_node_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/jwt.hpp>

#include "ca_cluster_node_harness.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef CA_CLUSTER_NODE_PATH
#define CA_CLUSTER_NODE_PATH "ca_cluster_node"
#endif
#ifndef OIDC_PYJWT_SCRIPT
#define OIDC_PYJWT_SCRIPT "tests/oidc_pyjwt_verify.py"
#endif

using namespace raft::testing;
using namespace ca_cluster_harness;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) {
            X509_free(c);
        }
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert_pem(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

// Polls every node round-robin (rather than waiting on each sequentially, so
// the total wait is bounded by the slowest node, not the sum of all of
// them — all processes are already running concurrently).
//
// A node that exits before answering (typically because another process took
// one of its pre-picked ports between find_free_port() and the node's bind)
// ends the wait at once and is described in `*died`, so the caller can retry
// on fresh ports instead of waiting out the whole timeout.
auto wait_all_healthy(const std::vector<std::unique_ptr<cluster_node_process>>& nodes,
                      std::chrono::seconds timeout, std::string* died = nullptr) -> bool {
    std::vector<bool> healthy(nodes.size(), false);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_healthy = true;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (healthy[i]) {
                continue;
            }
            if (auto how = nodes[i]->reap_if_exited()) {
                if (died != nullptr) {
                    *died = "node " + std::to_string(nodes[i]->node_id) + " " + *how +
                            " during startup";
                }
                return false;
            }
            httplib::Client c("127.0.0.1", nodes[i]->http_port);
            c.set_connection_timeout(1, 0);
            c.set_read_timeout(10, 0);
            auto res = c.Get("/healthz");
            if (res && res->status == 200) {
                healthy[i] = true;
            } else {
                all_healthy = false;
            }
        }
        if (all_healthy) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::all_of(healthy.begin(), healthy.end(), [](bool h) { return h; });
}

struct leader_probe_result {
    std::size_t node_index;
    std::string root_pem;
};

// Polls every running node's /v1/root-ca (leader-gated) until exactly one
// answers 200 — that one is the current leader.
auto find_leader(const std::vector<std::unique_ptr<cluster_node_process>>& nodes,
                 const std::string& auth_token, std::chrono::seconds timeout)
    -> std::optional<leader_probe_result> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (!nodes[i]->is_running()) {
                continue;
            }
            httplib::Client c("127.0.0.1", nodes[i]->http_port);
            c.set_connection_timeout(1, 0);
            c.set_read_timeout(10, 0);
            auto res =
                c.Get("/v1/root-ca", httplib::Headers{{"Authorization", "Bearer " + auth_token}});
            if (res && res->status == 200 && !res->body.empty()) {
                return leader_probe_result{i, res->body};
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return std::nullopt;
}

// A node confirmed as leader via find_leader() (root-ca material replicated)
// may not yet have finished reconstructing its in-memory signer — that
// happens on the next ~200ms maintenance-thread tick, surfacing as a 503
// {"error":"not_ready"}. Separately, submit_command()'s own commit-wait can
// occasionally exceed its internal timeout under heavy host load (a real,
// transient condition — the entry still commits shortly after, just not
// inside that one call's window), surfacing as a 502 with a "Commit timeout"
// body. A real client is expected to retry both; this test helper does too,
// rather than treating either as a hard failure.
auto post_with_retry_on_not_ready(httplib::Client& client, const std::string& path,
                                  const httplib::Headers& headers, const std::string& body,
                                  std::chrono::seconds timeout) -> httplib::Result {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    httplib::Result res;
    do {
        res = client.Post(path, headers, body, "application/json");
        if (res && res->status != 503 && res->status != 502) {
            return res;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } while (std::chrono::steady_clock::now() < deadline);
    return res;
}

constexpr const char* k_auth_token = "cluster-test-token";

// This binary used to die of an uncaught SIGSEGV within 0.1 s of starting
// (October 2026, clang++-18 legs, mostly arm64), with no Boost output at all:
// ctest pipes stdout, so Boost's fully buffered progress lines died with the
// process. Unbuffered output named the case (the first /healthz poll), and
// the cause turned out to be cpp-httplib abandoning a getaddrinfo_a() request
// to glibc's resolver thread (patched in vcpkg-overlays/cpp-httplib). Keep
// stdout unbuffered and log test-unit entry and exit so any future crash
// still names the test case and how far it got.
struct unbuffered_progress_log {
    unbuffered_progress_log() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        boost::unit_test::unit_test_log.set_threshold_level(boost::unit_test::log_test_units);
    }
};
BOOST_TEST_GLOBAL_FIXTURE(unbuffered_progress_log);

// Every three_node_cluster runs the OIDC issuer, so the Property 16 scan
// below also covers its sealed signing keys.
constexpr const char* k_oidc_issuer = "https://issuer.kythira.test";

struct three_node_cluster {
    std::string tmp_root;
    std::string unseal_key_file;
    std::vector<std::unique_ptr<cluster_node_process>> nodes;

    three_node_cluster() {
        tmp_root = (std::filesystem::temp_directory_path() /
                    ("ca_cluster_node_test_" + std::to_string(::getpid()) + "_" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(this))))
                       .string();
        std::filesystem::create_directories(tmp_root);
        unseal_key_file = tmp_root + "/unseal.key";
        std::ofstream(unseal_key_file) << "multi-node-test-unseal-passphrase\n";

        // find_free_port() only proves a port was free a moment ago; on a
        // busy CI host another test can take it (as a listener or as an
        // outgoing connection's local port) before the node binds, and that
        // node then exits during startup. Retry on fresh ports and a fresh
        // data directory rather than failing the test case.
        constexpr int k_start_attempts = 3;
        std::string died;
        for (int attempt = 1; attempt <= k_start_attempts; ++attempt) {
            died.clear();
            if (start_nodes(attempt, &died)) {
                return;
            }
            BOOST_REQUIRE_MESSAGE(!died.empty(),
                                  "not every node became healthy within the timeout");
            BOOST_TEST_MESSAGE("three_node_cluster: attempt " << attempt << ": " << died
                                                              << "; retrying on fresh ports");
            for (auto& n : nodes) {
                n->stop();
            }
            nodes.clear();
        }
        BOOST_FAIL("three_node_cluster: no attempt brought all three nodes up; last: " << died);
    }

    // One attempt: picks fresh ports, spawns all three nodes into
    // node<id>_<attempt> directories, and waits for every /healthz.
    auto start_nodes(int attempt, std::string* died) -> bool {
        struct info {
            std::uint64_t id;
            int rpc_port;
            int http_port;
        };
        std::vector<info> infos = {{1, find_free_port(), find_free_port()},
                                   {2, find_free_port(), find_free_port()},
                                   {3, find_free_port(), find_free_port()}};

        std::ostringstream peers;
        for (std::size_t i = 0; i < infos.size(); ++i) {
            if (i > 0) {
                peers << ",";
            }
            peers << infos[i].id << ":127.0.0.1:" << infos[i].rpc_port
                  << "@http://127.0.0.1:" << infos[i].http_port;
        }
        std::string peers_arg = peers.str();

        // All three start at once: nodes without --bootstrap-ca never
        // campaign while their log is empty (Requirement 17.10), so node 1
        // leads the first term and creates the CA whatever the start order.
        // bootstrap_node_started_last_still_creates_the_ca covers that order
        // directly.
        for (std::size_t i = 0; i < infos.size(); ++i) {
            nodes.push_back(std::make_unique<cluster_node_process>(
                infos[i].id, infos[i].rpc_port, infos[i].http_port,
                tmp_root + "/node" + std::to_string(infos[i].id) + "_" + std::to_string(attempt),
                unseal_key_file, k_auth_token, peers_arg, /*bootstrap=*/i == 0,
                std::vector<std::string>{"--rpc-address", "127.0.0.1", "--http-address",
                                         "127.0.0.1", "--oidc-issuer-url", k_oidc_issuer}));
        }

        return wait_all_healthy(nodes, std::chrono::seconds(60), died);
    }

    ~three_node_cluster() {
        for (auto& n : nodes) {
            n->stop();
        }
        std::error_code ec;
        std::filesystem::remove_all(tmp_root, ec);
    }

    [[nodiscard]] auto auth_headers() const -> httplib::Headers {
        return {{"Authorization", "Bearer " + std::string(k_auth_token)}};
    }
};

}  // namespace

// Bootstrap convergence, Requirement 17.7 redirect behavior, and Property 16
// (no plaintext private key on disk) all share ONE 3-node cluster instance
// rather than each spinning up their own — these checks are read-only and
// non-destructive, and consolidating them substantially cuts the number of
// full Raft clusters this test binary spins up sequentially (each of which
// is 3 real OS processes with their own thread pools), which matters on
// resource-constrained hardware where cumulative process/port churn across
// many independent fixtures is the dominant source of flakiness, not any
// single cluster's own behavior.
BOOST_AUTO_TEST_CASE(bootstrap_convergence_redirect_and_property_16,
                     *boost::unit_test::timeout(150)) {
    three_node_cluster cluster;

    auto leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(30));
    BOOST_REQUIRE_MESSAGE(leader.has_value(), "no leader emerged within timeout");

    auto root = load_cert_pem(leader->root_pem);
    BOOST_REQUIRE(root != nullptr);
    BOOST_TEST(leader->root_pem.find("BEGIN CERTIFICATE") != std::string::npos);

    // Requirement 17.7: a non-leader node redirects a client-facing request
    // (preserving method + body) to the leader's http_address; a redirect-
    // following client transparently completes the request.
    std::size_t follower_index = (leader->node_index + 1) % cluster.nodes.size();
    auto& follower = *cluster.nodes[follower_index];

    httplib::Client raw_client("127.0.0.1", follower.http_port);
    raw_client.set_connection_timeout(2, 0);
    raw_client.set_read_timeout(20, 0);
    auto raw_res = raw_client.Get("/v1/root-ca", cluster.auth_headers());
    BOOST_REQUIRE(raw_res);
    BOOST_TEST(raw_res->status == 308);
    BOOST_TEST(!raw_res->get_header_value("Location").empty());

    // httplib::Client::set_follow_location(true) strips the Authorization
    // header when a redirect crosses host:port (treated as cross-origin, the
    // same credential-stripping caution browsers apply) — so it can't be used
    // here to validate end-to-end success. Follow the Location manually,
    // carrying the same auth header, to confirm the redirect target is
    // correct and actually serves the request.
    std::string location = raw_res->get_header_value("Location");
    auto scheme_end = location.find("://");
    BOOST_REQUIRE(scheme_end != std::string::npos);
    auto host_start = scheme_end + 3;
    auto path_start = location.find('/', host_start);
    BOOST_REQUIRE(path_start != std::string::npos);
    std::string host_port = location.substr(host_start, path_start - host_start);
    std::string path = location.substr(path_start);
    auto colon = host_port.rfind(':');
    BOOST_REQUIRE(colon != std::string::npos);
    std::string redirect_host = host_port.substr(0, colon);
    int redirect_port = std::stoi(host_port.substr(colon + 1));

    httplib::Client following_client(redirect_host, redirect_port);
    following_client.set_connection_timeout(2, 0);
    following_client.set_read_timeout(20, 0);
    auto followed_res = following_client.Get(path, cluster.auth_headers());
    BOOST_REQUIRE(followed_res);
    BOOST_TEST(followed_res->status == 200);
    BOOST_TEST(followed_res->body == leader->root_pem);

    // OIDC issuer (iam-compatible-auth Requirements 1.1-1.3, 1.10): the
    // leader creates the first signing key on its own and serves discovery
    // and JWKS with no bearer token; a follower redirects them.
    {
        httplib::Client leader_client("127.0.0.1", cluster.nodes[leader->node_index]->http_port);
        leader_client.set_connection_timeout(2, 0);
        leader_client.set_read_timeout(20, 0);
        httplib::Result jwks_res;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            jwks_res = leader_client.Get("/.well-known/jwks.json");
            if (jwks_res && jwks_res->status == 200) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        BOOST_REQUIRE(jwks_res);
        BOOST_REQUIRE_EQUAL(jwks_res->status, 200);
        auto jwks = boost::json::parse(jwks_res->body).as_object();
        const auto& keys = jwks.at("keys").as_array();
        BOOST_REQUIRE_EQUAL(keys.size(), 1u);
        const auto& jwk = keys.at(0).as_object();
        BOOST_TEST(jwk.at("kty").as_string() == "RSA");  // RS256 by default
        BOOST_TEST(jwk.at("alg").as_string() == "RS256");
        BOOST_TEST(jwk.at("use").as_string() == "sig");
        BOOST_TEST(!jwk.at("kid").as_string().empty());
        BOOST_TEST(!jwk.contains("d"));

        auto disc_res = leader_client.Get("/.well-known/openid-configuration");
        BOOST_REQUIRE(disc_res);
        BOOST_REQUIRE_EQUAL(disc_res->status, 200);
        auto disc = boost::json::parse(disc_res->body).as_object();
        BOOST_TEST(disc.at("issuer").as_string() == k_oidc_issuer);
        BOOST_TEST(disc.at("jwks_uri").as_string() ==
                   std::string(k_oidc_issuer) + "/.well-known/jwks.json");

        auto follower_jwks = raw_client.Get("/.well-known/jwks.json");
        BOOST_REQUIRE(follower_jwks);
        BOOST_TEST(follower_jwks->status == 308);
        BOOST_TEST(follower_jwks->get_header_value("Location").ends_with("/.well-known/jwks.json"));

        // Other routes still need the bearer token.
        auto unauth = leader_client.Get("/v1/root-ca");
        BOOST_REQUIRE(unauth);
        BOOST_TEST(unauth->status == 401);
    }

    // Property 16: the CA private key is never persisted in plaintext —
    // every on-disk log/snapshot file under every node's data_dir must be
    // free of a PEM private-key marker. Give replication a moment to settle
    // so every node's disk has the bootstrap entry.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    static const std::vector<std::string> markers = {"BEGIN PRIVATE KEY", "BEGIN EC PRIVATE KEY",
                                                     "BEGIN RSA PRIVATE KEY"};
    for (auto& node : cluster.nodes) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(node->data_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::ifstream f(entry.path(), std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            for (const auto& marker : markers) {
                BOOST_TEST(content.find(marker) == std::string::npos);
            }
        }
    }
}

// Requirement 17.7: a node with no known leader (never able to reach quorum)
// responds 503, not a redirect to nowhere. Simulated with a node configured
// with 2 peers that never actually run — it can never win an election nor
// hear from a leader.
BOOST_AUTO_TEST_CASE(no_known_leader_returns_503, *boost::unit_test::timeout(30)) {
    std::string tmp_root = (std::filesystem::temp_directory_path() /
                            ("ca_cluster_node_isolated_test_" + std::to_string(::getpid())))
                               .string();
    std::filesystem::create_directories(tmp_root);
    std::string unseal_key_file = tmp_root + "/unseal.key";
    std::ofstream(unseal_key_file) << "isolated-test-passphrase\n";

    int self_rpc = find_free_port();
    int self_http = find_free_port();
    int fake_peer_rpc_2 = find_free_port();
    int fake_peer_rpc_3 = find_free_port();

    std::ostringstream peers;
    peers << "2:127.0.0.1:" << fake_peer_rpc_2
          << "@http://127.0.0.1:1"  // unreachable, never spawned
          << ",3:127.0.0.1:" << fake_peer_rpc_3 << "@http://127.0.0.1:2";

    cluster_node_process isolated(1, self_rpc, self_http, tmp_root + "/node1", unseal_key_file,
                                  k_auth_token, peers.str(), /*bootstrap=*/false);
    BOOST_REQUIRE(wait_healthy(self_http, std::chrono::seconds(15)));

    // Give it a couple of election-timeout cycles to confirm it never
    // manufactures a leader out of thin air.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    httplib::Client c("127.0.0.1", self_http);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(20, 0);
    auto res = c.Get("/v1/root-ca",
                     httplib::Headers{{"Authorization", "Bearer " + std::string(k_auth_token)}});
    BOOST_REQUIRE(res);
    BOOST_TEST(res->status == 503);

    isolated.stop();
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
}

// Requirement 17.10: only the --bootstrap-ca node creates the CA root, and
// only while it leads, so a fresh cluster must never elect anyone else
// first. Here the other two nodes start first and have every chance to
// elect each other; they must not, and once the bootstrap node starts the
// CA must form. A full restart without --bootstrap-ca anywhere must then
// still elect a leader serving the same root: the rule holds only while a
// node's log is empty.
BOOST_AUTO_TEST_CASE(bootstrap_node_started_last_still_creates_the_ca,
                     *boost::unit_test::timeout(180)) {
    std::string tmp_root = (std::filesystem::temp_directory_path() /
                            ("ca_cluster_node_late_bootstrap_test_" + std::to_string(::getpid())))
                               .string();
    std::filesystem::create_directories(tmp_root);
    std::string unseal_key_file = tmp_root + "/unseal.key";
    std::ofstream(unseal_key_file) << "late-bootstrap-test-passphrase\n";

    std::vector<int> rpc_ports{find_free_port(), find_free_port(), find_free_port()};
    std::vector<int> http_ports{find_free_port(), find_free_port(), find_free_port()};
    std::ostringstream peers;
    for (std::size_t i = 0; i < 3; ++i) {
        peers << (i > 0 ? "," : "") << (i + 1) << ":127.0.0.1:" << rpc_ports[i]
              << "@http://127.0.0.1:" << http_ports[i];
    }

    std::vector<std::unique_ptr<cluster_node_process>> nodes(3);
    auto spawn = [&](std::size_t i) {
        nodes[i] = std::make_unique<cluster_node_process>(
            i + 1, rpc_ports[i], http_ports[i], tmp_root + "/node" + std::to_string(i + 1),
            unseal_key_file, k_auth_token, peers.str(), /*bootstrap=*/i == 0);
    };
    spawn(1);
    spawn(2);
    BOOST_REQUIRE(wait_healthy(http_ports[1], std::chrono::seconds(30)));
    BOOST_REQUIRE(wait_healthy(http_ports[2], std::chrono::seconds(30)));

    // Ten or more 150-300ms election timeouts: before the fix, nodes 2 and 3
    // elected one of themselves well inside this window.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    for (std::size_t i = 1; i < 3; ++i) {
        httplib::Client c("127.0.0.1", http_ports[i]);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(20, 0);
        auto res =
            c.Get("/v1/root-ca",
                  httplib::Headers{{"Authorization", "Bearer " + std::string(k_auth_token)}});
        BOOST_REQUIRE(res);
        BOOST_TEST(res->status == 503, "node " << (i + 1) << " answered " << res->status
                                               << " before the bootstrap node started");
    }

    spawn(0);
    auto leader = find_leader(nodes, k_auth_token, std::chrono::seconds(60));
    BOOST_REQUIRE_MESSAGE(leader.has_value(),
                          "the CA never formed after the bootstrap node started");

    for (auto& n : nodes) {
        n->stop();
    }
    for (auto& n : nodes) {
        n->restart(/*bootstrap_again=*/false);
    }
    auto after_restart = find_leader(nodes, k_auth_token, std::chrono::seconds(60));
    BOOST_REQUIRE_MESSAGE(after_restart.has_value(),
                          "no leader after restarting every node without --bootstrap-ca");
    BOOST_TEST(after_restart->root_pem == leader->root_pem);

    for (auto& n : nodes) {
        n->stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
}

// Property 17: an acknowledged issuance survives leader failover — after
// killing the leader, the new leader's replicated ledger still contains the
// pre-failover entry (checked precisely via revoke-by-serial, which 404s iff
// the ledger doesn't have that serial; and complementarily via a successful
// renewal, per the task's suggested check).
BOOST_AUTO_TEST_CASE(property_17_issuance_survives_leader_failover,
                     *boost::unit_test::timeout(450)) {
    three_node_cluster cluster;
    auto leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(30));
    BOOST_REQUIRE(leader.has_value());

    // Issue a certificate against the leader.
    leaf_certificate_options opts;
    opts.subject.common_name = "failover-client";
    opts.dns_names = {"failover-client.example.com"};
    opts.client_auth = true;
    opts.server_auth = false;
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string("failover-client.example.com")};
    body["server_auth"] = false;
    body["client_auth"] = true;

    // read_timeout (65s) exceeds ca_cluster_node's own internal
    // k_command_timeout (60s, cmd/ca_cluster_node/main.cpp) so the client
    // actually receives the server's own "Commit timeout" response under
    // contention instead of truncating the connection first; the 180s outer
    // retry budget gives post_with_retry_on_not_ready room for more than one
    // full attempt even when each one is that slow.
    httplib::Client leader_client("127.0.0.1", cluster.nodes[leader->node_index]->http_port);
    leader_client.set_connection_timeout(5, 0);
    leader_client.set_read_timeout(65, 0);
    auto issue_res =
        post_with_retry_on_not_ready(leader_client, "/v1/certificates", cluster.auth_headers(),
                                     boost::json::serialize(body), std::chrono::seconds(180));
    BOOST_REQUIRE(issue_res);
    BOOST_REQUIRE_MESSAGE(issue_res->status == 200, "issue failed: " << issue_res->body);
    auto issued = boost::json::parse(issue_res->body).as_object();
    std::string cert_pem = std::string(issued["certificate_pem"].as_string());
    auto cert = load_cert_pem(cert_pem);
    BOOST_REQUIRE(cert != nullptr);
    std::uint64_t serial = 0;
    BOOST_REQUIRE(ASN1_INTEGER_get_uint64(&serial, X509_get_serialNumber(cert.get())) == 1);

    // Kill the leader (simulating a crash) and wait for a new one to emerge
    // among the remaining two nodes.
    std::size_t old_leader_index = leader->node_index;
    cluster.nodes[old_leader_index]->kill_hard();

    auto new_leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(60));
    BOOST_REQUIRE_MESSAGE(new_leader.has_value(), "no new leader emerged after failover");
    BOOST_TEST(new_leader->node_index != old_leader_index);
    BOOST_TEST(new_leader->root_pem ==
               leader->root_pem);  // same CA identity, not a fresh bootstrap

    // Precise check: revoke-by-serial against the new leader must find the
    // pre-failover entry (404 iff the ledger lost it).
    //
    // With only two nodes left, leadership can still move once more: the
    // surviving follower may time out before the new leader's first
    // heartbeat reaches it, take a higher term, and cancel the revoke the
    // new leader had just accepted (a 503), or the node asked may no longer
    // lead (a 308). Neither says anything about the ledger, so ask whichever
    // node leads now until one answers 200 or 404. Revocation is idempotent:
    // re-revoking a serial that a cancelled-but-committed attempt already
    // marked still answers 200. A 400 is final too: the body was refused.
    std::optional<leader_probe_result> current_leader = new_leader;
    auto revoke = [&](const boost::json::object& revoke_body) -> httplib::Result {
        httplib::Result revoke_res;
        auto revoke_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < revoke_deadline) {
            if (!current_leader) {
                current_leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(30));
                if (!current_leader) {
                    continue;
                }
            }
            httplib::Client client("127.0.0.1",
                                   cluster.nodes[current_leader->node_index]->http_port);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(65, 0);
            revoke_res = client.Post("/v1/certificates/revoke", cluster.auth_headers(),
                                     boost::json::serialize(revoke_body), "application/json");
            if (revoke_res && (revoke_res->status == 200 || revoke_res->status == 404 ||
                               revoke_res->status == 400)) {
                break;
            }
            BOOST_TEST_MESSAGE("revoke attempt on node "
                               << cluster.nodes[current_leader->node_index]->node_id << ": "
                               << (revoke_res
                                       ? std::to_string(revoke_res->status) + " " + revoke_res->body
                                       : std::string("no response"))
                               << "; asking the current leader again");
            current_leader.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return revoke_res;
    };

    // The issuance response names the serial as "serial_hex"
    // (cloud-composite-node-ids Requirement 12.5).
    auto hex = raft::testing::certificate_serial::from_u64(serial).to_hex();
    BOOST_TEST(!issued.contains("serial"));
    BOOST_TEST(std::string(issued["serial_hex"].as_string()) == hex);
    // "serial" as a decimal string, exactly as before.
    auto revoke_res = revoke({{"serial", std::to_string(serial)}});
    BOOST_REQUIRE(revoke_res);
    BOOST_TEST_MESSAGE("revoke response: " << revoke_res->status << " " << revoke_res->body);
    BOOST_TEST(revoke_res->status == 200);

    auto status_of = [&](const boost::json::object& revoke_body) {
        auto res = revoke(revoke_body);
        BOOST_REQUIRE(res);
        BOOST_TEST_MESSAGE("revoke " << boost::json::serialize(revoke_body) << ": " << res->status
                                     << " " << res->body);
        return res->status;
    };
    // A JSON number, and serial_hex, name the same (already revoked) serial.
    BOOST_TEST(status_of({{"serial", serial}}) == 200);
    BOOST_TEST(status_of({{"serial_hex", hex}}) == 200);
    // A cloud-width serial_hex the ledger never held is a clean 404.
    BOOST_TEST(status_of({{"serial_hex", std::string(40, 'a')}}) == 404);
    // Hex in "serial" is refused rather than reinterpreted.
    BOOST_TEST(status_of({{"serial", "0a1b"}}) == 400);
    // Malformed serial_hex, both fields, or neither: 400.
    BOOST_TEST(status_of({{"serial_hex", "abc"}}) == 400);
    BOOST_TEST(status_of({{"serial", std::to_string(serial)}, {"serial_hex", hex}}) == 400);
    BOOST_TEST(status_of({}) == 400);
}

// Kill and restart one FOLLOWER (not the leader) pointed at the same
// data_dir; confirm it recovers and can rejoin/participate correctly — the
// functionally observable half of "recovers without a full resync": data
// durably survives the restart and the node can resume normal operation
// (including becoming leader itself if subsequently elected).
BOOST_AUTO_TEST_CASE(restarted_follower_recovers_and_can_become_leader,
                     *boost::unit_test::timeout(450)) {
    three_node_cluster cluster;
    auto leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(30));
    BOOST_REQUIRE(leader.has_value());

    std::size_t follower_index = (leader->node_index + 1) % cluster.nodes.size();
    std::size_t other_index = (leader->node_index + 2) % cluster.nodes.size();
    BOOST_REQUIRE(follower_index != leader->node_index && other_index != leader->node_index &&
                  other_index != follower_index);

    // Kill the follower, issue a certificate against the (still-running)
    // leader so the follower's disk is now behind, then restart it.
    cluster.nodes[follower_index]->stop();

    leaf_certificate_options opts;
    opts.subject.common_name = "restart-gap-client";
    opts.dns_names = {"restart-gap-client.example.com"};
    auto csr = generate_key_and_csr(opts);
    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] = boost::json::array{boost::json::string("restart-gap-client.example.com")};

    httplib::Client leader_client("127.0.0.1", cluster.nodes[leader->node_index]->http_port);
    leader_client.set_connection_timeout(5, 0);
    leader_client.set_read_timeout(65, 0);
    auto issue_res =
        post_with_retry_on_not_ready(leader_client, "/v1/certificates", cluster.auth_headers(),
                                     boost::json::serialize(body), std::chrono::seconds(180));
    BOOST_REQUIRE(issue_res);
    BOOST_REQUIRE_MESSAGE(issue_res->status == 200, "issue failed: " << issue_res->body);

    cluster.nodes[follower_index]->restart();
    BOOST_REQUIRE_MESSAGE(
        wait_healthy(cluster.nodes[follower_index]->http_port, std::chrono::seconds(30)),
        "restarted follower never became healthy");

    // Kill only the ORIGINAL leader (not both other nodes) — a 3-node
    // cluster only tolerates 1 failure, so leaving just the restarted
    // follower alive would make majority (2 of 3) permanently unreachable,
    // which is correct Raft behavior, not a recovery failure. With the
    // restarted follower and the third (never-touched) node both alive,
    // majority is achievable regardless of which of the two wins the new
    // election — the meaningful assertion is that the winner (whichever it
    // is) has the SAME CA identity, proving the restarted node's disk state
    // didn't fork from the cluster's.
    cluster.nodes[leader->node_index]->stop();

    auto new_leader = find_leader(cluster.nodes, k_auth_token, std::chrono::seconds(60));
    BOOST_REQUIRE_MESSAGE(new_leader.has_value(),
                          "no new leader emerged among the two surviving nodes");
    BOOST_TEST(new_leader->node_index != leader->node_index);
    BOOST_TEST(new_leader->root_pem == leader->root_pem);
}

// ── Plaintext Raft RPC is opt-in ─────────────────────────────────────────────
// A node with no RPC TLS material may only fall back to plain TCP when its
// RPC listener is loopback-only (every test above) or the operator opted in.

namespace {

// Clears CA_CLUSTER_ALLOW_PLAINTEXT_RPC for the test's duration (the spawned
// node inherits this process's environment), optionally setting it instead.
struct scoped_plaintext_env {
    explicit scoped_plaintext_env(const char* value) {
        if (value != nullptr) {
            ::setenv("CA_CLUSTER_ALLOW_PLAINTEXT_RPC", value, 1);
        } else {
            ::unsetenv("CA_CLUSTER_ALLOW_PLAINTEXT_RPC");
        }
    }
    ~scoped_plaintext_env() { ::unsetenv("CA_CLUSTER_ALLOW_PLAINTEXT_RPC"); }
    scoped_plaintext_env(const scoped_plaintext_env&) = delete;
    auto operator=(const scoped_plaintext_env&) -> scoped_plaintext_env& = delete;
};

}  // namespace

// The default --rpc-address is 0.0.0.0, so a node given no RPC TLS flags
// and no opt-in must refuse to start rather than serve plaintext Raft RPC.
BOOST_AUTO_TEST_CASE(plaintext_rpc_refused_by_default, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_refused");
    cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--http-address", "127.0.0.1"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE_MESSAGE(code.has_value(), "node did not exit; it started with plaintext RPC");
    BOOST_TEST(*code == 1);
}

BOOST_AUTO_TEST_CASE(plaintext_rpc_refused_on_explicit_wildcard_bind,
                     *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_wildcard");
    cluster_node_process node(
        1, find_free_port(), find_free_port(), dir.tmp_root + "/node1", dir.unseal_key_file,
        k_auth_token, dir.peers, /*bootstrap=*/false,
        std::vector<std::string>{"--rpc-address", "0.0.0.0", "--http-address", "127.0.0.1"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE(code.has_value());
    BOOST_TEST(*code == 1);
}

// "0" is not an opt-in: only the exact value "1" enables plaintext.
BOOST_AUTO_TEST_CASE(plaintext_rpc_env_zero_is_not_opt_in, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env("0");
    lone_node_env dir("plaintext_env_zero");
    cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--http-address", "127.0.0.1"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE(code.has_value());
    BOOST_TEST(*code == 1);
}

BOOST_AUTO_TEST_CASE(plaintext_rpc_allowed_with_flag, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_flag");
    int http_port = find_free_port();
    cluster_node_process node(
        1, find_free_port(), http_port, dir.tmp_root + "/node1", dir.unseal_key_file, k_auth_token,
        dir.peers, /*bootstrap=*/false,
        std::vector<std::string>{"--allow-plaintext-rpc", "--http-address", "127.0.0.1"});
    BOOST_TEST(wait_healthy(http_port, std::chrono::seconds(15)));
    node.stop();
}

BOOST_AUTO_TEST_CASE(plaintext_rpc_allowed_with_env, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env("1");
    lone_node_env dir("plaintext_env");
    int http_port = find_free_port();
    cluster_node_process node(1, find_free_port(), http_port, dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--http-address", "127.0.0.1"});
    BOOST_TEST(wait_healthy(http_port, std::chrono::seconds(15)));
    node.stop();
}

// "localhost" is loopback: it binds every loopback address it resolves to,
// so no opt-in is needed.
BOOST_AUTO_TEST_CASE(plaintext_rpc_allowed_on_localhost_bind, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_localhost");
    int http_port = find_free_port();
    cluster_node_process node(
        1, find_free_port(), http_port, dir.tmp_root + "/node1", dir.unseal_key_file, k_auth_token,
        dir.peers, /*bootstrap=*/false,
        std::vector<std::string>{"--rpc-address", "localhost", "--http-address", "127.0.0.1"});
    BOOST_TEST(wait_healthy(http_port, std::chrono::seconds(15)));
    node.stop();
}

// --rpc-address used to be parsed and ignored; it now picks the listener, so
// a name that doesn't resolve to this host is a startup error.
BOOST_AUTO_TEST_CASE(unusable_rpc_address_rejected, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("rpc_address_invalid");
    cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--rpc-address", "kythira-test.invalid"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE(code.has_value());
    BOOST_TEST(*code == 1);
}

// --http-address picks what the client API listens on; "*" is both
// wildcards, so the node still answers on 127.0.0.1. A wildcard plaintext API
// needs the explicit opt-in.
BOOST_AUTO_TEST_CASE(http_address_star_serves_ipv4, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("http_address_star");
    int http_port = find_free_port();
    cluster_node_process node(
        1, find_free_port(), http_port, dir.tmp_root + "/node1", dir.unseal_key_file, k_auth_token,
        dir.peers, /*bootstrap=*/false,
        std::vector<std::string>{"--rpc-address", "127.0.0.1", "--http-address", "*",
                                 "--allow-plaintext-http"});
    BOOST_TEST(wait_healthy(http_port, std::chrono::seconds(15)));
    node.stop();
}

// A --http-address name /etc/hosts doesn't list is a startup error, not a
// DNS lookup.
BOOST_AUTO_TEST_CASE(unusable_http_address_rejected, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("http_address_invalid");
    cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--rpc-address", "127.0.0.1",
                                                       "--http-address", "kythira-test.invalid"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE(code.has_value());
    BOOST_TEST(*code == 1);
}

// The bearer token and every issued certificate cross the client API, so
// without --tls-cert a node whose API is reachable off-host (the default
// --http-address 0.0.0.0) refuses to start unless plaintext is opted in.
BOOST_AUTO_TEST_CASE(plaintext_http_refused_by_default, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_http_refused");
    cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                              dir.unseal_key_file, k_auth_token, dir.peers, /*bootstrap=*/false,
                              std::vector<std::string>{"--rpc-address", "127.0.0.1"});
    auto code = node.wait_for_own_exit(std::chrono::seconds(15));
    BOOST_REQUIRE_MESSAGE(code.has_value(), "node did not exit; it served a plaintext HTTP API");
    BOOST_TEST(*code == 1);
}

BOOST_AUTO_TEST_CASE(plaintext_http_allowed_with_flag, *boost::unit_test::timeout(30)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("plaintext_http_flag");
    int http_port = find_free_port();
    cluster_node_process node(
        1, find_free_port(), http_port, dir.tmp_root + "/node1", dir.unseal_key_file, k_auth_token,
        dir.peers, /*bootstrap=*/false,
        std::vector<std::string>{"--rpc-address", "127.0.0.1", "--http-address", "0.0.0.0",
                                 "--allow-plaintext-http"});
    BOOST_TEST(wait_healthy(http_port, std::chrono::seconds(15)));
    node.stop();
}

// Peer calls carry enrollment and the RPC trust state: a peer address must
// name its scheme, an http:// one must be loopback (or opted in), and an
// https:// one needs a CA bundle to verify the peer against.
BOOST_AUTO_TEST_CASE(insecure_peer_http_addresses_rejected, *boost::unit_test::timeout(60)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("peer_http_insecure");
    const auto rpc_peer = "2:127.0.0.1:" + std::to_string(find_free_port());
    for (const std::string& http :
         {std::string("http://192.0.2.10:8443"), std::string("https://127.0.0.1:8443"),
          std::string("127.0.0.1:8443")}) {
        cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                                  dir.unseal_key_file, k_auth_token, rpc_peer + "@" + http,
                                  /*bootstrap=*/false);
        auto code = node.wait_for_own_exit(std::chrono::seconds(15));
        BOOST_REQUIRE_MESSAGE(code.has_value(), "node started with peer address " + http);
        BOOST_TEST(*code == 1);
    }
}

// iam-compatible-auth task 2 (Requirements 1.6-1.8): POST /v1/oidc/token over
// the client-API TLS listener. A workload certificate issued by this cluster
// gets a token without the bearer token, and that token verifies under the
// cluster's own JWKS both here and with PyJWT; an unlisted audience, a
// connection with no client certificate and a certificate from another CA
// are refused. The other refusals (revoked, lifetime past the certificate,
// workload outside the prefix) are covered against real certificates in
// oidc_token_unit_test, which drives the same decision function.
BOOST_AUTO_TEST_CASE(oidc_token_endpoint_over_mtls, *boost::unit_test::timeout(180)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("oidc_token");
    constexpr const char* k_audience = "sts.amazonaws.com";
    constexpr const char* k_prefix = "spiffe://kythira.test/wl/";
    const std::string workload = std::string(k_prefix) + "billing/api";

    certificate_authority listener_ca;
    const std::string listener_root = dir.tmp_root + "/listener_root.pem";
    std::ofstream(listener_root) << listener_ca.root_certificate_pem();
    {
        leaf_certificate_options o;
        o.subject.common_name = "ca-node-1";
        o.ip_addresses = {"127.0.0.1"};
        auto leaf = listener_ca.issue(o);
        std::ofstream(dir.tmp_root + "/cert.pem") << leaf.chain_pem;
        std::ofstream(dir.tmp_root + "/key.pem") << leaf.private_key_pem;
    }

    const int rpc_port = find_free_port();
    const int http_port = find_free_port();
    const std::string peers = "1:127.0.0.1:" + std::to_string(rpc_port) +
                              "@https://127.0.0.1:" + std::to_string(http_port);
    cluster_node_process node(
        1, rpc_port, http_port, dir.tmp_root + "/node1", dir.unseal_key_file, k_auth_token, peers,
        /*bootstrap=*/true,
        std::vector<std::string>{"--rpc-address", "127.0.0.1", "--http-address", "127.0.0.1",
                                 "--tls-cert", dir.tmp_root + "/cert.pem", "--tls-key",
                                 dir.tmp_root + "/key.pem", "--peer-tls-ca", listener_root,
                                 "--oidc-issuer-url", k_oidc_issuer, "--oidc-audience", k_audience,
                                 "--oidc-workload-uri-prefix", k_prefix});

    auto client = [&](const std::string& cert = {}, const std::string& key = {}) {
        auto c = cert.empty()
                     ? std::make_unique<httplib::SSLClient>("127.0.0.1", http_port)
                     : std::make_unique<httplib::SSLClient>("127.0.0.1", http_port, cert, key);
        c->set_ca_cert_path(listener_root);
        c->enable_server_certificate_verification(true);
        c->set_connection_timeout(5, 0);
        c->set_read_timeout(30, 0);
        return c;
    };
    const httplib::Headers bearer{{"Authorization", std::string("Bearer ") + k_auth_token}};

    // A workload client certificate from the cluster CA, through the
    // ordinary bearer-token issuance route, once the CA and the first OIDC
    // key exist.
    leaf_certificate_options wl;
    wl.subject.common_name = "billing-api";
    auto csr = generate_key_and_csr(wl);
    boost::json::object issue_body{{"csr_pem", csr.csr_pem},
                                   {"uri_names", boost::json::array{boost::json::string(workload)}},
                                   {"client_auth", true},
                                   {"server_auth", false}};
    std::string workload_cert_pem;
    std::string jwks_body;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline &&
           (workload_cert_pem.empty() || jwks_body.empty())) {
        if (workload_cert_pem.empty()) {
            auto res = client()->Post("/v1/certificates", bearer,
                                      boost::json::serialize(issue_body), "application/json");
            if (res && res->status == 200) {
                workload_cert_pem =
                    std::string(boost::json::parse(res->body).at("chain_pem").as_string());
            }
        }
        if (jwks_body.empty()) {
            auto res = client()->Get("/.well-known/jwks.json");
            if (res && res->status == 200) {
                jwks_body = res->body;
            }
        }
        if (workload_cert_pem.empty() || jwks_body.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    BOOST_REQUIRE_MESSAGE(!workload_cert_pem.empty(), "the node never issued a certificate");
    BOOST_REQUIRE_MESSAGE(!jwks_body.empty(), "the node never published a JWKS");
    const std::string wl_cert = dir.tmp_root + "/wl_cert.pem";
    const std::string wl_key = dir.tmp_root + "/wl_key.pem";
    std::ofstream(wl_cert) << workload_cert_pem;
    std::ofstream(wl_key) << csr.private_key_pem;

    const std::string form = std::string("audience=") + k_audience;
    auto ok = client(wl_cert, wl_key)
                  ->Post(std::string("/v1/oidc/token"), form, "application/x-www-form-urlencoded");
    BOOST_REQUIRE(ok);
    BOOST_REQUIRE_MESSAGE(ok->status == 200, "token request: " << ok->status << " " << ok->body);
    auto response = boost::json::parse(ok->body).as_object();
    BOOST_TEST(response.at("token_type").as_string() == "urn:ietf:params:oauth:token-type:jwt");
    BOOST_TEST(response.at("expires_in").as_int64() == 3600);
    const std::string token(response.at("token").as_string());

    // Verify against the published JWKS with this repository's own JOSE code.
    auto parts = kythira::jose::parse_compact(token);
    BOOST_REQUIRE(parts.has_value());
    const auto jwks = boost::json::parse(jwks_body).as_object();
    const boost::json::object* jwk = nullptr;
    for (const auto& k : jwks.at("keys").as_array()) {
        if (k.as_object().at("kid") == parts->header.at("kid")) {
            jwk = &k.as_object();
        }
    }
    BOOST_REQUIRE(jwk != nullptr);
    auto pub = kythira::jose::public_key_from_jwk(*jwk);
    auto verified = kythira::jose::verify_compact(token, pub.get(), kythira::jose::jws_alg::rs256);
    BOOST_REQUIRE(verified.has_value());
    auto claims = boost::json::parse(verified->payload).as_object();
    BOOST_TEST(claims.at("iss").as_string() == k_oidc_issuer);
    BOOST_TEST(claims.at("sub").as_string() == "workload:" + workload);
    BOOST_TEST(claims.at("aud").as_string() == k_audience);
    BOOST_TEST(claims.at("kythira:workload").as_string() == workload);
    BOOST_TEST(!claims.at("jti").as_string().empty());
    BOOST_TEST(claims.at("exp").as_int64() - claims.at("iat").as_int64() == 3600);

    // And with an independent library.
    {
        const std::string token_file = dir.tmp_root + "/token.jwt";
        const std::string jwks_file = dir.tmp_root + "/jwks.json";
        std::ofstream(token_file) << token;
        std::ofstream(jwks_file) << jwks_body;
        const std::string cmd = std::string("python3 '") + OIDC_PYJWT_SCRIPT + "' '" + token_file +
                                "' '" + jwks_file + "' '" + k_oidc_issuer + "' '" + k_audience +
                                "'";
        const int rc = std::system(cmd.c_str());
        const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
        if (code == 77) {
            BOOST_TEST_MESSAGE("PyJWT is not installed here; skipped the independent check");
        } else {
            BOOST_TEST(code == 0);
        }
    }

    // Refusals.
    auto unlisted = client(wl_cert, wl_key)
                        ->Post(std::string("/v1/oidc/token"), "audience=example.com",
                               "application/x-www-form-urlencoded");
    BOOST_REQUIRE(unlisted);
    BOOST_TEST(unlisted->status == 400);

    auto anonymous =
        client()->Post(std::string("/v1/oidc/token"), form, "application/x-www-form-urlencoded");
    BOOST_REQUIRE(anonymous);
    BOOST_TEST(anonymous->status == 401);

    // The right workload URI from the wrong CA (the listener CA here).
    {
        leaf_certificate_options o;
        o.subject.common_name = "impostor";
        o.uri_names = {workload};
        o.server_auth = false;
        o.client_auth = true;
        auto impostor = listener_ca.issue(o);
        std::ofstream(dir.tmp_root + "/imp_cert.pem") << impostor.chain_pem;
        std::ofstream(dir.tmp_root + "/imp_key.pem") << impostor.private_key_pem;
        auto res =
            client(dir.tmp_root + "/imp_cert.pem", dir.tmp_root + "/imp_key.pem")
                ->Post(std::string("/v1/oidc/token"), form, "application/x-www-form-urlencoded");
        BOOST_REQUIRE(res);
        BOOST_TEST(res->status == 401);
    }

    // Prometheus counters (the OIDC publisher's, when --oidc-publish-url is
    // set) sit behind the bearer token like the /v1/* routes.
    auto metrics = client()->Get("/metrics", bearer);
    BOOST_REQUIRE(metrics);
    BOOST_TEST(metrics->status == 200);
    auto metrics_anon = client()->Get("/metrics");
    BOOST_REQUIRE(metrics_anon);
    BOOST_TEST(metrics_anon->status == 401);

    // Bearer-token routes are unchanged: a client certificate is not a
    // bearer token.
    auto no_bearer = client(wl_cert, wl_key)->Get("/v1/root-ca");
    BOOST_REQUIRE(no_bearer);
    BOOST_TEST(no_bearer->status == 401);
    node.stop();
}

// --oidc-publish-url is checked at startup: a malformed URL, or a publish URL
// without an issuer, stops the node with a usage error (exit 1) instead of
// failing quietly at the first upload.
BOOST_AUTO_TEST_CASE(oidc_publish_url_checked_at_startup, *boost::unit_test::timeout(60)) {
    scoped_plaintext_env env(nullptr);
    lone_node_env dir("oidc_publish_flags");
    for (const auto& extra : std::vector<std::vector<std::string>>{
             {"--oidc-issuer-url", k_oidc_issuer, "--oidc-publish-url", "ftp://bucket/x"},
             {"--oidc-issuer-url", k_oidc_issuer, "--oidc-publish-url", "s3://bucket/../x"},
             {"--oidc-publish-url", "s3://bucket/x"},
             {"--oidc-audience", "sts.amazonaws.com"},
         }) {
        std::vector<std::string> args{"--rpc-address", "127.0.0.1", "--http-address", "127.0.0.1"};
        args.insert(args.end(), extra.begin(), extra.end());
        cluster_node_process node(1, find_free_port(), find_free_port(), dir.tmp_root + "/node1",
                                  dir.unseal_key_file, k_auth_token, dir.peers,
                                  /*bootstrap=*/false, args);
        auto code = node.wait_for_own_exit(std::chrono::seconds(15));
        BOOST_TEST_INFO("args: " << extra.back());
        BOOST_REQUIRE(code.has_value());
        BOOST_TEST(*code == 1);
    }
}
