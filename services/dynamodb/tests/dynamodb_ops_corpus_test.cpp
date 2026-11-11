// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The operations corpus (`.kiro/specs/dynamodb-compatible-api/` task 8,
// Requirement 21.1): every scenario of `tests/data/ops_corpus.jsonl`,
// recorded against DynamoDB Local by `tools/ops-corpus/record.sh`, replayed
// through the front end and handlers on local_backend.
//
// A refusal must carry DynamoDB Local's type and message. A Query must
// return the same items in the same order, the same counts and the same
// LastEvaluatedKey. Scan order is an implementation detail (DynamoDB Local
// and Kythira hash differently), so a Scan page is compared by its counts
// and whether it has a cursor, and the ScanAll and ScanSegments steps, which
// page to the end, by the multiset of items. BatchGetItem responses are
// compared per table as multisets. A created or described table is
// compared by its LocalSecondaryIndexes and GlobalSecondaryIndexes, each as
// a multiset without the ARNs and counts (timestamps and ARNs differ,
// DynamoDB Local counts index entries at once, and it lists indexes in its
// own order). A step marked `divergence` is checked against its `kythira`
// answer when it has one, and otherwise only run.

#define BOOST_TEST_MODULE dynamodb_ops_corpus_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef KYTHIRA_OPS_CORPUS
#error "KYTHIRA_OPS_CORPUS must name the corpus file"
#endif

using namespace dynamodb_test;

namespace {

/// JSON with every object's members in name order, so attribute order
/// does not matter.
auto canonical(const json::value& v) -> std::string {
    if (v.is_object()) {
        std::vector<std::pair<std::string, std::string>> members;
        for (const auto& [k, x] : v.get_object()) {
            members.emplace_back(std::string(k), canonical(x));
        }
        std::sort(members.begin(), members.end());
        std::string out = "{";
        for (const auto& [k, x] : members) {
            out += (out.size() > 1 ? "," : "") + json::serialize(json::string(k)) + ":" + x;
        }
        return out + "}";
    }
    if (v.is_array()) {
        std::string out = "[";
        for (const auto& x : v.get_array()) {
            out += (out.size() > 1 ? "," : "") + canonical(x);
        }
        return out + "]";
    }
    return json::serialize(v);
}

auto multiset(const json::value& items) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& it : items.as_array()) {
        out.push_back(canonical(it));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A refusal's type and message, and a cancelled transaction's reasons.
auto error_of(const reply& r) -> json::object {
    json::object e{{"type", r.type()}, {"message", r.message()}};
    if (const auto* reasons = r.body.if_contains("CancellationReasons")) {
        e["reasons"] = *reasons;
    }
    return json::object{{"error", std::move(e)}};
}

/// Scan with `input` to the end.
auto scan_all(fixture& f, json::object input) -> json::object {
    json::array items;
    for (int page = 0; page < 10000; ++page) {
        const auto r = f.call("Scan", json::serialize(input));
        if (r.status != 200U) {
            return error_of(r);
        }
        for (const auto& it : r.body.at("Items").as_array()) {
            items.push_back(it);
        }
        const auto* lek = r.body.if_contains("LastEvaluatedKey");
        if (lek == nullptr) {
            return json::object{{"items", std::move(items)}};
        }
        input["ExclusiveStartKey"] = *lek;
    }
    throw std::runtime_error("scan_all: no end after 10000 pages");
}

/// Whether `got` matches `want` for a step of `op`, with the reason when
/// not.
auto compare(std::string_view op, const json::object& step, const json::object& want,
             const json::object& got) -> std::string {
    if (want.contains("error") || got.contains("error")) {
        return canonical(want) == canonical(got) ? "" : "different outcome";
    }
    if (want.contains("items")) {
        return multiset(want.at("items")) == multiset(got.at("items")) ? "" : "different items";
    }
    const auto& w = want.at("response").as_object();
    const auto& g = got.at("response").as_object();
    if (op == "CreateTable" || op == "DescribeTable") {
        const auto* member = op == "CreateTable" ? "TableDescription" : "Table";
        const auto indexes = [&](const json::object& r, const char* kind) {
            json::array out;
            if (const auto* l = r.at(member).as_object().if_contains(kind)) {
                for (const auto& x : l->as_array()) {
                    auto o = x.as_object();
                    o.erase("IndexArn");
                    // DynamoDB Local counts a GSI's entries at once; AWS
                    // (and Kythira) about every six hours.
                    o.erase("IndexSizeBytes");
                    o.erase("ItemCount");
                    out.push_back(std::move(o));
                }
            }
            return multiset(out);
        };
        if (indexes(w, "LocalSecondaryIndexes") != indexes(g, "LocalSecondaryIndexes")) {
            return "different LocalSecondaryIndexes";
        }
        return indexes(w, "GlobalSecondaryIndexes") == indexes(g, "GlobalSecondaryIndexes")
                   ? ""
                   : "different GlobalSecondaryIndexes";
    }
    if (op == "Scan") {
        const bool paged = step.at("input").as_object().contains("Limit");
        if (w.at("Count") != g.at("Count") || w.at("ScannedCount") != g.at("ScannedCount")) {
            return "different counts";
        }
        if (w.contains("LastEvaluatedKey") != g.contains("LastEvaluatedKey")) {
            return "cursor presence differs";
        }
        if (!paged && w.contains("Items") && multiset(w.at("Items")) != multiset(g.at("Items"))) {
            return "different items";
        }
        return "";
    }
    if (op == "BatchGetItem") {
        const auto& wr = w.at("Responses").as_object();
        const auto& gr = g.at("Responses").as_object();
        if (wr.size() != gr.size()) {
            return "different tables in Responses";
        }
        for (const auto& [table, items] : wr) {
            if (!gr.contains(table) || multiset(items) != multiset(gr.at(table))) {
                return "different items for " + std::string(table);
            }
        }
        return canonical(w.at("UnprocessedKeys")) == canonical(g.at("UnprocessedKeys"))
                   ? ""
                   : "different UnprocessedKeys";
    }
    return canonical(w) == canonical(g) ? "" : "different response";
}

}  // namespace

