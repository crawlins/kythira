// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Fuzzing for the expression language (`.kiro/specs/dynamodb-compatible-api/`
// task 5): seeded mutations of every expression in the recorded corpus go
// through `compile_request`, and whatever compiles must survive the binary
// form's round trip and evaluate without failing; mutated binary forms must
// decode to a status, never crash. The seed is fixed, so a failure
// reproduces.

#define BOOST_TEST_MODULE dynamodb_expression_fuzz_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/expr.hpp>
#include <kythira/dynamodb/expr_eval.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/item_codec.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#ifndef KYTHIRA_EXPRESSION_CORPUS
#error "KYTHIRA_EXPRESSION_CORPUS must name the corpus file"
#endif

namespace ddb = kythira::dynamodb;

namespace {

constexpr std::uint32_t k_seed = 20261110;
constexpr int k_mutations_per_expression = 12;

constexpr std::string_view k_expression_members[] = {"ConditionExpression", "UpdateExpression",
                                                     "FilterExpression", "ProjectionExpression",
                                                     "KeyConditionExpression"};

// Characters the grammar gives meaning to, plus some it does not.
constexpr std::string_view k_alphabet = "abz09#:._[](),=<>+- \t\"'\\\x01\xc3\xa9";

constexpr std::string_view k_words[] = {"AND",
                                        "OR",
                                        "NOT",
                                        "BETWEEN",
                                        "IN",
                                        "SET",
                                        "REMOVE",
                                        "ADD",
                                        "DELETE",
                                        "size",
                                        "if_not_exists",
                                        "list_append",
                                        "attribute_exists",
                                        "begins_with",
                                        "contains",
                                        "attribute_type",
                                        "name",
                                        ":v",
                                        "#n",
                                        "[2147483648]",
                                        "((",
                                        "))",
                                        ".",
                                        ","};

auto keys() -> ddb::request_keys {
    ddb::request_keys k;
    k.table.partition_name = "pk";
    k.table.partition_type = ddb::attribute_type::S;
    k.table.sort_name = "sk";
    k.table.sort_type = ddb::attribute_type::N;
    return k;
}

auto load_corpus() -> std::vector<boost::json::object> {
    std::ifstream in(KYTHIRA_EXPRESSION_CORPUS);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " KYTHIRA_EXPRESSION_CORPUS);
    boost::json::parse_options opts;
    opts.max_depth = 128;
    std::vector<boost::json::object> out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            out.push_back(boost::json::parse(line, {}, opts).as_object());
        }
    }
    return out;
}

auto mutate(std::string text, std::mt19937& rng) -> std::string {
    const auto pick = [&](std::size_t n) {
        return n == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    };
    const int edits = 1 + static_cast<int>(pick(3));
    for (int e = 0; e < edits; ++e) {
        const auto at = pick(text.size() + 1);
        switch (pick(6)) {
            case 0:
                if (!text.empty()) {
                    text.erase(pick(text.size()), 1);
                }
                break;
            case 1:
                text.insert(at, 1, k_alphabet[pick(k_alphabet.size())]);
                break;
            case 2:
                text.insert(at, std::string(k_words[pick(std::size(k_words))]) + " ");
                break;
            case 3:
                if (!text.empty()) {
                    const auto from = pick(text.size());
                    const auto len = 1 + pick(text.size() - from);
                    text.insert(at, text.substr(from, len));
                }
                break;
            case 4:
                text.resize(at);
                break;
            default:
                if (text.size() > 1) {
                    std::swap(text[pick(text.size())], text[pick(text.size())]);
                }
                break;
        }
    }
    return text;
}

// Runs one request to the end: compile, round trip, evaluate against the
// case's item.
void exercise(const std::string& op, const boost::json::object& input,
              const boost::json::value& stored) {
    auto compiled = ddb::compile_request(op, input, keys());
    if (!compiled) {
        BOOST_TEST(!compiled.error().type.empty());
        return;
    }
    ddb::expression_set decoded;
    BOOST_REQUIRE(ddb::decode_expressions(ddb::encode_expressions(*compiled), decoded) ==
                  ddb::expr_decode_status::ok);
    BOOST_REQUIRE(decoded == *compiled);
    ddb::item it;
    if (stored.is_object()) {
        auto parsed = ddb::item_from_json(stored.get_object());
        BOOST_REQUIRE(parsed.has_value());
        it = *parsed;
    }
    if (decoded.condition) {
        (void)ddb::evaluate_condition(*decoded.condition, it);
    }
    if (decoded.filter) {
        (void)ddb::evaluate_condition(*decoded.filter, it);
    }
    if (decoded.key) {
        (void)ddb::matches_key(*decoded.key, it);
    }
    if (decoded.projection) {
        (void)ddb::project_item(*decoded.projection, it);
    }
    if (decoded.update) {
        auto copy = it;
        (void)ddb::apply_update(*decoded.update, copy);
    }
}

}  // namespace

