// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Vector indexes and SearchVectors (`.kiro/specs/dynamodb-compatible-api/`
// task 26, Requirements 22.2-22.4) over local_backend: `VectorIndexes` on
// `CreateTable` and their checks, the descriptions and the stored form,
// `VectorIndexUpdates` and the worker that finishes them, the three
// distance functions, `HASH` partitions and inline filters, projections,
// consumed capacity, and graphs that follow later writes and never return
// an item that is gone.

#define BOOST_TEST_MODULE dynamodb_vector_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/vector_index.hpp>
#include <kythira/dynamodb/vector_search.hpp>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

/// A `VectorIndexes` element (or `Create` action) as JSON text.
auto index_json(const std::string& name, int dims, const std::string& distance = "EUCLIDEAN",
                const std::string& schema = "", const std::string& projection = R"("ALL")")
    -> std::string {
    std::string out = R"({"IndexName":")" + name +
                      R"(","VectorAttribute":{"AttributeName":"vec"},"Dimensions":)" +
                      std::to_string(dims) + R"(,"DistanceFunction":")" + distance +
                      R"(","Projection":{"ProjectionType":)" + projection + "}";
    if (!schema.empty()) {
        out += R"(,"SearchSchema":)" + schema;
    }
    return out + "}";
}

auto vec(const std::vector<double>& v) -> std::string {
    std::string out = R"({"L":[)";
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::ostringstream n;
        n << v[i];
        out += (i == 0 ? "" : ",") + std::string(R"({"N":")") + n.str() + R"("})";
    }
    return out + "]}";
}

auto search_vector(const std::vector<double>& v) -> std::string {
    const auto l = vec(v);
    return l.substr(5, l.size() - 6);  // the list's elements, bracketed
}

/// local_backend, whose change log goes dark while `blind` is set.
struct blind_backend {
    ddb::local_backend<>& inner;
    bool blind = false;

    auto catalog(const ddb::catalog_command& c) { return inner.catalog(c); }
    auto write(const ddb::item_command& c) { return inner.write(c); }
    auto read(std::uint64_t id, const ddb::key_schema& s, const ddb::item& k, bool consistent) {
        return inner.read(id, s, k, consistent);
    }
    void read_range(const kythira::key_range& r, kythira::scan_direction d, bool consistent,
                    const ddb::range_visitor& visit) {
        inner.read_range(r, d, consistent, visit);
    }
    auto describe(std::string_view name) { return inner.describe(name); }
    auto list_tables(std::string_view start, std::size_t limit) {
        return inner.list_tables(start, limit);
    }
    auto now_ms() { return inner.now_ms(); }
    auto transact(const std::vector<ddb::item_command>& a) { return inner.transact(a); }
    void settle_item(const ddb::table_descriptor& t, const ddb::item& k) {
        inner.settle_item(t, k);
    }
    auto throttler() -> ddb::capacity_throttler& { return inner.throttler(); }
    auto change_shards(const kythira::key_range& r) -> std::vector<ddb::change_shard> {
        return blind ? std::vector<ddb::change_shard>{} : inner.change_shards(r);
    }
    auto read_changes(const std::string& key, std::uint64_t seq, std::size_t n) {
        return inner.read_changes(key, seq, n);
    }
};
static_assert(ddb::dynamo_backend<blind_backend>);

/// A call context for calling a handler directly.
struct call_context_holder {
    ddb::call_context ctx;
};

struct vector_fixture : fixture {
    ddb::vector_search_api<ddb::local_backend<>> vectors{backend};
    ddb::vector_index_worker<ddb::local_backend<>> worker{backend, &vectors.registry()};

    vector_fixture() {
        vectors.register_with(*fe);
        without_throttling();
    }

