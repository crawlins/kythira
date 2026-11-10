// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The invariants the AWS DynamoDB documentation states, one case each,
// through the front end as an SDK reaches it (services/dynamodb/doc/
// invariants.md lists them with their sources; the ids below are its ids).
// Each case states the rule it checks. Invariants of features not built
// yet are listed in the document as deferred, with their task, rather than
// tested here. The distributed ones (read-your-writes across nodes, a
// write surviving a leader loss) are dynamodb_raft_backend_test's.
//
// The expectations themselves were checked against DynamoDB Local: with
// KYTHIRA_DYNAMODB_ORACLE=host:port set, every case sends its requests to
// that endpoint instead of the front end (a run by hand, never in CI; the
// few cases that reach into kythira's own backend skip that part).

#define BOOST_TEST_MODULE dynamodb_invariants_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <cstddef>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

/// The oracle endpoint, "host:port", when one is set.
auto oracle() -> std::string {
    const char* v = std::getenv("KYTHIRA_DYNAMODB_ORACLE");  // NOLINT(concurrency-mt-unsafe)
    return v == nullptr ? std::string() : std::string(v);
}

/// Whether the run is against DynamoDB Local: a case where Local departs
/// from the documented behaviour (its own documentation lists the
/// differences) returns early, so a clean oracle run means every other
/// expectation is Local's.
auto local_differs() -> bool {
    return !oracle().empty();
}

/// Sends `req` to the oracle over plain HTTP.
auto oracle_send(const std::string& endpoint, const ddb::front_end_request& req) -> reply {
    namespace beast = boost::beast;
    namespace http = beast::http;
    const auto colon = endpoint.rfind(':');
    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);
    stream.connect(resolver.resolve(endpoint.substr(0, colon), endpoint.substr(colon + 1)));
    http::request<http::string_body> out{http::verb::post, "/", 11};
    for (const auto& [name, value] : req.headers) {
        out.set(name, value);
    }
    out.body() = req.body;
    out.prepare_payload();
    http::write(stream, out);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(std::uint64_t{64} << 20U);
    http::read(stream, buffer, parser);
    const auto& res = parser.get();
    return reply{static_cast<unsigned>(res.result_int()), json::parse(res.body()).as_object()};
}

/// A fixture with the tables the cases use:
/// - `tab`: pk (S) HASH, sk (N) RANGE
/// - `hsh`: pk (S) HASH only
/// - `str`: pk (S) HASH, sk (S) RANGE
/// - `bin`: pk (B) HASH, sk (B) RANGE
struct tables : fixture {
    tables() {
        if (const auto endpoint = oracle(); !endpoint.empty()) {
            remote = [endpoint](const ddb::front_end_request& r) {
                return oracle_send(endpoint, r);
            };
            // Each case starts from no tables, as it does on the front end.
            const auto listed = ok("ListTables", "{}");
            for (const auto& name : listed.at("TableNames").as_array()) {
                ok("DeleteTable", R"({"TableName":")" + std::string(name.as_string()) + R"("})");
            }
        }
        create("tab");
        ok("CreateTable", R"({"TableName":"hsh","AttributeDefinitions":)"
                          R"([{"AttributeName":"pk","AttributeType":"S"}],)"
                          R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
                          R"("BillingMode":"PAY_PER_REQUEST"})");
        ok("CreateTable", R"({"TableName":"str","AttributeDefinitions":)"
                          R"([{"AttributeName":"pk","AttributeType":"S"},)"
                          R"({"AttributeName":"sk","AttributeType":"S"}],)"
                          R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                          R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
                          R"("BillingMode":"PAY_PER_REQUEST"})");
        ok("CreateTable", R"({"TableName":"bin","AttributeDefinitions":)"
                          R"([{"AttributeName":"pk","AttributeType":"B"},)"
                          R"({"AttributeName":"sk","AttributeType":"B"}],)"
                          R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                          R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
                          R"("BillingMode":"PAY_PER_REQUEST"})");
    }

    /// Expects `op` to fail with `type` (and `message`, when given).
    auto fails(std::string_view op, const std::string& body, const std::string& type,
               const std::string& message = "") -> reply {
        auto r = call(op, body);
        BOOST_TEST(r.status == 400U, op << " " << body << " -> " << r.status);
        BOOST_TEST(r.type() == type,
                   op << " " << body << " -> " << r.type() << ": " << r.message());
        if (!message.empty()) {
            BOOST_TEST(r.message() == message);
        }
        return r;
    }

    /// Puts `{"pk":{"S":pk},"sk":{"N":sk}, extra...}` into `tab`.
    auto put_t(const std::string& pk, const std::string& sk, const std::string& extra = "")
        -> json::object {
        return ok("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":")" + pk +
                                 R"("},"sk":{"N":")" + sk + R"("})" + extra + "}}");
    }

    /// The item of `tab` under (pk, sk), consistently read; null when absent.
    auto get_t(const std::string& pk, const std::string& sk) -> json::value {
        const auto r =
            ok("GetItem", R"({"TableName":"tab","ConsistentRead":true,"Key":{"pk":{"S":")" + pk +
                              R"("},"sk":{"N":")" + sk + R"("}}})");
        const auto* it = r.if_contains("Item");
        return it == nullptr ? json::value(nullptr) : *it;
    }

    auto update_t(const std::string& pk, const std::string& sk, const std::string& rest) -> reply {
        return call("UpdateItem", R"({"TableName":"tab","Key":{"pk":{"S":")" + pk +
                                      R"("},"sk":{"N":")" + sk + R"("}},)" + rest + "}");
    }

    /// Query `tab` for partition `pk` with extra members; the response.
    auto query_t(const std::string& pk, const std::string& extra = "") -> json::object {
        return ok("Query", R"({"TableName":"tab","KeyConditionExpression":"pk = :p",)"
                           R"("ExpressionAttributeValues":{":p":{"S":")" +
                               pk + R"("}})" + extra + "}");
    }
};

auto sort_keys(const json::object& r, const char* type = "N") -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& it : r.at("Items").as_array()) {
        out.emplace_back(it.at("sk").at(type).as_string());
    }
    return out;
}

auto n_of(const json::value& item, const char* name) -> std::string {
    return std::string(item.at(name).at("N").as_string());
}

}  // namespace

// ---------------------------------------------------------------------------
// A. Names and keys
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(a01_table_names_are_3_to_255_of_the_allowed_characters, tables) {
    const auto create = [&](const std::string& name) {
        return call("CreateTable",
                    R"({"TableName":")" + name +
                        R"(","AttributeDefinitions":[{"AttributeName":"pk",)"
                        R"("AttributeType":"S"}],"KeySchema":[{"AttributeName":)"
                        R"("pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})");
    };
    BOOST_TEST(create("ab").type() == "ValidationException");
    BOOST_TEST(create(std::string(256, 'x')).type() == "ValidationException");
    BOOST_TEST(create("bad name").type() == "ValidationException");
    BOOST_TEST(create("bad/name").type() == "ValidationException");
    BOOST_TEST(create("abc").status == 200U);
    BOOST_TEST(create(std::string(255, 'y')).status == 200U);
    BOOST_TEST(create("A-z_0.9").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(a02_names_are_case_sensitive, tables) {
    if (local_differs()) {
        return;  // DynamoDB Local folds table names to one case
    }
    // Tables `tab` and `TAB` are different tables; attributes `a` and `A` are
    // different attributes.
    create("TAB");
    put_t("p", "1", R"(,"a":{"S":"lower"},"A":{"S":"upper"})");
    const auto it = get_t("p", "1");
    BOOST_TEST(it.at("a").at("S").as_string() == "lower");
    BOOST_TEST(it.at("A").at("S").as_string() == "upper");
    BOOST_TEST(ok("ListTables", "{}").at("TableNames").as_array().size() == 5U);
}

BOOST_FIXTURE_TEST_CASE(a03_attribute_names_are_at_least_one_character, tables) {
    fails("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"":{"S":"x"}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a04_key_attributes_are_string_number_or_binary, tables) {
    for (const char* type : {"BOOL", "SS", "M", "L", "NULL"}) {
        fails(
            "CreateTable",
            R"({"TableName":"bad","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":")" +
                std::string(type) +
                R"("}],"KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
                R"("BillingMode":"PAY_PER_REQUEST"})",
            "ValidationException");
    }
}

