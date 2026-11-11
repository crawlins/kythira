// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Global secondary indexes in the item state machine and the propagation
// plan (`.kiro/specs/dynamodb-compatible-api/` task 11, Requirements
// 11.2-11.4): index writes apply only when newer than the entry, writes on
// a table with GSIs leave pending changes that an ack ends, deletes keep
// item versions, pending changes survive restart and splits, the plan
// carries an item's entries and skips dropped indexes, a deleted table
// leaves no keys behind, and the older entry and descriptor forms still
// decode. Runs on memory_kv_engine.

#define BOOST_TEST_MODULE dynamodb_gsi_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/gsi_propagator.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ddb = kythira::dynamodb;
using kythira::column_family;
using kythira::memory_kv_engine;

namespace {

using item_sm = ddb::dynamo_item_state_machine<>;

constexpr std::uint64_t k_table = 7;
constexpr std::uint64_t k_by_g = 9;
constexpr std::uint64_t k_by_h = 10;

auto schema() -> ddb::key_schema {
    ddb::key_schema s;
    s.partition_name = "pk";
    s.partition_type = ddb::attribute_type::S;
    s.sort_name = "sk";
    s.sort_type = ddb::attribute_type::N;
    return s;
}

/// `by_g` (hash `g`, ALL) and `by_h` (hash `h`, range `sk`, KEYS_ONLY).
auto indexes() -> std::vector<ddb::global_index> {
    ddb::global_index g;
    g.name = "by_g";
    g.index_id = k_by_g;
    g.schema.partition_name = "g";
    g.schema.partition_type = ddb::attribute_type::S;
    ddb::global_index h;
    h.name = "by_h";
    h.index_id = k_by_h;
    h.schema.partition_name = "h";
    h.schema.partition_type = ddb::attribute_type::S;
    h.schema.sort_name = "sk";
    h.schema.sort_type = ddb::attribute_type::N;
    h.projection = ddb::projection_kind::keys_only;
    return {g, h};
}

auto table() -> ddb::table_descriptor {
    ddb::table_descriptor d;
    d.name = "orders";
    d.table_id = k_table;
    d.schema = schema();
    d.status = ddb::table_status::active;
    d.gsis = indexes();
    return d;
}

auto json_item(const char* text) -> ddb::item {
    auto parsed = ddb::item_from_json(boost::json::parse(text).as_object());
    BOOST_REQUIRE(parsed.has_value());
    return *parsed;
}

auto compile(const char* op, const char* input) -> ddb::expression_set {
    ddb::request_keys keys;
    keys.table = schema();
    auto compiled = ddb::compile_request(op, boost::json::parse(input).as_object(), keys);
    BOOST_REQUIRE(compiled.has_value());
    return *compiled;
}

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

auto entry_key(std::size_t index, const char* it) -> std::string {
    auto k = ddb::gsi_entry_key(schema(), indexes()[index], json_item(it));
    BOOST_REQUIRE(k.has_value());
    return *k;
}

auto pending_key(const char* key) -> std::string {
    auto k = ddb::gsi_pending_key(k_table, schema(), json_item(key));
    BOOST_REQUIRE(k.has_value());
    return *k;
}

constexpr const char* k_key = R"({"pk":{"S":"a"},"sk":{"N":"1"}})";
constexpr const char* k_item_x = R"({"pk":{"S":"a"},"sk":{"N":"1"},"g":{"S":"x"},"v":{"N":"1"}})";
constexpr const char* k_item_y = R"({"pk":{"S":"a"},"sk":{"N":"1"},"g":{"S":"y"},"h":{"S":"q"}})";

/// One group holding the table and both index key spaces (the whole key
/// space), as a single-group cluster has.
struct fixture {
    memory_kv_engine engine;
    item_sm sm;
    std::uint64_t index = 0;
    /// The table as the catalog describes it now.
    ddb::table_descriptor desc = table();

    fixture() { sm.open(engine, kythira::key_range{}); }

