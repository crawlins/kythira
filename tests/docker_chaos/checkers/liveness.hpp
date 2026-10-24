// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Liveness checker (.kiro/specs/cairn-chaos-testing/ Requirement 5.3, design
// §2.3): once the last fault is healed, a leader is elected and commit_index
// advances within a bound of k × ELECTION_TIMEOUT_MAX_MS ×
// KYTHIRA_TEST_TIMEOUT_SCALE.
//
// The check is split from the polling so it can be unit tested: check()
// judges a list of timestamped samples, and wait() takes a probe and a clock
// and produces that list. The caller drives writes while waiting; a cluster
// that keeps its leader commits nothing on its own.

#include "linearizability.hpp"  // verdict

#include "../../test_timeout_scale.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace docker_chaos::checkers::liveness {

/// How many maximum election timeouts recovery may take. Split votes cost a
/// timeout each and a randomised timeout can land at the maximum, so a few
/// rounds plus the commit of the new leader's no-op: 10 covers three or four
/// split votes back to back with room for container scheduling.
inline constexpr unsigned default_k = 10;

[[nodiscard]] constexpr auto bound(std::chrono::milliseconds election_timeout_max,
                                   unsigned k = default_k) -> std::chrono::milliseconds {
    return election_timeout_max * k * static_cast<unsigned>(KYTHIRA_TEST_TIMEOUT_SCALE);
}

/// One observation of the cluster, `at` measured from when the fault healed.
struct sample {
    std::chrono::milliseconds at{0};
    std::optional<int> leader;
    std::uint64_t commit_index{0};
};

struct result {
    verdict overall{verdict::ok};
    std::chrono::milliseconds limit{0};
    std::optional<std::chrono::milliseconds> leader_at;
    std::optional<std::chrono::milliseconds> commit_at;
    std::uint64_t baseline_commit{0};
    std::string detail;
};

/// `baseline_commit` is the highest commit_index seen before the heal.
/// Samples after `limit` are ignored: progress late is a violation.
[[nodiscard]] inline auto check(const std::vector<sample>& samples, std::uint64_t baseline_commit,
                                std::chrono::milliseconds limit) -> result {
    result r{.overall = verdict::ok,
             .limit = limit,
             .leader_at = std::nullopt,
             .commit_at = std::nullopt,
             .baseline_commit = baseline_commit,
             .detail = {}};
    for (const auto& s : samples) {
        if (s.at > limit) {
            break;
        }
        if (!r.leader_at && s.leader) {
            r.leader_at = s.at;
        }
        if (!r.commit_at && s.commit_index > baseline_commit) {
            r.commit_at = s.at;
        }
    }
    if (!r.leader_at) {
        r.overall = verdict::violation;
        r.detail = "no leader within " + std::to_string(limit.count()) + "ms of the heal";
    } else if (!r.commit_at) {
        r.overall = verdict::violation;
        r.detail = "commit_index stayed at " + std::to_string(baseline_commit) + " for " +
                   std::to_string(limit.count()) + "ms after the heal";
    }
    return r;
}

/// Polls `probe` every `interval` until a leader is seen and commit_index
/// has passed `baseline_commit`, or `limit` runs out, then checks what it
/// saw. `probe` returns the leader (if any node reports one) and the highest
/// commit_index any reachable node reports; it should not throw for an
/// unreachable node.
template<typename Clock = std::chrono::steady_clock>
[[nodiscard]] auto wait(const std::function<sample()>& probe, std::uint64_t baseline_commit,
                        std::chrono::milliseconds limit,
                        std::chrono::milliseconds interval = std::chrono::milliseconds{100})
    -> result {
    std::vector<sample> seen;
    const auto start = Clock::now();
    bool leader = false;
    bool committed = false;
    while (true) {
        auto s = probe();
        s.at = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
        leader = leader || s.leader.has_value();
        committed = committed || s.commit_index > baseline_commit;
        const bool late = s.at > limit;
        seen.push_back(s);
        if ((leader && committed) || late) {
            break;
        }
        std::this_thread::sleep_for(interval);
    }
    return check(seen, baseline_commit, limit);
}

}  // namespace docker_chaos::checkers::liveness
