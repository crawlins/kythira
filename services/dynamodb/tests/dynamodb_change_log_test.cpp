// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The change log (`.kiro/specs/dynamodb-compatible-api/` task 15,
// Requirement 14.1): the record and state codecs, one record per write that
// changed an item, timestamps that never go back, restart and replay, the
// lineage of streams across a split and a merge, and a snapshot install that
// fetches the active segment so the follower appends the same bytes as the
// leader. Runs the item state machine on memory_kv_engine with a real
// record_segment_store in a temporary directory.

#define BOOST_TEST_MODULE dynamodb_change_log_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ddb = kythira::dynamodb;
using kythira::memory_kv_engine;
using kythira::record_segment_store;

namespace {

using item_sm = ddb::dynamo_item_state_machine<>;

constexpr std::uint64_t k_table = 7;
constexpr std::uint64_t k_now = 1794300000000ULL;

auto schema() -> ddb::key_schema {
    ddb::key_schema s;
    s.partition_name = "pk";
    s.partition_type = ddb::attribute_type::S;
    s.sort_name = "sk";
    s.sort_type = ddb::attribute_type::N;
    return s;
}

auto json_item(const char* text) -> ddb::item {
    auto parsed = ddb::item_from_json(boost::json::parse(text).as_object());
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

auto compile(const char* op, const char* input) -> ddb::expression_set {
    ddb::request_keys keys;
    keys.table = schema();
    auto compiled = ddb::compile_request(op, boost::json::parse(input).as_object(), keys);
    BOOST_REQUIRE(compiled.has_value());
    return *compiled;
}

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto temp_dir(const std::string& what) -> std::filesystem::path {
    std::random_device rd;
    auto p = std::filesystem::temp_directory_path() /
             ("kythira-ddb-log-" + what + "-" + std::to_string(rd()));
    std::filesystem::create_directories(p);
    return p;
}

auto store_at(const std::filesystem::path& dir) -> std::unique_ptr<record_segment_store> {
    kythira::record_segment_store_options o;
    o._data_dir = dir;
    o._barriers_enabled = false;
    return std::make_unique<record_segment_store>(o);
}

/// One replica: an engine, a store and the state machine over the table.
struct replica {
    std::filesystem::path dir;
    memory_kv_engine engine;
    std::unique_ptr<record_segment_store> store;
    item_sm sm;
    std::uint64_t index = 0;

    explicit replica(const std::string& name, bool with_store = true) : dir(temp_dir(name)) {
        if (with_store) {
            store = store_at(dir);
        }
        sm.set_change_log(store.get());
        sm.open(engine, ddb::table_range(k_table));
    }
    replica(const replica&) = delete;
    auto operator=(const replica&) -> replica& = delete;
    replica(replica&&) = delete;
    auto operator=(replica&&) -> replica& = delete;
    ~replica() {
        store.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    auto run(const ddb::item_command& c) -> ddb::item_result {
        const auto out = sm.apply(bytes_of(ddb::encode_item_command(c)), ++index);
        auto r = ddb::decode_item_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    static auto command(ddb::item_command::kind op, const char* it, const char* input,
                        std::uint64_t written_ms) -> ddb::item_command {
        ddb::item_command c;
        c.op = op;
        c.table_id = k_table;
        c.schema = schema();
        c.payload = json_item(it);
        c.expressions = compile(op == ddb::item_command::kind::put      ? "PutItem"
                                : op == ddb::item_command::kind::update ? "UpdateItem"
                                                                        : "DeleteItem",
                                input);
        c.log_changes = true;
        c.written_ms = written_ms;
        return c;
    }

    auto put(const char* it, const char* input = "{}", std::uint64_t at = k_now)
        -> ddb::item_result {
        return run(command(ddb::item_command::kind::put, it, input, at));
    }
    auto update(const char* key, const char* input, std::uint64_t at = k_now) -> ddb::item_result {
        return run(command(ddb::item_command::kind::update, key, input, at));
    }
    auto del(const char* key, const char* input = "{}", std::uint64_t at = k_now)
        -> ddb::item_result {
        return run(command(ddb::item_command::kind::delete_, key, input, at));
    }

    auto records(std::uint64_t from = 1) const
        -> std::vector<std::pair<std::uint64_t, ddb::change_record>> {
        return sm.read_changes(from);
    }
};

constexpr const char* k_a1 = R"({"pk":{"S":"a"},"sk":{"N":"1"}})";

auto key_of(const char* pk) -> std::string {
    return ddb::partition_prefix(k_table, ddb::attribute_value::string(pk));
}

}  // namespace

BOOST_AUTO_TEST_CASE(records_and_state_round_trip) {
    ddb::change_record r;
    r.event = ddb::change_event::modify;
    r.table_id = 42;
    r.keys = json_item(k_a1);
    r.old_image = json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"1"}})");
    r.new_image = json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"2"}})");
    r.ttl_delete = true;
    r.written_ms = k_now;
    BOOST_TEST((ddb::decode_change_record(ddb::encode_change_record(r)) == r));
    BOOST_TEST(!ddb::decode_change_record("").has_value());

    ddb::change_log_state s;
    s.epoch = 3;
    s.last_ms = k_now;
    s.current.key = ddb::change_stream_key("t/", 3);
    s.current.parents = {"ddb/x/2"};
    s.current.last_seq = 9;
    kythira::record_segment_descriptor d;
    d._stream = "ddb/x/2";
    d._first_seq = 1;
    d._last_seq = 4;
    d._records = 4;
    d._bytes = 300;
    d._sha256[0] = std::byte{7};
    d._sealed = true;
    s.current.sealed.push_back(d);
    s.current.active = d;
    s.closed.push_back(ddb::log_stream{"ddb/x/2", {}, {d}, std::nullopt, 1, 4});
    BOOST_TEST((ddb::decode_change_log_state(ddb::encode_change_log_state(s)) == s));
    auto bytes = ddb::encode_change_log_state(s);
    bytes.push_back('x');
    BOOST_TEST(!ddb::decode_change_log_state(bytes).has_value());
}

