// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_node_cluster.hpp
/// @brief A cluster of `node<Types>` over the libcoap backend
///        (.kiro/specs/coap-transport-multi-raft/ task 5 onward).
///
/// The cluster itself is in coap_node_cluster_base.hpp, which names no
/// backend; this header binds it to libcoap's `coap_client`/`coap_server`.

#include "coap_node_cluster_base.hpp"

#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>

namespace kythira::testing {

/// libcoap, the default CoAP backend.
struct libcoap_backend {
    template<typename Types> using client = kythira::coap_client<Types>;
    template<typename Types> using server = kythira::coap_server<Types>;
};

template<typename Serializer>
using coap_node_raft_types = basic_coap_node_raft_types<Serializer, libcoap_backend>;

template<typename Serializer = kythira::json_rpc_serializer<std::vector<std::byte>>>
using coap_node_cluster = basic_coap_node_cluster<libcoap_backend, Serializer>;

}  // namespace kythira::testing