    void put(const std::string& table, const std::string& pk, int sk, const std::string& vector,
             const std::string& extra = "") {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":")" + std::to_string(sk) + R"("},"vec":)" + vector +
                                extra + "}}");
    }

    auto search(const std::string& table, const std::string& index,
                const std::vector<double>& query, int top_k, const std::string& extra = "")
        -> reply {
        return call("SearchVectors", R"({"TableName":")" + table + R"(","IndexName":")" + index +
                                         R"(","SearchVector":)" + search_vector(query) +
                                         R"(,"TopK":)" + std::to_string(top_k) + extra + "}");
    }

    /// The `sk` of each result, in order.
    static auto order(const reply& r) -> std::vector<int> {
        std::vector<int> out;
        for (const auto& e : r.body.at("SearchResults").as_array()) {
            out.push_back(std::stoi(std::string(e.at("Item").at("sk").at("N").as_string())));
        }
        return out;
    }

    static auto score(const reply& r, std::size_t i) -> double {
        return r.body.at("SearchResults").as_array().at(i).at("Score").to_number<double>();
    }

    auto describe_vectors(const std::string& table) -> json::array {
        auto d = ok("DescribeTable", R"({"TableName":")" + table + R"("})");
        const auto* v = d.at("Table").as_object().if_contains("VectorIndexes");
        return v == nullptr ? json::array{} : v->as_array();
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(create_table_describes_vector_indexes, vector_fixture) {
    const auto created = create(
        "vtab",
        R"(,"VectorIndexes":[)" +
            index_json("byvec", 3, "COSINE",
                       R"([{"AttributeName":"tenant","SearchSchemaElementType":"HASH"},)"
                       R"({"AttributeName":"color","SearchSchemaElementType":"INLINE_FILTER"}])",
                       R"("INCLUDE","NonKeyAttributes":["title"])") +
            "]");
    const auto& list = created.at("TableDescription").at("VectorIndexes").as_array();
    BOOST_REQUIRE_EQUAL(list.size(), 1U);
    const auto& v = list[0].as_object();
    BOOST_TEST(v.at("IndexName").as_string() == "byvec");
    BOOST_TEST(v.at("VectorAttribute").at("AttributeName").as_string() == "vec");
    BOOST_TEST(v.at("Dimensions").to_number<int>() == 3);
    BOOST_TEST(v.at("DistanceFunction").as_string() == "COSINE");
    BOOST_TEST(v.at("IndexStatus").as_string() == "ACTIVE");
    BOOST_TEST(v.at("Projection").at("ProjectionType").as_string() == "INCLUDE");
    BOOST_TEST(v.at("Projection").at("NonKeyAttributes").as_array().size() == 1U);
    BOOST_TEST(v.at("SearchSchema").as_array().size() == 2U);
    BOOST_TEST(v.at("SearchSchema").as_array()[0].at("SearchSchemaElementType").as_string() ==
               "HASH");
    BOOST_TEST(std::string(v.at("IndexArn").as_string()).ends_with(":table/vtab/index/byvec"));

    // The descriptor's stored form keeps every field, at version 9.
    const auto d = backend.describe("vtab");
    BOOST_REQUIRE(d.has_value());
    const auto bytes = ddb::encode_table_descriptor(*d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 9);
    const auto back = ddb::decode_table_descriptor(bytes);
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST((back->vectors == d->vectors));
    BOOST_TEST(d->logs_changes());
}

BOOST_FIXTURE_TEST_CASE(create_table_checks_vector_indexes, vector_fixture) {
    const auto refused = [&](const std::string& indexes) {
        const auto r = call(
            "CreateTable",
            R"({"TableName":"bad","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
            R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST",)"
            R"("VectorIndexes":[)" +
                indexes + "]}");
        BOOST_TEST(r.status == 400U);
        BOOST_TEST(r.type() == "ValidationException");
        return r.message();
    };
    // The model's own bounds catch 0.
    BOOST_TEST(refused(index_json("idxa", 0)).find("dimensions") != std::string::npos);
    BOOST_TEST(refused(index_json("idxa", 4097)).find("Dimensions") != std::string::npos);
    BOOST_TEST(
        refused(
            R"({"IndexName":"idxa","VectorAttribute":{"AttributeName":"pk"},)"
            R"("Dimensions":2,"DistanceFunction":"COSINE","Projection":{"ProjectionType":"ALL"}})")
            .find("key attribute") != std::string::npos);
    BOOST_TEST(refused(index_json("idxa", 2, "COSINE",
                                  R"([{"AttributeName":"x","SearchSchemaElementType":"HASH"},)"
                                  R"({"AttributeName":"y","SearchSchemaElementType":"HASH"}])"))
                   .find("at most one HASH") != std::string::npos);
    BOOST_TEST(
        refused(index_json("idxa", 2, "COSINE", "", R"("INCLUDE")")).find("NonKeyAttributes") !=
        std::string::npos);
    BOOST_TEST(refused(index_json("idxa", 2) + "," + index_json("idxa", 3)).find("Duplicate") !=
               std::string::npos);
    BOOST_TEST(!backend.describe("bad").has_value());
}

BOOST_FIXTURE_TEST_CASE(euclidean_search_ranks_and_scores, vector_fixture) {
    create("vtab", R"(,"VectorIndexes":[)" + index_json("byvec", 2) + "]");
    put("vtab", "a", 1, vec({0, 0}));
    put("vtab", "a", 2, vec({3, 4}));
    put("vtab", "a", 3, vec({1, 1}));
    put("vtab", "a", 4, vec({1, 2, 3}));             // wrong length: not indexed
    put("vtab", "a", 5, R"({"S":"not a vector"})");  // not a list: not indexed
    const auto r = search("vtab", "byvec", {0, 0}, 10);
    BOOST_REQUIRE_EQUAL(r.status, 200U);
    BOOST_TEST((order(r) == std::vector<int>{1, 3, 2}));
    BOOST_TEST(score(r, 0) == 0.0);
    BOOST_TEST(std::abs(score(r, 1) - std::sqrt(2.0)) < 1e-6);
    BOOST_TEST(std::abs(score(r, 2) - 5.0) < 1e-6);
    // TopK cuts the list.
    BOOST_TEST((order(search("vtab", "byvec", {3, 4}, 1)) == std::vector<int>{2}));
    // A projection of ALL shows the whole item.
    BOOST_TEST(r.body.at("SearchResults").as_array()[0].at("Item").as_object().contains("vec"));
    // The search vector must match the index.
    const auto bad = search("vtab", "byvec", {1, 2, 3}, 1);
    BOOST_TEST(bad.status == 400U);
    BOOST_TEST(bad.message().find("dimensions") != std::string::npos);
    const auto missing = search("vtab", "nope", {1, 2}, 1);
    BOOST_TEST(missing.type() == "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(cosine_and_dot_product, vector_fixture) {
    create("vtab", R"(,"VectorIndexes":[)" + index_json("cos", 2, "COSINE") + "," +
                       index_json("dot", 2, "DOT_PRODUCT") + "]");
    put("vtab", "a", 1, vec({1, 0}));
    put("vtab", "a", 2, vec({0, 5}));
    put("vtab", "a", 3, vec({2, 2}));
    put("vtab", "a", 4, vec({0, 0}));
    const auto cos = search("vtab", "cos", {1, 0}, 4);
    BOOST_TEST((order(cos) == std::vector<int>{1, 3, 2, 4}));
    BOOST_TEST(std::abs(score(cos, 0)) < 1e-6);
    BOOST_TEST(std::abs(score(cos, 1) - (1.0 - 1.0 / std::sqrt(2.0))) < 1e-6);
    BOOST_TEST(std::abs(score(cos, 2) - 1.0) < 1e-6);
    BOOST_TEST(std::abs(score(cos, 3) - 1.0) < 1e-6);  // a zero vector has no direction
    const auto dot = search("vtab", "dot", {1, 1}, 4);
    BOOST_TEST((order(dot) == std::vector<int>{2, 3, 1, 4}));
    BOOST_TEST(std::abs(score(dot, 0) - 5.0) < 1e-6);
}

BOOST_FIXTURE_TEST_CASE(hash_partitions_and_inline_filters, vector_fixture) {
    create(
        "vtab",
        R"(,"VectorIndexes":[)" +
            index_json("byvec", 2, "EUCLIDEAN",
                       R"([{"AttributeName":"tenant","SearchSchemaElementType":"HASH"},)"
                       R"({"AttributeName":"color","SearchSchemaElementType":"INLINE_FILTER"}])") +
            "]");
    put("vtab", "a", 1, vec({0, 0}), R"(,"tenant":{"S":"t1"},"color":{"S":"red"})");
    put("vtab", "a", 2, vec({0, 1}), R"(,"tenant":{"S":"t1"},"color":{"S":"blue"})");
    put("vtab", "a", 3, vec({0, 0}), R"(,"tenant":{"S":"t2"},"color":{"S":"red"})");
    put("vtab", "a", 4, vec({0, 0}));  // no HASH value: not indexed
    const std::string t1 =
        R"(,"SearchConditionExpression":"#t = :t","ExpressionAttributeNames":{"#t":"tenant"},)"
        R"("ExpressionAttributeValues":{":t":{"S":"t1"}})";
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10, t1)) == std::vector<int>{1, 2}));
    const std::string t1_blue =
        R"(,"SearchConditionExpression":"(tenant = :t) AND color = :c",)"
        R"("ExpressionAttributeValues":{":t":{"S":"t1"},":c":{"S":"blue"}})";
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10, t1_blue)) == std::vector<int>{2}));
    const std::string t2 = R"(,"SearchConditionExpression":"tenant = :t",)"
                           R"("ExpressionAttributeValues":{":t":{"S":"t2"}})";
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10, t2)) == std::vector<int>{3}));

    // The HASH value is required; only search schema attributes and `=`.
    const auto no_hash = search("vtab", "byvec", {0, 0}, 10);
    BOOST_TEST(no_hash.status == 400U);
    BOOST_TEST(no_hash.message().find("HASH") != std::string::npos);
    const auto other = search("vtab", "byvec", {0, 0}, 10,
                              R"(,"SearchConditionExpression":"tenant = :t AND pk = :p",)"
                              R"("ExpressionAttributeValues":{":t":{"S":"t1"},":p":{"S":"a"}})");
    BOOST_TEST(other.status == 400U);
    BOOST_TEST(other.message().find("search schema") != std::string::npos);
    const auto or_ = search("vtab", "byvec", {0, 0}, 10,
                            R"(,"SearchConditionExpression":"tenant = :t OR color = :c",)"
                            R"("ExpressionAttributeValues":{":t":{"S":"t1"},":c":{"S":"red"}})");
    BOOST_TEST(or_.status == 400U);

    // Moving an item to another tenant moves it between partitions.
    put("vtab", "a", 2, vec({0, 1}), R"(,"tenant":{"S":"t2"},"color":{"S":"blue"})");
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10, t1)) == std::vector<int>{1}));
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10, t2)) == std::vector<int>{3, 2}));
}

