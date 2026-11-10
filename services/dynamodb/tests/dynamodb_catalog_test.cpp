// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The catalog state machine and the disk guard
// (`.kiro/specs/dynamodb-compatible-api/` task 6, Requirements 7.2-7.4,
// 20.2): linearizable descriptors in the reserved range, status
// transitions, never-reused table ids, a background delete driven by
// bounded steps, restart and checkpoints, and writes refused below the
// disk floor.

#define BOOST_TEST_MODULE dynamodb_catalog_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/item_state_machine.hpp>
#include <kythira/dynamodb/key_layout.hpp>

#include <raft/memory_kv_engine.hpp>

#include <boost/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace ddb = kythira::dynamodb;
using kythira::memory_kv_engine;

namespace {

using catalog_sm = ddb::dynamo_catalog_state_machine<>;
using item_sm = ddb::dynamo_item_state_machine<>;

auto schema() -> ddb::key_schema {
    ddb::key_schema s;
    s.partition_name = "pk";
    s.partition_type = ddb::attribute_type::S;
    return s;
}

auto bytes_of(const std::string& s) -> std::vector<std::byte> {
    std::vector<std::byte> out(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out[i] = static_cast<std::byte>(s[i]);
    }
    return out;
}

struct fixture {
    memory_kv_engine engine;
    catalog_sm catalog;
    std::uint64_t index = 0;

    fixture() { catalog.open(engine, ddb::catalog_range()); }

    auto run(const ddb::catalog_command& c) -> ddb::catalog_result {
        const auto out = catalog.apply(bytes_of(ddb::encode_catalog_command(c)), ++index);
        auto r = ddb::decode_catalog_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(r.has_value());
        return *r;
    }

    auto create(const std::string& name, std::uint64_t now = 1000) -> ddb::catalog_result {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::create_table;
        c.table.name = name;
        c.table.schema = schema();
        c.table.read_capacity = 5;
        c.table.write_capacity = 5;
        c.table.tags = {{"team", "storage"}};
        c.now_ms = now;
        return run(c);
    }

    auto set_status(const std::string& name, std::uint64_t id, ddb::table_status from,
                    ddb::table_status to) -> ddb::catalog_result {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::set_status;
        c.table.name = name;
        c.table.table_id = id;
        c.from = from;
        c.to = to;
        return run(c);
    }

    auto activate(const std::string& name) -> std::uint64_t {
        const auto created = create(name);
        BOOST_REQUIRE(created.ok());
        const auto id = created.table->table_id;
        BOOST_REQUIRE(
            set_status(name, id, ddb::table_status::creating, ddb::table_status::active).ok());
        return id;
    }

    auto update(const std::string& name, std::uint8_t fields, const ddb::table_descriptor& with)
        -> ddb::catalog_result {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::update_table;
        c.table = with;
        c.table.name = name;
        c.update_fields = fields;
        return run(c);
    }

    auto remove(const std::string& name) -> ddb::catalog_result {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::delete_table;
        c.table.name = name;
        return run(c);
    }

    auto progress(std::uint64_t id, std::uint64_t deleted, bool done) -> ddb::catalog_result {
        ddb::catalog_command c;
        c.op = ddb::catalog_command::kind::job_progress;
        c.table.table_id = id;
        c.deleted = deleted;
        c.done = done;
        return run(c);
    }
};

auto type_is(const ddb::catalog_result& r, const char* name) -> bool {
    return r.error_type.ends_with(std::string("#") + name);
}

}  // namespace

