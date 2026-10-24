// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// chaos_workload: drives a chaos_node cluster with reads, writes and CAS
// and writes a JSONL history to stdout (.kiro/specs/cairn-chaos-testing/
// Requirement 4, design §2.2). Configured from the environment, because it
// runs as a compose service:
//
//   WORKLOAD_NODES        "1=c5node1:8080,2=c5node2:8080,..."  (required)
//   WORKLOAD_KEYS         keys k0..k<n-1>                  (default 3)
//   WORKLOAD_VALUES       values 0..<n-1>                  (default 5)
//   WORKLOAD_CONCURRENCY  worker threads                   (default 4)
//   WORKLOAD_RATE         total ops/second, 0 = unpaced    (default 50)
//   WORKLOAD_DURATION_S   seconds to run, 0 = until signalled (default 10)
//   WORKLOAD_MIX          "read=3,local=1,write=4,cas=2"   (default)
//   WORKLOAD_SEED         RNG seed; worker i uses seed+i   (default: time)
//   WORKLOAD_TIMEOUT_MS   per-request read timeout          (default 3000)
//
// History lines go to stdout and nothing else does; diagnostics go to
// stderr. SIGTERM or SIGINT stops it early, the same way the deadline does:
// each worker finishes the operation it has in flight, so the history never
// ends on an invoke the client gave up on (chaos_ctl's workload.stop relies
// on this). Exit status is 0 once the duration has passed or a stop signal
// arrived, 2 on bad configuration.

#include "http_transport.hpp"
#include "runner.hpp"
#include "workload.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void request_stop(int /*signal*/) {
    g_stop_requested.store(true);
}

auto env_or(const char* name, const std::string& fallback) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != 0) ? std::string(v) : fallback;
}

auto env_uint(const char* name, std::uint64_t fallback) -> std::uint64_t {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == 0) {
        return fallback;
    }
    return std::stoull(v);
}

}  // namespace

int main() {
    using clock = std::chrono::steady_clock;
    try {
        const auto nodes = chaos_workload::parse_nodes(env_or("WORKLOAD_NODES", ""));
        const auto keys = static_cast<unsigned>(env_uint("WORKLOAD_KEYS", 3));
        const auto values = static_cast<unsigned>(env_uint("WORKLOAD_VALUES", 5));
        const auto concurrency = static_cast<unsigned>(env_uint("WORKLOAD_CONCURRENCY", 4));
        const auto rate = env_uint("WORKLOAD_RATE", 50);
        const auto duration = std::chrono::seconds{env_uint("WORKLOAD_DURATION_S", 10)};
        const auto weights = chaos_workload::parse_mix(env_or("WORKLOAD_MIX", ""));
        const auto seed = env_uint(
            "WORKLOAD_SEED", static_cast<std::uint64_t>(clock::now().time_since_epoch().count()));
        const auto timeout = std::chrono::milliseconds{env_uint("WORKLOAD_TIMEOUT_MS", 3000)};
        std::cerr << "[chaos_workload] nodes=" << nodes.size() << " keys=" << keys
                  << " concurrency=" << concurrency << " rate=" << rate
                  << " duration_s=" << duration.count() << " seed=" << seed << "\n";

        std::signal(SIGTERM, request_stop);
        std::signal(SIGINT, request_stop);
        const auto start = clock::now();
        const auto deadline = start + duration;
        const bool forever = duration.count() == 0;
        std::mutex out_mutex;
        auto emit = [&out_mutex](const chaos_workload::event& e) {
            auto line = docker_chaos::history::to_line(e);
            std::lock_guard<std::mutex> lock(out_mutex);
            std::cout << line << '\n' << std::flush;
        };
        const auto processes = chaos_workload::run(
            nodes, [timeout] { return chaos_workload::http_transport(timeout); },
            chaos_workload::run_options{.keys = keys,
                                        .values = values,
                                        .concurrency = concurrency,
                                        .rate = rate,
                                        .weights = weights,
                                        .seed = seed,
                                        .key_prefix = "k"},
            [deadline, forever] {
                return g_stop_requested.load() || (!forever && clock::now() >= deadline);
            },
            emit, start);
        std::cerr << "[chaos_workload] done, " << processes << " processes used\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "chaos_workload: " << e.what() << "\n";
        return 2;
    }
}
