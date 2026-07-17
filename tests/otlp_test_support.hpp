#pragma once

/// @file otlp_test_support.hpp
/// @brief Shared test-only helpers for the otlp-telemetry-backend spec's
///        unit tests (otlp_metrics_test.cpp, otlp_logger_test.cpp,
///        otlp_exporter_unit_test.cpp): a thread-safe stub `Poster`
///        (Requirement 3.7) recording every call an
///        `otlp_http_batch_exporter`'s background thread makes, with no
///        real network I/O, plus a condition-variable-based wait helper so
///        tests never need a fixed sleep to synchronize with that thread.

#include <raft/otlp_exporter.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace otlp_test_support {

struct recording_poster_state {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> paths;
    std::vector<std::string> bodies;
    std::vector<std::vector<std::pair<std::string, std::string>>> headers;
    unsigned fail_first_n_calls = 0;
    unsigned call_count = 0;
};

[[nodiscard]] inline auto make_recording_poster(std::shared_ptr<recording_poster_state> state)
    -> kythira::http_poster_fn {
    return [state](std::string_view /*origin*/, std::string_view path,
                   const std::vector<std::pair<std::string, std::string>>& hdrs,
                   std::string_view json_body,
                   std::chrono::milliseconds /*timeout*/) -> kythira::http_post_result {
        std::lock_guard<std::mutex> lock(state->mu);
        ++state->call_count;
        state->paths.emplace_back(path);
        state->bodies.emplace_back(json_body);
        state->headers.push_back(hdrs);
        state->cv.notify_all();
        if (state->call_count <= state->fail_first_n_calls) {
            return {.ok = false, .status = 503};
        }
        return {.ok = true, .status = 200};
    };
}

/// Polls via condition_variable (never a fixed sleep) until the poster has
/// recorded at least `n` calls, or `timeout` elapses.
[[nodiscard]] inline auto wait_for_calls(recording_poster_state& state, std::size_t n,
                                         std::chrono::milliseconds timeout) -> bool {
    std::unique_lock<std::mutex> lock(state.mu);
    return state.cv.wait_for(lock, timeout, [&] { return state.call_count >= n; });
}

// A small config tuned so a single push() is flushed almost immediately —
// keeps unit tests fast without relying on a fixed sleep matching a longer
// production-sized flush_interval.
[[nodiscard]] inline auto fast_test_config(std::string endpoint = "http://localhost:1")
    -> kythira::otlp_export_config {
    kythira::otlp_export_config cfg;
    cfg.endpoint_base_url = std::move(endpoint);
    cfg.max_batch_size = 1;
    cfg.flush_interval = std::chrono::milliseconds{20};
    cfg.max_queue_size = 16;
    cfg.http_timeout = std::chrono::milliseconds{500};
    cfg.max_retries = 2;
    cfg.retry_backoff_base = std::chrono::milliseconds{10};
    return cfg;
}

[[nodiscard]] inline auto test_resource() -> kythira::otlp_resource {
    kythira::otlp_resource res;
    res.service_name = "otlp-test";
    res.service_instance_id = "1";
    return res;
}

}  // namespace otlp_test_support