BOOST_FIXTURE_TEST_CASE(projections_and_capacity, vector_fixture) {
    create(
        "vtab",
        R"(,"VectorIndexes":[)" +
            index_json("keys", 2, "EUCLIDEAN",
                       R"([{"AttributeName":"color","SearchSchemaElementType":"INLINE_FILTER"}])",
                       R"("KEYS_ONLY")") +
            "," +
            index_json("inc", 2, "EUCLIDEAN", "", R"("INCLUDE","NonKeyAttributes":["title"])") +
            "]");
    put("vtab", "a", 1, vec({0, 0}),
        R"(,"color":{"S":"red"},"title":{"S":"one"},"body":{"S":"x"})");
    const auto keys = search("vtab", "keys", {0, 0}, 1);
    const auto& k = keys.body.at("SearchResults").as_array()[0].at("Item").as_object();
    BOOST_TEST(k.size() == 4U);  // pk, sk, vec, color
    BOOST_TEST(k.contains("color"));
    BOOST_TEST(!k.contains("title"));
    const auto inc = search("vtab", "inc", {0, 0}, 1);
    const auto& i = inc.body.at("SearchResults").as_array()[0].at("Item").as_object();
    BOOST_TEST(i.contains("title"));
    BOOST_TEST(!i.contains("body"));
    // A ProjectionExpression narrows what the index projects.
    const auto narrowed =
        search("vtab", "inc", {0, 0}, 1,
               R"(,"ProjectionExpression":"#t, body","ExpressionAttributeNames":{"#t":"title"})");
    const auto& n = narrowed.body.at("SearchResults").as_array()[0].at("Item").as_object();
    BOOST_TEST(n.size() == 1U);
    BOOST_TEST(n.contains("title"));
    // Capacity counts the search vector's bytes.
    const auto cap = search("vtab", "inc", {0, 0}, 1, R"(,"ReturnConsumedCapacity":"TOTAL")");
    BOOST_TEST(cap.body.at("ConsumedCapacity").at("VectorSearchRequestBytes").to_number<double>() ==
               8.0);
    BOOST_TEST(!keys.body.contains("ConsumedCapacity"));
}

BOOST_FIXTURE_TEST_CASE(graphs_follow_writes_and_deletes, vector_fixture) {
    create("vtab", R"(,"VectorIndexes":[)" + index_json("byvec", 2) + "]");
    put("vtab", "a", 1, vec({0, 0}));
    put("vtab", "a", 2, vec({5, 5}));
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10)) == std::vector<int>{1, 2}));
    const auto d = backend.describe("vtab");
    BOOST_TEST(vectors.registry().builds(*d, d->vectors[0]) == 1U);
    // Later writes reach the graphs from the change log, without a rebuild.
    put("vtab", "a", 3, vec({1, 0}));
    put("vtab", "a", 2, vec({0.5, 0}));
    (void)ok("DeleteItem", R"({"TableName":"vtab","Key":{"pk":{"S":"a"},"sk":{"N":"1"}}})");
    (void)ok(
        "UpdateItem",
        R"({"TableName":"vtab","Key":{"pk":{"S":"a"},"sk":{"N":"3"}},"UpdateExpression":"REMOVE vec"})");
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 10)) == std::vector<int>{2}));
    BOOST_TEST(vectors.registry().builds(*d, d->vectors[0]) == 1U);
    BOOST_TEST(vectors.registry().item_count(*d, d->vectors[0]) == 1U);
}

