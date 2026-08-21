// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file coap_test_support.hpp
/// @brief Shared helpers for the `tests/coap_*.cpp` suites.
///
/// Exists to remove a failure mode that has repeatedly been mistaken for
/// several unrelated bugs: a CoAP test case that exceeds its
/// `*boost::unit_test::timeout()` while worker threads are still running.
/// Boost's timeout unwinds the test case, the local `std::vector<std::thread>`
/// is destroyed, and `~std::thread()` on a still-joinable thread calls
/// `std::terminate()` — aborting the whole binary instead of failing one case.
/// Every case that would have run afterwards is lost, and the abort surfaces
/// at an arbitrary point, which is why the reported failure set moves around
/// between CI runs and why it has been reported variously as an "arm64
/// SEGFAULT" and as unrelated later tests crashing.

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

#include <raft/coap_transport.hpp>

namespace kythira::testing::coap {

/// @brief Client settings for a case that deliberately targets an endpoint
///        with no listener.
///
/// Several coap tests point a `coap_client` at an address nothing is bound to
/// — `coap://127.0.0.1:61150`, `coap://localhost:61120` — to exercise the
/// failure path. With the default settings that is a race the test can only
/// win by luck: `ack_timeout` is 2s, `max_retransmit` is 4, and
/// `exponential_backoff_factor` is 2.0, so a confirmable message is retried
/// for roughly 2+4+8+16+32 = 62 seconds (plus up to 1s of random factor per
/// attempt) against a `*boost::unit_test::timeout()` of 15-30s. The case
/// passes only when an ICMP port-unreachable happens to cut the schedule
/// short first, which is why these tests fail in bursts on a loaded runner
/// and why raising their timeouts never fixed them.
///
/// This collapses the schedule so the failure is bounded by construction: a
/// short ack window and a single retransmission, roughly 200ms + 400ms rather
/// than 62 seconds. The test still exercises the same code path — a request
/// that gets no response, retransmitted and then given up on — it just stops
/// depending on the network stack to interrupt a minute-long retry loop.
///
/// The retransmission counts are deliberately 1 and not 0.
/// `coap_confirmable_message_property_test` asserts
/// `BOOST_CHECK_GT(config.max_retransmissions, 0)` and
/// `BOOST_CHECK_GT(config.retransmission_timeout.count(), 0)` — the
/// configuration is part of what it is checking — so a config that zeroed
/// them would fail the very tests this helper is meant to stabilise, and
/// would also stop exercising the retransmit path at all.
[[nodiscard]] inline auto unreachable_endpoint_client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.ack_timeout = std::chrono::milliseconds{200};
    config.ack_random_factor_ms = std::chrono::milliseconds{0};
    config.max_retransmit = 1;
    config.max_retransmissions = 1;
    config.retransmission_timeout = std::chrono::milliseconds{200};
    return config;
}

/// @brief Owns worker threads and joins them on destruction.
///
/// Use in place of a bare `std::vector<std::thread>` in any test case that
/// spawns threads. The explicit `join_all()` stays useful for tests that need
/// to observe results before asserting; the destructor is the safety net for
/// the path that has actually been biting — unwinding on timeout before
/// reaching that call.
///
/// Note what this does and does not buy. It does not make a slow test fast,
/// and if a worker is blocked (e.g. inside `send_rpc()` waiting out CoAP
/// retransmission) the destructor blocks with it. What it converts is the
/// *failure mode*: from `std::terminate()` at an arbitrary point — taking
/// every later test case with it — into either a clean per-case failure, or
/// at worst a hang that CTest's own `TIMEOUT` attributes to the right test.
/// Bounding the underlying wait is a separate concern from bounding the
/// damage, and is handled by the retransmission settings on the tests that
/// deliberately target a non-listening endpoint.
class joining_thread_group {
public:
    joining_thread_group() = default;

    joining_thread_group(const joining_thread_group&) = delete;
    auto operator=(const joining_thread_group&) -> joining_thread_group& = delete;
    joining_thread_group(joining_thread_group&&) = delete;
    auto operator=(joining_thread_group&&) -> joining_thread_group& = delete;

    ~joining_thread_group() { join_all(); }

    /// Constructs and starts a thread owned by this group.
    template<typename Fn, typename... Args> void spawn(Fn&& fn, Args&&... args) {
        _threads.emplace_back(std::forward<Fn>(fn), std::forward<Args>(args)...);
    }

    /// Joins every thread still joinable. Idempotent, so an explicit call
    /// followed by the destructor's call is safe.
    void join_all() noexcept {
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return _threads.size(); }

private:
    std::vector<std::thread> _threads;
};

}  // namespace kythira::testing::coap