BOOST_FIXTURE_TEST_CASE(a05_partition_key_is_1_to_2048_bytes, tables) {
    ok("PutItem", R"({"TableName":"hsh","Item":{"pk":{"S":")" + std::string(2048, 'k') + R"("}}})");
    fails("PutItem",
          R"({"TableName":"hsh","Item":{"pk":{"S":")" + std::string(2049, 'k') + R"("}}})",
          "ValidationException");
    // Bytes, not characters: 1025 two-byte characters are 2050 bytes.
    std::string wide;
    for (int i = 0; i < 1025; ++i) {
        wide += "\xC3\xA9";
    }
    fails("PutItem", R"({"TableName":"hsh","Item":{"pk":{"S":")" + wide + R"("}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a06_sort_key_is_1_to_1024_bytes, tables) {
    ok("PutItem", R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":")" +
                      std::string(1024, 'k') + R"("}}})");
    fails("PutItem",
          R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":")" + std::string(1025, 'k') +
              R"("}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a07_key_attributes_cannot_be_empty, tables) {
    fails("PutItem", R"({"TableName":"hsh","Item":{"pk":{"S":""}}})", "ValidationException");
    fails("PutItem", R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":""}}})",
          "ValidationException");
    fails("PutItem", R"({"TableName":"bin","Item":{"pk":{"B":""},"sk":{"B":"AA=="}}})",
          "ValidationException");
    fails("GetItem", R"({"TableName":"hsh","Key":{"pk":{"S":""}}})", "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a08_key_attributes_must_match_the_schema_types, tables) {
    fails("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"S":"1"}}})",
          "ValidationException");
    fails("PutItem", R"({"TableName":"tab","Item":{"pk":{"N":"1"},"sk":{"N":"1"}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(a09_every_item_operation_needs_the_full_primary_key, tables) {
    fails("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"}}})", "ValidationException");
    fails("GetItem", R"({"TableName":"tab","Key":{"pk":{"S":"p"}}})", "ValidationException");
    fails("DeleteItem", R"({"TableName":"tab","Key":{"pk":{"S":"p"}}})", "ValidationException");
    fails("UpdateItem",
          R"({"TableName":"tab","Key":{"pk":{"S":"p"}},"UpdateExpression":"SET a = :a",)"
          R"("ExpressionAttributeValues":{":a":{"S":"x"}}})",
          "ValidationException");
    // Nor more than the key: a Key with another attribute is refused.
    fails("GetItem", R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"},"x":{"S":"y"}}})",
          "ValidationException");
}

// ---------------------------------------------------------------------------
// B. Data types
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(b01_numbers_have_at_most_38_significant_digits, tables) {
    put_t("p", "1", R"(,"n":{"N":")" + std::string(38, '9') + R"("})");
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"2"},"n":{"N":")" +
              std::string(39, '9') + R"("}}})",
          "ValidationException");
    // Trailing zeros of an integer are not significant digits.
    put_t("p", "3", R"(,"n":{"N":"1)" + std::string(40, '0') + R"("})");
}

BOOST_FIXTURE_TEST_CASE(b02_numbers_are_within_1e_minus_130_and_1e126, tables) {
    const std::string max = "9.9999999999999999999999999999999999999E+125";
    for (const auto& v : {std::string("1E-130"), max, "-" + max, std::string("-1E-130")}) {
        put_t("p", "1", R"(,"n":{"N":")" + v + R"("})");
    }
    for (const char* v : {"1E-131", "1E+126", "-1E+126", "-1E-131"}) {
        fails("PutItem",
              R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"n":{"N":")" +
                  std::string(v) + R"("}}})",
              "ValidationException");
    }
}

BOOST_FIXTURE_TEST_CASE(b03_numbers_are_normalized, tables) {
    // Leading and trailing zeros are trimmed; equal numbers are one key.
    put_t("p", "007.50", R"(,"n":{"N":"0001.2300"})");
    const auto it = get_t("p", "7.5");
    BOOST_TEST_REQUIRE(it.is_object());
    BOOST_TEST(n_of(it, "sk") == "7.5");
    BOOST_TEST(n_of(it, "n") == "1.23");
    put_t("p", "7.500", R"(,"n":{"N":"2"})");
    BOOST_TEST(query_t("p").at("Count").to_number<int>() == 1);
}

BOOST_FIXTURE_TEST_CASE(b04_number_sort_keys_order_numerically, tables) {
    for (const char* sk : {"10", "2", "-3", "0.5", "-0.25", "100"}) {
        put_t("p", sk);
    }
    const std::vector<std::string> want{"-3", "-0.25", "0.5", "2", "10", "100"};
    BOOST_TEST(sort_keys(query_t("p")) == want);
}

BOOST_FIXTURE_TEST_CASE(b05_string_sort_keys_order_by_utf8_bytes, tables) {
    // "A" (0x41) < "a" (0x61) < "z" (0x7A) < "¿" (0xC2 0xBF).
    for (const char* sk : {"z", "\xC2\xBF", "a", "A"}) {
        ok("PutItem", R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":")" + std::string(sk) +
                          R"("}}})");
    }
    const auto r = ok("Query", R"({"TableName":"str","KeyConditionExpression":"pk = :p",)"
                               R"("ExpressionAttributeValues":{":p":{"S":"p"}}})");
    const std::vector<std::string> want{"A", "a", "z", "\xC2\xBF"};
    BOOST_TEST(sort_keys(r, "S") == want);
}

BOOST_FIXTURE_TEST_CASE(b06_binary_sort_keys_order_as_unsigned_bytes, tables) {
    // 0x7F < 0x80 < 0xFF (base64 fw==, gA==, /w==).
    for (const char* sk : {"/w==", "fw==", "gA=="}) {
        ok("PutItem", R"({"TableName":"bin","Item":{"pk":{"B":"AA=="},"sk":{"B":")" +
                          std::string(sk) + R"("}}})");
    }
    const auto r = ok("Query", R"({"TableName":"bin","KeyConditionExpression":"pk = :p",)"
                               R"("ExpressionAttributeValues":{":p":{"B":"AA=="}}})");
    const std::vector<std::string> want{"fw==", "gA==", "/w=="};
    BOOST_TEST(sort_keys(r, "B") == want);
}

BOOST_FIXTURE_TEST_CASE(b07_sets_cannot_be_empty, tables) {
    for (const char* v : {R"({"SS":[]})", R"({"NS":[]})", R"({"BS":[]})"}) {
        fails("PutItem",
              R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"s":)" + std::string(v) +
                  "}}",
              "ValidationException");
    }
}

BOOST_FIXTURE_TEST_CASE(b08_set_elements_are_unique, tables) {
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"s":{"SS":["a","a"]}}})",
          "ValidationException");
    // Equal numbers are duplicates however they are written.
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"s":{"NS":["1","1.0"]}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(b09_set_elements_are_all_of_the_set_type, tables) {
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"s":{"NS":["1","x"]}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(b10_empty_strings_and_binaries_are_allowed_outside_keys, tables) {
    put_t("p", "1",
          R"(,"e":{"S":""},"eb":{"B":""},"l":{"L":[{"S":""}]},"m":{"M":{"k":{"S":""}}},)"
          R"("ss":{"SS":[""]},"bs":{"BS":[""]})");
    const auto it = get_t("p", "1");
    BOOST_TEST(it.at("e").at("S").as_string().empty());
    BOOST_TEST(it.at("eb").at("B").as_string().empty());
    BOOST_TEST(it.at("ss").at("SS").as_array().size() == 1U);
}

BOOST_FIXTURE_TEST_CASE(b11_empty_lists_and_maps_are_allowed, tables) {
    put_t("p", "1", R"(,"l":{"L":[]},"m":{"M":{}})");
    const auto it = get_t("p", "1");
    BOOST_TEST(it.at("l").at("L").as_array().empty());
    BOOST_TEST(it.at("m").at("M").as_object().empty());
}

BOOST_FIXTURE_TEST_CASE(b12_documents_nest_at_most_32_levels, tables) {
    const auto nested = [](int depth) {
        std::string v = R"({"S":"x"})";
        for (int i = 0; i < depth; ++i) {
            v = R"({"L":[)" + v + "]}";
        }
        return v;
    };
    // The attribute is level 1: 31 nested lists or maps are 32 levels (DynamoDB
    // Local takes 31 and refuses 32).
    put_t("p", "1", R"(,"d":)" + nested(31));
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"2"},"d":)" + nested(32) + "}}",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(b13_items_are_at_most_400_kb_with_names_counted, tables) {
    // pk "p" (2 + 1) + sk 1 (2 + 2: a number takes a byte per two digits
    // plus one) + name "v" (1) + the value: 400 KB in all. DynamoDB Local
    // takes exactly this value and refuses one byte more.
    const std::size_t fixed = 3 + 4 + 1;
    const std::string fits(400 * 1024 - fixed, 'x');
    put_t("p", "1", R"(,"v":{"S":")" + fits + R"("})");
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"v":{"S":")" + fits +
              R"(x"}}})",
          "ValidationException");
    // The documentation's example: two names and two one-byte values.
    ddb::item it{{"shirt-color", ddb::attribute_value::string("R")},
                 {"shirt-size", ddb::attribute_value::string("M")}};
    BOOST_TEST(ddb::item_size(it) == 23U);
}

BOOST_FIXTURE_TEST_CASE(b14_a_null_attribute_exists, tables) {
    put_t("p", "1", R"(,"n":{"NULL":true})");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET x = :x","ConditionExpression":)"
                        R"x("attribute_exists(n)","ExpressionAttributeValues":{":x":{"S":"y"}})x")
                   .status == 200U);
}

