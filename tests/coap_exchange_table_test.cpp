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
///
/// The reply_* cases cover the optional reply storage the cantcoap server uses
/// to answer a retransmitted request (.kiro/specs/coap-cantcoap-duplicate-
/// replay/ Requirements 3, 4.2 and 5.7).

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

// ── Reply storage (.kiro/specs/coap-cantcoap-duplicate-replay/) ─────────────

namespace {

auto bytes_of(std::size_t size, std::uint8_t fill) -> std::vector<std::byte> {
    return std::vector<std::byte>(size, std::byte{fill});
}

auto caching_table(std::size_t budget,
                   clock_type::duration retention = kythira::coap_max_transmit_wait)
    -> kythira::coap_exchange_table {
    return kythira::coap_exchange_table{kythira::coap_exchange_lifetime,
                                        kythira::coap_reply_cache_limits{budget, retention}};
}

}  // namespace

BOOST_AUTO_TEST_CASE(reply_retention_defaults_to_max_transmit_wait,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // ACK_TIMEOUT 2 * (2^(MAX_RETRANSMIT 4 + 1) - 1) * ACK_RANDOM_FACTOR 1.5.
    static_assert(kythira::coap_max_transmit_wait == std::chrono::seconds{93});
    const kythira::coap_exchange_table table;
    BOOST_TEST(table.reply_limits().budget_bytes == 0U);
    BOOST_TEST(table.reply_limits().retention ==
               clock_type::duration{kythira::coap_max_transmit_wait});
}

BOOST_AUTO_TEST_CASE(reply_is_stored_and_replayed_for_the_same_exchange,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(1024);
    const auto t0 = clock_type::now();

    const auto first = table.classify(peer_a, 7, "tok", t0);
    BOOST_TEST((first.kind == kythira::coap_duplicate_kind::fresh));
    BOOST_TEST(!first.reply);

    // A copy before the reply exists (the request is still being handled) is
    // dropped: the reply the original produces answers both.
    BOOST_TEST(
        (table.classify(peer_a, 7, "tok", t0 + 1ms).kind == kythira::coap_duplicate_kind::drop));

    table.attach_reply(peer_a, 7, "tok", bytes_of(40, 0xAB), t0 + 2ms);
    BOOST_TEST(table.replies().count == 1U);
    BOOST_TEST(table.replies().bytes == 40U);

    const auto again = table.classify(peer_a, 7, "tok", t0 + 2s);
    BOOST_TEST((again.kind == kythira::coap_duplicate_kind::replay));
    BOOST_REQUIRE(again.reply);
    BOOST_TEST((*again.reply == bytes_of(40, 0xAB)));

    // Same Message ID from another peer, or another token: a new exchange.
    BOOST_TEST(
        (table.classify(peer_b, 7, "tok", t0 + 2s).kind == kythira::coap_duplicate_kind::fresh));
    BOOST_TEST(
        (table.classify(peer_a, 8, "tok", t0 + 2s).kind == kythira::coap_duplicate_kind::fresh));
}

BOOST_AUTO_TEST_CASE(reply_expires_at_retention_while_the_record_stays,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(1024);
    const auto t0 = clock_type::now();
    (void)table.classify(peer_a, 1, "tok", t0);
    table.attach_reply(peer_a, 1, "tok", bytes_of(10, 1), t0);

    BOOST_TEST(
        (table.classify(peer_a, 1, "tok", t0 + 92s).kind == kythira::coap_duplicate_kind::replay));

    // Past MAX_TRANSMIT_WAIT the reply is gone, but the record still makes a
    // late copy a duplicate: dropped, never handled a second time.
    const auto late = table.classify(peer_a, 1, "tok", t0 + 93s);
    BOOST_TEST((late.kind == kythira::coap_duplicate_kind::drop));
    BOOST_TEST(table.replies().count == 0U);
    BOOST_TEST(table.replies().bytes == 0U);
    BOOST_TEST(table.is_duplicate(peer_a, 1, "tok", t0 + 246s));

    // At EXCHANGE_LIFETIME the record goes too, and the Message ID is new again.
    BOOST_TEST(
        (table.classify(peer_a, 1, "tok", t0 + 247s).kind == kythira::coap_duplicate_kind::fresh));
}

