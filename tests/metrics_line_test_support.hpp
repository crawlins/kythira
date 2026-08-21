// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file metrics_line_test_support.hpp
/// @brief Shared helpers for the line-protocol metrics backend tests
///        (telegraf_metrics, netdata_metrics, metrics_line_exporter):
///        a recording line_sender_fn and a condition-variable wait —
///        the same shape as otlp_test_support.hpp's recording poster,
///        never a fixed sleep.

#include <raft/metrics_line_exporter.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace metrics_line_test_support {

struct recording_sender_state {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> payloads;  ///< One entry per sender invocation.
    std::size_t fail_first_n_calls = 0;
    std::size_t calls = 0;
};

[[nodiscard]] inline auto make_recording_sender(std::shared_ptr<recording_sender_state> state)
    -> kythira::line_sender_fn {
    return [state](std::string_view payload) -> bool {
        std::lock_guard<std::mutex> lock(state->mu);
        ++state->calls;
        const bool fail = state->calls <= state->fail_first_n_calls;
        if (!fail) state->payloads.emplace_back(payload);
        state->cv.notify_all();
        return !fail;
    };
}

/// Wait until the sender has delivered at least `n` payloads.
[[nodiscard]] inline auto wait_for_payloads(recording_sender_state& state, std::size_t n,
                                            std::chrono::milliseconds timeout) -> bool {
    std::unique_lock<std::mutex> lock(state.mu);
    return state.cv.wait_for(lock, timeout, [&] { return state.payloads.size() >= n; });
}

/// Exporter config tuned so tests never wait on the default 1 s flush
/// interval (mirrors otlp_test_support::fast_test_config()).
[[nodiscard]] inline auto fast_exporter_config() -> kythira::metrics_line_exporter_config {
    kythira::metrics_line_exporter_config config;
    config.max_batch_lines = 1;
    config.flush_interval = std::chrono::milliseconds{20};
    return config;
}

/// Split a payload into its newline-separated protocol lines.
[[nodiscard]] inline auto split_lines(const std::string& payload) -> std::vector<std::string> {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= payload.size()) {
        auto pos = payload.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(payload.substr(start));
            break;
        }
        lines.push_back(payload.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

}  // namespace metrics_line_test_support
