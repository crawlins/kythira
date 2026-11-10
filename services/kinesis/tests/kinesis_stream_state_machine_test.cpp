// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Task 5: the stream state machine (.kiro/specs/kinesis-compatible-streams/
// Requirements 6.2-6.4, 8.1, 8.3): catalog commands, per-shard counters and
// arrival clocks, the segment index, the split hook that protects routing
// keys, determinism across replicas, the split/absorb law, and restoring a
// snapshot onto a node that lacks the active segment.

#define BOOST_TEST_MODULE kinesis_stream_state_machine_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/record_segment_store.hpp>
#include <raft/splittable_state_machine.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace k = kythira::kinesis;

namespace {
const std::string test_account = "111122223333";
}  // namespace
using sm_type = k::stream_state_machine<std::uint64_t>;

static_assert(kythira::state_machine<sm_type, std::uint64_t>);
static_assert(kythira::splittable_state_machine<sm_type, std::string>);

BOOST_TEST_DONT_PRINT_LOG_VALUE(k::apply_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(k::stream_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::vector<std::byte>)

namespace {

/// A scratch directory removed at scope exit.
struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path = std::filesystem::temp_directory_path() /
                ("kinesis_sm_test." + std::to_string(::getpid()) + "." + std::to_string(++counter));
        std::filesystem::remove_all(_path);
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

/// Small segments so a test rolls several; no fsync (tmp).
auto make_store(const temp_dir& dir, std::uint64_t max_segment_bytes = 600)
    -> std::unique_ptr<kythira::record_segment_store> {
    return std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
        ._data_dir = dir._path,
        ._max_segment_bytes = max_segment_bytes,
        ._max_segment_age_ms = 0,
        ._max_record_bytes = 4096,
        ._index_interval_bytes = 128,
        ._barriers_enabled = false,
    });
}

struct driver {
    sm_type& _sm;
    std::uint64_t _index = 0;
    std::vector<std::vector<std::byte>> _log{};

    auto run(const k::stream_command& c) -> k::apply_result {
        _log.push_back(k::encode_command(c));
        return k::decode_result(_sm.apply(_log.back(), ++_index));
    }
};

auto append(const std::string& key, std::int64_t arrival, std::size_t n, std::size_t size = 40,
            std::uint32_t tag = 0) -> k::append_records_command {
    k::append_records_command c{._key = key, ._arrival_ms = arrival};
    for (std::size_t i = 0; i < n; ++i) {
        c._records.push_back({._partition_key = "pk-" + std::to_string(tag + i),
                              ._data = std::vector<std::byte>(size, std::byte(tag + i))});
    }
    return c;
}

/// Create a stream and open all its shards in this one instance (a single
/// group holding catalog and routing keys alike). Returns the stream id.
auto create_and_open(driver& d, const std::string& name, std::uint32_t shards) -> std::uint64_t {
    auto r = d.run(k::create_stream_command{
        ._account = test_account, ._name = name, ._shard_count = shards, ._created_ms = 1000});
    BOOST_REQUIRE(r._status == k::apply_status::ok);
    for (std::uint32_t i = 0; i < shards; ++i) {
        BOOST_REQUIRE(d.run(k::open_shard_command{k::routing_key(r._id, i)})._status ==
                      k::apply_status::ok);
    }
    BOOST_REQUIRE(
        d.run(k::set_stream_status_command{test_account, name, r._id, k::stream_status::active})
            ._status == k::apply_status::ok);
    return r._id;
}

