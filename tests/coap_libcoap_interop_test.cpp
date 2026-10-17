// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_libcoap_interop_test.cpp
/// @brief A libcoap CoAP client against a coap_interop_peer server on every
///        backend that was built (coap_interop_cases.hpp).

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libcoap_interop_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>

#ifdef LIBCOAP_AVAILABLE

namespace coap_interop {

template<typename Types> using backend_client = kythira::coap_client<Types>;

[[nodiscard]] inline auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    return config;
}

}  // namespace coap_interop

#include "coap_interop_cases.hpp"

#else  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_libcoap_interop_skipped_without_libcoap) {
    BOOST_TEST_MESSAGE("libcoap not available; the libcoap interop tests are skipped.");
}

#endif  // LIBCOAP_AVAILABLE
