// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// History and equality indexes (.kiro/specs/verifiable-ledger/ task 6,
// Requirements 6.1-6.3): history in commit order with time bounds and
// pagination, index creation (active at once on an empty table, built by
// ledger_index_build entries otherwise), index maintenance on every write,
// index lookups at a snapshot and their read-set conflicts, and filtered
// scans with a byte budget.

#define BOOST_TEST_MODULE ledger_history_index_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/memory_kv_engine.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace kythira::ledger;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;
using sessions = ledger_session_manager<ledger_sm>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;

auto person(std::string_view name, std::string_view city) -> ion_value {
    ion_value v = ion_value::make_struct();
    v.set("name", ion_value::string(name)).set("city", ion_value::string(city));
    return v;
}

/// A valid document id (22 base62 characters) that sorts like `s`.
auto I(std::string s) -> std::string {
    s.resize(id_length, '0');
    return s;
}

auto city_path() -> document_path {
    return {std::string("city")};
}

auto ids_of(const scan_page& p) -> std::set<std::string> {
    std::set<std::string> out;
    for (const auto& r : p.documents) {
        out.insert(r.id);
    }
    return out;
}

struct fixture {
    memory_kv_engine engine;
    ledger_sm sm;
    std::uint64_t index = 0;
    std::int64_t clock = t0;
    std::chrono::steady_clock::time_point now{};
    sessions mgr;

    explicit fixture(ledger_options options = {})
        : sm(options), mgr(sm, {}, [this] { return now; }) {
        sm.open(engine, ledger_range("people"));
    }

    auto apply(const prepared_transaction& p) -> apply_result {
        if (!p.entry) {
            return p.rejection;
        }
        return apply_result::decode(sm.apply(p.command, ++index));
    }

    auto run(const std::vector<ion_value>& statements) -> apply_result {
        clock += 10;
        return apply(mgr.execute(1, statements, clock));
    }

    auto commit(const std::string& token) -> apply_result {
        clock += 10;
        return apply(mgr.commit(token, clock));
    }

    /// Apply one ledger_index_build step; its result Ion.
    auto build_step(std::string_view table, std::string_view idx, std::size_t limit) -> ion_value {
        const auto bytes = encode_index_build({std::string(table), std::string(idx), limit});
        std::vector<std::byte> command(bytes.size());
        std::memcpy(command.data(), bytes.data(), bytes.size());
        const auto out = sm.apply(command, ++index);
        return ion_value::parse(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(out.data()), out.size()));
    }
};

auto status_of(const ion_value& v) -> std::string {
    const ion_value* s = v.find("status");
    return s != nullptr ? s->as_text().value_or("") : "";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(history)

BOOST_AUTO_TEST_CASE(a_documents_history_is_every_revision_in_order_deletes_included) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p")}).committed);
    BOOST_REQUIRE(f.run({statements::insert("p", I("a"), person("Ann", "Oslo"))}).committed);
    BOOST_REQUIRE(f.run({statements::replace("p", I("a"), person("Ann", "Rome"))}).committed);
    BOOST_REQUIRE(f.run({statements::remove("p", I("a"))}).committed);

    const auto h = f.sm.history("p", I("a"), std::nullopt, std::nullopt);
    BOOST_REQUIRE_EQUAL(h.revisions.size(), 3U);
    BOOST_TEST(!h.next);
    for (std::size_t i = 0; i < 3; ++i) {
        BOOST_TEST(h.revisions[i].version == static_cast<std::int64_t>(i));
    }
    BOOST_TEST(h.revisions[0].data->find("city")->as_text().value_or("") == "Oslo");
    BOOST_TEST(h.revisions[1].data->find("city")->as_text().value_or("") == "Rome");
    BOOST_TEST(!h.revisions[2].data);  // the delete
}

