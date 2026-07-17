#pragma once

/// @file otlp_exporter.hpp
/// @brief Shared, non-blocking, batching OTLP/HTTP-JSON export engine used by
///        both `otlp_metrics` (otlp_metrics.hpp) and `otlp_logger`
///        (otlp_logger.hpp). See .kiro/specs/otlp-telemetry-backend/.
///
/// `otlp_http_batch_exporter<Record>` owns a bounded queue and a background
/// thread: `push()` (the only method either concept-facing class calls) just
/// enqueues under a mutex and returns — no socket, DNS, or TLS work ever
/// happens on the calling thread. The background thread batches, JSON-encodes
/// via a caller-supplied `encode` callback, and POSTs with retry.

#include <boost/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

// ── Time helper ──────────────────────────────────────────────────────────────

[[nodiscard]] inline auto otlp_now_unix_nanos() -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// ── JSON helpers shared by otlp_metrics.hpp / otlp_logger.hpp ────────────────

[[nodiscard]] inline auto otlp_string_kv(std::string_view key, std::string_view value)
    -> boost::json::object {
    return boost::json::object{
        {"key", boost::json::string(key)},
        {"value", boost::json::object{{"stringValue", boost::json::string(value)}}}};
}

[[nodiscard]] inline auto otlp_attributes_array(
    const std::vector<std::pair<std::string, std::string>>& attributes) -> boost::json::array {
    boost::json::array arr;
    arr.reserve(attributes.size());
    for (const auto& [key, value] : attributes) arr.push_back(otlp_string_kv(key, value));
    return arr;
}

// ── Resource / config (Requirements 4, 5) ─────────────────────────────────────

/// @brief Attributes identifying the process emitting telemetry, attached once
///        per OTLP export request and shared by every data point/LogRecord in
///        it (Requirement 4).
struct otlp_resource {
    std::string service_name;
    std::string service_instance_id;
    std::optional<std::string> service_namespace;
    std::vector<std::pair<std::string, std::string>> extra_attributes;

    [[nodiscard]] auto to_json() const -> boost::json::object {
        boost::json::array attrs;
        attrs.push_back(otlp_string_kv("service.name", service_name));
        attrs.push_back(otlp_string_kv("service.instance.id", service_instance_id));
        if (service_namespace) attrs.push_back(otlp_string_kv("service.namespace", *service_namespace));
        for (const auto& [key, value] : extra_attributes) attrs.push_back(otlp_string_kv(key, value));
        return boost::json::object{{"attributes", attrs}};
    }
};

/// @brief Batching/retry/histogram-bucket knobs (Requirement 5.1). Only
///        `endpoint_base_url` is required; everything else has a documented
///        default.
struct otlp_export_config {
    std::string endpoint_base_url;  ///< e.g. "http://otel-collector:4318" (no trailing slash).
    std::vector<std::pair<std::string, std::string>> headers;

    std::size_t max_batch_size = 512;
    std::chrono::milliseconds flush_interval{5000};
    std::size_t max_queue_size = 4096;
    std::chrono::milliseconds http_timeout{5000};
    unsigned max_retries = 3;
    std::chrono::milliseconds retry_backoff_base{200};

    // Requirement 1.4: milliseconds; spread covering sub-millisecond to
    // multi-second Raft RPC/consensus latencies.
    std::vector<double> histogram_bounds_ms{1,  2,   5,   10,  25,  50,   100,
                                            250, 500, 1000, 2500, 5000, 10000};
};

// ── Series identity for delta-temporality start-time tracking (Req 1.3) ──────

using otlp_series_key = std::string;

[[nodiscard]] inline auto otlp_make_series_key(
    std::string_view name, const std::vector<std::pair<std::string, std::string>>& dimensions)
    -> otlp_series_key {
    auto sorted = dimensions;
    std::ranges::sort(sorted);
    std::string key(name);
    for (const auto& [dim_name, dim_value] : sorted) {
        key += '\x1f';
        key += dim_name;
        key += '=';
        key += dim_value;
    }
    return key;
}

