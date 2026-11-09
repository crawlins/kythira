// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger node's group state machine (.kiro/specs/verifiable-ledger/
// task 7c): it opens as the catalog or as a ledger by range, every copy
// shares one state machine, the node hears of each open and drop, and
// splits and merges are vetoed and counted.

#define BOOST_TEST_MODULE ledger_group_state_machine_test
#include <boost/test/unit_test.hpp>

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_group_state_machine.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/persistent_state_machine.hpp>
#include <raft/splittable_state_machine.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace kythira::ledger;
using kythira::memory_kv_engine;

using group_sm = basic_ledger_group_state_machine<memory_kv_engine>;

static_assert(kythira::persistent_state_machine<group_sm>);
static_assert(kythira::persistent_splittable_state_machine<group_sm>);
static_assert(kythira::split_policy_state_machine<group_sm, std::string>);
static_assert(kythira::storage_kind_of<group_sm> == kythira::state_machine_storage::persistent);

namespace {

constexpr std::int64_t t0 = 1'762'689'600'000;  // 2025-11-09T12:00:00.000Z

}  // namespace

BOOST_AUTO_TEST_CASE(ledger_ids_come_only_from_ledger_ranges) {
    BOOST_TEST(ledger_id_of_range(ledger_range("abc")).value_or("") == "abc");
    BOOST_TEST(!ledger_id_of_range(catalog_range()).has_value());
    BOOST_TEST(!ledger_id_of_range({"l/abc/", std::nullopt}).has_value());
    BOOST_TEST(!ledger_id_of_range({"l/a/b/", "l/a/b0"}).has_value());
    BOOST_TEST(!ledger_id_of_range({"l//", "l/0"}).has_value());
}

BOOST_AUTO_TEST_CASE(opens_as_the_catalog_or_a_ledger_by_range) {
    memory_kv_engine engine;
    auto ctx = std::make_shared<group_sm::context>();
    std::vector<std::string> opened_ranges;
    std::vector<std::string> option_ids;
    ctx->options = [&](const std::string& id) {
        option_ids.push_back(id);
        return ledger_options{};
    };
    ctx->on_open = [&](const kythira::key_range& r, const group_sm::opened&) {
        opened_ranges.push_back(r.start);
    };

    group_sm catalog(ctx);
    catalog.open(engine, catalog_range());
    BOOST_REQUIRE(catalog.get().has_value());
    BOOST_TEST(std::holds_alternative<std::shared_ptr<group_sm::catalog_type>>(*catalog.get()));

    group_sm ledger(ctx);
    ledger.open(engine, ledger_range("L1"));
    BOOST_REQUIRE(ledger.get().has_value());
    BOOST_TEST(std::holds_alternative<std::shared_ptr<group_sm::ledger_type>>(*ledger.get()));
    BOOST_TEST((ledger.range() == ledger_range("L1")));

    BOOST_TEST(opened_ranges == (std::vector<std::string>{"c/catalog/", "l/L1/"}));
    BOOST_TEST(option_ids == std::vector<std::string>{"L1"});

    group_sm stray(ctx);
    BOOST_CHECK_THROW(stray.open(engine, {"x/", "x0"}), std::invalid_argument);
    BOOST_CHECK_THROW((void)stray.applied_index(), std::logic_error);
}

BOOST_AUTO_TEST_CASE(applies_through_the_opened_state_machine_and_copies_share_it) {
    memory_kv_engine engine;
    group_sm catalog;
    group_sm copy = catalog;  // as multi_raft moves the factory's value
    copy.open(engine, catalog_range());

    const auto result = catalog_result::decode(
        catalog.apply(catalog_entries::create("books", new_id(), true, t0), 1));
    BOOST_TEST(result.ok(), result.code);
    BOOST_TEST(copy.applied_index() == 1U);
    BOOST_TEST(catalog.applied_index() == 1U);
    const auto& c = std::get<std::shared_ptr<group_sm::catalog_type>>(*catalog.get());
    BOOST_TEST(c->get("books").has_value());

    group_sm ledger;
    ledger.open(engine, ledger_range("L1"));
    const auto& l = std::get<std::shared_ptr<group_sm::ledger_type>>(*ledger.get());
    const auto p =
        l->prepare(new_id(), t0 + 1000, {ion_value::parse(R"({op: create_table, table: "acct"})")});
    BOOST_REQUIRE(p.entry.has_value());
    const auto applied = apply_result::decode(ledger.apply(p.command, 1));
    BOOST_TEST(applied.committed);
    BOOST_TEST(ledger.applied_index() == 1U);

    // A restart finds both where they were.
    group_sm catalog2;
    catalog2.open(engine, catalog_range());
    BOOST_TEST(catalog2.applied_index() == 1U);
    group_sm ledger2;
    ledger2.open(engine, ledger_range("L1"));
    BOOST_TEST(ledger2.applied_index() == 1U);
}

