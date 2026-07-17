#pragma once

/// @file otlp_metrics.hpp
/// @brief `otlp_metrics`: a `kythira::metrics` implementation that emits OTLP
///        Sum/Histogram/Gauge data points over OTLP/HTTP JSON. See
///        .kiro/specs/otlp-telemetry-backend/ (Requirement 1).

#include <raft/metrics.hpp>
#include <raft/otlp_exporter.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira {

enum class otlp_metric_shape : std::uint8_t {
    none,
    sum_delta,
    histogram,
    gauge
};

/// Finalized shape of one `set_metric_name`...`emit()` call. Carries its own
/// copy of the histogram bounds it was bucketed against (rather than a
/// reference back into `otlp_metrics`) so `otlp_metric_record` has no
/// lifetime dependency on the object that produced it — it is handed to the
/// background exporter thread, which outlives any single call site.
struct otlp_metric_record {
    std::string name;
    std::vector<std::pair<std::string, std::string>> dimensions;
    otlp_metric_shape shape = otlp_metric_shape::none;

    std::int64_t sum_count = 0;  // shape == sum_delta

    double histogram_sum_ms = 0.0;                       // shape == histogram
    std::vector<double> histogram_bounds_ms;             // shape == histogram
    std::vector<std::uint64_t> histogram_bucket_counts;  // shape == histogram

    double gauge_value = 0.0;  // shape == gauge

    std::uint64_t start_time_unix_nano = 0;  // sum_delta / histogram only
    std::uint64_t time_unix_nano = 0;
};

namespace otlp_metrics_detail {

[[nodiscard]] inline auto encode_batch(const otlp_resource& resource,
                                       std::span<const otlp_metric_record> records)
    -> boost::json::object {
    boost::json::array metrics;
    metrics.reserve(records.size());

    for (const auto& record : records) {
        if (record.shape == otlp_metric_shape::none) continue;

        boost::json::object data_point{{"attributes", otlp_attributes_array(record.dimensions)},
                                       {"timeUnixNano", std::to_string(record.time_unix_nano)}};

        boost::json::object metric{{"name", record.name}};

        switch (record.shape) {
            case otlp_metric_shape::sum_delta:
                data_point["startTimeUnixNano"] = std::to_string(record.start_time_unix_nano);
                data_point["asInt"] = std::to_string(record.sum_count);
                metric["sum"] =
                    boost::json::object{{"aggregationTemporality", "AGGREGATION_TEMPORALITY_DELTA"},
                                        {"isMonotonic", true},
                                        {"dataPoints", boost::json::array{data_point}}};
                break;

            case otlp_metric_shape::histogram: {
                data_point["startTimeUnixNano"] = std::to_string(record.start_time_unix_nano);
                data_point["count"] = std::to_string(1);
                data_point["sum"] = record.histogram_sum_ms;

                boost::json::array bounds;
                bounds.reserve(record.histogram_bounds_ms.size());
                for (double bound : record.histogram_bounds_ms) bounds.push_back(bound);
                data_point["explicitBounds"] = bounds;

                boost::json::array counts;
                counts.reserve(record.histogram_bucket_counts.size());
                for (auto count : record.histogram_bucket_counts)
                    counts.push_back(std::to_string(count));
                data_point["bucketCounts"] = counts;

                metric["histogram"] =
                    boost::json::object{{"aggregationTemporality", "AGGREGATION_TEMPORALITY_DELTA"},
                                        {"dataPoints", boost::json::array{data_point}}};
                break;
            }

            case otlp_metric_shape::gauge:
                data_point["asDouble"] = record.gauge_value;
                metric["gauge"] =
                    boost::json::object{{"dataPoints", boost::json::array{data_point}}};
                break;

            case otlp_metric_shape::none:
                continue;
        }

        metrics.push_back(std::move(metric));
    }

    boost::json::object scope_metrics{{"metrics", metrics}};
    boost::json::object resource_metrics{{"resource", resource.to_json()},
                                         {"scopeMetrics", boost::json::array{scope_metrics}}};
    return boost::json::object{{"resourceMetrics", boost::json::array{resource_metrics}}};
}

}  // namespace otlp_metrics_detail

