// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file object_persistence_real_support.hpp
/// @brief The real-tier scaffolding every provider's object-persistence suite
///        shares: skip-correctness, teardown, latency measurement, and the four
///        load-bearing cases themselves (cloud-object-persistence task 16).
///
/// ## Why the cases live here and not in each suite
///
/// Task 16 asks for the same four cases against five providers. Written out
/// per provider that is five copies of the same reasoning, and the copies drift
/// — the fencing case in particular is subtle enough that five independent
/// versions would not stay equivalent, and "provider X passes" would stop
/// meaning the same thing as "provider Y passes". The whole design of this
/// spec is one engine over a concept; its real tier should be one suite over
/// the same concept.
///
/// So each provider's file is thin: build a client from the environment, name
/// the bucket, and call these. What differs between providers is
/// authentication and a bucket name, which is exactly what stays in the
/// per-provider file.
///
/// ## Skip-correctness, and why a failed pre-flight skips rather than fails
///
/// These suites are **never CTest-registered** — `ctest -N` must not list them,
/// because a listed test eventually runs, and these cost money and need
/// credentials. They are run by name, by the real-cloud CI job.
///
/// Run without configuration they exit **77** (CTest's skip code) after naming
/// every missing value, so an operator sees what to set rather than a failure
/// they have to decode. The read-only pre-flight — one LIST against the target
/// bucket — also skips rather than fails: a suite that cannot reach the service
/// has learned nothing about the code, and reporting that as a defect would
/// train people to ignore red.
///
/// The distinction that matters: **a reachable service that behaves wrongly
/// fails.** Only "I could not run" skips.

#include <raft/key_object_store.hpp>
#include <raft/object_store_backup.hpp>
#include <raft/object_store_persistence.hpp>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <time.h>

