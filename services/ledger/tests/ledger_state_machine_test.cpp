// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger state machine on memory_kv_engine
// (.kiro/specs/verifiable-ledger/ tasks 3-4, Requirements 2.1, 2.2, 3.1-3.4,
// 4.1-4.4, 5.1-5.5): tables, document writes, revisions and their hashes, the block
// chain, the leader's hash in the entry and the replica check, stale and
// rejected transactions, restart, checkpoint and the split veto; and the
// digest, block and revision proofs at every tip.

#define BOOST_TEST_MODULE ledger_state_machine_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/engine_merkle_store.hpp>
#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>
#include <kythira/ledger/merkle.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace kythira::ledger;
using kythira::column_family;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;

static_assert(kythira::persistent_state_machine<ledger_sm>);
static_assert(kythira::persistent_splittable_state_machine<ledger_sm>);
static_assert(kythira::storage_kind_of<ledger_sm> == kythira::state_machine_storage::persistent);

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;  // 2025-11-09T12:00:00.000Z

class temp_dir {
public:
    temp_dir() {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("kythira-ledger-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
    std::filesystem::path path_;
};

auto doc(std::string_view text) -> ion_value {
    return ion_value::parse(text);
}

/// A ledger on its own engine, with a log index counter.
struct fixture {
    memory_kv_engine engine;
    ledger_sm sm;
    std::uint64_t index = 0;
    std::int64_t clock = t0;

    explicit fixture(ledger_options options = {}) : sm(options) {
        sm.open(engine, ledger_range("books"));
    }

    auto prepare(const std::vector<ion_value>& statements) -> prepared_transaction {
        clock += 1000;
        return sm.prepare(new_id(), clock, statements);
    }

    auto apply(const std::vector<std::byte>& command) -> apply_result {
        return apply_result::decode(sm.apply(command, ++index));
    }

    /// Prepare and apply; a rejection from prepare comes back as is.
    auto run(const std::vector<ion_value>& statements) -> apply_result {
        auto p = prepare(statements);
        if (!p.entry) {
            return p.rejection;
        }
        return apply(p.command);
    }

    auto key_count() const -> std::size_t {
        return engine.key_count(column_family::data) + engine.key_count(column_family::meta);
    }
};

auto to_bytes(const std::vector<std::uint8_t>& v) -> std::vector<std::byte> {
    std::vector<std::byte> out(v.size());
    std::memcpy(out.data(), v.data(), v.size());
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(formats)

BOOST_AUTO_TEST_CASE(ids_are_22_base62_characters_and_distinct) {
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const auto id = new_id();
        BOOST_TEST(valid_id(id));
        seen.insert(id);
    }
    BOOST_TEST(seen.size() == 200U);
    BOOST_TEST(!valid_id("short"));
    BOOST_TEST(!valid_id("0123456789012345678901/"));
    BOOST_TEST(!valid_id("012345678901234567890-"));
}

BOOST_AUTO_TEST_CASE(table_names_follow_the_qldb_rule) {
    BOOST_TEST(valid_table_name("Vehicle_Registration2"));
    BOOST_TEST(valid_table_name("_x"));
    BOOST_TEST(!valid_table_name(""));
    BOOST_TEST(!valid_table_name("2cars"));
    BOOST_TEST(!valid_table_name("a/b"));
    BOOST_TEST(!valid_table_name(std::string(129, 'a')));
    BOOST_TEST(valid_table_name(std::string(128, 'a')));
}

BOOST_AUTO_TEST_CASE(timestamps_round_trip_in_millisecond_utc_form) {
    BOOST_TEST(format_timestamp(t0 + 123) == "2025-11-09T12:00:00.123Z");
    BOOST_TEST(parse_timestamp("2025-11-09T12:00:00.123Z").value_or(0) == t0 + 123);
    BOOST_TEST(format_timestamp(0) == "1970-01-01T00:00:00.000Z");
    BOOST_TEST(format_timestamp(-1) == "1969-12-31T23:59:59.999Z");
    BOOST_TEST(!parse_timestamp("2025-11-09T12:00:00Z"));
    BOOST_TEST(!parse_timestamp("2025-02-30T12:00:00.000Z"));
    BOOST_TEST(!parse_timestamp("2025-11-09 12:00:00.123Z"));
    BOOST_TEST(timestamp_millis(timestamp_value(t0)).value_or(0) == t0);
    // A timestamp with an offset or another precision is not a block time.
    BOOST_TEST(!timestamp_millis(ion_value::timestamp("2025-11-09T04:00:00.000-08:00")));
}

BOOST_AUTO_TEST_CASE(a_statement_hashes_the_same_in_any_encoding) {
    const std::string id = "0123456789ABCDEFGHIJab";
    const auto built = statements::insert("cars", id, doc("{vin: \"1N4AL11D75C109151\", n: 2}"));
    const auto text = doc(R"({value: {n: 2, vin: "1N4AL11D75C109151"}, id: "0123456789ABCDEFGHIJab",
                             table: "cars", op: insert})");
    BOOST_TEST(to_hex(built.hash()) == to_hex(text.hash()));
    BOOST_TEST(to_hex(built.hash()) == to_hex(ion_hash(built.to_binary())));
}

BOOST_AUTO_TEST_CASE(malformed_statements_are_refused) {
    const std::string id = new_id();
    BOOST_CHECK_THROW(parse_statement(doc("{op: frobnicate, table: \"t\"}")), ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("{op: insert, table: \"t\", id: \"bad\", value: 1}")),
                      ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("{op: insert, table: \"t\", id: \"" + id + "\"}")),
                      ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("{op: patch, table: \"t\", id: \"" + id + "\"}")),
                      ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("{op: patch, table: \"t\", id: \"" + id +
                                          "\", set: [{path: [], value: 1}]}")),
                      ledger_format_error);
    BOOST_CHECK_THROW(
        parse_statement(doc("{op: delete, table: \"t\", id: \"" + id + "\", ifVersion: -1}")),
        ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("{op: create_table, table: \"1t\"}")),
                      ledger_format_error);
    BOOST_CHECK_THROW(parse_statement(doc("[op, create_table]")), ledger_format_error);
}

