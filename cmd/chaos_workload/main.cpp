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
//   WORKLOAD_DURATION_S   seconds to run                   (default 10)
//   WORKLOAD_MIX          "read=3,local=1,write=4,cas=2"   (default)
//   WORKLOAD_SEED         RNG seed; worker i uses seed+i   (default: time)
//   WORKLOAD_TIMEOUT_MS   per-request read timeout          (default 3000)
//
// History lines go to stdout and nothing else does; diagnostics go to
// stderr. Exit status is 0 once the duration has passed, 2 on bad
// configuration.

#include "workload.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

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

// One request over cpp-httplib. A refused connection is the only failure
// reported as not sent: anything after connect may have reached the server.
auto http_transport(std::chrono::milliseconds read_timeout) -> chaos_workload::transport {
    return [read_timeout](const std::string& address, const std::string& path,
                          const std::string& body) -> chaos_workload::response {
        auto colon = address.rfind(':');
        httplib::Client cli(address.substr(0, colon), std::stoi(address.substr(colon + 1)));
        cli.set_connection_timeout(std::chrono::seconds{1});
        cli.set_read_timeout(read_timeout);
        cli.set_write_timeout(read_timeout);
        auto res = body.empty() ? cli.Get(path) : cli.Post(path, body, "application/json");
        if (!res) {
            const auto err = res.error();
            return chaos_workload::response{.status = 0,
                                            .body = {},
                                            .sent = err != httplib::Error::Connection,
                                            .error = httplib::to_string(err)};
        }
        return chaos_workload::response{
            .status = res->status, .body = res->body, .sent = true, .error = {}};
    };
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
        if (concurrency == 0) {
            throw std::invalid_argument("WORKLOAD_CONCURRENCY must be at least 1");
        }
        std::cerr << "[chaos_workload] nodes=" << nodes.size() << " keys=" << keys
                  << " concurrency=" << concurrency << " rate=" << rate
                  << " duration_s=" << duration.count() << " seed=" << seed << "\n";

        const auto start = clock::now();
        const auto deadline = start + duration;
        auto now_ns = [start] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start)
                .count();
        };
        // Each worker runs its share of the total rate.
        const auto gap =
            rate == 0 ? clock::duration::zero()
                      : std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(
                            static_cast<double>(concurrency) / static_cast<double>(rate)));

        std::mutex out_mutex;
        auto emit = [&out_mutex](const chaos_workload::event& e) {
            auto line = docker_chaos::history::to_line(e);
            std::lock_guard<std::mutex> lock(out_mutex);
            std::cout << line << '\n' << std::flush;
        };
        std::atomic<std::int64_t> next_process{0};

        std::vector<std::thread> workers;
        workers.reserve(concurrency);
        for (unsigned w = 0; w < concurrency; ++w) {
            workers.emplace_back([&, w] {
                chaos_workload::generator gen(seed + w, keys, values, weights);
                chaos_workload::client cli(nodes, http_transport(timeout));
                // Local reads stay on one replica per worker, spread across
                // the nodes, so a stale read names the replica that served it.
                auto it = nodes.begin();
                std::advance(it, static_cast<long>(w % nodes.size()));
                const int local_node = it->first;

                std::int64_t process = next_process++;
                auto next_at = clock::now();
                while (clock::now() < deadline) {
                    auto op = gen.next();
                    chaos_workload::event invoke;
                    invoke.process = process;
                    invoke.type = chaos_workload::event_type::invoke;
                    invoke.f = op.f;
                    invoke.key = op.key;
                    invoke.value = op.f == chaos_workload::op_kind::read ? std::nullopt : op.value;
                    invoke.expected = op.expected;
                    if (op.f == chaos_workload::op_kind::read) {
                        invoke.mode = op.local_read ? "local" : "linearizable";
                    }
                    invoke.time_ns = now_ns();
                    emit(invoke);

                    auto done = cli.run(op, local_node);
                    done.process = process;
                    done.time_ns = now_ns();
                    emit(done);
                    if (done.type == chaos_workload::event_type::info) {
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
        std::cerr << "[chaos_workload] done, " << next_process.load() << " processes used\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "chaos_workload: " << e.what() << "\n";
        return 2;
    }
}