// ---------------------------------------------------------------------------
// C. Condition and filter expressions
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(c01_expressions_are_at_most_4_kb, tables) {
    put_t("p", "1");
    // `a = :v OR a = :v ...` padded past 4096 bytes.
    std::string cond = "attribute_not_exists(zz)";
    while (cond.size() <= 4096) {
        cond += " OR attribute_not_exists(zz)";
    }
    fails("DeleteItem",
          R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},"ConditionExpression":")" +
              cond + R"("})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(c02_a_placeholder_is_at_most_255_bytes, tables) {
    put_t("p", "1");
    const std::string name = "#" + std::string(255, 'n');
    fails("DeleteItem",
          R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},"ConditionExpression":)"
          R"("attribute_not_exists()" +
              name + R"x()","ExpressionAttributeNames":{")x" + name + R"(":"a"}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(c03_an_expression_has_at_most_300_operators, tables) {
    put_t("p", "1", R"(,"a":{"N":"1"})");
    const auto with = [](int comparisons) {
        std::string c = "a = :v";
        for (int i = 1; i < comparisons; ++i) {
            c += " OR a = :v";
        }
        return c;
    };
    const auto del = [&](const std::string& cond) {
        return call(
            "DeleteItem",
            R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},"ConditionExpression":")" +
                cond + R"(","ExpressionAttributeValues":{":v":{"N":"2"}}})");
    };
    // 150 comparisons and 149 ORs: 299 operators.
    BOOST_TEST(del(with(150)).type() == "ConditionalCheckFailedException");
    // 151 and 150: 301.
    const auto r = del(with(151));
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message().find("The expression contains too many operators; operator count: "
                                "301") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(c04_in_takes_at_most_100_operands, tables) {
    put_t("p", "1", R"(,"a":{"N":"1"})");
    const auto in = [&](int n) {
        std::string list = ":v";
        for (int i = 1; i < n; ++i) {
            list += ", :v";
        }
        return call(
            "DeleteItem",
            R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},"ConditionExpression":"a IN ()" +
                list + R"x()","ExpressionAttributeValues":{":v":{"N":"2"}}})x");
    };
    BOOST_TEST(in(100).type() == "ConditionalCheckFailedException");
    BOOST_TEST(in(101).type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(c05_reserved_words_need_a_placeholder, tables) {
    // `Status` is on the reserved-word list (`size` is a function name, not
    // a reserved word).
    put_t("p", "1", R"(,"Status":{"N":"1"})");
    fails("DeleteItem",
          R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
          R"x("ConditionExpression":"attribute_exists(Status)"})x",
          "ValidationException");
    BOOST_TEST(call("DeleteItem", R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                                  R"x("ConditionExpression":"attribute_exists(#s)",)x"
                                  R"("ExpressionAttributeNames":{"#s":"Status"}})")
                   .status == 200U);
}

BOOST_FIXTURE_TEST_CASE(c06_and_binds_tighter_than_or_and_parentheses_override, tables) {
    // a = true, b = true, c = false: `a OR b AND c` holds, `(a OR b) AND c`
    // does not.
    put_t("p", "1", R"(,"a":{"N":"1"},"b":{"N":"1"},"c":{"N":"0"})");
    const auto try_cond = [&](const std::string& cond) {
        return update_t("p", "1",
                        R"("UpdateExpression":"SET x = :one","ConditionExpression":")" + cond +
                            R"(","ExpressionAttributeValues":{":one":{"N":"1"}})");
    };
    BOOST_TEST(try_cond("a = :one OR b = :one AND c = :one").status == 200U);
    BOOST_TEST(try_cond("(a = :one OR b = :one) AND c = :one").type() ==
               "ConditionalCheckFailedException");
    // NOT binds tighter than AND.
    BOOST_TEST(try_cond("NOT c = :one AND a = :one").status == 200U);
}

BOOST_FIXTURE_TEST_CASE(c07_between_is_inclusive_at_both_ends, tables) {
    for (const char* sk : {"1", "2", "3", "4"}) {
        put_t("p", sk);
    }
    const auto r =
        ok("Query",
           R"({"TableName":"tab","KeyConditionExpression":"pk = :p AND sk BETWEEN :a AND :b",)"
           R"("ExpressionAttributeValues":{":p":{"S":"p"},":a":{"N":"2"},":b":{"N":"3"}}})");
    const std::vector<std::string> want{"2", "3"};
    BOOST_TEST(sort_keys(r) == want);
}

BOOST_FIXTURE_TEST_CASE(c08_attribute_type_takes_every_type_token, tables) {
    put_t("p", "1",
          R"(,"S":{"S":"x"},"N":{"N":"1"},"B":{"B":"AA=="},"BOOL":{"BOOL":true},)"
          R"("NULL":{"NULL":true},"L":{"L":[]},"M":{"M":{}},"SS":{"SS":["a"]},)"
          R"("NS":{"NS":["1"]},"BS":{"BS":["AA=="]})");
    for (const char* t : {"S", "N", "B", "BOOL", "NULL", "L", "M", "SS", "NS", "BS"}) {
        const auto r =
            update_t("p", "1",
                     R"("UpdateExpression":"SET x = :t","ConditionExpression":)"
                     R"x("attribute_type(#a, :t)","ExpressionAttributeNames":{"#a":")x" +
                         std::string(t) + R"("},"ExpressionAttributeValues":{":t":{"S":")" +
                         std::string(t) + R"("}})");
        BOOST_TEST(r.status == 200U, t);
    }
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET x = :t","ConditionExpression":)"
                        R"x("attribute_type(S, :t)","ExpressionAttributeValues":{":t":{"S":"N"}})x")
                   .type() == "ConditionalCheckFailedException");
}

BOOST_FIXTURE_TEST_CASE(c09_contains_works_on_strings_sets_and_lists, tables) {
    put_t("p", "1",
          R"(,"str":{"S":"hello"},"set":{"SS":["a","b"]},"lst":{"L":[{"N":"7"},{"S":"q"}]})");
    const auto holds = [&](const std::string& cond, const std::string& values) {
        return update_t("p", "1",
                        R"("UpdateExpression":"SET x = :one","ConditionExpression":")" + cond +
                            R"(","ExpressionAttributeValues":{":one":{"N":"1"},)" + values + "}")
                   .status == 200U;
    };
    BOOST_TEST(holds("contains(str, :v)", R"(":v":{"S":"ell"})"));
    BOOST_TEST(
        holds("contains(#s, :v)", R"(":v":{"S":"b"}},"ExpressionAttributeNames":{"#s":"set")"));
    BOOST_TEST(holds("contains(lst, :v)", R"(":v":{"N":"7"})"));
    BOOST_TEST(!holds("contains(lst, :v)", R"(":v":{"N":"8"})"));
    // The path and the operand must differ.
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET x = :one","ConditionExpression":)"
                        R"x("contains(str, str)","ExpressionAttributeValues":{":one":{"N":"1"}})x")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(c10_size_counts_by_type, tables) {
    // String: characters' bytes; binary: bytes; set, list, map: elements.
    put_t("p", "1",
          R"(,"str":{"S":"abcd"},"bin":{"B":"AAECAw=="},"set":{"SS":["a","b","c"]},)"
          R"("lst":{"L":[{"N":"1"},{"N":"2"}]},"map":{"M":{"k":{"N":"1"}}})");
    // `set` and `map` are reserved words, hence the placeholders.
    const auto holds = [&](const std::string& path, int n) {
        const auto names = path[0] == '#' ? R"(,"ExpressionAttributeNames":{")" + path + R"(":")" +
                                                (path == "#s" ? "set" : "map") + R"("})"
                                          : std::string();
        return update_t("p", "1",
                        R"("UpdateExpression":"SET x = :n","ConditionExpression":"size()" + path +
                            R"() = :n","ExpressionAttributeValues":{":n":{"N":")" +
                            std::to_string(n) + R"("}})" + names)
                   .status == 200U;
    };
    BOOST_TEST(holds("str", 4));
    BOOST_TEST(holds("bin", 4));
    BOOST_TEST(holds("#s", 3));
    BOOST_TEST(holds("lst", 2));
    BOOST_TEST(holds("#m", 1));
    BOOST_TEST(!holds("str", 3));
}

BOOST_FIXTURE_TEST_CASE(c11_begins_with_matches_string_prefixes, tables) {
    put_t("p", "1", R"(,"link":{"S":"http://example"})");
    const auto with = [&](const char* prefix) {
        return update_t("p", "1",
                        R"("UpdateExpression":"SET x = :p","ConditionExpression":)"
                        R"x("begins_with(link, :p)","ExpressionAttributeValues":{":p":{"S":")x" +
                            std::string(prefix) + R"("}})");
    };
    BOOST_TEST(with("http://").status == 200U);
    BOOST_TEST(with("https://").type() == "ConditionalCheckFailedException");
}