BOOST_AUTO_TEST_CASE(reply_budget_evicts_the_oldest_first,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(100);
    const auto t0 = clock_type::now();
    for (std::uint16_t mid = 0; mid < 3; ++mid) {
        (void)table.classify(peer_a, mid, "tok", t0);
        table.attach_reply(peer_a, mid, "tok", bytes_of(40, static_cast<std::uint8_t>(mid)),
                           t0 + std::chrono::milliseconds{mid});
    }
    // 40 + 40 fit; the third needed the first's room.
    BOOST_TEST(table.replies().count == 2U);
    BOOST_TEST(table.replies().bytes == 80U);
    BOOST_TEST(table.replies().evicted == 1U);

    const auto now = t0 + 1s;
    BOOST_TEST((table.classify(peer_a, 0, "tok", now).kind == kythira::coap_duplicate_kind::drop));
    BOOST_TEST(
        (table.classify(peer_a, 1, "tok", now).kind == kythira::coap_duplicate_kind::replay));
    BOOST_TEST(
        (table.classify(peer_a, 2, "tok", now).kind == kythira::coap_duplicate_kind::replay));
}

BOOST_AUTO_TEST_CASE(reply_larger_than_the_whole_budget_is_not_stored,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(64);
    const auto t0 = clock_type::now();
    (void)table.classify(peer_a, 1, "small", t0);
    table.attach_reply(peer_a, 1, "small", bytes_of(32, 1), t0);
    (void)table.classify(peer_a, 2, "big", t0);
    table.attach_reply(peer_a, 2, "big", bytes_of(65, 2), t0);

    // The oversized reply evicted nothing on its way to being refused.
    BOOST_TEST(table.replies().count == 1U);
    BOOST_TEST(table.replies().evicted == 0U);
    BOOST_TEST(
        (table.classify(peer_a, 2, "big", t0 + 1s).kind == kythira::coap_duplicate_kind::drop));
    BOOST_TEST(
        (table.classify(peer_a, 1, "small", t0 + 1s).kind == kythira::coap_duplicate_kind::replay));
}

BOOST_AUTO_TEST_CASE(reply_for_an_unrecorded_exchange_is_ignored,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(1024);
    const auto t0 = clock_type::now();
    table.attach_reply(peer_a, 1, "tok", bytes_of(10, 1), t0);
    (void)table.classify(peer_a, 2, "tok", t0);
    table.attach_reply(peer_a, 2, "other-token", bytes_of(10, 1), t0);
    BOOST_TEST(table.replies().count == 0U);
}

// A Message ID that wraps inside the retention puts a newer exchange's reply
// in the slot an older one's reply occupied. Evicting the older entry from the
// queue must not take the newer reply with it.
BOOST_AUTO_TEST_CASE(reply_eviction_spares_a_newer_exchange_on_a_wrapped_message_id,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(100);
    const auto t0 = clock_type::now();

    (void)table.classify(peer_a, 5, "old", t0);
    table.attach_reply(peer_a, 5, "old", bytes_of(40, 1), t0);
    // The counter laps: Message ID 5 again, a new token, a new reply.
    (void)table.classify(peer_a, 5, "new", t0 + 1ms);
    table.attach_reply(peer_a, 5, "new", bytes_of(40, 2), t0 + 1ms);
    BOOST_TEST(table.replies().count == 1U);
    BOOST_TEST(table.replies().bytes == 40U);

    // Room for this one needs no eviction (40 + 40 <= 100), but the queue's
    // front is the stale "old" entry; a later squeeze pops it first.
    (void)table.classify(peer_a, 6, "x", t0 + 2ms);
    table.attach_reply(peer_a, 6, "x", bytes_of(40, 3), t0 + 2ms);
    (void)table.classify(peer_a, 7, "y", t0 + 3ms);
    table.attach_reply(peer_a, 7, "y", bytes_of(40, 4), t0 + 3ms);

    // Popping the stale entry freed nothing, so "new" (the oldest live reply)
    // was the one evicted -- once -- and the two later ones survive.
    BOOST_TEST(table.replies().evicted == 1U);
    BOOST_TEST(table.replies().count == 2U);
    BOOST_TEST(
        (table.classify(peer_a, 6, "x", t0 + 1s).kind == kythira::coap_duplicate_kind::replay));
    BOOST_TEST(
        (table.classify(peer_a, 7, "y", t0 + 1s).kind == kythira::coap_duplicate_kind::replay));
}

