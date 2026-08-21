// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file loki_logger.hpp
/// @brief `loki_logger`: a `kythira::diagnostic_logger` implementation that
///        pushes log lines to Grafana Loki's `/loki/api/v1/push` API — the
///        logging leg of the Prometheus metrics backend's ecosystem
///        (doc/TODO.md "Metrics Backends", logging-alongside-backends item).
///        See doc/prometheus_metrics_backend.md's Logging section.
///
/// Reuses the OTLP backend's batching machinery wholesale: Loki's push API
/// is a single JSON document per POST, which is exactly the shape
/// `otlp_http_batch_exporter` was built for — same bounded queue, same
/// background thread, same retry/backoff, same injectable poster seam. The
/// constructor signature deliberately mirrors `otlp_logger`'s
/// (`otlp_export_config` + `otlp_resource` + min level + poster): the
/// resource's service_name/service_instance_id become Loki's `job`/
/// `instance` stream labels and extra_attributes become extra labels.
///
/// Wire mapping: entries group into one stream per severity, labelled
/// {job, instance, level, ...extra}; severity is a fine stream label
/// (six values — low cardinality). Structured key-value pairs are rendered
/// into the line itself in logfmt form (`msg="..." key="value" ...`) —
/// per-entry structured metadata is a Loki 3.x feature, and logfmt lines
/// parse in every Loki version and in LogQL's own `| logfmt` stage.

#include <raft/logger.hpp>
#include <raft/otlp_exporter.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// One log() call, rendered and timestamped at the call site; the encoder
/// only groups and serializes.
struct loki_log_record {
    std::uint64_t time_unix_nano = 0;
    std::string level_text;  ///< "trace" ... "critical" — becomes the `level` stream label.
    std::string line;        ///< Pre-rendered logfmt line.
};

namespace loki_logger_detail {

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

/// logfmt value escaping: backslash and double-quote, newline replaced (a
/// raw newline would split the entry into two Loki lines).
[[nodiscard]] inline auto logfmt_escape(std::string_view value) -> std::string {
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

[[nodiscard]] inline auto render_logfmt(
    std::string_view message,
    const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
    -> std::string {
    std::string line = "msg=\"" + logfmt_escape(message) + "\"";
    for (const auto& [key, value] : key_value_pairs) {
        line += " " + std::string(key) + "=\"" + logfmt_escape(value) + "\"";
    }
    return line;
}

/// Build the push body: one stream per severity present in the batch,
/// values sorted by timestamp within each stream (Loki accepts out-of-order
/// entries only on newer configs; sorted input works everywhere).
[[nodiscard]] inline auto encode_batch(const otlp_resource& resource,
                                       std::span<const loki_log_record> records)
    -> boost::json::object {
    std::map<std::string, std::vector<const loki_log_record*>> by_level;
    for (const auto& record : records) by_level[record.level_text].push_back(&record);

    boost::json::array streams;
    for (auto& [level, entries] : by_level) {
        std::ranges::sort(entries, {}, &loki_log_record::time_unix_nano);

        boost::json::object labels{{"job", resource.service_name},
                                   {"instance", resource.service_instance_id},
                                   {"level", level}};
        if (resource.service_namespace) labels["namespace"] = *resource.service_namespace;
        for (const auto& [key, value] : resource.extra_attributes) labels[key] = value;

        boost::json::array values;
        values.reserve(entries.size());
        for (const auto* record : entries) {
            values.push_back(
                boost::json::array{std::to_string(record->time_unix_nano), record->line});
        }
        streams.push_back(
            boost::json::object{{"stream", std::move(labels)}, {"values", std::move(values)}});
    }
    return boost::json::object{{"streams", std::move(streams)}};
}

}  // namespace loki_logger_detail

/// @brief `kythira::diagnostic_logger` implementation pushing to Loki.
///        Same mutex/min-level/move conventions as `otlp_logger`.
class loki_logger {
public:
    /// `config.endpoint_base_url` e.g. "http://loki:3100"; the push path is
    /// Loki's fixed `/loki/api/v1/push`.
    explicit loki_logger(otlp_export_config config, otlp_resource resource,
                         log_level min_level = log_level::trace,
                         http_poster_fn poster = real_http_poster())
        : _min_level(min_level),
          _exporter(std::move(config), std::move(resource), "/loki/api/v1/push",
                    &loki_logger_detail::encode_batch, std::move(poster)) {}

    // std::mutex has no move constructor; the moved-to object gets a fresh
    // one (otlp_logger's convention).
    loki_logger(loki_logger&& other) noexcept
        : _min_level(other._min_level), _exporter(std::move(other._exporter)) {}

    auto operator=(loki_logger&& other) noexcept -> loki_logger& {
        if (this != &other) {
            _min_level = other._min_level;
            _exporter = std::move(other._exporter);
        }
        return *this;
    }

    loki_logger(const loki_logger&) = delete;
    auto operator=(const loki_logger&) -> loki_logger& = delete;

    auto log(log_level level, std::string_view message) -> void { log(level, message, {}); }

    auto log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (level < _min_level) return;
        }
        loki_log_record record;
        record.time_unix_nano = otlp_now_unix_nanos();
        record.level_text = std::string(loki_logger_detail::level_text_for(level));
        record.line = loki_logger_detail::render_logfmt(message, key_value_pairs);
        _exporter.push(std::move(record));
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

    [[nodiscard]] auto dropped_record_count() const -> std::uint64_t {
        return _exporter.dropped_record_count();
    }

private:
    mutable std::mutex _mutex;
    log_level _min_level;
    otlp_http_batch_exporter<loki_log_record> _exporter;
};

static_assert(diagnostic_logger<loki_logger>, "loki_logger must satisfy diagnostic_logger concept");

}  // namespace kythira
