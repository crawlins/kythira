// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Transactions below the API (`.kiro/specs/dynamodb-compatible-api/` task
// 10, Requirement 12): the item state machine's `validate` hook, writes of
// one entry staged into one batch, and a group under core's transaction
// wrapper, as a Raft host drives it. The handlers' answers are checked
// against DynamoDB Local by the operations corpus (the "transactions"
// scenario) and across groups by dynamodb_raft_backend_test.

#define BOOST_TEST_MODULE dynamodb_txn_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/group_state_machine.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/txn_envelope.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::txn_result_status)
BOOST_TEST_DONT_PRINT_LOG_VALUE(kythira::dynamodb::item_result::status)

using namespace dynamodb_test;

namespace {

constexpr std::uint64_t k_table = 7;

auto schema() -> ddb::key_schema {
    ddb::key_schema s;
    s.partition_name = "pk";
    s.partition_type = ddb::attribute_type::S;
    s.sort_name = "sk";
    s.sort_type = ddb::attribute_type::N;
    return s;
}

auto lsis() -> std::vector<ddb::local_index> {
    ddb::local_index l;
    l.name = "by_ls";
    l.number = 0;
    l.sort_name = "ls";
    return {l};
}

auto json_item(const std::string& text) -> ddb::item {
    auto parsed = ddb::item_from_json(json::parse(text).as_object());
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

auto command(ddb::item_command::kind op, const std::string& item_json,
             const std::string& condition = "") -> ddb::item_command {
    ddb::item_command c;
    c.op = op;
    c.table_id = k_table;
    c.schema = schema();
    c.payload = json_item(item_json);
    c.lsis = lsis();
    if (!condition.empty()) {
        ddb::request_keys keys;
        keys.table = schema();
        auto compiled =
            ddb::compile_request("DeleteItem", json::parse(condition).as_object(), keys);
        BOOST_REQUIRE(compiled.has_value());
        c.expressions = std::move(*compiled);
    }
    return c;
}

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto view(const std::vector<std::byte>& b) -> std::string_view {
    return {reinterpret_cast<const char*>(b.data()), b.size()};  // NOLINT
}

auto key_of(const ddb::item_command& c) -> std::string {
    return *ddb::base_item_key(c.table_id, c.schema, c.payload);
}

auto encoded(const ddb::item_command& c) -> std::vector<std::byte> {
    return bytes_of(ddb::encode_item_command(c));
}

using txn_group = ddb::dynamo_txn_group_state_machine<>;
using envelope = kythira::txn_envelope<std::string>;
using txn_write = kythira::txn_write<std::string>;

/// One item group under the transaction wrapper, fed envelopes as a host
/// would feed it log entries.
struct wrapped_group {
    kythira::memory_kv_engine engine;
    txn_group sm;
    std::uint64_t index = 0;

    wrapped_group() { sm.open(engine, ddb::prefix_range("t/")); }

    auto run(const envelope& e) -> kythira::txn_result<std::string> {
        return kythira::decode_txn_result<std::string>(
            sm.apply(kythira::encode_txn_envelope<std::string>(e), ++index));
    }

    auto plain(const ddb::item_command& c) -> kythira::txn_result<std::string> {
        return run(kythira::txn_plain{encoded(c)});
    }

    [[nodiscard]] auto items() const -> const ddb::dynamo_item_state_machine<>& {
        return *sm.inner().item_group();
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(validate_judges_against_the_committed_item) {
    wrapped_group g;
    BOOST_TEST(g.plain(command(ddb::item_command::kind::put,
                               R"j({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"1"}})j"))
                   ._status == kythira::txn_result_status::ok);
    const auto& items = g.sm.inner();

    // A passing write has nothing to say.
    auto ok = command(ddb::item_command::kind::put, R"j({"pk":{"S":"a"},"sk":{"N":"1"}})j",
                      R"j({"ConditionExpression":"attribute_exists(pk)"})j");
    auto v = items.validate(key_of(ok), encoded(ok));
    BOOST_TEST(v._ok);
    BOOST_TEST(v._detail.empty());

    // A failing one reports the result the write would have returned, with
    // the stored item when it asked for ALL_OLD.
    auto refused =
        command(ddb::item_command::kind::condition_check, R"j({"pk":{"S":"a"},"sk":{"N":"1"}})j",
                R"j({"ConditionExpression":"attribute_not_exists(pk)"})j");
    refused.old_on_condition_failure = true;
    v = items.validate(key_of(refused), encoded(refused));
    BOOST_TEST(!v._ok);
    const auto r = ddb::decode_item_result(view(v._detail));
    BOOST_TEST_REQUIRE(r.has_value());
    BOOST_TEST(r->outcome == ddb::item_result::status::condition_failed);
    BOOST_TEST_REQUIRE(r->attributes.has_value());
    BOOST_TEST(ddb::find_attribute(*r->attributes, "v")->num.to_string() == "1");

    // A read always passes and carries the item, or nothing.
    const auto read =
        command(ddb::item_command::kind::read_lock, R"j({"pk":{"S":"a"},"sk":{"N":"1"}})j");
    v = items.validate(key_of(read), encoded(read));
    BOOST_TEST(v._ok);
    BOOST_TEST(ddb::decode_item_result(view(v._detail))->attributes.has_value());
    const auto missing =
        command(ddb::item_command::kind::read_lock, R"j({"pk":{"S":"b"},"sk":{"N":"1"}})j");
    v = items.validate(key_of(missing), encoded(missing));
    BOOST_TEST(v._ok);
    BOOST_TEST(!ddb::decode_item_result(view(v._detail))->attributes.has_value());

    // A write filed under another item's key is a coordinator bug.
    BOOST_CHECK_THROW((void)items.validate(key_of(missing), encoded(ok)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(condition_checks_and_reads_write_nothing) {
    wrapped_group g;
    const auto put = command(ddb::item_command::kind::put,
                             R"j({"pk":{"S":"a"},"sk":{"N":"1"},"ls":{"S":"x"}})j");
    BOOST_TEST(g.plain(put)._status == kythira::txn_result_status::ok);
    const auto before = g.items().item_version(k_table, schema(), put.payload);
    const auto check =
        command(ddb::item_command::kind::condition_check, R"j({"pk":{"S":"a"},"sk":{"N":"1"}})j",
                R"j({"ConditionExpression":"attribute_exists(pk)"})j");
    const auto read =
        command(ddb::item_command::kind::read_lock, R"j({"pk":{"S":"a"},"sk":{"N":"2"}})j");
    const auto r = g.run(kythira::txn_one_phase<std::string>{
        kythira::txn_id{1, 1},
        {txn_write{key_of(check), encoded(check)}, txn_write{key_of(read), encoded(read)}},
        0,
        0});
    // The read's detail makes it `validated`.
    BOOST_TEST(r._status == kythira::txn_result_status::validated);
    BOOST_TEST(g.items().item_version(k_table, schema(), put.payload) == before);
    BOOST_TEST(!g.items().get_item(k_table, schema(), read.payload).has_value());
    BOOST_TEST(g.sm.intent_count() == 0U);
}

BOOST_AUTO_TEST_CASE(one_entrys_writes_to_one_collection_keep_its_size) {
    // Two puts to one item collection in one one-phase commit: the second
    // must read the size the first staged, or the collection's count is
    // short by the first item.
    wrapped_group g;
    const auto a = command(ddb::item_command::kind::put,
                           R"j({"pk":{"S":"p"},"sk":{"N":"1"},"ls":{"S":"x"}})j");
    const auto b = command(ddb::item_command::kind::put,
                           R"j({"pk":{"S":"p"},"sk":{"N":"2"},"ls":{"S":"y"}})j");
    const auto r = g.run(kythira::txn_one_phase<std::string>{
        kythira::txn_id{1, 2},
        {txn_write{key_of(a), encoded(a)}, txn_write{key_of(b), encoded(b)}},
        0,
        0});
    BOOST_TEST(r._status == kythira::txn_result_status::ok);

    wrapped_group h;
    BOOST_TEST(h.plain(a)._status == kythira::txn_result_status::ok);
    BOOST_TEST(h.plain(b)._status == kythira::txn_result_status::ok);
    const auto pk = ddb::attribute_value::string("p");
    BOOST_TEST(g.items().collection_size(k_table, pk) != 0U);
    BOOST_TEST(g.items().collection_size(k_table, pk) == h.items().collection_size(k_table, pk));
}

BOOST_AUTO_TEST_CASE(a_prepared_item_refuses_plain_writes_until_its_commit) {
    wrapped_group g;
    const auto put =
        command(ddb::item_command::kind::put, R"j({"pk":{"S":"a"},"sk":{"N":"1"},"v":{"N":"2"}})j");
    const kythira::txn_id id{3, 3};
    BOOST_TEST(
        g.run(kythira::txn_prepare<std::string>{
                  id, key_of(put), {txn_write{key_of(put), encoded(put)}}, {}, 0, 1000, true})
            ._status == kythira::txn_result_status::ok);
    const auto plain =
        g.plain(command(ddb::item_command::kind::delete_, R"j({"pk":{"S":"a"},"sk":{"N":"1"}})j"));
    BOOST_TEST(plain._status == kythira::txn_result_status::key_locked);
    // Reads see the committed state, which has no item yet.
    BOOST_TEST(!g.items().get_item(k_table, schema(), put.payload).has_value());
    BOOST_TEST(g.run(kythira::txn_commit_primary<std::string>{id, key_of(put), 0})._status ==
               kythira::txn_result_status::ok);
    BOOST_TEST(g.items().get_item(k_table, schema(), put.payload).has_value());
    // The lock and its record survive a restart from the engine.
    txn_group reopened;
    reopened.open(g.engine, ddb::prefix_range("t/"));
    BOOST_TEST(reopened.intent_count() == 0U);
    BOOST_TEST(reopened.record_of(key_of(put), id).has_value());
}

BOOST_AUTO_TEST_CASE(tokens_are_per_caller_and_capacity_counts_twice) {
    fixture f;
    f.ok("CreateTable", R"j({"TableName":"tok","BillingMode":"PAY_PER_REQUEST",)j"
                        R"j("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)j"
                        R"j("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})j");
    const std::string body =
        R"j({"ClientRequestToken":"t1","ReturnConsumedCapacity":"TOTAL",)j"
        R"j("TransactItems":[{"Put":{"TableName":"tok","Item":{"id":{"S":"a"}}}}]})j";
    const auto first = f.ok("TransactWriteItems", body);
    BOOST_TEST(json::serialize(first.at("ConsumedCapacity")) ==
               R"j([{"TableName":"tok","CapacityUnits":2E0}])j");

    // The same token from another caller is another token: its changed
    // request runs rather than being refused as a mismatch.
    ddb::call_context other;
    other.principal.arn = "arn:aws:iam::123456789012:user/other";
    auto changed =
        json::parse(R"j({"ClientRequestToken":"t1","TransactItems":[{"Put":{"TableName":"tok",)j"
                    R"j("Item":{"id":{"S":"b"}}}}]})j")
            .as_object();
    BOOST_TEST(f.transactions.transact_write(other, changed).has_value());
    // From the first caller, the changed request is a mismatch.
    const auto mismatch =
        f.call("TransactWriteItems", R"j({"ClientRequestToken":"t1","TransactItems":[{"Put":{)j"
                                     R"j("TableName":"tok","Item":{"id":{"S":"c"}}}}]})j");
    BOOST_TEST(mismatch.type() == "IdempotentParameterMismatchException");

    // After ten minutes the token is free again.
    f.clock_ms += 10 * 60 * 1000 + 1;
    BOOST_TEST(f.call("TransactWriteItems",
                      R"j({"ClientRequestToken":"t1","TransactItems":[{"Put":{)j"
                      R"j("TableName":"tok","Item":{"id":{"S":"c"}}}}]})j")
                   .status == 200U);

    const auto got =
        f.ok("TransactGetItems", R"j({"ReturnConsumedCapacity":"TOTAL","TransactItems":[)j"
                                 R"j({"Get":{"TableName":"tok","Key":{"id":{"S":"a"}}}},)j"
                                 R"j({"Get":{"TableName":"tok","Key":{"id":{"S":"c"}}}}]})j");
    BOOST_TEST(json::serialize(got.at("ConsumedCapacity")) ==
               R"j([{"TableName":"tok","CapacityUnits":4E0}])j");
}

BOOST_AUTO_TEST_CASE(a_transaction_holds_at_most_100_items_and_4_mb) {
    fixture f;
    f.ok("CreateTable", R"j({"TableName":"lim","BillingMode":"PAY_PER_REQUEST",)j"
                        R"j("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)j"
                        R"j("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})j");
    const auto request = [](int count, std::size_t pad) {
        std::string body = R"j({"TransactItems":[)j";
        for (int i = 0; i < count; ++i) {
            body += (i == 0 ? "" : ",");
            body += R"j({"Put":{"TableName":"lim","Item":{"id":{"S":"i)j" + std::to_string(i) +
                    R"j("},"p":{"S":")j" + std::string(pad, 'x') + R"j("}}}})j";
        }
        return body + "]}";
    };
    BOOST_TEST(f.call("TransactWriteItems", request(100, 1)).status == 200U);
    BOOST_TEST(f.call("TransactWriteItems", request(101, 1)).type() == "ValidationException");

    // Eleven items of 390 KB fit the item limit one by one, but not the
    // request limit together; nothing is written.
    const auto big = f.call("TransactWriteItems", request(11, 390 * 1024));
    BOOST_TEST(big.type() == "ValidationException");
    BOOST_TEST(big.message() == "Transaction request size exceeded the 4 MB limit");
    const auto got = f.ok("GetItem", R"j({"TableName":"lim","Key":{"id":{"S":"i0"}}})j");
    BOOST_TEST(json::serialize(got.at("Item").as_object().at("p")) == R"j({"S":"x"})j");
}
