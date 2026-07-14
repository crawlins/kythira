// **Feature: ca-cluster-rpc-mtls**
// Real sockets, real OpenSSL: a 2-node tls_tcp_rpc_client/server pair
// completing a genuine mutual-TLS handshake and one RPC round trip under
// each named trust policy (pinned_fingerprint, either, ca_root_only), plus
// a connection rejected because the presented certificate doesn't satisfy
// the server's trust policy.
// **Validates: Requirements 1.1, 1.2, 1.4**
#define BOOST_TEST_MODULE tls_tcp_rpc_integration_test
#include <boost/test/unit_test.hpp>

#include <raft/ca_bootstrap_client.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace raft::testing;
using kythira::ca_root_only;
using kythira::either;
using kythira::pinned_fingerprint;
using kythira::tls_tcp_rpc_client;
using kythira::tls_tcp_rpc_config;
using kythira::tls_tcp_rpc_server;

namespace {

auto find_free_port() -> std::uint16_t {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = INADDR_ANY;
    BOOST_REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
    ::close(fd);
    return port;
}

// temp_pem_files: writes certificate_authority::pem_material to disk so
// tls_tcp_rpc_config (which takes filesystem paths, matching real
// ca_cluster_node usage) can load them.
struct temp_pem_files {
    std::filesystem::path dir;
    std::string cert_path;
    std::string key_path;

    temp_pem_files(const std::string& cert_pem, const std::string& key_pem) {
        dir = std::filesystem::temp_directory_path() /
              ("tls_tcp_rpc_integration_test_" + std::to_string(::getpid()) + "_" +
               std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        cert_path = (dir / "cert.pem").string();
        key_path = (dir / "key.pem").string();
        std::ofstream(cert_path) << cert_pem;
        std::ofstream(key_path) << key_pem;
    }

    ~temp_pem_files() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    temp_pem_files(const temp_pem_files&) = delete;
    auto operator=(const temp_pem_files&) -> temp_pem_files& = delete;
};

auto fingerprint_of_root(const std::string& root_pem) -> std::string {
    auto root = root_cert_from_pem_bundle(root_pem);
    return ca_bootstrap_detail::sha256_fingerprint_hex_bare(root.get());
}

// Starts a server on a fresh port with an echo-shaped request_vote handler,
// dials it with `client`, and returns whether the round trip succeeded.
// Both server and client are stopped/destroyed before returning.
auto try_round_trip(tls_tcp_rpc_config server_config, tls_tcp_rpc_config client_config) -> bool {
    auto port = find_free_port();
    tls_tcp_rpc_server server(port, std::move(server_config));
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{req.term(), true};
    });
    server.start();
    // Give the accept thread a moment to reach accept() — start() returns
    // once the listening socket is bound, but the background thread's first
    // accept() call happening before the client's connect() isn't
    // synchronized beyond that.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    tls_tcp_rpc_client client(std::move(client_config));
    client.add_peer(1, "127.0.0.1", port);

    kythira::request_vote_request<> req{7, 42, 0, 0};
    bool ok = false;
    try {
        auto resp = client.send_request_vote(1, req, std::chrono::milliseconds(5000)).get();
        ok = resp.vote_granted() && resp.term() == 7;
    } catch (const std::exception&) {
        ok = false;
    }

    server.stop();
    return ok;
}

