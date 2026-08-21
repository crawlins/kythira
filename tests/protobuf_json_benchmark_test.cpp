// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE ProtobufJsonBenchmarkTest
#include <boost/test/unit_test.hpp>

#include <raft/json_serializer.hpp>
#include <raft/protobuf_serializer.hpp>

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <random>
#include <sstream>
#include <vector>

// Benchmark comparing protobuf_rpc_serializer against json_rpc_serializer for
// representative append_entries_request payloads, varying the entry count and
// per-entry command size (Requirement 8.1). Reports serialized payload size and
// serialize/deserialize latency; the recorded numbers live in
// doc/protobuf_serializer_performance_comparison.md (Requirement 8.2).
//
// This is a characterization benchmark, not a correctness gate: it asserts only
// that both serializers round-trip, and prints a comparison table via
// BOOST_TEST_MESSAGE.

namespace {
constexpr std::size_t bench_iterations = 2000;

// Prevents the optimizer from eliding the serialize/deserialize work.
volatile std::size_t g_sink = 0;
void benchmark_sink(std::size_t v) {
    g_sink += v;
}

auto make_command(std::mt19937& rng, std::size_t size) -> std::vector<std::byte> {
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::vector<std::byte> cmd;
    cmd.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        cmd.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return cmd;
}

auto make_request(std::mt19937& rng, std::size_t entry_count, std::size_t command_size)
    -> kythira::append_entries_request<> {
    kythira::append_entries_request<> req;
    req._term = 42;
    req._leader_id = 7;
    req._prev_log_index = 1000;
    req._prev_log_term = 41;
    req._leader_commit = 999;
    req._entries.reserve(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
        kythira::log_entry<> e;
        e._term = 42;
        e._index = 1001 + i;
        e._command = make_command(rng, command_size);
        e._type = kythira::entry_type::normal;
        req._entries.push_back(e);
    }
    return req;
}

template<typename Serializer> struct bench_result {
    std::size_t payload_size = 0;
    double serialize_us = 0.0;
    double deserialize_us = 0.0;
};

template<typename Serializer>
auto run_bench(const Serializer& s, const kythira::append_entries_request<>& req)
    -> bench_result<Serializer> {
    using clock = std::chrono::steady_clock;
    bench_result<Serializer> r;

    auto sample = s.serialize(req);
    r.payload_size = sample.size();

    auto t0 = clock::now();
    for (std::size_t i = 0; i < bench_iterations; ++i) {
        auto data = s.serialize(req);
        benchmark_sink(data.size());
    }
    auto t1 = clock::now();
    for (std::size_t i = 0; i < bench_iterations; ++i) {
        auto back = s.deserialize_append_entries_request(sample);
        benchmark_sink(back.entries().size());
    }
    auto t2 = clock::now();

    r.serialize_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / bench_iterations;
    r.deserialize_us =
        std::chrono::duration<double, std::micro>(t2 - t1).count() / bench_iterations;
    return r;
}
}

BOOST_AUTO_TEST_CASE(benchmark_protobuf_vs_json_append_entries, *boost::unit_test::timeout(300)) {
    std::mt19937 rng(12345);
    kythira::json_rpc_serializer<std::vector<std::byte>> json;
    kythira::protobuf_rpc_serializer<std::vector<std::byte>> proto;

    struct scenario {
        std::size_t entries;
        std::size_t command_size;
    };
    const std::vector<scenario> scenarios = {
        {0, 0}, {1, 16}, {1, 256}, {8, 64}, {8, 512}, {64, 64}, {64, 512},
    };

    std::ostringstream table;
    table << "\n";
    table << "entries  cmd_bytes | json_size proto_size size_ratio | "
             "json_ser proto_ser | json_des proto_des (us/op)\n";
    table << "----------------------------------------------------------"
             "---------------------------------------\n";

    for (const auto& sc : scenarios) {
        auto req = make_request(rng, sc.entries, sc.command_size);
        auto jr = run_bench(json, req);
        auto pr = run_bench(proto, req);

        // Both must round-trip identically for the comparison to be meaningful.
        BOOST_TEST(json.deserialize_append_entries_request(json.serialize(req)).entries().size() ==
                   sc.entries);
        BOOST_TEST(
            proto.deserialize_append_entries_request(proto.serialize(req)).entries().size() ==
            sc.entries);

        const double size_ratio =
            jr.payload_size == 0 ? 1.0 : static_cast<double>(pr.payload_size) / jr.payload_size;

        table << std::setw(7) << sc.entries << "  " << std::setw(9) << sc.command_size << " | "
              << std::setw(9) << jr.payload_size << " " << std::setw(10) << pr.payload_size << " "
              << std::setw(10) << std::fixed << std::setprecision(3) << size_ratio << " | "
              << std::setw(8) << std::setprecision(3) << jr.serialize_us << " " << std::setw(9)
              << pr.serialize_us << " | " << std::setw(8) << jr.deserialize_us << " "
              << std::setw(9) << pr.deserialize_us << "\n";
    }

    BOOST_TEST_MESSAGE(table.str());
    BOOST_TEST_MESSAGE("g_sink=" << g_sink);  // keep the sink observable
}
