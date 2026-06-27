#pragma once

/// @file metrics.hpp
/// @brief Metrics-collection concept and a zero-overhead no-op implementation.

#include <concepts>
#include <string_view>
#include <cstdint>
#include <chrono>

namespace kythira {

/// @brief Concept for a metrics-collection back-end.
///
/// Implementations may forward data to CloudWatch, Prometheus, StatsD, etc.
/// All methods must be non-blocking; defer I/O to a background emitter.
///
/// @tparam M Concrete metrics type.
template<typename M>
concept metrics = requires(M metric, std::string_view name, std::string_view dimension_name,
                           std::string_view dimension_value, std::int64_t count,
                           std::chrono::nanoseconds duration, double value) {
    /// Set the metric name; called once before any recording method.
    { metric.set_metric_name(name) } -> std::same_as<void>;
    /// Attach a named dimension (tag) to this metric.
    { metric.add_dimension(dimension_name, dimension_value) } -> std::same_as<void>;

    /// Increment the counter by one.
    { metric.add_one() } -> std::same_as<void>;
    /// Increment the counter by `count`.
    { metric.add_count(count) } -> std::same_as<void>;
    /// Record a latency sample.
    { metric.add_duration(duration) } -> std::same_as<void>;
    /// Record an arbitrary floating-point gauge value.
    { metric.add_value(value) } -> std::same_as<void>;

    /// Flush the current metric to the back-end.
    { metric.emit() } -> std::same_as<void>;
};

/// @brief Zero-cost no-op metrics implementation for testing and production environments
///        where metrics are unwanted.
///
/// All methods are inlined and compile away to nothing.
class noop_metrics {
public:
    auto set_metric_name([[maybe_unused]] std::string_view name) -> void {}
    auto add_dimension([[maybe_unused]] std::string_view dimension_name,
                       [[maybe_unused]] std::string_view dimension_value) -> void {}
    auto add_one() -> void {}
    auto add_count([[maybe_unused]] std::int64_t count) -> void {}
    auto add_duration([[maybe_unused]] std::chrono::nanoseconds duration) -> void {}
    auto add_value([[maybe_unused]] double value) -> void {}
    auto emit() -> void {}
};

static_assert(metrics<noop_metrics>, "noop_metrics must satisfy metrics concept");

}  // namespace kythira
