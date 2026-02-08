#pragma once

#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/logger.hpp>
#include <folly/futures/Future.h>
#include <folly/executors/InlineExecutor.h>
#include <string>
#include <cstdint>

namespace kythira {

// Test transport types template for use in CoAP transport tests
template<typename Serializer>
struct test_transport_types {
    template<typename T>
    using future_template = folly::Future<T>;
    
    using serializer_type = Serializer;
    using rpc_serializer_type = Serializer;
    using metrics_type = kythira::noop_metrics;
    using executor_type = folly::InlineExecutor;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
};

} // namespace kythira
