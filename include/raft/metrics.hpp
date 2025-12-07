#pragma once

#include <concepts>
#include <string_view>
#include <cstdint>
#include <chrono>

namespace raft {

// Metrics concept for collecting and reporting performance metrics
template<typename M>
concept metrics = requires(
    M metric,
    std::string_view name,
    std::string_view dimension_name,
    std::string_view dimension_value,
    std::int64_t count,
    std::chrono::nanoseconds duration,
    double value
) {
    // Metric configuration
    { metric.set_metric_name(name) } -> std::same_as<void>;
    { metric.add_dimension(dimension_name, dimension_value) } -> std::same_as<void>;
    
    // Recording methods
    { metric.add_one() } -> std::same_as<void>;
    { metric.add_count(count) } -> std::same_as<void>;
    { metric.add_duration(duration) } -> std::same_as<void>;
    { metric.add_value(value) } -> std::same_as<void>;
    
    // Metric emission
    { metric.emit() } -> std::same_as<void>;
};

// No-op metrics implementation for testing without metrics overhead
// All operations are inlined and do nothing, resulting in zero runtime cost
class noop_metrics {
public:
    // Metric configuration - no-op
    auto set_metric_name([[maybe_unused]] std::string_view name) -> void {}
    
    auto add_dimension(
        [[maybe_unused]] std::string_view dimension_name,
        [[maybe_unused]] std::string_view dimension_value
    ) -> void {}
    
    // Recording methods - no-op
    auto add_one() -> void {}
    
    auto add_count([[maybe_unused]] std::int64_t count) -> void {}
    
    auto add_duration([[maybe_unused]] std::chrono::nanoseconds duration) -> void {}
    
    auto add_value([[maybe_unused]] double value) -> void {}
    
    // Metric emission - no-op
    auto emit() -> void {}
};

// Verify that noop_metrics satisfies the metrics concept
static_assert(metrics<noop_metrics>, "noop_metrics must satisfy metrics concept");

} // namespace raft
