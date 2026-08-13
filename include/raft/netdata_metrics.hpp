#pragma once

/// @file netdata_metrics.hpp
/// @brief NetData metrics backend: a `kythira::metrics`-conforming handle
///        emitting StatsD datagrams to NetData's built-in StatsD collector
///        (listening on UDP 8125 by default), for operators already running
///        NetData for host-level monitoring who want Kythira's Raft/RPC
///        metrics on the same dashboard. See doc/netdata_metrics_backend.md.
///
/// Wire mapping: add_one/add_count → `name:<n>|c`, add_value → `name:<v>|g`,
/// add_duration → `name:<ms>|ms`. Dimensions are appended as DataDog-style
/// tags (`|#k:v,k:v`), which NetData's StatsD collector accepts on the wire —
/// but be aware NetData aggregates a metric into one chart per NAME; tags are
/// exposed as chart labels, not separate time series, and older NetData
/// versions drop them entirely when re-exporting (netdata/netdata#3813).
/// Operators who need per-dimension series should prefer the Prometheus or
/// Telegraf backends; this one exists for the same-dashboard convenience the
/// TODO entry describes. Tags can be disabled outright with
/// `netdata_metrics_config::include_tags = false`.
///
/// Copy semantics: same as prometheus_metrics.hpp/telegraf_metrics.hpp — a
/// cheap copyable handle over a shared `metrics_line_exporter`, because the
/// transports copy the handle per emission.

#include <raft/metrics.hpp>
#include <raft/metrics_line_exporter.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

// ── StatsD sanitization ──────────────────────────────────────────────────────
// ':' separates name from value, '|' separates protocol segments, '\n'
// separates lines in a multi-metric datagram; none is escapable, so all are
// replaced. For tags, ',' additionally separates tag pairs (the first ':' in
// a tag separates key from value, so ':' is only replaced in tag KEYS).

[[nodiscard]] inline auto statsd_sanitize_name(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back((c == ':' || c == '|' || c == '\n' || c == ' ') ? '_' : c);
    }
    return out;
}

[[nodiscard]] inline auto statsd_sanitize_tag_key(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back((c == ':' || c == '|' || c == '\n' || c == ',') ? '_' : c);
    }
    return out;
}

[[nodiscard]] inline auto statsd_sanitize_tag_value(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back((c == '|' || c == '\n' || c == ',') ? '_' : c);
    }
    return out;
}

// ── Configuration ────────────────────────────────────────────────────────────

struct netdata_metrics_config {
    std::string host;           ///< NetData host (compose service name or IP).
    std::uint16_t port = 8125;  ///< NetData's StatsD collector default port.
    /// Append dimensions as DataDog-style tags. On by default — NetData
    /// accepts them and current versions surface them as chart labels; see
    /// the header comment for the aggregation caveat.
    bool include_tags = true;
    metrics_line_exporter_config exporter{};
};

// ── The concept-conforming handle ────────────────────────────────────────────

class netdata_metrics {
public:
    /// `sender` is an injectable test seam; when empty a real UDP sender is
    /// built (NetData's StatsD collector is UDP-first; its TCP listener
    /// exists but UDP is the documented default).
    explicit netdata_metrics(netdata_metrics_config config, line_sender_fn sender = {})
        : _include_tags(config.include_tags),
          _exporter(std::make_shared<metrics_line_exporter>(
              config.exporter,
              sender ? std::move(sender) : udp_line_sender(config.host, config.port))) {}

    auto set_metric_name(std::string_view name) -> void {
        _name = statsd_sanitize_name(name);
        _dimensions.clear();
        _lines.clear();
    }

    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _dimensions.emplace_back(statsd_sanitize_tag_key(dimension_name),
                                 statsd_sanitize_tag_value(dimension_value));
    }

    auto add_one() -> void { add_count(1); }

    auto add_count(std::int64_t count) -> void { _lines.push_back(std::format("{}|c", count)); }

    auto add_duration(std::chrono::nanoseconds duration) -> void {
        _lines.push_back(std::format("{}|ms", static_cast<double>(duration.count()) / 1e6));
    }

    auto add_value(double value) -> void { _lines.push_back(std::format("{}|g", value)); }

    auto emit() -> void {
        if (!_name.empty()) {
            std::string tags;
            if (_include_tags && !_dimensions.empty()) {
                tags = "|#";
                for (std::size_t i = 0; i < _dimensions.size(); ++i) {
                    if (i > 0) tags += ",";
                    tags += _dimensions[i].first + ":" + _dimensions[i].second;
                }
            }
            for (auto& value_and_type : _lines) {
                _exporter->push(_name + ":" + value_and_type + tags);
            }
        }
        _dimensions.clear();
        _lines.clear();
    }

    [[nodiscard]] auto dropped_line_count() const -> std::uint64_t {
        return _exporter->dropped_line_count();
    }

private:
    bool _include_tags;
    std::shared_ptr<metrics_line_exporter> _exporter;
    std::string _name;
    std::vector<std::pair<std::string, std::string>> _dimensions;
    /// Rendered `<value>|<type>` fragments staged until emit().
    std::vector<std::string> _lines;
};

static_assert(metrics<netdata_metrics>, "netdata_metrics must satisfy metrics concept");

}  // namespace kythira
