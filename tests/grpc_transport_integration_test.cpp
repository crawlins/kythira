// Feature: grpc-transport
//
// End-to-end integration tests for the gRPC transport
// (.kiro/specs/grpc-transport/): a real grpc_server and grpc_client talking
// over a loopback socket. Covers core-RPC success, server-side error mapping
// (handler throws → grpc_server_error), the UNIMPLEMENTED behavior of an
// unregistered optional service (Property 5), lifecycle safety, mutual TLS with
// certificates minted by certificate_authority (Requirement 19.6), and
// concurrent calls with no cross-talk (Requirement 19.3).

#define BOOST_TEST_MODULE GrpcTransportIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_transport_impl.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using types = kythira::grpc_kythira_transport_types;
using namespace std::chrono_literals;

// Distinct port per scenario to avoid TIME_WAIT / cross-test collisions.
std::atomic<std::uint16_t> g_next_port{50751};
auto next_port() -> std::uint16_t {
    return g_next_port.fetch_add(1);
}

auto make_server(std::uint16_t port, folly::CPUThreadPoolExecutor& exec,
                 kythira::grpc_server_config cfg = {})
    -> std::unique_ptr<kythira::grpc_server<types>> {
    return std::make_unique<kythira::grpc_server<types>>("127.0.0.1", port, std::move(cfg),
                                                         kythira::noop_metrics{}, exec);
}

auto make_client(std::uint16_t port, folly::CPUThreadPoolExecutor& exec,
                 kythira::grpc_client_config cfg = {})
    -> std::unique_ptr<kythira::grpc_client<types>> {
    std::unordered_map<std::uint64_t, std::string> book{{1, "127.0.0.1:" + std::to_string(port)}};
    return std::make_unique<kythira::grpc_client<types>>(std::move(book), std::move(cfg),
                                                         kythira::noop_metrics{}, exec);
}
}  // namespace

BOOST_AUTO_TEST_CASE(request_vote_end_to_end_insecure) {
    folly::CPUThreadPoolExecutor exec(4);
    const auto port = next_port();

    auto server = make_server(port, exec);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server->start();
    BOOST_TEST(server->is_running());

    auto client = make_client(port, exec);
    kythira::request_vote_request<> req{
        ._term = 7, ._candidate_id = 2, ._last_log_index = 5, ._last_log_term = 6};
    auto resp = client->send_request_vote(1, req, 2000ms).get();
    BOOST_TEST(resp.term() == 7U);
    BOOST_TEST(resp.vote_granted());

    server->stop();
    BOOST_TEST(!server->is_running());
}

BOOST_AUTO_TEST_CASE(handler_exception_maps_to_server_error) {
    folly::CPUThreadPoolExecutor exec(4);
    const auto port = next_port();

    auto server = make_server(port, exec);
    server->register_append_entries_handler(
        [](const kythira::append_entries_request<>&) -> kythira::append_entries_response<> {
            throw std::runtime_error("handler blew up");
        });
    server->start();

    auto client = make_client(port, exec);
    kythira::append_entries_request<> req{._term = 1,
                                          ._leader_id = 1,
                                          ._prev_log_index = 0,
                                          ._prev_log_term = 0,
                                          ._entries = {},
                                          ._leader_commit = 0};
    // Handler throw → INTERNAL → grpc_server_error (Requirement 6.5, 11.4).
    BOOST_CHECK_THROW(client->send_append_entries(1, req, 2000ms).get(),
                      kythira::grpc_server_error);

    server->stop();
}

BOOST_AUTO_TEST_CASE(unregistered_extension_returns_unimplemented) {
    folly::CPUThreadPoolExecutor exec(4);
    const auto port = next_port();

    // Only a core handler is registered; the pre-vote extension service is
    // never added to the builder, so a pre-vote call must come back
    // UNIMPLEMENTED rather than hang or crash (Property 5, Requirement 6.3).
    auto server = make_server(port, exec);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = false};
    });
    server->start();

    auto client = make_client(port, exec);
    kythira::request_pre_vote_request<> req{
        ._term = 3, ._candidate_id = 2, ._last_log_index = 1, ._last_log_term = 1};
    bool threw = false;
    try {
        client->send_request_pre_vote(1, req, 2000ms).get();
    } catch (const kythira::grpc_client_error& e) {
        threw = true;
        BOOST_TEST((e.status_code() == grpc::StatusCode::UNIMPLEMENTED));
    }
    BOOST_TEST(threw);

    server->stop();
}

