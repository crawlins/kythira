// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The Types bundle each CoAP and HTTP transport header instantiates its own
// client and server with, so that header can `static_assert` which transport
// concepts it satisfies (.kiro/specs/coap-transport-multi-raft/ Requirement 2,
// .kiro/specs/http-coap-pre-vote-timeout-now/ Requirement 2).
//
// It exists because a header cannot assert a concept against a class
// template, only against an instantiation, and none of these transports
// ships a concrete Types bundle of its own the way grpc_transport.hpp ships
// grpc_kythira_transport_types. Every transport asserts against this one, so
// a difference between their answers is a difference between the transports
// and never between the bundles they were asked about.
//
// Deliberately free of any C library header: libcoap's and libnyoci's cannot
// share a translation unit, and this file is included by both.

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kythira::transport_detail {

struct conformance_types {
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
    using serializer_registry_type = kythira::single_serializer_registry<serializer_type>;
    using rpc_serializer_type = serializer_type;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    // `transport_types` names the executor and never invokes it, and none of
    // the three backends does either, so any complete type serves. A
    // non-executor keeps every CoAP header off Folly's executor headers.
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

static_assert(kythira::transport_types<conformance_types>,
              "the conformance bundle must itself model transport_types");

}  // namespace kythira::transport_detail

namespace kythira::coap_detail {

// The name the CoAP backends have always asserted against.
using conformance_types = kythira::transport_detail::conformance_types;

}  // namespace kythira::coap_detail