BOOST_AUTO_TEST_CASE(time_bounds_are_inclusive_and_pages_resume) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p")}).committed);
    BOOST_REQUIRE(f.run({statements::insert("p", I("a"), person("Ann", "0"))}).committed);
    for (int i = 1; i < 6; ++i) {
        BOOST_REQUIRE(
            f.run({statements::replace("p", I("a"), person("Ann", std::to_string(i)))}).committed);
    }
    // Versions 0..5; find their times.
    const auto all = f.sm.history("p", I("a"), std::nullopt, std::nullopt);
    BOOST_REQUIRE_EQUAL(all.revisions.size(), 6U);
    const auto t1 = all.revisions[1].tx_time;
    const auto t4 = all.revisions[4].tx_time;

    const auto ranged = f.sm.history("p", I("a"), t1, t4);
    BOOST_REQUIRE_EQUAL(ranged.revisions.size(), 4U);
    BOOST_TEST(ranged.revisions.front().version == 1);
    BOOST_TEST(ranged.revisions.back().version == 4);

    std::vector<std::int64_t> versions;
    std::optional<std::string> cursor;
    do {
        const auto page = f.sm.history("p", I("a"), std::nullopt, std::nullopt, cursor, 4);
        BOOST_TEST(page.revisions.size() <= 4U);
        for (const auto& r : page.revisions) {
            versions.push_back(r.version);
        }
        cursor = page.next;
    } while (cursor);
    BOOST_TEST(versions == (std::vector<std::int64_t>{0, 1, 2, 3, 4, 5}));
}

BOOST_AUTO_TEST_CASE(a_tables_history_is_in_commit_order_with_time_bounds_and_pages) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p"), statements::create_table("q")}).committed);
    BOOST_REQUIRE(f.run({statements::insert("p", I("b"), person("Bo", "Oslo")),
                         statements::insert("p", I("a"), person("Al", "Oslo"))})
                      .committed);
    BOOST_REQUIRE(f.run({statements::insert("q", I("x"), person("Xi", "Rome"))}).committed);
    BOOST_REQUIRE(f.run({statements::replace("p", I("b"), person("Bo", "Rome"))}).committed);
    BOOST_REQUIRE(f.run({statements::remove("p", I("a"))}).committed);

    const auto h = f.sm.history("p", std::nullopt, std::nullopt, std::nullopt);
    BOOST_REQUIRE_EQUAL(h.revisions.size(), 4U);
    // Block 1 (a and b, by id), then block 3 (b v1), then block 4 (a deleted).
    BOOST_TEST(h.revisions[0].id == I("a"));
    BOOST_TEST(h.revisions[1].id == I("b"));
    BOOST_TEST(h.revisions[2].id == I("b"));
    BOOST_TEST(h.revisions[2].version == 1);
    BOOST_TEST(h.revisions[3].id == I("a"));
    BOOST_TEST(!h.revisions[3].data);
    for (std::size_t i = 1; i < h.revisions.size(); ++i) {
        BOOST_TEST(h.revisions[i - 1].seq <= h.revisions[i].seq);
    }

    // Only block 3's time: one revision.
    const auto t3 = h.revisions[2].tx_time;
    const auto only = f.sm.history("p", std::nullopt, t3, t3);
    BOOST_REQUIRE_EQUAL(only.revisions.size(), 1U);
    BOOST_TEST(only.revisions[0].seq == 3U);

    // From block 3 on, with no upper bound.
    BOOST_TEST(f.sm.history("p", std::nullopt, t3, std::nullopt).revisions.size() == 2U);
    // Before the first block, and after the last.
    BOOST_TEST(f.sm.history("p", std::nullopt, std::nullopt, t0).revisions.empty());
    BOOST_TEST(f.sm.history("p", std::nullopt, f.clock + 1, std::nullopt).revisions.empty());

    std::size_t seen = 0;
    std::optional<std::string> cursor;
    do {
        const auto page = f.sm.history("p", std::nullopt, std::nullopt, std::nullopt, cursor, 1);
        BOOST_TEST(page.revisions.size() <= 1U);
        seen += page.revisions.size();
        cursor = page.next;
    } while (cursor);
    BOOST_TEST(seen == 4U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(indexes)

BOOST_AUTO_TEST_CASE(an_index_on_an_empty_table_is_active_at_once_and_followed) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p"),
                         statements::create_index("p", "by_city", city_path())})
                      .committed);
    const auto info = f.sm.get_index("p", "by_city");
    BOOST_REQUIRE(info);
    BOOST_TEST(info->active);
    BOOST_TEST(f.sm.indexes("p").size() == 1U);

    BOOST_REQUIRE(f.run({statements::insert("p", I("a"), person("Al", "Oslo")),
                         statements::insert("p", I("b"), person("Bo", "Oslo")),
                         statements::insert("p", I("c"), person("Cy", "Rome"))})
                      .committed);
    const auto lookup = [&](std::string_view city) {
        return ids_of(
            f.sm.find_by_index("p", "by_city", ion_value::string(city), f.sm.block_count()));
    };
    BOOST_TEST(lookup("Oslo") == (std::set<std::string>{I("a"), I("b")}));
    BOOST_TEST(lookup("Rome") == (std::set<std::string>{I("c")}));

    // replace moves a document between keys
    BOOST_REQUIRE(f.run({statements::replace("p", I("a"), person("Al", "Rome"))}).committed);
    BOOST_TEST(lookup("Oslo") == (std::set<std::string>{I("b")}));
    BOOST_TEST(lookup("Rome") == (std::set<std::string>{I("a"), I("c")}));

    // patch removing the field takes it out of every key
    BOOST_REQUIRE(f.run({statements::patch("p", I("b"), {}, {city_path()})}).committed);
    BOOST_TEST(lookup("Oslo").empty());

    // patch setting it puts it back
    BOOST_REQUIRE(
        f.run({statements::patch("p", I("b"), {{city_path(), ion_value::string("Lima")}}, {})})
            .committed);
    BOOST_TEST(lookup("Lima") == (std::set<std::string>{I("b")}));

    // delete
    BOOST_REQUIRE(f.run({statements::remove("p", I("c"))}).committed);
    BOOST_TEST(lookup("Rome") == (std::set<std::string>{I("a")}));
}

