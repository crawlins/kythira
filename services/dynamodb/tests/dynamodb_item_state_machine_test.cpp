// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The item state machine and key layout (`.kiro/specs/dynamodb-compatible-api/`
// task 6): conditions evaluated in apply() against the committed item,
// atomic writes with the applied index and item versions, ReturnValues,
// replay, restart, checkpoints, splits only between item collections, and
// the bounded background drop of a deleted table. Runs on memory_kv_engine.

#define BOOST_TEST_MODULE dynamodb_item_state_machine_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace ddb = kythira::dynamodb;
using kythira::column_family;
using kythira::key_range;
using kythira::memory_kv_engine;

namespace {

using item_sm = ddb::dynamo_item_state_machine<>;

constexpr std::uint64_t k_table = 7;

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
    BOOST_REQUIRE_MESSAGE(compiled.has_value(),
                          (compiled ? std::string() : compiled.error().message.value_or("")));
    return *compiled;
}

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

struct fixture {
    memory_kv_engine engine;
    item_sm sm;
    std::uint64_t index = 0;

    fixture() { sm.open(engine, ddb::table_range(k_table)); }

    auto run(const ddb::item_command& c) -> ddb::item_result {
        const auto out = sm.apply(bytes_of(ddb::encode_item_command(c)), ++index);
        auto r = ddb::decode_item_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    auto put(const char* it, const char* input = "{}",
             ddb::return_values rv = ddb::return_values::none, bool old_on_failure = false)
        -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::put;
        c.table_id = k_table;
        c.schema = schema();
        c.payload = json_item(it);
        c.expressions = compile("PutItem", input);
        c.returns = rv;
        c.old_on_condition_failure = old_on_failure;
        return run(c);
    }

    auto update(const char* key, const char* input,
                ddb::return_values rv = ddb::return_values::none) -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::update;
        c.table_id = k_table;
        c.schema = schema();
        c.payload = json_item(key);
        c.expressions = compile("UpdateItem", input);
        c.returns = rv;
        return run(c);
    }

    auto del(const char* key, const char* input = "{}",
             ddb::return_values rv = ddb::return_values::none) -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::delete_;
        c.table_id = k_table;
        c.schema = schema();
        c.payload = json_item(key);
        c.expressions = compile("DeleteItem", input);
        c.returns = rv;
        return run(c);
    }

    auto get(const char* key) const -> std::optional<ddb::item> {
        return sm.get_item(k_table, schema(), json_item(key));
    }
};

constexpr const char* k_key1 = R"({"pk":{"S":"a"},"sk":{"N":"1"}})";

}  // namespace

BOOST_AUTO_TEST_CASE(put_get_and_versions) {
    fixture f;
    BOOST_TEST(!f.get(k_key1).has_value());
    auto r = f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"one"}})");
    BOOST_TEST((r.outcome == ddb::item_result::status::ok));
    BOOST_TEST(r.version == 1U);
    BOOST_TEST(!r.attributes.has_value());
    BOOST_TEST(f.sm.applied_index() == 1U);

    r = f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"two"}})", "{}",
              ddb::return_values::all_old);
    BOOST_TEST(r.version == 2U);
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_TEST(ddb::find_attribute(*r.attributes, "v")->str == "one");
    BOOST_TEST(ddb::find_attribute(*f.get(k_key1), "v")->str == "two");
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key1)) == 2U);
}

BOOST_AUTO_TEST_CASE(a_failed_condition_changes_nothing_but_the_applied_index) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"one"}})");
    const auto r = f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"two"}})",
                         R"j({"ConditionExpression":"attribute_not_exists(pk)"})j",
                         ddb::return_values::all_old, true);
    BOOST_TEST((r.outcome == ddb::item_result::status::condition_failed));
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_TEST(ddb::find_attribute(*r.attributes, "v")->str == "one");
    BOOST_TEST(ddb::find_attribute(*f.get(k_key1), "v")->str == "one");
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key1)) == 1U);
    BOOST_TEST(f.sm.applied_index() == 2U);

    // Without ReturnValuesOnConditionCheckFailure the item is not returned.
    const auto quiet = f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"}})",
                             R"j({"ConditionExpression":"attribute_not_exists(pk)"})j");
    BOOST_TEST((quiet.outcome == ddb::item_result::status::condition_failed));
    BOOST_TEST(!quiet.attributes.has_value());
}

