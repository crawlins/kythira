// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_concept_conformance_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>

#include <chrono>
#include <type_traits>
#include <utility>

// Unlike coap_concept_conformance_test.cpp, this header is included
// unconditionally: coap_transport_libnyoci_impl.hpp compiles with or without
// LIBNYOCI_AVAILABLE and keeps its full concept surface either way, so the
// static_asserts below hold in a build that has no libnyoci at all. That is
// the point — the concept conformance this file asserts is a property of the
// adapter, not of whether the C library happened to be found (Requirement 7.1,
// 7.5). It deliberately does NOT include raft/coap_transport.hpp: libcoap's
// and libnyoci's C headers cannot share a translation unit.
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

namespace {
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using test_metrics = kythira::noop_metrics;

// coap_libnyoci_client has three RPCs with three distinct Response types, so
// Types::future_template<T> must be genuinely parameterized over T — see the
// same note in coap_concept_conformance_test.cpp.
struct test_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = test_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};

using test_client = kythira::coap_libnyoci_client<test_types>;
using test_server = kythira::coap_libnyoci_server<test_types>;
}

BOOST_AUTO_TEST_SUITE(coap_libnyoci_concept_conformance_tests)

// The whole reason the alternate backend exists: the Raft layer selects it
// without any consensus-layer change, because it satisfies the same concepts
// the libcoap backend does (Requirements 1.1, 1.2, 1.4).
BOOST_AUTO_TEST_CASE(test_libnyoci_client_satisfies_network_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    static_assert(kythira::network_client<test_client>,
                  "coap_libnyoci_client must satisfy the network_client concept");
    BOOST_TEST(kythira::network_client<test_client>);
}

BOOST_AUTO_TEST_CASE(test_libnyoci_server_satisfies_network_server,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    static_assert(kythira::network_server<test_server>,
                  "coap_libnyoci_server must satisfy the network_server concept");
    BOOST_TEST(kythira::network_server<test_server>);
}

// Requirement 1.5: the serializer is a pure substitution. The backend is
// templated on Types and never names a concrete serializer, so a Types bundle
// carrying a genuinely different negotiation surface — two serializers instead
// of one, hence Accept lists and Content-Format numbers this test's default
// bundle never produces — conforms exactly the same way.
namespace {
struct cbor_and_json_types : test_types {
    using serializer_type = kythira::cbor_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type =
        kythira::multi_serializer_registry<kythira::cbor_rpc_serializer<std::vector<std::byte>>,
                                           test_serializer>;
};
}

BOOST_AUTO_TEST_CASE(test_conformance_is_independent_of_serializer,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    static_assert(kythira::network_client<kythira::coap_libnyoci_client<cbor_and_json_types>>,
                  "conformance must not depend on which serializers Types carries");
    static_assert(kythira::network_server<kythira::coap_libnyoci_server<cbor_and_json_types>>,
                  "conformance must not depend on which serializers Types carries");
    BOOST_TEST(true);
}

// Requirement 1.1: the futures the client hands back are exactly the ones
// Types names, not a backend-specific type that merely converts. This is what
// lets a caller hold `Types::future_template<...>` and stay backend-agnostic.
BOOST_AUTO_TEST_CASE(test_client_returns_the_types_bundle_future,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    static_assert(std::is_same_v<decltype(std::declval<test_client&>().send_request_vote(
                                     0, std::declval<const kythira::request_vote_request<>&>(),
                                     std::chrono::milliseconds{1})),
                                 kythira::future_default<kythira::request_vote_response<>>>);
    static_assert(std::is_same_v<decltype(std::declval<test_client&>().send_append_entries(
                                     0, std::declval<const kythira::append_entries_request<>&>(),
                                     std::chrono::milliseconds{1})),
                                 kythira::future_default<kythira::append_entries_response<>>>);
    static_assert(std::is_same_v<decltype(std::declval<test_client&>().send_install_snapshot(
                                     0, std::declval<const kythira::install_snapshot_request<>&>(),
                                     std::chrono::milliseconds{1})),
                                 kythira::future_default<kythira::install_snapshot_response<>>>);
    BOOST_TEST(true);
}

// The gate itself: a build without libnyoci still compiles everything above,
// and says so at run time rather than by failing to link (Requirement 2.2).
BOOST_AUTO_TEST_CASE(test_backend_availability_matches_the_build,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
#ifdef LIBNYOCI_AVAILABLE
    BOOST_TEST(test_client::backend_available());
    BOOST_TEST(test_server::backend_available());
#else
    BOOST_TEST(!test_client::backend_available());
    BOOST_TEST(!test_server::backend_available());
    BOOST_TEST_MESSAGE("libnyoci not available — the runtime backend is compiled out");
#endif
}

BOOST_AUTO_TEST_SUITE_END()
