// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, task 6.2 (CoAP, cantcoap backend)**
// coap_cantcoap_client and coap_cantcoap_server instantiated for std::string
// and aws_ec2_node_id carry every RPC kind through JSON and CBOR with the ids
// intact; an unknown textual target fails the call naming it; and, with
// OpenSSL, coap_peer_identity_validator binds a DTLS-PKI peer to the node id
// its certificate names. The cases live in
// coap_alternate_textual_node_id_cases.hpp, shared with the libnyoci suite.
// **Validates: Requirements 9.2**

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_cantcoap_textual_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_transport_cantcoap_impl.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#ifdef CANTCOAP_AVAILABLE

namespace textual_ids {

template<typename Types, typename NodeId>
using backend_client = kythira::coap_cantcoap_client<Types, NodeId>;
template<typename Types, typename NodeId>
using backend_server = kythira::coap_cantcoap_server<Types, NodeId>;

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

}  // namespace textual_ids

#include "coap_alternate_textual_node_id_cases.hpp"

#else  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_cantcoap_textual_node_ids_skipped_without_cantcoap) {
    BOOST_TEST_MESSAGE("cantcoap not available; the cantcoap textual node id tests are skipped.");
}

#endif  // CANTCOAP_AVAILABLE
