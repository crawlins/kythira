/**
 * End-to-end sanity check for ion_rpc_serializer over both the real HTTP
 * (cpp-httplib) and CoAP transports (.kiro/specs/ion-rpc-serializer/tasks.md,
 * task 9.4).
 *
 * Mirrors tests/coap_cbor_end_to_end_test.cpp's own precedent for
 * cbor_rpc_serializer (cbor-rpc-serializer task 10.3): starts a real server
 * bound to localhost, points a real client at it, and drives a full
 * RequestVote/AppendEntries/InstallSnapshot cycle over an actual socket --
 * both sides configured with ion_rpc_serializer as Types::serializer_type.
 * Extended here to cover HTTP as well as CoAP, since Requirement 6 (and this
 * spec's own task 9.4) explicitly calls for both.
 */
#define BOOST_TEST_MODULE ion_http_coap_end_to_end_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/coap_transport.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>  // test_transport_types::executor_type below is folly::Executor directly
#include <raft/coap_transport_impl.hpp>
#include <raft/coap_utils.hpp>
#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/ion_serializer.hpp>
#include <raft/console_logger.hpp>
#include <raft/executor_default.hpp>
#include <raft/serializer_registry.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace kythira;

namespace {
constexpr const char* test_bind_address = "127.0.0.1";
constexpr std::uint16_t coap_bind_port = 57931;
constexpr std::uint16_t http_bind_port = 58231;
constexpr std::uint64_t test_node_id = 1;
constexpr std::chrono::milliseconds test_timeout{5000};

using ion_serializer_type = kythira::ion_rpc_serializer<std::vector<std::byte>>;

// Matches coap_cbor_end_to_end_test.cpp's own test_transport_types shape:
// future_template<T> must stay genuinely parameterized over T because
// coap_client has three RPCs with three distinct Response types.
struct coap_test_transport_types {
    using serializer_type = ion_serializer_type;
    using serializer_registry_type = kythira::single_serializer_registry<ion_serializer_type>;
    using rpc_serializer_type = ion_serializer_type;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

using http_test_transport_types =
    kythira::http_transport_types<ion_serializer_type, kythira::noop_metrics,
                                  kythira::executor_default>;

template<typename Server> auto register_handlers(Server& server) -> void {
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return kythira::request_vote_response<>{req.term(), true};
        });
    server.register_append_entries_handler(
        [](const kythira::append_entries_request<>& req) -> kythira::append_entries_response<> {
            return kythira::append_entries_response<>{req.term(), true, std::nullopt, std::nullopt};
        });
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            return kythira::install_snapshot_response<>{req.term()};
        });
}
}  // namespace

BOOST_AUTO_TEST_SUITE(ion_http_coap_end_to_end_tests)

// **Feature: ion-rpc-serializer, task 9.4: End-to-end sanity check over HTTP
// and CoAP transports**
// Property: ion_rpc_serializer's name() maps to "application/ion" via both
// transports' shared serializer-name detection (coap_utils), the single
// source of truth content_type_for_serializer (HTTP) also routes through.
BOOST_AUTO_TEST_CASE(test_ion_content_type_and_format_mapping) {
    ion_serializer_type binary_serializer(kythira::ion_encoding::binary);
    ion_serializer_type text_serializer(kythira::ion_encoding::text);
    BOOST_TEST(binary_serializer.name() == "ion-binary");
    BOOST_TEST(text_serializer.name() == "ion-text");

    for (const auto& serializer_name : {binary_serializer.name(), text_serializer.name()}) {
        auto format = coap_utils::get_content_format_for_serializer(serializer_name);
        BOOST_TEST(static_cast<int>(format) ==
                   static_cast<int>(coap_utils::coap_content_format::application_ion));
        BOOST_TEST(coap_utils::content_format_to_string(format) == "application/ion");
        BOOST_TEST(kythira::content_type_for_serializer(ion_serializer_type(
                       serializer_name == "ion-binary" ? kythira::ion_encoding::binary
                                                       : kythira::ion_encoding::text)) ==
                   "application/ion");
    }
}

BOOST_AUTO_TEST_CASE(test_ion_coap_round_trip, *boost::unit_test::timeout(30)) {
    kythira::coap_server_config server_config;
    server_config.enable_dtls = false;
    kythira::noop_metrics server_metrics;
    coap_server<coap_test_transport_types> server(test_bind_address, coap_bind_port, server_config,
                                                  server_metrics);
    register_handlers(server);
    server.start();

    kythira::coap_client_config client_config;
    client_config.enable_dtls = false;
    std::unordered_map<std::uint64_t, std::string> endpoints;
    endpoints[test_node_id] = std::format("coap://{}:{}", test_bind_address, coap_bind_port);
    kythira::noop_metrics client_metrics;
    coap_client<coap_test_transport_types> client(std::move(endpoints), client_config,
                                                  client_metrics);

    {
        kythira::request_vote_request<> request{7, 42, 3, 6};
        auto future = client.send_request_vote(test_node_id, request, test_timeout);
        BOOST_REQUIRE(future.wait(test_timeout));
        auto response = std::move(future).get();
        BOOST_TEST(response.term() == 7);
        BOOST_TEST(response.vote_granted());
    }
    {
        kythira::append_entries_request<> request{7, 1, 2, 6, {}, 1};
        auto future = client.send_append_entries(test_node_id, request, test_timeout);
        BOOST_REQUIRE(future.wait(test_timeout));
        auto response = std::move(future).get();
        BOOST_TEST(response.term() == 7);
        BOOST_TEST(response.success());
    }
    {
        std::vector<std::byte> snapshot_data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        kythira::install_snapshot_request<> request{7, 1, 5, 6, 0, snapshot_data, true};
        auto future = client.send_install_snapshot(test_node_id, request, test_timeout);
        BOOST_REQUIRE(future.wait(test_timeout));
        auto response = std::move(future).get();
        BOOST_TEST(response.term() == 7);
    }

    server.stop();
}

BOOST_AUTO_TEST_CASE(test_ion_http_round_trip) {
    kythira::cpp_httplib_server<http_test_transport_types> server(test_bind_address, http_bind_port,
                                                                  {}, kythira::noop_metrics{});
    register_handlers(server);
    server.start();
    BOOST_TEST(server.is_running());

    std::unordered_map<std::uint64_t, std::string> node_map{
        {test_node_id,
         std::string("http://") + test_bind_address + ":" + std::to_string(http_bind_port)}};
    kythira::cpp_httplib_client<http_test_transport_types> client(node_map, {},
                                                                  kythira::noop_metrics{});

    {
        kythira::request_vote_request<> request{7, 42, 3, 6};
        auto response =
            std::move(client.send_request_vote(test_node_id, request, test_timeout)).get();
        BOOST_TEST(response.term() == 7);
        BOOST_TEST(response.vote_granted());
    }
    {
        kythira::append_entries_request<> request{7, 1, 2, 6, {}, 1};
        auto response =
            std::move(client.send_append_entries(test_node_id, request, test_timeout)).get();
        BOOST_TEST(response.term() == 7);
        BOOST_TEST(response.success());
    }
    {
        std::vector<std::byte> snapshot_data{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
        kythira::install_snapshot_request<> request{7, 1, 5, 6, 0, snapshot_data, true};
        auto response =
            std::move(client.send_install_snapshot(test_node_id, request, test_timeout)).get();
        BOOST_TEST(response.term() == 7);
    }

    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()