BOOST_AUTO_TEST_CASE(the_condition_reads_the_committed_item) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"n":{"N":"5"}})");
    const char* guarded =
        R"({"ConditionExpression":"n = :five","ExpressionAttributeValues":{":five":{"N":"5"}}})";
    BOOST_TEST((f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"n":{"N":"6"}})", guarded).outcome ==
                ddb::item_result::status::ok));
    // The same entry again sees n = 6 and fails.
    BOOST_TEST((f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"n":{"N":"7"}})", guarded).outcome ==
                ddb::item_result::status::condition_failed));
}

BOOST_AUTO_TEST_CASE(update_creates_from_the_key_and_returns_what_was_asked) {
    fixture f;
    auto r = f.update(k_key1,
                      R"({"UpdateExpression":"SET a = :x ADD c :one",)"
                      R"("ExpressionAttributeValues":{":x":{"S":"x"},":one":{"N":"1"}}})",
                      ddb::return_values::all_new);
    BOOST_TEST((r.outcome == ddb::item_result::status::ok));
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_TEST(r.attributes->size() == 4U);
    BOOST_TEST(r.version == 1U);

    r = f.update(
        k_key1,
        R"({"UpdateExpression":"ADD c :one","ExpressionAttributeValues":{":one":{"N":"1"}}})",
        ddb::return_values::updated_new);
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_REQUIRE(r.attributes->size() == 1U);
    BOOST_TEST(ddb::find_attribute(*r.attributes, "c")->num.to_string() == "2");

    r = f.update(k_key1, R"({"UpdateExpression":"REMOVE a"})", ddb::return_values::updated_old);
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_TEST(ddb::find_attribute(*r.attributes, "a")->str == "x");

    // UPDATED_NEW of a removed attribute has nothing to return.
    r = f.update(k_key1, R"({"UpdateExpression":"REMOVE c"})", ddb::return_values::updated_new);
    BOOST_TEST(!r.attributes.has_value());
    BOOST_TEST(f.get(k_key1)->size() == 2U);
}

BOOST_AUTO_TEST_CASE(an_update_that_cannot_apply_is_an_error_and_writes_nothing) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"s":{"S":"text"}})");
    const auto r = f.update(
        k_key1,
        R"({"UpdateExpression":"ADD s :one","ExpressionAttributeValues":{":one":{"N":"1"}}})");
    BOOST_TEST((r.outcome == ddb::item_result::status::error));
    BOOST_TEST(r.error_type.find("ValidationException") != std::string::npos);
    BOOST_TEST(!r.error_message.empty());
    BOOST_TEST(ddb::find_attribute(*f.get(k_key1), "s")->str == "text");
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key1)) == 1U);
}

BOOST_AUTO_TEST_CASE(delete_returns_the_old_item_and_clears_the_version) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"one"}})");
    auto r = f.del(k_key1, "{}", ddb::return_values::all_old);
    BOOST_REQUIRE(r.attributes.has_value());
    BOOST_TEST(ddb::find_attribute(*r.attributes, "v")->str == "one");
    BOOST_TEST(!f.get(k_key1).has_value());
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key1)) == 0U);
    BOOST_TEST(f.engine.key_count(column_family::data) == 0U);
    // Deleting what is not there succeeds and returns nothing.
    r = f.del(k_key1, "{}", ddb::return_values::all_old);
    BOOST_TEST((r.outcome == ddb::item_result::status::ok));
    BOOST_TEST(!r.attributes.has_value());
}

BOOST_AUTO_TEST_CASE(a_replayed_index_is_a_no_op) {
    fixture f;
    ddb::item_command c;
    c.op = ddb::item_command::kind::put;
    c.table_id = k_table;
    c.schema = schema();
    c.payload = json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"}})");
    const auto entry = bytes_of(ddb::encode_item_command(c));
    BOOST_TEST(!f.sm.apply(entry, 5).empty());
    BOOST_TEST(f.sm.apply(entry, 5).empty());
    BOOST_TEST(f.sm.apply(entry, 3).empty());
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key1)) == 1U);
}

BOOST_AUTO_TEST_CASE(restart_reads_the_applied_index_and_items_from_the_engine) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"}})");
    (void)f.put(R"({"pk":{"S":"b"},"sk":{"N":"2"}})");
    item_sm reopened;
    reopened.open(f.engine, ddb::table_range(k_table));
    BOOST_TEST(reopened.applied_index() == 2U);
    BOOST_TEST(reopened.get_item(k_table, schema(), json_item(k_key1)).has_value());
}

