// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-transport
//
// End-to-end integration tests for the gRPC transport
// (.kiro/specs/grpc-transport/): a real grpc_server and grpc_client talking
// over a loopback socket. Covers core-RPC success, server-side error mapping
// (handler throws → grpc_server_error), the UNIMPLEMENTED behavior of an
// unregistered optional service (Property 5), lifecycle safety, mutual TLS with
// certificates minted by certificate_authority (Requirement 19.6), and
// concurrent calls with no cross-talk (Requirement 19.3), and the plaintext
// gate that keeps an unauthenticated listener or channel on this host unless
// allow_plaintext is set (.kiro/specs/grpc-plaintext-opt-in/).

#define BOOST_TEST_MODULE GrpcTransportIntegrationTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/grpc_exceptions.hpp>
#include <raft/grpc_transport_impl.hpp>

#include "recording_metrics.hpp"

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

// Same bundle with a metrics model that remembers what was emitted, for the
// cases whose only observable effect is a metric.
struct recording_types {
    template<typename T> using future_template = kythira::Future<T>;
    using metrics_type = kythira::testing::recording_metrics;
    using executor_type = folly::CPUThreadPoolExecutor;
};

// Bind port 0 so the kernel allocates a free port, then read it back with
// `server->bound_port()` after start() to address the server.
//
// This used to be a counter from a fixed base of 50751, one port per scenario,
// which avoided collisions *between these scenarios* but not with anything else
// on the machine -- and 50751 sits inside Linux's default ephemeral range
// (32768-60999), so the kernel is free to hand the same port to an unrelated
// process as a source port. On August 10 2026 it did: `mutual_tls_end_to_end`
// (the 6th scenario, so port 50756) failed on main at 7d9f51c with "Address
// already in use", identically on all three `--repeat until-pass:3` attempts
// because the squatter outlived them all.
//
// Picking a different constant only narrows the window -- any port chosen ahead
// of the bind can be taken before the bind happens. Port 0 closes it: the kernel
// reserves the port as it assigns it.
auto make_server(folly::CPUThreadPoolExecutor& exec, kythira::grpc_server_config cfg = {})
    -> std::unique_ptr<kythira::grpc_server<types>> {
    return std::make_unique<kythira::grpc_server<types>>("127.0.0.1", 0, std::move(cfg),
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

    auto server = make_server(exec);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server->start();
    BOOST_TEST(server->is_running());

    auto client = make_client(server->bound_port(), exec);
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

    auto server = make_server(exec);
    server->register_append_entries_handler(
        [](const kythira::append_entries_request<>&) -> kythira::append_entries_response<> {
            throw std::runtime_error("handler blew up");
        });
    server->start();

    auto client = make_client(server->bound_port(), exec);
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

    // Only a core handler is registered; the pre-vote extension service is
    // never added to the builder, so a pre-vote call must come back
    // UNIMPLEMENTED rather than hang or crash (Property 5, Requirement 6.3).
    auto server = make_server(exec);
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = false};
    });
    server->start();

    auto client = make_client(server->bound_port(), exec);
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
    auto server = make_server(exec);
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

    auto server = make_server(exec);
    // Echo the term back so each caller can verify it got its own response.
    server->register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(),
                                                ._vote_granted = (req.candidate_id() % 2 == 0)};
    });
    server->start();

    auto client = make_client(server->bound_port(), exec);
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

    kythira::grpc_server_config server_cfg;
    server_cfg.enable_tls = true;
    server_cfg.server_cert_pem = server_cert.certificate_pem;
    server_cfg.server_key_pem = server_cert.private_key_pem;
    server_cfg.ca_cert_pem = ca.root_certificate_pem();
    server_cfg.require_client_cert = true;

    auto server = make_server(exec, server_cfg);
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

    auto client = make_client(server->bound_port(), exec, client_cfg);
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
    BOOST_CHECK_THROW(make_server(exec, bad_server), kythira::grpc_tls_configuration_error);

    // Client with only half of an mTLS pair → also fails closed.
    kythira::grpc_client_config bad_client;
    bad_client.enable_tls = true;
    bad_client.client_cert_pem = "-----BEGIN CERTIFICATE-----\nnot-real\n-----END CERTIFICATE-----";
    // client_key_pem intentionally empty
    BOOST_CHECK_THROW(make_client(0, exec, bad_client), kythira::grpc_tls_configuration_error);
}