BOOST_AUTO_TEST_CASE(reply_from_an_old_queue_entry_never_clears_a_newer_reply,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // Retention-driven variant of the wrap guard: the old entry expires while
    // the newer reply in the same slot is still within retention.
    auto table = caching_table(1024, 10s);
    const auto t0 = clock_type::now();
    (void)table.classify(peer_a, 5, "old", t0);
    table.attach_reply(peer_a, 5, "old", bytes_of(8, 1), t0);
    (void)table.classify(peer_a, 5, "new", t0 + 5s);
    table.attach_reply(peer_a, 5, "new", bytes_of(8, 2), t0 + 5s);

    const auto at = t0 + 11s;  // "old" entry past retention, "new" is not
    const auto lookup = table.classify(peer_a, 5, "new", at);
    BOOST_TEST((lookup.kind == kythira::coap_duplicate_kind::replay));
    BOOST_REQUIRE(lookup.reply);
    BOOST_TEST((*lookup.reply == bytes_of(8, 2)));
}

BOOST_AUTO_TEST_CASE(reply_budget_zero_stores_nothing,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    // The default limits: the libcoap backend's table, unchanged by this.
    kythira::coap_exchange_table table;
    const auto t0 = clock_type::now();
    BOOST_TEST((table.classify(peer_a, 1, "tok", t0).kind == kythira::coap_duplicate_kind::fresh));
    table.attach_reply(peer_a, 1, "tok", bytes_of(10, 1), t0);
    BOOST_TEST(table.replies().count == 0U);
    BOOST_TEST(table.replies().bytes == 0U);
    BOOST_TEST(
        (table.classify(peer_a, 1, "tok", t0 + 1s).kind == kythira::coap_duplicate_kind::drop));
}

BOOST_AUTO_TEST_CASE(reply_bytes_are_released_by_clear_sweep_and_overwrite,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(15))) {
    auto table = caching_table(1024);
    const auto t0 = clock_type::now();
    for (std::uint16_t mid = 0; mid < 4; ++mid) {
        (void)table.classify(peer_a, mid, "tok", t0);
        table.attach_reply(peer_a, mid, "tok", bytes_of(10, 1), t0);
    }
    BOOST_TEST(table.replies().bytes == 40U);

    // Recording a new exchange over a slot frees the old exchange's reply.
    table.record(peer_a, 0, "lapped", t0 + 1s);
    BOOST_TEST(table.replies().bytes == 30U);

    // Attaching twice to one exchange replaces, not adds.
    table.attach_reply(peer_a, 1, "tok", bytes_of(25, 2), t0 + 1s);
    BOOST_TEST(table.replies().bytes == 45U);
    BOOST_TEST(table.replies().count == 3U);

    // A sweep past the lifetime takes records and replies alike.
    table.sweep(t0 + kythira::coap_exchange_lifetime + 1s);
    BOOST_TEST(table.replies().bytes == 0U);
    BOOST_TEST(table.size() == 0U);

    (void)table.classify(peer_b, 9, "tok", t0);
    table.attach_reply(peer_b, 9, "tok", bytes_of(10, 1), t0);
    table.clear();
    BOOST_TEST(table.replies().bytes == 0U);
    BOOST_TEST(table.replies().count == 0U);
    BOOST_TEST((table.classify(peer_b, 9, "tok", t0).kind == kythira::coap_duplicate_kind::fresh));
}

BOOST_AUTO_TEST_SUITE_END()
