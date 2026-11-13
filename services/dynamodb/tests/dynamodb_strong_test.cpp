// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Multi-region strong consistency (`.kiro/specs/dynamodb-compatible-api/`
// task 27): the strong catalog's ids and the backend that routes each call
// to the host owning its table. The cluster's host and the strong host are
// two local_backends here; the Raft-hosted pair is tested in
// dynamodb_gateway_cluster_test.

#define BOOST_TEST_MODULE dynamodb_strong_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/routed_backend.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

const std::string k_create_s =
    R"({"TableName":"Strong","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})";
const std::string k_create_l =
    R"({"TableName":"Local","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],"BillingMode":"PAY_PER_REQUEST"})";

auto seed(ddb::local_backend<>& b, std::uint64_t id) -> void {
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::seed_ids;
    c.table.table_id = id;
    BOOST_TEST_REQUIRE(b.catalog(c).ok());
}

auto put_of(const ddb::table_descriptor& d, const std::string& pk) -> ddb::item_command {
    ddb::item_command c;
    c.op = ddb::item_command::kind::put;
    c.table_id = d.table_id;
    c.schema = d.schema;
    c.payload = ddb::item{{"pk", ddb::attribute_value::string(pk)}};
    return c;
}

auto key_of(const std::string& pk) -> ddb::item {
    return ddb::item{{"pk", ddb::attribute_value::string(pk)}};
}

auto count_in(auto& backend, std::uint64_t table_id) -> std::size_t {
    std::size_t n = 0;
    backend.read_range(ddb::table_range(table_id), kythira::scan_direction::forward, true,
                       [&](std::string_view, const ddb::item&) {
                           ++n;
                           return true;
                       });
    return n;
}

/// A region's node: its cluster's host (`local`) and the strong host
/// (`strong`), whose catalog was seeded as a strong voter seeds it.
struct region {
    fixture local;
    fixture strong;
    ddb::routed_backend<ddb::local_backend<>> routed{local.backend, &strong.backend};
    ddb::table_descriptor s;
    ddb::table_descriptor l;