// ── HTTP POST seam (Requirement 3.7) ──────────────────────────────────────────

struct http_post_result {
    bool ok = false;
    int status = 0;
};

/// Injectable POST seam: `origin` is `otlp_export_config::endpoint_base_url`;
/// `path` is the OTLP signal path ("/v1/metrics" or "/v1/logs"). Kept
/// separate rather than a single pre-joined URL so the default implementation
/// never has to parse one back apart.
using http_poster_fn = std::function<http_post_result(
    std::string_view origin, std::string_view path,
    const std::vector<std::pair<std::string, std::string>>& headers, std::string_view json_body,
    std::chrono::milliseconds timeout)>;

/// Real `cpp-httplib`-backed poster (`httplib::Client` auto-selects TLS based
/// on `origin`'s scheme, same as every other httplib::Client construction in
/// this project — see include/raft/ca_bootstrap_client.hpp).
[[nodiscard]] inline auto real_http_poster() -> http_poster_fn {
    return [](std::string_view origin, std::string_view path,
             const std::vector<std::pair<std::string, std::string>>& headers,
             std::string_view json_body, std::chrono::milliseconds timeout) -> http_post_result {
        httplib::Client client{std::string(origin)};
        const auto secs = static_cast<time_t>(timeout.count() / 1000);
        const auto usecs = static_cast<time_t>((timeout.count() % 1000) * 1000);
        client.set_connection_timeout(secs, usecs);
        client.set_read_timeout(secs, usecs);
        client.set_write_timeout(secs, usecs);

        httplib::Headers hdrs;
        for (const auto& [key, value] : headers) hdrs.emplace(key, value);

        auto res = client.Post(std::string(path), hdrs, std::string(json_body), "application/json");
        if (!res) return {.ok = false, .status = 0};
        return {.ok = (res->status >= 200 && res->status < 300), .status = res->status};
    };
}

// ── Shared batch exporter (Requirement 3) ─────────────────────────────────────

template<typename Record>
using otlp_encode_fn =
    std::function<boost::json::object(const otlp_resource&, std::span<const Record>)>;

/// One instance per OTLP signal (metrics or logs). `push()` is the only
/// method reachable from a `metrics`/`diagnostic_logger` call site and never
/// blocks on I/O (Requirement 3.2). Pimpl'd (`_impl` is a stable-address
/// heap object) so the wrapper itself stays trivially movable — required so
/// `otlp_metrics`/`otlp_logger` can satisfy Requirement 1.6/2.5's
/// move-constructible requirement — while the background thread's captured
/// `this` (pointing at `impl`, never at the outer wrapper) stays valid across
/// moves of the wrapper.
template<typename Record>
class otlp_http_batch_exporter {
public:
    otlp_http_batch_exporter(otlp_export_config config, otlp_resource resource,
                             std::string signal_path, otlp_encode_fn<Record> encode,
                             http_poster_fn poster = real_http_poster())
        : _impl(std::make_unique<impl>(std::move(config), std::move(resource),
                                       std::move(signal_path), std::move(encode),
                                       std::move(poster))) {}

    otlp_http_batch_exporter(otlp_http_batch_exporter&&) noexcept = default;
    auto operator=(otlp_http_batch_exporter&&) noexcept -> otlp_http_batch_exporter& = default;
    otlp_http_batch_exporter(const otlp_http_batch_exporter&) = delete;
    auto operator=(const otlp_http_batch_exporter&) -> otlp_http_batch_exporter& = delete;

    ~otlp_http_batch_exporter() = default;  // impl's destructor does the real shutdown work.

    // Requirement 3.2: never blocks on I/O.
    auto push(Record record) -> void {
        if (_impl) _impl->push(std::move(record));
    }

