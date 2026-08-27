// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE beast_client_test
#include <boost/test/unit_test.hpp>

#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// Client-focused slice of the one-file-per-concern split of what used to be
// tests/beast_transport_test.cpp -- one-to-one with tests/http_client_*,
// covering client-only concerns (construction, concept compliance, TLS
// reload) rather than full request/response round trips, which live in
// beast_integration_test.cpp instead (matching how http_client_test.cpp
// itself stays construction/config-level and defers round trips to
// http_integration_test.cpp).
namespace {
constexpr const char* test_server_url = "http://127.0.0.1:18099";
constexpr std::uint64_t test_node_id = 1;

using test_transport_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;

// Generates a real, valid self-signed cert/key pair via the `openssl` CLI
// (not a mocked or placeholder PEM) so the reload test below exercises
// boost_beast_client's actual certificate-validation code path, not just a
// hardcoded fixture.
struct temp_tls_material {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    temp_tls_material() {
        auto dir = std::filesystem::temp_directory_path();
        auto unique = std::to_string(std::random_device{}());
        cert_path = dir / ("beast_test_cert_" + unique + ".pem");
        key_path = dir / ("beast_test_key_" + unique + ".pem");
        std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path.string() +
                          " -out " + cert_path.string() +
                          " -days 1 -nodes -subj \"/CN=127.0.0.1\" -addext "
                          "\"subjectAltName=IP:127.0.0.1\" 2>/dev/null";
        int rc = std::system(cmd.c_str());
        BOOST_REQUIRE_MESSAGE(rc == 0,
                              "openssl CLI must be available to generate test TLS material");
        BOOST_REQUIRE(std::filesystem::exists(cert_path));
        BOOST_REQUIRE(std::filesystem::exists(key_path));
    }

    ~temp_tls_material() {
        std::error_code ec;
        std::filesystem::remove(cert_path, ec);
        std::filesystem::remove(key_path, ec);
    }
};

}  // namespace

// **Feature: boost-beast-http-transport, Property 10: Concept Compliance**
// **Validates: Requirement 16.1**
static_assert(kythira::future_default_transport_types<test_transport_types>);
static_assert(kythira::network_client<kythira::boost_beast_client<test_transport_types>>);

// Requirement 19.4: the narrower concept boost_beast_client actually
// requires must reject a Types bundle that merely satisfies transport_types
// but doesn't pin future_template to kythira::future_default -- confirming
// this is a real, checked constraint, not just documentation.
using http_transport_types_for_beast_test =
    kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                  kythira::executor_default>;
static_assert(kythira::transport_types<http_transport_types_for_beast_test>);
static_assert(!kythira::future_default_transport_types<http_transport_types_for_beast_test>);

BOOST_AUTO_TEST_SUITE(beast_client_tests)

// **Feature: boost-beast-http-transport, Property 7: TLS Material Reload Atomicity**
// **Validates: Requirement 7.3**
// Requirement 7.1: the client side of the all-or-nothing reload contract,
// applied to client_cert_path/client_key_path/ca_cert_path. No server is
// started here -- reload_tls_material() only re-validates on-disk material
// and swaps the client's own SSL context, neither of which needs a live
// peer to exercise.
BOOST_AUTO_TEST_CASE(client_reload_tls_material) {
    boost::asio::io_context ioc;
    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};

    temp_tls_material tls;
    kythira::boost_beast_client_config client_config;
    client_config.client_cert_path = tls.cert_path.string();
    client_config.client_key_path = tls.key_path.string();
    kythira::boost_beast_client<test_transport_types> client(ioc, node_map, client_config,
                                                             kythira::noop_metrics{});
    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    // Remove the on-disk key *after* successful construction, so this
    // exercises reload()'s own re-validation rather than the constructor's.
    std::filesystem::remove(tls.key_path);
    BOOST_CHECK_THROW(client.reload_tls_material(), std::exception);
}

