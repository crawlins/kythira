#pragma once

/// @file prometheus_metrics.hpp
/// @brief Prometheus metrics backend: a `kythira::metrics`-conforming handle
///        that accumulates into a shared registry, a text-exposition-format
///        renderer, and an HTTP scrape server exposing `/metrics`.
///        See doc/TODO.md "Metrics Backends" for the requirements and
///        doc/prometheus_metrics_backend.md for operator documentation.
///
/// Prometheus is pull-based, so this backend does no network I/O on the
/// recording path at all: `emit()` takes one mutex and updates in-memory
/// aggregate state; the Prometheus server reads it back out through
/// `prometheus_scrape_server` on its own schedule. That makes the
/// non-blocking requirement (metrics.hpp) trivially true — there is no
/// background emitter to defer to because there is nothing to send.
///
/// Copy semantics: the transports copy the metrics handle per emission
/// (`auto metric = _metrics;` — see include/raft/http_transport_impl.hpp),
/// so recording state cannot live in the handle. `prometheus_metrics` is a
/// cheap copyable handle over a `std::shared_ptr<prometheus_registry>`;
/// every copy writes to the same registry (the same idiom
/// tests/recording_metrics.hpp documents). This is deliberately NOT the
/// move-only shape `otlp_metrics` uses — that one is unusable with the
/// HTTP transports for exactly this reason.

#include <raft/metrics.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

// ── Text-format sanitization ─────────────────────────────────────────────────
// Metric names must match [a-zA-Z_:][a-zA-Z0-9_:]*, label names
// [a-zA-Z_][a-zA-Z0-9_]*. Call sites in this project use dotted names
// ("http.client.request.sent"), which map to the Prometheus convention by
// replacing every invalid character with '_'.

[[nodiscard]] inline auto prometheus_sanitize_name(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == ':';
        out.push_back(valid ? c : '_');
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) out.insert(out.begin(), '_');
    return out;
}

[[nodiscard]] inline auto prometheus_sanitize_label_name(std::string_view name) -> std::string {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const bool valid =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        out.push_back(valid ? c : '_');
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) out.insert(out.begin(), '_');
    return out;
}

/// Label values may contain anything; the text format requires escaping
/// backslash, double-quote, and newline.
[[nodiscard]] inline auto prometheus_escape_label_value(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

// ── Configuration ────────────────────────────────────────────────────────────

struct prometheus_metrics_config {
    /// Histogram bucket upper bounds for `add_duration` samples, in
    /// milliseconds. Same default spread as otlp_export_config — covering
    /// sub-millisecond to multi-second Raft RPC/consensus latencies.
    std::vector<double> histogram_bounds_ms{1,   2,   5,    10,   25,   50,   100,
                                            250, 500, 1000, 2500, 5000, 10000};
};

// ── Registry (the shared sink) ───────────────────────────────────────────────

/// Aggregate state for every series observed since construction. Thread-safe;
/// each observation takes one mutex, does arithmetic, and returns — no I/O.
///
/// Shape rule: a metric name must be used with exactly one recording shape
/// (counter via add_one/add_count, gauge via add_value, histogram via
/// add_duration). The same family name appearing under two shapes would
/// render two conflicting TYPE lines, which Prometheus rejects at scrape
/// time; this project's call sites are shape-consistent per name.
class prometheus_registry {
public:
    explicit prometheus_registry(prometheus_metrics_config config = {})
        : _config(std::move(config)) {
        if (_config.histogram_bounds_ms.empty()) {
            throw std::invalid_argument(
                "prometheus_registry: histogram_bounds_ms must not be empty");
        }
        if (!std::ranges::is_sorted(_config.histogram_bounds_ms)) {
            throw std::invalid_argument(
                "prometheus_registry: histogram_bounds_ms must be sorted ascending");
        }
    }

    auto observe_counter(std::string_view name,
                         const std::vector<std::pair<std::string, std::string>>& dimensions,
                         double delta) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _counters[prometheus_sanitize_name(name)][render_labels(dimensions)] += delta;
    }

    auto observe_gauge(std::string_view name,
                       const std::vector<std::pair<std::string, std::string>>& dimensions,
                       double value) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _gauges[prometheus_sanitize_name(name)][render_labels(dimensions)] = value;
    }

    auto observe_duration(std::string_view name,
                          const std::vector<std::pair<std::string, std::string>>& dimensions,
                          double milliseconds) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& hist = _histograms[prometheus_sanitize_name(name)][render_labels(dimensions)];
        if (hist.bucket_counts.empty()) {
            hist.bucket_counts.assign(_config.histogram_bounds_ms.size(), 0);
        }
        for (std::size_t i = 0; i < _config.histogram_bounds_ms.size(); ++i) {
            if (milliseconds <= _config.histogram_bounds_ms[i]) {
                ++hist.bucket_counts[i];
                break;
            }
        }
        hist.sum += milliseconds;
        ++hist.count;
    }

    /// Render the whole registry in Prometheus text exposition format
    /// (version 0.0.4). Counter families get the idiomatic `_total` suffix
    /// appended unless the recorded name already carries it.
    [[nodiscard]] auto render_text() const -> std::string {
        std::lock_guard<std::mutex> lock(_mutex);
        std::string out;

        for (const auto& [family, series] : _counters) {
            auto total_name = family.ends_with("_total") ? family : family + "_total";
            out += "# TYPE " + total_name + " counter\n";
            for (const auto& [labels, value] : series) {
                out += total_name + labels + " " + std::format("{}", value) + "\n";
            }
        }

        for (const auto& [family, series] : _gauges) {
            out += "# TYPE " + family + " gauge\n";
            for (const auto& [labels, value] : series) {
                out += family + labels + " " + std::format("{}", value) + "\n";
            }
        }

        for (const auto& [family, series] : _histograms) {
            out += "# TYPE " + family + " histogram\n";
            for (const auto& [labels, hist] : series) {
                std::uint64_t cumulative = 0;
                for (std::size_t i = 0; i < _config.histogram_bounds_ms.size(); ++i) {
                    cumulative += hist.bucket_counts[i];
                    out += family + "_bucket" +
                           merge_label(labels, "le",
                                       std::format("{}", _config.histogram_bounds_ms[i])) +
                           " " + std::format("{}", cumulative) + "\n";
                }
                out += family + "_bucket" + merge_label(labels, "le", "+Inf") + " " +
                       std::format("{}", hist.count) + "\n";
                out += family + "_sum" + labels + " " + std::format("{}", hist.sum) + "\n";
                out += family + "_count" + labels + " " + std::format("{}", hist.count) + "\n";
            }
        }
        return out;
    }