BOOST_AUTO_TEST_CASE(durable_applied_index_follows_the_engine) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"}})");
    f.engine.hold_durability();
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"2"}})");
    BOOST_TEST(f.sm.applied_index() == 2U);
    BOOST_TEST(f.sm.durable_applied_index() == 1U);
    f.engine.release_durability();
    f.engine.sync(f.engine.latest_sequence());
    BOOST_TEST(f.sm.durable_applied_index() == 2U);
}

BOOST_AUTO_TEST_CASE(checkpoint_and_ingest_move_items_and_versions) {
    fixture f;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"one"}})");
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"two"}})");
    const auto dir = std::filesystem::temp_directory_path() /
                     ("ddb-item-sm-" + std::to_string(std::random_device{}()));
    const auto manifest = f.sm.checkpoint(dir);
    memory_kv_engine other;
    item_sm follower;
    follower.open(other, ddb::table_range(k_table));
    follower.ingest(dir, manifest, f.sm.applied_index());
    std::filesystem::remove_all(dir);
    BOOST_TEST(follower.applied_index() == 2U);
    BOOST_TEST(
        ddb::find_attribute(*follower.get_item(k_table, schema(), json_item(k_key1)), "v")->str ==
        "two");
    BOOST_TEST(follower.item_version(k_table, schema(), json_item(k_key1)) == 2U);
}

