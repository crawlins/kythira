// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// coap-alternate-backend-security-parity on the cantcoap backend: revocation
// (checked before cn_validator) and ACE-OAuth behave as they do on libcoap. The cases
// themselves live in coap_alternate_security_parity_cases.hpp, shared with the
// libnyoci suite so both alternates are held to the same ones.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_cantcoap_security_parity_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#define BOOST_TEST_TIMEOUT (300 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/coap_transport_cantcoap_impl.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#ifdef CANTCOAP_AVAILABLE

namespace parity {

struct test_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type = kythira::single_serializer_registry<serializer_type>;
    using rpc_serializer_type = serializer_type;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;  // named, never invoked; see coap_conformance_types.hpp

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using test_client = kythira::coap_cantcoap_client<test_types>;
using test_server = kythira::coap_cantcoap_server<test_types>;

// The scheme is the backend's business: it dials DTLS when the config asks
// for it, whatever the endpoint string says.
[[nodiscard]] inline auto endpoint_for(std::uint16_t port) -> std::string {
    return "coap://127.0.0.1:" + std::to_string(port);
}

[[nodiscard]] inline auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    config.ack_timeout = std::chrono::milliseconds{300};
    config.ack_random_factor_ms = std::chrono::milliseconds{50};
    config.max_retransmit = 3;
    return config;
}

}  // namespace parity

#include "coap_alternate_security_parity_cases.hpp"

#else  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_cantcoap_security_parity_skipped_without_cantcoap) {
    BOOST_TEST_MESSAGE("cantcoap not available; the cantcoap security parity tests are skipped.");
}

#endif  // CANTCOAP_AVAILABLE