    auto run(const ddb::item_command& c) -> ddb::item_result {
        const auto out = sm.apply(bytes_of(ddb::encode_item_command(c)), ++index);
        auto r = ddb::decode_item_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    auto put(const char* it) -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::put;
        c.table_id = k_table;
        c.schema = schema();
        c.gsis = ddb::maintained_indexes(desc.gsis);
        c.payload = json_item(it);
        c.expressions = compile("PutItem", "{}");
        return run(c);
    }

    auto del(const char* key) -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::delete_;
        c.table_id = k_table;
        c.schema = schema();
        c.gsis = ddb::maintained_indexes(desc.gsis);
        c.payload = json_item(key);
        c.expressions = compile("DeleteItem", "{}");
        return run(c);
    }

    auto index_op(ddb::item_command::kind op, const std::string& target, std::uint64_t version,
                  const char* payload = nullptr) -> ddb::item_result {
        ddb::item_command c;
        c.op = op;
        c.target = target;
        c.version = version;
        if (payload != nullptr) {
            c.payload = json_item(payload);
        }
        return run(c);
    }

    auto ack(const std::string& target, std::uint64_t version,
             std::vector<std::string> entries = {}) -> ddb::item_result {
        ddb::item_command c;
        c.op = ddb::item_command::kind::index_ack;
        c.target = target;
        c.version = version;
        c.entries = std::move(entries);
        return run(c);
    }

    /// Carries every pending change, as the backends do.
    auto propagate() -> std::size_t {
        std::size_t carried = 0;
        while (sm.gsi_pending_count() > 0) {
            for (const auto& w : sm.gsi_pending_batch(16)) {
                const auto plan = ddb::plan_propagation(desc, w);
                for (const auto& c : plan.index_writes) {
                    (void)run(c);
                }
                (void)run(plan.ack);
                ++carried;
            }
        }
        return carried;
    }

    [[nodiscard]] auto entry(const std::string& key) const -> std::optional<std::string> {
        return engine.get(column_family::data, key);
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(index_writes_apply_only_when_newer) {
    fixture f;
    const auto key = entry_key(0, k_item_x);
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_put, key, 5, k_item_x).version == 5U);
    BOOST_TEST(f.sm.entry_version(key) == 5U);
    BOOST_TEST(f.entry(key).has_value());

    // Older puts and deletes (replays, retries, a late propagator) change
    // nothing.
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_put, key, 3, k_item_y).version == 0U);
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_delete, key, 4).version == 0U);
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_put, key, 5, k_item_y).version == 0U);
    BOOST_TEST((ddb::decode_item(*f.entry(key)) == json_item(k_item_x)));

    // A newer delete removes the entry and keeps its version, so an older
    // put cannot bring it back.
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_delete, key, 6).version == 6U);
    BOOST_TEST(!f.entry(key).has_value());
    BOOST_TEST(f.sm.entry_version(key) == 6U);
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_put, key, 5, k_item_x).version == 0U);
    BOOST_TEST(!f.entry(key).has_value());
}

