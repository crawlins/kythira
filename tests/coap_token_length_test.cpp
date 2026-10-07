// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE coap_token_length_test
#include <boost/test/unit_test.hpp>

#include <raft/coap_utils.hpp>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

/// @file coap_token_length_test.cpp
/// @brief Regression tests for the CoAP token length cap (RFC 7252 §5.3.1).
///
/// The bug these exist to prevent: tokens used to be built as
/// `"token_" + std::to_string(counter)`, which is 7 bytes at counter 0 and
/// **9 bytes from counter 100 onward** — past the 8-byte maximum. Nothing
/// rejected it. `coap_add_token()` accepted the over-long token and
/// `coap_send()` then dropped the PDU, logging only:
///
///     WARN coap_send: PDU dropped as token too long (9 > 8)
///
/// while `send_rpc()` went on returning futures that could never complete. The
/// observable effect was a node that silently stopped transmitting after its
/// 100th request. It stayed latent because nothing exercised the real libcoap
/// path until d54bc46 wired it into the non-stub build.
///
/// The tests below therefore care much less about *which* token is produced
/// than about the width being independent of the counter.

using kythira::coap_max_token_length;
using kythira::coap_utils::format_sequential_token;
using kythira::coap_utils::token_scrambler;

BOOST_AUTO_TEST_CASE(token_width_is_independent_of_counter) {
    // The exact values that used to straddle the boundary. 99 was the last
    // counter that fit and 100 was the first that did not, so a regression
    // that reintroduced a variable-width encoding would show up right here.
    for (std::uint32_t counter : {0U, 1U, 9U, 10U, 99U, 100U, 101U, 999U, 1000U}) {
        BOOST_TEST_CONTEXT("counter=" << counter) {
            BOOST_CHECK_EQUAL(format_sequential_token(counter).size(), coap_max_token_length);
        }
    }
}

BOOST_AUTO_TEST_CASE(token_never_exceeds_the_cap_across_the_counter_range) {
    // Boundaries of each hex digit's rollover, plus the extremes. A token
    // longer than the cap is dropped by libcoap without failing the send, so
    // this is the property that has to hold for every reachable counter.
    for (std::uint32_t counter : {0U, 0xFU, 0x10U, 0xFFU, 0x100U, 0xFFFFU, 0x10000U, 0xFFFFFFU,
                                  0x1000000U, 0xFFFFFFFEU, 0xFFFFFFFFU}) {
        BOOST_TEST_CONTEXT("counter=" << counter) {
            BOOST_CHECK_LE(format_sequential_token(counter).size(), coap_max_token_length);
        }
    }
}

BOOST_AUTO_TEST_CASE(tokens_are_printable) {
    // The transport logs the token on ~45 paths and uses it as a map key, so a
    // token containing control bytes would corrupt log output even though it
    // would be a legal CoAP token.
    for (std::uint32_t counter : {0U, 100U, 0xDEADBEEFU, 0xFFFFFFFFU}) {
        const auto token = format_sequential_token(counter);
        BOOST_TEST_CONTEXT("counter=" << counter << " token=" << token) {
            for (char c : token) {
                BOOST_CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(tokens_are_distinct_across_a_sweep) {
    // Correlation depends on outstanding tokens differing; handle_response()
    // matches the response's token against _pending_requests.
    std::set<std::string> seen;
    constexpr std::uint32_t sweep = 5000;
    for (std::uint32_t counter = 0; counter < sweep; ++counter) {
        seen.insert(format_sequential_token(counter));
    }
    BOOST_CHECK_EQUAL(seen.size(), sweep);
}

BOOST_AUTO_TEST_CASE(token_encodes_the_counter_most_significant_first) {
    // Documented ordering property: lexicographic order matches numeric order,
    // which is what makes tokens readable in a log.
    BOOST_CHECK_EQUAL(format_sequential_token(0), "00000000");
    BOOST_CHECK_EQUAL(format_sequential_token(1), "00000001");
    BOOST_CHECK_EQUAL(format_sequential_token(100), "00000064");
    BOOST_CHECK_EQUAL(format_sequential_token(0xDEADBEEFU), "deadbeef");
    BOOST_CHECK_EQUAL(format_sequential_token(0xFFFFFFFFU), "ffffffff");
    BOOST_CHECK_LT(format_sequential_token(41), format_sequential_token(42));
}

BOOST_AUTO_TEST_CASE(the_previous_encoding_would_have_failed_these_tests) {
    // Guards the guard: pins down that the old scheme really did cross the cap,
    // so this file cannot be quietly rewritten into something vacuous.
    const auto old_encoding = [](std::uint32_t n) { return "token_" + std::to_string(n); };

    BOOST_CHECK_LE(old_encoding(99).size(), coap_max_token_length);
    BOOST_CHECK_GT(old_encoding(100).size(), coap_max_token_length);
}

// The scrambler is what keeps tokens unique once they stop being sequential:
// distinct counters must still give distinct tokens. A run of 2^20 counters
// (well past any realistic number of requests in flight) must not collide.
BOOST_AUTO_TEST_CASE(scrambled_counters_never_collide) {
    const token_scrambler scramble;
    std::vector<bool> seen(std::size_t{1} << 20U);
    std::set<std::uint32_t> outside;
    for (std::uint32_t counter = 0; counter < (1U << 20U); ++counter) {
        const auto value = scramble(counter);
        if (value < seen.size()) {
            BOOST_REQUIRE(!seen[value]);
            seen[value] = true;
        } else {
            BOOST_REQUIRE(outside.insert(value).second);
        }
    }
}

// RFC 7252 5.3.1: an off-path attacker should not be able to guess a token.
// Sequential counters must not come out sequential, and two clients (two
// random keys) must not issue the same token sequence.
BOOST_AUTO_TEST_CASE(scrambled_tokens_are_not_predictable_from_the_counter) {
    const token_scrambler first;
    const token_scrambler second;
    int sequential = 0;
    int shared = 0;
    for (std::uint32_t counter = 1; counter <= 1000; ++counter) {
        if (first(counter) == first(counter - 1) + 1) {
            ++sequential;
        }
        if (first(counter) == second(counter)) {
            ++shared;
        }
    }
    BOOST_TEST(sequential < 5);
    BOOST_TEST(shared < 5);
}

// Fixed keys give a fixed permutation, and the output still renders as a
// full-width token.
BOOST_AUTO_TEST_CASE(scrambler_is_deterministic_for_fixed_keys) {
    const token_scrambler a{std::array<std::uint32_t, 4>{1, 2, 3, 4}};
    const token_scrambler b{std::array<std::uint32_t, 4>{1, 2, 3, 4}};
    const token_scrambler c{std::array<std::uint32_t, 4>{1, 2, 3, 5}};
    BOOST_TEST(a(42) == b(42));
    BOOST_TEST(a(42) != c(42));
    BOOST_TEST(format_sequential_token(a(42)).size() == coap_max_token_length);
}