/// A random but reproducible history: streams created, shards opened,
/// appends of varying sizes and arrival times, closes and trims.
auto random_history(std::uint64_t seed) -> std::vector<k::stream_command> {
    std::mt19937_64 rng(seed);
    std::vector<k::stream_command> out;
    std::vector<std::string> keys;
    std::uint64_t next_id = 1;
    for (int s = 0; s < 3; ++s) {
        const auto shards = static_cast<std::uint32_t>(1 + (rng() % 4));
        const auto name = "stream-" + std::to_string(s);
        out.emplace_back(k::create_stream_command{._account = test_account,
                                                  ._name = name,
                                                  ._shard_count = shards,
                                                  ._created_ms = 1000 + s});
        for (std::uint32_t i = 0; i < shards; ++i) {
            keys.push_back(k::routing_key(next_id, i));
            out.emplace_back(k::open_shard_command{keys.back()});
        }
        out.emplace_back(
            k::set_stream_status_command{test_account, name, next_id, k::stream_status::active});
        ++next_id;
    }
    std::int64_t clock = 10'000;
    for (int i = 0; i < 120; ++i) {
        const auto& key = keys[rng() % keys.size()];
        // The clock wanders backwards sometimes, as leaders' clocks do.
        clock += static_cast<std::int64_t>(rng() % 50) - 10;
        const auto roll = rng() % 20;
        if (roll == 0) {
            out.emplace_back(k::trim_shard_command{key, rng() % 30});
        } else if (roll == 1 && i > 100) {
            out.emplace_back(k::close_shard_command{key});
        } else {
            out.emplace_back(append(key, clock, 1 + (rng() % 5), 10 + (rng() % 120),
                                    static_cast<std::uint32_t>(i * 7)));
        }
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(catalog_commands)

BOOST_AUTO_TEST_CASE(create_allocates_ids_and_an_even_shard_table) {
    sm_type sm;
    driver d{sm};
    auto r = d.run(k::create_stream_command{._account = test_account,
                                            ._name = "orders",
                                            ._shard_count = 3,
                                            ._retention_hours = 48,
                                            ._created_ms = 5,
                                            ._tags = {{"a", "b"}}});
    BOOST_TEST(r._status == k::apply_status::ok);
    BOOST_TEST(r._id == 1U);
    const auto* s = sm.find_stream(test_account, "orders");
    BOOST_TEST_REQUIRE(s != nullptr);
    BOOST_TEST(s->_status == k::stream_status::creating);
    BOOST_TEST(s->_shards.size() == 3U);
    BOOST_TEST(s->_next_ordinal == 3U);
    BOOST_TEST(s->_retention_hours == 48U);
    BOOST_TEST(s->_created_ms == 5);
    BOOST_TEST(s->_tags.at("a") == "b");
    BOOST_TEST((s->_shards == k::initial_shards(3)));
    BOOST_TEST(sm.next_stream_id() == 2U);

    BOOST_TEST(
        d.run(k::create_stream_command{._account = test_account, ._name = "orders"})._status ==
        k::apply_status::already_exists);
    BOOST_TEST(
        d.run(k::create_stream_command{._account = test_account, ._name = "bad/name"})._status ==
        k::apply_status::invalid);
    BOOST_TEST(d.run(k::create_stream_command{
                         ._account = test_account, ._name = "zero", ._shard_count = 0})
                   ._status == k::apply_status::invalid);
    // Refused creates consume no id.
    BOOST_TEST(sm.next_stream_id() == 2U);
}

// Requirement 6.3: a recreated stream gets a new id, so new routing keys.
BOOST_AUTO_TEST_CASE(ids_are_never_reused) {
    sm_type sm;
    driver d{sm};
    const auto first = create_and_open(d, "s", 1);
    BOOST_TEST(
        d.run(k::set_stream_status_command{test_account, "s", first, k::stream_status::deleting})
            ._status == k::apply_status::ok);
    BOOST_TEST(d.run(k::drop_shard_command{k::routing_key(first, 0)})._status ==
               k::apply_status::ok);
    BOOST_TEST(d.run(k::remove_stream_command{test_account, "s", first})._status ==
               k::apply_status::ok);
    BOOST_TEST(sm.find_stream(test_account, "s") == nullptr);
    const auto second = create_and_open(d, "s", 1);
    BOOST_TEST(second != first);
    BOOST_TEST(second == first + 1);
    BOOST_TEST(sm.find_shard(k::routing_key(first, 0)) == nullptr);
    BOOST_TEST(sm.find_shard(k::routing_key(second, 0))->_last_counter == 0U);
}

BOOST_AUTO_TEST_CASE(status_changes_follow_the_lifecycle) {
    sm_type sm;
    driver d{sm};
    const auto id = d.run(k::create_stream_command{._account = test_account, ._name = "s"})._id;
    using enum k::stream_status;
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, updating})._status ==
               k::apply_status::bad_transition);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, active})._status ==
               k::apply_status::ok);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, updating})._status ==
               k::apply_status::ok);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, active})._status ==
               k::apply_status::ok);
    // The id guards against acting on a recreated stream of the same name.
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id + 1, deleting})._status ==
               k::apply_status::not_found);
    BOOST_TEST(d.run(k::remove_stream_command{test_account, "s", id + 1})._status ==
               k::apply_status::not_found);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, deleting})._status ==
               k::apply_status::ok);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "s", id, active})._status ==
               k::apply_status::bad_transition);
    BOOST_TEST(sm.find_stream(test_account, "s")->_status == deleting);
    BOOST_TEST(d.run(k::set_stream_status_command{test_account, "nope", 1, active})._status ==
               k::apply_status::not_found);
}