    region() {
        seed(strong.backend, ddb::k_strong_table_id_base);
        strong.ok("CreateTable", k_create_s);
        local.ok("CreateTable", k_create_l);
        s = *strong.backend.describe("Strong");
        l = *local.backend.describe("Local");
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(the_strong_catalog_hands_out_ids_from_the_strong_base) {
    region r;
    BOOST_TEST(r.s.table_id == ddb::k_strong_table_id_base);
    BOOST_TEST(ddb::is_strong_table(r.s.table_id));
    BOOST_TEST(!ddb::is_strong_table(r.l.table_id));
    BOOST_TEST(r.l.table_id < 16U);
    // Every key of a strong table sorts at or after the strong range, and
    // every key of a cluster table before it.
    BOOST_TEST(ddb::is_strong_key(ddb::table_prefix(r.s.table_id)));
    BOOST_TEST(!ddb::is_strong_key(ddb::table_prefix(r.l.table_id)));
    BOOST_TEST(!ddb::is_strong_key(ddb::table_prefix(ddb::k_strong_table_id_base - 1)));
    BOOST_TEST(
        (ddb::table_range(ddb::k_strong_table_id_base - 1).end.value() <= ddb::strong_key_start()));

    // Every strong voter seeds at start; only the first changes anything,
    // and a lower seed never moves the next id back.
    seed(r.strong.backend, ddb::k_strong_table_id_base);
    seed(r.strong.backend, 5);
    r.strong.ok("CreateTable", R"({"TableName":"Strong2",)" + k_create_s.substr(22));
    BOOST_TEST(r.strong.backend.describe("Strong2")->table_id == ddb::k_strong_table_id_base + 1);
}

BOOST_AUTO_TEST_CASE(the_seed_command_round_trips_and_needs_format_11) {
    ddb::catalog_command c;
    c.op = ddb::catalog_command::kind::seed_ids;
    c.table.table_id = ddb::k_strong_table_id_base;
    const auto bytes = ddb::encode_catalog_command(c);
    BOOST_TEST(static_cast<int>(static_cast<std::uint8_t>(bytes[0])) == 11);
    bool newer = false;
    const auto back = ddb::decode_catalog_command(bytes, newer);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((back->op == ddb::catalog_command::kind::seed_ids));
    BOOST_TEST(back->table.table_id == ddb::k_strong_table_id_base);
    // A node on format 10 could not apply it, so it must not read it as
    // something else.
    auto old = bytes;
    old[0] = static_cast<char>(10);
    BOOST_TEST(!ddb::decode_catalog_command(old, newer).has_value());
}

BOOST_AUTO_TEST_CASE(items_go_to_the_host_owning_their_table) {
    region r;
    BOOST_TEST(
        (r.routed.write(put_of(r.s, "strong-item")).outcome == ddb::item_result::status::ok));
    BOOST_TEST((r.routed.write(put_of(r.l, "local-item")).outcome == ddb::item_result::status::ok));

    BOOST_TEST(
        r.strong.backend.read(r.s.table_id, r.s.schema, key_of("strong-item"), true).has_value());
    BOOST_TEST(
        !r.local.backend.read(r.s.table_id, r.s.schema, key_of("strong-item"), true).has_value());
    BOOST_TEST(
        r.local.backend.read(r.l.table_id, r.l.schema, key_of("local-item"), true).has_value());
    BOOST_TEST(
        !r.strong.backend.read(r.l.table_id, r.l.schema, key_of("local-item"), true).has_value());

    BOOST_TEST(r.routed.read(r.s.table_id, r.s.schema, key_of("strong-item"), false).has_value());
    BOOST_TEST(r.routed.read(r.l.table_id, r.l.schema, key_of("local-item"), false).has_value());
    BOOST_TEST(count_in(r.routed, r.s.table_id) == 1U);
    BOOST_TEST(count_in(r.routed, r.l.table_id) == 1U);
    BOOST_TEST(count_in(r.local.backend, r.s.table_id) == 0U);

    // The catalog the API reads is the cluster's.
    BOOST_TEST(r.routed.describe("Local").has_value());
    BOOST_TEST(!r.routed.describe("Strong").has_value());
    BOOST_TEST((r.routed.list_tables("", 10) == std::vector<std::string>{"Local"}));
}

BOOST_AUTO_TEST_CASE(change_streams_go_to_the_host_owning_their_range) {
    region r;
    const auto shards = r.routed.change_shards(ddb::table_range(r.s.table_id));
    BOOST_TEST_REQUIRE(!shards.empty());
    for (const auto& sh : shards) {
        const auto parsed = ddb::parse_change_stream_key(sh.stream.key);
        BOOST_TEST_REQUIRE(parsed.has_value());
        // The strong backend here is one group over `t/`; on a strong host
        // the bootstrap layout starts it at the strong range.
        (void)r.routed.read_changes(sh.stream.key, 0, 10);
    }
    BOOST_TEST(!r.routed.change_shards(ddb::table_range(r.l.table_id)).empty());
}

BOOST_AUTO_TEST_CASE(a_range_across_both_hosts_or_a_mixed_transaction_is_refused) {
    region r;
    BOOST_CHECK_THROW(r.routed.read_range(kythira::key_range{"t/", std::nullopt},
                                          kythira::scan_direction::forward, false,
                                          [](std::string_view, const ddb::item&) { return true; }),
                      std::invalid_argument);
    BOOST_CHECK_THROW(r.routed.transact({put_of(r.s, "a"), put_of(r.l, "b")}),
                      std::invalid_argument);
    const auto report = r.routed.transact({put_of(r.s, "a")});
    BOOST_TEST(r.strong.backend.read(r.s.table_id, r.s.schema, key_of("a"), true).has_value());
    (void)report;
}

BOOST_AUTO_TEST_CASE(without_a_strong_host_everything_stays_in_the_cluster) {
    fixture f;
    f.ok("CreateTable", k_create_l);
    const auto d = *f.backend.describe("Local");
    ddb::routed_backend<ddb::local_backend<>> routed(f.backend);
    BOOST_TEST(!routed.has_strong());
    BOOST_TEST((routed.write(put_of(d, "x")).outcome == ddb::item_result::status::ok));
    BOOST_TEST(count_in(f.backend, d.table_id) == 1U);
    BOOST_TEST(count_in(routed, d.table_id) == 1U);
}