BOOST_FIXTURE_TEST_CASE(c12_comparisons_of_mismatched_types_are_false, tables) {
    put_t("p", "1", R"(,"a":{"N":"1"})");
    // A number is neither equal to nor ordered against a string.
    for (const char* op : {"=", "<", ">", "<=", ">="}) {
        const auto r =
            update_t("p", "1",
                     R"("UpdateExpression":"SET x = :s","ConditionExpression":"a )" +
                         std::string(op) + R"( :s","ExpressionAttributeValues":{":s":{"S":"1"}})");
        BOOST_TEST(r.type() == "ConditionalCheckFailedException", op);
    }
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET x = :s","ConditionExpression":"a <> :s",)"
                        R"("ExpressionAttributeValues":{":s":{"S":"1"}})")
                   .status == 200U);
}

// ---------------------------------------------------------------------------
// D. Update expressions
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(d01_each_clause_appears_at_most_once, tables) {
    put_t("p", "1");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET a = :v SET b = :v",)"
                        R"("ExpressionAttributeValues":{":v":{"N":"1"}})")
                   .type() == "ValidationException");
    BOOST_TEST(update_t("p", "1", R"("UpdateExpression":"REMOVE a REMOVE b")").type() ==
               "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(d02_actions_read_the_pre_update_item, tables) {
    // The documentation's example: REMOVE a SET b = a, c = b.
    put_t("id", "1", R"(,"a":{"N":"1"},"b":{"N":"2"},"c":{"N":"3"})");
    BOOST_TEST(update_t("id", "1", R"("UpdateExpression":"REMOVE a SET b = a, c = b")").status ==
               200U);
    const auto it = get_t("id", "1");
    BOOST_TEST(!it.as_object().contains("a"));
    BOOST_TEST(n_of(it, "b") == "1");
    BOOST_TEST(n_of(it, "c") == "2");
}

BOOST_FIXTURE_TEST_CASE(d03_set_adds_or_overwrites, tables) {
    put_t("p", "1", R"(,"a":{"S":"old"})");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET a = :n, b = :n",)"
                        R"("ExpressionAttributeValues":{":n":{"S":"new"}})")
                   .status == 200U);
    const auto it = get_t("p", "1");
    BOOST_TEST(it.at("a").at("S").as_string() == "new");
    BOOST_TEST(it.at("b").at("S").as_string() == "new");
}

BOOST_FIXTURE_TEST_CASE(d04_if_not_exists_keeps_a_present_value, tables) {
    put_t("p", "1", R"(,"price":{"N":"5"})");
    BOOST_TEST(
        update_t("p", "1",
                 R"("UpdateExpression":"SET price = if_not_exists(price, :p), extra = )"
                 R"x(if_not_exists(extra, :p)","ExpressionAttributeValues":{":p":{"N":"9"}})x")
            .status == 200U);
    const auto it = get_t("p", "1");
    BOOST_TEST(n_of(it, "price") == "5");
    BOOST_TEST(n_of(it, "extra") == "9");
}

BOOST_FIXTURE_TEST_CASE(d05_list_append_appends_or_prepends_by_argument_order, tables) {
    put_t("p", "1", R"(,"l":{"L":[{"N":"2"}]})");
    update_t("p", "1",
             R"x("UpdateExpression":"SET l = list_append(l, :end)",)x"
             R"("ExpressionAttributeValues":{":end":{"L":[{"N":"3"}]}})");
    update_t("p", "1",
             R"x("UpdateExpression":"SET l = list_append(:front, l)",)x"
             R"("ExpressionAttributeValues":{":front":{"L":[{"N":"1"}]}})");
    const auto item = get_t("p", "1");
    const auto& l = item.at("l").at("L").as_array();
    BOOST_TEST_REQUIRE(l.size() == 3U);
    BOOST_TEST(l[0].at("N").as_string() == "1");
    BOOST_TEST(l[2].at("N").as_string() == "3");
}

BOOST_FIXTURE_TEST_CASE(d06_setting_a_list_index_past_the_end_appends, tables) {
    put_t("p", "1", R"(,"l":{"L":[{"N":"0"}]})");
    update_t("p", "1",
             R"("UpdateExpression":"SET l[0] = :a, l[9] = :b",)"
             R"("ExpressionAttributeValues":{":a":{"N":"5"},":b":{"N":"6"}})");
    const auto item = get_t("p", "1");
    const auto& l = item.at("l").at("L").as_array();
    BOOST_TEST_REQUIRE(l.size() == 2U);
    BOOST_TEST(l[0].at("N").as_string() == "5");
    BOOST_TEST(l[1].at("N").as_string() == "6");
}

BOOST_FIXTURE_TEST_CASE(d07_a_nested_set_needs_its_parent_map, tables) {
    put_t("p", "1");
    fails("UpdateItem",
          R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
          R"("UpdateExpression":"SET m.k = :v","ExpressionAttributeValues":{":v":{"N":"1"}}})",
          "ValidationException",
          "The document path provided in the update expression is invalid for update");
    put_t("p", "1", R"(,"m":{"M":{}})");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET m.k = :v",)"
                        R"("ExpressionAttributeValues":{":v":{"N":"1"}})")
                   .status == 200U);
}

BOOST_FIXTURE_TEST_CASE(d08_arithmetic_works_on_numbers_only, tables) {
    put_t("p", "1", R"(,"price":{"N":"10"},"name":{"S":"x"})");
    update_t("p", "1",
             R"("UpdateExpression":"SET price = price - :d","ExpressionAttributeValues":)"
             R"({":d":{"N":"2.5"}})");
    BOOST_TEST(n_of(get_t("p", "1"), "price") == "7.5");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET name = name + :d",)"
                        R"("ExpressionAttributeValues":{":d":{"N":"1"}})")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(d09_remove_is_a_no_op_when_absent_and_closes_list_gaps, tables) {
    put_t("p", "1", R"(,"l":{"L":[{"N":"0"},{"N":"1"},{"N":"2"},{"N":"3"}]})");
    BOOST_TEST(update_t("p", "1", R"("UpdateExpression":"REMOVE nothing, l[1], l[2]")").status ==
               200U);
    const auto item = get_t("p", "1");
    const auto& l = item.at("l").at("L").as_array();
    BOOST_TEST_REQUIRE(l.size() == 2U);
    BOOST_TEST(l[0].at("N").as_string() == "0");
    BOOST_TEST(l[1].at("N").as_string() == "3");
}

BOOST_FIXTURE_TEST_CASE(d10_add_takes_numbers_from_zero_and_unions_sets, tables) {
    put_t("p", "1", R"(,"s":{"SS":["a"]},"str":{"S":"x"})");
    update_t("p", "1",
             R"("UpdateExpression":"ADD n :five, s :more","ExpressionAttributeValues":)"
             R"({":five":{"N":"5"},":more":{"SS":["a","b"]}})");
    update_t("p", "1",
             R"("UpdateExpression":"ADD n :neg","ExpressionAttributeValues":{":neg":{"N":"-2"}})");
    const auto it = get_t("p", "1");
    BOOST_TEST(n_of(it, "n") == "3");
    BOOST_TEST(it.at("s").at("SS").as_array().size() == 2U);
    // Only numbers and sets.
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"ADD str :v","ExpressionAttributeValues":)"
                        R"({":v":{"S":"y"}})")
                   .type() == "ValidationException");
    // A set of another element type.
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"ADD s :v","ExpressionAttributeValues":)"
                        R"({":v":{"NS":["1"]}})")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(d11_delete_removes_set_elements_only, tables) {
    put_t("p", "1", R"(,"s":{"SS":["a","b","c"]},"str":{"S":"x"})");
    update_t("p", "1",
             R"("UpdateExpression":"DELETE s :v","ExpressionAttributeValues":)"
             R"({":v":{"SS":["a","c"]}})");
    const auto item = get_t("p", "1");
    const auto& s = item.at("s").at("SS").as_array();
    BOOST_TEST_REQUIRE(s.size() == 1U);
    BOOST_TEST(s[0].as_string() == "b");
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"DELETE str :v","ExpressionAttributeValues":)"
                        R"({":v":{"SS":["x"]}})")
                   .type() == "ValidationException");
    // An empty set operand is an error.
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"DELETE s :v","ExpressionAttributeValues":)"
                        R"({":v":{"SS":[]}})")
                   .type() == "ValidationException");
    // Deleting every element removes the attribute (no empty set is stored).
    update_t("p", "1",
             R"("UpdateExpression":"DELETE s :v","ExpressionAttributeValues":{":v":{"SS":["b"]}})");
    BOOST_TEST(!get_t("p", "1").as_object().contains("s"));
}