BOOST_AUTO_TEST_CASE(list_streams_in_name_order_with_paging) {
    sm_type sm;
    driver d{sm};
    for (const auto* n : {"charlie", "alpha", "bravo", "delta"}) {
        (void)d.run(k::create_stream_command{._account = test_account, ._name = n});
    }
    std::vector<std::string> names;
    for (const auto* s : sm.list_streams(test_account)) {
        names.push_back(s->_name);
    }
    BOOST_TEST((names == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));
    auto page = sm.list_streams(test_account, "bravo", 1);
    BOOST_TEST_REQUIRE(page.size() == 1U);
    BOOST_TEST(page[0]->_name == "charlie");
}

BOOST_AUTO_TEST_CASE(stream_names_are_per_account) {
    // Two accounts may each have a stream called `orders`; they get
    // different ids, so different routing keys and records.
    sm_type sm;
    driver d{sm};
    const auto mine = d.run(k::create_stream_command{._account = test_account, ._name = "orders"});
    const auto theirs =
        d.run(k::create_stream_command{._account = "444455556666", ._name = "orders"});
    BOOST_TEST(mine._status == k::apply_status::ok);
    BOOST_TEST(theirs._status == k::apply_status::ok);
    BOOST_TEST(mine._id != theirs._id);
    BOOST_TEST(sm.find_stream(test_account, "orders")->_id == mine._id);
    BOOST_TEST(sm.find_stream("444455556666", "orders")->_id == theirs._id);
    BOOST_TEST(sm.find_stream("777788889999", "orders") == nullptr);
    BOOST_TEST(sm.list_streams(test_account).size() == 1U);
    BOOST_TEST(sm.find_stream(test_account, "orders")->_account == test_account);
    // A status change names its account too.
    BOOST_TEST(d.run(k::set_stream_status_command{"444455556666", "orders", mine._id,
                                                  k::stream_status::active})
                   ._status == k::apply_status::not_found);
    BOOST_TEST(d.run(k::create_stream_command{._account = "", ._name = "x"})._status ==
               k::apply_status::invalid);
    BOOST_TEST(d.run(k::create_stream_command{._account = "a/b", ._name = "x"})._status ==
               k::apply_status::invalid);
}

BOOST_AUTO_TEST_CASE(malformed_commands_are_refused_alike) {
    sm_type sm;
    auto r = k::decode_result(sm.apply({std::byte{9}, std::byte{9}}, 1));
    BOOST_TEST(r._status == k::apply_status::invalid);
    auto good = k::encode_command(k::create_stream_command{._account = test_account, ._name = "s"});
    good.pop_back();
    BOOST_TEST(k::decode_result(sm.apply(good, 2))._status == k::apply_status::invalid);
    BOOST_TEST(sm.entries().empty());
}

