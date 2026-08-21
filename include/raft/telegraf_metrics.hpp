// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file telegraf_metrics.hpp
/// @brief Telegraf metrics backend: a `kythira::metrics`-conforming handle
///        emitting InfluxDB line protocol to a Telegraf agent's
///        `socket_listener` input over UDP (default) or TCP, so operators can
///        fan Kythira's metrics out to whatever Telegraf output plugin they
///        already run. See doc/telegraf_metrics_backend.md.
///
/// InfluxDB line protocol rather than StatsD, deliberately: line protocol
/// carries dimensions as first-class tags with no extension syntax, and
/// Telegraf's `socket_listener` parses it natively (`data_format =
/// "influx"`). Each emit() renders one line and enqueues it on the shared
/// `metrics_line_exporter` — no I/O on the recording path.
///
/// Copy semantics: same as prometheus_metrics.hpp — the transports copy the
/// handle per emission, so this is a cheap copyable handle over a shared
/// exporter; every copy feeds the same background sender.

#include <raft/metrics.hpp>
#include <raft/metrics_line_exporter.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

// ── Line-protocol escaping ───────────────────────────────────────────────────
// Measurement names escape ',' and ' '; tag keys, tag values, and field keys
// additionally escape '='. Newlines are protocol delimiters and cannot be
// escaped at all, so they are replaced. Only numeric field values are
// emitted, so string-field quoting never comes up.

[[nodiscard]] inline auto influx_escape_measurement(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == ',' || c == ' ') out.push_back('\\');
        if (c == '\n') {
            out.push_back('_');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] inline auto influx_escape_tag(std::string_view in) -> std::string {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == ',' || c == ' ' || c == '=') out.push_back('\\');
        if (c == '\n') {
            out.push_back('_');
            continue;
        }
        out.push_back(c);
    }
    return out;
}

// ── Configuration ────────────────────────────────────────────────────────────

enum class telegraf_protocol : std::uint8_t {
    udp,
    tcp
};

struct telegraf_metrics_config {
    std::string host;           ///< Telegraf agent host (compose service name or IP).
    std::uint16_t port = 8094;  ///< Telegraf socket_listener's customary port.
    telegraf_protocol protocol = telegraf_protocol::udp;
    metrics_line_exporter_config exporter{};
};

// ── The concept-conforming handle ────────────────────────────────────────────

class telegraf_metrics {
public:
    /// `sender` is an injectable test seam; when empty the real socket sender
    /// implied by `config.protocol` is built.
    explicit telegraf_metrics(telegraf_metrics_config config, line_sender_fn sender = {})
        : _exporter(std::make_shared<metrics_line_exporter>(
              config.exporter, sender ? std::move(sender)
                               : config.protocol == telegraf_protocol::udp
                                   ? udp_line_sender(config.host, config.port)
                                   : tcp_line_sender(config.host, config.port))) {}

    auto set_metric_name(std::string_view name) -> void {
        _name.assign(name);
        _dimensions.clear();
        _fields.clear();
        _count_is_integer = false;
    }

    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _dimensions.emplace_back(influx_escape_tag(dimension_name),
                                 influx_escape_tag(dimension_value));
    }

    auto add_one() -> void { add_count(1); }

    auto add_count(std::int64_t count) -> void {
        // Accumulate into the one count field rather than append, so
        // multiple calls before emit() yield one field (matching the other
        // backends' counter semantics) and a line can never carry the same
        // field key twice — invalid line protocol.
        for (auto& [field, value] : _fields) {
            if (field == "count") {
                value += static_cast<double>(count);
                return;
            }
        }
        _fields.emplace_back("count", static_cast<double>(count));
        _count_is_integer = true;
    }

    auto add_duration(std::chrono::nanoseconds duration) -> void {
        _fields.emplace_back("duration_ms", static_cast<double>(duration.count()) / 1e6);
    }

    auto add_value(double value) -> void { _fields.emplace_back("value", value); }

    auto emit() -> void {
        if (!_fields.empty() && !_name.empty()) {
            std::string line = influx_escape_measurement(_name);
            for (const auto& [tag_name, tag_value] : _dimensions) {
                line += "," + tag_name + "=" + tag_value;
            }
            line += " ";
            for (std::size_t i = 0; i < _fields.size(); ++i) {
                if (i > 0) line += ",";
                const auto& [field, value] = _fields[i];
                if (field == "count" && _count_is_integer) {
                    line += field + "=" + std::format("{}", static_cast<std::int64_t>(value)) + "i";
                } else {
                    line += field + "=" + std::format("{}", value);
                }
            }
            line += " " + std::format("{}", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::system_clock::now().time_since_epoch())
                                                .count());
            _exporter->push(std::move(line));
        }
        _dimensions.clear();
        _fields.clear();
        _count_is_integer = false;
    }

    [[nodiscard]] auto dropped_line_count() const -> std::uint64_t {
        return _exporter->dropped_line_count();
    }

private:
    std::shared_ptr<metrics_line_exporter> _exporter;
    std::string _name;
    std::vector<std::pair<std::string, std::string>> _dimensions;
    std::vector<std::pair<std::string, double>> _fields;
    bool _count_is_integer = false;
};

static_assert(metrics<telegraf_metrics>, "telegraf_metrics must satisfy metrics concept");

}  // namespace kythira