BOOST_FIXTURE_TEST_CASE(d12_key_attributes_cannot_be_updated, tables) {
    put_t("p", "1");
    for (const char* expr : {"SET pk = :s", "SET sk = :n", "REMOVE sk", "ADD sk :n"}) {
        const auto r = update_t("p", "1",
                                R"("UpdateExpression":")" + std::string(expr) +
                                    R"(","ExpressionAttributeValues":{":s":{"S":"q"},)"
                                    R"(":n":{"N":"2"}})");
        BOOST_TEST(r.type() == "ValidationException", expr);
    }
}

BOOST_FIXTURE_TEST_CASE(d13_update_creates_an_absent_item, tables) {
    BOOST_TEST(update_t("new", "1",
                        R"("UpdateExpression":"SET a = :v",)"
                        R"("ExpressionAttributeValues":{":v":{"S":"x"}})")
                   .status == 200U);
    BOOST_TEST(get_t("new", "1").at("a").at("S").as_string() == "x");
}

BOOST_FIXTURE_TEST_CASE(d14_update_return_values, tables) {
    const std::string set = R"("UpdateExpression":"SET a = :v","ExpressionAttributeValues":)"
                            R"({":v":{"N":"2"}},"ReturnValues":")";
    // On an upsert: ALL_OLD and UPDATED_OLD return nothing; ALL_NEW the
    // whole item; UPDATED_NEW the updated attributes.
    BOOST_TEST(!update_t("u", "1", set + R"(ALL_OLD")").body.contains("Attributes"));
    BOOST_TEST(!update_t("u", "2", set + R"(UPDATED_OLD")").body.contains("Attributes"));
    BOOST_TEST(update_t("u", "3", set + R"(ALL_NEW")").body.at("Attributes").as_object().size() ==
               3U);
    BOOST_TEST(
        update_t("u", "4", set + R"(UPDATED_NEW")").body.at("Attributes").as_object().size() == 1U);
    // On an existing item.
    put_t("p", "1", R"(,"a":{"N":"1"},"b":{"N":"9"})");
    const auto old_all = update_t("p", "1", set + R"(ALL_OLD")").body.at("Attributes");
    BOOST_TEST(n_of(old_all, "a") == "1");
    BOOST_TEST(old_all.as_object().contains("b"));
    const std::string set3 = R"("UpdateExpression":"SET a = :v","ExpressionAttributeValues":)"
                             R"({":v":{"N":"3"}},"ReturnValues":")";
    const auto old_upd = update_t("p", "1", set3 + R"(UPDATED_OLD")").body.at("Attributes");
    BOOST_TEST(old_upd.as_object().size() == 1U);
    BOOST_TEST(n_of(old_upd, "a") == "2");
    const auto new_upd = update_t("p", "1", set + R"(UPDATED_NEW")").body.at("Attributes");
    BOOST_TEST(new_upd.as_object().size() == 1U);
    BOOST_TEST(n_of(new_upd, "a") == "2");
}

// ---------------------------------------------------------------------------
// E. Item writes
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(e01_put_replaces_the_whole_item, tables) {
    put_t("p", "1", R"(,"a":{"S":"x"},"b":{"S":"y"})");
    put_t("p", "1", R"(,"c":{"S":"z"})");
    const auto it = get_t("p", "1").as_object();
    BOOST_TEST(!it.contains("a"));
    BOOST_TEST(!it.contains("b"));
    BOOST_TEST(it.contains("c"));
}

BOOST_FIXTURE_TEST_CASE(e02_put_all_old_returns_the_replaced_item_only, tables) {
    const auto first = ok("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},)"
                                     R"("a":{"S":"x"}},"ReturnValues":"ALL_OLD"})");
    BOOST_TEST(!first.contains("Attributes"));
    const auto second =
        ok("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                      R"("ReturnValues":"ALL_OLD"})");
    BOOST_TEST(second.at("Attributes").at("a").at("S").as_string() == "x");
    // PutItem takes NONE and ALL_OLD only.
    fails("PutItem",
          R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},"ReturnValues":"ALL_NEW"})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(e03_delete_returns_the_deleted_item_and_succeeds_when_absent, tables) {
    put_t("p", "1", R"(,"a":{"S":"x"})");
    const std::string key = R"("Key":{"pk":{"S":"p"},"sk":{"N":"1"}},"ReturnValues":"ALL_OLD")";
    const auto gone = ok("DeleteItem", R"({"TableName":"tab",)" + key + "}");
    BOOST_TEST(gone.at("Attributes").at("a").at("S").as_string() == "x");
    const auto again = ok("DeleteItem", R"({"TableName":"tab",)" + key + "}");
    BOOST_TEST(!again.contains("Attributes"));
    BOOST_TEST(get_t("p", "1").is_null());
}

BOOST_FIXTURE_TEST_CASE(e04_a_failed_condition_changes_nothing, tables) {
    put_t("p", "1", R"(,"price":{"N":"20"})");
    const auto r = call(
        "PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"price":{"N":"1"}},)"
                   R"("ConditionExpression":"price = :p","ExpressionAttributeValues":)"
                   R"({":p":{"N":"10"}}})");
    BOOST_TEST(r.type() == "ConditionalCheckFailedException");
    BOOST_TEST(r.message() == "The conditional request failed");
    BOOST_TEST(!r.body.contains("Item"));
    BOOST_TEST(n_of(get_t("p", "1"), "price") == "20");
    BOOST_TEST(call("DeleteItem",
                    R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                    R"("ConditionExpression":"price = :p","ExpressionAttributeValues":)"
                    R"({":p":{"N":"10"}}})")
                   .type() == "ConditionalCheckFailedException");
    BOOST_TEST(get_t("p", "1").is_object());
}

BOOST_FIXTURE_TEST_CASE(e05_a_condition_on_an_absent_item_sees_no_attributes, tables) {
    // Never written, and written then deleted: both are "no item".
    const std::string cond = R"x("ConditionExpression":"attribute_exists(pk)")x";
    BOOST_TEST(call("PutItem",
                    R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},)" + cond + "}")
                   .type() == "ConditionalCheckFailedException");
    put_t("p", "1");
    ok("DeleteItem", R"({"TableName":"tab","Key":{"pk":{"S":"p"},"sk":{"N":"1"}}})");
    BOOST_TEST(call("PutItem",
                    R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},)" + cond + "}")
                   .type() == "ConditionalCheckFailedException");
    BOOST_TEST(call("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                               R"x("ConditionExpression":"attribute_not_exists(pk)"})x")
                   .status == 200U);
}

BOOST_FIXTURE_TEST_CASE(e06_return_values_on_condition_failure_carries_the_item, tables) {
    put_t("p", "1", R"(,"a":{"S":"x"})");
    const auto r = call("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"}},)"
                                   R"x("ConditionExpression":"attribute_not_exists(pk)",)x"
                                   R"("ReturnValuesOnConditionCheckFailure":"ALL_OLD"})");
    BOOST_TEST(r.type() == "ConditionalCheckFailedException");
    BOOST_TEST(r.body.at("Item").at("a").at("S").as_string() == "x");
}

BOOST_FIXTURE_TEST_CASE(e07_atomic_counters_apply_every_increment, tables) {
    for (int i = 0; i < 25; ++i) {
        update_t("c", "1",
                 R"("UpdateExpression":"SET n = if_not_exists(n, :z) + :one",)"
                 R"("ExpressionAttributeValues":{":z":{"N":"0"},":one":{"N":"1"}})");
    }
    BOOST_TEST(n_of(get_t("c", "1"), "n") == "25");
}

BOOST_FIXTURE_TEST_CASE(e08_a_conditional_update_is_safe_to_retry, tables) {
    // "Increase Price by 3 only if it is 20": the retry fails the condition
    // and the price stays 23.
    put_t("p", "1", R"(,"price":{"N":"20"})");
    const std::string upd = R"("UpdateExpression":"SET price = price + :d",)"
                            R"("ConditionExpression":"price = :c","ExpressionAttributeValues":)"
                            R"({":d":{"N":"3"},":c":{"N":"20"}})";
    BOOST_TEST(update_t("p", "1", upd).status == 200U);
    BOOST_TEST(update_t("p", "1", upd).type() == "ConditionalCheckFailedException");
    BOOST_TEST(n_of(get_t("p", "1"), "price") == "23");
}

BOOST_FIXTURE_TEST_CASE(e09_a_failed_write_is_never_read, tables) {
    // Read-committed: a write that did not succeed leaves no trace.
    BOOST_TEST(
        call("PutItem", R"({"TableName":"tab","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"n":{"N":")" +
                            std::string(39, '9') + R"("}}})")
            .status == 400U);
    BOOST_TEST(get_t("p", "1").is_null());
    BOOST_TEST(update_t("p", "1",
                        R"("UpdateExpression":"SET a = :v","ConditionExpression":)"
                        R"x("attribute_exists(pk)","ExpressionAttributeValues":{":v":{"N":"1"}})x")
                   .type() == "ConditionalCheckFailedException");
    BOOST_TEST(get_t("p", "1").is_null());
}