BOOST_AUTO_TEST_CASE(commands_round_trip_and_name_their_key) {
    for (const auto& c : random_history(3)) {
        const auto bytes = k::encode_command(c);
        BOOST_TEST((k::encode_command(k::decode_command(bytes)) == bytes));
    }
    BOOST_TEST(k::command_key(k::create_stream_command{._account = test_account, ._name = "x"}) ==
               k::stream_key(test_account, "x"));
    BOOST_TEST(k::command_key(k::close_shard_command{k::routing_key(1, 2)}) ==
               k::routing_key(1, 2));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(shard_commands)

// Requirements 8.1 and 8.3.
BOOST_AUTO_TEST_CASE(counters_increase_and_arrival_never_goes_back) {
    sm_type sm;
    driver d{sm};
    const auto id = create_and_open(d, "s", 2);
    const auto key = k::routing_key(id, 1);
    auto a = d.run(append(key, 5000, 3));
    BOOST_TEST(a._status == k::apply_status::ok);
    BOOST_TEST(a._first_counter == 1U);
    BOOST_TEST(a._last_counter == 3U);
    BOOST_TEST(a._arrival_ms == 5000);
    // An earlier wall clock (a new leader behind the old one) is clamped.
    auto b = d.run(append(key, 4000, 2));
    BOOST_TEST(b._first_counter == 4U);
    BOOST_TEST(b._last_counter == 5U);
    BOOST_TEST(b._arrival_ms == 5000);
    auto c = d.run(append(key, 6000, 1));
    BOOST_TEST(c._arrival_ms == 6000);
    // The other shard has its own counter.
    BOOST_TEST(d.run(append(k::routing_key(id, 0), 1, 1))._first_counter == 1U);
    const auto* s = sm.find_shard(key);
    BOOST_TEST(s->_last_counter == 6U);
    BOOST_TEST(s->_records == 6U);
    BOOST_TEST(s->_last_arrival_ms == 6000);
}

BOOST_AUTO_TEST_CASE(refusals) {
    sm_type sm;
    driver d{sm};
    const auto id = create_and_open(d, "s", 1);
    const auto key = k::routing_key(id, 0);
    BOOST_TEST(d.run(append(k::routing_key(id, 5), 1, 1))._status == k::apply_status::not_found);
    BOOST_TEST(d.run(k::append_records_command{._key = key})._status == k::apply_status::invalid);
    auto bad_pk = append(key, 1, 1);
    bad_pk._records[0]._partition_key.clear();
    BOOST_TEST(d.run(bad_pk)._status == k::apply_status::invalid);
    bad_pk._records[0]._partition_key = std::string(257, 'p');
    BOOST_TEST(d.run(bad_pk)._status == k::apply_status::invalid);
    BOOST_TEST(d.run(k::open_shard_command{key})._status == k::apply_status::already_exists);
    BOOST_TEST(d.run(k::open_shard_command{"k/nothex"})._status == k::apply_status::invalid);
    BOOST_TEST(sm.find_shard(key)->_last_counter == 0U);

    BOOST_TEST(d.run(k::close_shard_command{key})._status == k::apply_status::ok);
    BOOST_TEST(d.run(append(key, 1, 1))._status == k::apply_status::shard_closed);
    BOOST_TEST(d.run(k::close_shard_command{key})._status == k::apply_status::ok);
    BOOST_TEST(d.run(k::close_shard_command{"k/x"})._status == k::apply_status::not_found);
    BOOST_TEST(d.run(k::trim_shard_command{"k/x", 1})._status == k::apply_status::not_found);
    BOOST_TEST(d.run(k::drop_shard_command{"k/x"})._status == k::apply_status::not_found);
}

BOOST_AUTO_TEST_CASE(child_shards_start_at_their_counter) {
    sm_type sm;
    driver d{sm};
    const auto key = k::routing_key(9, 4);
    BOOST_TEST(d.run(k::open_shard_command{key, 100})._status == k::apply_status::ok);
    BOOST_TEST(d.run(append(key, 1, 2))._first_counter == 101U);
}

BOOST_AUTO_TEST_CASE(records_land_in_the_store_with_index_and_trim) {
    temp_dir dir;
    auto store = make_store(dir);
    sm_type sm(store.get());
    driver d{sm};
    const auto id = create_and_open(d, "s", 1);
    const auto key = k::routing_key(id, 0);
    for (int i = 0; i < 10; ++i) {
        BOOST_TEST(d.run(append(key, 1000 + i, 3, 50, static_cast<std::uint32_t>(i * 3)))._status ==
                   k::apply_status::ok);
    }
    const auto* s = sm.find_shard(key);
    // 30 records of ~70 bytes in 600-byte segments: several rolled.
    BOOST_TEST_REQUIRE(s->_sealed.size() >= 3U);
    BOOST_TEST(s->_sealed.front()._first_seq == 1U);
    for (std::size_t i = 1; i < s->_sealed.size(); ++i) {
        BOOST_TEST(s->_sealed[i]._first_seq == s->_sealed[i - 1]._last_seq + 1);
        BOOST_TEST(s->_sealed[i]._sealed);
    }

    // Records read back carry counter, arrival time and the partition key.
    auto read = store->read(key, 1);
    BOOST_TEST_REQUIRE(read._records.size() == 30U);
    for (std::size_t i = 0; i < 30; ++i) {
        BOOST_TEST(read._records[i]._seq == i + 1);
        BOOST_TEST(read._records[i]._timestamp_ms == 1000 + static_cast<std::int64_t>(i / 3));
        const auto rec = k::decode_stored_record(read._records[i]._data);
        BOOST_TEST(rec._partition_key == "pk-" + std::to_string(i));
        BOOST_TEST(rec._data.size() == 50U);
    }

    // Trim below the second sealed segment's end: the first goes, from the
    // index and the store alike.
    const auto cut = s->_sealed[1]._first_seq;
    const auto bytes_before = s->_retained_bytes;
    BOOST_TEST(d.run(k::trim_shard_command{key, cut})._status == k::apply_status::ok);
    BOOST_TEST(s->_trim_before == cut);
    BOOST_TEST(s->_sealed.front()._first_seq == cut);
    BOOST_TEST(s->_retained_bytes < bytes_before);
    BOOST_TEST(store->local_segments(key).front()._first_seq == cut);
    // A lower trim is a no-op; a trim past the end stops at the end.
    BOOST_TEST(d.run(k::trim_shard_command{key, 1})._status == k::apply_status::ok);
    BOOST_TEST(s->_trim_before == cut);
    BOOST_TEST(d.run(k::trim_shard_command{key, 1'000'000})._status == k::apply_status::ok);
    BOOST_TEST(s->_trim_before == s->_last_counter + 1);

    // Closing seals the active segment into the index.
    BOOST_TEST(d.run(k::close_shard_command{key})._status == k::apply_status::ok);
    BOOST_TEST(s->_sealed.back()._last_seq == 30U);
    BOOST_TEST(store->local_segments(key).back()._sealed);

    // Dropping the shard deletes its segments.
    BOOST_TEST(d.run(k::drop_shard_command{key})._status == k::apply_status::ok);
    BOOST_TEST(store->local_segments(key).empty());
}

BOOST_AUTO_TEST_CASE(a_full_log_replay_over_the_same_store_rewrites_the_shard) {
    // A restart with no snapshot replays the whole log into a fresh machine
    // over the files the first life left behind. The replayed appends must
    // not collide with those files.
    temp_dir dir;
    std::vector<std::vector<std::byte>> log;
    std::size_t sealed_before = 0;
    {
        auto store = make_store(dir);
        sm_type sm(store.get());
        driver d{sm};
        const auto id = create_and_open(d, "s", 1);
        const auto key = k::routing_key(id, 0);
        for (int i = 0; i < 10; ++i) {
            BOOST_TEST(
                d.run(append(key, 1000 + i, 3, 50, static_cast<std::uint32_t>(i * 3)))._status ==
                k::apply_status::ok);
        }
        sealed_before = sm.find_shard(key)->_sealed.size();
        log = d._log;
    }
    auto store = make_store(dir);
    sm_type sm(store.get());
    std::uint64_t index = 0;
    for (const auto& cmd : log) {
        BOOST_TEST(k::decode_result(sm.apply(cmd, ++index))._status == k::apply_status::ok);
    }
    const auto key = k::routing_key(sm.find_stream(test_account, "s")->_id, 0);
    const auto* s = sm.find_shard(key);
    BOOST_TEST_REQUIRE(s != nullptr);
    BOOST_TEST(s->_last_counter == 30U);
    BOOST_TEST(s->_sealed.size() == sealed_before);
    const auto read = store->read(key, 1);
    BOOST_TEST(read._records.size() == 30U);
}

BOOST_AUTO_TEST_CASE(a_record_over_the_store_limit_is_refused_before_any_write) {
    temp_dir dir;
    auto store = make_store(dir);
    sm_type sm(store.get());
    driver d{sm};
    const auto key = k::routing_key(1, 0);
    (void)d.run(k::open_shard_command{key});
    auto c = append(key, 1, 2);
    c._records[1]._data.assign(5000, std::byte{1});
    BOOST_TEST(d.run(c)._status == k::apply_status::invalid);
    BOOST_TEST(sm.find_shard(key)->_last_counter == 0U);
    BOOST_TEST(!store->last_seq(key).has_value());
}

BOOST_AUTO_TEST_CASE(a_batch_appends_to_each_shard_and_answers_per_part) {
    temp_dir dir;
    auto store = make_store(dir, 64 * 1024);
    sm_type sm(store.get());
    driver d{sm};
    const auto id = create_and_open(d, "s", 3);
    const auto k0 = k::routing_key(id, 0);
    const auto k1 = k::routing_key(id, 1);
    const auto k2 = k::routing_key(id, 2);
    BOOST_TEST(d.run(append(k1, 1000, 2))._last_counter == 2U);
    BOOST_TEST(d.run(k::close_shard_command{k2})._status == k::apply_status::ok);

    const k::append_batch_command batch{._appends = {append(k0, 2000, 3), append(k1, 2000, 1),
                                                     append(k2, 2000, 1),
                                                     append(k::routing_key(id, 9), 2000, 1)}};
    BOOST_TEST(k::command_key(batch) == k0);
    const auto r = d.run(batch);
    BOOST_TEST((r._status == k::apply_status::ok));
    BOOST_TEST_REQUIRE(r._parts.size() == 4U);
    BOOST_TEST((r._parts[0]._status == k::apply_status::ok));
    BOOST_TEST(r._parts[0]._first_counter == 1U);
    BOOST_TEST(r._parts[0]._last_counter == 3U);
    BOOST_TEST(r._parts[0]._arrival_ms == 2000);
    BOOST_TEST(r._parts[1]._first_counter == 3U);  // after the earlier two
    // A closed shard and a shard this group does not hold are refused and
    // change nothing; the other parts commit regardless.
    BOOST_TEST((r._parts[2]._status == k::apply_status::shard_closed));
    BOOST_TEST((r._parts[3]._status == k::apply_status::not_found));
    BOOST_TEST(sm.find_shard(k2)->_last_counter == 0U);
    BOOST_TEST(store->read(k0, 1)._records.size() == 3U);
    BOOST_TEST(store->read(k1, 1)._records.size() == 3U);

    // An empty batch is malformed.
    BOOST_TEST((d.run(k::append_batch_command{})._status == k::apply_status::invalid));
}

BOOST_AUTO_TEST_CASE(batches_and_their_results_round_trip) {
    const k::append_batch_command batch{
        ._appends = {append(k::routing_key(1, 0), 5, 2), append(k::routing_key(1, 1), 6, 1)}};
    const auto bytes = k::encode_command(batch);
    BOOST_TEST((k::encode_command(k::decode_command(bytes)) == bytes));

    const k::apply_result with_parts{
        ._parts = {{._status = k::apply_status::ok, ._first_counter = 1, ._last_counter = 2},
                   {._status = k::apply_status::not_found}}};
    BOOST_TEST((k::decode_result(k::encode_result(with_parts)) == with_parts));
    // A result without parts encodes exactly as before batches existed.
    const k::apply_result plain{._first_counter = 7, ._last_counter = 9, ._arrival_ms = 3};
    BOOST_TEST(k::encode_result(plain).size() == 1 + 8 * 4U);
    BOOST_TEST((k::decode_result(k::encode_result(plain)) == plain));
    // A leader's throttled answer crosses the forwarding hop too, whole or
    // per part.
    const k::apply_result throttled{._status = k::apply_status::throttled};
    BOOST_TEST((k::decode_result(k::encode_result(throttled)) == throttled));
    const k::apply_result some_throttled{
        ._parts = {{._status = k::apply_status::ok, ._first_counter = 1, ._last_counter = 1},
                   {._status = k::apply_status::throttled}}};
    BOOST_TEST((k::decode_result(k::encode_result(some_throttled)) == some_throttled));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(replication)

// Every replica applying the same log holds the same state, segment index
// included, whatever the leader's clock did.
BOOST_AUTO_TEST_CASE(replicas_agree_byte_for_byte) {
    temp_dir da;
    temp_dir db;
    auto store_a = make_store(da);
    auto store_b = make_store(db);
    sm_type a(store_a.get());
    sm_type b(store_b.get());
    std::uint64_t i = 0;
    for (const auto& c : random_history(17)) {
        const auto bytes = k::encode_command(c);
        ++i;
        BOOST_TEST((a.apply(bytes, i) == b.apply(bytes, i)));
    }
    BOOST_TEST((a.get_state() == b.get_state()));
    for (const auto& key : store_a->streams()) {
        BOOST_TEST((store_a->local_segments(key) == store_b->local_segments(key)));
    }
}

// Requirement 6.4.
BOOST_AUTO_TEST_CASE(split_points_never_cut_a_routing_key_or_the_catalog) {
    sm_type sm;
    const auto rk = k::routing_key(3, 1);
    BOOST_TEST(sm.can_split_at(rk));
    BOOST_TEST(!sm.can_split_at(rk + "/sub"));
    BOOST_TEST(!sm.can_split_at(rk + "\x01"));
    BOOST_TEST(sm.can_split_at(k::routing_prefix(3)));
    BOOST_TEST(sm.can_split_at("k/"));
    BOOST_TEST(sm.can_split_at("m"));
    BOOST_TEST(sm.can_split_at(k::catalog_prefix()));
    BOOST_TEST(!sm.can_split_at(k::next_id_key()));
    BOOST_TEST(!sm.can_split_at(k::stream_key(test_account, "x")));
    BOOST_TEST(!sm.can_split_at(k::streams_prefix()));

    driver d{sm};
    for (int s = 0; s < 3; ++s) {
        const auto id = create_and_open(d, "s" + std::to_string(s), 4);
        for (std::uint32_t o = 0; o < 4; ++o) {
            (void)d.run(append(k::routing_key(id, o), 1, 1 + o, 100 * (o + 1)));
        }
    }
    auto keys = sm.suggest_split_keys(5);
    BOOST_TEST(!keys.empty());
    BOOST_TEST(keys.size() <= 5U);
    for (const auto& key : keys) {
        BOOST_TEST(k::parse_routing_key(key).has_value());
        BOOST_TEST(sm.can_split_at(key));
        BOOST_TEST(key != sm.entries().begin()->first);
    }
    BOOST_TEST(std::is_sorted(keys.begin(), keys.end()));
    BOOST_TEST(sm.suggest_split_keys(0).empty());
    BOOST_TEST(sm.approximate_key_count() == sm.entries().size());
    BOOST_TEST(sm.approximate_size_bytes() > 0U);
}

// The split/absorb law, with and without a (shared) store: splitting at any
// cuts and absorbing the parts back reproduces get_state() exactly.
BOOST_AUTO_TEST_CASE(split_then_absorb_is_the_identity) {
    for (const bool with_store : {false, true}) {
        temp_dir dir;
        auto store = with_store ? make_store(dir) : nullptr;
        sm_type sm(store.get());
        std::uint64_t index = 0;
        for (const auto& c : random_history(23)) {
            (void)sm.apply(k::encode_command(c), ++index);
        }
        const auto before = sm.get_state();
        std::vector<std::string> all;
        for (const auto& [key, e] : sm.entries()) {
            all.push_back(key);
        }
        std::mt19937_64 rng(with_store ? 1 : 2);
        for (int round = 0; round < 20; ++round) {
            std::vector<std::string> cuts;
            const auto n = 1 + (rng() % 4);
            for (std::size_t i = 0; i < n; ++i) {
                cuts.push_back(all[rng() % all.size()]);
            }
            cuts.emplace_back("k/");  // between catalog and routing keys
            auto parts = sm.split_state(cuts);
            BOOST_TEST_REQUIRE(parts.size() >= 2U);

            sm_type joined(store.get());
            joined.restore_from_snapshot(parts.front(), index);
            for (std::size_t p = 1; p < parts.size(); ++p) {
                joined.absorb(parts[p], {});
            }
            BOOST_TEST((joined.get_state() == before));
        }
        // Splitting at a key past everything leaves an empty right part.
        auto parts = sm.split_state({"\xff"});
        BOOST_TEST((parts.front() == before));
        sm_type empty;
        empty.restore_from_snapshot(parts.back(), index);
        BOOST_TEST(empty.entries().empty());
    }
}

BOOST_AUTO_TEST_CASE(snapshot_decoding_refuses_truncation) {
    sm_type sm;
    std::uint64_t index = 0;
    for (const auto& c : random_history(5)) {
        (void)sm.apply(k::encode_command(c), ++index);
    }
    const auto state = sm.get_state();
    for (std::size_t n = 0; n < state.size(); n += 7) {
        sm_type other;
        BOOST_CHECK_THROW(
            other.restore_from_snapshot(
                std::vector<std::byte>(state.begin(), state.begin() + static_cast<long>(n)), 1),
            std::exception);
    }
}

// A replica restoring a snapshot onto an empty store fetches the active
// segment through the hook, then appends into the same segment as the
// replica that took the snapshot: the two stay byte-identical.
BOOST_AUTO_TEST_CASE(restore_fetches_the_active_prefix_and_stays_identical) {
    temp_dir da;
    temp_dir db;
    auto store_a = make_store(da);
    auto store_b = make_store(db);
    sm_type a(store_a.get());
    driver d{a};
    const auto id = create_and_open(d, "s", 2);
    for (int i = 0; i < 12; ++i) {
        (void)d.run(
            append(k::routing_key(id, i % 2), 100 + i, 2, 60, static_cast<std::uint32_t>(i * 2)));
    }
    const auto snap = a.get_state();
    const auto snap_index = d._index;

    std::vector<std::string> fetched;
    auto fetch = [&](const kythira::record_segment_descriptor& desc) {
        fetched.push_back(desc._stream);
        const std::vector<std::string> peers{"a"};
        const bool ok =
            store_b->fetch_segment(desc, peers,
                                   [&](const std::string&, const std::string& stream,
                                       std::uint64_t first, std::uint64_t length) {
                                       return store_a->read_segment_bytes(stream, first, length);
                                   });
        if (!ok) {
            throw kythira::record_segment_error("fetch failed");
        }
    };
    sm_type b(store_b.get(), fetch);
    b.restore_from_snapshot(snap, snap_index);
    BOOST_TEST(fetched.size() == 2U);  // one active segment per shard
    BOOST_TEST((b.get_state() == snap));

    // Both apply the same tail.
    driver db_drv{b, snap_index};
    for (int i = 0; i < 12; ++i) {
        const auto c =
            append(k::routing_key(id, i % 2), 200 + i, 3, 60, static_cast<std::uint32_t>(100 + i));
        BOOST_TEST((d.run(c) == db_drv.run(c)));
    }
    BOOST_TEST((a.get_state() == b.get_state()));
    // Same active segment on both; B holds no sealed segment from before the
    // snapshot (those are fetched lazily on read).
    for (std::uint32_t o = 0; o < 2; ++o) {
        const auto key = k::routing_key(id, o);
        BOOST_TEST((store_a->local_segments(key).back() == store_b->local_segments(key).back()));
    }

    // Without a fetcher, a missing active segment stops the restore.
    temp_dir dc;
    auto store_c = make_store(dc);
    sm_type c(store_c.get());
    BOOST_CHECK_THROW(c.restore_from_snapshot(snap, snap_index), kythira::record_segment_error);
}

// After a crash, segments may hold records past the snapshot whose entries
// will be replayed; restore cuts them so the replay does not collide.
BOOST_AUTO_TEST_CASE(restore_truncates_a_tail_past_the_snapshot) {
    temp_dir dir;
    auto store = make_store(dir, 1 << 20);
    sm_type a(store.get());
    driver d{a};
    const auto id = create_and_open(d, "s", 1);
    const auto key = k::routing_key(id, 0);
    (void)d.run(append(key, 1, 4));
    const auto snap = a.get_state();
    const auto snap_index = d._index;
    const auto replay = append(key, 2, 3, 40, 50);
    (void)d.run(replay);
    BOOST_TEST(*store->last_seq(key) == 7U);

    sm_type b(store.get());
    b.restore_from_snapshot(snap, snap_index);
    BOOST_TEST(*store->last_seq(key) == 4U);
    driver replay_drv{b, snap_index};
    auto r = replay_drv.run(replay);
    BOOST_TEST(r._status == k::apply_status::ok);
    BOOST_TEST(r._first_counter == 5U);
    BOOST_TEST((b.get_state() == a.get_state()));
}

BOOST_AUTO_TEST_SUITE_END()
