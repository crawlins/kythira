// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oscore_group_contexts_test.cpp
/// @brief One OSCORE Security Context per (peer, group)
///        (.kiro/specs/coap-transport-multi-raft/ tasks 9, 10 and 11).
///
/// Everything here drives raft/oscore_group_contexts.hpp and
/// raft/oscore.hpp's security_context directly, with no transport: the
/// properties are about keys, windows and bounds, and a socket would only add
/// timing to them.

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE oscore_group_contexts_test
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>
#include <raft/oscore_group_contexts.hpp>
#include <raft/shard_placement_driver.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace osc = kythira::oscore;

namespace {

using namespace std::chrono_literals;

const std::string peer_b = "10.0.0.2:5683";
constexpr std::uint64_t group_7 = 7;
constexpr std::uint64_t group_8 = 8;

auto base_credentials(std::byte sender, std::byte recipient) -> kythira::oscore_credentials {
    kythira::oscore_credentials creds;
    creds.sender_id = {sender};
    creds.recipient_id = {recipient};
    creds.master_secret = std::vector<std::byte>(16, std::byte{0x2B});
    creds.master_salt = std::vector<std::byte>(8, std::byte{0x9E});
    return creds;
}

/// A node's registry: node A sends with sender id 0x0A and receives from 0x0B.
struct node {
    explicit node(std::byte self, std::byte other, std::set<std::uint64_t> hosted,
                  kythira::oscore::group_context_limits limits = {},
                  std::vector<std::byte> boot_nonce = osc::generate_boot_nonce())
        : hosted_groups{std::move(hosted)},
          registry{[this, self, other](const std::string&) {
                       ++bootstrap_calls;
                       return base_credentials(self, other);
                   },
                   [this](std::uint64_t group) { return hosted_groups.contains(group); }, limits,
                   std::move(boot_nonce)} {}

    std::set<std::uint64_t> hosted_groups;
    int bootstrap_calls{0};
    osc::group_context_registry registry;
};

auto get_request(std::uint16_t message_id) -> osc::coap_message {
    // A CON GET with a two-byte token; the content does not matter here.
    osc::coap_message message;
    message.type = 0;
    message.code = 0x01;
    message.message_id = message_id;
    message.token = {std::byte{0x01}, std::byte{0x02}};
    return message;
}

/// Verifies one protected request from `sender_name` at `receiver`, letting the
/// registry pick or derive the context. True when it verified.
auto deliver(node& receiver, const std::string& sender_name,
             const osc::coap_message& protected_message) -> bool {
    try {
        auto context = receiver.registry.recipient_context(sender_name, protected_message);
        osc::request_binding binding;
        (void)context->unprotect_request(protected_message, binding);
        return true;
    } catch (const osc::verification_error&) {
        return false;
    }
}

auto all_zero(const std::vector<std::byte>& bytes) -> bool {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oscore_group_contexts_tests)

// ── Task 9: a context per (peer, group) ────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_id_context_is_group_then_boot_nonce) {
    const std::vector<std::byte> nonce(osc::boot_nonce_length, std::byte{0xAB});
    const auto id = osc::make_group_id_context(0x0102030405060708ULL, nonce);
    BOOST_REQUIRE(id.size() == 8 + osc::boot_nonce_length);
    BOOST_TEST(static_cast<int>(id[0]) == 0x01);
    BOOST_TEST(static_cast<int>(id[7]) == 0x08);
    BOOST_TEST(static_cast<int>(id[8]) == 0xAB);
    BOOST_TEST(osc::group_of_id_context(id).value() == 0x0102030405060708ULL);
    // Too short to hold a full boot nonce: not one of ours.
    BOOST_TEST(!osc::group_of_id_context(std::span(id).first(12)).has_value());
}

BOOST_AUTO_TEST_CASE(the_process_boot_nonce_is_random_and_stable) {
    BOOST_TEST(osc::process_boot_nonce().size() >= osc::boot_nonce_length);
    BOOST_TEST((osc::process_boot_nonce() == osc::process_boot_nonce()));
    BOOST_TEST((osc::generate_boot_nonce() != osc::generate_boot_nonce()));
}

