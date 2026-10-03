// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: ca-cluster-rpc-mtls, Property 5: A Restarted, Already-Cutover
// Node Needs No Bootstrap Credential**
// Cuts over a 3-node cluster (bootstrap credential only), deletes the
// bootstrap credential entirely, restarts one node with NEITHER
// --rpc-tls-cert nor --rpc-tls-key given at all, and confirms it still
// rejoins the cluster and the cluster keeps issuing certificates — proving
// the persisted peer certificate under --data-dir (Requirement 7.1) is
// sufficient on its own.
//
// A second case forces every node's peer certificate into its renewal
// window (--rpc-renewal-window-secs just under the 30-day peer validity)
// and confirms each node, leader included, renews it and serves the new
// certificate on its Raft RPC port without a restart (Requirements 7.2,
// 7.3).
// **Validates: Requirements 7.1, 7.2, 7.3**

#define BOOST_TEST_MODULE ca_cluster_node_rpc_tls_restart_test

#include <boost/test/unit_test.hpp>

#include <raft/ca_bootstrap_client.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include "ca_cluster_node_process_wait.hpp"

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
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

extern char** environ;

using namespace raft::testing;

namespace {

auto find_free_port() -> int {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

struct rpc_tls_node_process {
    pid_t pid{-1};
    std::uint64_t node_id;
    int http_port;
    int rpc_port;
    std::string data_dir;
    std::string unseal_key_file;
    std::string auth_token;
    std::string peers_arg;
    std::optional<std::string> bootstrap_cert_path;  // nullopt: no --rpc-tls-cert/key at all
    std::optional<std::string> bootstrap_key_path;
    bool bootstrap;
    std::vector<std::string> extra_args;  // appended to every spawn

    rpc_tls_node_process(std::uint64_t id, int rpc_port_, int http_port_, std::string data_dir_,
                         std::string unseal_key_file_, std::string auth_token_,
                         std::string peers_arg_, std::optional<std::string> bootstrap_cert_path_,
                         std::optional<std::string> bootstrap_key_path_, bool bootstrap_,
                         std::vector<std::string> extra_args_ = {})
        : node_id(id),
          http_port(http_port_),
          rpc_port(rpc_port_),
          data_dir(std::move(data_dir_)),
          unseal_key_file(std::move(unseal_key_file_)),
          auth_token(std::move(auth_token_)),
          peers_arg(std::move(peers_arg_)),
          bootstrap_cert_path(std::move(bootstrap_cert_path_)),
          bootstrap_key_path(std::move(bootstrap_key_path_)),
          bootstrap(bootstrap_),
          extra_args(std::move(extra_args_)) {
        std::filesystem::create_directories(data_dir);
        spawn();
    }

    ~rpc_tls_node_process() { stop(); }

    rpc_tls_node_process(const rpc_tls_node_process&) = delete;
    auto operator=(const rpc_tls_node_process&) -> rpc_tls_node_process& = delete;

    auto spawn() -> void {
        std::vector<std::string> argv_strs = {
            CA_CLUSTER_NODE_PATH,
            "--node-id",
            std::to_string(node_id),
            "--rpc-port",
            std::to_string(rpc_port),
            "--http-port",
            std::to_string(http_port),
            "--data-dir",
            data_dir,
            "--unseal-key-file",
            unseal_key_file,
            "--auth-token",
            auth_token,
            // See ca_cluster_node_rpc_tls_test.cpp's identical comment: RPC
            // TLS's per-call full handshake needs more timeout headroom
            // than the plain-TCP default under real host contention.
            // Every RPC call under RPC TLS pays a full TLS handshake
            // (asymmetric crypto, no session reuse across the per-call-
            // connect transport model — see tls_tcp_rpc.hpp's design
            // comment) on top of a new OS thread per connection on both
            // ends. Confirmed during this spec's implementation: even
            // 600/1200/200/2000ms (election-min/max/heartbeat/rpc) was
            // insufficient to avoid cascading re-elections under this
            // repository's own shared-host CI/dev contention once a
            // second concurrent RPC-heavy operation (record_rpc_tls_ready's
            // submit_command) overlaps with routine heartbeat traffic.
            // These much larger values trade test wall-clock time (still
            // comfortably inside this file's TIMEOUT) for headroom that
            // holds regardless of host load — this test asserts eventual
            // functional convergence, not latency.
            // No client-API TLS listener here, so keep that API on
            // loopback, the one plaintext case allowed without
            // --allow-plaintext-http.
            "--http-address",
            "127.0.0.1",
            "--election-timeout-min-ms",
            "3000",
            "--election-timeout-max-ms",
            "5000",
            "--heartbeat-interval-ms",
            "500",
            "--rpc-timeout-ms",
            "5000",
        };
        if (!peers_arg.empty()) {
            argv_strs.emplace_back("--peers");
            argv_strs.push_back(peers_arg);
        }
        if (bootstrap) {
            argv_strs.emplace_back("--bootstrap-ca");
        }
        if (bootstrap_cert_path.has_value()) {
            argv_strs.emplace_back("--rpc-tls-cert");
            argv_strs.push_back(*bootstrap_cert_path);
            argv_strs.emplace_back("--rpc-tls-key");
            argv_strs.push_back(*bootstrap_key_path);
        }
        argv_strs.insert(argv_strs.end(), extra_args.begin(), extra_args.end());

        std::vector<char*> argv;
        argv.reserve(argv_strs.size() + 1);
        for (auto& s : argv_strs) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);

        // fork()+prctl(PR_SET_PDEATHSIG)+execve() rather than posix_spawn:
        // if this test process dies abnormally before this object's
        // destructor runs, an orphaned ca_cluster_node child inherits this
        // process's stdout/stderr pipe and keeps it open indefinitely -
        // observed (in ca_cluster_node_test.cpp, which shares this exact
        // spawn pattern) to wedge ctest's own output capture. PR_SET_PDEATHSIG
        // makes the kernel SIGKILL this child directly the moment its
        // parent dies, for any reason, without relying on that parent's
        // own cleanup code running at all.
        pid_t parent_pid = getpid();
        pid = fork();
        BOOST_REQUIRE_MESSAGE(pid >= 0, "fork() failed: " << std::strerror(errno));
        if (pid == 0) {
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent_pid) {
                _exit(1);
            }
            execve(CA_CLUSTER_NODE_PATH, argv.data(), environ);
            _exit(127);  // execve() only returns on failure
        }
    }