private:
    struct histogram_state {
        std::vector<std::uint64_t> bucket_counts;  ///< Per-bound (non-cumulative) counts.
        double sum = 0.0;
        std::uint64_t count = 0;
    };

    /// Render dimensions as a sorted `{k="v",...}` block ("" when empty).
    /// Sorted so the same dimension set always produces the same series key
    /// regardless of add_dimension() call order.
    [[nodiscard]] static auto render_labels(
        const std::vector<std::pair<std::string, std::string>>& dimensions) -> std::string {
        if (dimensions.empty()) return "";
        auto sorted = dimensions;
        std::ranges::sort(sorted);
        std::string out = "{";
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) out += ",";
            out += prometheus_sanitize_label_name(sorted[i].first) + "=\"" +
                   prometheus_escape_label_value(sorted[i].second) + "\"";
        }
        out += "}";
        return out;
    }

    /// Insert one more label into an already-rendered label block (for
    /// histogram `le`). `le` sorts into place lexically only by accident, so
    /// it is appended last — Prometheus does not require label ordering.
    [[nodiscard]] static auto merge_label(const std::string& labels, std::string_view name,
                                          std::string_view value) -> std::string {
        std::string pair = std::string(name) + "=\"" + std::string(value) + "\"";
        if (labels.empty()) return "{" + pair + "}";
        std::string out = labels;
        out.insert(out.size() - 1, "," + pair);
        return out;
    }

    prometheus_metrics_config _config;
    mutable std::mutex _mutex;
    // family -> rendered-label-block -> state. std::map for deterministic
    // exposition order, which the unit tests assert against directly.
    std::map<std::string, std::map<std::string, double>> _counters;
    std::map<std::string, std::map<std::string, double>> _gauges;
    std::map<std::string, std::map<std::string, histogram_state>> _histograms;
};

// ── The concept-conforming handle ────────────────────────────────────────────

/// Cheap copyable handle; all copies share one `prometheus_registry`.
/// Staging state (name/dimensions/pending sample) lives in the handle and is
/// reset by `emit()`, matching the set_metric_name → add_dimension* →
/// record → emit() protocol every call site follows.
class prometheus_metrics {
public:
    explicit prometheus_metrics(std::shared_ptr<prometheus_registry> registry)
        : _registry(std::move(registry)) {
        if (!_registry) {
            throw std::invalid_argument("prometheus_metrics: registry must not be null");
        }
    }