BOOST_AUTO_TEST_CASE(writes_leave_pending_changes_that_an_ack_ends) {
    fixture f;
    const auto pkey = pending_key(k_key);
    BOOST_TEST(f.put(k_item_x).version == 1U);
    BOOST_TEST(f.sm.gsi_pending_count() == 1U);
    auto batch = f.sm.gsi_pending_batch(10);
    BOOST_REQUIRE(batch.size() == 1U);
    BOOST_TEST(batch[0].pending_key == pkey);
    BOOST_TEST(batch[0].pending.version == 1U);
    BOOST_TEST(batch[0].pending.table_id == k_table);
    // A new item: the indexes hold nothing for it yet.
    BOOST_TEST(batch[0].pending.old_entries.empty());
    BOOST_TEST((batch[0].current == json_item(k_item_x)));

    // A second write before propagation raises the version and keeps the
    // entries to remove.
    BOOST_TEST(f.put(k_item_y).version == 2U);
    batch = f.sm.gsi_pending_batch(10);
    BOOST_REQUIRE(batch.size() == 1U);
    BOOST_TEST(batch[0].pending.version == 2U);
    BOOST_TEST(batch[0].pending.old_entries.empty());

    // An ack for the first version keeps the record (a newer write is not
    // carried yet) and records what the indexes now hold.
    const auto x = entry_key(0, k_item_x);
    BOOST_TEST(f.index_op(ddb::item_command::kind::index_put, x, 1, k_item_x).version == 1U);
    (void)f.ack(pkey, 1, {x});
    batch = f.sm.gsi_pending_batch(10);
    BOOST_REQUIRE(batch.size() == 1U);
    BOOST_TEST((batch[0].pending.old_entries == std::vector<std::string>{x}));

    // Carrying version 2 removes the stale entry and writes both new ones.
    const auto plan = ddb::plan_propagation(table(), batch[0]);
    BOOST_TEST(plan.index_writes.size() == 3U);
    for (const auto& c : plan.index_writes) {
        BOOST_TEST(c.version == 2U);
        (void)f.run(c);
    }
    (void)f.run(plan.ack);
    BOOST_TEST(f.sm.gsi_pending_count() == 0U);
    BOOST_TEST(!f.entry(x).has_value());
    BOOST_TEST(f.entry(entry_key(0, k_item_y)).has_value());
    const auto h = f.entry(entry_key(1, k_item_y));
    BOOST_REQUIRE(h.has_value());
    // KEYS_ONLY: the index and table keys only.
    BOOST_TEST(
        (ddb::decode_item(*h) == json_item(R"({"pk":{"S":"a"},"sk":{"N":"1"},"h":{"S":"q"}})")));
    // A repeated ack finds nothing.
    (void)f.run(plan.ack);
    BOOST_TEST(f.sm.gsi_pending_count() == 0U);

    // A write that changes no entry leaves no record.
    BOOST_TEST(f.put(k_item_y).version == 3U);
    BOOST_TEST(f.sm.gsi_pending_count() == 0U);
    // One that changes a projected attribute does.
    BOOST_TEST(f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"g":{"S":"y"},"h":{"S":"q"},)"
                     R"("v":{"N":"2"}})")
                   .version == 4U);
    BOOST_TEST(f.sm.gsi_pending_count() == 1U);
    BOOST_TEST(f.propagate() == 1U);
    BOOST_TEST(f.sm.entry_version(entry_key(0, k_item_y)) == 4U);
    // by_h projects only keys and was rewritten at the same version.
    BOOST_TEST(f.sm.entry_version(entry_key(1, k_item_y)) == 4U);
}

BOOST_AUTO_TEST_CASE(deletes_keep_item_versions) {
    fixture f;
    BOOST_TEST(f.put(k_item_x).version == 1U);
    (void)f.propagate();
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key)) == 1U);
    (void)f.del(k_key);
    BOOST_TEST(f.sm.item_version(k_table, schema(), json_item(k_key)) == 2U);
    auto batch = f.sm.gsi_pending_batch(10);
    BOOST_REQUIRE(batch.size() == 1U);
    BOOST_TEST(!batch[0].current.has_value());
    const auto x = entry_key(0, k_item_x);
    BOOST_TEST((batch[0].pending.old_entries == std::vector<std::string>{x}));
    BOOST_TEST(f.propagate() == 1U);
    BOOST_TEST(!f.entry(x).has_value());
    BOOST_TEST(f.sm.entry_version(x) == 2U);

    // Put again: the version goes on from the delete, so the new entry is
    // newer than the tombstone.
    BOOST_TEST(f.put(k_item_x).version == 3U);
    BOOST_TEST(f.propagate() == 1U);
    BOOST_TEST(f.entry(x).has_value());
    BOOST_TEST(f.sm.entry_version(x) == 3U);
}

