// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_libnyoci_interop_test.cpp
/// @brief A libnyoci CoAP client against a coap_interop_peer server on every
///        backend that was built (coap_interop_cases.hpp).

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_interop_test
#include <boost/test/unit_test.hpp>

// Deliberately NOT raft/coap_transport.hpp: libcoap's and libnyoci's headers
// cannot share a translation unit (see coap_transport_libnyoci_impl.hpp).
#include <raft/coap_transport_libnyoci_impl.hpp>

#ifdef LIBNYOCI_AVAILABLE

namespace coap_interop {

template<typename Types> using backend_client = kythira::coap_libnyoci_client<Types>;

[[nodiscard]] inline auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    return config;
}

}  // namespace coap_interop

// This backend carries EDHOC over /.well-known/edhoc, so it can run the
// EDHOC cases against the other backend that does.
#define KYTHIRA_INTEROP_CLIENT_EDHOC_OVER_COAP
#include "coap_interop_cases.hpp"

#else  // LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_libnyoci_interop_skipped_without_libnyoci) {
    BOOST_TEST_MESSAGE("libnyoci not available; the libnyoci interop tests are skipped.");
}

#endif  // LIBNYOCI_AVAILABLE
