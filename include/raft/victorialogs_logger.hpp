#pragma once

/// @file victorialogs_logger.hpp
/// @brief `victorialogs_logger`: a `kythira::diagnostic_logger`
///        implementation pushing newline-delimited JSON to VictoriaLogs'
///        `/insert/jsonline` API — the logging leg of the VictoriaMetrics
///        metrics backend's ecosystem (doc/TODO.md "Metrics Backends",
///        logging-alongside-backends item). See
///        doc/victoriametrics_metrics_backend.md's Logging section.
///
/// VictoriaLogs ingests ND-JSON — one JSON object per line — which is a
/// line protocol, so the transport is the metrics backends' shared
/// `metrics_line_exporter` (bounded queue, background sender, drop-oldest)
/// with an HTTP sender in place of a socket: each payload of joined lines
/// becomes one POST. Stream identity (`job`/`instance`) rides in every
/// line's fields and is declared via the `_stream_fields` query arg, the
/// VictoriaLogs analogue of the metrics backend's constant labels.
///
/// Timestamps are RFC3339 with nanosecond precision in `_time` — measured
/// against a real VictoriaLogs (v1.0.0), which does NOT auto-detect
/// nanosecond unix integers: it parses a bare integer as milliseconds and
/// rejects nanosecond-sized values outright ("too big timestamp in
/// milliseconds"). RFC3339 parses in every version. Structured key-value
/// pairs become first-class JSON fields, queryable directly in LogsQL.

#include <raft/logger.hpp>
#include <raft/metrics_line_exporter.hpp>
#include <raft/otlp_exporter.hpp>  // otlp_now_unix_nanos + http_post_result/http_poster_fn seam.

#include <boost/json.hpp>

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

// ── Configuration ────────────────────────────────────────────────────────────

struct victorialogs_config {
    /// e.g. "http://victorialogs:9428" (no trailing slash).
    std::string endpoint_base_url;
    /// `_stream_fields` names the per-line fields that define the stream.
    std::string insert_path = "/insert/jsonline?_stream_fields=job,instance";
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::milliseconds http_timeout{5000};

    /// Stream identity, attached to every line (the `_stream_fields` above).
    std::string job = "kythira";
    std::string instance;

    /// Batching knobs. The default payload ceiling is raised well past the
    /// UDP-safe 1400 the socket backends use — an HTTP body has no datagram
    /// limit, and bigger payloads mean fewer POSTs.
    metrics_line_exporter_config exporter{.max_payload_bytes = 262144,
                                          .flush_interval = std::chrono::milliseconds{1000}};
};

namespace victorialogs_detail {

/// RFC3339 UTC with nanosecond fraction, e.g. "2026-08-13T06:46:54.123456789Z".
[[nodiscard]] inline auto rfc3339_nanos(std::uint64_t unix_nanos) -> std::string {
    const auto secs = static_cast<time_t>(unix_nanos / 1'000'000'000ULL);
    const auto frac = static_cast<unsigned long>(unix_nanos % 1'000'000'000ULL);
    struct tm utc{};
    ::gmtime_r(&secs, &utc);
    char buffer[64];
    auto len = ::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &utc);
    return std::string(buffer, len) + "." + std::format("{:09}", frac) + "Z";
}

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

/// Real sender: one POST per payload of joined JSON lines. Built on the
/// same httplib pattern as real_http_poster / victoriametrics_real_poster;
/// returns false on transport failure or non-2xx so the exporter counts the
/// payload's lines as dropped (no retry — the exporter's documented lossy
/// contract, and log lines age fast).
[[nodiscard]] inline auto http_jsonline_sender(
    std::string endpoint_base_url, std::string insert_path,
    std::vector<std::pair<std::string, std::string>> headers, std::chrono::milliseconds timeout)
    -> line_sender_fn {
    return [endpoint_base_url = std::move(endpoint_base_url), insert_path = std::move(insert_path),
            headers = std::move(headers), timeout](std::string_view payload) -> bool {
        httplib::Client client{endpoint_base_url};
        const auto secs = static_cast<time_t>(timeout.count() / 1000);
        const auto usecs = static_cast<time_t>((timeout.count() % 1000) * 1000);
        client.set_connection_timeout(secs, usecs);
        client.set_read_timeout(secs, usecs);
        client.set_write_timeout(secs, usecs);

        httplib::Headers hdrs;
        for (const auto& [key, value] : headers) hdrs.emplace(key, value);

        auto res =
            client.Post(insert_path, hdrs, std::string(payload) + "\n", "application/stream+json");
        return res && res->status >= 200 && res->status < 300;
    };
}

}  // namespace victorialogs_detail

/// @brief `kythira::diagnostic_logger` implementation pushing to
///        VictoriaLogs. Same mutex/min-level/move conventions as
///        `otlp_logger`/`loki_logger`.
class victorialogs_logger {
public:
    /// `sender` is the injectable test seam; when empty the real HTTP
    /// jsonline sender implied by `config` is built.
    explicit victorialogs_logger(victorialogs_config config, log_level min_level = log_level::trace,
                                 line_sender_fn sender = {})
        : _min_level(min_level), _job(config.job), _instance(config.instance) {
        if (!sender && config.endpoint_base_url.empty()) {
            throw std::invalid_argument("victorialogs_logger: endpoint_base_url must not be empty");
        }
        _exporter = std::make_unique<metrics_line_exporter>(
            config.exporter,
            sender ? std::move(sender)
                   : victorialogs_detail::http_jsonline_sender(
                         std::move(config.endpoint_base_url), std::move(config.insert_path),
                         std::move(config.headers), config.http_timeout));
    }

    victorialogs_logger(victorialogs_logger&& other) noexcept
        : _min_level(other._min_level),
          _job(std::move(other._job)),
          _instance(std::move(other._instance)),
          _exporter(std::move(other._exporter)) {}

    auto operator=(victorialogs_logger&& other) noexcept -> victorialogs_logger& {
        if (this != &other) {
            _min_level = other._min_level;
            _job = std::move(other._job);
            _instance = std::move(other._instance);
            _exporter = std::move(other._exporter);
        }
        return *this;
    }

    victorialogs_logger(const victorialogs_logger&) = delete;
    auto operator=(const victorialogs_logger&) -> victorialogs_logger& = delete;

    auto log(log_level level, std::string_view message) -> void { log(level, message, {}); }

    auto log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (level < _min_level) return;
        }
        boost::json::object line{
            {"_time", victorialogs_detail::rfc3339_nanos(otlp_now_unix_nanos())},
            {"_msg", message},
            {"level", victorialogs_detail::level_text_for(level)},
            {"job", _job},
            {"instance", _instance},
        };
        // Structured pairs become first-class fields; boost::json handles
        // all string escaping. Reserved-name collisions (_msg/_time/...)
        // are resolved last-writer-wins by object::operator[], which is
        // fine — a caller using those names means to set them.
        for (const auto& [key, value] : key_value_pairs) line[key] = value;
        _exporter->push(boost::json::serialize(line));
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
    std::string _job;
    std::string _instance;
    // unique_ptr (not a member value) so the hand-written moves stay simple;
    // the exporter itself is already pimpl'd and movable.
    std::unique_ptr<metrics_line_exporter> _exporter;
};

static_assert(diagnostic_logger<victorialogs_logger>,
              "victorialogs_logger must satisfy diagnostic_logger concept");

}  // namespace kythira
