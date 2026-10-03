// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// coap-alternate-backend-security-parity on the libnyoci backend: revocation,
// cn_validator and ACE-OAuth behave as they do on libcoap. The cases
// themselves live in coap_alternate_security_parity_cases.hpp, shared with the
// cantcoap suite so both alternates are held to the same ones.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_security_parity_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#define BOOST_TEST_TIMEOUT (300 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
// Deliberately NOT raft/coap_transport.hpp: libcoap's and libnyoci's headers
// cannot share a translation unit (see coap_transport_libnyoci_impl.hpp).
#include <raft/coap_transport_libnyoci_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifdef LIBNYOCI_AVAILABLE

namespace parity {

struct test_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type = kythira::single_serializer_registry<serializer_type>;
    using rpc_serializer_type = serializer_type;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using test_client = kythira::coap_libnyoci_client<test_types>;
using test_server = kythira::coap_libnyoci_server<test_types>;

// The scheme is the backend's business: it dials DTLS when the config asks
// for it, whatever the endpoint string says.
[[nodiscard]] inline auto endpoint_for(std::uint16_t port) -> std::string {
    return "coap://127.0.0.1:" + std::to_string(port);
}

[[nodiscard]] inline auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    return config;
}

}  // namespace parity

#include "coap_alternate_security_parity_cases.hpp"

// ── cn_validator (Requirement 2) ───────────────────────────────────────────
// cantcoap already enforced cn_validator before this spec, and its own DTLS
// suite covers it; on libnyoci it was ignored outright.

BOOST_AUTO_TEST_SUITE(cn_validator)

namespace {

auto validator_round_trip(std::function<bool(const std::string&)> validator, bool on_server)
    -> std::pair<bool, bool> {
    parity::revocation_pki pki;
    auto server_security = parity::pki_security(pki, pki.server_cert_file, pki.server_key_file);
    auto client_security = parity::pki_security(pki, pki.good_cert_file, pki.good_key_file);
    if (on_server) {
        server_security = parity::with_validator(server_security, validator);
    } else {
        client_security = parity::with_validator(client_security, validator);
    }
    parity::recording_server peer{parity::server_config_with(server_security)};
    auto client = parity::make_client(peer, parity::client_config_with(client_security));
    const bool answered = parity::answered(client, std::chrono::seconds{on_server ? 5 : 10});
    return {answered, peer.handler_ran->load()};
}

}  // namespace

// Requirement 2.1: an accepting validator lets the RPC through, on both ends,
// and is handed the peer's leaf certificate.
BOOST_AUTO_TEST_CASE(accepting_validator_lets_the_rpc_through,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    for (const bool on_server : {true, false}) {
        auto seen = std::make_shared<std::string>();
        const auto [answered, ran] = validator_round_trip(
            [seen](const std::string& pem) {
                *seen = pem;
                return true;
            },
            on_server);
        BOOST_TEST(answered);
        BOOST_TEST(ran);
        BOOST_TEST(seen->find("BEGIN CERTIFICATE") != std::string::npos);
    }
}

// Requirement 2.2: a refusing validator fails the handshake on both ends.
BOOST_AUTO_TEST_CASE(refusing_validator_fails_the_handshake,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    for (const bool on_server : {true, false}) {
        const auto [answered, ran] =
            validator_round_trip([](const std::string&) { return false; }, on_server);
        BOOST_TEST(!answered);
        BOOST_TEST(!ran, "no request may cross a session the validator refused");
    }
}

// Requirement 2.2: a validator that throws refuses, rather than escaping
// into libnyoci's C code.
BOOST_AUTO_TEST_CASE(throwing_validator_fails_the_handshake,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    for (const bool on_server : {true, false}) {
        const auto [answered, ran] = validator_round_trip(
            [](const std::string&) -> bool { throw std::runtime_error("validator exploded"); },
            on_server);
        BOOST_TEST(!answered);
        BOOST_TEST(!ran);
    }
}

BOOST_AUTO_TEST_SUITE_END()

#else  // LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_libnyoci_security_parity_skipped_without_libnyoci) {
    BOOST_TEST_MESSAGE("libnyoci not available; the libnyoci security parity tests are skipped.");
}

#endif  // LIBNYOCI_AVAILABLE