// Exercises send_append_entries/send_install_snapshot specifically — the
// other round trips above only cover send_request_vote, leaving the
// append_entries/install_snapshot branches of client_impl::call() and
// server_impl::handle()'s dispatch untested by any coverage-scanned binary
// (the real ca_cluster_node process that also exercises them isn't itself
// coverage-instrumented/scanned).
auto try_all_rpc_types_round_trip(tls_tcp_rpc_config server_config,
                                  tls_tcp_rpc_config client_config) -> bool {
    auto port = find_free_port();
    tls_tcp_rpc_server server(port, std::move(server_config));
    server.register_append_entries_handler([](const kythira::append_entries_request<>& req) {
        return kythira::append_entries_response<>{req.term(), true, std::nullopt, std::nullopt};
    });
    server.register_install_snapshot_handler([](const kythira::install_snapshot_request<>& req) {
        return kythira::install_snapshot_response<>{req.term()};
    });
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    tls_tcp_rpc_client client(std::move(client_config));
    client.add_peer(1, "127.0.0.1", port);

    bool ok = false;
    try {
        kythira::append_entries_request<> ae_req{3, 42, 0, 0, {}, 0};
        auto ae_resp = client.send_append_entries(1, ae_req, std::chrono::milliseconds(5000)).get();

        kythira::install_snapshot_request<> is_req{3, 42, 0, 0, 0, {}, true};
        auto is_resp =
            client.send_install_snapshot(1, is_req, std::chrono::milliseconds(5000)).get();

        ok = ae_resp.success() && ae_resp.term() == 3 && is_resp.term() == 3;
    } catch (const std::exception&) {
        ok = false;
    }

    server.stop();
    return ok;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(tls_tcp_rpc_integration_tests)

BOOST_AUTO_TEST_CASE(round_trip_under_pinned_fingerprint_policy, *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    temp_pem_files files(bootstrap_cred.root_certificate_pem(),
                         detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred));
    auto fp = fingerprint_of_root(bootstrap_cred.root_certificate_pem());

    tls_tcp_rpc_config server_cfg{files.cert_path, files.key_path, pinned_fingerprint(fp)};
    tls_tcp_rpc_config client_cfg{files.cert_path, files.key_path, pinned_fingerprint(fp)};

    BOOST_TEST(try_round_trip(std::move(server_cfg), std::move(client_cfg)));
}

BOOST_AUTO_TEST_CASE(round_trip_under_either_policy_using_ca_issued_certs,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority ca;

    leaf_certificate_options server_opts;
    server_opts.subject.common_name = "server-peer";
    server_opts.dns_names = {"server-peer"};
    auto server_leaf = ca.issue(server_opts);
    temp_pem_files server_files(server_leaf.certificate_pem, server_leaf.private_key_pem);

    leaf_certificate_options client_opts;
    client_opts.subject.common_name = "client-peer";
    client_opts.dns_names = {"client-peer"};
    auto client_leaf = ca.issue(client_opts);
    temp_pem_files client_files(client_leaf.certificate_pem, client_leaf.private_key_pem);

    auto bootstrap_fp = fingerprint_of_root(bootstrap_cred.root_certificate_pem());
    auto dual = either(bootstrap_fp, ca.root_certificate_pem());

    tls_tcp_rpc_config server_cfg{server_files.cert_path, server_files.key_path, dual};
    tls_tcp_rpc_config client_cfg{client_files.cert_path, client_files.key_path, dual};

    BOOST_TEST(try_round_trip(std::move(server_cfg), std::move(client_cfg)));
}

BOOST_AUTO_TEST_CASE(round_trip_under_ca_root_only_policy, *boost::unit_test::timeout(30)) {
    certificate_authority ca;

    leaf_certificate_options server_opts;
    server_opts.subject.common_name = "server-peer";
    server_opts.dns_names = {"server-peer"};
    auto server_leaf = ca.issue(server_opts);
    temp_pem_files server_files(server_leaf.certificate_pem, server_leaf.private_key_pem);

    leaf_certificate_options client_opts;
    client_opts.subject.common_name = "client-peer";
    client_opts.dns_names = {"client-peer"};
    auto client_leaf = ca.issue(client_opts);
    temp_pem_files client_files(client_leaf.certificate_pem, client_leaf.private_key_pem);

    auto policy = ca_root_only(ca.root_certificate_pem());
    tls_tcp_rpc_config server_cfg{server_files.cert_path, server_files.key_path, policy};
    tls_tcp_rpc_config client_cfg{client_files.cert_path, client_files.key_path, policy};

    BOOST_TEST(try_round_trip(std::move(server_cfg), std::move(client_cfg)));
}

// Requirement 1.2: a connection whose peer presents a certificate that
// fails verification under the currently active trust policy is rejected
// before any RPC payload is exchanged — here, the server only trusts
// ca_root_only(ca) but the client only holds the (unrelated) bootstrap
// credential.
BOOST_AUTO_TEST_CASE(connection_rejected_when_client_cert_not_trusted_by_server,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    certificate_authority ca;

    leaf_certificate_options server_opts;
    server_opts.subject.common_name = "server-peer";
    server_opts.dns_names = {"server-peer"};
    auto server_leaf = ca.issue(server_opts);
    temp_pem_files server_files(server_leaf.certificate_pem, server_leaf.private_key_pem);

    temp_pem_files client_files(bootstrap_cred.root_certificate_pem(),
                                detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred));

    tls_tcp_rpc_config server_cfg{server_files.cert_path, server_files.key_path,
                                  ca_root_only(ca.root_certificate_pem())};
    // The client trusts the server's cert fine (server chains to ca), but
    // the server does NOT trust the client's bootstrap-credential cert — the
    // handshake completes at the TLS layer (SSL_VERIFY_PEER without
    // SSL_VERIFY_FAIL_IF_NO_PEER_CERT-triggering absence — a cert IS
    // presented) but the round trip must still fail once the server's
    // post-handshake trust check runs.
    tls_tcp_rpc_config client_cfg{client_files.cert_path, client_files.key_path,
                                  ca_root_only(ca.root_certificate_pem())};

    BOOST_TEST(!try_round_trip(std::move(server_cfg), std::move(client_cfg)));
}