    auto set_metric_name(std::string_view name) -> void {
        _name.assign(name);
        _dimensions.clear();
        _shape = shape::none;
        _count = 0.0;
        _value = 0.0;
        _duration_ms = 0.0;
    }

    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _dimensions.emplace_back(std::string(dimension_name), std::string(dimension_value));
    }

    auto add_one() -> void {
        _shape = shape::counter;
        _count += 1.0;
    }

    auto add_count(std::int64_t count) -> void {
        _shape = shape::counter;
        _count += static_cast<double>(count);
    }

    auto add_duration(std::chrono::nanoseconds duration) -> void {
        _shape = shape::histogram;
        _duration_ms = static_cast<double>(duration.count()) / 1e6;
    }

    auto add_value(double value) -> void {
        _shape = shape::gauge;
        _value = value;
    }

    auto emit() -> void {
        switch (_shape) {
            case shape::none:
                break;  // emit() without a recording call is a no-op.
            case shape::counter:
                _registry->observe_counter(_name, _dimensions, _count);
                break;
            case shape::gauge:
                _registry->observe_gauge(_name, _dimensions, _value);
                break;
            case shape::histogram:
                _registry->observe_duration(_name, _dimensions, _duration_ms);
                break;
        }
        // Reset staging (keep the name; the protocol starts every record
        // with set_metric_name anyway). Not set_metric_name(_name) — that
        // would self-assign _name through a view of its own buffer.
        _dimensions.clear();
        _shape = shape::none;
        _count = 0.0;
        _value = 0.0;
        _duration_ms = 0.0;
    }

    /// The shared registry, for wiring a scrape server to the same sink.
    [[nodiscard]] auto registry() const -> const std::shared_ptr<prometheus_registry>& {
        return _registry;
    }

private:
    enum class shape : std::uint8_t {
        none,
        counter,
        gauge,
        histogram
    };

    std::shared_ptr<prometheus_registry> _registry;
    std::string _name;
    std::vector<std::pair<std::string, std::string>> _dimensions;
    shape _shape = shape::none;
    double _count = 0.0;
    double _value = 0.0;
    double _duration_ms = 0.0;
};

static_assert(metrics<prometheus_metrics>, "prometheus_metrics must satisfy metrics concept");

// ── Scrape server ────────────────────────────────────────────────────────────

/// Minimal HTTP server exposing `GET /metrics` (text exposition format) from
/// a shared registry, on its own thread — the same httplib::Server-plus-
/// std::thread shape as cmd/chaos_node/http_control.hpp. Pass port 0 to bind
/// an ephemeral port (tests); `port()` reports the actual one.
class prometheus_scrape_server {
public:
    prometheus_scrape_server(std::shared_ptr<prometheus_registry> registry,
                             std::string bind_address, std::uint16_t port)
        : _registry(std::move(registry)), _bind_address(std::move(bind_address)), _port(port) {
        if (!_registry) {
            throw std::invalid_argument("prometheus_scrape_server: registry must not be null");
        }
        _server.Get("/metrics", [reg = _registry](const httplib::Request&, httplib::Response& res) {
            res.set_content(reg->render_text(), "text/plain; version=0.0.4; charset=utf-8");
        });
        if (_port == 0) {
            // Bind on the constructing thread so port() is correct as soon as
            // the constructor returns; only the accept loop runs on _thread.
            int bound = _server.bind_to_any_port(_bind_address);
            if (bound <= 0) {
                throw std::runtime_error("prometheus_scrape_server: failed to bind " +
                                         _bind_address);
            }
            _port = static_cast<std::uint16_t>(bound);
            _thread = std::thread([this] { _server.listen_after_bind(); });
        } else {
            if (!_server.bind_to_port(_bind_address, _port)) {
                throw std::runtime_error("prometheus_scrape_server: failed to bind " +
                                         _bind_address + ":" + std::to_string(_port));
            }
            _thread = std::thread([this] { _server.listen_after_bind(); });
        }
    }

    ~prometheus_scrape_server() { stop(); }

    prometheus_scrape_server(const prometheus_scrape_server&) = delete;
    auto operator=(const prometheus_scrape_server&) -> prometheus_scrape_server& = delete;
    prometheus_scrape_server(prometheus_scrape_server&&) = delete;
    auto operator=(prometheus_scrape_server&&) -> prometheus_scrape_server& = delete;

    auto stop() -> void {
        _server.stop();
        if (_thread.joinable()) _thread.join();
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

private:
    std::shared_ptr<prometheus_registry> _registry;
    std::string _bind_address;
    std::uint16_t _port;
    httplib::Server _server;
    std::thread _thread;
};

}  // namespace kythira
