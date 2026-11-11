// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// PartiQL (`.kiro/specs/dynamodb-compatible-api/` task 14): ExecuteStatement,
// BatchExecuteStatement and ExecuteTransaction over local_backend. Every
// answer below is DynamoDB Local 3.3's for the same request, recorded by
// sending each statement to both.

#define BOOST_TEST_MODULE dynamodb_partiql_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <set>
#include <string>

using namespace dynamodb_test;

namespace {

struct partiql_fixture : fixture {
    partiql_fixture() {
        (void)create("tbl");
        for (int i = 1; i <= 3; ++i) {
            put("a", i, R"(,"v":{"N":")" + std::to_string(i * 10) + R"("})");
        }
        put("b", 1,
            R"j(,"m":{"M":{"k":{"S":"deep"}}},"l":{"L":[{"N":"0"},{"S":"one"}]},"tags":{"SS":["x","y"]})j");
    }

    void put(const std::string& pk, int sk, const std::string& extra = "") {
        (void)ok("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" +
                                std::to_string(sk) + R"("})" + extra + "}}");
    }
    auto get(const std::string& pk, int sk) -> json::object {
        auto r =
            ok("GetItem", R"({"TableName":"tbl","Key":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" +
                              std::to_string(sk) + R"("}},"ConsistentRead":true})");
        if (auto* item = r.if_contains("Item")) {
            return item->as_object();
        }
        return {};
    }
    auto statement(const std::string& text, const std::string& extra = "") -> reply {
        return call("ExecuteStatement",
                    "{\"Statement\":" + json::serialize(json::value(text)) + extra + "}");
    }
    auto items(const std::string& text, const std::string& extra = "") -> json::array {
        auto r = statement(text, extra);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, text + ": " + r.type() + ": " + r.message());
        return r.body.at("Items").as_array();
    }
    static auto keys_of(const json::array& rows) -> std::set<std::string> {
        std::set<std::string> out;
        for (const auto& row : rows) {
            const auto& o = row.as_object();
            out.insert(std::string(o.at("pk").at("S").as_string()) + "/" +
                       std::string(o.at("sk").at("N").as_string()));
        }
        return out;
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(select_by_partition_key_runs_as_a_query, partiql_fixture) {
    const auto rows = items(R"(SELECT * FROM "tbl" WHERE pk = 'a')");
    BOOST_TEST(rows.size() == 3U);
    BOOST_TEST(rows[0].at("sk").at("N").as_string() == "1");
    const auto desc = items(R"(SELECT * FROM "tbl" WHERE pk = 'a' ORDER BY sk DESC)");
    BOOST_TEST(desc[0].at("sk").at("N").as_string() == "3");
}

BOOST_FIXTURE_TEST_CASE(select_applies_sort_key_and_filter_terms, partiql_fixture) {
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE pk = 'a' AND sk > 1)")) ==
               (std::set<std::string>{"a/2", "a/3"}));
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE pk = 'a' AND v = 20)")) ==
               (std::set<std::string>{"a/2"}));
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE pk = 'a' AND sk BETWEEN 2 AND 3)")) ==
               (std::set<std::string>{"a/2", "a/3"}));
}

BOOST_FIXTURE_TEST_CASE(select_with_in_and_scan_shapes, partiql_fixture) {
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE pk IN ['a', 'b'] AND sk = 1)")) ==
               (std::set<std::string>{"a/1", "b/1"}));
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl)")).size() == 4U);
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE v >= 20)")) ==
               (std::set<std::string>{"a/2", "a/3"}));
    BOOST_TEST(keys_of(items(R"(SELECT * FROM tbl WHERE attribute_exists(tags))")) ==
               (std::set<std::string>{"b/1"}));
}

BOOST_FIXTURE_TEST_CASE(projections_are_named_as_dynamodb_local_names_them, partiql_fixture) {
    const auto rows = items(R"(SELECT pk, m.k, l[1] FROM tbl WHERE pk = 'b' AND sk = 1)");
    BOOST_REQUIRE(rows.size() == 1U);
    const auto& row = rows[0].as_object();
    BOOST_TEST(row.at("pk").at("S").as_string() == "b");
    BOOST_TEST(row.at("k").at("S").as_string() == "deep");
    BOOST_TEST(row.at("l[1]").at("S").as_string() == "one");
    BOOST_TEST(!row.contains("sk"));
}

BOOST_FIXTURE_TEST_CASE(limit_pages_with_a_next_token, partiql_fixture) {
    auto r = statement(R"(SELECT * FROM tbl WHERE pk = 'a')", R"(,"Limit":2)");
    BOOST_REQUIRE(r.status == 200U);
    BOOST_TEST(r.body.at("Items").as_array().size() == 2U);
    BOOST_REQUIRE(r.body.contains("NextToken"));
    const std::string token(r.body.at("NextToken").as_string());
    auto rest = statement(R"(SELECT * FROM tbl WHERE pk = 'a')",
                          R"(,"Limit":2,"NextToken":")" + token + "\"");
    BOOST_REQUIRE(rest.status == 200U);
    BOOST_TEST(rest.body.at("Items").as_array().size() == 1U);
    BOOST_TEST(rest.body.at("Items").as_array()[0].at("sk").at("N").as_string() == "3");
}