BOOST_AUTO_TEST_CASE(two_groups_against_one_peer_derive_different_keys) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7, group_8}};
    const auto g7 = a.registry.sender_context(peer_b, group_7);
    const auto g8 = a.registry.sender_context(peer_b, group_8);
    BOOST_TEST((g7->sender_key() != g8->sender_key()));
    BOOST_TEST((g7->recipient_key() != g8->recipient_key()));
    BOOST_TEST((g7->common_iv() != g8->common_iv()));
    // The same group twice is the same context, not a second derivation.
    BOOST_TEST(a.registry.sender_context(peer_b, group_7) == g7);
    BOOST_TEST(a.registry.counters().sender_derivations == 2u);
}

BOOST_AUTO_TEST_CASE(a_restart_derives_different_keys_for_the_same_group) {
    // Same master secret, same group, a new process incarnation: the keys must
    // differ, or the restarted node would reissue Partial IV 0 under a key it
    // had already used.
    node before{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    node after{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    const auto old_context = before.registry.sender_context(peer_b, group_7);
    const auto new_context = after.registry.sender_context(peer_b, group_7);
    BOOST_TEST((old_context->id_context() != new_context->id_context()));
    BOOST_TEST((old_context->sender_key() != new_context->sender_key()));
    BOOST_TEST((old_context->common_iv() != new_context->common_iv()));
}

BOOST_AUTO_TEST_CASE(n_groups_cost_one_bootstrap) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {}};
    for (std::uint64_t group = 1; group <= 1000; ++group) {
        (void)a.registry.sender_context(peer_b, group);
    }
    BOOST_TEST(a.bootstrap_calls == 1);
    BOOST_TEST(a.registry.counters().bootstraps == 1u);
    BOOST_TEST(a.registry.counters().sender_derivations == 1000u);
}

// ── Task 10: recipient selection and bounded on-demand derivation ──────────

BOOST_AUTO_TEST_CASE(a_request_selects_the_recipient_context_by_kid_context) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7, group_8}};
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7, group_8}};

    osc::request_binding binding;
    const auto g7 =
        a.registry.sender_context(peer_b, group_7)->protect_request(get_request(1), binding);
    const auto g8 =
        a.registry.sender_context(peer_b, group_8)->protect_request(get_request(2), binding);
    BOOST_TEST(deliver(b, "node-a", g7));
    BOOST_TEST(deliver(b, "node-a", g8));
    BOOST_TEST(b.registry.recipient_count("node-a") == 2u);
    BOOST_TEST(b.registry.counters().recipient_derivations == 2u);

    // A second message in a known group reuses its context.
    const auto again =
        a.registry.sender_context(peer_b, group_7)->protect_request(get_request(3), binding);
    BOOST_TEST(deliver(b, "node-a", again));
    BOOST_TEST(b.registry.counters().recipient_derivations == 2u);
}

BOOST_AUTO_TEST_CASE(an_unhosted_group_is_rejected_without_deriving) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_8}};  // does not host 7

    osc::request_binding binding;
    const auto request =
        a.registry.sender_context(peer_b, group_7)->protect_request(get_request(1), binding);
    BOOST_TEST(!deliver(b, "node-a", request));
    BOOST_TEST(b.registry.counters().recipient_derivations == 0u);
    BOOST_TEST(b.registry.counters().rejected_unhosted == 1u);
    BOOST_TEST(b.registry.recipient_count("node-a") == 0u);
}

BOOST_AUTO_TEST_CASE(a_request_without_a_kid_context_is_rejected) {
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7}};
    osc::security_context plain{base_credentials(std::byte{0x0A}, std::byte{0x0B})};
    osc::request_binding binding;
    const auto request = plain.protect_request(get_request(1), binding);
    BOOST_CHECK_THROW((void)b.registry.recipient_context("node-a", request),
                      osc::verification_error);
    BOOST_TEST(b.registry.counters().rejected_malformed == 1u);
    BOOST_TEST(b.registry.counters().recipient_derivations == 0u);
}