BOOST_AUTO_TEST_CASE(an_entry_round_trips) {
    transaction_entry e;
    e.tx_id = new_id();
    e.block_timestamp = t0;
    e.seq = 7;
    e.previous_block_hash = sha256(std::span<const std::uint8_t>{});
    e.block_hash = dot(e.previous_block_hash, e.previous_block_hash);
    e.statements.push_back(parse_statement(statements::create_table("t")));
    e.statements.push_back(parse_statement(statements::patch(
        "t", new_id(), {{{std::string("a"), std::size_t{0}}, ion_value::integer(1)}},
        {{std::string("b")}}, 4)));
    const auto back = decode_entry(encode_entry(e));
    BOOST_TEST(back.tx_id == e.tx_id);
    BOOST_TEST(back.block_timestamp == t0);
    BOOST_TEST(back.seq == 7U);
    BOOST_TEST(back.previous_block_hash == e.previous_block_hash);
    BOOST_TEST(back.block_hash == e.block_hash);
    BOOST_REQUIRE(back.statements.size() == 2U);
    BOOST_TEST(back.statements[1].sets.size() == 1U);
    BOOST_TEST(back.statements[1].removes.size() == 1U);
    BOOST_TEST(back.statements[1].if_version.value_or(0) == 4);
    BOOST_TEST(back.statements[1].ion.hash() == e.statements[1].ion.hash());
}

BOOST_AUTO_TEST_CASE(patch_paths_set_append_and_remove) {
    ion_value d = doc("{a: {b: [1, 2]}, c: 3}");
    patch_set_path(d, {std::string("a"), std::string("b"), std::size_t{1}}, ion_value::integer(20));
    patch_set_path(d, {std::string("a"), std::string("b"), std::size_t{2}}, ion_value::integer(30));
    patch_set_path(d, {std::string("a"), std::string("x")}, ion_value::string("new"));
    patch_remove_path(d, {std::string("c")});
    patch_remove_path(d, {std::string("a"), std::string("b"), std::size_t{0}});
    BOOST_TEST(to_hex(d.hash()) == to_hex(doc("{a: {b: [20, 30], x: \"new\"}}").hash()));
    BOOST_CHECK_THROW(patch_set_path(d, {std::string("zz"), std::string("y")}, ion_value{}),
                      patch_error);
    BOOST_CHECK_THROW(
        patch_set_path(d, {std::string("a"), std::string("b"), std::size_t{5}}, ion_value{}),
        patch_error);
    BOOST_CHECK_THROW(patch_remove_path(d, {std::string("c")}), patch_error);
    BOOST_CHECK_THROW(
        patch_set_path(d, {std::string("a"), std::string("b"), std::string("k")}, ion_value{}),
        patch_error);
}