// ---------------------------------------------------------------------------
// F. Query
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(f01_the_key_condition_tests_the_partition_key_for_equality, tables) {
    // Every value given is used, so a refusal is the condition's.
    const auto q = [&](const std::string& cond, const std::string& values) {
        return call("Query", R"({"TableName":"tab","KeyConditionExpression":")" + cond +
                                 R"(","ExpressionAttributeValues":{)" + values + "}}");
    };
    const std::string p = R"(":p":{"S":"p"})";
    const std::string pn = p + R"(,":n":{"N":"1"})";
    BOOST_TEST(q("pk > :p", p).type() == "ValidationException");
    BOOST_TEST(q("sk = :n", R"(":n":{"N":"1"})").type() == "ValidationException");
    BOOST_TEST(q("pk = :p OR sk = :n", pn).type() == "ValidationException");
    BOOST_TEST(q("pk = :p", p).status == 200U);
    BOOST_TEST(q("pk = :p AND sk = :n", pn).status == 200U);
}

BOOST_FIXTURE_TEST_CASE(f02_sort_key_conditions_take_the_documented_operators, tables) {
    for (const char* sk : {"1", "2", "3"}) {
        put_t("p", sk);
    }
    const auto count = [&](const std::string& cond) {
        const std::string m = cond.find(":m") == std::string::npos ? "" : R"(,":m":{"N":"3"})";
        return ok("Query", R"({"TableName":"tab","KeyConditionExpression":"pk = :p AND )" + cond +
                               R"(","ExpressionAttributeValues":{":p":{"S":"p"},":n":{"N":"2"})" +
                               m + "}}")
            .at("Count")
            .to_number<int>();
    };
    BOOST_TEST(count("sk = :n") == 1);
    BOOST_TEST(count("sk < :n") == 1);
    BOOST_TEST(count("sk <= :n") == 2);
    BOOST_TEST(count("sk > :n") == 1);
    BOOST_TEST(count("sk >= :n") == 2);
    BOOST_TEST(count("sk BETWEEN :n AND :m") == 2);
    // `<>`, IN and the other functions are not key conditions.
    BOOST_TEST(call("Query",
                    R"({"TableName":"tab","KeyConditionExpression":"pk = :p AND sk <> :n",)"
                    R"("ExpressionAttributeValues":{":p":{"S":"p"},":n":{"N":"2"}}})")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(f03_begins_with_is_refused_on_a_number_sort_key, tables) {
    fails("Query",
          R"x({"TableName":"tab","KeyConditionExpression":"pk = :p AND begins_with(sk, :n)",)x"
          R"("ExpressionAttributeValues":{":p":{"S":"p"},":n":{"N":"1"}}})",
          "ValidationException");
    ok("PutItem", R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":"abc"}}})");
    ok("PutItem", R"({"TableName":"str","Item":{"pk":{"S":"p"},"sk":{"S":"xyz"}}})");
    BOOST_TEST(ok("Query", R"({"TableName":"str","KeyConditionExpression":)"
                           R"x("pk = :p AND begins_with(sk, :a)","ExpressionAttributeValues":)x"
                           R"({":p":{"S":"p"},":a":{"S":"ab"}}})")
                   .at("Count")
                   .to_number<int>() == 1);
}

BOOST_FIXTURE_TEST_CASE(f04_results_are_in_sort_key_order_either_way, tables) {
    for (const char* sk : {"3", "1", "2"}) {
        put_t("p", sk);
    }
    const std::vector<std::string> up{"1", "2", "3"};
    const std::vector<std::string> down{"3", "2", "1"};
    BOOST_TEST(sort_keys(query_t("p")) == up);
    BOOST_TEST(sort_keys(query_t("p", R"(,"ScanIndexForward":false)")) == down);
}

BOOST_FIXTURE_TEST_CASE(f05_limit_counts_items_evaluated_before_the_filter, tables) {
    for (int i = 1; i <= 6; ++i) {
        put_t("p", std::to_string(i),
              R"(,"even":{"BOOL":)" + std::string(i % 2 == 0 ? "true" : "false") + "}");
    }
    const auto r =
        query_t("p", R"(,"Limit":3,"FilterExpression":"even = :t",)"
                     R"("ExpressionAttributeValues":{":p":{"S":"p"},":t":{"BOOL":true}})");
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 3);
    BOOST_TEST(r.at("Count").to_number<int>() == 1);
    BOOST_TEST(r.contains("LastEvaluatedKey"));
    fails("Query",
          R"({"TableName":"tab","KeyConditionExpression":"pk = :p","Limit":0,)"
          R"("ExpressionAttributeValues":{":p":{"S":"p"}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(f06_a_page_reads_at_most_1_mb_before_filtering, tables) {
    if (local_differs()) {
        return;  // DynamoDB Local does not cap a page at 1 MB
    }
    // 30 items of about 100 KB: a page stops after about 1 MB, whatever
    // the filter keeps.
    const std::string big(100 * 1024, 'x');
    for (int i = 0; i < 30; ++i) {
        put_t("p", std::to_string(i), R"(,"v":{"S":")" + big + R"("})");
    }
    const auto r = query_t("p", R"x(,"FilterExpression":"attribute_not_exists(v)")x");
    BOOST_TEST(r.at("Count").to_number<int>() == 0);
    const auto scanned = r.at("ScannedCount").to_number<int>();
    BOOST_TEST(scanned >= 10);
    BOOST_TEST(scanned <= 11);
    BOOST_TEST(r.contains("LastEvaluatedKey"));
}

BOOST_FIXTURE_TEST_CASE(f07_pages_resume_after_the_last_evaluated_key, tables) {
    for (int i = 0; i < 20; ++i) {
        put_t("p", std::to_string(i));
    }
    std::vector<std::string> seen;
    std::string start;
    int pages = 0;
    for (;;) {
        const auto r = query_t(
            "p", R"(,"Limit":3)" + (start.empty() ? "" : R"(,"ExclusiveStartKey":)" + start));
        for (const auto& sk : sort_keys(r)) {
            seen.push_back(sk);
        }
        ++pages;
        const auto* last = r.if_contains("LastEvaluatedKey");
        if (last == nullptr) {
            break;
        }
        start = json::serialize(*last);
    }
    BOOST_TEST(seen.size() == 20U);
    BOOST_TEST(std::set<std::string>(seen.begin(), seen.end()).size() == 20U);
    BOOST_TEST(pages >= 7);
}

BOOST_FIXTURE_TEST_CASE(f08_a_page_may_be_empty_yet_not_the_last, tables) {
    for (int i = 0; i < 5; ++i) {
        put_t("p", std::to_string(i));
    }
    const auto r = query_t("p", R"x(,"Limit":2,"FilterExpression":"attribute_exists(nothing)")x");
    BOOST_TEST(r.at("Items").as_array().empty());
    BOOST_TEST(r.contains("LastEvaluatedKey"));
}

BOOST_FIXTURE_TEST_CASE(f09_a_filter_cannot_name_a_key_attribute, tables) {
    fails("Query",
          R"({"TableName":"tab","KeyConditionExpression":"pk = :p","FilterExpression":"sk > :n",)"
          R"("ExpressionAttributeValues":{":p":{"S":"p"},":n":{"N":"1"}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(f10_count_equals_scanned_count_without_a_filter, tables) {
    for (int i = 0; i < 4; ++i) {
        put_t("p", std::to_string(i));
    }
    const auto r = query_t("p");
    BOOST_TEST(r.at("Count").to_number<int>() == 4);
    BOOST_TEST(r.at("ScannedCount").to_number<int>() == 4);
}

BOOST_FIXTURE_TEST_CASE(f11_select_rules, tables) {
    put_t("p", "1", R"(,"a":{"S":"x"})");
    // COUNT returns no items.
    const auto c = query_t("p", R"(,"Select":"COUNT")");
    BOOST_TEST(!c.contains("Items"));
    BOOST_TEST(c.at("Count").to_number<int>() == 1);
    // A projection goes with SPECIFIC_ATTRIBUTES or no Select.
    fails("Query",
          R"({"TableName":"tab","KeyConditionExpression":"pk = :p","ProjectionExpression":"a",)"
          R"("Select":"ALL_ATTRIBUTES","ExpressionAttributeValues":{":p":{"S":"p"}}})",
          "ValidationException");
    const auto proj = query_t("p", R"(,"ProjectionExpression":"a","Select":"SPECIFIC_ATTRIBUTES")");
    BOOST_TEST(proj.at("Items").as_array()[0].as_object().size() == 1U);
    // ALL_PROJECTED_ATTRIBUTES needs an index.
    fails("Query",
          R"({"TableName":"tab","KeyConditionExpression":"pk = :p",)"
          R"("Select":"ALL_PROJECTED_ATTRIBUTES","ExpressionAttributeValues":{":p":{"S":"p"}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(f12_no_match_is_an_empty_result_not_an_error, tables) {
    const auto r = query_t("nobody");
    BOOST_TEST(r.at("Items").as_array().empty());
    BOOST_TEST(r.at("Count").to_number<int>() == 0);
    BOOST_TEST(!r.contains("LastEvaluatedKey"));
}

BOOST_FIXTURE_TEST_CASE(f13_reads_of_a_missing_table_are_resource_not_found, tables) {
    for (const auto& [op, body] : std::vector<std::pair<std::string, std::string>>{
             {"Query", R"({"TableName":"none","KeyConditionExpression":"pk = :p",)"
                       R"("ExpressionAttributeValues":{":p":{"S":"p"}}})"},
             {"Scan", R"({"TableName":"none"})"},
             {"GetItem", R"({"TableName":"none","Key":{"pk":{"S":"p"}}})"},
             {"PutItem", R"({"TableName":"none","Item":{"pk":{"S":"p"}}})"},
             {"DeleteTable", R"({"TableName":"none"})"},
         }) {
        BOOST_TEST(call(op, body).type() == "ResourceNotFoundException", op);
    }
}

BOOST_FIXTURE_TEST_CASE(f14_exclusive_start_key_takes_scalar_keys_only, tables) {
    fails("Query",
          R"({"TableName":"tab","KeyConditionExpression":"pk = :p","ExpressionAttributeValues":)"
          R"({":p":{"S":"p"}},"ExclusiveStartKey":{"pk":{"S":"p"},"sk":{"NS":["1"]}}})",
          "ValidationException");
}

// ---------------------------------------------------------------------------
// G. Scan
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(g01_segments_partition_the_table_by_partition_key, tables) {
    for (int p = 0; p < 40; ++p) {
        for (int s = 0; s < 2; ++s) {
            put_t("p" + std::to_string(p), std::to_string(s));
        }
    }
    std::set<std::string> all;
    std::size_t total = 0;
    for (int seg = 0; seg < 4; ++seg) {
        const auto r = ok("Scan", R"({"TableName":"tab","Segment":)" + std::to_string(seg) +
                                      R"(,"TotalSegments":4})");
        std::set<std::string> pks;
        for (const auto& it : r.at("Items").as_array()) {
            const std::string pk(it.at("pk").at("S").as_string());
            pks.insert(pk);
            all.insert(pk + "/" + std::string(it.at("sk").at("N").as_string()));
            ++total;
        }
        // Both items of a partition land in the same segment.
        BOOST_TEST(r.at("Count").to_number<std::size_t>() == pks.size() * 2);
    }
    BOOST_TEST(total == 80U);
    BOOST_TEST(all.size() == 80U);
    fails("Scan", R"({"TableName":"tab","Segment":4,"TotalSegments":4})", "ValidationException");
    fails("Scan", R"({"TableName":"tab","Segment":0})", "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(g02_scan_returns_a_partition_in_sort_key_order, tables) {
    for (const char* sk : {"5", "-1", "3"}) {
        put_t("only", sk);
    }
    const std::vector<std::string> want{"-1", "3", "5"};
    BOOST_TEST(sort_keys(ok("Scan", R"({"TableName":"tab"})")) == want);
}

BOOST_FIXTURE_TEST_CASE(g03_scan_limit_and_filter_and_pages, tables) {
    for (int i = 0; i < 10; ++i) {
        put_t("p" + std::to_string(i), "1", R"(,"n":{"N":")" + std::to_string(i) + R"("})");
    }
    std::size_t count = 0;
    std::size_t scanned = 0;
    std::string start;
    for (;;) {
        const auto r =
            ok("Scan", R"({"TableName":"tab","Limit":3,"FilterExpression":"n < :five",)"
                       R"("ExpressionAttributeValues":{":five":{"N":"5"}})" +
                           (start.empty() ? "" : R"(,"ExclusiveStartKey":)" + start) + "}");
        BOOST_TEST(r.at("ScannedCount").to_number<std::size_t>() <= 3U);
        count += r.at("Count").to_number<std::size_t>();
        scanned += r.at("ScannedCount").to_number<std::size_t>();
        const auto* last = r.if_contains("LastEvaluatedKey");
        if (last == nullptr) {
            break;
        }
        start = json::serialize(*last);
    }
    BOOST_TEST(count == 5U);
    BOOST_TEST(scanned == 10U);
}

// ---------------------------------------------------------------------------
// H. Batch operations
// ---------------------------------------------------------------------------

namespace {

auto put_request(const std::string& pk, int sk) -> std::string {
    return R"({"PutRequest":{"Item":{"pk":{"S":")" + pk + R"("},"sk":{"N":")" + std::to_string(sk) +
           R"("}}}})";
}

