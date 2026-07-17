#pragma once

/// @file otlp_logger.hpp
/// @brief `otlp_logger`: a `kythira::diagnostic_logger` implementation that
///        emits OTLP LogRecords over OTLP/HTTP JSON. See
///        .kiro/specs/otlp-telemetry-backend/ (Requirement 2).

#include <raft/logger.hpp>
#include <raft/otlp_exporter.hpp>

#include <boost/json.hpp>

#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira {

/// One `log(...)` call's complete, self-contained OTLP LogRecord — unlike
/// `otlp_metric_record`, no multi-call accumulation is needed: every
/// `log()`/convenience-method call already carries everything a LogRecord
/// needs.
struct otlp_log_record {
    std::uint64_t time_unix_nano = 0;
    int severity_number = 0;
    std::string severity_text;
    std::string body;
    std::vector<std::pair<std::string, std::string>> attributes;
};

namespace otlp_logger_detail {

// Requirement 2.2: base value of each level's 4-wide OTLP SeverityNumber band.
[[nodiscard]] inline auto severity_for(log_level level) -> std::pair<int, std::string_view> {
    switch (level) {
        case log_level::trace:
            return {1, "TRACE"};
        case log_level::debug:
            return {5, "DEBUG"};
        case log_level::info:
            return {9, "INFO"};
        case log_level::warning:
            return {13, "WARN"};
        case log_level::error:
            return {17, "ERROR"};
        case log_level::critical:
            return {21, "FATAL"};
    }
    return {0, "UNSPECIFIED"};
}

[[nodiscard]] inline auto encode_batch(const otlp_resource& resource,
                                       std::span<const otlp_log_record> records)
    -> boost::json::object {
    boost::json::array log_records;
    log_records.reserve(records.size());

    for (const auto& record : records) {
        boost::json::object entry{
            {"timeUnixNano", std::to_string(record.time_unix_nano)},
            {"severityNumber", record.severity_number},
            {"severityText", record.severity_text},
            {"body", boost::json::object{{"stringValue", record.body}}},
            {"attributes", otlp_attributes_array(record.attributes)},
        };
        log_records.push_back(std::move(entry));
    }

    boost::json::object scope_logs{{"logRecords", log_records}};
    boost::json::object resource_logs{{"resource", resource.to_json()},
                                      {"scopeLogs", boost::json::array{scope_logs}}};
    return boost::json::object{{"resourceLogs", boost::json::array{resource_logs}}};
}

}  // namespace otlp_logger_detail

/// @brief `kythira::diagnostic_logger` implementation emitting OTLP/HTTP
///        JSON LogRecords. Owns its own mutex (Requirement 2.4), mirroring
///        `console_logger`'s existing convention.
class otlp_logger {
public:
    // `poster` defaults to the real cpp-httplib-backed implementation;
    // Requirement 3.7's injectable seam is exposed here (not just on
    // otlp_http_batch_exporter itself) so unit tests can substitute a stub
    // without any real network I/O — see tests/otlp_logger_test.cpp.
    explicit otlp_logger(otlp_export_config config, otlp_resource resource,
                         log_level min_level = log_level::trace,
                         http_poster_fn poster = real_http_poster())
        : _min_level(min_level),
          _exporter(std::move(config), std::move(resource), "/v1/logs",
                    &otlp_logger_detail::encode_batch, std::move(poster)) {}

    otlp_logger(otlp_logger&&) noexcept = default;
    auto operator=(otlp_logger&&) noexcept -> otlp_logger& = default;
    otlp_logger(const otlp_logger&) = delete;
    auto operator=(const otlp_logger&) -> otlp_logger& = delete;

    auto log(log_level level, std::string_view message) -> void {
        log(level, message, {});
    }

    auto log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (level < _min_level) return;
        }

        const auto [severity_number, severity_text] = otlp_logger_detail::severity_for(level);

        otlp_log_record record;
        record.time_unix_nano = otlp_now_unix_nanos();
        record.severity_number = severity_number;
        record.severity_text = std::string(severity_text);
        record.body = std::string(message);
        record.attributes.reserve(key_value_pairs.size());
        for (const auto& [key, value] : key_value_pairs) record.attributes.emplace_back(key, value);

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
    otlp_http_batch_exporter<otlp_log_record> _exporter;
};

static_assert(diagnostic_logger<otlp_logger>, "otlp_logger must satisfy diagnostic_logger concept");

}  // namespace kythira