// ── concurrent RPCs to one target ────────────────────────────────────────────
//
// A regression test for a crash, and the reason this client no longer shares a
// single connection per target between concurrent RPCs.
//
// The class used to keep exactly one connection per target and let every
// concurrent RPC to that target use it at once, on the reasoning that the
// connection's `net::strand` serialized them. A strand serializes handler
// invocation, not composed operations: `async_write` then `async_read` is two
// composed operations, and two RPCs sharing one stream interleave regardless of
// strand. The second RPC's response framing broke and surfaced as
// `end of stream`; the failure path then tore the shared connection down
// underneath the *other* in-flight RPCs; and because `send_rpc` held a
// `pooled_connection&` into the map that teardown erased, the reference
// dangled and `connection->is_open()` faulted at address 0x0.
//
// It took real concurrency to see: every pre-existing suite drove either one
// RPC at a time or several to *different* targets, which is the case that
// always worked. It was found by a multi-Raft workload, where four groups on
// four executor stripes replicate to the same two peers simultaneously.
//
// The control is the sequential arm. If both arms are clean the fix holds; if
// only the sequential arm is clean the pooling has regressed to sharing.
namespace {

[[nodiscard]] auto reserve_loopback_port() -> std::uint16_t {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    BOOST_REQUIRE_MESSAGE(fd >= 0, "socket() failed");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    BOOST_REQUIRE_MESSAGE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                          "bind() failed");
    socklen_t len = sizeof(addr);
    BOOST_REQUIRE_MESSAGE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
                          "getsockname() failed");
    const auto port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

struct concurrency_arm_result {
    std::size_t attempted{0};
    std::size_t failed{0};
    std::string first_error;
};

auto run_same_target_arm(std::size_t threads, std::size_t per_thread) -> concurrency_arm_result {
    boost::asio::io_context ioc;
    auto work = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 4; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    const auto port = reserve_loopback_port();
    kythira::boost_beast_server<test_transport_types> server(
        ioc, "127.0.0.1", port, kythira::boost_beast_server_config{}, kythira::noop_metrics{});
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            kythira::request_vote_response<> resp{};
            resp._term = req.term();
            resp._vote_granted = true;
            return resp;
        });
    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            kythira::append_entries_response<> resp{};
            resp._term = req.term();
            resp._success = true;
            return resp;
        });
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            kythira::install_snapshot_response<> resp{};
            resp._term = req.term();
            return resp;
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> urls{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<test_transport_types> client(
        ioc, urls, kythira::boost_beast_client_config{}, kythira::noop_metrics{});

    std::atomic<std::size_t> failed{0};
    std::mutex first_error_mutex;
    std::string first_error;
    {
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (std::size_t t = 0; t < threads; ++t) {
            workers.emplace_back([&, t] {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    kythira::request_vote_request<> req{};
                    req._term = static_cast<std::uint64_t>(t * per_thread + i + 1);
                    try {
                        auto response =
                            std::move(client.send_request_vote(test_node_id, req,
                                                               std::chrono::milliseconds{5000}))
                                .get();
                        // The response has to belong to THIS request. Two RPCs
                        // interleaved on one stream can each read a well-formed
                        // response that answers the other one, which a
                        // did-it-throw check alone would call success.
                        if (response.term() != req.term()) {
                            failed.fetch_add(1);
                            std::lock_guard lock(first_error_mutex);
                            if (first_error.empty()) {
                                first_error = "response carried term " +
                                              std::to_string(response.term()) + ", expected " +
                                              std::to_string(req.term());
                            }
                        }
                    } catch (const std::exception& e) {
                        failed.fetch_add(1);
                        std::lock_guard lock(first_error_mutex);
                        if (first_error.empty()) {
                            first_error = e.what();
                        }
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
    }

    server.stop();
    work.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }

    return concurrency_arm_result{threads * per_thread, failed.load(), first_error};
}

}  // namespace

BOOST_AUTO_TEST_CASE(client_sequential_rpcs_to_one_target, *boost::unit_test::timeout(120)) {
    const auto arm = run_same_target_arm(1, 40);
    BOOST_TEST_MESSAGE("sequential (1 x 40): " << arm.failed << "/" << arm.attempted << " failed");
    BOOST_CHECK_MESSAGE(arm.failed == 0, "sequential RPCs failed: " << arm.first_error);
}

BOOST_AUTO_TEST_CASE(client_concurrent_rpcs_to_one_target, *boost::unit_test::timeout(120)) {
    const auto arm = run_same_target_arm(8, 20);
    BOOST_TEST_MESSAGE("concurrent (8 x 20): " << arm.failed << "/" << arm.attempted << " failed");
    BOOST_CHECK_MESSAGE(arm.failed == 0, "concurrent RPCs to one target failed ("
                                             << arm.failed << "/" << arm.attempted
                                             << "): " << arm.first_error);
}

BOOST_AUTO_TEST_SUITE_END()