BOOST_FIXTURE_TEST_CASE(parameters_bind_in_order, partiql_fixture) {
    const auto rows = items(R"(SELECT * FROM tbl WHERE pk = ? AND sk = ?)",
                            R"(,"Parameters":[{"S":"a"},{"N":"2"}])");
    BOOST_TEST(keys_of(rows) == (std::set<std::string>{"a/2"}));
    const auto r =
        statement(R"(SELECT * FROM tbl WHERE pk = ? AND sk = ?)", R"(,"Parameters":[{"S":"a"}])");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() == "Number of parameters in request and statement don't match.");
}

BOOST_FIXTURE_TEST_CASE(insert_writes_once_and_refuses_a_duplicate, partiql_fixture) {
    const auto rows = items(R"(INSERT INTO tbl VALUE {'pk': 'c', 'sk': 1, 'n': 5})");
    BOOST_TEST(rows.empty());
    BOOST_TEST(get("c", 1).at("n").at("N").as_string() == "5");
    const auto r = statement(R"(INSERT INTO tbl VALUE {'pk': 'c', 'sk': 1})");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "DuplicateItemException");
    BOOST_TEST(r.message() == "Duplicate primary key exists in table");
}

BOOST_FIXTURE_TEST_CASE(update_sets_removes_and_returns, partiql_fixture) {
    BOOST_TEST(
        items(R"(UPDATE tbl SET v = v + 1 SET w = 'new' WHERE pk = 'a' AND sk = 1)").empty());
    auto item = get("a", 1);
    BOOST_TEST(item.at("v").at("N").as_string() == "11");
    BOOST_TEST(item.at("w").at("S").as_string() == "new");
    (void)items(R"(UPDATE tbl REMOVE w WHERE pk = 'a' AND sk = 1)");
    BOOST_TEST(!get("a", 1).contains("w"));
    const auto ret =
        items(R"(UPDATE tbl SET v = 99 WHERE pk = 'a' AND sk = 1 RETURNING ALL NEW *)");
    BOOST_REQUIRE(ret.size() == 1U);
    BOOST_TEST(ret[0].at("v").at("N").as_string() == "99");
}

BOOST_FIXTURE_TEST_CASE(update_of_a_missing_item_fails_its_condition, partiql_fixture) {
    auto r = statement(R"(UPDATE tbl SET v = 1 WHERE pk = 'zz' AND sk = 1)");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ConditionalCheckFailedException");
    BOOST_TEST(r.message() == "The conditional request failed");
    r = statement(R"(UPDATE tbl SET v = 1 WHERE pk = 'a')");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() ==
               "Where clause does not contain a mandatory equality on all key attributes");
}

BOOST_FIXTURE_TEST_CASE(delete_removes_and_ignores_a_missing_item, partiql_fixture) {
    BOOST_TEST(items(R"(DELETE FROM tbl WHERE pk = 'a' AND sk = 1)").empty());
    BOOST_TEST(get("a", 1).empty());
    BOOST_TEST(items(R"(DELETE FROM tbl WHERE pk = 'zz' AND sk = 1)").empty());
    const auto r = statement(R"(DELETE FROM tbl WHERE pk = 'a' AND sk = 2 AND v = 1)");
    BOOST_TEST(r.type() == "ConditionalCheckFailedException");
    BOOST_TEST(!get("a", 2).empty());
}

BOOST_FIXTURE_TEST_CASE(errors_match_dynamodb_local, partiql_fixture) {
    auto r = statement("SELEKT nothing");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() == "Statement wasn't well formed, can't be processed: SELEKT nothing");
    r = statement(R"(SELECT * FROM "missing")");
    BOOST_TEST(r.type() == "ResourceNotFoundException");
    r = statement(R"(SELECT * FROM "tbl"."nope")");
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() == "The table does not have the specified index: nope");
}

BOOST_FIXTURE_TEST_CASE(batch_answers_per_statement, partiql_fixture) {
    auto r = call("BatchExecuteStatement",
                  R"j({"Statements":[
                      {"Statement":"SELECT * FROM tbl WHERE pk = 'a' AND sk = 1"},
                      {"Statement":"SELECT * FROM tbl WHERE pk = 'zz' AND sk = 1"},
                      {"Statement":"SELECT * FROM tbl WHERE pk = 'a'"}]})j");
    BOOST_REQUIRE(r.status == 200U);
    const auto& out = r.body.at("Responses").as_array();
    BOOST_REQUIRE(out.size() == 3U);
    BOOST_TEST(out[0].at("Item").at("v").at("N").as_string() == "10");
    BOOST_TEST(out[0].at("TableName").as_string() == "tbl");
    BOOST_TEST(json::serialize(out[1]) == R"({"TableName":"tbl"})");
    BOOST_TEST(out[2].at("Error").at("Message").as_string() ==
               "Only single item select is supported");

    r = call("BatchExecuteStatement",
             R"j({"Statements":[
                 {"Statement":"INSERT INTO tbl VALUE {'pk':'d','sk':1}"},
                 {"Statement":"INSERT INTO tbl VALUE {'pk':'a','sk':1}"}]})j");
    BOOST_REQUIRE(r.status == 200U);
    const auto& writes = r.body.at("Responses").as_array();
    BOOST_TEST(!writes[0].as_object().contains("Error"));
    BOOST_TEST(writes[1].at("Error").at("Code").as_string() == "DuplicateItem");
    BOOST_TEST(writes[1].at("TableName").as_string() == "tbl");
    BOOST_TEST(!get("d", 1).empty());

    r = call("BatchExecuteStatement",
             R"j({"Statements":[
                 {"Statement":"SELECT * FROM tbl WHERE pk = 'a' AND sk = 1"},
                 {"Statement":"DELETE FROM tbl WHERE pk = 'a' AND sk = 1"}]})j");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.message() ==
               "Read and write requests together in the same batch is not supported.");
}