// A bind name listens on every address /etc/hosts lists it under, all on
// one port even when that port is ephemeral, and an IPv6 literal is
// bracketed rather than run into the port.
BOOST_AUTO_TEST_CASE(localhost_and_ipv6_binds_end_to_end) {
    folly::CPUThreadPoolExecutor exec(4);
    for (std::string bind : {"localhost", "*", "::1"}) {
        kythira::grpc_server_config cfg;
        // "*" listens on every interface, which plaintext may do only by
        // explicit opt-in (.kiro/specs/grpc-plaintext-opt-in/).
        cfg.allow_plaintext = bind == "*";
        kythira::grpc_server<types> server(bind, 0, cfg, kythira::noop_metrics{}, exec);
        server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
            return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
        });
        try {
            server.start();
        } catch (const kythira::grpc_transport_error& e) {
            // "::1" needs IPv6, which some hosts don't have.
            BOOST_TEST_MESSAGE("bind " << bind << " unavailable here: " << e.what());
            BOOST_TEST(bind == "::1");
            continue;
        }
        std::string target =
            (bind == "::1" ? "[::1]:" : "127.0.0.1:") + std::to_string(server.bound_port());
        std::unordered_map<std::uint64_t, std::string> book{{1, target}};
        kythira::grpc_client<types> client(std::move(book), {}, kythira::noop_metrics{}, exec);
        kythira::request_vote_request<> req{
            ._term = 3, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0};
        BOOST_TEST_INFO("bind " << bind);
        BOOST_TEST(client.send_request_vote(1, req, 2000ms).get().vote_granted());
        server.stop();
    }
}

BOOST_AUTO_TEST_CASE(unlisted_bind_name_is_refused) {
    folly::CPUThreadPoolExecutor exec(2);
    // An unlisted name is not loopback-only, so without the opt-in the
    // plaintext gate would refuse it in the constructor and the bind-name
    // check in start() would never run.
    kythira::grpc_server_config cfg;
    cfg.allow_plaintext = true;
    kythira::grpc_server<types> server("kythira-test.invalid", 0, cfg, kythira::noop_metrics{},
                                       exec);
    BOOST_CHECK_THROW(server.start(), kythira::grpc_transport_error);
}

// ── Plaintext gate (.kiro/specs/grpc-plaintext-opt-in/) ─────────────────────

// A plaintext listener on a wildcard address is refused at construction,
// before start() -- the only place a listening socket is ever opened -- so
// the refusal cannot leave a port bound. The error names the address, is
// FAILED_PRECONDITION, and existing TLS-misconfiguration catch sites still
// handle it.
BOOST_AUTO_TEST_CASE(plaintext_server_off_loopback_is_refused) {
    folly::CPUThreadPoolExecutor exec(1);
    for (std::string bind : {"0.0.0.0", "*", "::"}) {
        BOOST_TEST_INFO("bind " << bind);
        try {
            kythira::grpc_server<types> server(bind, 0, {}, kythira::noop_metrics{}, exec);
            BOOST_FAIL("plaintext server on " << bind << " was constructed");
        } catch (const kythira::grpc_plaintext_refused_error& e) {
            BOOST_TEST(e.address() == bind);
            BOOST_TEST(e.status_code() == grpc::StatusCode::FAILED_PRECONDITION);
            BOOST_TEST(std::string(e.what()).find("allow_plaintext") != std::string::npos);
        }
    }
    BOOST_CHECK_THROW(kythira::grpc_server<types>("0.0.0.0", 0, {}, kythira::noop_metrics{}, exec),
                      kythira::grpc_tls_configuration_error);
}