BOOST_AUTO_TEST_CASE(the_cap_evicts_the_least_recently_used_not_the_newest) {
    node b{std::byte{0x0B}, std::byte{0x0A}, {1, 2, 3, 4}, {.max_recipient_contexts_per_peer = 3}};
    const auto nonce = osc::generate_boot_nonce();
    const std::vector<std::byte> kid{std::byte{0x0A}};
    const auto t0 = osc::group_context_registry::clock::now();
    auto id = [&](std::uint64_t group) { return osc::make_group_id_context(group, nonce); };

    const auto c1 = b.registry.recipient_context("node-a", kid, id(1), t0);
    (void)b.registry.recipient_context("node-a", kid, id(2), t0 + 1s);
    (void)b.registry.recipient_context("node-a", kid, id(3), t0 + 2s);
    // Touch 1, so 2 is now the least recently used.
    BOOST_TEST(b.registry.recipient_context("node-a", kid, id(1), t0 + 3s) == c1);
    const auto c4 = b.registry.recipient_context("node-a", kid, id(4), t0 + 4s);

    BOOST_TEST(b.registry.recipient_count("node-a") == 3u);
    BOOST_TEST(b.registry.counters().evictions == 1u);
    // 1 and the newest, 4, survive; 2 was evicted, so asking for it derives again.
    BOOST_TEST(b.registry.recipient_context("node-a", kid, id(1), t0 + 5s) == c1);
    BOOST_TEST(b.registry.recipient_context("node-a", kid, id(4), t0 + 5s) == c4);
    const auto before = b.registry.counters().recipient_derivations;
    (void)b.registry.recipient_context("node-a", kid, id(2), t0 + 6s);
    BOOST_TEST(b.registry.counters().recipient_derivations == before + 1);
}

// A peer restarts: it now sends under a new boot nonce, while messages it
// sent under the old one are still in flight. Inside the TTL none of them is
// lost; past it the old context goes.
BOOST_AUTO_TEST_CASE(a_peer_restart_loses_no_in_flight_message_inside_the_ttl) {
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7}, {.idle_ttl = 60s}};
    node old_a{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    node new_a{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    const auto t0 = osc::group_context_registry::clock::now();

    osc::request_binding binding;
    std::vector<osc::coap_message> in_flight;
    for (std::uint16_t i = 0; i < 5; ++i) {
        in_flight.push_back(old_a.registry.sender_context(peer_b, group_7)
                                ->protect_request(get_request(i), binding));
    }
    // The first old-incarnation message arrives, then the restart's first.
    auto verify_at = [&](const osc::coap_message& m, auto when) {
        try {
            auto context = b.registry.recipient_context("node-a", m, when);
            osc::request_binding received;
            (void)context->unprotect_request(m, received);
            return true;
        } catch (const osc::verification_error&) {
            return false;
        }
    };
    BOOST_TEST(verify_at(in_flight[0], t0));
    const auto fresh =
        new_a.registry.sender_context(peer_b, group_7)->protect_request(get_request(100), binding);
    BOOST_TEST(verify_at(fresh, t0 + 1s));
    for (std::size_t i = 1; i < in_flight.size(); ++i) {
        BOOST_TEST(verify_at(in_flight[i], t0 + 2s + std::chrono::seconds{i}));
    }
    BOOST_TEST(b.registry.recipient_count("node-a") == 2u);

    // Long after, the old incarnation's context expires on its own.
    b.registry.sweep(t0 + 10min);
    BOOST_TEST(b.registry.counters().expirations == 2u);
    BOOST_TEST(b.registry.recipient_count("node-a") == 0u);
}

// ── Task 11: the replay-window property, lifecycle, ids ────────────────────

// The property the phase exists for. Group 7 sends 200 messages while group
// 8's 10 messages are held back and delivered last, as independent pipelines
// reorder them. With one shared context per peer, group 8's messages fall
// more than 64 Partial IVs behind and fail the replay window; with a context
// per group none does. The window itself stays RFC 8613's 64.
BOOST_AUTO_TEST_CASE(one_busy_group_cannot_exhaust_another_groups_replay_window) {
    // Shared context: the failure this design exists to prevent.
    {
        osc::security_context sender{base_credentials(std::byte{0x0A}, std::byte{0x0B})};
        osc::security_context receiver{base_credentials(std::byte{0x0B}, std::byte{0x0A})};
        osc::request_binding binding;
        std::vector<osc::coap_message> held;
        for (int i = 0; i < 10; ++i) {
            held.push_back(sender.protect_request(get_request(1000 + i), binding));
        }
        for (int i = 0; i < 200; ++i) {
            osc::request_binding received;
            (void)receiver.unprotect_request(sender.protect_request(get_request(i), binding),
                                             received);
        }
        int rejected = 0;
        for (const auto& message : held) {
            osc::request_binding received;
            try {
                (void)receiver.unprotect_request(message, received);
            } catch (const osc::verification_error&) {
                ++rejected;
            }
        }
        BOOST_TEST(rejected == 10);
    }

    // A context per group: same traffic, nothing rejected.
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7, group_8}};
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7, group_8}};
    osc::request_binding binding;
    std::vector<osc::coap_message> held;
    for (int i = 0; i < 10; ++i) {
        held.push_back(a.registry.sender_context(peer_b, group_8)
                           ->protect_request(get_request(1000 + i), binding));
    }
    int delivered_7 = 0;
    for (int i = 0; i < 200; ++i) {
        delivered_7 += deliver(b, "node-a",
                               a.registry.sender_context(peer_b, group_7)
                                   ->protect_request(get_request(i), binding))
                           ? 1
                           : 0;
    }
    int delivered_8 = 0;
    for (const auto& message : held) {
        delivered_8 += deliver(b, "node-a", message) ? 1 : 0;
    }
    BOOST_TEST(delivered_7 == 200);
    BOOST_TEST(delivered_8 == 10);
}