BOOST_FIXTURE_TEST_CASE(a_search_never_returns_a_missing_item, vector_fixture) {
    create("vtab", R"(,"VectorIndexes":[)" + index_json("byvec", 2) + "]");
    put("vtab", "a", 1, vec({0, 0}));
    put("vtab", "a", 2, vec({1, 1}));
    put("vtab", "a", 3, vec({2, 2}));
    // A node whose graphs stop seeing the change log (as one lagging far
    // behind would) still answers only items in the table, re-scored.
    blind_backend blind{backend};
    ddb::vector_search_api<blind_backend> lagging{blind};
    call_context_holder holder;
    auto first = json::parse(R"({"TableName":"vtab","IndexName":"byvec","SearchVector":)" +
                             search_vector({0, 0}) + R"(,"TopK":1})")
                     .as_object();
    BOOST_REQUIRE(lagging.search(holder.ctx, first).has_value());
    blind.blind = true;
    (void)ok("DeleteItem", R"({"TableName":"vtab","Key":{"pk":{"S":"a"},"sk":{"N":"1"}}})");
    (void)ok("DeleteItem", R"({"TableName":"vtab","Key":{"pk":{"S":"a"},"sk":{"N":"2"}}})");
    put("vtab", "a", 3, vec({3, 4}));
    auto again = first;
    const auto r = lagging.search(holder.ctx, again);
    BOOST_REQUIRE(r.has_value());
    const auto& results = r->at("SearchResults").as_array();
    BOOST_REQUIRE_EQUAL(results.size(), 1U);
    BOOST_TEST(results[0].at("Item").at("sk").at("N").as_string() == "3");
    BOOST_TEST(std::abs(results[0].at("Score").to_number<double>() - 5.0) < 1e-6);
    const auto d = backend.describe("vtab");
    BOOST_TEST(lagging.registry().item_count(*d, d->vectors[0]) == 3U);
}