BOOST_AUTO_TEST_CASE(apply_results_round_trip) {
    apply_result c;
    c.committed = true;
    c.seq = 3;
    c.block_hash[0] = 9;
    c.documents.push_back({"t", new_id(), 2});
    const auto back = apply_result::decode(c.encode());
    BOOST_TEST(back.committed);
    BOOST_TEST(back.seq == 3U);
    BOOST_TEST(back.block_hash == c.block_hash);
    BOOST_TEST(back.documents.at(0).id == c.documents[0].id);
    const auto r = apply_result::decode(apply_result::rejected("table_exists", "m", 1).encode());
    BOOST_TEST(!r.committed);
    BOOST_TEST(r.code == "table_exists");
    BOOST_TEST(r.statement.value_or(9) == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(documents_and_tables)

BOOST_AUTO_TEST_CASE(the_ledger_range_covers_exactly_its_prefix) {
    const auto r = ledger_range("books");
    BOOST_TEST(r.start == "l/books/");
    BOOST_TEST(r.end.value_or("") == "l/books0");
    BOOST_TEST(r.contains("l/books/meta/tip"));
    BOOST_TEST(!r.contains("l/books2/meta/tip"));
    BOOST_TEST(!r.contains("l/book/x"));
}

BOOST_AUTO_TEST_CASE(insert_writes_version_0_and_the_first_block) {
    fixture f;
    const auto id = new_id();
    const auto res = f.run({statements::create_table("cars"),
                            statements::insert("cars", id, doc("{vin: \"V1\", miles: 10}"))});
    BOOST_REQUIRE(res.committed);
    BOOST_TEST(res.seq == 0U);
    BOOST_REQUIRE(res.documents.size() == 1U);
    BOOST_TEST(res.documents[0].version == 0);

    const auto rev = f.sm.get_document("cars", id);
    BOOST_REQUIRE(rev.has_value());
    BOOST_TEST(rev->version == 0);
    BOOST_TEST(rev->seq == 0U);
    BOOST_TEST(to_hex(rev->data->hash()) == to_hex(doc("{miles: 10, vin: \"V1\"}").hash()));
    BOOST_TEST(rev->hash == dot(rev->data->hash(), rev->metadata().hash()));
    BOOST_TEST(rev->hash == rev->compute_hash());

    const auto blk = f.sm.get_block(0);
    BOOST_REQUIRE(blk.has_value());
    BOOST_TEST(blk->verify());
    BOOST_TEST(blk->previous_block_hash == genesis_previous_hash);
    BOOST_TEST(blk->block_hash == res.block_hash);
    BOOST_TEST(blk->statements.size() == 2U);
    BOOST_TEST(blk->revisions.size() == 1U);
    // entriesHash: statement hashes, then revision hashes.
    const std::vector<hash256> leaves{blk->statements[0].hash(), blk->statements[1].hash(),
                                      rev->hash};
    BOOST_TEST(blk->entries_hash == merkle_root(leaves));

    const auto tip = f.sm.tip();
    BOOST_REQUIRE(tip.has_value());
    BOOST_TEST(tip->seq == 0U);
    BOOST_TEST(tip->hash == res.block_hash);
    BOOST_TEST(f.sm.applied_index() == 1U);
    BOOST_TEST(f.sm.table("cars").has_value());
}

BOOST_AUTO_TEST_CASE(each_block_links_to_the_one_before) {
    fixture f;
    const auto id = new_id();
    BOOST_REQUIRE(f.run({statements::create_table("cars")}).committed);
    BOOST_REQUIRE(f.run({statements::insert("cars", id, doc("{n: 0}"))}).committed);
    for (int i = 1; i <= 5; ++i) {
        const auto r =
            f.run({statements::replace("cars", id, doc("{n: " + std::to_string(i) + "}"))});
        BOOST_REQUIRE(r.committed);
        BOOST_TEST(r.seq == static_cast<std::uint64_t>(i + 1));
        BOOST_TEST(r.documents.at(0).version == i);
    }
    for (std::uint64_t s = 1; s <= 6; ++s) {
        const auto b = f.sm.get_block(s);
        BOOST_REQUIRE(b.has_value());
        BOOST_TEST(b->verify());
        BOOST_TEST(b->previous_block_hash == f.sm.get_block(s - 1)->block_hash);
        BOOST_TEST(b->timestamp > f.sm.get_block(s - 1)->timestamp);
    }
    // Every revision stays readable.
    for (std::int64_t v = 0; v <= 5; ++v) {
        const auto rev = f.sm.get_revision("cars", id, v);
        BOOST_REQUIRE(rev.has_value());
        BOOST_TEST(to_hex(rev->data->hash()) ==
                   to_hex(doc("{n: " + std::to_string(v) + "}").hash()));
        BOOST_TEST(rev->hash == rev->compute_hash());
    }
    BOOST_TEST(f.sm.get_document("cars", id)->version == 5);
}

BOOST_AUTO_TEST_CASE(a_conditional_write_checks_the_current_version) {
    fixture f;
    const auto id = new_id();
    f.run({statements::create_table("t"), statements::insert("t", id, doc("{a: 1}"))});
    const auto tip = f.sm.tip();
    const auto bad = f.run({statements::replace("t", id, doc("{a: 2}"), 3)});
    BOOST_TEST(!bad.committed);
    BOOST_TEST(bad.code == "version_mismatch");
    BOOST_TEST(bad.statement.value_or(9) == 0U);
    BOOST_TEST(f.sm.tip()->seq == tip->seq);  // nothing appended
    const auto good = f.run({statements::replace("t", id, doc("{a: 2}"), 0)});
    BOOST_TEST(good.committed);
    BOOST_TEST(f.run({statements::remove("t", id, 0)}).code == "version_mismatch");
    BOOST_TEST(f.run({statements::remove("t", id, 1)}).committed);
}

BOOST_AUTO_TEST_CASE(patch_edits_the_current_document) {
    fixture f;
    const auto id = new_id();
    f.run({statements::create_table("t"),
           statements::insert("t", id, doc("{owner: {name: \"A\"}, tags: [x], old: 1}"))});
    const auto r = f.run(
        {statements::patch("t", id,
                           {{{std::string("owner"), std::string("name")}, ion_value::string("B")},
                            {{std::string("tags"), std::size_t{1}}, ion_value::symbol("y")}},
                           {{std::string("old")}})});
    BOOST_REQUIRE(r.committed);
    const auto cur = f.sm.get_document("t", id);
    BOOST_TEST(cur->version == 1);
    BOOST_TEST(to_hex(cur->data->hash()) ==
               to_hex(doc("{owner: {name: \"B\"}, tags: [x, y]}").hash()));

    const auto bad = f.run({statements::patch("t", id, {}, {{std::string("missing")}})});
    BOOST_TEST(bad.code == "invalid_path");
    // Two patches in one transaction: the second sees the first.
    const auto two =
        f.run({statements::patch("t", id, {{{std::string("n")}, ion_value::integer(1)}}, {}),
               statements::patch("t", id, {{{std::string("m")}, ion_value::integer(2)}}, {})});
    BOOST_REQUIRE(two.committed);
    BOOST_TEST(two.documents.size() == 2U);
    BOOST_TEST(two.documents[1].version == 3);
    BOOST_TEST(to_hex(f.sm.get_document("t", id)->data->hash()) ==
               to_hex(doc("{owner: {name: \"B\"}, tags: [x, y], n: 1, m: 2}").hash()));
}

BOOST_AUTO_TEST_CASE(delete_leaves_a_revision_hashed_on_metadata_alone) {
    fixture f;
    const auto id = new_id();
    f.run({statements::create_table("t"), statements::insert("t", id, doc("{a: 1}"))});
    const auto r = f.run({statements::remove("t", id)});
    BOOST_REQUIRE(r.committed);
    BOOST_TEST(!f.sm.get_document("t", id).has_value());
    const auto rev = f.sm.get_revision("t", id, 1);
    BOOST_REQUIRE(rev.has_value());
    BOOST_TEST(!rev->data.has_value());
    BOOST_TEST(rev->hash == rev->metadata().hash());
    BOOST_TEST(f.run({statements::replace("t", id, doc("{}"))}).code == "document_not_found");
    BOOST_TEST(f.run({statements::remove("t", id)}).code == "document_not_found");
    BOOST_TEST(f.run({statements::insert("t", id, doc("{}"))}).code == "document_exists");
}

BOOST_AUTO_TEST_CASE(table_lifecycle) {
    fixture f;
    const auto id = new_id();
    BOOST_TEST(f.run({statements::insert("t", id, doc("{}"))}).code == "table_not_found");
    BOOST_TEST(f.run({statements::create_table("t")}).committed);
    BOOST_TEST(f.run({statements::create_table("t")}).code == "table_exists");
    BOOST_TEST(f.run({statements::undrop_table("t")}).code == "table_active");
    BOOST_TEST(f.run({statements::insert("t", id, doc("{}"))}).committed);
    BOOST_TEST(f.run({statements::drop_table("t")}).committed);
    BOOST_TEST(!f.sm.table("t")->active);
    BOOST_TEST(f.run({statements::drop_table("t")}).code == "table_inactive");
    BOOST_TEST(f.run({statements::replace("t", id, doc("{}"))}).code == "table_inactive");
    BOOST_TEST(f.run({statements::create_table("t")}).code == "table_exists");
    // A dropped table keeps its documents.
    BOOST_TEST(f.sm.get_document("t", id).has_value());
    BOOST_TEST(f.run({statements::undrop_table("t")}).committed);
    BOOST_TEST(f.run({statements::replace("t", id, doc("{v: 1}"))}).committed);
    BOOST_TEST(f.run({statements::drop_table("nope")}).code == "table_not_found");
    BOOST_TEST(f.run({statements::create_table("u")}).committed);
    const auto all = f.sm.tables();
    BOOST_REQUIRE(all.size() == 2U);
    BOOST_TEST(all[0].name == "t");
    BOOST_TEST(all[0].created_seq == 0U);
    BOOST_TEST(all[1].name == "u");
}

BOOST_AUTO_TEST_CASE(a_rejected_transaction_writes_nothing_but_the_applied_index) {
    fixture f;
    const auto a = new_id();
    const auto b = new_id();
    f.run({statements::create_table("t"), statements::insert("t", a, doc("{}"))});
    const auto keys = f.key_count();
    const auto tip = f.sm.tip();
    // The first statement would succeed; the second fails, so neither applies.
    const auto r = f.run(
        {statements::insert("t", b, doc("{}")), statements::replace("t", new_id(), doc("{}"))});
    BOOST_TEST(!r.committed);
    BOOST_TEST(r.code == "document_not_found");
    BOOST_TEST(r.statement.value_or(9) == 1U);
    BOOST_TEST(!f.sm.get_document("t", b).has_value());
    BOOST_TEST(f.key_count() == keys);
    BOOST_TEST(f.sm.tip()->hash == tip->hash);
    BOOST_TEST(!f.sm.get_block(tip->seq + 1).has_value());
}

BOOST_AUTO_TEST_CASE(prepare_refuses_what_apply_would_reject) {
    fixture f;
    const auto p = f.prepare({statements::insert("t", new_id(), doc("{}"))});
    BOOST_TEST(!p.entry.has_value());
    BOOST_TEST(p.rejection.code == "table_not_found");
    BOOST_TEST(p.command.empty());
    const auto q = f.sm.prepare("not-an-id", t0, {statements::create_table("t")});
    BOOST_TEST(q.rejection.code == "invalid_entry");
    BOOST_TEST(f.sm.prepare(new_id(), t0, {}).rejection.code == "invalid_entry");
}

BOOST_AUTO_TEST_CASE(documents_over_the_size_limit_are_rejected) {
    fixture f(ledger_options{.max_document_bytes = 64});
    const auto id = new_id();
    BOOST_TEST(f.run({statements::create_table("t"),
                      statements::insert("t", id, doc("{s: \"" + std::string(100, 'x') + "\"}"))})
                   .code == "document_too_large");
    BOOST_TEST(f.run({statements::create_table("t"), statements::insert("t", id, doc("{s: 1}"))})
                   .committed);
    BOOST_TEST(
        f.run({statements::patch(
                  "t", id, {{{std::string("big")}, ion_value::string(std::string(100, 'y'))}}, {})})
            .code == "document_too_large");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(leader_and_replicas)

BOOST_AUTO_TEST_CASE(replicas_applying_the_same_entries_build_identical_journals) {
    fixture leader;
    fixture replica;
    const auto id = new_id();
    std::vector<std::vector<std::byte>> log;
    const std::vector<std::vector<ion_value>> txns{
        {statements::create_table("t")},
        {statements::insert("t", id, doc("{a: 1.50, b: 2026-01-01T00:00Z, c: {{AAEC}}}"))},
        {statements::patch("t", id, {{{std::string("d")}, doc("[1, two, \"3\"]")}}, {})},
        {statements::remove("t", id)},
    };
    for (const auto& t : txns) {
        auto p = leader.prepare(t);
        BOOST_REQUIRE(p.entry.has_value());
        BOOST_REQUIRE(leader.apply(p.command).committed);
        log.push_back(p.command);
    }
    for (const auto& c : log) {
        BOOST_REQUIRE(replica.apply(c).committed);
    }
    for (std::uint64_t s = 0; s < txns.size(); ++s) {
        BOOST_TEST(leader.sm.get_block(s)->to_ion().to_binary() ==
                   replica.sm.get_block(s)->to_ion().to_binary());
    }
    BOOST_TEST(leader.sm.tip()->hash == replica.sm.tip()->hash);
}

BOOST_AUTO_TEST_CASE(a_proposal_prepared_against_an_old_tip_is_stale) {
    fixture f;
    f.run({statements::create_table("t")});
    auto first = f.prepare({statements::insert("t", new_id(), doc("{}"))});
    auto second = f.prepare({statements::insert("t", new_id(), doc("{}"))});
    BOOST_REQUIRE(first.entry && second.entry);
    BOOST_TEST(first.entry->seq == second.entry->seq);
    BOOST_TEST(f.apply(first.command).committed);
    const auto keys = f.key_count();
    const auto r = f.apply(second.command);
    BOOST_TEST(!r.committed);
    BOOST_TEST(r.code == "stale_block");
    BOOST_TEST(f.key_count() == keys);
    BOOST_TEST(f.sm.applied_index() == f.index);
}

BOOST_AUTO_TEST_CASE(a_block_hash_that_differs_from_the_leaders_stops_the_replica) {
    fixture f;
    f.run({statements::create_table("t")});
    auto p = f.prepare({statements::insert("t", new_id(), doc("{a: 1}"))});
    BOOST_REQUIRE(p.entry.has_value());
    auto tampered = *p.entry;
    tampered.block_hash[5] ^= 0x01U;
    const auto bytes = to_bytes(encode_entry(tampered));
    const auto applied = f.sm.applied_index();
    const auto keys = f.key_count();
    BOOST_CHECK_THROW(f.sm.apply(bytes, applied + 1), ledger_divergence_error);
    BOOST_CHECK_THROW(f.sm.apply(bytes, applied + 1), kythira::persistent_storage_error);
    BOOST_TEST(f.sm.applied_index() == applied);
    BOOST_TEST(f.key_count() == keys);
    // The leader's own entry still applies at that index.
    BOOST_TEST(apply_result::decode(f.sm.apply(p.command, applied + 1)).committed);
}

BOOST_AUTO_TEST_CASE(the_block_timestamp_never_runs_backwards) {
    fixture f;
    f.run({statements::create_table("t")});
    const auto last = f.sm.tip()->timestamp;
    auto p = f.sm.prepare(new_id(), last - 5000, {statements::create_table("u")});
    BOOST_REQUIRE(p.entry.has_value());
    BOOST_TEST(p.entry->block_timestamp == last);  // clamped
    BOOST_TEST(f.apply(p.command).committed);

    // An entry whose timestamp is behind the tip is refused even when its
    // seq and previous hash fit.
    auto q = f.sm.prepare(new_id(), last + 1000, {statements::create_table("v")});
    auto back = *q.entry;
    back.block_timestamp = last - 1;
    const auto r = f.apply(to_bytes(encode_entry(back)));
    BOOST_TEST(r.code == "stale_block");
}

BOOST_AUTO_TEST_CASE(a_malformed_entry_is_rejected_without_stopping) {
    fixture f;
    const std::vector<std::byte> junk{std::byte{0xE0}, std::byte{0x01}, std::byte{0x00},
                                      std::byte{0xEA}, std::byte{0x2F}};
    const auto r = f.apply(junk);
    BOOST_TEST(r.code == "invalid_entry");
    BOOST_TEST(f.sm.applied_index() == 1U);
    const auto text = std::string("{type: ledger_txn}");
    const auto r2 = f.apply(to_bytes({text.begin(), text.end()}));
    BOOST_TEST(r2.code == "invalid_entry");
    BOOST_TEST(!f.sm.tip().has_value());
}

BOOST_AUTO_TEST_CASE(replaying_an_applied_entry_does_nothing) {
    fixture f;
    auto p = f.prepare({statements::create_table("t")});
    BOOST_TEST(f.apply(p.command).committed);
    const auto keys = f.key_count();
    BOOST_TEST(f.sm.apply(p.command, f.index).empty());
    BOOST_TEST(f.key_count() == keys);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(persistence)

BOOST_AUTO_TEST_CASE(reopening_restores_the_tip_and_applied_index) {
    fixture f;
    const auto id = new_id();
    f.run({statements::create_table("t"), statements::insert("t", id, doc("{a: 1}"))});
    f.run({statements::replace("t", id, doc("{a: 2}"))});
    const auto tip = f.sm.tip();

    ledger_sm reopened;
    reopened.open(f.engine, ledger_range("books"));
    BOOST_TEST(reopened.applied_index() == f.index);
    BOOST_TEST(reopened.durable_applied_index() == f.index);
    BOOST_REQUIRE(reopened.tip().has_value());
    BOOST_TEST(reopened.tip()->hash == tip->hash);
    BOOST_TEST(reopened.tip()->timestamp == tip->timestamp);
    auto p = reopened.prepare(new_id(), f.clock + 1000, {statements::remove("t", id)});
    BOOST_REQUIRE(p.entry.has_value());
    BOOST_TEST(p.entry->seq == tip->seq + 1);
    BOOST_TEST(apply_result::decode(reopened.apply(p.command, f.index + 1)).committed);
}

BOOST_AUTO_TEST_CASE(two_ledgers_share_an_engine_without_touching_each_other) {
    memory_kv_engine engine;
    ledger_sm a;
    ledger_sm b;
    a.open(engine, ledger_range("a"));
    b.open(engine, ledger_range("b"));
    auto pa = a.prepare(new_id(), t0, {statements::create_table("t")});
    BOOST_TEST(apply_result::decode(a.apply(pa.command, 1)).committed);
    BOOST_TEST(!b.tip().has_value());
    BOOST_TEST(!b.table("t").has_value());
    auto pb = b.prepare(new_id(), t0, {statements::create_table("t")});
    BOOST_TEST(apply_result::decode(b.apply(pb.command, 1)).committed);
    BOOST_TEST(a.tip()->hash != b.tip()->hash);  // different txIds
    b.drop();
    BOOST_TEST(!b.table("t").has_value());
    BOOST_TEST(a.table("t").has_value());
}

BOOST_AUTO_TEST_CASE(a_checkpoint_carries_the_whole_ledger) {
    fixture src;
    const auto id = new_id();
    src.run({statements::create_table("t"), statements::insert("t", id, doc("{a: 1}"))});
    src.run({statements::replace("t", id, doc("{a: 2}"))});
    temp_dir dir;
    const auto ckpt = dir.path() / "checkpoint";  // the engine creates it
    const auto manifest = src.sm.checkpoint(ckpt);

    memory_kv_engine engine;
    ledger_sm dst;
    dst.open(engine, ledger_range("books"));
    dst.ingest(ckpt, manifest, src.index);
    BOOST_TEST(dst.applied_index() == src.index);
    BOOST_TEST(dst.tip()->hash == src.sm.tip()->hash);
    BOOST_TEST(dst.get_document("t", id)->version == 1);
    BOOST_TEST(dst.get_block(0)->verify());
    // Both continue identically.
    auto p = src.prepare({statements::remove("t", id)});
    BOOST_TEST(src.apply(p.command).committed);
    BOOST_TEST(apply_result::decode(dst.apply(p.command, src.index)).committed);
    BOOST_TEST(dst.tip()->hash == src.sm.tip()->hash);

    dst.drop();
    BOOST_TEST(dst.applied_index() == 0U);
    BOOST_TEST(!dst.tip().has_value());
    BOOST_TEST(engine.key_count(column_family::data) == 0U);
    BOOST_TEST(engine.key_count(column_family::meta) == 0U);
}

BOOST_AUTO_TEST_CASE(durable_applied_index_follows_the_engine) {
    fixture f;
    f.run({statements::create_table("t")});
    const auto floor = f.sm.durable_applied_index();
    BOOST_TEST(floor == f.index);
    f.engine.hold_durability();
    f.run({statements::create_table("u")});
    f.run({statements::create_table("v")});
    BOOST_TEST(f.sm.applied_index() == floor + 2);
    BOOST_TEST(f.sm.durable_applied_index() == floor);
    f.engine.release_durability();
    BOOST_TEST(f.sm.durable_applied_index() == floor + 2);
}

BOOST_AUTO_TEST_CASE(splits_and_merges_are_vetoed) {
    fixture f;
    BOOST_CHECK_THROW(f.sm.split_at({"l/books/m"}), ledger_split_vetoed);
    BOOST_CHECK_THROW(f.sm.adopt(ledger_range("other")), ledger_split_vetoed);
    BOOST_TEST(f.sm.range().start == "l/books/");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(journal_records)

BOOST_AUTO_TEST_CASE(blocks_and_revisions_round_trip_and_detect_tampering) {
    fixture f;
    const auto id = new_id();
    f.run({statements::create_table("t"), statements::insert("t", id, doc("{a: 1}"))});
    const auto blk = f.sm.get_block(0);
    BOOST_REQUIRE(blk.has_value());
    const auto again = block::from_ion(ion_value::parse(blk->to_ion().to_binary()));
    BOOST_TEST(again.verify());
    BOOST_TEST(again.block_hash == blk->block_hash);

    auto bad_data = *blk;
    bad_data.revisions[0].data = doc("{a: 2}");
    BOOST_TEST(!bad_data.verify());
    auto bad_stmt = *blk;
    bad_stmt.statements[0] = statements::create_table("u");
    BOOST_TEST(!bad_stmt.verify());
    auto bad_time = *blk;
    bad_time.timestamp += 1;
    BOOST_TEST(!bad_time.verify());
    auto bad_prev = *blk;
    bad_prev.previous_block_hash[0] ^= 1U;
    BOOST_TEST(!bad_prev.verify());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Task 4: digests and proofs (Requirements 4.1-4.4, 4.6)
// ---------------------------------------------------------------------------

namespace {

/// A fixture with `blocks` committed blocks of mixed shapes: table
/// creation, multi-statement inserts, patches, replaces and deletes.
struct journal_fixture : fixture {
    std::vector<std::string> ids;

    explicit journal_fixture(std::size_t blocks) {
        BOOST_REQUIRE(
            run({statements::create_table("t"), statements::create_table("u")}).committed);
        while (tip()->seq + 1 < blocks) {
            const auto n = tip()->seq;
            if (n % 4 == 1 || ids.size() < 2) {
                const auto a = new_id();
                const auto b = new_id();
                BOOST_REQUIRE(
                    run({statements::insert("t", a, doc("{n: " + std::to_string(n) + "}")),
                         statements::insert("u", b, doc("[" + std::to_string(n) + "]"))})
                        .committed);
                ids.push_back(a);
            } else if (n % 4 == 2) {
                BOOST_REQUIRE(
                    run({statements::patch("t", ids.back(),
                                           {{{std::string("p")},
                                             ion_value::integer(static_cast<std::int64_t>(n))}},
                                           {})})
                        .committed);
            } else if (n % 4 == 3) {
                BOOST_REQUIRE(
                    run({statements::replace("t", ids.front(), doc("{r: 1}"))}).committed);
            } else {
                const auto victim = ids.back();
                ids.pop_back();
                BOOST_REQUIRE(run({statements::remove("t", victim)}).committed);
            }
        }
    }

    auto tip() const -> std::optional<block_tip> { return sm.tip(); }

    /// Merkle node keys in the engine.
    auto merkle_keys() const -> std::size_t {
        std::size_t count = 0;
        const std::string from = "l/books/merkle/";
        std::string to = from;
        to.back() = '0';
        engine.scan(column_family::data, kythira::key_range{from, to},
                    kythira::scan_direction::forward, [&](std::string_view, std::string_view) {
                        ++count;
                        return true;
                    });
        return count;
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(digests_and_proofs)

BOOST_AUTO_TEST_CASE(the_block_hash_is_a_dot_fold_of_entries_header_and_previous) {
    journal_fixture f(3);
    for (std::uint64_t s = 0; s < 3; ++s) {
        const auto b = *f.sm.get_block(s);
        BOOST_TEST(b.block_hash ==
                   dot(dot(b.entries_hash, b.header().hash()), b.previous_block_hash));
        for (std::size_t i = 0; i < b.revisions.size(); ++i) {
            BOOST_TEST(verify_proof(b.revisions[i].hash, b.revision_proof(i), b.block_hash));
        }
    }
}

BOOST_AUTO_TEST_CASE(the_digest_is_the_accumulator_over_block_hashes) {
    fixture f;
    BOOST_TEST(!f.sm.get_digest().has_value());
    BOOST_CHECK_THROW((void)f.sm.digest_at(0), ledger_query_error);
    memory_merkle_store ref_store;
    merkle_accumulator<memory_merkle_store> ref(ref_store);
    const auto id = new_id();
    f.run({statements::create_table("t")});
    ref.append(f.sm.tip()->hash);
    for (int i = 0; i < 20; ++i) {
        // Rejections add no leaf.
        BOOST_TEST(!f.run({statements::create_table("t")}).committed);
        const auto r =
            i == 0 ? f.run({statements::insert("t", id, doc("{i: 0}"))})
                   : f.run({statements::replace("t", id, doc("{i: " + std::to_string(i) + "}"))});
        BOOST_REQUIRE(r.committed);
        ref.append(r.block_hash);
        const auto d = f.sm.get_digest();
        BOOST_REQUIRE(d.has_value());
        BOOST_TEST(d->tip == r.seq);
        BOOST_TEST(d->digest == *ref.digest());
    }
    for (std::uint64_t t = 0; t <= f.sm.tip()->seq; ++t) {
        BOOST_TEST(f.sm.digest_at(t) == ref.digest_at(t + 1));
    }
}

BOOST_AUTO_TEST_CASE(every_block_proves_at_every_tip) {
    journal_fixture f(24);
    const auto last = f.sm.tip()->seq;
    for (std::uint64_t tip = 0; tip <= last; ++tip) {
        const auto digest = f.sm.digest_at(tip);
        for (std::uint64_t seq = 0; seq <= tip; ++seq) {
            const auto p = f.sm.get_block_proof(seq, tip);
            BOOST_TEST(p.blk.seq == seq);
            BOOST_TEST(p.digest == digest);
            BOOST_TEST(p.blk.verify());
            BOOST_TEST(verify_proof(p.blk.block_hash, p.proof, digest));
            BOOST_TEST(p.proof.size() <= static_cast<std::size_t>(std::bit_width(tip)));
        }
    }
    BOOST_CHECK_THROW((void)f.sm.get_block_proof(5, 4), ledger_query_error);
    BOOST_CHECK_THROW((void)f.sm.get_block_proof(0, last + 1), ledger_query_error);
}

BOOST_AUTO_TEST_CASE(every_revision_proves_at_every_tip) {
    journal_fixture f(16);
    const auto last = f.sm.tip()->seq;
    std::size_t checked = 0;
    for (std::uint64_t seq = 0; seq <= last; ++seq) {
        const auto blk = f.sm.get_block(seq);
        for (const auto& r : blk->revisions) {
            for (std::uint64_t tip = seq; tip <= last; ++tip) {
                const auto p = f.sm.get_revision_proof(r.table, r.id, r.version, tip);
                BOOST_REQUIRE(p.has_value());
                BOOST_TEST(p->rev.hash == r.hash);
                BOOST_TEST(p->digest == f.sm.digest_at(tip));
                BOOST_TEST(verify_proof(p->rev.hash, p->proof, p->digest));
                ++checked;
            }
            if (seq > 0) {
                BOOST_CHECK_THROW((void)f.sm.get_revision_proof(r.table, r.id, r.version, seq - 1),
                                  ledger_query_error);
            }
        }
    }
    BOOST_TEST(checked > 50U);
    BOOST_TEST(!f.sm.get_revision_proof("t", new_id(), 0, last).has_value());
}

BOOST_AUTO_TEST_CASE(a_proof_does_not_verify_against_another_digest_or_leaf) {
    journal_fixture f(9);
    const auto p = f.sm.get_block_proof(3, 8);
    BOOST_TEST(!verify_proof(p.blk.block_hash, p.proof, f.sm.digest_at(7)));
    BOOST_TEST(!verify_proof(f.sm.get_block(4)->block_hash, p.proof, p.digest));
    auto cut = p.proof;
    cut.pop_back();
    BOOST_TEST(!verify_proof(p.blk.block_hash, cut, p.digest));
}

BOOST_AUTO_TEST_CASE(the_merkle_nodes_are_written_with_their_blocks) {
    for (const std::size_t n : {1U, 2U, 7U, 8U, 13U}) {
        journal_fixture f(n);
        // Every complete node, leaves included: 2n - popcount(n).
        BOOST_TEST(f.merkle_keys() == 2 * n - static_cast<std::size_t>(std::popcount(n)));
    }
}

BOOST_AUTO_TEST_CASE(reopening_rebuilds_the_frontier) {
    journal_fixture f(11);
    const auto d = f.sm.get_digest();
    ledger_sm reopened;
    reopened.open(f.engine, ledger_range("books"));
    BOOST_TEST(reopened.get_digest()->digest == d->digest);
    BOOST_TEST(reopened.get_digest()->tip == d->tip);
    // And keeps appending the same way as the original would.
    auto p = reopened.prepare(new_id(), f.clock + 1000, {statements::create_table("z")});
    BOOST_REQUIRE(p.entry.has_value());
    const auto r = apply_result::decode(reopened.apply(p.command, f.index + 1));
    BOOST_REQUIRE(r.committed);
    memory_merkle_store ref_store;
    merkle_accumulator<memory_merkle_store> ref(ref_store);
    for (std::uint64_t s = 0; s <= r.seq; ++s) {
        ref.append(reopened.get_block(s)->block_hash);
    }
    BOOST_TEST(reopened.get_digest()->digest == *ref.digest());
    const auto proof = reopened.get_block_proof(4, r.seq);
    BOOST_TEST(verify_proof(proof.blk.block_hash, proof.proof, proof.digest));
}

BOOST_AUTO_TEST_CASE(a_checkpoint_carries_the_merkle_nodes) {
    journal_fixture src(10);
    temp_dir dir;
    const auto ckpt = dir.path() / "checkpoint";
    const auto manifest = src.sm.checkpoint(ckpt);
    memory_kv_engine engine;
    ledger_sm dst;
    dst.open(engine, ledger_range("books"));
    dst.ingest(ckpt, manifest, src.index);
    BOOST_TEST(dst.get_digest()->digest == src.sm.get_digest()->digest);
    for (std::uint64_t s = 0; s <= 9; ++s) {
        const auto p = dst.get_block_proof(s, 9);
        BOOST_TEST(verify_proof(p.blk.block_hash, p.proof, p.digest));
    }
    dst.drop();
    BOOST_TEST(!dst.get_digest().has_value());
}

BOOST_AUTO_TEST_CASE(a_tampered_node_breaks_the_proofs_through_it) {
    journal_fixture f(8);
    const auto good = f.sm.get_block_proof(0, 7);
    BOOST_REQUIRE(verify_proof(good.blk.block_hash, good.proof, good.digest));
    // Node (1, 1) covers blocks 2-3; it is in block 0's path to the root.
    engine_merkle_store<memory_kv_engine> store(f.engine, "l/books/merkle/");
    hash256 junk{};
    junk.fill(0xAB);
    f.engine.write(kythira::write_batch{}.put(column_family::data, store.key(1, 1),
                                              std::string(junk.begin(), junk.end())));
    const auto bad = f.sm.get_block_proof(0, 7);
    BOOST_TEST(!verify_proof(bad.blk.block_hash, bad.proof, good.digest));
    // A missing node is an error, not a silently short proof.
    f.engine.write(kythira::write_batch{}.erase(column_family::data, store.key(1, 1)));
    BOOST_CHECK_THROW((void)f.sm.get_block_proof(0, 7), merkle_store_error);
}

BOOST_AUTO_TEST_CASE(a_divergent_entry_leaves_the_accumulator_untouched) {
    journal_fixture f(3);
    auto p = f.prepare({statements::create_table("x")});
    auto tampered = *p.entry;
    tampered.block_hash[0] ^= 1U;
    BOOST_CHECK_THROW(f.sm.apply(to_bytes(encode_entry(tampered)), f.index + 1),
                      ledger_divergence_error);
    BOOST_TEST(f.sm.get_digest()->tip == 2U);
    const auto r = apply_result::decode(f.sm.apply(p.command, ++f.index));
    BOOST_REQUIRE(r.committed);
    const auto proof = f.sm.get_block_proof(3, 3);
    BOOST_TEST(verify_proof(r.block_hash, proof.proof, proof.digest));
}

BOOST_AUTO_TEST_SUITE_END()