BOOST_AUTO_TEST_CASE(splits_only_between_item_collections) {
    fixture f;
    for (const char* pk : {"a", "b", "c", "d", "e", "f"}) {
        for (int sk = 0; sk < 3; ++sk) {
            const auto text = std::string(R"({"pk":{"S":")") + pk + R"("},"sk":{"N":")" +
                              std::to_string(sk) + R"("}})";
            (void)f.put(text.c_str());
        }
    }
    // A base item key is inside a collection: vetoed.
    const auto item_key =
        *ddb::base_item_key(k_table, schema(), json_item(R"({"pk":{"S":"c"},"sk":{"N":"1"}})"));
    BOOST_TEST(!item_sm::can_split_at(item_key));
    BOOST_CHECK_THROW((void)f.sm.split_at({item_key}), std::invalid_argument);

    // A partition prefix is a boundary.
    ddb::attribute_value c;
    c.type = ddb::attribute_type::S;
    c.str = "c";
    const auto cut = ddb::partition_prefix(k_table, c);
    BOOST_TEST(item_sm::can_split_at(cut));
    BOOST_TEST(item_sm::can_split_at(ddb::table_prefix(k_table)));
    const auto children = f.sm.split_at({cut});
    BOOST_REQUIRE(children.size() == 1U);
    BOOST_TEST(children[0].start == cut);

    item_sm child;
    child.open(f.engine, children[0]);
    BOOST_TEST(child.applied_index() == f.sm.applied_index());
    // Collection c is wholly in the child: every one of its items moved
    // with it, and the parent no longer sees them.
    for (int sk = 0; sk < 3; ++sk) {
        const auto key =
            std::string(R"({"pk":{"S":"c"},"sk":{"N":")") + std::to_string(sk) + R"("}})";
        BOOST_TEST(child.get_item(k_table, schema(), json_item(key.c_str())).has_value());
        BOOST_TEST(!f.sm.get_item(k_table, schema(), json_item(key.c_str())).has_value());
    }

    // Merge back.
    f.sm.adopt(children[0]);
    BOOST_TEST((f.sm.range() == ddb::table_range(k_table)));
    std::size_t found = 0;
    for (const char* pk : {"a", "b", "c", "d", "e", "f"}) {
        const auto key = std::string(R"({"pk":{"S":")") + pk + R"("},"sk":{"N":"0"}})";
        found += f.sm.get_item(k_table, schema(), json_item(key.c_str())).has_value() ? 1U : 0U;
    }
    BOOST_TEST(found == 6U);
}

BOOST_AUTO_TEST_CASE(number_partition_keys_have_boundaries_too) {
    ddb::attribute_value n;
    n.type = ddb::attribute_type::N;
    BOOST_REQUIRE((ddb::dynamo_decimal::parse("12.5", n.num) == ddb::decimal_status::ok));
    const auto prefix = ddb::partition_prefix(3, n);
    BOOST_TEST(ddb::is_partition_boundary(prefix));
    BOOST_TEST(!ddb::is_partition_boundary(prefix + '\0'));
    // A wrong hash is not a boundary even if the rest decodes.
    auto forged = prefix;
    forged[11] = static_cast<char>(forged[11] ^ 1);
    BOOST_TEST(!ddb::is_partition_boundary(forged));
    BOOST_TEST(!ddb::is_partition_boundary("x/short"));
}

BOOST_AUTO_TEST_CASE(dropping_a_table_is_bounded_per_entry) {
    fixture f;
    for (int sk = 0; sk < 5; ++sk) {
        const auto text =
            std::string(R"({"pk":{"S":"a"},"sk":{"N":")") + std::to_string(sk) + R"("}})";
        (void)f.put(text.c_str());
    }
    // Another table's item in the same engine is not touched.
    memory_kv_engine& engine = f.engine;
    item_sm other;
    other.open(engine, ddb::table_range(k_table + 1));
    ddb::item_command put;
    put.table_id = k_table + 1;
    put.schema = schema();
    put.payload = json_item(k_key1);
    (void)other.apply(bytes_of(ddb::encode_item_command(put)), 1);

    ddb::item_command step;
    step.op = ddb::item_command::kind::drop_table_step;
    step.table_id = k_table;
    step.schema = schema();
    step.max_keys = 2;
    std::vector<std::uint64_t> deleted;
    for (;;) {
        const auto r = f.run(step);
        deleted.push_back(r.deleted);
        if (!r.more) {
            break;
        }
    }
    BOOST_TEST((deleted == std::vector<std::uint64_t>{2, 2, 1}));
    BOOST_TEST(f.engine.key_count(column_family::data) == 1U);
    BOOST_TEST(other.get_item(k_table + 1, schema(), json_item(k_key1)).has_value());
    // A further step finds nothing.
    const auto r = f.run(step);
    BOOST_TEST(r.deleted == 0U);
    BOOST_TEST(!r.more);
}

BOOST_AUTO_TEST_CASE(entries_round_trip_and_bad_ones_stop_the_group) {
    ddb::item_command c;
    c.op = ddb::item_command::kind::update;
    c.table_id = 0x0102030405060708ULL;
    c.schema = schema();
    c.payload = json_item(k_key1);
    c.expressions = compile("UpdateItem", R"({"UpdateExpression":"SET a = :x",)"
                                          R"j("ConditionExpression":"attribute_exists(pk)",)j"
                                          R"("ExpressionAttributeValues":{":x":{"S":"x"}}})");
    c.returns = ddb::return_values::updated_new;
    c.old_on_condition_failure = true;
    const auto bytes = ddb::encode_item_command(c);
    ddb::item_command back;
    BOOST_TEST((ddb::decode_item_command(bytes, back) == ddb::item_command_status::ok));
    BOOST_TEST((back == c));
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        ddb::item_command cut;
        BOOST_TEST(
            (ddb::decode_item_command(bytes.substr(0, n), cut) != ddb::item_command_status::ok));
    }
    BOOST_TEST(
        (ddb::decode_item_command(bytes + 'x', back) == ddb::item_command_status::malformed));

    ddb::item_result r;
    r.outcome = ddb::item_result::status::condition_failed;
    r.attributes = json_item(k_key1);
    r.error_type = "t";
    r.error_message = "m";
    r.version = 9;
    r.deleted = 3;
    r.more = true;
    const auto rb = ddb::encode_item_result(r);
    BOOST_TEST((ddb::decode_item_result(rb) == r));
    for (std::size_t n = 0; n < rb.size(); ++n) {
        BOOST_TEST(!ddb::decode_item_result(rb.substr(0, n)).has_value());
    }

    fixture f;
    auto newer = bytes;
    newer[0] = static_cast<char>(2);
    BOOST_CHECK_THROW((void)f.sm.apply(bytes_of(newer), 1), std::runtime_error);
    BOOST_CHECK_THROW((void)f.sm.apply(bytes_of("\x01"), 1), std::invalid_argument);
    BOOST_TEST(f.sm.applied_index() == 0U);
}

BOOST_AUTO_TEST_CASE(a_key_outside_the_group_is_a_routing_bug) {
    fixture f;
    ddb::item_command c;
    c.table_id = k_table + 1;
    c.schema = schema();
    c.payload = json_item(k_key1);
    BOOST_CHECK_THROW((void)f.sm.apply(bytes_of(ddb::encode_item_command(c)), 1), std::logic_error);
}
