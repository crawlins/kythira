// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file coap_exchange_table_test.cpp
/// @brief CoAP duplicate detection keyed on (peer endpoint, Message ID, token)
///        (.kiro/specs/coap-transport-multi-raft/ Requirement 5, tasks 3-4).
///
/// Drives coap_exchange_table directly with an injected clock, so the
/// properties that depend on time — retention for exactly EXCHANGE_LIFETIME,
/// a counter wrapping inside it, the memory held at multi-Raft rates — are
/// checked in milliseconds rather than by waiting four minutes. The same table
/// backs the libcoap client and server and the cantcoap backend; the wire-level
/// regression is coap_duplicate_detection_peer_test.cpp.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_exchange_table_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_exchange_table.hpp>

#include <malloc.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using clock_type = kythira::coap_exchange_table::clock;
using namespace std::chrono_literals;

// Two peers as a three-node cluster's server sees them.
constexpr const char* peer_a = "127.0.0.1:41001";
constexpr const char* peer_b = "127.0.0.1:41002";

// One heartbeat per 50 ms to each of two followers is 40 Message IDs per second
// per group, the rate design §5 of the specification derives its numbers from.
constexpr std::uint32_t messages_per_second_per_group = 40;

auto token_for(std::uint64_t n) -> std::string {
    // Shaped like coap_client's sequential tokens: distinct per request,
    // repeated only by a retransmission of that request.
    return "t" + std::to_string(n);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_exchange_table_tests)

BOOST_AUTO_TEST_CASE(exchange_lifetime_is_rfc_7252s_value,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // MAX_TRANSMIT_SPAN 45 + 2 * MAX_LATENCY 100 + PROCESSING_DELAY 2.
    static_assert(kythira::coap_exchange_lifetime == std::chrono::seconds{247});
    kythira::coap_exchange_table table;
    BOOST_TEST(table.lifetime() == clock_type::duration{kythira::coap_exchange_lifetime});
}

// The single-peer behaviour coap_duplicate_detection_property_test asserts
// through the transport: a retransmission is recognised, once and for as long
// as RFC 7252 says a sender may retransmit.
BOOST_AUTO_TEST_CASE(single_peer_retransmission_is_a_duplicate,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();

    BOOST_TEST(!table.check_and_record(peer_a, 7, "tok", t0));
    BOOST_TEST(table.check_and_record(peer_a, 7, "tok", t0 + 2s));
    BOOST_TEST(table.check_and_record(peer_a, 7, "tok", t0 + 246s));
    BOOST_TEST(table.size() == 1u);
}

BOOST_AUTO_TEST_CASE(retention_ends_at_exchange_lifetime,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();
    table.record(peer_a, 7, "tok", t0);

    BOOST_TEST(table.is_duplicate(peer_a, 7, "tok", t0 + kythira::coap_exchange_lifetime - 1ms));
    BOOST_TEST(!table.is_duplicate(peer_a, 7, "tok", t0 + kythira::coap_exchange_lifetime));

    table.sweep(t0 + kythira::coap_exchange_lifetime);
    BOOST_TEST(table.size() == 0u);
    BOOST_TEST(table.peer_count() == 0u);
}

// Requirement 5.5. Every peer numbers its own messages, so two peers' counters
// meet; under the old bare-Message-ID key the second peer's request was
// answered 2.03 and never delivered.
BOOST_AUTO_TEST_CASE(two_peers_with_coinciding_message_ids_are_both_delivered,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();

    for (std::uint16_t mid = 1; mid <= 100; ++mid) {
        BOOST_TEST(!table.check_and_record(peer_a, mid, token_for(mid), t0));
        BOOST_TEST(!table.check_and_record(peer_b, mid, token_for(mid), t0));
    }
    BOOST_TEST(table.size() == 200u);
    BOOST_TEST(table.peer_count() == 2u);

    // And each peer's genuine retransmission is still caught.
    BOOST_TEST(table.check_and_record(peer_a, 1, token_for(1), t0 + 1s));
    BOOST_TEST(table.check_and_record(peer_b, 1, token_for(1), t0 + 1s));
}