BOOST_AUTO_TEST_CASE(commands_without_log_fields_keep_format_two) {
    auto c = replica::command(ddb::item_command::kind::put, k_a1, "{}", 0);
    c.log_changes = false;
    BOOST_TEST(ddb::encode_item_command(c)[0] == '\2');
    c.log_changes = true;
    c.ttl_delete = true;
    c.written_ms = k_now;
    const auto bytes = ddb::encode_item_command(c);
    BOOST_TEST(bytes[0] == '\3');
    ddb::item_command back;
    BOOST_REQUIRE(ddb::decode_item_command(bytes, back) == ddb::item_command_status::ok);
    BOOST_TEST((back == c));
}

BOOST_AUTO_TEST_CASE(every_write_that_changes_an_item_appends_one_record) {
    replica r("writes");
    BOOST_TEST(!r.sm.change_log().has_value());
    (void)r.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"1"}})");
    (void)r.update(k_a1, R"({"UpdateExpression":"SET v = :v",)"
                         R"("ExpressionAttributeValues":{":v":{"N":"2"}}})");
    (void)r.del(k_a1);

    const auto got = r.records();
    BOOST_REQUIRE(got.size() == 3U);
    BOOST_TEST(got[0].first == 1U);
    BOOST_TEST(got[2].first == 3U);
    BOOST_TEST((got[0].second.event == ddb::change_event::insert));
    BOOST_TEST(!got[0].second.old_image.has_value());
    BOOST_TEST((got[0].second.keys == json_item(k_a1)));
    BOOST_TEST((got[1].second.event == ddb::change_event::modify));
    BOOST_TEST(
        (got[1].second.old_image == json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"1"}})")));
    BOOST_TEST(
        (got[1].second.new_image == json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"2"}})")));
    BOOST_TEST((got[2].second.event == ddb::change_event::remove));
    BOOST_TEST(!got[2].second.new_image.has_value());

    const auto& st = r.sm.change_log();
    BOOST_REQUIRE(st.has_value());
    BOOST_TEST(st->epoch == 0U);
    BOOST_TEST(st->current.key == ddb::change_stream_key(ddb::table_range(k_table).start, 0));
    BOOST_TEST(st->current.last_seq == 3U);
}

