// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The recorded expression corpus (`.kiro/specs/dynamodb-compatible-api/`
// task 5, Requirement 21.3): every case of
// `tests/data/expression_corpus.jsonl`, recorded against DynamoDB Local by
// `tools/expr-corpus/record.sh`, replayed through `compile_request`, the
// evaluator and the binary form. A rejection must carry DynamoDB Local's
// message exactly; an accepted request must leave the item, or return the
// items, DynamoDB Local did.

#define BOOST_TEST_MODULE dynamodb_expression_corpus_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/item_codec.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef KYTHIRA_EXPRESSION_CORPUS
#error "KYTHIRA_EXPRESSION_CORPUS must name the corpus file"
#endif

namespace ddb = kythira::dynamodb;

namespace {

auto keys() -> ddb::request_keys {
    ddb::request_keys k;
    k.table.partition_name = "pk";
    k.table.partition_type = ddb::attribute_type::S;
    k.table.sort_name = "sk";
    k.table.sort_type = ddb::attribute_type::N;
    return k;
}

auto parse_item(const boost::json::value& v) -> std::optional<ddb::item> {
    if (!v.is_object()) {
        return std::nullopt;
    }
    auto it = ddb::item_from_json(v.get_object());
    BOOST_REQUIRE_MESSAGE(it.has_value(), "corpus item does not parse");
    return *it;
}

auto items_equal(const ddb::item& a, const ddb::item& b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].first != b[i].first || !ddb::eval_detail::values_equal(a[i].second, b[i].second)) {
            return false;
        }
    }
    return true;
}

// What Kythira answers for a case, in the recorder's `expect` shape.
auto simulate(const boost::json::object& c) -> boost::json::object {
    const std::string op(c.at("op").as_string());
    const auto& input = c.at("input").as_object();
    boost::json::object key;
    key["pk"] = boost::json::object{{"S", "k"}};
    key["sk"] = boost::json::object{{"N", "1"}};
    std::optional<ddb::item> stored;
    if (c.at("item").is_object()) {
        boost::json::object full = key;
        for (const auto& kv : c.at("item").as_object()) {
            full[kv.key()] = kv.value();
        }
        stored = parse_item(full);
    }

    boost::json::object out;
    const auto fail = [&](const kythira::aws_json::error& e) {
        boost::json::object err;
        const auto hash = e.type.rfind('#');
        err["type"] = hash == std::string::npos ? e.type : e.type.substr(hash + 1);
        err["message"] = e.message.value_or("");
        out["error"] = std::move(err);
        return out;
    };
    auto compiled = ddb::compile_request(op, input, keys());
    if (!compiled) {
        return fail(compiled.error());
    }
    // Every tree survives the trip through the Raft entry's binary form.
    ddb::expression_set decoded;
    BOOST_REQUIRE(ddb::decode_expressions(ddb::encode_expressions(*compiled), decoded) ==
                  ddb::expr_decode_status::ok);
    BOOST_REQUIRE(decoded == *compiled);
    const auto& e = decoded;
    const ddb::item empty;
    const ddb::item& current = stored ? *stored : empty;

    if (op == "DeleteItem" || op == "PutItem") {
        if (e.condition && !ddb::evaluate_condition(*e.condition, current)) {
            out["condition_failed"] = true;
            return out;
        }
        out["item"] = nullptr;
        return out;
    }
    if (op == "UpdateItem") {
        if (e.condition && !ddb::evaluate_condition(*e.condition, current)) {
            out["condition_failed"] = true;
            return out;
        }
        ddb::item it = stored ? *stored : *parse_item(key);
        if (e.update) {
            if (auto err = ddb::apply_update(*e.update, it)) {
                return fail(*err);
            }
        }
        out["item"] = ddb::item_to_json(it);
        return out;
    }
    if (op == "GetItem") {
        if (!stored) {
            out["item"] = nullptr;
            return out;
        }
        out["item"] =
            ddb::item_to_json(e.projection ? ddb::project_item(*e.projection, *stored) : *stored);
        return out;
    }
    // Query and Scan over the one item the table holds.
    boost::json::array items;
    if (stored && (!e.key || ddb::matches_key(*e.key, *stored)) &&
        (!e.filter || ddb::evaluate_condition(*e.filter, *stored))) {
        items.push_back(
            ddb::item_to_json(e.projection ? ddb::project_item(*e.projection, *stored) : *stored));
    }
    out["items"] = std::move(items);
    return out;
}

