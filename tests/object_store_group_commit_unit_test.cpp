// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE object_store_group_commit_unit_test
#include <boost/test/unit_test.hpp>

#include "mock_object_store.hpp"

#include <raft/group_storage.hpp>
#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Engine-level coverage for `.kiro/specs/object-store-group-commit/`: the
// object-store engine as a `barriered_persistence_engine`. Task 1 is the
// engine with write-behind off, where it answers `durability_class::
// synchronous` and must send exactly the requests it sent before it had a
// barrier (Requirement 1.2). `node`'s side, that a synchronous engine's
// follower runs still arrive as one `append_log_entries` call, is in
// raft_batched_durable_writes_test.cpp.

using namespace kythira;

namespace {

using log_entry_t = log_entry<std::uint64_t, std::uint64_t>;
using store_t = mock_object_store;
using engine_t = object_store_persistence_engine<store_t>;
using fenced_engine_t = fenced_object_store_persistence_engine<store_t>;

constexpr std::string_view k_bucket = "kythira";
constexpr std::string_view k_prefix = "raft";

auto make_entry(std::uint64_t term, std::uint64_t index) -> log_entry_t {
    return log_entry_t{term, index, std::vector<std::byte>{std::byte{0x2a}}};
}

auto make_run(std::uint64_t term, std::uint64_t first, std::uint64_t count)
    -> std::vector<log_entry_t> {
    std::vector<log_entry_t> run;
    for (std::uint64_t i = 0; i < count; ++i) {
        run.push_back(make_entry(term, first + i));
    }
    return run;
}

auto unfenced() -> object_persistence_options {
    return {};
}

auto fenced() -> object_persistence_options {
    object_persistence_options opts;
    opts.owner_id = "node-a";
    return opts;
}

auto segmented(object_persistence_options opts) -> object_persistence_options {
    opts.log = log_layout::segmented;
    opts.append_concurrency = 1;
    return opts;
}

/// The requests `drive` makes on a fresh engine, after construction.
template<typename Engine, typename Drive>
auto requests_of(const object_persistence_options& opts, Drive drive) -> std::vector<std::string> {
    const store_t store;
    Engine engine{store, std::string(k_bucket), std::string(k_prefix), opts};
    store.clear_requests();
    drive(engine);
    return store.request_log();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(object_store_group_commit_synchronous)

// Requirement 1.3 and 2.4: one type at every setting, satisfying the barrier
// concept, durable and never blocking.
BOOST_AUTO_TEST_CASE(the_engine_is_barriered_synchronous_and_durable) {
    static_assert(barriered_persistence_engine<engine_t>);
    static_assert(barriered_persistence_engine<fenced_engine_t>);
    static_assert(bulk_append_persistence_engine<engine_t>);
    static_assert(barriered_persistence_engine<group_scoped_persistence<engine_t>>);

    const store_t store;
    engine_t engine{store, std::string(k_bucket), std::string(k_prefix)};
    BOOST_TEST((engine.durability() == durability_class::synchronous));
    BOOST_TEST(describes_durable_storage(engine));
    BOOST_TEST(to_string(durability_class::synchronous) == "synchronous");
}

// Requirement 2.2: the other three classes keep their answers.
BOOST_AUTO_TEST_CASE(only_barrier_and_synchronous_describe_durable_storage) {
    struct fixed {
        using log_entry_t = ::log_entry_t;
        durability_class answer;
        auto append_log_entry_sequenced(const log_entry_t& /*entry*/) -> write_sequence {
            return 0;
        }
        auto barrier_through(write_sequence /*seq*/) -> void {}
        [[nodiscard]] auto durable_through() const -> write_sequence { return 0; }
        [[nodiscard]] auto durability() const -> durability_class { return answer; }
    };
    static_assert(barriered_persistence_engine<fixed>);
    for (const auto answer : {durability_class::none, durability_class::buffered}) {
        fixed engine{answer};
        BOOST_TEST(!describes_durable_storage(engine));
    }
    for (const auto answer : {durability_class::barrier, durability_class::synchronous}) {
        fixed engine{answer};
        BOOST_TEST(describes_durable_storage(engine));
    }
}

// Requirement 1.2: a sequenced append is the PUT a plain one sends, and the
// barrier sends nothing, under both layouts and both fencing modes.
template<typename Engine>
auto a_sequenced_append_sends_what_a_plain_one_does(const object_persistence_options& opts)
    -> void {
    const auto plain = requests_of<Engine>(opts, [](Engine& engine) {
        engine.append_log_entry(make_entry(1, 1));
        engine.append_log_entry(make_entry(1, 2));
    });
    const auto sequenced = requests_of<Engine>(opts, [](Engine& engine) {
        const auto first = engine.append_log_entry_sequenced(make_entry(1, 1));
        engine.barrier_through(first);
        const auto second = engine.append_log_entry_sequenced(make_entry(1, 2));
        engine.barrier_through(second);
        BOOST_TEST(second > first);
        BOOST_TEST(engine.durable_through() == second);
    });
    BOOST_TEST(plain.size() == 2U);
    BOOST_TEST(sequenced == plain, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_sequenced_append_sends_exactly_the_plain_appends_requests) {
    BOOST_TEST_CONTEXT("unfenced") {
        a_sequenced_append_sends_what_a_plain_one_does<engine_t>(unfenced());
    }
    BOOST_TEST_CONTEXT("fenced") {
        a_sequenced_append_sends_what_a_plain_one_does<fenced_engine_t>(fenced());
    }
    BOOST_TEST_CONTEXT("segmented, unfenced") {
        a_sequenced_append_sends_what_a_plain_one_does<engine_t>(segmented(unfenced()));
    }
    BOOST_TEST_CONTEXT("segmented, fenced") {
        a_sequenced_append_sends_what_a_plain_one_does<fenced_engine_t>(segmented(fenced()));
    }
}

// Every append advances the sequence, whichever call made it, so
// `durable_through` is a count of durable entries a caller can compare
// against: a run of N advances it by N.
BOOST_AUTO_TEST_CASE(every_append_advances_the_durable_sequence) {
    const store_t store;
    engine_t engine{store, std::string(k_bucket), std::string(k_prefix), segmented(unfenced())};
    BOOST_TEST(engine.durable_through() == 0U);
    engine.append_log_entry(make_entry(1, 1));
    BOOST_TEST(engine.durable_through() == 1U);
    engine.append_log_entries(make_run(1, 2, 4));
    BOOST_TEST(engine.durable_through() == 5U);
    BOOST_TEST(engine.append_log_entry_sequenced(make_entry(1, 6)) == 6U);
    store.clear_requests();
    engine.barrier_through(6);
    BOOST_TEST(store.request_log().empty());
}

// A failed append assigns no sequence: nothing was made durable.
BOOST_AUTO_TEST_CASE(a_failed_append_advances_nothing) {
    const store_t store;
    engine_t engine{store, std::string(k_bucket), std::string(k_prefix), segmented(unfenced())};
    engine.append_log_entry(make_entry(1, 1));
    // Two: an unconditional PUT is retried once.
    store.fail_next_puts(2);
    BOOST_CHECK_THROW(std::ignore = engine.append_log_entry_sequenced(make_entry(1, 2)),
                      std::runtime_error);
    BOOST_TEST(engine.durable_through() == 1U);
    store.fail_next_puts(2);
    BOOST_CHECK_THROW(engine.append_log_entries(make_run(1, 2, 3)), std::runtime_error);
    BOOST_TEST(engine.durable_through() == 1U);
}

// The sequence is not state: a restarted engine starts again at zero, as
// every barriered engine does — a barrier is about writes this process made.
BOOST_AUTO_TEST_CASE(a_restarted_engine_starts_its_sequence_again) {
    const store_t store;
    {
        engine_t writer{store, std::string(k_bucket), std::string(k_prefix)};
        writer.append_log_entries(make_run(1, 1, 3));
        BOOST_TEST(writer.durable_through() == 3U);
    }
    engine_t reader{store, std::string(k_bucket), std::string(k_prefix)};
    BOOST_TEST(reader.durable_through() == 0U);
    BOOST_TEST(reader.get_last_log_index() == 3U);
}

BOOST_AUTO_TEST_SUITE_END()