BOOST_AUTO_TEST_CASE(splits_and_merges_are_vetoed_and_counted) {
    memory_kv_engine engine;
    auto ctx = std::make_shared<group_sm::context>();
    group_sm ledger(ctx);
    ledger.open(engine, ledger_range("L1"));
    group_sm catalog(ctx);
    catalog.open(engine, catalog_range());

    BOOST_TEST(ledger.approximate_size_bytes() == 0U);
    BOOST_TEST(ledger.approximate_key_count() == 0U);
    BOOST_TEST(ledger.suggest_split_keys(4).empty());
    BOOST_TEST(!ledger.can_split_at("l/L1/m"));
    BOOST_CHECK_THROW(ledger.split_at({"l/L1/m"}), ledger_split_vetoed);
    BOOST_CHECK_THROW(ledger.adopt(ledger_range("L2")), ledger_split_vetoed);
    BOOST_CHECK_THROW(catalog.split_at({"c/catalog/m"}), ledger_catalog_error);
    BOOST_TEST(ctx->vetoes.load() == 4U);
}

BOOST_AUTO_TEST_CASE(drop_clears_the_range_and_tells_the_node) {
    memory_kv_engine engine;
    auto ctx = std::make_shared<group_sm::context>();
    std::vector<std::string> dropped;
    ctx->on_drop = [&](const kythira::key_range& r) { dropped.push_back(r.start); };
    group_sm ledger(ctx);
    ledger.open(engine, ledger_range("L1"));
    const auto& l = std::get<std::shared_ptr<group_sm::ledger_type>>(*ledger.get());
    const auto p =
        l->prepare(new_id(), t0 + 1000, {ion_value::parse(R"({op: create_table, table: "acct"})")});
    BOOST_REQUIRE(p.entry.has_value());
    BOOST_TEST(apply_result::decode(ledger.apply(p.command, 1)).committed);
    BOOST_TEST(engine.key_count(kythira::column_family::data) > 0U);

    ledger.drop();
    BOOST_TEST(engine.key_count(kythira::column_family::data) == 0U);
    BOOST_TEST(dropped == std::vector<std::string>{"l/L1/"});
}

BOOST_AUTO_TEST_CASE(the_catalog_assigns_group_ids_to_placed_ledgers) {
    memory_kv_engine engine;
    group_sm catalog;
    catalog.open(engine, catalog_range());
    const auto& c = std::get<std::shared_ptr<group_sm::catalog_type>>(*catalog.get());

    auto result = catalog_result::decode(
        catalog.apply(catalog_entries::create("one", new_id(), false, t0, {"1", "2", "3"}), 1));
    BOOST_TEST(result.ok(), result.code);
    result = catalog_result::decode(
        catalog.apply(catalog_entries::create("two", new_id(), false, t0, {"2", "3", "4"}), 2));
    BOOST_TEST(result.ok(), result.code);
    // Unplaced (a single-process backend): no group.
    result = catalog_result::decode(
        catalog.apply(catalog_entries::create("three", new_id(), false, t0), 3));
    BOOST_TEST(result.ok(), result.code);
    // Repeated voters are refused, and use no group id.
    result = catalog_result::decode(
        catalog.apply(catalog_entries::create("four", new_id(), false, t0, {"1", "1"}), 4));
    BOOST_TEST(result.code == "invalid_entry");

    BOOST_TEST(c->get("one")->group.value_or(0) == first_ledger_group);
    BOOST_TEST(c->get("one")->voters == (std::vector<std::string>{"1", "2", "3"}));
    BOOST_TEST(c->get("two")->group.value_or(0) == first_ledger_group + 1);
    BOOST_TEST(!c->get("three")->group.has_value());
    BOOST_TEST(!c->get("four").has_value());

    // The counter survives a restart.
    group_sm reopened;
    reopened.open(engine, catalog_range());
    result = catalog_result::decode(
        reopened.apply(catalog_entries::create("five", new_id(), false, t0, {"1"}), 5));
    BOOST_TEST(result.ok(), result.code);
    const auto& c2 = std::get<std::shared_ptr<group_sm::catalog_type>>(*reopened.get());
    BOOST_TEST(c2->get("five")->group.value_or(0) == first_ledger_group + 2);
}
