// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The workload's worker loop: `concurrency` threads, each a generator and a
// client, recording invoke and completion events until told to stop. Shared
// by the chaos_workload binary, which stops at a deadline, and the
// docker_chaos fixture, which stops when the scenario ends.

#include "workload.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace chaos_workload {

struct run_options {
    unsigned keys{3};
    unsigned values{5};
    unsigned concurrency{4};
    std::uint64_t rate{50};  // total ops/second, 0 = unpaced
    mix weights{};
    std::uint64_t seed{0};  // worker i uses seed + i
    std::string key_prefix{"k"};
};

/// Runs workers until `should_stop` returns true, then joins them. `emit` is
/// called from the worker threads and must be thread-safe. Event times are
/// nanoseconds since `start`. Returns the number of process ids used.
///
/// A worker checks `should_stop` before each invoke, never between an
/// invoke and its completion, so every invoke it emits is completed.
inline auto run(const std::map<int, std::string>& nodes,
                const std::function<transport()>& make_transport, const run_options& opts,
                const std::function<bool()>& should_stop,
                const std::function<void(const event&)>& emit,
                std::chrono::steady_clock::time_point start) -> std::int64_t {
    using clock = std::chrono::steady_clock;
    if (opts.concurrency == 0) {
        throw std::invalid_argument("workload: concurrency must be at least 1");
    }
    if (nodes.empty()) {
        throw std::invalid_argument("workload: no nodes");
    }
    auto now_ns = [start] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
    };
    // Each worker runs its share of the total rate.
    const auto gap =
        opts.rate == 0
            ? clock::duration::zero()
            : std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                  static_cast<double>(opts.concurrency) / static_cast<double>(opts.rate)));
    std::atomic<std::int64_t> next_process{0};

    std::vector<std::thread> workers;
    workers.reserve(opts.concurrency);
    for (unsigned w = 0; w < opts.concurrency; ++w) {
        workers.emplace_back([&, w] {
            generator gen(opts.seed + w, opts.keys, opts.values, opts.weights, opts.key_prefix);
            client cli(nodes, make_transport());
            // Local reads stay on one replica per worker, spread across the
            // nodes, so a stale read names the replica that served it.
            auto it = nodes.begin();
            std::advance(it, static_cast<long>(w % nodes.size()));
            const int local_node = it->first;

            std::int64_t process = next_process++;
            auto next_at = clock::now();
            while (!should_stop()) {
                auto op = gen.next();
                event invoke;
                invoke.process = process;
                invoke.type = event_type::invoke;
                invoke.f = op.f;
                invoke.key = op.key;
                invoke.value = op.f == op_kind::read ? std::nullopt : op.value;
                invoke.expected = op.expected;
                if (op.f == op_kind::read) {
                    invoke.mode = op.local_read ? "local" : "linearizable";
                }
                invoke.time_ns = now_ns();
                emit(invoke);

                auto done = cli.run(op, local_node);
                done.process = process;
                done.time_ns = now_ns();
                emit(done);
                if (done.type == event_type::info) {
                    // Retire: its operation may still complete, so this
                    // process id never invokes again (WGL needs that).
                    process = next_process++;
                }
                if (gap != clock::duration::zero()) {
                    next_at += gap;
                    std::this_thread::sleep_until(next_at);
                }
            }
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    return next_process.load();
}

}  // namespace chaos_workload