BOOST_FIXTURE_TEST_CASE(update_table_adds_and_drops_vector_indexes, vector_fixture) {
    create("vtab");
    put("vtab", "a", 1, vec({0, 0}));
    put("vtab", "a", 2, vec({3, 0}));
    const auto added = ok("UpdateTable", R"({"TableName":"vtab","VectorIndexUpdates":[{"Create":)" +
                                             index_json("byvec", 2) + "}]}");
    const auto& v = added.at("TableDescription").at("VectorIndexes").as_array()[0];
    BOOST_TEST(v.at("IndexStatus").as_string() == "CREATING");
    BOOST_TEST(v.at("Backfilling").as_bool());
    const auto early = search("vtab", "byvec", {0, 0}, 1);
    BOOST_TEST(early.status == 400U);
    BOOST_TEST(early.message().find("CREATING") != std::string::npos);
    // Another change waits for this one.
    const auto busy =
        call("UpdateTable", R"({"TableName":"vtab","VectorIndexUpdates":[{"Create":)" +
                                index_json("other", 2) + "}]}");
    BOOST_TEST(busy.type() == "LimitExceededException");

    // The worker finishes the backfill from the items already there.
    BOOST_TEST(worker.pass() == 1U);
    BOOST_TEST(describe_vectors("vtab")[0].at("IndexStatus").as_string() == "ACTIVE");
    BOOST_TEST((order(search("vtab", "byvec", {0, 0}, 5)) == std::vector<int>{1, 2}));
    BOOST_TEST(worker.pass() == 0U);

    // Two changes at once, a duplicate name, a missing index.
    const auto two = call("UpdateTable", R"({"TableName":"vtab","VectorIndexUpdates":[{"Create":)" +
                                             index_json("idxx", 2) + R"(},{"Create":)" +
                                             index_json("idxy", 2) + "}]}");
    BOOST_TEST(two.type() == "LimitExceededException");
    const auto dup = call("UpdateTable", R"({"TableName":"vtab","VectorIndexUpdates":[{"Create":)" +
                                             index_json("byvec", 2) + "}]}");
    BOOST_TEST(dup.type() == "ValidationException");
    const auto gone =
        call("UpdateTable",
             R"({"TableName":"vtab","VectorIndexUpdates":[{"Delete":{"IndexName":"idxx"}}]})");
    BOOST_TEST(gone.type() == "ResourceNotFoundException");

    // A delete leaves the index DELETING until the worker drops it.
    const auto dropped =
        ok("UpdateTable",
           R"({"TableName":"vtab","VectorIndexUpdates":[{"Delete":{"IndexName":"byvec"}}]})");
    BOOST_TEST(dropped.at("TableDescription")
                   .at("VectorIndexes")
                   .as_array()[0]
                   .at("IndexStatus")
                   .as_string() == "DELETING");
    BOOST_TEST(search("vtab", "byvec", {0, 0}, 1).status == 400U);
    BOOST_TEST(worker.pass() == 1U);
    BOOST_TEST(describe_vectors("vtab").empty());
    BOOST_TEST(search("vtab", "byvec", {0, 0}, 1).type() == "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(worker_waits_for_the_backfill_delay, vector_fixture) {
    ddb::vector_index_worker<ddb::local_backend<>> slow{backend, &vectors.registry(), {60'000}};
    create("vtab");
    (void)ok("UpdateTable", R"({"TableName":"vtab","VectorIndexUpdates":[{"Create":)" +
                                index_json("byvec", 2) + "}]}");
    BOOST_TEST(slow.pass() == 0U);
    clock_ms += 60'000;
    BOOST_TEST(slow.pass() == 1U);
    BOOST_TEST(describe_vectors("vtab")[0].at("IndexStatus").as_string() == "ACTIVE");
}

BOOST_AUTO_TEST_CASE(search_conditions_parse) {
    ddb::vector_index v;
    v.name = "i";
    v.schema = {{"t", true}, {"c", false}};
    const json::object values{{":t", json::object{{"S", "x"}}}, {":c", json::object{{"N", "1"}}}};
    const auto ok = ddb::parse_search_condition(":t = t and c = :c", v, nullptr, &values);
    BOOST_REQUIRE(ok.has_value());
    BOOST_TEST(ok->equals.size() == 2U);
    BOOST_TEST(!ddb::parse_search_condition("c = :c", v, nullptr, &values).has_value());
    BOOST_TEST(!ddb::parse_search_condition("t = :t AND", v, nullptr, &values).has_value());
    BOOST_TEST(!ddb::parse_search_condition("t = :t AND t = :t", v, nullptr, &values).has_value());
    BOOST_TEST(!ddb::parse_search_condition("t = :missing", v, nullptr, &values).has_value());
    BOOST_TEST(!ddb::parse_search_condition("#n = :t", v, nullptr, &values).has_value());
}

#if defined(KYTHIRA_DYNAMODB_HAS_HNSWLIB)

#include <kythira/dynamodb/hnsw_graph.hpp>

#include <random>
#include <set>

namespace {

/// The same random entries in an exact graph and an HNSW graph that is
/// approximate from its first entry.
struct graph_pair {
    ddb::vector_index index;
    ddb::exact_vector_graph exact;
    ddb::hnsw_vector_graph hnsw;
    std::vector<std::vector<float>> vectors;

    graph_pair(ddb::vector_distance d, std::size_t dims, std::size_t count)
        : index(make_index(d, dims)), hnsw(index, options()) {
        std::mt19937 rng(7);
        std::normal_distribution<float> normal;
        for (std::size_t i = 0; i < count; ++i) {
            ddb::vector_entry e;
            e.vector.resize(dims);
            for (auto& x : e.vector) {
                x = normal(rng);
            }
            e.filters = {{"parity", ddb::attribute_value::string(i % 2 == 0 ? "even" : "odd")}};
            vectors.push_back(e.vector);
            exact.upsert(key(i), e);
            hnsw.upsert(key(i), e);
        }
    }

    static auto make_index(ddb::vector_distance d, std::size_t dims) -> ddb::vector_index {
        ddb::vector_index v;
        v.name = "ann";
        v.dimensions = static_cast<std::uint32_t>(dims);
        v.distance = d;
        return v;
    }
    static auto options() -> ddb::hnsw_vector_graph::options {
        ddb::hnsw_vector_graph::options o;
        o.exact_below = 0;
        o.initial_capacity = 16;  // grows as entries arrive
        return o;
    }
    static auto key(std::size_t i) -> std::string { return "k" + std::to_string(i); }
};

auto ids(const std::vector<std::pair<double, const ddb::vector_entry*>>& found)
    -> std::set<const ddb::vector_entry*> {
    std::set<const ddb::vector_entry*> out;
    for (const auto& f : found) {
        out.insert(f.second);
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(hnsw_recall_matches_exact_search) {
    const auto all = [](const ddb::vector_entry&) { return true; };
    for (const auto d : {ddb::vector_distance::euclidean, ddb::vector_distance::cosine,
                         ddb::vector_distance::dot_product}) {
        graph_pair g(d, 16, 2000);
        BOOST_TEST(g.hnsw.approximate());
        std::mt19937 rng(11);
        std::normal_distribution<float> normal;
        std::size_t hits = 0;
        std::size_t total = 0;
        for (int q = 0; q < 50; ++q) {
            std::vector<float> query(16);
            for (auto& x : query) {
                x = normal(rng);
            }
            const auto want = g.exact.nearest(query, d, 10, all);
            const auto got = g.hnsw.nearest(query, d, 10, all);
            BOOST_REQUIRE_EQUAL(got.size(), 10U);
            // Scores are exact, and best first.
            for (std::size_t i = 0; i + 1 < got.size(); ++i) {
                BOOST_TEST(!ddb::better_score(d, got[i + 1].first, got[i].first));
            }
            BOOST_TEST(std::abs(got[0].first - ddb::vector_score(d, query, got[0].second->vector)) <
                       1e-9);
            // The same best score when the best neighbour was found.
            std::set<std::vector<float>> exact_vectors;
            for (const auto& w : want) {
                exact_vectors.insert(w.second->vector);
            }
            for (const auto& h : got) {
                hits += exact_vectors.contains(h.second->vector) ? 1 : 0;
            }
            total += want.size();
        }
        const double recall = static_cast<double>(hits) / static_cast<double>(total);
        BOOST_TEST_MESSAGE("recall@10 " << ddb::distance_name(d) << ": " << recall);
        BOOST_TEST(recall >= 0.95);
    }
}

BOOST_AUTO_TEST_CASE(hnsw_deletes_replacements_and_filters) {
    graph_pair g(ddb::vector_distance::euclidean, 8, 500);
    const std::vector<float> origin(8, 0.0F);
    const auto all = [](const ddb::vector_entry&) { return true; };
    // Delete the 50 nearest: none comes back, and the graph still answers.
    const auto first = g.exact.nearest(origin, ddb::vector_distance::euclidean, 50, all);
    std::set<std::vector<float>> deleted;
    for (const auto& f : first) {
        deleted.insert(f.second->vector);
    }
    for (std::size_t i = 0; i < g.vectors.size(); ++i) {
        if (deleted.contains(g.vectors[i])) {
            g.hnsw.remove(graph_pair::key(i));
        }
    }
    BOOST_TEST(g.hnsw.size() == 450U);
    const auto after = g.hnsw.nearest(origin, ddb::vector_distance::euclidean, 20, all);
    BOOST_TEST(after.size() == 20U);
    for (const auto& a : after) {
        BOOST_TEST(!deleted.contains(a.second->vector));
    }
    // Re-adding reuses deleted slots, past the starting capacity.
    for (std::size_t i = 0; i < 100; ++i) {
        ddb::vector_entry e;
        e.vector.assign(8, static_cast<float>(i + 1) * 0.001F);
        g.hnsw.upsert("new" + std::to_string(i), e);
    }
    BOOST_TEST(g.hnsw.size() == 550U);
    const auto near = g.hnsw.nearest(origin, ddb::vector_distance::euclidean, 1, all);
    BOOST_REQUIRE_EQUAL(near.size(), 1U);
    BOOST_TEST(near[0].second->vector[0] == 0.001F);
    // A filter keeps only matching entries.
    const auto odd =
        g.hnsw.nearest(origin, ddb::vector_distance::euclidean, 10, [](const ddb::vector_entry& e) {
            const auto* p = ddb::find_attribute(e.filters, "parity");
            return p != nullptr && p->str == "odd";
        });
    BOOST_TEST(odd.size() == 10U);
    for (const auto& o : odd) {
        BOOST_TEST(ddb::find_attribute(o.second->filters, "parity")->str == "odd");
    }
}

BOOST_AUTO_TEST_CASE(hnsw_cosine_zero_vector_and_small_partitions) {
    ddb::vector_index v = graph_pair::make_index(ddb::vector_distance::cosine, 2);
    ddb::hnsw_vector_graph small(v);  // exact below 1000 entries
    ddb::vector_entry zero;
    zero.vector = {0.0F, 0.0F};
    ddb::vector_entry east;
    east.vector = {2.0F, 0.0F};
    small.upsert("zero", zero);
    small.upsert("east", east);
    BOOST_TEST(!small.approximate());
    const auto all = [](const ddb::vector_entry&) { return true; };
    const auto r = small.nearest({1.0F, 0.0F}, ddb::vector_distance::cosine, 2, all);
    BOOST_REQUIRE_EQUAL(r.size(), 2U);
    BOOST_TEST(std::abs(r[0].first) < 1e-6);
    BOOST_TEST(std::abs(r[1].first - 1.0) < 1e-6);
    ddb::hnsw_vector_graph big(v, graph_pair::options());
    big.upsert("zero", zero);
    big.upsert("east", east);
    BOOST_TEST(big.approximate());
    const auto a = big.nearest({1.0F, 0.0F}, ddb::vector_distance::cosine, 2, all);
    BOOST_REQUIRE_EQUAL(a.size(), 2U);
    BOOST_TEST(std::abs(a[0].first) < 1e-6);
    BOOST_TEST(std::abs(a[1].first - 1.0) < 1e-6);
}

#endif