BOOST_AUTO_TEST_CASE(create_writes_creating_then_the_leader_activates) {
    fixture f;
    const auto r = f.create("orders", 1234);
    BOOST_REQUIRE(r.ok());
    BOOST_REQUIRE(r.table.has_value());
    BOOST_TEST(r.table->table_id == 1U);
    BOOST_TEST((r.table->status == ddb::table_status::creating));
    BOOST_TEST(r.table->created_ms == 1234U);
    BOOST_TEST(r.table->revision == 1U);
    BOOST_TEST((f.catalog.describe("orders") == r.table));

    const auto again = f.create("orders");
    BOOST_TEST(type_is(again, "ResourceInUseException"));
    BOOST_TEST(again.error_message == "Table already exists: orders");

    const auto active =
        f.set_status("orders", 1, ddb::table_status::creating, ddb::table_status::active);
    BOOST_TEST((active.table->status == ddb::table_status::active));
    BOOST_TEST(active.table->revision == 2U);
    // The same transition again, or one naming another id, changes nothing.
    const auto stale =
        f.set_status("orders", 1, ddb::table_status::creating, ddb::table_status::active);
    BOOST_TEST(stale.table->revision == 2U);
    BOOST_TEST(
        type_is(f.set_status("orders", 9, ddb::table_status::active, ddb::table_status::updating),
                "ResourceNotFoundException"));
    BOOST_TEST(
        type_is(f.set_status("nope", 1, ddb::table_status::creating, ddb::table_status::active),
                "ResourceNotFoundException"));
}

BOOST_AUTO_TEST_CASE(update_table_moves_through_updating) {
    fixture f;
    f.activate("orders");
    ddb::table_descriptor with;
    with.read_capacity = 10;
    with.write_capacity = 20;
    auto r = f.update("orders", ddb::catalog_command::update_throughput, with);
    BOOST_REQUIRE(r.ok());
    BOOST_TEST((r.table->status == ddb::table_status::updating));
    BOOST_TEST(r.table->read_capacity == 10U);

    // Not while it is still updating.
    r = f.update("orders", ddb::catalog_command::update_throughput, with);
    BOOST_TEST(type_is(r, "ResourceInUseException"));
    BOOST_TEST(r.error_message ==
               "Attempt to change a resource which is still in use: Table is being updated: "
               "orders");
    (void)f.set_status("orders", 1, ddb::table_status::updating, ddb::table_status::active);

    // Deletion protection needs no work: the table stays ACTIVE.
    with.deletion_protection = true;
    r = f.update("orders", ddb::catalog_command::update_deletion_protection, with);
    BOOST_TEST((r.table->status == ddb::table_status::active));
    BOOST_TEST(r.table->deletion_protection);

    // On-demand drops provisioned throughput.
    with.billing = ddb::billing_mode::pay_per_request;
    r = f.update("orders", ddb::catalog_command::update_billing, with);
    BOOST_TEST((r.table->billing == ddb::billing_mode::pay_per_request));
    BOOST_TEST(r.table->read_capacity == 0U);

    BOOST_TEST(type_is(f.update("nope", 0, with), "ResourceNotFoundException"));
}

BOOST_AUTO_TEST_CASE(deletion_protection_refuses_delete) {
    fixture f;
    f.activate("orders");
    ddb::table_descriptor with;
    with.deletion_protection = true;
    (void)f.update("orders", ddb::catalog_command::update_deletion_protection, with);
    const auto r = f.remove("orders");
    BOOST_TEST(r.error_type.ends_with("ValidationException"));
    BOOST_TEST(r.error_message ==
               "Resource cannot be deleted as it is currently protected against deletion. "
               "Disable deletion protection first.");
    BOOST_TEST((f.catalog.describe("orders")->status == ddb::table_status::active));
}