BOOST_AUTO_TEST_CASE(mutated_expressions_compile_or_fail_cleanly) {
    const auto corpus = load_corpus();
    std::mt19937 rng(k_seed);
    std::size_t runs = 0;
    std::size_t compiled = 0;
    for (const auto& c : corpus) {
        const std::string op(c.at("op").as_string());
        const auto& input = c.at("input").as_object();
        for (const auto member : k_expression_members) {
            const auto* text = input.if_contains(member);
            if (text == nullptr || !text->is_string()) {
                continue;
            }
            for (int m = 0; m < k_mutations_per_expression; ++m) {
                auto mutated = input;
                mutated[member] = mutate(std::string(text->get_string()), rng);
                exercise(op, mutated, c.at("item"));
                ++runs;
                compiled += ddb::compile_request(op, mutated, keys()).has_value() ? 1U : 0U;
            }
        }
    }
    BOOST_TEST_MESSAGE(runs << " mutated requests, " << compiled << " compiled");
    BOOST_TEST(runs > 10000U);
    // Some mutations must still be valid, or the evaluation half never ran.
    BOOST_TEST(compiled > 100U);
}

BOOST_AUTO_TEST_CASE(mutated_binary_forms_decode_to_a_status) {
    const auto corpus = load_corpus();
    std::mt19937 rng(k_seed);
    std::size_t forms = 0;
    for (const auto& c : corpus) {
        auto compiled = ddb::compile_request(std::string(c.at("op").as_string()),
                                             c.at("input").as_object(), keys());
        if (!compiled) {
            continue;
        }
        const auto bytes = ddb::encode_expressions(*compiled);
        ++forms;
        // Every truncation is malformed.
        for (std::size_t n = 0; n < bytes.size(); ++n) {
            ddb::expression_set out;
            BOOST_TEST((ddb::decode_expressions(std::string_view(bytes).substr(0, n), out) !=
                        ddb::expr_decode_status::ok));
        }
        // Trailing bytes are malformed.
        {
            ddb::expression_set out;
            BOOST_TEST(
                (ddb::decode_expressions(bytes + '\0', out) == ddb::expr_decode_status::malformed));
        }
        // Flipped bytes decode to some status; a decoded result re-encodes.
        for (int f = 0; f < 16; ++f) {
            auto flipped = bytes;
            const auto at = std::uniform_int_distribution<std::size_t>(1, bytes.size() - 1)(rng);
            flipped[at] = static_cast<char>(std::uniform_int_distribution<int>(0, 255)(rng));
            ddb::expression_set out;
            if (ddb::decode_expressions(flipped, out) == ddb::expr_decode_status::ok) {
                ddb::expression_set again;
                BOOST_TEST((ddb::decode_expressions(ddb::encode_expressions(out), again) ==
                            ddb::expr_decode_status::ok));
            }
        }
    }
    BOOST_TEST(forms > 500U);
}

BOOST_AUTO_TEST_CASE(a_newer_format_version_stops_rather_than_guesses) {
    auto compiled =
        ddb::compile_request("DeleteItem",
                             boost::json::parse(R"({"ConditionExpression":"a = :v",)"
                                                R"("ExpressionAttributeValues":{":v":{"N":"1"}}})")
                                 .as_object(),
                             keys());
    BOOST_REQUIRE(compiled.has_value());
    auto bytes = ddb::encode_expressions(*compiled);
    BOOST_REQUIRE(static_cast<std::uint8_t>(bytes[0]) == ddb::k_expr_format_version);
    bytes[0] = static_cast<char>(ddb::k_expr_format_version + 1);
    ddb::expression_set out;
    BOOST_TEST((ddb::decode_expressions(bytes, out) == ddb::expr_decode_status::unknown_version));
    bytes[0] = 0;
    BOOST_TEST((ddb::decode_expressions(bytes, out) == ddb::expr_decode_status::malformed));
}
