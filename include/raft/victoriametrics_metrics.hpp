// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file victoriametrics_metrics.hpp
/// @brief VictoriaMetrics metrics backend: a `kythira::metrics`-conforming
///        handle that shares the Prometheus backend's registry and text
///        renderer (prometheus_metrics.hpp), plus a background pusher that
///        periodically POSTs the rendered exposition to VictoriaMetrics'
///        `/api/v1/import/prometheus` endpoint. See
///        doc/victoriametrics_metrics_backend.md.
///
/// Exactly the sharing doc/TODO.md's entry predicted ("likely shares most of
/// the Prometheus implementation's wire format"): the aggregate state,
/// sanitization, and rendering are all prometheus_registry's; the only new
/// machinery is the push loop. Pushing the full cumulative exposition every
/// interval has the same semantics as being scraped at that interval, so
/// counters remain monotonic and a failed push loses resolution, not data —
/// the next push carries the same cumulative totals. That is also why a
/// failed push is simply counted and NOT retried.
///
/// The push body content type is text/plain: VictoriaMetrics' import
/// endpoint parses the body as Prometheus text format regardless, but the
/// honest label costs nothing.
///
/// Copy semantics: same as the other backends here — copyable handle,
/// shared registry and pusher.

#include <raft/metrics.hpp>
#include <raft/otlp_exporter.hpp>  // http_post_result / http_poster_fn seam.
#include <raft/prometheus_metrics.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

// ── Configuration ────────────────────────────────────────────────────────────

struct victoriametrics_config {
    /// e.g. "http://victoriametrics:8428" (no trailing slash).
    std::string endpoint_base_url;
    std::string import_path = "/api/v1/import/prometheus";
    std::vector<std::pair<std::string, std::string>> headers;

    std::chrono::milliseconds push_interval{5000};
    std::chrono::milliseconds http_timeout{5000};

    /// Labels attached to every series this process emits — pushed data has
    /// no scrape target to inherit `instance`/`job` from, so identity must
    /// travel in the labels themselves (e.g. {{"job","kythira"},
    /// {"instance","node-42"}}).
    std::vector<std::pair<std::string, std::string>> constant_labels;

    prometheus_metrics_config registry{};
};

/// Real poster for the import endpoint. Same shape as otlp_exporter.hpp's
/// real_http_poster, but with a text/plain body.
[[nodiscard]] inline auto victoriametrics_real_poster() -> http_poster_fn {
    return [](std::string_view origin, std::string_view path,
              const std::vector<std::pair<std::string, std::string>>& headers,
              std::string_view body, std::chrono::milliseconds timeout) -> http_post_result {
        httplib::Client client{std::string(origin)};
        const auto secs = static_cast<time_t>(timeout.count() / 1000);
        const auto usecs = static_cast<time_t>((timeout.count() % 1000) * 1000);
        client.set_connection_timeout(secs, usecs);
        client.set_read_timeout(secs, usecs);
        client.set_write_timeout(secs, usecs);

        httplib::Headers hdrs;
        for (const auto& [key, value] : headers) hdrs.emplace(key, value);

        auto res = client.Post(std::string(path), hdrs, std::string(body), "text/plain");
        if (!res) return {.ok = false, .status = 0};
        return {.ok = (res->status >= 200 && res->status < 300), .status = res->status};
    };
}

// ── The concept-conforming handle ────────────────────────────────────────────

class victoriametrics_metrics {
public:
    /// `poster` is the injectable test seam from otlp_exporter.hpp.
    explicit victoriametrics_metrics(victoriametrics_config config,
                                     http_poster_fn poster = victoriametrics_real_poster())
        : _registry(std::make_shared<prometheus_registry>(config.registry)),
          _constant_labels(config.constant_labels),
          _inner(_registry),
          _pusher(std::make_shared<pusher>(std::move(config), _registry, std::move(poster))) {}

    auto set_metric_name(std::string_view name) -> void {
        _inner.set_metric_name(name);
        for (const auto& [label, value] : _constant_labels) _inner.add_dimension(label, value);
    }

    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        _inner.add_dimension(dimension_name, dimension_value);
    }

    auto add_one() -> void { _inner.add_one(); }
    auto add_count(std::int64_t count) -> void { _inner.add_count(count); }
    auto add_duration(std::chrono::nanoseconds duration) -> void { _inner.add_duration(duration); }
    auto add_value(double value) -> void { _inner.add_value(value); }
    auto emit() -> void { _inner.emit(); }

    /// The shared registry (also useful for exposing a scrape endpoint
    /// alongside the push — the two backends compose on one registry).
    [[nodiscard]] auto registry() const -> const std::shared_ptr<prometheus_registry>& {
        return _registry;
    }

    /// Pushes that returned failure (transport error or non-2xx). Not a data
    /// loss count — see the header comment.
    [[nodiscard]] auto failed_push_count() const -> std::uint64_t {
        return _pusher->failed_pushes.load(std::memory_order_relaxed);
    }

private:
    /// Background push loop: render + POST every push_interval, plus one
    /// final best-effort push at destruction so short-lived processes still
    /// deliver their last totals.
    struct pusher {
        pusher(victoriametrics_config cfg, std::shared_ptr<prometheus_registry> reg,
               http_poster_fn post)
            : config(std::move(cfg)), registry(std::move(reg)), poster(std::move(post)) {
            if (config.endpoint_base_url.empty()) {
                throw std::invalid_argument(
                    "victoriametrics_metrics: endpoint_base_url must not be empty");
            }
            worker = std::thread([this] { push_loop(); });
        }

        ~pusher() {
            {
                std::lock_guard<std::mutex> lock(mu);
                stop_flag = true;
            }
            cv.notify_all();
            if (worker.joinable()) worker.join();
        }

        pusher(const pusher&) = delete;
        auto operator=(const pusher&) -> pusher& = delete;
        pusher(pusher&&) = delete;
        auto operator=(pusher&&) -> pusher& = delete;

        auto push_once() -> void {
            auto body = registry->render_text();
            if (body.empty()) return;
            auto result = poster(config.endpoint_base_url, config.import_path, config.headers, body,
                                 config.http_timeout);
            if (!result.ok) failed_pushes.fetch_add(1, std::memory_order_relaxed);
        }

        auto push_loop() -> void {
            std::unique_lock<std::mutex> lock(mu);
            while (!stop_flag) {
                cv.wait_for(lock, config.push_interval, [this] { return stop_flag; });
                lock.unlock();
                push_once();
                lock.lock();
            }
            lock.unlock();
            push_once();  // Final best-effort flush; bounded by http_timeout.
        }

        victoriametrics_config config;
        std::shared_ptr<prometheus_registry> registry;
        http_poster_fn poster;

        std::mutex mu;
        std::condition_variable cv;
        bool stop_flag = false;
        std::atomic<std::uint64_t> failed_pushes{0};
        // Constructor-body assignment, same rationale as the other exporters.
        std::thread worker;
    };

    std::shared_ptr<prometheus_registry> _registry;
    std::vector<std::pair<std::string, std::string>> _constant_labels;
    prometheus_metrics _inner;
    std::shared_ptr<pusher> _pusher;
};

static_assert(metrics<victoriametrics_metrics>,
              "victoriametrics_metrics must satisfy metrics concept");

}  // namespace kythira