BOOST_FIXTURE_TEST_CASE(transactions_are_all_or_nothing, partiql_fixture) {
    auto r = call("ExecuteTransaction",
                  R"j({"TransactStatements":[
                      {"Statement":"INSERT INTO tbl VALUE {'pk':'t','sk':1}"},
                      {"Statement":"UPDATE tbl SET v = 0 WHERE pk = 'a' AND sk = 1"}]})j");
    BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
    BOOST_TEST(json::serialize(r.body) == R"({"Responses":[]})");
    BOOST_TEST(!get("t", 1).empty());
    BOOST_TEST(get("a", 1).at("v").at("N").as_string() == "0");

    r = call("ExecuteTransaction",
             R"j({"TransactStatements":[
                 {"Statement":"INSERT INTO tbl VALUE {'pk':'u','sk':1}"},
                 {"Statement":"INSERT INTO tbl VALUE {'pk':'t','sk':1}"}]})j");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.type() == "TransactionCanceledException");
    const auto& reasons = r.body.at("CancellationReasons").as_array();
    BOOST_REQUIRE(reasons.size() == 2U);
    BOOST_TEST(reasons[0].at("Code").as_string() == "None");
    BOOST_TEST(reasons[1].at("Code").as_string() == "ValidationError");
    BOOST_TEST(reasons[1].at("Message").as_string() == "Duplicate primary key exists in table");
    BOOST_TEST(get("u", 1).empty());

    r = call("ExecuteTransaction",
             R"j({"TransactStatements":[
                 {"Statement":"SELECT * FROM tbl WHERE pk = 'a' AND sk = 2"},
                 {"Statement":"SELECT * FROM tbl WHERE pk = 'a' AND sk = 3"},
                 {"Statement":"SELECT * FROM tbl WHERE pk = 'zz' AND sk = 1"}]})j");
    BOOST_REQUIRE(r.status == 200U);
    const auto& reads = r.body.at("Responses").as_array();
    BOOST_REQUIRE(reads.size() == 3U);
    BOOST_TEST(reads[1].at("Item").at("v").at("N").as_string() == "30");
    BOOST_TEST(reads[2].as_object().empty());

    r = call("ExecuteTransaction",
             R"j({"TransactStatements":[
                 {"Statement":"SELECT * FROM tbl WHERE pk = 'a' AND sk = 2"},
                 {"Statement":"DELETE FROM tbl WHERE pk = 'a' AND sk = 3"}]})j");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.message() ==
               "ExecuteTransaction API does not support both read and write "
               "operations in the same request.");
}

BOOST_FIXTURE_TEST_CASE(exists_is_a_condition_check_in_a_transaction, partiql_fixture) {
    auto r = call("ExecuteTransaction",
                  R"j({"TransactStatements":[
                      {"Statement":"EXISTS(SELECT * FROM tbl WHERE pk = 'a' AND sk = 1 AND v = 10)"},
                      {"Statement":"UPDATE tbl SET w = 1 WHERE pk = 'b' AND sk = 1"}]})j");
    BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
    BOOST_TEST(get("b", 1).at("w").at("N").as_string() == "1");

    r = call("ExecuteTransaction",
             R"j({"TransactStatements":[
                 {"Statement":"EXISTS(SELECT * FROM tbl WHERE pk = 'a' AND sk = 1 AND v = 5)"},
                 {"Statement":"UPDATE tbl SET w = 2 WHERE pk = 'b' AND sk = 1"}]})j");
    BOOST_TEST(r.type() == "TransactionCanceledException");
    BOOST_TEST(r.message() ==
               "Transaction cancelled, please refer cancellation reasons for "
               "specific reasons [ConditionalCheckFailed, None]");
    BOOST_TEST(get("b", 1).at("w").at("N").as_string() == "1");

    r = call("ExecuteTransaction",
             R"j({"TransactStatements":[
                 {"Statement":"EXISTS(SELECT * FROM tbl WHERE pk = 'a' AND sk = 1)"},
                 {"Statement":"UPDATE tbl SET w = 2 WHERE pk = 'b' AND sk = 1"}]})j");
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message() ==
               "Validation failed in TransactStatements[0]: EXISTS() must contain "
               "a single item read with additional condition");
}