BOOST_AUTO_TEST_CASE(lookups_read_the_snapshot_and_page) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p"),
                         statements::create_index("p", "by_city", city_path())})
                      .committed);
    std::vector<ion_value> st;
    for (int i = 0; i < 5; ++i) {
        st.push_back(statements::insert("p", I("d" + std::to_string(i)), person("x", "Oslo")));
    }
    BOOST_REQUIRE(f.run(st).committed);
    const auto snap = f.sm.block_count();
    BOOST_REQUIRE(f.run({statements::replace("p", I("d0"), person("x", "Rome")),
                         statements::insert("p", I("d9"), person("x", "Oslo"))})
                      .committed);

    // At the old snapshot Oslo still holds d0..d4 (the fallback scan path).
    const auto old = f.sm.find_by_index("p", "by_city", ion_value::string("Oslo"), snap);
    BOOST_TEST(ids_of(old) == (std::set<std::string>{I("d0"), I("d1"), I("d2"), I("d3"), I("d4")}));
    // Now: d1..d4, d9.
    const auto cur =
        f.sm.find_by_index("p", "by_city", ion_value::string("Oslo"), f.sm.block_count());
    BOOST_TEST(ids_of(cur) == (std::set<std::string>{I("d1"), I("d2"), I("d3"), I("d4"), I("d9")}));

    std::set<std::string> paged;
    std::optional<std::string> cursor;
    do {
        const auto page = f.sm.find_by_index("p", "by_city", ion_value::string("Oslo"),
                                             f.sm.block_count(), cursor, 2);
        BOOST_TEST(page.documents.size() <= 2U);
        for (const auto& r : page.documents) {
            BOOST_TEST(paged.insert(r.id).second);
        }
        cursor = page.next;
    } while (cursor);
    BOOST_TEST(paged == ids_of(cur));
}