BOOST_AUTO_TEST_CASE(pending_changes_survive_restart_and_split) {
    fixture f;
    for (const char* pk : {"a", "b", "c", "d"}) {
        const auto text =
            std::string(R"({"pk":{"S":")") + pk + R"("},"sk":{"N":"1"},"g":{"S":"x"}})";
        (void)f.put(text.c_str());
    }
    BOOST_TEST(f.sm.gsi_pending_count() == 4U);

    item_sm reopened;
    reopened.open(f.engine, kythira::key_range{});
    BOOST_TEST(reopened.gsi_pending_count() == 4U);

    // Split the table at c's collection: each side keeps its own changes.
    item_sm parent;
    parent.open(f.engine, ddb::table_range(k_table));
    BOOST_TEST(parent.gsi_pending_count() == 4U);
    ddb::attribute_value c;
    c.type = ddb::attribute_type::S;
    c.str = "c";
    const auto cut = ddb::partition_prefix(k_table, c);
    const auto children = parent.split_at({cut});
    BOOST_REQUIRE(children.size() == 1U);
    item_sm child;
    child.open(f.engine, children[0]);
    BOOST_TEST(parent.gsi_pending_count() + child.gsi_pending_count() == 4U);
    for (const auto& w : child.gsi_pending_batch(10)) {
        BOOST_TEST(children[0].contains(w.pending_key));
        BOOST_TEST(children[0].contains(w.pending.base_key));
    }
    // Merged back, the parent sees them all again.
    parent.adopt(children[0]);
    BOOST_TEST(parent.gsi_pending_count() == 4U);

    // The batch wraps round from a cursor past the last change.
    const auto all = parent.gsi_pending_batch(10);
    BOOST_REQUIRE(all.size() == 4U);
    const auto from_last = parent.gsi_pending_batch(2, all.back().pending_key);
    BOOST_REQUIRE(from_last.size() == 2U);
    BOOST_TEST(from_last[0].pending_key == all.back().pending_key);
    BOOST_TEST(from_last[1].pending_key == all.front().pending_key);
}

BOOST_AUTO_TEST_CASE(the_plan_skips_dropped_indexes_and_gone_tables) {
    ddb::gsi_work w;
    w.pending_key = pending_key(k_key);
    w.pending.table_id = k_table;
    w.pending.version = 7;
    w.current = json_item(k_item_y);
    // An entry of by_g (stale), and one of an index no longer on the table.
    const auto stale = entry_key(0, k_item_x);
    auto dropped_index = indexes()[0];
    dropped_index.index_id = 99;
    const auto dropped = *ddb::gsi_entry_key(schema(), dropped_index, json_item(k_item_x));
    w.pending.old_entries = {stale, dropped, entry_key(0, k_item_y)};

    const auto plan = ddb::plan_propagation(table(), w);
    std::size_t puts = 0;
    std::vector<std::string> deletes;
    for (const auto& c : plan.index_writes) {
        BOOST_TEST(c.version == 7U);
        if (c.op == ddb::item_command::kind::index_put) {
            ++puts;
        } else {
            BOOST_TEST((c.op == ddb::item_command::kind::index_delete));
            deletes.push_back(c.target);
        }
    }
    BOOST_TEST(puts == 2U);
    BOOST_TEST((deletes == std::vector<std::string>{stale}));
    BOOST_TEST((plan.ack.op == ddb::item_command::kind::index_ack));
    BOOST_TEST(plan.ack.target == w.pending_key);
    BOOST_TEST(plan.ack.version == 7U);
    BOOST_TEST(plan.ack.entries.size() == 2U);

    // The table is gone, or another table now has its name: only the ack.
    BOOST_TEST(ddb::plan_propagation(std::nullopt, w).index_writes.empty());
    auto other = table();
    other.table_id = k_table + 100;
    const auto gone = ddb::plan_propagation(other, w);
    BOOST_TEST(gone.index_writes.empty());
    BOOST_TEST(gone.ack.version == 7U);
}

BOOST_AUTO_TEST_CASE(dropping_a_table_and_its_indexes_leaves_no_keys) {
    fixture f;
    for (int sk = 0; sk < 4; ++sk) {
        const auto text = std::string(R"({"pk":{"S":"a"},"sk":{"N":")") + std::to_string(sk) +
                          R"("},"g":{"S":"x"},"h":{"S":"y"}})";
        (void)f.put(text.c_str());
    }
    (void)f.propagate();
    // A deleted item leaves a version, an unpropagated write a record.
    (void)f.del(R"({"pk":{"S":"a"},"sk":{"N":"0"}})");
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"9"},"g":{"S":"z"}})");
    BOOST_TEST(f.sm.gsi_pending_count() == 2U);

    // The delete job of the table and one per index, as the catalog
    // writes them.
    const auto t = table();
    std::vector<ddb::delete_job> jobs{{t.table_id, t.name, t.schema, 0}};
    for (const auto& g : t.gsis) {
        jobs.push_back({g.index_id, t.name, g.schema, 0});
    }
    for (const auto& job : jobs) {
        for (;;) {
            const auto r = f.run(ddb::drop_step_for(job, 3));
            if (!r.more) {
                break;
            }
        }
    }
    BOOST_TEST(f.sm.gsi_pending_count() == 0U);
    for (const auto cf : {column_family::data, column_family::meta}) {
        std::size_t left = 0;
        f.engine.scan(cf, ddb::table_range(k_table), kythira::scan_direction::forward,
                      [&](std::string_view, std::string_view) { return ++left, true; });
        for (const auto id : {k_by_g, k_by_h}) {
            f.engine.scan(cf, ddb::table_range(id), kythira::scan_direction::forward,
                          [&](std::string_view, std::string_view) { return ++left, true; });
        }
        BOOST_TEST(left == 0U);
    }
}