BOOST_AUTO_TEST_CASE(every_recorded_step_matches_dynamodb_local) {
    std::ifstream in(KYTHIRA_OPS_CORPUS);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot open " KYTHIRA_OPS_CORPUS);
    std::string line;
    std::size_t scenarios = 0;
    std::size_t steps = 0;
    std::size_t divergences = 0;
    std::size_t mismatches = 0;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto sc = json::parse(line).as_object();
        ++scenarios;
        fixture f;
        for (const auto& t : sc.at("tables").as_array()) {
            f.ok("CreateTable", json::serialize(t));
        }
        for (const auto& s : sc.at("setup").as_array()) {
            f.ok(std::string(s.at("op").as_string()), json::serialize(s.at("input")));
        }
        json::object previous;
        std::size_t index = 0;
        for (const auto& sv : sc.at("steps").as_array()) {
            const auto& st = sv.as_object();
            const std::string op(st.at("op").as_string());
            auto input = st.at("input").as_object();
            if (st.contains("continue")) {
                const auto* r = previous.if_contains("response");
                const auto* lek =
                    r != nullptr ? r->as_object().if_contains("LastEvaluatedKey") : nullptr;
                BOOST_REQUIRE_MESSAGE(lek != nullptr, std::string(sc.at("name").as_string()) +
                                                          " step " + std::to_string(index) +
                                                          ": no cursor to continue from");
                input["ExclusiveStartKey"] = *lek;
            }
            json::object got;
            if (op == "ScanAll") {
                got = scan_all(f, input);
            } else if (op == "ScanSegments") {
                json::array items;
                const auto n = st.at("segments").to_number<int>();
                for (int seg = 0; seg < n && !got.contains("error"); ++seg) {
                    auto part = input;
                    part["Segment"] = seg;
                    part["TotalSegments"] = n;
                    auto r = scan_all(f, part);
                    if (r.contains("error")) {
                        got = r;
                        break;
                    }
                    for (const auto& it : r.at("items").as_array()) {
                        items.push_back(it);
                    }
                }
                if (!got.contains("error")) {
                    got = json::object{{"items", std::move(items)}};
                }
            } else {
                auto r = f.call(op, json::serialize(input));
                if (r.status != 200U) {
                    got = error_of(r);
                } else {
                    r.body.erase("ConsumedCapacity");
                    got = json::object{{"response", r.body}};
                }
            }
            ++steps;
            const json::object* want = &st.at("expect").as_object();
            if (st.contains("divergence")) {
                ++divergences;
                if (!st.contains("kythira")) {
                    previous = got;
                    ++index;
                    continue;
                }
                want = &st.at("kythira").as_object();
            }
            if (const auto why = compare(op, st, *want, got); !why.empty()) {
                ++mismatches;
                std::cerr << sc.at("name").as_string() << " step " << index << " " << op << ": "
                          << why << "\n  input: " << json::serialize(input)
                          << "\n  local: " << json::serialize(*want)
                          << "\n  ours:  " << json::serialize(got) << "\n";
            }
            previous = got;
            ++index;
        }
    }
    BOOST_TEST_MESSAGE(scenarios << " scenarios, " << steps << " steps, " << divergences
                                 << " documented divergences");
    BOOST_TEST(scenarios >= 6U);
    BOOST_TEST(mismatches == 0U);
}