// Loopback-only binds still serve plaintext with no opt-in, exactly as before.
BOOST_AUTO_TEST_CASE(plaintext_server_on_loopback_needs_no_opt_in) {
    folly::CPUThreadPoolExecutor exec(4);
    for (std::string bind : {"127.0.0.1", "localhost", "::1"}) {
        kythira::testing::recording_metrics metrics;
        kythira::grpc_server<recording_types> server(bind, 0, {}, metrics, exec);
        server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
            return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
        });
        try {
            server.start();
        } catch (const kythira::grpc_transport_error& e) {
            BOOST_TEST_MESSAGE("bind " << bind << " unavailable here: " << e.what());
            BOOST_TEST(bind == "::1");  // needs IPv6, which some hosts don't have
            continue;
        }
        BOOST_TEST_INFO("bind " << bind);
        std::string target =
            (bind == "::1" ? "[::1]:" : "127.0.0.1:") + std::to_string(server.bound_port());
        kythira::grpc_client<types> client({{1, target}}, {}, kythira::noop_metrics{}, exec);
        kythira::request_vote_request<> req{
            ._term = 4, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0};
        BOOST_TEST(client.send_request_vote(1, req, 2000ms).get().vote_granted());
        // No opt-in was used, so none is reported.
        BOOST_TEST(metrics.recorder()->count_named("grpc.server.plaintext.enabled") == 0U);
        server.stop();
    }
}

// allow_plaintext admits a wildcard listener, and start() says so.
BOOST_AUTO_TEST_CASE(plaintext_server_opt_in_starts_and_is_reported) {
    folly::CPUThreadPoolExecutor exec(4);
    kythira::testing::recording_metrics metrics;
    kythira::grpc_server_config cfg;
    cfg.allow_plaintext = true;
    kythira::grpc_server<recording_types> server("0.0.0.0", 0, cfg, metrics, exec);
    server.register_request_vote_handler([](const kythira::request_vote_request<>& req) {
        return kythira::request_vote_response<>{._term = req.term(), ._vote_granted = true};
    });
    server.start();
    BOOST_TEST(server.is_running());

    const auto& rec = *metrics.recorder();
    BOOST_TEST(rec.count_named("grpc.server.plaintext.enabled") == 1U);
    BOOST_TEST(rec.dimension_values("grpc.server.plaintext.enabled", "bind_address") ==
                   std::vector<std::string>{"0.0.0.0"},
               boost::test_tools::per_element());
    BOOST_TEST(rec.dimension_values("grpc.server.plaintext.enabled", "loopback_only") ==
                   std::vector<std::string>{"false"},
               boost::test_tools::per_element());

    kythira::grpc_client<types> client({{1, "127.0.0.1:" + std::to_string(server.bound_port())}},
                                       {}, kythira::noop_metrics{}, exec);
    kythira::request_vote_request<> req{
        ._term = 5, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0};
    BOOST_TEST(client.send_request_vote(1, req, 2000ms).get().vote_granted());
    server.stop();
}

// With TLS on, the gate does not apply and allow_plaintext changes nothing
// (Property 2): a wildcard bind starts either way, and nothing reports
// plaintext.
BOOST_AUTO_TEST_CASE(tls_server_off_loopback_is_unaffected) {
    raft::testing::certificate_authority ca;
    raft::testing::leaf_certificate_options server_opts;
    server_opts.subject.common_name = "localhost";
    server_opts.dns_names = {"localhost"};
    server_opts.ip_addresses = {"127.0.0.1"};
    server_opts.server_auth = true;
    auto server_cert = ca.issue(server_opts);

    folly::CPUThreadPoolExecutor exec(2);
    for (bool allow_plaintext : {false, true}) {
        BOOST_TEST_INFO("allow_plaintext " << allow_plaintext);
        kythira::testing::recording_metrics metrics;
        kythira::grpc_server_config cfg;
        cfg.enable_tls = true;
        cfg.server_cert_pem = server_cert.certificate_pem;
        cfg.server_key_pem = server_cert.private_key_pem;
        cfg.allow_plaintext = allow_plaintext;
        kythira::grpc_server<recording_types> server("0.0.0.0", 0, cfg, metrics, exec);
        server.start();
        BOOST_TEST(server.is_running());
        BOOST_TEST(metrics.recorder()->count_named("grpc.server.plaintext.enabled") == 0U);
        server.stop();
    }
}