BOOST_AUTO_TEST_CASE(an_index_on_a_full_table_builds_before_queries_use_it) {
    fixture f;
    std::vector<ion_value> st{statements::create_table("p")};
    for (int i = 0; i < 7; ++i) {
        st.push_back(statements::insert("p", I("d" + std::to_string(i)),
                                        person("x", i % 2 == 0 ? "Oslo" : "Rome")));
    }
    BOOST_REQUIRE(f.run(st).committed);
    BOOST_REQUIRE(f.run({statements::create_index("p", "by_city", city_path())}).committed);
    BOOST_TEST(!f.sm.get_index("p", "by_city")->active);
    BOOST_CHECK_THROW(
        (void)f.sm.find_by_index("p", "by_city", ion_value::string("Oslo"), f.sm.block_count()),
        ledger_query_error);

    // A write while building: d1 moves to Oslo, d6 is deleted.
    BOOST_REQUIRE(f.run({statements::replace("p", I("d1"), person("x", "Oslo"))}).committed);
    auto step = f.build_step("p", "by_city", 3);
    BOOST_TEST(status_of(step) == "index_building");
    BOOST_REQUIRE(f.run({statements::remove("p", I("d6")),
                         statements::replace("p", I("d5"), person("x", "Oslo"))})
                      .committed);
    BOOST_TEST(status_of(f.build_step("p", "by_city", 3)) == "index_building");
    BOOST_TEST(status_of(f.build_step("p", "by_city", 3)) == "index_active");
    BOOST_TEST(f.sm.get_index("p", "by_city")->active);

    const auto oslo =
        f.sm.find_by_index("p", "by_city", ion_value::string("Oslo"), f.sm.block_count());
    BOOST_TEST(ids_of(oslo) ==
               (std::set<std::string>{I("d0"), I("d1"), I("d2"), I("d4"), I("d5")}));
    const auto rome =
        f.sm.find_by_index("p", "by_city", ion_value::string("Rome"), f.sm.block_count());
    BOOST_TEST(ids_of(rome) == (std::set<std::string>{I("d3")}));

    // Another build step is refused.
    const auto again = apply_result::decode([&] {
        const auto bytes = encode_index_build({"p", "by_city", 3});
        std::vector<std::byte> command(bytes.size());
        std::memcpy(command.data(), bytes.data(), bytes.size());
        return f.sm.apply(command, ++f.index);
    }());
    BOOST_TEST(!again.committed);
    BOOST_TEST(again.code == "index_active");
}

BOOST_AUTO_TEST_CASE(index_names_and_counts_are_checked) {
    ledger_options o;
    o.max_indexes_per_table = 2;
    fixture f(o);
    BOOST_REQUIRE(
        f.run({statements::create_table("p"), statements::create_index("p", "i1", city_path())})
            .committed);
    auto r = f.run({statements::create_index("p", "i1", {std::string("name")})});
    BOOST_TEST(r.code == "index_exists");
    BOOST_REQUIRE(f.run({statements::create_index("p", "i2", {std::string("name")})}).committed);
    r = f.run({statements::create_index("p", "i3", {std::string("x")})});
    BOOST_TEST(r.code == "index_limit");
    r = f.run({statements::create_index("nope", "i1", city_path())});
    BOOST_TEST(r.code == "table_not_found");
    BOOST_CHECK_THROW(
        (void)f.sm.find_by_index("p", "zzz", ion_value::string("x"), f.sm.block_count()),
        ledger_query_error);
}

BOOST_AUTO_TEST_CASE(a_session_lookup_conflicts_when_its_key_changes) {
    fixture f;
    BOOST_REQUIRE(
        f.run({statements::create_table("p"), statements::create_index("p", "by_city", city_path()),
               statements::insert("p", I("a"), person("Al", "Oslo")),
               statements::insert("p", I("z"), person("Zed", "Lima"))})
            .committed);

    // A change to another key does not conflict.
    auto tok = f.mgr.begin(1);
    BOOST_TEST(ids_of(f.mgr.lookup(tok, "p", "by_city", ion_value::string("Oslo"))) ==
               (std::set<std::string>{I("a")}));
    BOOST_REQUIRE(f.run({statements::replace("p", I("z"), person("Zed", "Rome"))}).committed);
    f.mgr.add(tok, statements::insert("p", I("n1"), person("N", "Kyiv")));
    BOOST_TEST(f.commit(tok).committed);

    // A document moving into the key (a phantom) conflicts.
    tok = f.mgr.begin(1);
    (void)f.mgr.lookup(tok, "p", "by_city", ion_value::string("Oslo"));
    BOOST_REQUIRE(f.run({statements::insert("p", I("b"), person("Bo", "Oslo"))}).committed);
    f.mgr.add(tok, statements::insert("p", I("n2"), person("N", "Kyiv")));
    auto r = f.commit(tok);
    BOOST_TEST(!r.committed);
    BOOST_TEST(r.code == "occ_conflict");

    // A document moving out of it conflicts too.
    tok = f.mgr.begin(1);
    (void)f.mgr.lookup(tok, "p", "by_city", ion_value::string("Oslo"));
    BOOST_REQUIRE(f.run({statements::replace("p", I("a"), person("Al", "Rome"))}).committed);
    f.mgr.add(tok, statements::insert("p", I("n3"), person("N", "Kyiv")));
    r = f.commit(tok);
    BOOST_TEST(r.code == "occ_conflict");
}

