// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The core record segment store (.kiro/specs/kinesis-compatible-streams/
// task 3, Requirements 11.1 and 11.5), held to the shared conformance suite
// twice: once shaped like Kinesis shards and once like DynamoDB's change
// log, which reuses the store (.kiro/specs/dynamodb-compatible-api/
// task 15). The cases after the suites cover what only this binary can:
// the barrier's fault point and coalescing.

#define BOOST_TEST_MODULE record_segment_store_test

#include <boost/test/unit_test.hpp>

#include "record_segment_store_conformance.hpp"

#include <raft/record_segment_store.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifdef FIU_ENABLE
#include <fiu-control.h>
#include <fiu.h>
#endif

namespace {

/// Bytes derived from `seq` alone, of a length that varies with it.
auto pattern(std::uint64_t seq, std::size_t base, std::size_t spread) -> std::vector<std::byte> {
    std::vector<std::byte> out(base + static_cast<std::size_t>((seq * 2654435761U) % spread));
    std::uint64_t x = seq * 0x9E3779B97F4A7C15ULL + 1;
    for (auto& b : out) {
        x ^= x << 13U;
        x ^= x >> 7U;
        x ^= x << 17U;
        b = static_cast<std::byte>(x & 0xFFU);
    }
    return out;
}

/// Kinesis: one stream per shard routing key, payloads of a few KiB.
struct kinesis_shard_harness {
    [[nodiscard]] auto stream(int i) const -> std::string {
        return "k/7f0c2a9e-41d6-4c4e-9a54-1d2b3c4d5e6f/" + std::to_string(i);
    }
    [[nodiscard]] auto payload(std::uint64_t seq) const -> std::vector<std::byte> {
        return pattern(seq, 256, 4096);
    }
    void tweak(kythira::record_segment_store_options& /*o*/) const {}
};

/// DynamoDB change log: one stream per group, small mutation records and
/// keys with bytes that need escaping.
struct change_log_harness {
    [[nodiscard]] auto stream(int i) const -> std::string {
        return std::string("\0meta/ddb/changes/orders table/", 31) + std::to_string(i);
    }
    [[nodiscard]] auto payload(std::uint64_t seq) const -> std::vector<std::byte> {
        return pattern(seq, 16, 400);
    }
    void tweak(kythira::record_segment_store_options& o) const {
        o._max_segment_bytes = 16 * 1024;
        o._index_interval_bytes = 512;
    }
};

}  // namespace

KYTHIRA_RECORD_SEGMENT_CONFORMANCE(kinesis_shard_harness, kinesis)
KYTHIRA_RECORD_SEGMENT_CONFORMANCE(change_log_harness, change_log)

BOOST_AUTO_TEST_SUITE(record_segment_store_only)

BOOST_AUTO_TEST_CASE(a_record_larger_than_a_segment_gets_one_to_itself) {
    kythira::segment_conformance::scratch_dir d;
    kythira::record_segment_store_options o;
    o._data_dir = d.path();
    o._max_segment_bytes = 1024;
    o._max_record_bytes = 8192;
    kythira::record_segment_store store(o);
    const std::vector<std::byte> small(100, std::byte{1});
    const std::vector<std::byte> big(4000, std::byte{2});
    store.append("s", kythira::segment_record_view{1, 0, small});
    auto r = store.append("s", kythira::segment_record_view{2, 0, big});
    BOOST_REQUIRE_EQUAL(r._sealed.size(), 1U);
    r = store.append("s", kythira::segment_record_view{3, 0, small});
    BOOST_REQUIRE_EQUAL(r._sealed.size(), 1U);
    BOOST_TEST(r._sealed[0]._first_seq == 2U);
    BOOST_TEST(r._sealed[0]._records == 1U);
    const std::vector<std::byte> too_big(8193, std::byte{3});
    BOOST_CHECK_THROW(store.append("s", kythira::segment_record_view{4, 0, too_big}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(concurrent_barriers_cover_every_append_before_them) {
    kythira::segment_conformance::scratch_dir d;
    kythira::record_segment_store_options o;
    o._data_dir = d.path();
    kythira::record_segment_store store(o);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&store, t] {
            const std::vector<std::byte> p(64, std::byte{static_cast<unsigned char>(t)});
            for (std::uint64_t i = 1; i <= 50; ++i) {
                store.append("s" + std::to_string(t), kythira::segment_record_view{i, 0, p});
                store.barrier();
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    // Coalescing: fewer rounds of fsyncs than barrier calls is allowed,
    // more is not.
    BOOST_TEST(store.stats()._barriers <= 200U);
    BOOST_TEST(store.stats()._barriers >= 1U);
    for (int t = 0; t < 4; ++t) {
        BOOST_TEST(store.last_seq("s" + std::to_string(t)) == std::optional<std::uint64_t>(50));
    }
}

#ifdef FIU_ENABLE
BOOST_AUTO_TEST_CASE(a_failed_barrier_throws_and_the_next_one_retries) {
    kythira::segment_conformance::scratch_dir d;
    kythira::record_segment_store_options o;
    o._data_dir = d.path();
    kythira::record_segment_store store(o);
    const std::vector<std::byte> p(64, std::byte{9});
    store.append("s", kythira::segment_record_view{1, 0, p});
    fiu_init(0);
    fiu_enable("raft/segments/barrier", 1, nullptr, 0);
    BOOST_CHECK_THROW(store.barrier(), kythira::record_segment_error);
    fiu_disable("raft/segments/barrier");
    const auto before = store.stats()._barriers;
    BOOST_CHECK_NO_THROW(store.barrier());
    BOOST_TEST(store.stats()._barriers == before + 1);
}
#endif

BOOST_AUTO_TEST_CASE(a_disabled_barrier_is_a_no_op) {
    kythira::segment_conformance::scratch_dir d;
    kythira::record_segment_store_options o;
    o._data_dir = d.path();
    o._barriers_enabled = false;
    kythira::record_segment_store store(o);
    const std::vector<std::byte> p(64, std::byte{9});
    store.append("s", kythira::segment_record_view{1, 0, p});
    store.barrier();
    BOOST_TEST(store.stats()._barriers == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