namespace kythira::object_real {

// ── Environment and skip-correctness ─────────────────────────────────────────

[[nodiscard]] inline auto env(const char* name) -> std::string {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

/// One required setting: the variable an operator must set, and what it
/// currently holds.
struct required_value {
    const char* variable;
    std::string value;
};

/// Exit 77 naming **every** missing value, not just the first.
///
/// Naming only the first would make configuring these suites an iterative
/// guessing game — set one, re-run, discover the next. An operator running this
/// for the first time should learn the whole list in one go.
inline auto skip_unless_configured(const std::vector<required_value>& required) -> void {
    std::vector<const char*> missing;
    for (const auto& one : required) {
        if (one.value.empty()) {
            missing.push_back(one.variable);
        }
    }
    if (missing.empty()) {
        return;
    }
    for (const auto* variable : missing) {
        std::cout << "SKIP: " << variable << " is not set\n";
    }
    std::cout << "SKIP: " << missing.size() << " required value(s) missing — not run\n";
    std::cout.flush();
    std::exit(77);
}

/// A read-only pre-flight: one LIST against the bucket the suite will use.
///
/// Skips rather than fails, because an unreachable service says nothing about
/// the code under test. Read-only on purpose — a pre-flight that wrote would
/// itself be the first thing to break against a misconfigured bucket, and would
/// leave residue behind when it did.
template<key_object_store Store>
auto skip_unless_reachable(const Store& store, const std::string& bucket,
                           const std::string& probe_prefix) -> void {
    try {
        (void)store.list_keys(bucket, probe_prefix);
    } catch (const std::exception& err) {
        std::cout << "SKIP: pre-flight LIST of " << bucket << "/" << probe_prefix
                  << " failed, so the service is not reachable with this configuration: "
                  << err.what() << "\n";
        std::cout << "SKIP: not run\n";
        std::cout.flush();
        std::exit(77);
    }
}

// ── Teardown, including on a signal ──────────────────────────────────────────

using teardown_fn = std::function<void()>;

inline auto teardown_registry() -> std::vector<teardown_fn>& {
    static std::vector<teardown_fn> registry;
    return registry;
}

/// Runs every registered teardown exactly once, whichever path gets here first.
///
/// The `exchange` matters: normal exit and a signal can both reach this, and
/// deleting the same objects twice would report spurious failures from the
/// second pass.
inline auto run_teardown_once() -> void {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    for (auto& fn : teardown_registry()) {
        try {
            fn();
        } catch (const std::exception& err) {
            std::cout << "teardown: " << err.what() << "\n";
        }
    }
    teardown_registry().clear();
    // Explicit, because the signal path exits without unwinding and a buffered
    // report would be lost exactly when it is most wanted.
    std::cout.flush();
}

/// @brief Deletes this run's objects on normal exit **and** on SIGINT/SIGTERM.
///
/// ## Why a thread and not a signal handler
///
/// The first version installed `run_teardown` as a `std::signal` handler. That
/// is undefined behaviour and it bit: teardown allocates, does TLS network I/O
/// and writes to `std::cout`, none of which is async-signal-safe. A SIGTERM
/// arriving mid-request — the overwhelmingly likely moment, since that is where
/// these suites spend their time — lands while the main thread holds the
/// allocator lock, and the handler deadlocks trying to take it again. Observed
/// exactly that: a run killed during teardown produced no teardown output, no
/// deletions, and **48 objects left in a shared CI bucket**.
///
/// The fix is the standard one: **block the signals and receive them in a
/// dedicated thread**. `sigwait` returns in ordinary thread context, where
/// allocation, sockets and iostreams are all legal, so teardown is the same
/// code on both paths rather than a restricted imitation of it.
///
/// It also fixes a second, quieter problem the handler had: the main thread is
/// usually blocked in a socket read when the signal arrives, so a
/// flag-and-poll design would not notice it until that read returned — which,
/// for a hung request, is never. A separate thread is not an optimisation here;
/// it is the only structure that can act while the main thread is stuck.
struct signal_teardown_fixture {
    signal_teardown_fixture() {
        sigemptyset(&_blocked);
        sigaddset(&_blocked, SIGINT);
        sigaddset(&_blocked, SIGTERM);
        // Blocked process-wide *before* the watcher starts, so no other thread
        // can take the signal and run the default disposition instead.
        pthread_sigmask(SIG_BLOCK, &_blocked, nullptr);

        _watcher = std::thread([this] {
            while (!_stop.load(std::memory_order_relaxed)) {
                // Timed, not `sigwait`, so the destructor can retire this
                // thread without inventing a wake-up signal of its own.
                const timespec timeout{0, 100 * 1000 * 1000};
                siginfo_t info{};
                const int sig = sigtimedwait(&_blocked, &info, &timeout);
                if (sig < 0) {
                    continue;  // EAGAIN: nothing arrived in this slice.
                }
                std::cout << "\nreceived signal " << sig
                          << " — running teardown before exit so this run leaves nothing"
                             " behind\n";
                std::cout.flush();
                run_teardown_once();
                // `_Exit`, not `raise`: unwinding from here would race the main
                // thread, which is still inside whatever request it was making.
                // Teardown is complete and its output is flushed, which is the
                // only thing that had to happen.
                std::_Exit(128 + sig);
            }
        });
    }

    ~signal_teardown_fixture() {
        run_teardown_once();
        _stop.store(true, std::memory_order_relaxed);
        if (_watcher.joinable()) {
            _watcher.join();
        }
        pthread_sigmask(SIG_UNBLOCK, &_blocked, nullptr);
    }

    signal_teardown_fixture(const signal_teardown_fixture&) = delete;
    auto operator=(const signal_teardown_fixture&) -> signal_teardown_fixture& = delete;
    signal_teardown_fixture(signal_teardown_fixture&&) = delete;
    auto operator=(signal_teardown_fixture&&) -> signal_teardown_fixture& = delete;

private:
    sigset_t _blocked{};
    std::atomic<bool> _stop{false};
    std::thread _watcher;
};

/// Every object this run writes lives under one prefix, so teardown is a
/// prefixed LIST and delete rather than bookkeeping the suite has to keep
/// accurate.
[[nodiscard]] inline auto unique_prefix() -> std::string {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return "kythira-real-test/obj-" +
           std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

/// Sweeps @p prefix until a LIST comes back empty, or @p max_rounds is spent.
///
/// **The loop is not belt-and-braces; a single pass provably leaks.** On the
/// signal path the main thread is still running — it is mid-suite, which is why
/// someone killed it — so it keeps writing objects while the watcher deletes
/// them. Measured with a real SIGTERM during `list_after_write`: one pass
/// deleted 58 objects and left **17**, all created after that pass's LIST.
///
/// Each round is a LIST plus deletes and the writer is bounded by round-trip
/// latency, so this converges in two or three rounds and then stops. It is
/// bounded rather than unbounded because a teardown that could hang forever is
/// worse than one that reports what it could not remove.
template<key_object_store Store>
auto register_prefix_teardown(Store store, std::string bucket, std::string prefix,
                              int max_rounds = 5) -> void {
    teardown_registry().emplace_back([store = std::move(store), bucket = std::move(bucket),
                                      prefix = std::move(prefix), max_rounds] {
        std::size_t deleted = 0;
        std::size_t residual = 0;
        int rounds = 0;
        for (; rounds < max_rounds; ++rounds) {
            const auto keys = store.list_keys(bucket, prefix);
            if (keys.empty()) {
                break;
            }
            for (const auto& key : keys) {
                store.delete_object(bucket, key);
                ++deleted;
            }
        }
        // Counted from a fresh LIST, not from the deletions above: the residual
        // figure has to come from the service, or it is only the suite agreeing
        // with itself.
        residual = store.list_keys(bucket, prefix).size();
        std::cout << "teardown: deleted " << deleted << " object(s) under " << prefix << " in "
                  << rounds << " round(s); residual objects counted from a listing: " << residual
                  << "\n";
        if (residual != 0) {
            // Named loudly. A shared CI bucket accumulating residue is exactly
            // the condition that makes a future leak undiagnosable.
            std::cout << "teardown: WARNING — " << residual << " object(s) remain under " << prefix
                      << "; sweep them before trusting the next run's residual count\n";
        }
        std::cout.flush();
    });
}

// ── Latency measurement ──────────────────────────────────────────────────────

/// Percentile latencies for one operation, in milliseconds.
struct latency_report {
    std::string operation;
    std::size_t samples{0};
    double p50_ms{0};
    double p99_ms{0};
};

/// @brief Times @p op @p samples times and reports p50/p99.
///
/// **Where the measurement is taken is recorded in the output**, because it is
/// the first thing that makes two latency numbers incomparable. These are
/// measured **client-side, around the engine call** — so they include TLS,
/// request signing, the round trip and the engine's own bookkeeping, which is
/// what a Raft node actually waits for. They are *not* service-side latencies
/// and must not be compared against a provider's published figures.
/// @p min_interval spaces successive samples. It exists because **GCS
/// rate-limits mutations to roughly one per second per OBJECT** — measured, not
/// read: sampling `save_current_term` 20 times in a tight loop against a real
/// bucket returned `429 rateLimitExceeded` ("exceeded the rate limit for object
/// mutation operations"). Every single-slot key the engine owns (`term`,
/// `voted_for`, `snapshot`, `owner`) is subject to it. Without spacing, this
/// case would measure a provider's throttle rather than its latency, and would
/// fail on GCS for a reason that says nothing about the code.
template<typename Op>
[[nodiscard]] auto measure(const std::string& operation, std::size_t samples, Op&& op,
                           std::chrono::milliseconds min_interval = std::chrono::milliseconds{0})
    -> latency_report {
    std::vector<double> timings;
    timings.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        if (i != 0 && min_interval.count() > 0) {
            std::this_thread::sleep_for(min_interval);
        }
        const auto start = std::chrono::steady_clock::now();
        op(i);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        timings.push_back(
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(elapsed).count());
    }
    std::sort(timings.begin(), timings.end());

    latency_report report;
    report.operation = operation;
    report.samples = timings.size();
    if (timings.empty()) {
        return report;
    }
    const auto pick = [&timings](double q) {
        // Nearest-rank, clamped. With the small sample counts a paid real-tier
        // run can justify, an interpolating percentile would invent precision
        // the data does not carry.
        const auto rank = static_cast<std::size_t>(q * static_cast<double>(timings.size()));
        return timings[std::min(rank, timings.size() - 1)];
    };
    report.p50_ms = pick(0.50);
    report.p99_ms = pick(0.99);
    return report;
}

/// A fixed, greppable line so runs and providers are comparable, and so a CI
/// job can extract them without parsing Boost.Test output.
///
///     KYTHIRA_LATENCY provider=s3 op=append_log_entry samples=20 p50_ms=41.2 p99_ms=93.7
///     measured=client-side-around-engine-call
inline auto report_latency(const std::string& provider, const latency_report& report) -> void {
    std::cout << "KYTHIRA_LATENCY provider=" << provider << " op=" << report.operation
              << " samples=" << report.samples << " p50_ms=" << report.p50_ms
              << " p99_ms=" << report.p99_ms << " measured=client-side-around-engine-call\n";
    std::cout.flush();
}

}  // namespace kythira::object_real