auto batch_of(int n, const std::string& table = "tab") -> std::string {
    std::string list;
    for (int i = 0; i < n; ++i) {
        list += (i == 0 ? "" : ",") + put_request("b", i);
    }
    return R"({"RequestItems":{")" + table + R"(":[)" + list + "]}}";
}

}  // namespace

BOOST_FIXTURE_TEST_CASE(h01_batch_write_takes_at_most_25_requests, tables) {
    BOOST_TEST(ok("BatchWriteItem", batch_of(25)).at("UnprocessedItems").as_object().empty());
    fails("BatchWriteItem", batch_of(26), "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(h02_batch_write_refuses_one_item_twice, tables) {
    fails("BatchWriteItem",
          R"({"RequestItems":{"tab":[)" + put_request("x", 1) +
              R"(,{"DeleteRequest":{"Key":{"pk":{"S":"x"},"sk":{"N":"1"}}}}]}})",
          "ValidationException");
    fails("BatchWriteItem",
          R"({"RequestItems":{"tab":[)" + put_request("x", 1) + "," + put_request("x", 1) + "]}}",
          "ValidationException");
    BOOST_TEST(get_t("x", "1").is_null());
}

BOOST_FIXTURE_TEST_CASE(h03_batch_write_rejects_the_whole_batch_on_a_bad_request, tables) {
    // A missing table, a key that does not match the schema, an item over
    // 400 KB: nothing in the batch is written.
    fails("BatchWriteItem",
          R"({"RequestItems":{"tab":[)" + put_request("y", 1) + R"(],"none":[)" +
              put_request("y", 1) + "]}}",
          "ResourceNotFoundException");
    fails("BatchWriteItem",
          R"({"RequestItems":{"tab":[)" + put_request("y", 1) +
              R"(,{"PutRequest":{"Item":{"pk":{"S":"y"}}}}]}})",
          "ValidationException");
    fails("BatchWriteItem",
          R"({"RequestItems":{"tab":[)" + put_request("y", 1) +
              R"(,{"PutRequest":{"Item":{"pk":{"S":"y"},"sk":{"N":"2"},"v":{"S":")" +
              std::string(400 * 1024, 'x') + R"("}}}}]}})",
          "ValidationException");
    BOOST_TEST(get_t("y", "1").is_null());
}

BOOST_FIXTURE_TEST_CASE(h04_batch_write_puts_replace_items, tables) {
    put_t("b", "0", R"(,"old":{"S":"x"})");
    ok("BatchWriteItem", batch_of(1));
    BOOST_TEST(!get_t("b", "0").as_object().contains("old"));
}

BOOST_FIXTURE_TEST_CASE(h05_batch_get_takes_at_most_100_keys, tables) {
    const auto keys = [](int n) {
        std::string list;
        for (int i = 0; i < n; ++i) {
            list += std::string(i == 0 ? "" : ",") + R"({"pk":{"S":"k"},"sk":{"N":")" +
                    std::to_string(i) + R"("}})";
        }
        return R"({"RequestItems":{"tab":{"Keys":[)" + list + "]}}}";
    };
    ok("BatchGetItem", keys(100));
    fails("BatchGetItem", keys(101), "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(h06_batch_get_refuses_a_repeated_key, tables) {
    fails("BatchGetItem",
          R"({"RequestItems":{"tab":{"Keys":[{"pk":{"S":"k"},"sk":{"N":"1"}},)"
          R"({"pk":{"S":"k"},"sk":{"N":"1"}}]}}})",
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(h07_batch_get_omits_missing_items, tables) {
    put_t("k", "1");
    const auto r = ok("BatchGetItem", R"({"RequestItems":{"tab":{"ConsistentRead":true,"Keys":[)"
                                      R"({"pk":{"S":"k"},"sk":{"N":"1"}},)"
                                      R"({"pk":{"S":"k"},"sk":{"N":"2"}}]}}})");
    BOOST_TEST(r.at("Responses").at("tab").as_array().size() == 1U);
    BOOST_TEST(r.at("UnprocessedKeys").as_object().empty());
}

BOOST_FIXTURE_TEST_CASE(h08_batch_get_returns_at_most_16_mb_and_the_rest_unprocessed, tables) {
    const std::string big(390 * 1024, 'x');
    std::string keys;
    for (int i = 0; i < 50; ++i) {
        put_t("big", std::to_string(i), R"(,"v":{"S":")" + big + R"("})");
        keys += std::string(i == 0 ? "" : ",") + R"({"pk":{"S":"big"},"sk":{"N":")" +
                std::to_string(i) + R"("}})";
    }
    const auto r = ok("BatchGetItem", R"({"RequestItems":{"tab":{"Keys":[)" + keys + "]}}}");
    const auto got = r.at("Responses").at("tab").as_array().size();
    const auto left = r.at("UnprocessedKeys").at("tab").at("Keys").as_array().size();
    BOOST_TEST(got < 50U);
    // Each item is 390 KB of value plus its key: 42 fit in 16 MB, 43 do
    // not (DynamoDB Local answers 42 and 8 unprocessed).
    BOOST_TEST(got == 42U);
    BOOST_TEST(got + left == 50U);
}