BOOST_AUTO_TEST_CASE(delete_runs_in_the_background_and_ids_are_never_reused) {
    fixture f;
    const auto id = f.activate("orders");

    // Items of the table in an item group sharing the engine.
    item_sm items;
    items.open(f.engine, ddb::table_range(id));
    for (std::uint64_t i = 0; i < 7; ++i) {
        ddb::item_command put;
        put.table_id = id;
        put.schema = schema();
        ddb::attribute_value pk;
        pk.type = ddb::attribute_type::S;
        pk.str = "k" + std::to_string(i);
        put.payload = {{"pk", pk}};
        (void)items.apply(bytes_of(ddb::encode_item_command(put)), i + 1);
    }

    auto r = f.remove("orders");
    BOOST_REQUIRE(r.ok());
    BOOST_TEST((r.table->status == ddb::table_status::deleting));
    BOOST_TEST(type_is(f.remove("orders"), "ResourceInUseException"));
    BOOST_TEST(type_is(f.create("orders"), "ResourceInUseException"));

    // The leader drives the job: bounded drop steps on the item group, each
    // recorded as progress.
    auto jobs = f.catalog.delete_jobs();
    BOOST_REQUIRE(jobs.size() == 1U);
    BOOST_TEST(jobs[0].table_id == id);
    std::uint64_t index = 100;
    for (;;) {
        const auto out = items.apply(
            bytes_of(ddb::encode_item_command(ddb::drop_step_for(jobs[0], 3))), ++index);
        const auto step = ddb::decode_item_result(
            std::string_view(reinterpret_cast<const char*>(out.data()), out.size()));
        BOOST_REQUIRE(step.has_value());
        BOOST_TEST(step->deleted <= 3U);
        (void)f.progress(id, step->deleted, !step->more);
        if (!step->more) {
            break;
        }
        // A new leader would read the job back and carry on.
        jobs = f.catalog.delete_jobs();
        BOOST_REQUIRE(jobs.size() == 1U);
    }
    BOOST_TEST(f.catalog.delete_jobs().empty());
    BOOST_TEST(!f.catalog.describe("orders").has_value());
    BOOST_TEST(f.engine.key_count(kythira::column_family::data) == 1U);  // next-table-id
    // A repeated last step is harmless.
    BOOST_TEST(f.progress(id, 0, true).ok());

    // The name is free again, with a new id.
    const auto recreated = f.create("orders");
    BOOST_REQUIRE(recreated.ok());
    BOOST_TEST(recreated.table->table_id == id + 1);
    BOOST_TEST(f.catalog.next_table_id() == id + 2);
}

BOOST_AUTO_TEST_CASE(delete_only_from_active) {
    fixture f;
    (void)f.create("orders");
    const auto r = f.remove("orders");
    BOOST_TEST(type_is(r, "ResourceInUseException"));
    BOOST_TEST(r.error_message ==
               "Attempt to change a resource which is still in use: Table is being created: "
               "orders");
    BOOST_TEST(type_is(f.remove("nope"), "ResourceNotFoundException"));
    BOOST_TEST(f.remove("nope").error_message ==
               "Requested resource not found: Table: nope not found");
}

BOOST_AUTO_TEST_CASE(list_tables_pages_in_name_order) {
    fixture f;
    for (const char* name : {"c", "a", "b", "d", "a.b"}) {
        (void)f.create(name);
    }
    BOOST_TEST(
        (f.catalog.list_tables("", 100) == std::vector<std::string>{"a", "a.b", "b", "c", "d"}));
    BOOST_TEST((f.catalog.list_tables("", 2) == std::vector<std::string>{"a", "a.b"}));
    BOOST_TEST((f.catalog.list_tables("a.b", 2) == std::vector<std::string>{"b", "c"}));
    BOOST_TEST(f.catalog.list_tables("d", 2).empty());
}