    auto stop() -> void {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            // Bounded, not a plain blocking waitpid(): see
            // ca_cluster_node_process_wait.hpp's own comment for why -- this
            // is a safety net against a regression of the exact shutdown-
            // ordering bug that used to hang this process indefinitely on
            // SIGTERM, not an expectation that 30s is ever actually needed.
            if (!wait_for_exit_or_kill(pid, std::chrono::seconds(30))) {
                BOOST_ERROR("cluster_node_process::stop(): node " << node_id << " (pid " << pid
                                                                  << ") did not exit within 30s "
                                                                     "of SIGTERM; force-killed");
            }
            pid = -1;
        }
    }

    // Restarts against the SAME data_dir/ports — simulates a process
    // crash-and-restart. `new_bootstrap_cert_path`/`new_bootstrap_key_path`
    // let a restart change (or entirely drop) the --rpc-tls-cert/key flags,
    // the whole point of this test file.
    auto restart(std::optional<std::string> new_bootstrap_cert_path,
                 std::optional<std::string> new_bootstrap_key_path) -> void {
        BOOST_REQUIRE(pid <= 0);
        bootstrap = false;
        bootstrap_cert_path = std::move(new_bootstrap_cert_path);
        bootstrap_key_path = std::move(new_bootstrap_key_path);
        spawn();
    }

    [[nodiscard]] auto is_running() const -> bool { return pid > 0; }

    // Whether the process spawned last is still alive — true across a
    // renewal is what "without a restart" means.
    [[nodiscard]] auto still_alive() const -> bool {
        if (pid <= 0) {
            return false;
        }
        int status = 0;
        return ::waitpid(pid, &status, WNOHANG) == 0;
    }
};