// A plaintext client refuses, at construction, any configured target that
// can leave this host, and names it.
BOOST_AUTO_TEST_CASE(plaintext_client_to_remote_target_is_refused) {
    folly::CPUThreadPoolExecutor exec(1);
    std::unordered_map<std::uint64_t, std::string> book{{1, "127.0.0.1:5000"},
                                                        {2, "10.0.0.1:5000"}};
    try {
        kythira::grpc_client<types> client(book, {}, kythira::noop_metrics{}, exec);
        BOOST_FAIL("plaintext client to 10.0.0.1 was constructed");
    } catch (const kythira::grpc_plaintext_refused_error& e) {
        BOOST_TEST(e.address() == "10.0.0.1:5000");
        BOOST_TEST(e.status_code() == grpc::StatusCode::FAILED_PRECONDITION);
    }
    // Local targets of every form pass without the opt-in.
    kythira::grpc_client<types> local({{1, "127.0.0.1:5000"},
                                       {2, "[::1]:5000"},
                                       {3, "localhost:5000"},
                                       {4, "unix:/tmp/kythira-raft.sock"}},
                                      {}, kythira::noop_metrics{}, exec);
}

// allow_plaintext admits the same remote target, and the client says so.
BOOST_AUTO_TEST_CASE(plaintext_client_opt_in_constructs_and_is_reported) {
    folly::CPUThreadPoolExecutor exec(1);
    kythira::testing::recording_metrics metrics;
    kythira::grpc_client_config cfg;
    cfg.allow_plaintext = true;
    kythira::grpc_client<recording_types> client({{1, "10.0.0.1:5000"}}, cfg, metrics, exec);
    const auto& rec = *metrics.recorder();
    BOOST_TEST(rec.count_named("grpc.client.plaintext.enabled") == 1U);
    BOOST_TEST(rec.dimension_values("grpc.client.plaintext.enabled", "loopback_only") ==
                   std::vector<std::string>{"false"},
               boost::test_tools::per_element());
}

// The bootstrap path dials an address that was never configured, so it is
// checked per call: a refused address fails that call and caches no channel,
// and a later call to a loopback address still goes through.
BOOST_AUTO_TEST_CASE(plaintext_bootstrap_to_remote_address_is_refused) {
    folly::CPUThreadPoolExecutor exec(4);
    kythira::grpc_server<types> server("127.0.0.1", 0, {}, kythira::noop_metrics{}, exec);
    server.register_cluster_join_handler([](const kythira::cluster_join_request<>&) {
        return kythira::cluster_join_response<>{.accepted = true, .redirect = std::nullopt};
    });
    server.start();

    kythira::testing::recording_metrics metrics;
    kythira::grpc_client<recording_types> client({}, {}, metrics, exec);
    kythira::cluster_join_request<> req{.node_id = 9, .contact_address = "127.0.0.1:1"};

    BOOST_CHECK_THROW((void)client.send_cluster_join_request("10.0.0.1:5000", req, 2000ms),
                      kythira::grpc_plaintext_refused_error);
    BOOST_CHECK_THROW((void)client.send_cluster_join_request("10.0.0.1:5000", req, 2000ms),
                      kythira::grpc_plaintext_refused_error);
    BOOST_TEST(metrics.recorder()->count_named("grpc.client.channel.created") == 0U);

    auto addr = "127.0.0.1:" + std::to_string(server.bound_port());
    BOOST_TEST(client.send_cluster_join_request(addr, req, 2000ms).get().is_accepted());
    BOOST_TEST(metrics.recorder()->count_named("grpc.client.channel.created") == 1U);
    server.stop();
}