BOOST_AUTO_TEST_CASE(append_entries_and_install_snapshot_round_trip_under_pinned_fingerprint,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    temp_pem_files files(bootstrap_cred.root_certificate_pem(),
                         detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred));
    auto fp = fingerprint_of_root(bootstrap_cred.root_certificate_pem());

    tls_tcp_rpc_config server_cfg{files.cert_path, files.key_path, pinned_fingerprint(fp)};
    tls_tcp_rpc_config client_cfg{files.cert_path, files.key_path, pinned_fingerprint(fp)};

    BOOST_TEST(try_all_rpc_types_round_trip(std::move(server_cfg), std::move(client_cfg)));
}

// Requirement 1.3/6.2/7.2: reload_identity()/reload_trust_policy() update the
// live client/server transport in place (no restart) — exercised here on
// both the client and server handles (which just forward to their impls),
// followed by a round trip proving the reloaded identity/policy is actually
// what subsequent connections use.
BOOST_AUTO_TEST_CASE(reload_identity_and_trust_policy_apply_to_next_connection,
                     *boost::unit_test::timeout(30)) {
    certificate_authority bootstrap_cred;
    temp_pem_files bootstrap_files(
        bootstrap_cred.root_certificate_pem(),
        detail_testing::unsafe_extract_ca_private_key_pem(bootstrap_cred));
    auto bootstrap_fp = fingerprint_of_root(bootstrap_cred.root_certificate_pem());

    certificate_authority ca;
    leaf_certificate_options server_opts;
    server_opts.subject.common_name = "server-peer";
    server_opts.dns_names = {"server-peer"};
    auto server_leaf = ca.issue(server_opts);
    temp_pem_files server_files(server_leaf.certificate_pem, server_leaf.private_key_pem);

    leaf_certificate_options client_opts;
    client_opts.subject.common_name = "client-peer";
    client_opts.dns_names = {"client-peer"};
    auto client_leaf = ca.issue(client_opts);
    temp_pem_files client_files(client_leaf.certificate_pem, client_leaf.private_key_pem);

    auto port = find_free_port();
    tls_tcp_rpc_server server(
        port, tls_tcp_rpc_config{bootstrap_files.cert_path, bootstrap_files.key_path,
                                 pinned_fingerprint(bootstrap_fp)});
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{req.term(), true};
    });
    BOOST_TEST(!server.is_running());
    server.start();
    BOOST_TEST(server.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    tls_tcp_rpc_client client(tls_tcp_rpc_config{
        bootstrap_files.cert_path, bootstrap_files.key_path, pinned_fingerprint(bootstrap_fp)});
    client.add_peer(1, "127.0.0.1", port);

    // Property 3/6.2-analogue at the transport-unit level: cut both sides
    // over from the bootstrap credential to CA-issued identities/policy —
    // the server now presents server_leaf and only accepts ca_root_only(ca);
    // the client now presents client_leaf and only accepts the same.
    auto dual = kythira::either(bootstrap_fp, ca.root_certificate_pem());
    server.reload_trust_policy(dual);
    client.reload_trust_policy(dual);
    server.reload_identity(server_files.cert_path, server_files.key_path);
    client.reload_identity(client_files.cert_path, client_files.key_path);

    auto policy = ca_root_only(ca.root_certificate_pem());
    server.reload_trust_policy(policy);
    client.reload_trust_policy(policy);

    kythira::request_vote_request<> req{9, 42, 0, 0};
    auto resp = client.send_request_vote(1, req, std::chrono::milliseconds(5000)).get();
    BOOST_TEST(resp.vote_granted());
    BOOST_TEST(resp.term() == 9);

    server.stop();
    BOOST_TEST(!server.is_running());
}

BOOST_AUTO_TEST_SUITE_END()