BOOST_AUTO_TEST_CASE(writes_that_change_nothing_append_nothing) {
    replica r("noop");
    (void)r.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"1"}})");
    // A failed condition, the same item again (in another attribute order),
    // an update to the value it has, and a delete of a missing item.
    (void)r.put(R"({"pk":{"S":"a"},"sk":{"N":"1"}})",
                R"j({"ConditionExpression":"attribute_not_exists(pk)"})j");
    (void)r.put(R"({"v":{"N":"1"},"sk":{"N":"1"},"pk":{"S":"a"}})");
    (void)r.update(k_a1, R"({"UpdateExpression":"SET v = :v",)"
                         R"("ExpressionAttributeValues":{":v":{"N":"1"}}})");
    (void)r.del(R"({"pk":{"S":"zz"},"sk":{"N":"1"}})");
    // A table whose changes are not logged.
    auto unlogged = replica::command(ddb::item_command::kind::put,
                                     R"({"pk":{"S":"b"},"sk":{"N":"1"}})", "{}", k_now);
    unlogged.log_changes = false;
    (void)r.run(unlogged);
    BOOST_TEST(r.records().size() == 1U);
    BOOST_TEST(r.sm.change_log()->current.last_seq == 1U);
}

BOOST_AUTO_TEST_CASE(record_times_never_go_back_and_carry_ttl_deletes) {
    replica r("times");
    (void)r.put(k_a1, "{}", k_now + 5000);
    auto ttl = replica::command(ddb::item_command::kind::delete_, k_a1, "{}", k_now);
    ttl.ttl_delete = true;
    (void)r.run(ttl);
    const auto got = r.records();
    BOOST_REQUIRE(got.size() == 2U);
    BOOST_TEST(got[1].second.written_ms == k_now + 5000);
    BOOST_TEST(got[1].second.ttl_delete);
    BOOST_TEST(!got[0].second.ttl_delete);
}

BOOST_AUTO_TEST_CASE(a_replica_without_a_store_keeps_the_same_state) {
    replica with("with");
    replica without("without", false);
    for (auto* r : {&with, &without}) {
        (void)r->put(k_a1);
        (void)r->put(R"({"pk":{"S":"b"},"sk":{"N":"1"}})");
    }
    BOOST_TEST(without.records().empty());
    BOOST_TEST(with.sm.change_log()->current.last_seq == without.sm.change_log()->current.last_seq);
    BOOST_TEST(with.sm.change_log()->epoch == without.sm.change_log()->epoch);
}

BOOST_AUTO_TEST_CASE(a_restart_drops_records_the_engine_never_covered) {
    replica r("restart");
    (void)r.put(k_a1);
    (void)r.put(R"({"pk":{"S":"b"},"sk":{"N":"1"}})");
    // A record appended whose entry never reached the engine (a crash
    // between the two): the reopened group truncates it, and the replayed
    // entry appends it again as seq 3.
    const auto key = r.sm.change_log()->current.key;
    const std::string stray = "stray";
    (void)r.store->append(
        key, kythira::segment_record_view{
                 3, static_cast<std::int64_t>(k_now),
                 std::span<const std::byte>(reinterpret_cast<const std::byte*>(stray.data()),
                                            stray.size())});
    r.store.reset();
    r.store = store_at(r.dir);
    item_sm reopened;
    reopened.set_change_log(r.store.get());
    reopened.open(r.engine, ddb::table_range(k_table));
    BOOST_TEST(r.store->last_seq(key).value_or(0) == 2U);
    BOOST_TEST(reopened.read_changes(1).size() == 2U);
}