BOOST_AUTO_TEST_CASE(older_forms_still_decode) {
    // A version 1 descriptor: no GSI list.
    auto d = table();
    d.gsis.clear();
    d.revision = 5;
    const auto v2 = ddb::encode_table_descriptor(d);
    BOOST_REQUIRE(v2.ends_with(std::string("\0\0\0\x05", 4)));
    const auto v1 = '\x01' + v2.substr(1, v2.size() - 5) + std::string("\0\0\x05", 3);
    BOOST_TEST((ddb::decode_table_descriptor(v1) == d));
    // And the current form round-trips with GSIs.
    const auto with = table();
    BOOST_TEST((ddb::decode_table_descriptor(ddb::encode_table_descriptor(with)) == with));

    // A version 1 entry: no GSI fields. A put still decodes; an index
    // write cannot be one.
    ddb::item_command put;
    put.op = ddb::item_command::kind::put;
    put.table_id = k_table;
    put.schema = schema();
    put.payload = json_item(k_key);
    const auto pb = ddb::encode_item_command(put);
    BOOST_REQUIRE(pb.ends_with(std::string(4, '\0')));
    const auto pv1 = '\x01' + pb.substr(1, pb.size() - 5);
    ddb::item_command back;
    BOOST_TEST((ddb::decode_item_command(pv1, back) == ddb::item_command_status::ok));
    BOOST_TEST((back == put));

    ddb::item_command idx;
    idx.op = ddb::item_command::kind::index_delete;
    const auto ib = ddb::encode_item_command(idx);
    BOOST_REQUIRE(ib.ends_with(std::string(4, '\0')));
    const auto iv1 = '\x01' + ib.substr(1, ib.size() - 5);
    BOOST_TEST((ddb::decode_item_command(iv1, back) == ddb::item_command_status::malformed));
    BOOST_TEST((ddb::decode_item_command(ib, back) == ddb::item_command_status::ok));

    // The current form round-trips GSI fields, and every cut is refused.
    ddb::item_command full = put;
    full.gsis = indexes();
    full.target = "t/x";
    full.version = 12;
    full.entries = {"e1", "e2"};
    const auto fb = ddb::encode_item_command(full);
    BOOST_TEST((ddb::decode_item_command(fb, back) == ddb::item_command_status::ok));
    BOOST_TEST((back == full));
    for (std::size_t n = 0; n < fb.size(); ++n) {
        ddb::item_command cut;
        BOOST_TEST(
            (ddb::decode_item_command(fb.substr(0, n), cut) != ddb::item_command_status::ok));
    }

    // The pending record's form.
    const ddb::gsi_pending p{k_table, "base", 3, {"o1", "o2"}};
    const auto pr = ddb::encode_gsi_pending(p);
    BOOST_TEST(ddb::is_gsi_pending_value(pr));
    BOOST_TEST((ddb::decode_gsi_pending(pr) == p));
    BOOST_TEST(!ddb::is_gsi_pending_value(std::string(8, '\xD7')));
    BOOST_TEST(!ddb::decode_gsi_pending(pr + 'x').has_value());
}