/// @brief `kythira::metrics` implementation emitting OTLP/HTTP JSON.
///
/// Non-blocking throughout (Requirement 1.1): `emit()` only finalizes the
/// pending record and hands it to the shared `otlp_http_batch_exporter`,
/// which owns all network I/O on its own background thread. Owns its own
/// mutex (Requirement 1.5) rather than relying on a single-threaded caller.
class otlp_metrics {
public:
    // `poster` defaults to the real cpp-httplib-backed implementation;
    // Requirement 3.7's injectable seam is exposed here (not just on
    // otlp_http_batch_exporter itself) so unit tests can substitute a stub
    // without any real network I/O — see tests/otlp_metrics_test.cpp.
    explicit otlp_metrics(otlp_export_config config, otlp_resource resource,
                          http_poster_fn poster = real_http_poster())
        : _histogram_bounds(config.histogram_bounds_ms),
          _construction_time(otlp_now_unix_nanos()),
          _exporter(std::move(config), std::move(resource), "/v1/metrics",
                    &otlp_metrics_detail::encode_batch, std::move(poster)) {}

    otlp_metrics(otlp_metrics&&) noexcept = default;
    auto operator=(otlp_metrics&&) noexcept -> otlp_metrics& = default;
    otlp_metrics(const otlp_metrics&) = delete;
    auto operator=(const otlp_metrics&) -> otlp_metrics& = delete;

    auto set_metric_name(std::string_view name) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _pending = otlp_metric_record{};
        _pending.name = std::string(name);
    }

    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _pending.dimensions.emplace_back(dimension_name, dimension_value);
    }

    auto add_one() -> void { add_count(1); }

    auto add_count(std::int64_t count) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _pending.shape = otlp_metric_shape::sum_delta;
        _pending.sum_count = count;
    }

    // Requirement 1.4: bucket placement against the (possibly overridden)
    // configured boundaries; the record carries its own copy of the bounds
    // it was placed against.
    auto add_duration(std::chrono::nanoseconds duration) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _pending.shape = otlp_metric_shape::histogram;
        const double duration_ms = std::chrono::duration<double, std::milli>(duration).count();
        _pending.histogram_sum_ms = duration_ms;
        _pending.histogram_bounds_ms = _histogram_bounds;

        _pending.histogram_bucket_counts.assign(_histogram_bounds.size() + 1, 0);
        std::size_t bucket = _histogram_bounds.size();  // overflow bucket by default
        for (std::size_t i = 0; i < _histogram_bounds.size(); ++i) {
            if (duration_ms <= _histogram_bounds[i]) {
                bucket = i;
                break;
            }
        }
        _pending.histogram_bucket_counts[bucket] = 1;
    }

    auto add_value(double value) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _pending.shape = otlp_metric_shape::gauge;
        _pending.gauge_value = value;
    }

    // Requirement 1.2/1.3: finalizes the pending record (a no-op, nothing
    // pushed, if no recording method was called since set_metric_name) and
    // tracks each series' delta-temporality start time.
    auto emit() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_pending.shape == otlp_metric_shape::none) return;

        const auto now = otlp_now_unix_nanos();
        _pending.time_unix_nano = now;

        if (_pending.shape != otlp_metric_shape::gauge) {
            auto key = otlp_make_series_key(_pending.name, _pending.dimensions);
            auto it = _series_start_time.try_emplace(key, _construction_time).first;
            _pending.start_time_unix_nano = it->second;
            it->second = now;
        }

        _exporter.push(std::move(_pending));
        _pending = otlp_metric_record{};
    }

    [[nodiscard]] auto dropped_record_count() const -> std::uint64_t {
        return _exporter.dropped_record_count();
    }

private:
    std::mutex _mutex;
    otlp_metric_record _pending;
    std::vector<double> _histogram_bounds;
    std::uint64_t _construction_time;
    std::unordered_map<otlp_series_key, std::uint64_t> _series_start_time;
    otlp_http_batch_exporter<otlp_metric_record> _exporter;
};

static_assert(metrics<otlp_metrics>, "otlp_metrics must satisfy metrics concept");

}  // namespace kythira
