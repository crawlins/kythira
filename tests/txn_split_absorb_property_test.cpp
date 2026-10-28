// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file txn_split_absorb_property_test.cpp
/// @brief The split/absorb round-trip law for `transactional_state_machine`
///        (task 4 of `.kiro/specs/multi-raft-transactions/`).
///
/// The law the multi-raft host relies on: split a state at any legal set of
/// keys, restore the leftmost child, absorb the others in order, and the
/// result's `get_state()` is byte-identical to the original's. The host's
/// merge is exactly that absorb, so a wrapper that dropped or duplicated a
/// lock, a record or a settled entry on a split would break a later merge.
///
/// Each trial builds a random shard: plain writes, transactions left in every
/// state (pending with intents, committed, aborted, a rollback marker with no
/// prepare, prepared on a secondary only) and an oracle reservation. It also
/// checks that each child holds only its own range's keys, since the law
/// alone would pass a split that put everything in one child.

#define BOOST_TEST_MODULE txn_split_absorb_property_test
#include <boost/test/unit_test.hpp>

#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#include "multi_raft_kv_workload.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<kythira::intent_view<std::string>>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

using kythira::txn_id;
using kythira::testing::kv_partitioner;

namespace {

using kv_sm = kythira::test_key_value_state_machine<>;
using txn_sm = kythira::transactional_state_machine<kv_sm, std::string, kv_partitioner>;
using env = kythira::txn_envelope<std::string>;
using write = kythira::txn_write<std::string>;

constexpr int k_trials = 300;
constexpr std::uint64_t k_key_space = 60;

struct shard {
    txn_sm sm;
    std::uint64_t index{0};

    auto run(const env& e) -> void {
        std::ignore = sm.apply(kythira::encode_txn_envelope<std::string>(e), ++index);
    }
};

auto key(std::uint64_t n) -> std::string {
    // Fixed width, so string order matches numeric order and cuts land where
    // a reader of a failure expects.
    std::string s = std::to_string(n);
    return "k" + std::string(3 - s.size(), '0') + s;
}

/// Fill `s` with a random mix of committed data and transactions in every
/// state. Keys are drawn without replacement per transaction so a prepare
/// never conflicts with itself; conflicts with other transactions are
/// allowed and simply leave nothing behind, as they would in production.
auto populate(shard& s, std::mt19937_64& rng) -> void {
    std::uniform_int_distribution<std::uint64_t> pick_key(0, k_key_space - 1);
    std::uniform_int_distribution<int> pick(0, 99);

    const int plain_writes = pick(rng) % 30;
    for (int i = 0; i < plain_writes; ++i) {
        const auto k = key(pick_key(rng));
        s.run(kythira::txn_plain{kv_sm::make_put_command(k, "v" + std::to_string(i))});
    }

    const int txns = 1 + pick(rng) % 12;
    for (int t = 0; t < txns; ++t) {
        const txn_id id{rng(), static_cast<std::uint64_t>(t)};
        std::vector<std::uint64_t> picks;
        const int writes = 1 + pick(rng) % 4;
        while (static_cast<int>(picks.size()) < writes) {
            const auto n = pick_key(rng);
            if (std::find(picks.begin(), picks.end(), n) == picks.end()) {
                picks.push_back(n);
            }
        }
        std::vector<write> ws;
        std::vector<std::string> keys;
        for (const auto n : picks) {
            keys.push_back(key(n));
            ws.push_back({key(n), kv_sm::make_put_command(key(n), "t" + std::to_string(t))});
        }
        const std::string primary = keys.front();
        const auto deadline = 100 + static_cast<std::uint64_t>(pick(rng));

        switch (pick(rng) % 6) {
            case 0:  // committed in one phase
                s.run(kythira::txn_one_phase<std::string>{id, ws, 0, 1});
                break;
            case 1:  // pending: intents and a record stay behind
                s.run(kythira::txn_prepare<std::string>{id, primary, ws, {}, 0, deadline, true});
                break;
            case 2:  // committed in two phases: record and settled entries
                s.run(kythira::txn_prepare<std::string>{id, primary, ws, {}, 0, deadline, true});
                s.run(kythira::txn_commit_primary<std::string>{id, primary, 5});
                break;
            case 3:  // aborted
                s.run(kythira::txn_prepare<std::string>{id, primary, ws, {}, 0, deadline, true});
                s.run(kythira::txn_abort<std::string>{id, primary, keys});
                break;
            case 4:  // a rollback marker with no prepare
                s.run(kythira::txn_check_and_resolve<std::string>{id, primary, 0});
                break;
            default:  // this shard is only a secondary: intents, no record
                s.run(kythira::txn_prepare<std::string>{
                    id, key(k_key_space), ws, {}, 0, deadline, false});
                break;
        }
    }

    if (pick(rng) % 2 == 0) {
        s.run(kythira::txn_oracle_reserve{rng() % 10000});
    }
}

auto random_cuts(std::mt19937_64& rng) -> std::vector<std::string> {
    std::uniform_int_distribution<std::uint64_t> pick_key(0, k_key_space + 5);
    std::uniform_int_distribution<int> count(1, 4);
    std::vector<std::string> cuts;
    const int n = count(rng);
    for (int i = 0; i < n; ++i) {
        cuts.push_back(key(pick_key(rng)));
    }
    return cuts;
}

}  // namespace

