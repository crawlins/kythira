// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, task 6.2 (CoAP, libnyoci backend)**
// coap_libnyoci_client and coap_libnyoci_server instantiated for std::string
// and aws_ec2_node_id carry every RPC kind through JSON and CBOR with the ids
// intact; an unknown textual target fails the call naming it; and, with
// OpenSSL, coap_peer_identity_validator binds a DTLS-PKI peer to the node id
// its certificate names. The cases live in
// coap_alternate_textual_node_id_cases.hpp, shared with the cantcoap suite.
// **Validates: Requirements 9.2**

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_libnyoci_textual_node_id_test
#include <boost/test/unit_test.hpp>

// Deliberately NOT raft/coap_transport.hpp: libcoap's and libnyoci's headers
// cannot share a translation unit (see coap_transport_libnyoci_impl.hpp).
#include <raft/coap_transport_libnyoci_impl.hpp>

#include <cstdint>
#include <string>

#ifdef LIBNYOCI_AVAILABLE

namespace textual_ids {

template<typename Types, typename NodeId>
using backend_client = kythira::coap_libnyoci_client<Types, NodeId>;
template<typename Types, typename NodeId>
using backend_server = kythira::coap_libnyoci_server<Types, NodeId>;

// The scheme is the backend's business: it dials DTLS when the config asks
// for it, whatever the endpoint string says.
[[nodiscard]] inline auto endpoint_for(std::uint16_t port) -> std::string {
    return "coap://127.0.0.1:" + std::to_string(port);
}

[[nodiscard]] inline auto client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    return config;
}

}  // namespace textual_ids

#include "coap_alternate_textual_node_id_cases.hpp"

#else  // LIBNYOCI_AVAILABLE

BOOST_AUTO_TEST_CASE(test_libnyoci_textual_node_ids_skipped_without_libnyoci) {
    BOOST_TEST_MESSAGE(
        "libnyoci not available; the libnyoci textual node id "
        "tests are skipped.");
}

#endif  // LIBNYOCI_AVAILABLE