BOOST_AUTO_TEST_CASE(marks_backfill_a_new_index) {
    fixture f;
    // The table starts with by_g only.
    f.desc.gsis.pop_back();
    std::vector<std::string> keys;
    for (const char* pk : {"a", "b", "c"}) {
        const auto text = std::string(R"({"pk":{"S":")") + pk +
                          R"("},"sk":{"N":"1"},"g":{"S":"x"},"h":{"S":"q"}})";
        (void)f.put(text.c_str());
        keys.push_back(*ddb::base_item_key(k_table, schema(), json_item(text.c_str())));
    }
    (void)f.propagate();
    const auto h_of = [](const char* pk) {
        return entry_key(
            1,
            (std::string(R"({"pk":{"S":")") + pk + R"("},"sk":{"N":"1"},"h":{"S":"q"}})").c_str());
    };
    BOOST_TEST(!f.entry(h_of("a")).has_value());

    // by_h is created; one item has a write in flight (its own record).
    f.desc.gsis = indexes();
    f.desc.gsis[1].status = ddb::index_status::creating;
    (void)f.put(R"({"pk":{"S":"b"},"sk":{"N":"1"},"g":{"S":"y"},"h":{"S":"q"}})");
    BOOST_TEST(f.sm.gsi_pending_count() == 1U);

    // The marks: one per item without a record, a missing item skipped, a
    // key outside the group reported.
    auto marks = ddb::backfill_marks(f.desc, keys);
    marks.entries.push_back(
        *ddb::base_item_key(k_table, schema(), json_item(R"({"pk":{"S":"zz"},"sk":{"N":"1"}})")));
    auto r = f.run(marks);
    BOOST_TEST(r.deleted == 2U);
    BOOST_TEST(!r.more);
    BOOST_TEST(f.sm.gsi_pending_count() == 3U);
    BOOST_TEST(f.propagate() == 3U);
    for (const char* pk : {"a", "b", "c"}) {
        BOOST_TEST(f.entry(h_of(pk)).has_value(), pk);
    }
    // by_g still holds b under its new key only.
    BOOST_TEST(
        !f.entry(entry_key(0, R"({"pk":{"S":"b"},"sk":{"N":"1"},"g":{"S":"x"}})")).has_value());

    // Marking again (a retried backfill) changes no entry's version.
    const auto before = f.sm.entry_version(h_of("a"));
    (void)f.run(ddb::backfill_marks(f.desc, keys));
    (void)f.propagate();
    BOOST_TEST(f.sm.entry_version(h_of("a")) == before);

    // A group that does not hold the keys reports them.
    item_sm other;
    memory_kv_engine other_engine;
    other.open(other_engine, ddb::table_range(k_table + 1));
    auto stray = ddb::backfill_marks(f.desc, keys);
    stray.target = ddb::table_prefix(k_table + 1);
    const auto out = other.apply(bytes_of(ddb::encode_item_command(stray)), 1);
    const auto sr = ddb::decode_item_result(
        std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
    BOOST_REQUIRE(sr.has_value());
    BOOST_TEST(sr->more);
    BOOST_TEST(sr->deleted == 0U);

    // Once by_h is being deleted, writes stop maintaining it and the plan
    // leaves its entries to the delete job.
    f.desc.gsis[1].status = ddb::index_status::deleting;
    (void)f.put(R"({"pk":{"S":"a"},"sk":{"N":"1"},"g":{"S":"x"},"h":{"S":"moved"}})");
    (void)f.propagate();
    BOOST_TEST(f.entry(h_of("a")).has_value());
}

BOOST_AUTO_TEST_CASE(the_catalog_creates_activates_and_deletes_one_index_at_a_time) {
    memory_kv_engine engine;
    ddb::dynamo_catalog_state_machine<> catalog;
    catalog.open(engine, ddb::catalog_range());
    std::uint64_t index = 0;
    const auto run = [&](const ddb::catalog_command& c) {
        const auto out = catalog.apply(bytes_of(ddb::encode_catalog_command(c)), ++index);
        auto r = ddb::decode_catalog_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(r.has_value());
        return *r;
    };
    ddb::catalog_command create;
    create.op = ddb::catalog_command::kind::create_table;
    create.table = table();
    create.table.gsis.pop_back();
    auto r = run(create);
    BOOST_REQUIRE(r.ok());
    const auto id = r.table->table_id;
    ddb::catalog_command active;
    active.op = ddb::catalog_command::kind::set_status;
    active.table.name = "orders";
    active.table.table_id = id;
    BOOST_REQUIRE(run(active).ok());

    const auto update = [&](std::vector<ddb::global_index> gsis) {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::update_table;
        c.table = *catalog.describe("orders");
        c.table.gsis = std::move(gsis);
        c.update_fields = ddb::catalog_command::update_gsis;
        return run(c);
    };
    // Add by_h: CREATING, with the next key space id; the table stays
    // ACTIVE.
    const auto next = catalog.next_table_id();
    r = update(indexes());
    BOOST_REQUIRE(r.ok());
    BOOST_TEST((r.table->status == ddb::table_status::active));
    BOOST_REQUIRE(r.table->gsis.size() == 2U);
    BOOST_TEST((r.table->gsis[1].status == ddb::index_status::creating));
    BOOST_TEST(r.table->gsis[1].index_id == next);
    BOOST_TEST(catalog.next_table_id() == next + 1);
    BOOST_TEST(ddb::backfilling_index(*r.table)->name == "by_h");

    // Nothing else may be created or deleted meanwhile.
    auto third = indexes();
    third.push_back(third[0]);
    third.back().name = "by_x";
    BOOST_TEST(update(third).error_type.ends_with("LimitExceededException"));
    BOOST_TEST(update({indexes()[1]}).error_type.ends_with("LimitExceededException"));

    // The backfill ends it.
    r = run(ddb::activate_index(*catalog.describe("orders"), "by_h"));
    BOOST_REQUIRE(r.ok());
    BOOST_TEST((r.table->gsis[1].status == ddb::index_status::active));
    BOOST_TEST(ddb::backfilling_index(*r.table) == nullptr);

    // Two changes in one entry are refused; one delete is DELETING with a
    // job for its key space, and the index goes when the job is done.
    BOOST_TEST(update({}).error_type.ends_with("LimitExceededException"));
    r = update({indexes()[1]});
    BOOST_REQUIRE(r.ok());
    BOOST_TEST((r.table->gsis[0].status == ddb::index_status::deleting));
    const auto jobs = catalog.delete_jobs();
    BOOST_REQUIRE(jobs.size() == 1U);
    BOOST_TEST(jobs[0].table_id == r.table->gsis[0].index_id);
    ddb::catalog_command done;
    done.op = ddb::catalog_command::kind::job_progress;
    done.table.table_id = jobs[0].table_id;
    done.done = true;
    BOOST_REQUIRE(run(done).ok());
    const auto after = catalog.describe("orders");
    BOOST_REQUIRE(after.has_value());
    BOOST_REQUIRE(after->gsis.size() == 1U);
    BOOST_TEST(after->gsis[0].name == "by_h");
    BOOST_TEST(catalog.delete_jobs().empty());
}