BOOST_AUTO_TEST_CASE(the_replay_window_is_still_64) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7}};
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7}};
    osc::request_binding binding;
    auto sender = a.registry.sender_context(peer_b, group_7);
    std::vector<osc::coap_message> sent;
    for (int i = 0; i < 66; ++i) {
        sent.push_back(sender->protect_request(get_request(i), binding));
    }
    // Deliver the newest first, then the one exactly 64 behind it (out) and
    // the one 63 behind it (in).
    BOOST_TEST(deliver(b, "node-a", sent[65]));
    BOOST_TEST(!deliver(b, "node-a", sent[1]));
    BOOST_TEST(deliver(b, "node-a", sent[2]));
    // And a replay is still a replay.
    BOOST_TEST(!deliver(b, "node-a", sent[65]));
}

BOOST_AUTO_TEST_CASE(forgetting_a_merged_away_group_zeroes_its_keys) {
    node a{std::byte{0x0A}, std::byte{0x0B}, {group_7, group_8}};
    node b{std::byte{0x0B}, std::byte{0x0A}, {group_7, group_8}};
    osc::request_binding binding;
    const auto sender = a.registry.sender_context(peer_b, group_7);
    const auto request = sender->protect_request(get_request(1), binding);
    BOOST_REQUIRE(deliver(b, "node-a", request));
    const auto recipient = b.registry.recipient_context("node-a", request);
    const auto survivor = a.registry.sender_context(peer_b, group_8);

    a.registry.forget_group(group_7);
    b.registry.forget_group(group_7);

    for (const auto& context : {sender, recipient}) {
        BOOST_TEST(context->is_wiped());
        BOOST_TEST(all_zero(context->sender_key()));
        BOOST_TEST(all_zero(context->recipient_key()));
        BOOST_TEST(all_zero(context->common_iv()));
    }
    // A holder of the old pointer cannot use it.
    BOOST_CHECK_THROW((void)sender->protect_request(get_request(2), binding),
                      kythira::coap_security_error);
    // Other groups are untouched.
    BOOST_TEST(!survivor->is_wiped());
    BOOST_TEST(!all_zero(survivor->sender_key()));
    BOOST_TEST(a.registry.sender_count(peer_b) == 1u);
    BOOST_TEST(b.registry.recipient_count("node-a") == 0u);
}

// Requirement 4.10: this design depends on group ids never being reissued.
// The placement driver's allocator is what guarantees it, so assert against
// that allocator: every id it hands out is distinct, and exhaustion is a
// refusal (fewer ids), never a repeat.
BOOST_AUTO_TEST_CASE(the_placement_driver_never_reissues_a_group_id) {
    kythira::no_op_shard_placement_driver<> driver{100, 1100};
    std::set<std::uint64_t> seen;
    std::size_t handed_out = 0;
    for (int round = 0; round < 40; ++round) {
        const auto batch = driver.allocate_shard_ids(37).get();
        for (const auto& allocation : batch) {
            BOOST_TEST(seen.insert(allocation._group_id).second);
            ++handed_out;
        }
    }
    BOOST_TEST(handed_out == 1000u);
    BOOST_TEST(driver.allocate_shard_ids(1).get().empty());
}

BOOST_AUTO_TEST_CASE(bootstrap_credentials_carrying_an_id_context_are_refused) {
    osc::group_context_registry registry{[](const std::string&) {
                                             auto creds =
                                                 base_credentials(std::byte{0x0A}, std::byte{0x0B});
                                             creds.id_context = {std::byte{0x01}};
                                             return creds;
                                         },
                                         [](std::uint64_t) { return true; }};
    BOOST_CHECK_THROW((void)registry.sender_context(peer_b, group_7),
                      kythira::coap_security_config_error);
}

BOOST_AUTO_TEST_SUITE_END()
