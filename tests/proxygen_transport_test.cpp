#define BOOST_TEST_MODULE proxygen_transport_test
#include <boost/test/unit_test.hpp>

#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/http_transport.hpp>
#include <raft/json_serializer.hpp>
#include <raft/executor_default.hpp>
#include <raft/network.hpp>

#include <folly/executors/IOThreadPoolExecutor.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t test_bind_port = 18199;
constexpr std::uint16_t test_tls_bind_port = 18200;
constexpr std::uint16_t test_multi_bind_port_base = 18210;
constexpr const char* test_server_url = "http://127.0.0.1:18199";
constexpr std::uint64_t test_node_id = 1;

// Requirement 12.6/19.3: a metrics implementation that records every
// emitted metric name/dimension pair, so a test can positively confirm
// *which* internal path (Folly fast path vs. generic bridge) a given RPC
// actually took -- success alone doesn't distinguish that (Requirement
// 19.3's own point). Copyable (proxygen_client/server copy `_metrics` per
// call, `auto metric = _metrics;`), backed by a shared_ptr so every copy
// still records into the same log.
class recording_metrics {
public:
    struct entry {
        std::string name;
        std::unordered_map<std::string, std::string> dimensions;
    };

    recording_metrics() : _state(std::make_shared<state>()) {}

    auto set_metric_name(std::string_view name) -> void { _current.name = std::string(name); }
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _current.dimensions[std::string(dimension_name)] = std::string(dimension_value);
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        std::lock_guard<std::mutex> lock(_state->mutex);
        _state->entries.push_back(_current);
        _current = entry{};
    }

    [[nodiscard]] auto entries_named(std::string_view name) const -> std::vector<entry> {
        std::lock_guard<std::mutex> lock(_state->mutex);
        std::vector<entry> result;
        for (const auto& e : _state->entries) {
            if (e.name == name) {
                result.push_back(e);
            }
        }
        return result;
    }

private:
    struct state {
        std::mutex mutex;
        std::vector<entry> entries;
    };
    std::shared_ptr<state> _state;
    entry _current;
};

static_assert(kythira::metrics<recording_metrics>);

using test_transport_types =
    kythira::future_default_proxygen_transport_types<kythira::json_serializer, recording_metrics,
                                                     kythira::executor_default>;

auto register_echo_handlers(kythira::proxygen_server<test_transport_types>& server) -> void {
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
}

// Real, valid self-signed cert/key pair via the `openssl` CLI, matching
// beast_transport_test.cpp's own temp_tls_material -- exercises a genuine
// TLS handshake through folly::SSLContext/wangle::SSLContextConfig, not
// just config-field validation.
struct temp_tls_material {
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    temp_tls_material() {
        auto dir = std::filesystem::temp_directory_path();
        auto unique = std::to_string(std::random_device{}());
        cert_path = dir / ("proxygen_test_cert_" + unique + ".pem");
        key_path = dir / ("proxygen_test_key_" + unique + ".pem");
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

static_assert(kythira::proxygen_future_default_transport_types<test_transport_types>);
static_assert(kythira::network_client<kythira::proxygen_client<test_transport_types>>);
static_assert(kythira::network_server<kythira::proxygen_server<test_transport_types>>);

// Requirement 15.2: the narrower concept proxygen_client/server actually
// require must reject a Types bundle that merely satisfies transport_types
// but doesn't pin future_template to kythira::future_default.
using http_transport_types_for_proxygen_test =
    kythira::http_transport_types<kythira::json_serializer, recording_metrics,
                                  kythira::executor_default>;
static_assert(kythira::transport_types<http_transport_types_for_proxygen_test>);
static_assert(
    !kythira::proxygen_future_default_transport_types<http_transport_types_for_proxygen_test>);

BOOST_AUTO_TEST_SUITE(proxygen_transport_tests)

BOOST_AUTO_TEST_CASE(request_vote_round_trip_and_connection_reuse, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, test_bind_port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 42;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(3000)))
            .get();
    BOOST_TEST(resp.term() == 42);
    BOOST_TEST(resp.vote_granted());

    // Second RPC to the same node -- exercises connection reuse (Property 5).
    auto resp2 =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(3000)))
            .get();
    BOOST_TEST(resp2.vote_granted());

    server.stop();
}

// Requirement 19.3/Property 11: confirms the Folly fast path is actually
// taken (not merely that the RPC succeeded) under
// KYTHIRA_DEFAULT_FUTURE_BACKEND=folly, this project's default -- via
// Requirement 12.6's metrics path label, the one compile-time-visible
// signal that distinguishes the two internal code paths from the outside.
BOOST_AUTO_TEST_CASE(folly_fast_path_is_taken, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 1);

    recording_metrics server_metrics;
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, server_metrics,
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    recording_metrics client_metrics;
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          client_metrics);

    kythira::request_vote_request<> req{};
    req._term = 1;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(3000)))
            .get();
    BOOST_TEST(resp.vote_granted());

    auto sent = client_metrics.entries_named("proxygen_http.client.request.sent");
    BOOST_REQUIRE(!sent.empty());
    BOOST_TEST(sent.front().dimensions.at("path") == "folly_fast_path");

    server.stop();
}