// With three or more paths, DynamoDB Local's overlap message sometimes
// names an unrelated path as "path two" (see `check_overlap_with_later`):
// only the kind of clash, and the expression it is in, must match.
// The set of types an `attribute_type` operand may be is printed from a Java
// hash set keyed by identity, so its order changes from run to run.
auto type_set(std::string_view m) -> std::optional<std::pair<std::string, std::set<std::string>>> {
    const auto open = m.find("operand type: {");
    if (open == std::string_view::npos || m.back() != '}') {
        return std::nullopt;
    }
    std::set<std::string> types;
    std::string_view rest = m.substr(open + 15, m.size() - open - 16);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        types.emplace(rest.substr(0, comma));
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return std::pair{std::string(m.substr(0, open)), std::move(types)};
}

auto same_message(std::string_view want, std::string_view got) -> bool {
    if (want == got) {
        return true;
    }
    if (const auto w = type_set(want)) {
        return w == type_set(got);
    }
    constexpr std::string_view k_clash = "Two document paths ";
    const auto w = want.find(k_clash);
    const auto g = got.find(k_clash);
    if (w == std::string_view::npos || g == std::string_view::npos) {
        return false;
    }
    const auto w_end = want.find("; path one:", w);
    const auto g_end = got.find("; path one:", g);
    return want.substr(0, w_end) == got.substr(0, g_end);
}

auto same(const boost::json::object& want, const boost::json::object& got) -> bool {
    if (want.contains("error") || got.contains("error")) {
        return want.contains("error") && got.contains("error") &&
               want.at("error").as_object().at("type") == got.at("error").as_object().at("type") &&
               same_message(want.at("error").as_object().at("message").as_string(),
                            got.at("error").as_object().at("message").as_string());
    }
    if (want.contains("condition_failed") || got.contains("condition_failed")) {
        return want.contains("condition_failed") && got.contains("condition_failed");
    }
    if (want.contains("items")) {
        if (!got.contains("items")) {
            return false;
        }
        const auto& a = want.at("items").as_array();
        const auto& b = got.at("items").as_array();
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!items_equal(*parse_item(a[i]), *parse_item(b[i]))) {
                return false;
            }
        }
        return true;
    }
    const auto& a = want.at("item");
    const auto& b = got.at("item");
    if (a.is_null() || b.is_null()) {
        return a.is_null() && b.is_null();
    }
    return items_equal(*parse_item(a), *parse_item(b));
}

}  // namespace

BOOST_AUTO_TEST_CASE(every_recorded_case_matches_dynamodb_local) {
    std::ifstream in(KYTHIRA_EXPRESSION_CORPUS);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " KYTHIRA_EXPRESSION_CORPUS);
    boost::json::parse_options opts;
    opts.max_depth = 128;
    std::string line;
    std::size_t cases = 0;
    std::size_t skipped = 0;
    std::size_t mismatches = 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto c = boost::json::parse(line, {}, opts).as_object();
        ++cases;
        const auto got = simulate(c);
        if (c.contains("divergence")) {
            // DynamoDB Local differs from AWS's documented behaviour here;
            // Kythira follows AWS, and the case must still run cleanly.
            ++skipped;
            continue;
        }
        const auto& want = c.at("expect").as_object();
        if (!same(want, got)) {
            ++mismatches;
            if (mismatches <= 400) {
                std::cerr << "case " << c.at("id") << " " << c.at("op").as_string() << " "
                          << boost::json::serialize(c.at("input"))
                          << "\n  item:  " << boost::json::serialize(c.at("item"))
                          << "\n  local: " << boost::json::serialize(want)
                          << "\n  ours:  " << boost::json::serialize(got) << "\n";
            }
        }
    }
    BOOST_TEST_MESSAGE(cases << " cases, " << skipped << " documented divergences");
    BOOST_TEST(cases > 1000U);
    BOOST_TEST(mismatches == 0U);
}