auto wait_healthy(int http_port, std::chrono::seconds timeout) -> bool {
    httplib::Client c("127.0.0.1", http_port);
    c.set_connection_timeout(1, 0);
    c.set_read_timeout(10, 0);
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto res = c.Get("/healthz");
        if (res && res->status == 200) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

auto find_leader_index(const std::vector<std::unique_ptr<rpc_tls_node_process>>& nodes,
                       const std::string& auth_token, std::chrono::seconds timeout)
    -> std::optional<std::size_t> {
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
                return i;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    return std::nullopt;
}

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

auto try_issue_certificate(const std::vector<std::unique_ptr<rpc_tls_node_process>>& nodes,
                           const std::string& auth_token, std::chrono::seconds timeout) -> bool {
    auto leader_index = find_leader_index(nodes, auth_token, timeout);
    if (!leader_index.has_value()) {
        return false;
    }

    leaf_certificate_options opts;
    opts.subject.common_name = "rpc-tls-restart-test-client";
    opts.dns_names = {"rpc-tls-restart-test-client.example.com"};
    auto csr = generate_key_and_csr(opts);

    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] =
        boost::json::array{boost::json::string("rpc-tls-restart-test-client.example.com")};

    httplib::Client client("127.0.0.1", nodes[*leader_index]->http_port);
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(65, 0);
    auto res = post_with_retry_on_not_ready(client, "/v1/certificates",
                                            {{"Authorization", "Bearer " + auth_token}},
                                            boost::json::serialize(body), timeout);
    return res && res->status == 200;
}

constexpr const char* k_auth_token = "rpc-tls-restart-test-token";

auto read_file(const std::string& path) -> std::string {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Serial of the PEM certificate at `path`, or nullopt when the file is
// missing or does not parse (a renewal can be rewriting it right now).
auto persisted_serial(const std::string& path) -> std::optional<std::string> {
    auto pem = read_file(path);
    if (pem.empty()) {
        return std::nullopt;
    }
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        return std::nullopt;
    }
    BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(cert), nullptr);
    char* hex = BN_bn2hex(bn);
    std::string out = hex;
    OPENSSL_free(hex);
    BN_free(bn);
    X509_free(cert);
    return out;
}

// Serial of the certificate a running node's Raft RPC listener presents,
// read off a real TLS handshake. The listener requires a client
// certificate, so the probe presents `client_cert_path`/`client_key_path`
// (another node's peer identity) and does not verify the server: only what
// it presents matters here.
auto rpc_listener_serial(int rpc_port, const std::string& client_cert_path,
                         const std::string& client_key_path) -> std::optional<std::string> {
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        return std::nullopt;
    }
    std::optional<std::string> out;
    if (SSL_CTX_use_certificate_chain_file(ctx, client_cert_path.c_str()) == 1 &&
        SSL_CTX_use_PrivateKey_file(ctx, client_key_path.c_str(), SSL_FILETYPE_PEM) == 1) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(rpc_port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, fd);
            if (SSL_connect(ssl) == 1) {
                if (X509* peer = SSL_get1_peer_certificate(ssl)) {
                    BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get0_serialNumber(peer), nullptr);
                    char* hex = BN_bn2hex(bn);
                    out = std::string(hex);
                    OPENSSL_free(hex);
                    BN_free(bn);
                    X509_free(peer);
                }
                SSL_shutdown(ssl);
            }
            SSL_free(ssl);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
    SSL_CTX_free(ctx);
    return out;
}