// ---------------------------------------------------------------------------
// J. Tables
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(j01_delete_table_answers_deleting_and_removes_every_item, tables) {
    if (local_differs()) {
        return;  // DynamoDB Local deletes at once and answers ACTIVE
    }
    put_t("p", "1");
    const auto r = ok("DeleteTable", R"({"TableName":"tab"})");
    BOOST_TEST(r.at("TableDescription").at("TableStatus").as_string() == "DELETING");
    BOOST_TEST(call("DescribeTable", R"({"TableName":"tab"})").type() ==
               "ResourceNotFoundException");
    // The name is free again, and the new table is empty.
    create("tab");
    BOOST_TEST(get_t("p", "1").is_null());
    BOOST_TEST(ok("Scan", R"({"TableName":"tab"})").at("Count").to_number<int>() == 0);
}

BOOST_FIXTURE_TEST_CASE(j02_a_table_name_is_unique, tables) {
    BOOST_TEST(
        call("CreateTable",
             R"({"TableName":"hsh","AttributeDefinitions":[{"AttributeName":"pk",)"
             R"("AttributeType":"S"}],"KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
             R"("BillingMode":"PAY_PER_REQUEST"})")
            .type() == "ResourceInUseException");
}

BOOST_FIXTURE_TEST_CASE(j03_deletion_protection_blocks_delete_table, tables) {
    ok("UpdateTable", R"({"TableName":"hsh","DeletionProtectionEnabled":true})");
    BOOST_TEST(call("DeleteTable", R"({"TableName":"hsh"})").type() == "ValidationException");
    ok("UpdateTable", R"({"TableName":"hsh","DeletionProtectionEnabled":false})");
    BOOST_TEST(call("DeleteTable", R"({"TableName":"hsh"})").status == 200U);
}

// ---------------------------------------------------------------------------
// K. Local secondary indexes
// ---------------------------------------------------------------------------

namespace {

/// A table pk (S) HASH, sk (N) RANGE with `indexes`, defining each of
/// `keys` as an S attribute (every defined attribute must be some key).
auto lsi_table(const std::string& name, const std::string& indexes,
               const std::vector<std::string>& keys = {"x"}) -> std::string {
    std::string defs;
    for (const auto& k : keys) {
        defs += R"(,{"AttributeName":")" + k + R"(","AttributeType":"S"})";
    }
    return R"({"TableName":")" + name +
           R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
           R"({"AttributeName":"sk","AttributeType":"N"})" +
           defs +
           R"(],"KeySchema":[{"AttributeName":"pk",)"
           R"("KeyType":"HASH"},{"AttributeName":"sk","KeyType":"RANGE"}],)"
           R"("BillingMode":"PAY_PER_REQUEST","LocalSecondaryIndexes":)" +
           indexes + "}";
}

auto lsi(const std::string& name, const std::string& hash, const std::string& range,
         const std::string& projection) -> std::string {
    return R"({"IndexName":")" + name + R"(","KeySchema":[{"AttributeName":")" + hash +
           R"(","KeyType":"HASH"},{"AttributeName":")" + range +
           R"(","KeyType":"RANGE"}],"Projection":)" + projection + "}";
}

}  // namespace

BOOST_FIXTURE_TEST_CASE(k01_an_lsi_has_the_table_partition_key, tables) {
    fails("CreateTable",
          lsi_table("lsi", "[" + lsi("idx", "x", "q", R"({"ProjectionType":"ALL"})") + "]",
                    {"x", "q"}),
          "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(k02_a_table_has_at_most_5_lsis, tables) {
    std::string six;
    for (int i = 0; i < 6; ++i) {
        six += (i == 0 ? "" : ",") +
               lsi("idx" + std::to_string(i), "pk", "x", R"({"ProjectionType":"KEYS_ONLY"})");
    }
    fails("CreateTable", lsi_table("lsi", "[" + six + "]"), "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(k03_at_most_100_projected_attributes_per_table, tables) {
    // Five indexes of 20 NonKeyAttributes: 100, allowed. 101 is refused.
    const auto names = [](int from, int n) {
        std::string out;
        for (int i = 0; i < n; ++i) {
            out += std::string(i == 0 ? "" : ",") + "\"a" + std::to_string(from + i) + "\"";
        }
        return R"({"ProjectionType":"INCLUDE","NonKeyAttributes":[)" + out + "]}";
    };
    std::string hundred;
    for (int i = 0; i < 5; ++i) {
        hundred +=
            (i == 0 ? "" : ",") + lsi("idx" + std::to_string(i), "pk", "x", names(i * 20, 20));
    }
    ok("CreateTable", lsi_table("l100", "[" + hundred + "]"));
    std::string over;
    for (int i = 0; i < 5; ++i) {
        over += (i == 0 ? "" : ",") +
                lsi("idx" + std::to_string(i), "pk", "x", names(i * 20, i == 4 ? 21 : 20));
    }
    fails("CreateTable", lsi_table("l101", "[" + over + "]"), "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(k04_lsis_need_a_composite_table_key, tables) {
    fails(
        "CreateTable",
        R"({"TableName":"lsi","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"x","AttributeType":"S"}],"KeySchema":[{"AttributeName":"pk",)"
        R"("KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST","LocalSecondaryIndexes":[)" +
            lsi("idx", "pk", "x", R"({"ProjectionType":"ALL"})") + "]}",
        "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(k05_lsis_are_sparse_and_fetch_unprojected_attributes, tables) {
    ok("CreateTable",
       lsi_table("lsi", "[" + lsi("idx", "pk", "x", R"({"ProjectionType":"KEYS_ONLY"})") + "]"));
    ok("PutItem", R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"x":{"S":"b"},)"
                  R"("extra":{"S":"e"}}})");
    ok("PutItem", R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"2"}}})");
    const std::string q = R"({"TableName":"lsi","IndexName":"idx","ConsistentRead":true,)"
                          R"("KeyConditionExpression":"pk = :p","ExpressionAttributeValues":)"
                          R"({":p":{"S":"p"}})";
    // Only the item with the index key has an entry.
    const auto keys = ok("Query", q + "}");
    BOOST_TEST_REQUIRE(keys.at("Count").to_number<int>() == 1);
    BOOST_TEST(!keys.at("Items").as_array()[0].as_object().contains("extra"));
    // ALL_ATTRIBUTES on an LSI fetches the rest from the table.
    const auto all = ok("Query", q + R"(,"Select":"ALL_ATTRIBUTES"})");
    BOOST_TEST(all.at("Items").as_array()[0].at("extra").at("S").as_string() == "e");
}

BOOST_FIXTURE_TEST_CASE(k06_an_item_collection_with_lsis_is_bounded, tables) {
    // The 10 GB limit, lowered for the test: a write that would grow the
    // collection past it fails and changes nothing.
    if (local_differs()) {
        return;  // the limit cannot be lowered on the oracle
    }
    ok("CreateTable",
       lsi_table("lsi", "[" + lsi("idx", "pk", "x", R"({"ProjectionType":"ALL"})") + "]"));
    backend.set_collection_limit(4096);
    const std::string v(1500, 'v');
    ok("PutItem",
       R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"x":{"S":"a"},"v":{"S":")" + v +
           R"("}}})");
    const auto r = call("PutItem", R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"2"},)"
                                   R"("x":{"S":"b"},"v":{"S":")" +
                                       v + R"("}}})");
    BOOST_TEST(r.type() == "ItemCollectionSizeLimitExceededException");
    // Another collection is unaffected.
    ok("PutItem",
       R"({"TableName":"lsi","Item":{"pk":{"S":"other"},"sk":{"N":"1"},"x":{"S":"a"}}})");
}

BOOST_FIXTURE_TEST_CASE(k07_lsi_index_keys_cannot_be_empty_or_mistyped, tables) {
    ok("CreateTable",
       lsi_table("lsi", "[" + lsi("idx", "pk", "x", R"({"ProjectionType":"ALL"})") + "]"));
    fails("PutItem", R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"x":{"S":""}}})",
          "ValidationException");
    fails("PutItem", R"({"TableName":"lsi","Item":{"pk":{"S":"p"},"sk":{"N":"1"},"x":{"N":"1"}}})",
          "ValidationException");
}