BOOST_AUTO_TEST_CASE(restart_and_checkpoint_keep_the_catalog) {
    fixture f;
    f.activate("orders");
    (void)f.create("users");

    catalog_sm reopened;
    reopened.open(f.engine, ddb::catalog_range());
    BOOST_TEST(reopened.applied_index() == f.catalog.applied_index());
    BOOST_TEST(reopened.describe("users").has_value());

    const auto dir = std::filesystem::temp_directory_path() /
                     ("ddb-catalog-" + std::to_string(std::random_device{}()));
    const auto manifest = f.catalog.checkpoint(dir);
    memory_kv_engine other;
    catalog_sm follower;
    follower.open(other, ddb::catalog_range());
    follower.ingest(dir, manifest, f.catalog.applied_index());
    std::filesystem::remove_all(dir);
    BOOST_TEST((follower.describe("orders") == f.catalog.describe("orders")));
    BOOST_TEST(follower.next_table_id() == 3U);

    // A replayed entry is a no-op.
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::create_table;
    c.table.name = "replayed";
    c.table.schema = schema();
    BOOST_TEST(f.catalog.apply(bytes_of(ddb::encode_catalog_command(c)), 1).empty());
    BOOST_TEST(!f.catalog.describe("replayed").has_value());

    BOOST_CHECK_THROW(reopened.open(f.engine, ddb::table_range(1)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(forms_round_trip_and_bad_entries_stop_the_group) {
    ddb::table_descriptor d;
    d.name = "orders";
    d.table_id = 42;
    d.schema = schema();
    d.schema.sort_name = "sk";
    d.schema.sort_type = ddb::attribute_type::B;
    d.status = ddb::table_status::deleting;
    d.created_ms = 1700000000000ULL;
    d.billing = ddb::billing_mode::pay_per_request;
    d.storage_class = ddb::table_class::standard_infrequent_access;
    d.deletion_protection = true;
    d.warm_read = 20000;
    d.warm_write = 9000;
    d.tags = {{"a", "1"}, {"b", ""}};
    d.revision = 7;
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST((ddb::decode_table_descriptor(bytes) == d));
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        BOOST_TEST(!ddb::decode_table_descriptor(bytes.substr(0, n)).has_value());
    }
    BOOST_TEST(!ddb::decode_table_descriptor(bytes + '\0').has_value());

    const ddb::delete_job job{42, "orders", d.schema, 99};
    BOOST_TEST((ddb::decode_delete_job(ddb::encode_delete_job(job)) == job));

    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::update_table;
    c.table = d;
    c.update_fields = ddb::catalog_command::update_billing | ddb::catalog_command::update_class;
    c.from = ddb::table_status::updating;
    c.now_ms = 5;
    c.deleted = 6;
    c.done = true;
    const auto cb = ddb::encode_catalog_command(c);
    bool newer = false;
    BOOST_TEST((ddb::decode_catalog_command(cb, newer) == c));
    for (std::size_t n = 0; n < cb.size(); ++n) {
        BOOST_TEST(!ddb::decode_catalog_command(cb.substr(0, n), newer).has_value());
        BOOST_TEST(!newer);
    }

    ddb::catalog_result r;
    r.error_type = "x";
    r.error_message = "y";
    r.table = d;
    BOOST_TEST((ddb::decode_catalog_result(ddb::encode_catalog_result(r)) == r));

    fixture f;
    auto future = cb;
    future[0] = static_cast<char>(2);
    BOOST_TEST(!ddb::decode_catalog_command(future, newer).has_value());
    BOOST_TEST(newer);
    BOOST_CHECK_THROW((void)f.catalog.apply(bytes_of(future), 1), std::runtime_error);
    BOOST_CHECK_THROW((void)f.catalog.apply(bytes_of("\x01"), 1), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(the_disk_guard_refuses_writes_below_the_floor) {
    std::optional<std::uintmax_t> free = 2048;
    const ddb::disk_guard guard("/data", 1024, [&](const std::filesystem::path&) { return free; });
    BOOST_TEST(!guard.check_write().has_value());
    free = 1024;
    BOOST_TEST(!guard.check_write().has_value());
    free = 1023;
    auto e = guard.check_write();
    BOOST_REQUIRE(e.has_value());
    BOOST_TEST(e->status == 500U);
    BOOST_TEST(e->type.ends_with("InternalServerError"));
    BOOST_TEST(e->message->find("disk guard") != std::string::npos);
    BOOST_TEST(e->message->find("1023") != std::string::npos);
    // Unknown free space fails closed.
    free.reset();
    BOOST_TEST(guard.check_write().has_value());

    // The real probe reads a directory that exists.
    const ddb::disk_guard real(std::filesystem::temp_directory_path(), 0);
    BOOST_TEST(!real.check_write().has_value());
    const ddb::disk_guard missing("/no/such/kythira/dir", 0);
    BOOST_TEST(missing.check_write().has_value());
}