// Issues one client certificate through whichever node currently leads,
// over the nodes' https:// client API verified against `listener_root`.
auto try_issue_certificate_https(const std::vector<std::unique_ptr<rpc_tls_node_process>>& nodes,
                                 const std::string& listener_root_path,
                                 std::chrono::seconds timeout) -> bool {
    leaf_certificate_options opts;
    opts.subject.common_name = "rpc-tls-renewal-test-client";
    opts.dns_names = {"rpc-tls-renewal-test-client.example.com"};
    auto csr = generate_key_and_csr(opts);
    boost::json::object body;
    body["csr_pem"] = csr.csr_pem;
    body["dns_names"] =
        boost::json::array{boost::json::string("rpc-tls-renewal-test-client.example.com")};
    auto payload = boost::json::serialize(body);

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& n : nodes) {
            httplib::SSLClient client("127.0.0.1", n->http_port);
            client.set_ca_cert_path(listener_root_path.c_str());
            client.enable_server_certificate_verification(true);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(65, 0);
            auto res = client.Post("/v1/certificates",
                                   {{"Authorization", std::string("Bearer ") + k_auth_token}},
                                   payload, "application/json");
            if (res && res->status == 200) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_CASE(restarted_node_rejoins_without_bootstrap_credential,
                     *boost::unit_test::timeout(550)) {
    auto tmp_root = (std::filesystem::temp_directory_path() /
                     ("ca_cluster_node_rpc_tls_restart_test_" + std::to_string(::getpid())))
                        .string();
    std::filesystem::create_directories(tmp_root);
    std::string unseal_key_file = tmp_root + "/unseal.key";
    std::ofstream(unseal_key_file) << "rpc-tls-restart-test-unseal-passphrase\n";

    certificate_authority bootstrap_cred;
    std::string bootstrap_cert_path = tmp_root + "/bootstrap.crt";
    std::string bootstrap_key_path = tmp_root + "/bootstrap.key";
    std::ofstream(bootstrap_cert_path) << bootstrap_cred.root_certificate_pem();
    std::ofstream(bootstrap_key_path)
        << detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred);

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

    std::vector<std::unique_ptr<rpc_tls_node_process>> nodes;
    // Two nodes first; the third only once the CA exists.
    //
    // Starting all three together is what made this test flaky. The
    // --bootstrap-ca node cannot win an election on its own -- its pre-vote has
    // no quorum, logged directly as "Pre-vote round did not reach quorum" -- so
    // the 6000ms head start below buys it nothing until a peer is reachable.
    // Under CPU contention both peers finish starting at about the same moment,
    // by which point every node's election timer has long since expired, and
    // they all campaign at once.
    //
    // Measured on 2026-09-29: node2 campaigned 17ms after node1 and took term 2,
    // node1 lost leadership at term 1 *before committing bootstrap_ca*, and the
    // CA was then never established at all. The cluster stayed perfectly healthy
    // afterwards -- thousands of successful AppendEntries and reads under node2 --
    // but certificate issuance had become permanently impossible. That is why
    // widening the issuance wait below from 30s to 120s changed nothing: there
    // was nothing left to wait for. A passing run logs "bootstrap_ca committed";
    // a failing one never logs it at all.
    //
    // With a single peer the race has one other candidate, whose own timer has
    // not expired yet, and the issuance check below then proves the CA is
    // committed before a third voter exists to change the outcome. This is the
    // staged startup ca_cluster_node_rpc_tls_test.cpp already uses, for the same
    // reason it gives: show the bootstrap credential alone establishes the CA
    // before the late node joins.
    for (std::size_t i = 0; i < 2; ++i) {
        nodes.push_back(std::make_unique<rpc_tls_node_process>(
            infos[i].id, infos[i].rpc_port, infos[i].http_port,
            tmp_root + "/node" + std::to_string(infos[i].id), unseal_key_file, k_auth_token,
            peers_arg, bootstrap_cert_path, bootstrap_key_path, /*bootstrap=*/i == 0));
        if (i == 0) {
            // Proportional to this file's much-larger-than-default
            // election timeout (3000-5000ms) — must comfortably exceed
            // it so the --bootstrap-ca node's own election timer always
            // fires first (Requirement 17.10's documented operational
            // practice), not a race against the other node's timer.
            std::this_thread::sleep_for(std::chrono::milliseconds(6000));
        }
    }
    BOOST_REQUIRE_MESSAGE(
        std::all_of(
            nodes.begin(), nodes.end(),
            [](const auto& n) { return wait_healthy(n->http_port, std::chrono::seconds(60)); }),
        "the first two nodes never became healthy");

    // Wait for the cluster to reach a stable, issuance-capable state
    // (bootstrap_ca committed, at least this far along toward cutover).
    bool ready = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (try_issue_certificate(nodes, k_auth_token, std::chrono::seconds(10))) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    BOOST_REQUIRE_MESSAGE(ready,
                          "cluster never reached an issuance-capable state with the first two "
                          "nodes up (bootstrap_ca not committed -- check whether the "
                          "--bootstrap-ca node lost the initial election)");

    // The third node joins only now. Before the CA is committed its vote could
    // hand the first election to a non-bootstrap node, which is the failure this
    // ordering removes.
    nodes.push_back(std::make_unique<rpc_tls_node_process>(
        infos[2].id, infos[2].rpc_port, infos[2].http_port,
        tmp_root + "/node" + std::to_string(infos[2].id), unseal_key_file, k_auth_token, peers_arg,
        bootstrap_cert_path, bootstrap_key_path, /*bootstrap=*/false));
    BOOST_REQUIRE_MESSAGE(wait_healthy(nodes[2]->http_port, std::chrono::seconds(60)),
                          "the third node never became healthy");

    // Pick a non-leader node to restart (restarting the leader would also
    // trigger a failover, an orthogonal concern already covered by
    // tests/ca_cluster_node_test.cpp's own failover property).
    auto leader_index = find_leader_index(nodes, k_auth_token, std::chrono::seconds(30));
    BOOST_REQUIRE(leader_index.has_value());
    std::size_t restart_index = (*leader_index + 1) % nodes.size();

    // Poll for the maintenance threads to reach full cutover on all three
    // nodes (each node's own persisted peer cert file under --data-dir is
    // what this test actually depends on existing) — a fixed sleep here
    // previously raced main.cpp's own k_identity_acquire_grace (a 3-second
    // minimum delay, added after ca-cluster-rpc-mtls's CI-only deadlock
    // fix, between a node first observing the CA root and that node
    // switching its presented identity): under CI contention this test's
    // own fixed wait could elapse before that grace period even finished,
    // let alone before real CSR generation/signing/persisting completed
    // afterward. Polling avoids needing to keep two independently-tuned
    // timing constants in sync by hand.
    auto peer_cert_path = std::filesystem::path(tmp_root) /
                          ("node" + std::to_string(nodes[restart_index]->node_id)) /
                          "rpc_peer_cert.pem";
    bool cutover_complete = false;
    auto cutover_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < cutover_deadline) {
        if (std::filesystem::exists(peer_cert_path)) {
            cutover_complete = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    // Confirm the persisted peer certificate actually exists before relying
    // on it — otherwise a false pass here would just mean "plain TCP-like
    // behavior happened to still work," not "Property 5 held."
    BOOST_REQUIRE_MESSAGE(cutover_complete,
                          "no persisted RPC peer certificate found before restart — "
                          "cutover likely hadn't happened yet");

    nodes[restart_index]->stop();

    // Delete the bootstrap credential entirely before restarting — the
    // restarted node must not need it.
    std::filesystem::remove(bootstrap_cert_path);
    std::filesystem::remove(bootstrap_key_path);

    nodes[restart_index]->restart(std::nullopt, std::nullopt);
    BOOST_REQUIRE_MESSAGE(wait_healthy(nodes[restart_index]->http_port, std::chrono::seconds(60)),
                          "restarted node (no --rpc-tls-cert/--rpc-tls-key) never became healthy");

    // The cluster as a whole must still be able to issue a certificate,
    // proving the restarted node rejoined the Raft cluster over RPC TLS
    // using only its persisted peer certificate.
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate(nodes, k_auth_token, std::chrono::seconds(60)),
        "certificate issuance failed after restarting a node without the bootstrap credential");

    for (auto& n : nodes) {
        n->stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
}

BOOST_AUTO_TEST_CASE(near_expiry_peer_certs_renew_without_restart,
                     *boost::unit_test::timeout(550)) {
    auto tmp_root = (std::filesystem::temp_directory_path() /
                     ("ca_cluster_node_rpc_tls_renewal_test_" + std::to_string(::getpid())))
                        .string();
    std::filesystem::create_directories(tmp_root);
    std::string unseal_key_file = tmp_root + "/unseal.key";
    std::ofstream(unseal_key_file) << "rpc-tls-renewal-test-unseal-passphrase\n";

    certificate_authority bootstrap_cred;
    std::string bootstrap_cert_path = tmp_root + "/bootstrap.crt";
    std::string bootstrap_key_path = tmp_root + "/bootstrap.key";
    std::ofstream(bootstrap_cert_path) << bootstrap_cred.root_certificate_pem();
    std::ofstream(bootstrap_key_path)
        << detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred);

    // /v1/certificates/renew authenticates by mTLS, so renewal only runs
    // against an https:// leader: give every node a client-API listener
    // certificate from one operator CA, as the verified-HTTPS case in
    // ca_cluster_node_rpc_tls_test.cpp does.
    certificate_authority listener_ca;
    std::string listener_root_path = tmp_root + "/listener_root.pem";
    std::ofstream(listener_root_path) << listener_ca.root_certificate_pem();

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
              << "@https://127.0.0.1:" << infos[i].http_port;
    }
    std::string peers_arg = peers.str();

    // Peer certificates are valid for 30 days. A window 30 seconds short of
    // that puts each one inside it about 30 seconds after issuance, so every
    // node renews repeatedly while the test watches, instead of after weeks.
    constexpr auto k_peer_validity = std::chrono::hours(24 * 30);
    const auto renewal_window = std::chrono::duration_cast<std::chrono::seconds>(k_peer_validity) -
                                std::chrono::seconds(30);

    auto data_dir_of = [&](std::uint64_t id) { return tmp_root + "/node" + std::to_string(id); };
    auto peer_cert_of = [&](std::uint64_t id) { return data_dir_of(id) + "/rpc_peer_cert.pem"; };
    auto peer_key_of = [&](std::uint64_t id) { return data_dir_of(id) + "/rpc_peer_key.pem"; };

    auto spawn_node = [&](std::size_t i) {
        leaf_certificate_options opts;
        opts.subject.common_name = "ca-node-" + std::to_string(infos[i].id);
        opts.ip_addresses = {"127.0.0.1"};
        opts.server_auth = true;
        opts.client_auth = false;
        auto leaf = listener_ca.issue(opts);
        auto dir = tmp_root + "/listener" + std::to_string(infos[i].id);
        std::filesystem::create_directories(dir);
        std::ofstream(dir + "/cert.pem") << leaf.chain_pem;
        std::ofstream(dir + "/key.pem") << leaf.private_key_pem;
        return std::make_unique<rpc_tls_node_process>(
            infos[i].id, infos[i].rpc_port, infos[i].http_port, data_dir_of(infos[i].id),
            unseal_key_file, k_auth_token, peers_arg, bootstrap_cert_path, bootstrap_key_path,
            /*bootstrap=*/i == 0,
            std::vector<std::string>{"--tls-cert", dir + "/cert.pem", "--tls-key", dir + "/key.pem",
                                     "--rpc-renewal-window-secs",
                                     std::to_string(renewal_window.count())});
    };

    // Same staged startup as the first case, for the same reason: with all
    // three up at once a non-bootstrap node can win the first election
    // before bootstrap_ca commits, and the CA then never exists (seen here
    // too: node 2 led term 1 at commit index 2 and nothing ever enrolled).
    std::vector<std::unique_ptr<rpc_tls_node_process>> nodes;
    nodes.push_back(spawn_node(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    nodes.push_back(spawn_node(1));
    BOOST_REQUIRE_MESSAGE(
        try_issue_certificate_https(nodes, listener_root_path, std::chrono::seconds(120)),
        "cluster never reached an issuance-capable state with the first two nodes up "
        "(bootstrap_ca not committed)");
    nodes.push_back(spawn_node(2));

    // Every node enrolls (persists a peer certificate) and finalizes the
    // cutover, so from here on the cluster's Raft RPC relies on CA-issued
    // peer certificates alone.
    auto all_have = [&](const std::string& file) {
        return std::ranges::all_of(infos, [&](const info& in) {
            return std::filesystem::exists(data_dir_of(in.id) + "/" + file);
        });
    };
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
    while (!all_have("rpc_cutover_finalized") && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    BOOST_REQUIRE_MESSAGE(all_have("rpc_peer_cert.pem"), "not every node enrolled a peer identity");
    BOOST_REQUIRE_MESSAGE(all_have("rpc_cutover_finalized"),
                          "not every node finalized the RPC TLS cutover");

    // Each node's first peer certificate; a renewal must replace it.
    std::map<std::uint64_t, std::string> first_serial;
    for (const auto& in : infos) {
        auto serial = persisted_serial(peer_cert_of(in.id));
        BOOST_REQUIRE_MESSAGE(serial.has_value(),
                              "node " << in.id << "'s persisted peer certificate does not parse");
        first_serial[in.id] = *serial;
    }

    // Requirement 7.2: every node, whichever one leads, renews on its own.
    // The leader has no leader to call /renew on and signs its renewal
    // in-process; before that, a long-serving leader's certificate expired.
    auto all_renewed = [&] {
        return std::ranges::all_of(infos, [&](const info& in) {
            auto serial = persisted_serial(peer_cert_of(in.id));
            return serial.has_value() && *serial != first_serial[in.id];
        });
    };
    auto renew_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (!all_renewed() && std::chrono::steady_clock::now() < renew_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    for (const auto& in : infos) {
        BOOST_TEST((persisted_serial(peer_cert_of(in.id)) != std::optional(first_serial[in.id])),
                   "node " << in.id << " never renewed its near-expiry peer certificate");
    }
    BOOST_REQUIRE(all_renewed());

    // Requirement 7.2's "without a restart": the same processes are still
    // running, and each one's live Raft RPC listener already presents a
    // renewed certificate (hot-reloaded, Requirement 7.3), not the one it
    // started with.
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        BOOST_TEST(nodes[i]->still_alive(), "node " << infos[i].id << " exited");
        const auto& prober = infos[(i + 1) % infos.size()];
        std::optional<std::string> presented;
        auto probe_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!presented.has_value() && std::chrono::steady_clock::now() < probe_deadline) {
            presented = rpc_listener_serial(infos[i].rpc_port, peer_cert_of(prober.id),
                                            peer_key_of(prober.id));
            if (!presented.has_value()) {
                // The prober's own files can be mid-rewrite by its renewal.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        BOOST_TEST(presented.has_value(),
                   "could not complete a TLS handshake with node " << infos[i].id << "'s RPC port");
        BOOST_TEST(
            (presented != std::optional(first_serial[infos[i].id])),
            "node " << infos[i].id << "'s RPC listener still presents its pre-renewal certificate");
    }

    // The renewed identities still carry Raft: the cluster keeps committing.
    BOOST_TEST(try_issue_certificate_https(nodes, listener_root_path, std::chrono::seconds(90)),
               "certificate issuance failed after every node renewed its peer certificate");

    for (auto& n : nodes) {
        n->stop();
    }
    std::error_code ec;
    std::filesystem::remove_all(tmp_root, ec);
}