// Requirement 5.6. A process leading many groups laps the 16-bit Message ID
// space inside EXCHANGE_LIFETIME; the new request on a wrapped counter carries
// a new token, and must be delivered.
BOOST_AUTO_TEST_CASE(a_counter_wrapping_inside_the_horizon_discards_no_live_message,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();

    // 2.5 laps of the Message ID space from one peer in 30 s: far inside the
    // 247 s horizon, which is what 64 groups at 40 msg/s/group produce.
    constexpr std::uint64_t total = 65536ull * 5 / 2;
    std::size_t dropped = 0;
    for (std::uint64_t n = 0; n < total; ++n) {
        const auto now = t0 + std::chrono::microseconds{n * 30'000'000 / total};
        if (table.check_and_record(peer_a, static_cast<std::uint16_t>(n), token_for(n), now)) {
            ++dropped;
        }
    }
    BOOST_TEST(dropped == 0u);

    // The slot now holds the newest exchange for each Message ID, so a
    // retransmission of the latest request is still recognised...
    const auto last = total - 1;
    BOOST_TEST(
        table.is_duplicate(peer_a, static_cast<std::uint16_t>(last), token_for(last), t0 + 30s));
    // ...and a stale token for that Message ID is not mistaken for it.
    BOOST_TEST(!table.is_duplicate(peer_a, static_cast<std::uint16_t>(last),
                                   token_for(last - 65536), t0 + 30s));
}

// Strictly narrowing (Requirement 5.2): every component must match.
BOOST_AUTO_TEST_CASE(a_duplicate_requires_peer_message_id_and_token_to_match,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();
    table.record(peer_a, 42, "tok", t0);

    BOOST_TEST(table.is_duplicate(peer_a, 42, "tok", t0));
    BOOST_TEST(!table.is_duplicate(peer_b, 42, "tok", t0));
    BOOST_TEST(!table.is_duplicate(peer_a, 43, "tok", t0));
    BOOST_TEST(!table.is_duplicate(peer_a, 42, "other", t0));
}

// Task 4: the memory the table holds at multi-Raft rates. One peer sending at
// 40N Message IDs per second for longer than EXCHANGE_LIFETIME, for N in
// {1, 8, 64}. The table is bounded by the Message ID space per peer, so past
// the point where a peer laps the space (N >= 7 at this rate) the footprint
// stops growing with N.
BOOST_AUTO_TEST_CASE(memory_held_at_multi_raft_rates_is_bounded_per_peer,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    struct row {
        std::uint32_t groups;
        std::size_t entries;
        std::size_t bytes;
    };
    std::vector<row> rows;

    for (const std::uint32_t groups : {1u, 8u, 64u}) {
        const auto before = ::mallinfo2().uordblks;
        {
            kythira::coap_exchange_table table;
            const auto t0 = clock_type::now();
            const std::uint64_t rate = messages_per_second_per_group * groups;
            // One and a quarter lifetimes, so the table reaches steady state
            // and has swept at least once.
            const std::uint64_t seconds = 309;
            const std::uint64_t total = rate * seconds;
            for (std::uint64_t n = 0; n < total; ++n) {
                const auto now = t0 + std::chrono::microseconds{n * 1'000'000 / rate};
                table.record(peer_a, static_cast<std::uint16_t>(n), token_for(n), now);
            }
            table.sweep(t0 + std::chrono::seconds{seconds});

            const auto held = ::mallinfo2().uordblks - before;
            rows.push_back({groups, table.size(), held});

            // Never more than one entry per Message ID per peer...
            BOOST_TEST(table.size() <= 65536u);
            // ...and never fewer than the exchanges still inside the horizon,
            // capped by that same space.
            const auto live = std::min<std::uint64_t>(
                rate * static_cast<std::uint64_t>(kythira::coap_exchange_lifetime.count()), 65536);
            BOOST_TEST(table.size() >= live - rate);
        }
    }

    for (const auto& r : rows) {
        BOOST_TEST_MESSAGE("groups=" << r.groups << " entries=" << r.entries
                                     << " heap_bytes=" << r.bytes);
    }
    // The point of the bound: 64 groups cost what 8 do, not eight times more.
    BOOST_TEST(rows[2].entries == rows[1].entries);
}

BOOST_AUTO_TEST_SUITE_END()