    // Requirement 3.3: overflow/failed-export visibility for tests/operators.
    [[nodiscard]] auto dropped_record_count() const -> std::uint64_t {
        return _impl ? _impl->dropped_record_count() : 0;
    }

private:
    struct impl {
        impl(otlp_export_config cfg, otlp_resource res, std::string path,
            otlp_encode_fn<Record> enc, http_poster_fn p)
            : config(std::move(cfg)),
              resource(std::move(res)),
              signal_path(std::move(path)),
              encode(std::move(enc)),
              poster(std::move(p)) {
            if (config.endpoint_base_url.empty()) {
                throw std::invalid_argument(
                    "otlp_http_batch_exporter: endpoint_base_url must not be empty");
            }
            worker = std::thread([this] { background_loop(); });
        }

        ~impl() {
            stop_flag.store(true, std::memory_order_relaxed);
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }

        impl(const impl&) = delete;
        auto operator=(const impl&) -> impl& = delete;
        impl(impl&&) = delete;
        auto operator=(impl&&) -> impl& = delete;

        // Requirement 3.3: drop-oldest overflow, bounding memory during a
        // sustained collector outage.
        auto push(Record record) -> void {
            std::lock_guard<std::mutex> lock(mu);
            if (queue.size() >= config.max_queue_size) {
                queue.pop_front();
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
            queue.push_back(std::move(record));
            if (queue.size() >= config.max_batch_size) cv.notify_one();
        }

        [[nodiscard]] auto dropped_record_count() const -> std::uint64_t {
            return dropped.load(std::memory_order_relaxed);
        }

        // Requirement 3.4: batch by size or interval, whichever first.
        auto drain_locked() -> std::vector<Record> {
            std::vector<Record> batch;
            auto n = std::min(queue.size(), config.max_batch_size);
            batch.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(queue.front()));
                queue.pop_front();
            }
            return batch;
        }

        auto wait_and_drain() -> std::vector<Record> {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait_for(lock, config.flush_interval, [this] {
                return stop_flag.load(std::memory_order_relaxed) ||
                       queue.size() >= config.max_batch_size;
            });
            return drain_locked();
        }

        auto drain_now() -> std::vector<Record> {
            std::lock_guard<std::mutex> lock(mu);
            return drain_locked();
        }

        // Requirement 3.5: retryable transport failure / 429 / 502 / 503 /
        // 504 retried with doubling backoff up to max_retries, then dropped.
        auto send_with_retry(const std::vector<Record>& batch) -> void {
            if (batch.empty()) return;
            auto body = encode(resource, std::span<const Record>(batch));
            auto json_body = boost::json::serialize(body);

            auto backoff = config.retry_backoff_base;
            for (unsigned attempt = 0; attempt <= config.max_retries; ++attempt) {
                auto result =
                    poster(config.endpoint_base_url, signal_path, config.headers, json_body,
                          config.http_timeout);
                if (result.ok) return;

                const bool retryable = result.status == 0 || result.status == 429 ||
                                       result.status == 502 || result.status == 503 ||
                                       result.status == 504;
                if (!retryable || attempt == config.max_retries) {
                    dropped.fetch_add(batch.size(), std::memory_order_relaxed);
                    return;
                }
                std::this_thread::sleep_for(backoff);
                backoff *= 2;
            }
        }

        auto background_loop() -> void {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                send_with_retry(wait_and_drain());
            }
            // Requirement 3.6: one final best-effort flush, bounded by the
            // same http_timeout/max_retries send_with_retry already applies —
            // not a full drain loop, so a still-growing backlog at shutdown
            // cannot delay process exit indefinitely.
            send_with_retry(drain_now());
        }

        otlp_export_config config;
        otlp_resource resource;
        std::string signal_path;
        otlp_encode_fn<Record> encode;
        http_poster_fn poster;

        std::mutex mu;
        std::condition_variable cv;
        std::deque<Record> queue;
        std::atomic<bool> stop_flag{false};
        std::atomic<std::uint64_t> dropped{0};
        // Assigned in the constructor body (not the init list) so every
        // other member above is already fully constructed before
        // background_loop() — running on this thread — can observe them.
        std::thread worker;
    };

    std::unique_ptr<impl> _impl;
};

}  // namespace kythira