BOOST_AUTO_TEST_CASE(a_lookup_through_an_index_newer_than_the_snapshot_reads_the_snapshot) {
    fixture f;
    BOOST_REQUIRE(f.run({statements::create_table("p"),
                         statements::insert("p", I("a"), person("Al", "Oslo"))})
                      .committed);
    const auto tok = f.mgr.begin(1);
    // a is deleted, so the table is empty when the index is created and
    // the index never sees a; the session's snapshot still holds it.
    BOOST_REQUIRE(f.run({statements::remove("p", I("a"))}).committed);
    BOOST_REQUIRE(f.run({statements::create_index("p", "by_city", city_path())}).committed);
    BOOST_REQUIRE(f.sm.get_index("p", "by_city")->active);
    BOOST_TEST(ids_of(f.mgr.lookup(tok, "p", "by_city", ion_value::string("Oslo"))) ==
               (std::set<std::string>{I("a")}));
    f.mgr.add(tok, statements::insert("p", I("n1"), person("N", "Kyiv")));
    BOOST_TEST(f.commit(tok).code == "occ_conflict");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(scans)

BOOST_AUTO_TEST_CASE(a_filtered_scan_keeps_matching_documents_only) {
    fixture f;
    std::vector<ion_value> st{statements::create_table("p")};
    for (int i = 0; i < 9; ++i) {
        st.push_back(statements::insert("p", I("d" + std::to_string(i)),
                                        person("x", i % 3 == 0 ? "Oslo" : "Rome")));
    }
    BOOST_REQUIRE(f.run(st).committed);
    scan_options o;
    o.filter = std::make_pair(city_path(), ion_value::string("Oslo").hash());
    const auto page = f.sm.scan_table_at("p", f.sm.block_count(), o);
    BOOST_TEST(ids_of(page) == (std::set<std::string>{I("d0"), I("d3"), I("d6")}));
    BOOST_TEST(!page.next);
}

BOOST_AUTO_TEST_CASE(a_scan_page_stops_at_its_byte_budget) {
    fixture f;
    std::vector<ion_value> st{statements::create_table("p")};
    for (int i = 0; i < 10; ++i) {
        st.push_back(statements::insert("p", I("d" + std::to_string(i)),
                                        person(std::string(100, 'x'), "Oslo")));
    }
    BOOST_REQUIRE(f.run(st).committed);
    const auto one = person(std::string(100, 'x'), "Oslo").to_binary().size();

    scan_options o;
    o.max_bytes = (one * 3) - 1;  // reached by the third document
    std::size_t pages = 0;
    std::set<std::string> seen;
    do {
        const auto page = f.sm.scan_table_at("p", f.sm.block_count(), o);
        BOOST_TEST(page.documents.size() <= 3U);
        BOOST_TEST(!page.documents.empty());
        for (const auto& r : page.documents) {
            BOOST_TEST(seen.insert(r.id).second);
        }
        o.after = page.next;
        ++pages;
    } while (o.after);
    BOOST_TEST(seen.size() == 10U);
    BOOST_TEST(pages == 4U);

    // A budget below one document still returns one per page.
    o.after.reset();
    o.max_bytes = 1;
    BOOST_TEST(f.sm.scan_table_at("p", f.sm.block_count(), o).documents.size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()
