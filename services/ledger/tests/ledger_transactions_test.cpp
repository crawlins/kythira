// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Ledger transactions (.kiro/specs/verifiable-ledger/ task 5, Requirements
// 7.1-7.7 and 12.3): snapshot reads, read sets and their validation in
// apply, statement limits, session expiry and leader change, and a
// randomized check that committed transactions are serializable in commit
// order, phantoms included.

#define BOOST_TEST_MODULE ledger_transactions_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/memory_kv_engine.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace kythira::ledger;
using kythira::column_family;
using kythira::memory_kv_engine;

using ledger_sm = basic_ledger_state_machine<memory_kv_engine>;
using sessions = ledger_session_manager<ledger_sm>;

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;

auto doc(std::string_view text) -> ion_value {
    return ion_value::parse(text);
}

auto counter(std::int64_t n) -> ion_value {
    ion_value v = ion_value::make_struct();
    v.set("n", ion_value::integer(n));
    return v;
}

auto value_of(const revision& r) -> std::int64_t {
    return r.data->find("n")->as_int64().value_or(-1);
}

struct fixture {
    memory_kv_engine engine;
    ledger_sm sm;
    std::uint64_t index = 0;
    std::int64_t clock = t0;
    std::chrono::steady_clock::time_point now{};
    sessions mgr;