BOOST_AUTO_TEST_CASE(absorb_of_split_reproduces_the_state_byte_for_byte) {
    std::mt19937_64 rng(0x7478'6e5f'7370'6c74ULL);
    std::size_t locks_seen = 0;
    for (int trial = 0; trial < k_trials; ++trial) {
        BOOST_TEST_CONTEXT("trial " << trial) {
            shard s;
            populate(s, rng);
            locks_seen += s.sm.intent_count();
            const auto original = s.sm.get_state();

            auto cuts = random_cuts(rng);
            const auto parts = s.sm.split_state(cuts);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            BOOST_REQUIRE_EQUAL(parts.size(), cuts.size() + 1);

            // Each child holds only its own range's locks.
            std::size_t child_locks = 0;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                txn_sm child;
                child.restore_from_snapshot(parts[i], 1);
                child_locks += child.intent_count();
                for (std::uint64_t n = 0; n <= k_key_space; ++n) {
                    const auto k = key(n);
                    const bool below = i < cuts.size() && !(k < cuts[i]);
                    const bool above = i > 0 && k < cuts[i - 1];
                    if (below || above) {
                        BOOST_TEST(!child.intent_on(k).has_value());
                    } else {
                        BOOST_TEST(child.intent_on(k) == s.sm.intent_on(k));
                    }
                }
            }
            BOOST_TEST(child_locks == s.sm.intent_count());

            txn_sm joined;
            joined.restore_from_snapshot(parts[0], 1);
            for (std::size_t i = 1; i < parts.size(); ++i) {
                const kythira::shard_range<std::string> range{
                    cuts[i - 1],
                    i < cuts.size() ? std::optional<std::string>(cuts[i]) : std::nullopt};
                joined.absorb(parts[i], range);
            }
            BOOST_TEST(joined.get_state() == original);
        }
    }
    // The generator must actually leave locks behind, or the law is vacuous.
    BOOST_TEST(locks_seen > static_cast<std::size_t>(k_trials));
}

BOOST_AUTO_TEST_CASE(split_children_restore_into_a_working_shard) {
    // After a split the children are live shards: the lock on the right-hand
    // key still refuses a plain write, and its commit still applies.
    shard s;
    const txn_id id{1, 1};
    s.run(kythira::txn_prepare<std::string>{id,
                                            key(1),
                                            {{key(1), kv_sm::make_put_command(key(1), "a")},
                                             {key(50), kv_sm::make_put_command(key(50), "b")}},
                                            {},
                                            0,
                                            100,
                                            true});
    const auto parts = s.sm.split_state({key(30)});

    shard right;
    right.sm.restore_from_snapshot(parts[1], 1);
    const auto locked = kythira::decode_txn_result<std::string>(
        right.sm.apply(kythira::encode_txn_envelope<std::string>(
                           env{kythira::txn_plain{kv_sm::make_put_command(key(50), "x")}}),
                       2));
    BOOST_TEST((locked._status == kythira::txn_result_status::key_locked));
    right.run(kythira::txn_commit_secondary<std::string>{id, {key(50)}, 5});
    BOOST_TEST((right.sm.inner().get_value(key(50)) == std::optional<std::string>("b")));
    BOOST_TEST(right.sm.intent_count() == 0U);

    shard left;
    left.sm.restore_from_snapshot(parts[0], 1);
    left.run(kythira::txn_commit_primary<std::string>{id, key(1), 5});
    BOOST_TEST((left.sm.inner().get_value(key(1)) == std::optional<std::string>("a")));
    BOOST_TEST(left.sm.intent_count() == 0U);
}
