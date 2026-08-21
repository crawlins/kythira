// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/metrics.hpp>
#include <raft/logger.hpp>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/executors/InlineExecutor.h>
#include <string>
#include <cstdint>

namespace kythira {

// Test transport types template for use in CoAP transport tests
template<typename Serializer> struct test_transport_types {
    template<typename T> using future_template = folly::Future<T>;
    // Matches future_template's own folly::Future<T> - coap_transport_impl.hpp's
    // send_rpc() uses promise_template<Response> to build the promise/future pair
    // for both its LIBCOAP_AVAILABLE and stub code paths.
    template<typename T> using promise_template = folly::Promise<T>;

    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using rpc_serializer_type = Serializer;
    using metrics_type = kythira::noop_metrics;
    using executor_type = folly::InlineExecutor;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

}  // namespace kythira
