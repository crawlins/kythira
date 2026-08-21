// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file telegraf_logger.hpp
/// @brief `telegraf_logger`: a `kythira::diagnostic_logger` implementation
///        emitting log events as InfluxDB line-protocol records to the same
///        Telegraf `socket_listener` the Telegraf metrics backend uses —
///        the logging leg of that ecosystem (doc/TODO.md "Metrics
///        Backends", logging-alongside-backends item). See
///        doc/telegraf_metrics_backend.md's Logging section.
///
/// Rationale: Telegraf's value to this project is fan-out inheritance, and
/// that applies to logs the same way — a log event shipped as a line-
/// protocol record (measurement `kythira_log`, `level` tag, `msg` string
/// field, structured pairs as further string fields) reaches whatever
/// output the operator's Telegraf already routes to, with no new listener,
/// port, or agent config beyond the metrics leg's. Same shared
/// `metrics_line_exporter` transport: non-blocking, bounded, drop-oldest,
/// lossy by contract (`dropped_line_count()` makes loss observable).

#include <raft/logger.hpp>
#include <raft/metrics_line_exporter.hpp>
#include <raft/telegraf_metrics.hpp>  // influx escaping + telegraf_protocol.

#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

// ── Configuration ────────────────────────────────────────────────────────────

struct telegraf_logger_config {
    std::string host;           ///< Telegraf agent host (compose service name or IP).
    std::uint16_t port = 8094;  ///< Telegraf socket_listener's customary port.
    telegraf_protocol protocol = telegraf_protocol::udp;
    std::string measurement = "kythira_log";
    /// Constant tags on every record (e.g. {{"node_id","1"}}) — the
    /// identity leg, like the VictoriaMetrics backend's constant labels.
    std::vector<std::pair<std::string, std::string>> constant_tags;
    metrics_line_exporter_config exporter{};
};

namespace telegraf_logger_detail {

[[nodiscard]] inline auto level_text_for(log_level level) -> std::string_view {
    switch (level) {
        case log_level::trace:
            return "trace";
        case log_level::debug:
            return "debug";
        case log_level::info:
            return "info";
        case log_level::warning:
            return "warning";
        case log_level::error:
            return "error";
        case log_level::critical:
            return "critical";
    }
    return "unspecified";
}

/// Line-protocol string FIELD value escaping: backslash and double-quote
/// inside the quoted value; newlines are record delimiters and are
/// replaced.
[[nodiscard]] inline auto influx_escape_string_field(std::string_view value) -> std::string {
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
                out += ' ';
                break;
            default:
                out.push_back(c);
        }
    }
    return out;
}

}  // namespace telegraf_logger_detail

/// @brief `kythira::diagnostic_logger` implementation emitting line-protocol
///        log events. Same mutex/min-level/move conventions as the other
///        push loggers.
class telegraf_logger {
public:
    /// `sender` is the injectable test seam; when empty the real socket
    /// sender implied by `config.protocol` is built.
    explicit telegraf_logger(telegraf_logger_config config, log_level min_level = log_level::trace,
                             line_sender_fn sender = {})
        : _min_level(min_level),
          _measurement(influx_escape_measurement(config.measurement)),
          _exporter(std::make_unique<metrics_line_exporter>(
              config.exporter, sender ? std::move(sender)
                               : config.protocol == telegraf_protocol::udp
                                   ? udp_line_sender(config.host, config.port)
                                   : tcp_line_sender(config.host, config.port))) {
        for (const auto& [tag, value] : config.constant_tags) {
            _constant_tags += "," + influx_escape_tag(tag) + "=" + influx_escape_tag(value);
        }
    }

    telegraf_logger(telegraf_logger&& other) noexcept
        : _min_level(other._min_level),
          _measurement(std::move(other._measurement)),
          _constant_tags(std::move(other._constant_tags)),
          _exporter(std::move(other._exporter)) {}

    auto operator=(telegraf_logger&& other) noexcept -> telegraf_logger& {
        if (this != &other) {
            _min_level = other._min_level;
            _measurement = std::move(other._measurement);
            _constant_tags = std::move(other._constant_tags);
            _exporter = std::move(other._exporter);
        }
        return *this;
    }

    telegraf_logger(const telegraf_logger&) = delete;
    auto operator=(const telegraf_logger&) -> telegraf_logger& = delete;

    auto log(log_level level, std::string_view message) -> void { log(level, message, {}); }

    auto log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (level < _min_level) return;
        }
        // kythira_log,level=info[,const tags] msg="...",k="v"... <ts_ns>
        std::string line = _measurement +
                           ",level=" + std::string(telegraf_logger_detail::level_text_for(level)) +
                           _constant_tags + " msg=\"" +
                           telegraf_logger_detail::influx_escape_string_field(message) + "\"";
        for (const auto& [key, value] : key_value_pairs) {
            line += "," + influx_escape_tag(key) + "=\"" +
                    telegraf_logger_detail::influx_escape_string_field(value) + "\"";
        }
        line += " " + std::format("{}", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
        _exporter->push(std::move(line));
    }

    auto trace(std::string_view message) -> void { log(log_level::trace, message); }
    auto trace(std::string_view message,
               const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::trace, message, key_value_pairs);
    }

    auto debug(std::string_view message) -> void { log(log_level::debug, message); }
    auto debug(std::string_view message,
               const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::debug, message, key_value_pairs);
    }

    auto info(std::string_view message) -> void { log(log_level::info, message); }
    auto info(std::string_view message,
              const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::info, message, key_value_pairs);
    }

    auto warning(std::string_view message) -> void { log(log_level::warning, message); }
    auto warning(std::string_view message,
                 const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::warning, message, key_value_pairs);
    }

    auto error(std::string_view message) -> void { log(log_level::error, message); }
    auto error(std::string_view message,
               const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::error, message, key_value_pairs);
    }

    auto critical(std::string_view message) -> void { log(log_level::critical, message); }
    auto critical(std::string_view message,
                  const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        log(log_level::critical, message, key_value_pairs);
    }

    auto set_min_level(log_level level) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        _min_level = level;
    }

    [[nodiscard]] auto get_min_level() const -> log_level {
        std::lock_guard<std::mutex> lock(_mutex);
        return _min_level;
    }

    [[nodiscard]] auto dropped_line_count() const -> std::uint64_t {
        return _exporter ? _exporter->dropped_line_count() : 0;
    }

private:
    mutable std::mutex _mutex;
    log_level _min_level;
    std::string _measurement;
    std::string _constant_tags;  ///< Pre-rendered ",tag=value..." suffix ("" when none).
    std::unique_ptr<metrics_line_exporter> _exporter;
};

static_assert(diagnostic_logger<telegraf_logger>,
              "telegraf_logger must satisfy diagnostic_logger concept");

}  // namespace kythira