    explicit fixture(ledger_options options = {}, session_limits limits = {})
        : sm(options), mgr(sm, limits, [this] { return now; }) {
        sm.open(engine, ledger_range("bank"));
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
};

/// A ledger with table `acct` holding `n` counters at 0; their ids.
auto seed(fixture& f, int n) -> std::vector<std::string> {
    std::vector<ion_value> st{statements::create_table("acct")};
    std::vector<std::string> ids;
    for (int i = 0; i < n; ++i) {
        ids.push_back(new_id());
        st.push_back(statements::insert("acct", ids.back(), counter(0)));
    }
    BOOST_REQUIRE(f.run(st).committed);
    return ids;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(snapshot_reads)

BOOST_AUTO_TEST_CASE(a_session_reads_the_state_as_of_begin) {
    fixture f;
    const auto ids = seed(f, 2);
    const auto tok = f.mgr.begin(1);
    BOOST_TEST(f.mgr.snapshot(tok) == 1U);
    BOOST_REQUIRE(f.run({statements::replace("acct", ids[0], counter(5))}).committed);
    BOOST_REQUIRE(f.run({statements::remove("acct", ids[1])}).committed);
    const auto a = f.mgr.get(tok, "acct", ids[0]);
    BOOST_REQUIRE(a.has_value());
    BOOST_TEST(value_of(*a) == 0);
    BOOST_TEST(a->version == 0);
    BOOST_TEST(f.mgr.get(tok, "acct", ids[1]).has_value());  // deleted after S
    const auto page = f.mgr.scan(tok, "acct");
    BOOST_TEST(page.documents.size() == 2U);
    // A new session sees the changes.
    const auto tok2 = f.mgr.begin(1);
    BOOST_TEST(value_of(*f.mgr.get(tok2, "acct", ids[0])) == 5);
    BOOST_TEST(!f.mgr.get(tok2, "acct", ids[1]).has_value());
    BOOST_TEST(f.mgr.scan(tok2, "acct").documents.size() == 1U);
}

BOOST_AUTO_TEST_CASE(reads_at_older_snapshots_walk_back_through_versions) {
    fixture f;
    const auto ids = seed(f, 1);
    for (int i = 1; i <= 6; ++i) {
        BOOST_REQUIRE(f.run({statements::replace("acct", ids[0], counter(i))}).committed);
    }
    for (std::uint64_t blocks = 1; blocks <= 7; ++blocks) {
        const auto r = f.sm.get_document_at("acct", ids[0], blocks);
        BOOST_REQUIRE(r.has_value());
        BOOST_TEST(value_of(*r) == static_cast<std::int64_t>(blocks - 1));
    }
    BOOST_TEST(!f.sm.get_document_at("acct", ids[0], 0).has_value());
}

BOOST_AUTO_TEST_CASE(scans_page_in_id_order) {
    fixture f;
    const auto ids = seed(f, 7);
    std::set<std::string> seen;
    std::optional<std::string> after;
    std::size_t pages = 0;
    do {
        const auto page = f.sm.scan_table_at("acct", f.sm.block_count(), after, 3);
        for (const auto& r : page.documents) {
            BOOST_TEST((seen.empty() || r.id > *seen.rbegin()));
            seen.insert(r.id);
        }
        after = page.next;
        ++pages;
    } while (after);
    BOOST_TEST(seen.size() == 7U);
    BOOST_TEST(pages == 3U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(validation)

BOOST_AUTO_TEST_CASE(a_document_changed_after_the_snapshot_conflicts) {
    fixture f;
    const auto ids = seed(f, 2);
    const auto tok = f.mgr.begin(1);
    const auto a = f.mgr.get(tok, "acct", ids[0]);
    f.mgr.add(tok, statements::replace("acct", ids[1], counter(value_of(*a) + 1)));
    BOOST_REQUIRE(f.run({statements::replace("acct", ids[0], counter(9))}).committed);
    const auto r = f.commit(tok);
    BOOST_TEST(!r.committed);
    BOOST_TEST(r.code == "occ_conflict");
    BOOST_TEST(retryable_rejection(r.code));
    BOOST_TEST(value_of(*f.sm.get_document("acct", ids[1])) == 0);
}

BOOST_AUTO_TEST_CASE(apply_catches_a_conflict_that_arrives_after_prepare) {
    fixture f;
    const auto ids = seed(f, 2);
    const auto tok = f.mgr.begin(1);
    (void)f.mgr.get(tok, "acct", ids[0]);
    f.mgr.add(tok, statements::replace("acct", ids[1], counter(1)));
    f.clock += 10;
    const auto prepared = f.mgr.commit(tok, f.clock);  // valid now
    BOOST_REQUIRE(prepared.entry.has_value());
    // Another transaction changes ids[0], then the first entry applies. Its
    // seq is stale too, so re-prepare it as a leader would, keeping the
    // read set.
    BOOST_REQUIRE(f.run({statements::replace("acct", ids[0], counter(3))}).committed);
    std::vector<ion_value> st;
    for (const auto& s : prepared.entry->statements) {
        st.push_back(s.ion);
    }
    auto entry = *prepared.entry;
    entry.seq = f.sm.tip()->seq + 1;
    entry.previous_block_hash = f.sm.tip()->hash;
    entry.block_timestamp = f.sm.tip()->timestamp;
    const auto bytes = encode_entry(entry);
    std::vector<std::byte> cmd(bytes.size());
    std::memcpy(cmd.data(), bytes.data(), bytes.size());
    const auto r = apply_result::decode(f.sm.apply(cmd, ++f.index));
    BOOST_TEST(r.code == "occ_conflict");
    // prepare against the new state refuses it as well.
    BOOST_TEST(f.sm.prepare(new_id(), f.clock, st, prepared.entry->reads).rejection.code ==
               "occ_conflict");
}

BOOST_AUTO_TEST_CASE(disjoint_transactions_both_commit) {
    fixture f;
    const auto ids = seed(f, 2);
    const auto a = f.mgr.begin(1);
    const auto b = f.mgr.begin(1);
    f.mgr.add(a, statements::replace("acct", ids[0],
                                     counter(value_of(*f.mgr.get(a, "acct", ids[0])) + 1)));
    f.mgr.add(b, statements::replace("acct", ids[1],
                                     counter(value_of(*f.mgr.get(b, "acct", ids[1])) + 1)));
    BOOST_TEST(f.commit(a).committed);
    BOOST_TEST(f.commit(b).committed);
}

BOOST_AUTO_TEST_CASE(a_scan_conflicts_with_an_insert_into_the_table) {
    fixture f;
    const auto ids = seed(f, 3);
    BOOST_REQUIRE(f.run({statements::create_table("other")}).committed);
    const auto tok = f.mgr.begin(1);
    const auto page = f.mgr.scan(tok, "acct");
    f.mgr.insert(tok, "other", counter(static_cast<std::int64_t>(page.documents.size())));
    // A write to another table does not touch the scan.
    BOOST_REQUIRE(f.run({statements::insert("other", new_id(), counter(0))}).committed);
    const auto tok2 = f.mgr.begin(1);
    (void)f.mgr.scan(tok2, "acct");
    f.mgr.insert(tok2, "other", counter(1));
    BOOST_TEST(f.commit(tok2).committed);
    // A phantom: a new row in the scanned table.
    BOOST_REQUIRE(f.run({statements::insert("acct", new_id(), counter(0))}).committed);
    BOOST_TEST(f.commit(tok).code == "occ_conflict");
}

BOOST_AUTO_TEST_CASE(reading_a_missing_document_conflicts_with_its_creation) {
    fixture f;
    seed(f, 1);
    const auto id = new_id();
    const auto tok = f.mgr.begin(1);
    BOOST_TEST(!f.mgr.get(tok, "acct", id).has_value());
    f.mgr.add(tok, statements::create_table("log"));
    BOOST_REQUIRE(f.run({statements::insert("acct", id, counter(1))}).committed);
    BOOST_TEST(f.commit(tok).code == "occ_conflict");
}

BOOST_AUTO_TEST_CASE(an_index_key_changed_after_the_snapshot_conflicts) {
    fixture f;
    seed(f, 1);
    read_set reads;
    reads.blocks = f.sm.block_count();
    const auto h = ion_value::string("blue").hash();
    reads.indexes.push_back({"acct", "color", h});
    auto p = f.sm.prepare(new_id(), f.clock, {statements::create_table("x")}, reads);
    BOOST_TEST(p.entry.has_value());
    // Task 6 writes these markers; set one by hand at the current block.
    f.engine.write(kythira::write_batch{}.put(
        column_family::data, "l/bank/ichg/acct/color/" + to_hex(h), std::to_string(reads.blocks)));
    BOOST_TEST(f.apply(p).code == "occ_conflict");
}

BOOST_AUTO_TEST_CASE(a_read_set_from_the_future_is_invalid) {
    fixture f;
    seed(f, 1);
    read_set reads;
    reads.blocks = 99;
    reads.tables.push_back("acct");
    const auto p = f.sm.prepare(new_id(), f.clock, {statements::create_table("x")}, reads);
    BOOST_TEST(p.rejection.code == "invalid_entry");
}

BOOST_AUTO_TEST_CASE(the_read_set_round_trips_in_the_entry) {
    fixture f;
    const auto ids = seed(f, 1);
    const auto tok = f.mgr.begin(1);
    (void)f.mgr.get(tok, "acct", ids[0]);
    (void)f.mgr.get(tok, "acct", ids[0]);  // recorded once
    (void)f.mgr.scan(tok, "acct");
    f.mgr.add(tok, statements::create_table("x"));
    const auto p = f.mgr.commit(tok, t0 + 50);
    BOOST_REQUIRE(p.entry.has_value());
    std::vector<std::uint8_t> bytes(p.command.size());
    std::memcpy(bytes.data(), p.command.data(), p.command.size());
    const auto e = decode_entry(bytes);
    BOOST_REQUIRE(e.reads.has_value());
    BOOST_TEST(e.reads->blocks == 1U);
    BOOST_REQUIRE(e.reads->documents.size() == 1U);
    BOOST_TEST(e.reads->documents[0].version == 0);
    BOOST_TEST(e.reads->tables.size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(limits_and_sessions)

BOOST_AUTO_TEST_CASE(statement_limits_hold_in_the_session_and_in_apply) {
    fixture f({}, session_limits{.max_changed_documents = 3});
    seed(f, 1);
    const auto tok = f.mgr.begin(1);
    for (int i = 0; i < 3; ++i) {
        f.mgr.insert(tok, "acct", counter(i));
    }
    try {
        f.mgr.insert(tok, "acct", counter(4));
        BOOST_FAIL("expected transaction_too_large");
    } catch (const ledger_session_error& e) {
        BOOST_TEST(e.code() == "transaction_too_large");
        BOOST_TEST(!e.retryable());
    }
    BOOST_TEST(f.commit(tok).committed);

    // apply enforces its own limits whatever the leader sent.
    fixture g(ledger_options{.max_changed_documents = 2, .max_statement_bytes = 4096});
    seed(g, 0);
    std::vector<ion_value> three;
    for (int i = 0; i < 3; ++i) {
        three.push_back(statements::insert("acct", new_id(), counter(i)));
    }
    BOOST_TEST(g.sm.prepare(new_id(), t0, three).rejection.code == "transaction_too_large");
    const auto big =
        statements::insert("acct", new_id(), doc("{s: \"" + std::string(5000, 'x') + "\"}"));
    BOOST_TEST(g.sm.prepare(new_id(), t0, {big}).rejection.code == "transaction_too_large");
}

BOOST_AUTO_TEST_CASE(a_leader_change_fails_open_sessions_with_a_retryable_error) {
    fixture f;
    const auto ids = seed(f, 1);
    const auto tok = f.mgr.begin(1);
    f.mgr.leadership_changed(2);
    try {
        (void)f.mgr.get(tok, "acct", ids[0]);
        BOOST_FAIL("expected session_expired");
    } catch (const ledger_session_error& e) {
        BOOST_TEST(e.code() == "session_expired");
        BOOST_TEST(e.retryable());
    }
    BOOST_CHECK_THROW((void)f.mgr.begin(1), ledger_session_error);  // an older term
    const auto tok2 = f.mgr.begin(2);
    BOOST_TEST(f.mgr.get(tok2, "acct", ids[0]).has_value());
    // A token from term 2 dies when term 3 begins.
    (void)f.mgr.begin(3);
    BOOST_CHECK_THROW((void)f.mgr.get(tok2, "acct", ids[0]), ledger_session_error);
}

BOOST_AUTO_TEST_CASE(a_session_expires_after_its_timeout) {
    fixture f({}, session_limits{.timeout = std::chrono::seconds{30}});
    seed(f, 1);
    const auto tok = f.mgr.begin(1);
    f.now += std::chrono::seconds{29};
    BOOST_TEST(f.mgr.open_sessions() == 1U);
    f.now += std::chrono::seconds{2};
    BOOST_TEST(f.mgr.open_sessions() == 0U);
    BOOST_CHECK_THROW((void)f.mgr.commit(tok, t0), ledger_session_error);
}

BOOST_AUTO_TEST_CASE(single_statement_writes_are_one_call_transactions) {
    fixture f;
    const auto ids = seed(f, 1);
    BOOST_TEST(f.run({statements::replace("acct", ids[0], counter(7))}).committed);
    BOOST_TEST(f.mgr.open_sessions() == 0U);
    BOOST_CHECK_THROW((void)f.mgr.commit(f.mgr.begin(1), t0), ledger_session_error);  // empty
}

BOOST_AUTO_TEST_CASE(the_disk_floor_stops_transactions_until_space_returns) {
    fixture f;
    const auto ids = seed(f, 1);
    std::uint64_t free = 10'000;
    disk_guard guard;
    guard.path = "/data";
    guard.floor_bytes = 5'000;
    guard.free_bytes = [&](const std::filesystem::path&) { return free; };
    f.mgr.set_disk_guard(guard);

    const auto tok = f.mgr.begin(1);
    f.mgr.add(tok, statements::replace("acct", ids[0], counter(1)));
    free = 4'999;
    try {
        (void)f.mgr.commit(tok, f.clock);
        BOOST_FAIL("commit below the floor must throw");
    } catch (const ledger_session_error& e) {
        BOOST_TEST(e.code() == "insufficient_storage");
        BOOST_TEST(!e.retryable());
    }
    BOOST_CHECK_THROW((void)f.mgr.begin(1), ledger_session_error);
    BOOST_CHECK_THROW(f.run({statements::replace("acct", ids[0], counter(2))}),
                      ledger_session_error);
    // Reads of the applied state are not affected.
    BOOST_TEST(f.sm.get_document("acct", ids[0]).has_value());

    free = 5'000;
    BOOST_TEST(f.run({statements::replace("acct", ids[0], counter(3))}).committed);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// Requirement 12.3: serializability. Random interleaved sessions read
// counters and scan the table, then write values computed from what they
// read. Transactions apply one at a time in commit order; just before each
// applies, its reads are compared with the state it would serialize after.
// A committed transaction must have read exactly that state (so commit
// order is a serial order, phantoms included); an occ_conflict must have
// had at least one read that no longer holds (no false conflicts beyond a
// rewrite of a read document).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_SUITE(serializability)

namespace {

struct open_txn {
    std::string token;
    std::map<std::string, std::int64_t> versions;  // id -> version read (-1 none)
    std::optional<std::set<std::pair<std::string, std::int64_t>>> scanned;
    std::int64_t sum = 0;
};

auto current_scan(const ledger_sm& sm) -> std::set<std::pair<std::string, std::int64_t>> {
    std::set<std::pair<std::string, std::int64_t>> out;
    for (const auto& r : sm.scan_table_at("acct", sm.block_count(), std::nullopt, 1000).documents) {
        out.emplace(r.id, r.version);
    }
    return out;
}

auto reads_still_hold(const ledger_sm& sm, const open_txn& t) -> bool {
    for (const auto& [id, version] : t.versions) {
        const auto cur = sm.get_document("acct", id);
        if ((cur ? cur->version : std::int64_t{-1}) != version) {
            return false;
        }
    }
    return !t.scanned || *t.scanned == current_scan(sm);
}

}  // namespace

BOOST_AUTO_TEST_CASE(committed_transactions_are_serializable_in_commit_order) {
    for (const unsigned seed_value : {1U, 2U, 3U, 4U, 5U}) {
        fixture f;
        auto ids = seed(f, 5);
        std::mt19937 rng(seed_value);
        const auto pick = [&](std::size_t n) {
            return std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
        };
        std::vector<open_txn> open;
        std::size_t committed = 0;
        std::size_t conflicts = 0;
        for (int step = 0; step < 600; ++step) {
            const auto action = pick(10);
            if (open.size() < 4 && (open.empty() || action < 3)) {
                open.push_back({f.mgr.begin(1), {}, std::nullopt, 0});
                continue;
            }
            auto& t = open[pick(open.size())];
            if (action < 6) {  // read a document
                const auto& id = ids[pick(ids.size())];
                const auto r = f.mgr.get(t.token, "acct", id);
                t.versions.emplace(id, r ? r->version : -1);
                t.sum += r ? value_of(*r) : 0;
            } else if (action < 7) {  // scan the table
                std::set<std::pair<std::string, std::int64_t>> seen;
                for (const auto& r : f.mgr.scan(t.token, "acct", std::nullopt, 1000).documents) {
                    seen.emplace(r.id, r.version);
                    t.sum += value_of(r);
                }
                t.scanned = seen;
            } else if (action < 8 && pick(4) == 0) {  // a blind write elsewhere
                const auto& id = ids[pick(ids.size())];
                const auto r = f.run({statements::replace("acct", id, counter(1000))});
                BOOST_TEST((r.committed || r.code == "document_not_found"));
            } else {  // write from the reads, then commit
                if (t.versions.empty() && !t.scanned) {
                    continue;
                }
                const auto& target = ids[pick(ids.size())];
                if (pick(5) == 0) {
                    const auto id = f.mgr.insert(t.token, "acct", counter(t.sum));
                    ids.push_back(id);
                } else if (f.sm.get_document("acct", target)) {
                    f.mgr.add(t.token, statements::replace("acct", target, counter(t.sum + 1)));
                } else {
                    f.mgr.insert(t.token, "acct", counter(t.sum));
                }
                const bool hold = reads_still_hold(f.sm, t);
                const auto r = f.commit(t.token);
                if (r.committed) {
                    ++committed;
                    BOOST_TEST(hold, "seed " << seed_value << " step " << step
                                             << ": committed on reads that no longer held");
                } else {
                    BOOST_TEST(r.code == "occ_conflict");
                    ++conflicts;
                    BOOST_TEST(!hold, "seed " << seed_value << " step " << step
                                              << ": conflict on reads that still held");
                }
                open.erase(open.begin() + (&t - open.data()));
            }
        }
        BOOST_TEST(committed > 20U);
        BOOST_TEST(conflicts > 5U);
        // The journal still verifies end to end.
        const auto d = f.sm.get_digest();
        for (std::uint64_t s = 0; s <= d->tip; ++s) {
            const auto p = f.sm.get_block_proof(s, d->tip);
            BOOST_TEST(verify_proof(p.blk.block_hash, p.proof, d->digest));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