// Property 6: concurrent RPCs to different target nodes run genuinely
// concurrently, potentially on different EventBase threads of the shared
// folly::IOThreadPoolExecutor, and each gets the right response back.
BOOST_AUTO_TEST_CASE(concurrent_rpcs_to_multiple_nodes, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(4);

    constexpr int node_count = 4;
    std::vector<std::unique_ptr<kythira::proxygen_server<test_transport_types>>> servers;
    std::unordered_map<std::uint64_t, std::string> node_map;
    for (int i = 0; i < node_count; ++i) {
        auto port = static_cast<std::uint16_t>(test_multi_bind_port_base + i);
        auto server = std::make_unique<kythira::proxygen_server<test_transport_types>>(
            test_bind_address, port, kythira::proxygen_server_config{}, recording_metrics{},
            std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
        register_echo_handlers(*server);
        server->start();
        node_map[static_cast<std::uint64_t>(i + 1)] =
            std::string("http://127.0.0.1:") + std::to_string(port);
        servers.push_back(std::move(server));
    }

    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, {},
                                                          recording_metrics{});

    std::vector<kythira::request_vote_response<>> responses(static_cast<std::size_t>(node_count));
    std::vector<std::thread> rpc_threads;
    for (int i = 0; i < node_count; ++i) {
        kythira::request_vote_request<> req{};
        req._term = static_cast<std::uint64_t>(100 + i);
        rpc_threads.emplace_back([&client, &responses, i, req] {
            responses[static_cast<std::size_t>(i)] =
                std::move(client.send_request_vote(static_cast<std::uint64_t>(i + 1), req,
                                                   std::chrono::milliseconds(3000)))
                    .get();
        });
    }
    for (auto& t : rpc_threads) {
        t.join();
    }

    for (int i = 0; i < node_count; ++i) {
        const auto& resp = responses[static_cast<std::size_t>(i)];
        BOOST_TEST(resp.term() == static_cast<std::uint64_t>(100 + i));
        BOOST_TEST(resp.vote_granted());
    }

    for (auto& server : servers) {
        server->stop();
    }
}

BOOST_AUTO_TEST_CASE(tls_request_vote_round_trip, *boost::unit_test::timeout(30)) {
    temp_tls_material tls;
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, test_tls_bind_port, server_config, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    // enable_ssl_verification=false: this test's self-signed cert has no CA
    // to chain to -- exercising the encrypted-transport code path (SNI via
    // HTTPConnector::connectSSL, handshake, HTTPUpstreamSession reads/
    // writes) is the point here, not peer certificate validation.
    kythira::proxygen_client_config client_config;
    client_config.enable_ssl_verification = false;
    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id, std::string("https://127.0.0.1:") + std::to_string(test_tls_bind_port)}};
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 99;
    auto resp =
        std::move(client.send_request_vote(test_node_id, req, std::chrono::milliseconds(3000)))
            .get();
    BOOST_TEST(resp.term() == 99);
    BOOST_TEST(resp.vote_granted());

    server.stop();
}

// Requirement 7.2-7.3: reload_tls_material() validates new material
// all-or-nothing; a server not configured for TLS at all rejects the call
// outright.
BOOST_AUTO_TEST_CASE(server_reload_tls_material, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(2);

    kythira::proxygen_server<test_transport_types> plain_server(
        test_bind_address, static_cast<std::uint16_t>(test_bind_port + 2), {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    BOOST_CHECK_THROW(plain_server.reload_tls_material(), std::exception);

    temp_tls_material tls;
    kythira::proxygen_server_config server_config;
    server_config.enable_ssl = true;
    server_config.ssl_cert_path = tls.cert_path.string();
    server_config.ssl_key_path = tls.key_path.string();
    kythira::proxygen_server<test_transport_types> tls_server(
        test_bind_address, static_cast<std::uint16_t>(test_bind_port + 3), server_config,
        recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    tls_server.start();
    BOOST_CHECK_NO_THROW(tls_server.reload_tls_material());

    std::filesystem::remove(tls.cert_path);
    BOOST_CHECK_THROW(tls_server.reload_tls_material(), std::exception);
    tls_server.stop();
}

// Requirement 7.1: the client side of the same all-or-nothing reload
// contract.
BOOST_AUTO_TEST_CASE(client_reload_tls_material, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(2);
    std::unordered_map<std::uint64_t, std::string> node_map{{test_node_id, test_server_url}};

    temp_tls_material tls;
    kythira::proxygen_client_config client_config;
    client_config.client_cert_path = tls.cert_path.string();
    client_config.client_key_path = tls.key_path.string();
    kythira::proxygen_client<test_transport_types> client(io_executor, node_map, client_config,
                                                          recording_metrics{});
    BOOST_CHECK_NO_THROW(client.reload_tls_material());

    std::filesystem::remove(tls.key_path);
    BOOST_CHECK_THROW(client.reload_tls_material(), std::exception);
}

// Requirement 4.2-4.4: malformed request body -> 400; unregistered path ->
// 404 (surfaced client-side as http_client_error).
BOOST_AUTO_TEST_CASE(malformed_request_handling, *boost::unit_test::timeout(30)) {
    folly::IOThreadPoolExecutor io_executor(2);
    auto port = static_cast<std::uint16_t>(test_bind_port + 4);

    kythira::proxygen_server<test_transport_types> server(
        test_bind_address, port, {}, recording_metrics{},
        std::shared_ptr<folly::IOThreadPoolExecutorBase>(&io_executor, [](auto*) {}));
    register_echo_handlers(server);
    server.start();

    std::string response_body;
    unsigned status_code = 0;
    server.dispatch("/v1/raft/request_vote", std::vector<std::byte>{}, response_body, status_code);
    BOOST_TEST(status_code == 400);

    server.dispatch("/v1/raft/unknown_endpoint", std::vector<std::byte>{}, response_body,
                    status_code);
    BOOST_TEST(status_code == 404);

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