BOOST_AUTO_TEST_CASE(a_split_closes_the_stream_and_opens_one_per_group) {
    replica r("split");
    (void)r.put(k_a1);
    (void)r.put(R"({"pk":{"S":"zzz"},"sk":{"N":"1"}})");
    const auto parent = r.sm.change_log()->current.key;
    // Split at a partition between the two items.
    const auto cut_a = key_of("a");
    const auto cut_z = key_of("zzz");
    const auto cut = std::max(cut_a, cut_z);
    const auto children = r.sm.split_at({cut});
    BOOST_REQUIRE(children.size() == 1U);

    // The parent's stream is sealed in the store and named by both groups.
    BOOST_TEST(!r.store->local_segments(parent).empty());
    const auto& mine = *r.sm.change_log();
    BOOST_TEST(mine.epoch == 1U);
    BOOST_TEST(mine.current.parents == std::vector<std::string>{parent});
    BOOST_TEST(mine.current.key == ddb::change_stream_key(ddb::table_range(k_table).start, 1));
    BOOST_REQUIRE(mine.closed.size() == 1U);
    BOOST_TEST(mine.closed[0].key == parent);
    BOOST_TEST(mine.closed[0].last_seq == 2U);
    BOOST_REQUIRE(mine.closed[0].sealed.size() == 1U);
    BOOST_TEST(mine.closed[0].sealed[0]._sealed);

    item_sm child;
    child.set_change_log(r.store.get());
    child.open(r.engine, children[0]);
    BOOST_REQUIRE(child.change_log().has_value());
    BOOST_TEST(child.change_log()->current.key == ddb::change_stream_key(cut, 1));
    BOOST_TEST(child.change_log()->current.parents == std::vector<std::string>{parent});

    // Later writes go to each group's own stream.
    const bool a_in_parent = cut_a < cut;
    const char* below = a_in_parent ? k_a1 : R"({"pk":{"S":"zzz"},"sk":{"N":"1"}})";
    (void)r.del(below);
    BOOST_TEST(r.sm.change_log()->current.last_seq == 1U);
    BOOST_TEST(r.records().size() == 1U);
    BOOST_TEST(child.change_log()->current.last_seq == 0U);
}

BOOST_AUTO_TEST_CASE(a_merge_continues_both_streams_in_one) {
    replica r("merge");
    (void)r.put(k_a1);
    const auto cut = std::max(key_of("a"), key_of("zzz"));
    const auto children = r.sm.split_at({cut});
    BOOST_REQUIRE(children.size() == 1U);
    const auto left = r.sm.change_log()->current.key;
    const auto right = ddb::change_stream_key(cut, 1);
    r.sm.adopt(children[0]);
    const auto& st = *r.sm.change_log();
    BOOST_TEST(st.epoch == 2U);
    BOOST_TEST(st.current.key == ddb::change_stream_key(ddb::table_range(k_table).start, 2));
    BOOST_TEST(st.current.parents == (std::vector<std::string>{left, right}));
    // The first stream and both halves are kept as closed streams.
    BOOST_TEST(st.closed.size() == 3U);
    BOOST_TEST(!r.engine.get(kythira::column_family::meta, ddb::change_log_state_key(cut)));
}

BOOST_AUTO_TEST_CASE(an_install_fetches_the_active_segment_and_appends_the_same_bytes) {
    replica leader("leader");
    (void)leader.put(k_a1);
    (void)leader.put(R"({"pk":{"S":"b"},"sk":{"N":"1"}})");
    const auto snap = leader.dir / "snap";
    const auto manifest = leader.sm.checkpoint(snap);

    replica follower("follower");
    int fetched = 0;
    follower.sm.set_change_log(
        follower.store.get(), [&](const kythira::record_segment_descriptor& d) {
            ++fetched;
            const std::vector<std::string> peers{"leader"};
            const bool ok = follower.store->fetch_segment(
                d, peers,
                [&](const std::string&, const std::string& stream, std::uint64_t first,
                    std::uint64_t length) {
                    return leader.store->read_segment_bytes(stream, first, length);
                });
            BOOST_REQUIRE(ok);
        });
    follower.sm.ingest(snap, manifest, leader.index);
    follower.index = leader.index;
    BOOST_TEST(fetched == 1);
    BOOST_TEST(follower.records().size() == 2U);

    // Both append the next write identically.
    (void)leader.put(R"({"pk":{"S":"c"},"sk":{"N":"1"}})");
    (void)follower.put(R"({"pk":{"S":"c"},"sk":{"N":"1"}})");
    const auto key = leader.sm.change_log()->current.key;
    BOOST_TEST((leader.store->describe(key, 1) == follower.store->describe(key, 1)));
    BOOST_TEST((leader.sm.change_log() == follower.sm.change_log()));

    // Without a fetcher an install that needs the segment refuses.
    replica stranger("stranger");
    BOOST_CHECK_THROW(stranger.sm.ingest(snap, manifest, leader.index),
                      kythira::record_segment_error);
}