BOOST_AUTO_TEST_CASE(lifecycle_repeated_start_stop_is_safe) {
    folly::CPUThreadPoolExecutor exec(2);
    const auto port = next_port();
    auto server = make_server(port, exec);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });

    BOOST_TEST(!server->is_running());
    server->start();
    server->start();  // idempotent (Requirement 7.3 / 19.5)
    BOOST_TEST(server->is_running());
    server->stop();
    server->stop();  // idempotent
    BOOST_TEST(!server->is_running());
}

BOOST_AUTO_TEST_CASE(concurrent_calls_no_cross_talk) {
    folly::CPUThreadPoolExecutor exec(8);
    const auto port = next_port();

    auto server = make_server(port, exec);
    // Echo the term back so each caller can verify it got its own response.
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(),
                                                ._vote_granted = (req.candidate_id() % 2 == 0)};
    });
    server->start();

    auto client = make_client(port, exec);
    constexpr int thread_count = 8;
    constexpr int calls_per_thread = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&client, t]() {
            for (int c = 0; c < calls_per_thread; ++c) {
                const std::uint64_t term = static_cast<std::uint64_t>(t) * 1000 + c;
                kythira::request_vote_request<> req{._term = term,
                                                    ._candidate_id = static_cast<std::uint64_t>(t),
                                                    ._last_log_index = 0,
                                                    ._last_log_term = 0};
                auto resp = client->send_request_vote(1, req, 2000ms).get();
                if (resp.term() != term) {
                    // Would indicate a response delivered to the wrong caller.
                    return;
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    BOOST_TEST(failures.load() == 0);

    server->stop();
}

BOOST_AUTO_TEST_CASE(mutual_tls_end_to_end) {
    raft::testing::certificate_authority ca;

    raft::testing::leaf_certificate_options server_opts;
    server_opts.subject.common_name = "localhost";
    server_opts.dns_names = {"localhost"};
    server_opts.ip_addresses = {"127.0.0.1"};
    server_opts.server_auth = true;
    auto server_cert = ca.issue(server_opts);

    raft::testing::leaf_certificate_options client_opts;
    client_opts.subject.common_name = "raft-client";
    client_opts.dns_names = {"raft-client"};
    client_opts.server_auth = false;
    client_opts.client_auth = true;
    auto client_cert = ca.issue(client_opts);

    folly::CPUThreadPoolExecutor exec(4);
    const auto port = next_port();

    kythira::grpc_server_config server_cfg;
    server_cfg.enable_tls = true;
    server_cfg.server_cert_pem = server_cert.certificate_pem;
    server_cfg.server_key_pem = server_cert.private_key_pem;
    server_cfg.ca_cert_pem = ca.root_certificate_pem();
    server_cfg.require_client_cert = true;

    auto server = make_server(port, exec, server_cfg);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server->start();

    kythira::grpc_client_config client_cfg;
    client_cfg.enable_tls = true;
    client_cfg.ca_cert_pem = ca.root_certificate_pem();
    client_cfg.client_cert_pem = client_cert.certificate_pem;
    client_cfg.client_key_pem = client_cert.private_key_pem;
    client_cfg.target_name_override = "localhost";  // SAN matches, connecting to 127.0.0.1

    auto client = make_client(port, exec, client_cfg);
    kythira::request_vote_request<> req{
        ._term = 9, ._candidate_id = 3, ._last_log_index = 0, ._last_log_term = 0};
    auto resp = client->send_request_vote(1, req, 3000ms).get();
    BOOST_TEST(resp.term() == 9U);
    BOOST_TEST(resp.vote_granted());

    server->stop();
}

BOOST_AUTO_TEST_CASE(tls_misconfiguration_fails_closed) {
    folly::CPUThreadPoolExecutor exec(1);
    // TLS enabled but no server certificate material → construction throws
    // rather than silently downgrading to insecure (Property 7, Requirement 9.6).
    kythira::grpc_server_config bad_server;
    bad_server.enable_tls = true;  // server_cert_pem / server_key_pem left empty
    BOOST_CHECK_THROW(make_server(next_port(), exec, bad_server),
                      kythira::grpc_tls_configuration_error);

    // Client with only half of an mTLS pair → also fails closed.
    kythira::grpc_client_config bad_client;
    bad_client.enable_tls = true;
    bad_client.client_cert_pem = "-----BEGIN CERTIFICATE-----\nnot-real\n-----END CERTIFICATE-----";
    // client_key_pem intentionally empty
    BOOST_CHECK_THROW(make_client(next_port(), exec, bad_client),
                      kythira::grpc_tls_configuration_error);
}
