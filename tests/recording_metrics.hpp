// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file recording_metrics.hpp
/// @brief A `kythira::metrics` model that remembers every emission, so a test
///        can ask what the transport actually did rather than only whether it
///        returned.
///
/// Several behaviours in the transports are observable *only* through their
/// metrics: which media type an exchange settled on, and — since the retry
/// happens entirely inside `send_rpc` — how many times and with what a client
/// retried after a 415. A round trip that succeeds proves the two sides agreed
/// on something, not which something, and not how many attempts it took.
///
/// Lives in its own header rather than inside `negotiation_test_harness.hpp`,
/// where it started, because the harness drives cpp-httplib specifically while
/// this is transport-agnostic: the Beast and Proxygen negotiation suites need
/// the same observability and must not pull in an httplib client/server rig to
/// get it.

#include <raft/metrics.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kythira::testing {

/// One emitted metric, flattened to the two things these suites ask about.
struct emitted_metric {
    std::string name;
    std::unordered_map<std::string, std::string> dimensions;
};

/// Shared sink behind every copy of `recording_metrics`.
///
/// The transports take metrics *by value* and copy per emission
/// (`auto metric = _metrics;`), which is what a metrics handle is supposed to
/// support — so recording state cannot live in the handle. It lives here, behind
/// a `shared_ptr`, and every copy writes to the same sink.
struct metric_recorder {
    mutable std::mutex mutex;
    std::vector<emitted_metric> entries;

    auto record(emitted_metric e) -> void {
        const std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(std::move(e));
    }

    /// @brief The `media_type` dimension of the first metric named @p name, or
    ///        an empty string if no such metric carried one.
    ///
    /// Callers must treat empty as a failure rather than as "no preference": a
    /// missing emission is exactly the "green while doing nothing" shape this
    /// repo keeps hitting, and it must not be indistinguishable from a
    /// legitimately empty value.
    [[nodiscard]] auto media_type_of(std::string_view name) const -> std::string {
        const std::lock_guard<std::mutex> lock(mutex);
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find("media_type"); it != e.dimensions.end()) {
                return it->second;
            }
        }
        return {};
    }

    /// @brief Every `media_type` value seen on metrics named @p name, in order.
    [[nodiscard]] auto media_types_of(std::string_view name) const -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> result;
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find("media_type"); it != e.dimensions.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    /// @brief Every value of dimension @p dimension on metrics named @p name.
    [[nodiscard]] auto dimension_values(std::string_view name, std::string_view dimension) const
        -> std::vector<std::string> {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> result;
        for (const auto& e : entries) {
            if (e.name != name) {
                continue;
            }
            if (const auto it = e.dimensions.find(std::string(dimension));
                it != e.dimensions.end()) {
                result.push_back(it->second);
            }
        }
        return result;
    }

    [[nodiscard]] auto count_named(std::string_view name) const -> std::size_t {
        const std::lock_guard<std::mutex> lock(mutex);
        return static_cast<std::size_t>(
            std::count_if(entries.begin(), entries.end(),
                          [&](const emitted_metric& e) { return e.name == name; }));
    }
};

/// Models `kythira::metrics`, forwarding each emission to a shared recorder.
class recording_metrics {
public:
    recording_metrics() : _recorder(std::make_shared<metric_recorder>()) {}
    explicit recording_metrics(std::shared_ptr<metric_recorder> r) : _recorder(std::move(r)) {}

    auto set_metric_name(std::string_view name) -> void { _pending.name = std::string(name); }
    auto add_dimension(std::string_view name, std::string_view value) -> void {
        _pending.dimensions.emplace(std::string(name), std::string(value));
    }
    auto add_one() -> void {}
    auto add_count(std::int64_t) -> void {}
    auto add_duration(std::chrono::nanoseconds) -> void {}
    auto add_value(double) -> void {}
    auto emit() -> void {
        if (_recorder && !_pending.name.empty()) {
            _recorder->record(_pending);
        }
        _pending = {};
    }

    [[nodiscard]] auto recorder() const -> const std::shared_ptr<metric_recorder>& {
        return _recorder;
    }

private:
    std::shared_ptr<metric_recorder> _recorder;
    emitted_metric _pending;
};

static_assert(kythira::metrics<recording_metrics>);

}  // namespace kythira::testing
