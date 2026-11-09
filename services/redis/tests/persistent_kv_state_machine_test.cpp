// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// persistent-state-machine task 2: the Redis command set on a kv_engine,
// checked against the in-memory redis_kv_state_machine as the oracle, plus
// atomic apply, replay, crash recovery, checkpoints, split and merge.

#include <raft/memory_kv_engine.hpp>
#include <kythira/redis/persistent_kv_state_machine.hpp>
#include <kythira/redis/redis_kv_state_machine.hpp>

#define BOOST_TEST_MODULE persistent_kv_state_machine_test
#include <boost/test/unit_test.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

using kythira::column_family;
using kythira::key_range;
using kythira::memory_kv_engine;
using kythira::persistent_kv_state_machine;
using kythira::redis_kv_command;
using kythira::redis_kv_state_machine;

namespace {

using psm = persistent_kv_state_machine<memory_kv_engine>;
using oracle = redis_kv_state_machine<std::uint64_t>;

static_assert(kythira::persistent_state_machine<psm>);
static_assert(kythira::persistent_splittable_state_machine<psm>);
static_assert(kythira::storage_kind_of<psm> == kythira::state_machine_storage::persistent);
static_assert(!kythira::state_machine<psm, std::uint64_t>);
static_assert(kythira::persistent_staged_state_machine<psm>);

class temp_dir {
public:
    temp_dir() {
        auto tmpl = (std::filesystem::temp_directory_path() / "kythira-pkv-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        _path = tmpl;
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return _path; }

private:
    std::filesystem::path _path;
};

auto bytes(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

auto set(const std::string& k, const std::string& v, std::uint64_t expire = 0)
    -> std::vector<std::byte> {
    return kythira::encode_redis_kv_command(
        kythira::redis_kv_set_command{._key = k, ._value = bytes(v), ._expire_at_ms = expire});
}

auto del(const std::string& k) -> std::vector<std::byte> {
    return kythira::encode_redis_kv_command(kythira::redis_kv_del_command{._key = k});
}

const key_range k_everything{.start = "", .end = std::nullopt};

/// Every observable of the persistent machine equals the oracle's over the
/// key universe `keys`.
auto check_same(const psm& p, const oracle& o, const std::vector<std::string>& keys,
                std::uint64_t now_ms) -> void {
    for (const auto& k : keys) {
        const auto a = p.lookup(k);
        const auto b = o.lookup(k);
        BOOST_REQUIRE_MESSAGE((a == nullptr) == (b == nullptr), "presence of " << k);
        if (a != nullptr) {
            BOOST_REQUIRE_MESSAGE(a->_value == b->_value, "value of " << k);
            BOOST_REQUIRE_EQUAL(a->_expire_at_ms, b->_expire_at_ms);
        }
    }
    BOOST_REQUIRE_EQUAL(p.approximate_key_count(), o.approximate_key_count());
    BOOST_REQUIRE_EQUAL(p.approximate_size_bytes(), o.approximate_size_bytes());
    BOOST_REQUIRE_EQUAL(p.expiring_key_count(), o.expiring_key_count());
    const auto pe = p.collect_expired(now_ms, 1000);
    const auto oe = o.collect_expired(now_ms, 1000);
    BOOST_REQUIRE_EQUAL(pe.size(), oe.size());
    for (std::size_t i = 0; i < pe.size(); ++i) {
        BOOST_REQUIRE_EQUAL(pe[i]._key, oe[i]._key);
        BOOST_REQUIRE_EQUAL(pe[i]._expire_at_ms, oe[i]._expire_at_ms);
    }
    BOOST_REQUIRE_EQUAL(p.suggest_split_keys(3).size(),
                        const_cast<oracle&>(o).suggest_split_keys(3).size());
}

/// A random command over a small key universe, so sets, deletes, sweeps
/// and evictions keep hitting the same keys.
auto random_command(std::mt19937_64& rng, const std::vector<std::string>& keys, const oracle& o)
    -> std::vector<std::byte> {
    auto pick = [&] { return keys[rng() % keys.size()]; };
    switch (rng() % 8) {
        case 0:
        case 1:
        case 2: {
            const auto expire = (rng() % 3 == 0) ? 1000 + rng() % 100 : 0;
            return set(pick(), std::string(rng() % 40, static_cast<char>('a' + rng() % 26)),
                       expire);
        }
        case 3:
            return del(pick());
        case 4: {
            // A sweep built from the oracle's view, plus a stale entry.
            auto entries = o.collect_expired(1050, 3);
            entries.push_back({pick(), 1000 + rng() % 100});
            entries.push_back(entries.front());  // a key named twice
            return kythira::encode_redis_kv_command(
                kythira::redis_kv_sweep_command{._entries = entries});
        }
        case 5: {
            const auto k = pick();
            return kythira::encode_redis_kv_command(
                kythira::redis_kv_evict_command{._keys = {k, pick(), k}});
        }
        case 6:
            return kythira::encode_redis_kv_command(
                kythira::redis_kv_touch_command{._keys = {pick()}});
        default:
            return kythira::encode_redis_kv_command(kythira::redis_kv_put_chunk_command{
                ._key = pick(), ._sha = {}, ._bytes = bytes("chunk")});
    }
}

auto key_universe() -> std::vector<std::string> {
    std::vector<std::string> keys;
    for (char c = 'a'; c <= 'p'; ++c) {
        keys.emplace_back(std::string("key-") + c);
    }
    keys.emplace_back("");  // the empty key is a legal Redis key
    return keys;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(oracle_equivalence)

BOOST_AUTO_TEST_CASE(random_workloads_match_the_in_memory_machine) {
    const auto keys = key_universe();
    for (std::uint64_t seed = 1; seed <= 40; ++seed) {
        std::mt19937_64 rng(seed);
        memory_kv_engine engine;
        psm p("ns/");
        p.open(engine, k_everything);
        oracle o;
        for (std::uint64_t index = 1; index <= 150; ++index) {
            const auto cmd = random_command(rng, keys, o);
            const auto rp = p.apply(cmd, index);
            const auto ro = o.apply(cmd, index);
            BOOST_REQUIRE_MESSAGE(rp == ro, "reply differs at seed " << seed << " index " << index);
            BOOST_REQUIRE_EQUAL(p.applied_index(), index);
        }
        check_same(p, o, keys, 1050);

        // A restart reads back exactly the same machine.
        psm reopened("ns/");
        reopened.open(engine, k_everything);
        BOOST_REQUIRE_EQUAL(reopened.applied_index(), 150U);
        check_same(reopened, o, keys, 1050);
    }
}

BOOST_AUTO_TEST_CASE(for_each_key_visits_keys_in_order_without_the_namespace) {
    memory_kv_engine engine;
    psm p("tenant/");
    p.open(engine, k_everything);
    p.apply(set("b", "2"), 1);
    p.apply(set("a", "1", 99), 2);
    std::vector<std::string> seen;
    p.for_each_key([&](const std::string& k, const kythira::redis_kv_value_entry& e) {
        seen.push_back(
            k + "=" + std::string(reinterpret_cast<const char*>(e._value.data()), e._value.size()));
    });
    BOOST_TEST((seen == std::vector<std::string>{"a=1", "b=2"}));
    BOOST_TEST(engine.get(column_family::data, "tenant/a").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(atomic_apply_and_replay)

BOOST_AUTO_TEST_CASE(replay_at_or_below_applied_index_changes_nothing) {
    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    p.apply(set("k", "v1"), 1);
    p.apply(set("k", "v2"), 2);
    const auto seq = engine.latest_sequence();
    // A replay of entry 1 must not put v1 back.
    BOOST_TEST((p.apply(set("k", "v1"), 1) == std::vector<std::byte>{std::byte{0}}));
    BOOST_TEST((p.apply(del("k"), 2) == std::vector<std::byte>{std::byte{0}}));
    BOOST_TEST(engine.latest_sequence() == seq);
    BOOST_TEST((p.lookup("k")->_value == bytes("v2")));
    BOOST_TEST(p.applied_index() == 2U);
}

BOOST_AUTO_TEST_CASE(each_apply_is_one_batch_with_the_applied_index) {
    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    const auto before = engine.latest_sequence();
    p.apply(kythira::encode_redis_kv_command(kythira::redis_kv_evict_command{._keys = {"x", "y"}}),
            1);
    p.apply(set("x", "1"), 2);
    BOOST_TEST(engine.latest_sequence() == before + 2);
    // A no-op entry still records its index.
    psm reopened;
    reopened.open(engine, k_everything);
    BOOST_TEST(reopened.applied_index() == 2U);
}

// Requirement 8.1: a transactional wrapper stages several commands of one
// entry into one batch. Later commands see earlier ones, the counters carry
// across them, and the engine sees one write.
BOOST_AUTO_TEST_CASE(staged_commands_share_one_batch_and_see_each_other) {
    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    p.apply(set("a", "old"), 1);
    const auto before = engine.latest_sequence();

    kythira::write_batch batch;
    BOOST_TEST((p.stage(set("b", "1"), batch) == std::vector<std::byte>{std::byte{1}}));
    BOOST_TEST((p.stage(del("a"), batch) == std::vector<std::byte>{std::byte{1}}));
    // "a" is already deleted in this entry, so a second delete changes
    // nothing.
    BOOST_TEST((p.stage(del("a"), batch) == std::vector<std::byte>{std::byte{0}}));
    BOOST_TEST(engine.latest_sequence() == before);
    p.commit_staged(batch, 2);

    BOOST_TEST(engine.latest_sequence() == before + 1);
    BOOST_TEST(p.applied_index() == 2U);
    BOOST_TEST(p.approximate_key_count() == 1U);
    BOOST_TEST(!p.lookup("a"));
    psm reopened;
    reopened.open(engine, k_everything);
    BOOST_TEST(reopened.approximate_key_count() == 1U);
    BOOST_TEST(reopened.applied_index() == 2U);
}

BOOST_AUTO_TEST_CASE(discarded_stages_leave_nothing) {
    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    p.apply(set("a", "1"), 1);
    kythira::write_batch batch;
    std::ignore = p.stage(del("a"), batch);
    p.discard_staged();
    // A fresh entry reads the engine again, not the discarded delete.
    BOOST_TEST((p.apply(del("a"), 2) == std::vector<std::byte>{std::byte{1}}));
    BOOST_TEST(p.approximate_key_count() == 0U);
}

BOOST_AUTO_TEST_CASE(crash_loses_only_undurable_batches_and_replay_restores_them) {
    const auto keys = key_universe();
    std::mt19937_64 rng(7);
    std::vector<std::vector<std::byte>> log;
    oracle o;
    for (std::uint64_t i = 1; i <= 60; ++i) {
        log.push_back(random_command(rng, keys, o));
        o.apply(log.back(), i);
    }

    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    for (std::uint64_t i = 1; i <= 20; ++i) {
        p.apply(log[i - 1], i);
    }
    BOOST_TEST(p.durable_applied_index() == 20U);

    engine.hold_durability();
    for (std::uint64_t i = 21; i <= 45; ++i) {
        p.apply(log[i - 1], i);
    }
    BOOST_TEST(p.applied_index() == 45U);
    BOOST_TEST(p.durable_applied_index() == 20U);  // log compaction must stop here

    // Make part of the held writes durable: entries up to 30.
    p.apply(log[45], 46);
    engine.sync(engine.durable_sequence() + 10);
    BOOST_TEST(p.durable_applied_index() == 30U);

    engine.simulate_crash();
    psm recovered;
    recovered.open(engine, k_everything);
    BOOST_TEST(recovered.applied_index() == 30U);
    BOOST_TEST(recovered.durable_applied_index() == 30U);

    // The node replays from applied_index() + 1, and from further back too:
    // the overlap is harmless.
    for (std::uint64_t i = 25; i <= 60; ++i) {
        recovered.apply(log[i - 1], i);
    }
    check_same(recovered, o, keys, 1050);
}

// The node asks for this before compacting when the durable index lags
// (raft_configuration::_persistent_sync_lag_entries). Without it, on an
// engine that syncs only when asked (RocksDB), the durable index never
// moved and a gateway on engine storage never compacted its log: the
// engine chaos scenario's bulk load found it.
BOOST_AUTO_TEST_CASE(sync_durable_makes_every_applied_batch_durable) {
    memory_kv_engine engine;
    psm p;
    p.open(engine, k_everything);
    p.sync_durable();  // nothing applied yet: a no-op
    engine.hold_durability();
    for (std::uint64_t i = 1; i <= 12; ++i) {
        p.apply(set("k" + std::to_string(i), "v"), i);
    }
    BOOST_TEST(p.durable_applied_index() == 0U);
    p.sync_durable();
    BOOST_TEST(p.durable_applied_index() == 12U);

    engine.simulate_crash();
    psm recovered;
    recovered.open(engine, k_everything);
    BOOST_TEST(recovered.applied_index() == 12U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ranges_and_movement)

BOOST_AUTO_TEST_CASE(checkpoint_and_ingest_move_a_group_between_engines) {
    temp_dir tmp;
    const key_range shard{.start = "g", .end = std::string("p")};
    memory_kv_engine leader_engine;
    psm leader("ns/");
    leader.open(leader_engine, shard);
    oracle o;
    std::uint64_t index = 0;
    for (const auto* k : {"g", "h1", "h2", "m", "o"}) {
        ++index;
        leader.apply(set(k, std::string(k) + "-value", index % 2 == 0 ? 1001 : 0), index);
        o.apply(set(k, std::string(k) + "-value", index % 2 == 0 ? 1001 : 0), index);
    }
    ++index;
    leader.apply(del("h2"), index);
    o.apply(del("h2"), index);

    const auto manifest = leader.checkpoint(tmp.path() / "cp");

    // The follower's engine already holds another group below the shard and
    // stale keys of its own inside it.
    memory_kv_engine follower_engine;
    psm neighbour("ns/");
    neighbour.open(follower_engine, key_range{.start = "a", .end = std::string("g")});
    neighbour.apply(set("b", "neighbour"), 1);
    psm follower("ns/");
    follower.open(follower_engine, shard);
    follower.apply(set("n-stale", "x"), 1);

    follower.ingest(tmp.path() / "cp", manifest, index);
    BOOST_TEST(follower.applied_index() == index);
    check_same(follower, o, {"g", "h1", "h2", "m", "o", "n-stale"}, 1050);
    BOOST_TEST(follower.lookup("n-stale") == nullptr);
    BOOST_TEST((neighbour.lookup("b")->_value == bytes("neighbour")));

    psm reopened("ns/");
    reopened.open(follower_engine, shard);
    BOOST_TEST(reopened.applied_index() == index);
    BOOST_TEST(reopened.approximate_key_count() == o.approximate_key_count());
}

BOOST_AUTO_TEST_CASE(split_moves_no_keys_and_adopt_undoes_it) {
    memory_kv_engine engine;
    psm parent("ns/");
    parent.open(engine, k_everything);
    oracle o;
    const std::vector<std::string> keys{"a", "c", "e", "g", "i", "k"};
    std::uint64_t index = 0;
    for (const auto& k : keys) {
        ++index;
        parent.apply(set(k, k + k, k == "e" ? 77 : 0), index);
        o.apply(set(k, k + k, k == "e" ? 77 : 0), index);
    }
    const auto writes_before = engine.latest_sequence();

    const auto children = parent.split_at({"g", "d"});
    BOOST_REQUIRE_EQUAL(children.size(), 2U);
    BOOST_TEST(children[0].start == "d");
    BOOST_TEST((children[0].end == std::optional<std::string>("g")));
    BOOST_TEST(children[1].start == "g");
    BOOST_TEST(!children[1].end.has_value());
    BOOST_TEST(engine.latest_sequence() == writes_before + 1);  // one batch
    BOOST_TEST(parent.approximate_key_count() == 2U);           // a, c

    psm middle("ns/");
    middle.open(engine, children[0]);
    psm upper("ns/");
    upper.open(engine, children[1]);
    BOOST_TEST(middle.applied_index() == index);
    BOOST_TEST(upper.applied_index() == index);
    BOOST_TEST(middle.approximate_key_count() == 1U);  // e
    BOOST_TEST(middle.expiring_key_count() == 1U);
    BOOST_TEST(upper.approximate_key_count() == 3U);  // g, i, k
    BOOST_TEST(parent.approximate_size_bytes() + middle.approximate_size_bytes() +
                   upper.approximate_size_bytes() ==
               o.approximate_size_bytes());
    BOOST_TEST(parent.lookup("a") != nullptr);
    BOOST_TEST(upper.lookup("k") != nullptr);

    // Merge back: middle adopts upper, then parent adopts middle.
    middle.adopt(children[1]);
    middle.adopt(children[1]);  // idempotent
    BOOST_TEST(!engine.get(column_family::meta, "ns/g").has_value());
    parent.adopt(middle.range());
    BOOST_TEST(!engine.get(column_family::meta, "ns/d").has_value());
    BOOST_TEST(!parent.range().end.has_value());
    BOOST_TEST(parent.approximate_key_count() == o.approximate_key_count());
    BOOST_TEST(parent.approximate_size_bytes() == o.approximate_size_bytes());
    BOOST_TEST(parent.expiring_key_count() == o.expiring_key_count());

    // A range that is not adjacent on either side is refused.
    psm lone("other/");
    lone.open(engine, key_range{.start = "a", .end = std::string("b")});
    BOOST_CHECK_THROW(lone.adopt(key_range{.start = "c", .end = std::string("d")}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(adopt_from_below_moves_the_meta_record_to_the_new_start) {
    memory_kv_engine engine;
    psm parent("ns/");
    parent.open(engine, k_everything);
    parent.apply(set("a", "aa"), 1);
    parent.apply(set("m", "mm"), 2);
    parent.apply(set("x", "xx"), 3);
    const auto children = parent.split_at({"k"});
    BOOST_REQUIRE_EQUAL(children.size(), 1U);

    // The upper child absorbs the parent's range below it, as a merge whose
    // target is the right-hand group does.
    psm upper("ns/");
    upper.open(engine, children[0]);
    upper.apply(set("y", "yy"), 4);
    upper.adopt(parent.range());
    upper.adopt(parent.range());  // idempotent
    BOOST_TEST(upper.range().start == "");
    BOOST_TEST(!upper.range().end.has_value());
    BOOST_TEST(!engine.get(column_family::meta, "ns/k").has_value());
    BOOST_TEST(upper.approximate_key_count() == 4U);
    BOOST_TEST(upper.lookup("a") != nullptr);

    // The record now lives at the new start and carries the target's index.
    psm reopened("ns/");
    reopened.open(engine, upper.range());
    BOOST_TEST(reopened.applied_index() == 4U);
    BOOST_TEST(reopened.approximate_key_count() == 4U);
}

BOOST_AUTO_TEST_CASE(namespaces_keep_hosts_apart_and_drop_is_scoped) {
    memory_kv_engine engine;
    psm one("host-1/");
    psm two("host-2/");
    one.open(engine, k_everything);
    two.open(engine, k_everything);
    one.apply(set("k", "one"), 1);
    two.apply(set("k", "two"), 1);
    two.apply(set("only-two", "x"), 2);
    BOOST_TEST((one.lookup("k")->_value == bytes("one")));
    BOOST_TEST(one.lookup("only-two") == nullptr);
    BOOST_TEST(one.approximate_key_count() == 1U);

    one.drop();
    BOOST_TEST(one.lookup("k") == nullptr);
    BOOST_TEST(one.applied_index() == 0U);
    BOOST_TEST((two.lookup("k")->_value == bytes("two")));
    psm reopened("host-2/");
    reopened.open(engine, k_everything);
    BOOST_TEST(reopened.applied_index() == 2U);
}

BOOST_AUTO_TEST_CASE(use_before_open_is_refused) {
    psm p;
    BOOST_CHECK_THROW(p.apply(set("k", "v"), 1), std::logic_error);
    BOOST_CHECK_THROW((void)p.lookup("k"), std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()
