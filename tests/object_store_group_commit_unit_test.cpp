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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// Engine-level coverage for `.kiro/specs/object-store-group-commit/`: the
// object-store engine as a `barriered_persistence_engine`. Task 1 is the
// engine with write-behind off, where it answers `durability_class::
// synchronous` and must send exactly the requests it sent before it had a
// barrier (Requirement 1.2). `node`'s side, that a synchronous engine's
// follower runs still arrive as one `append_log_entries` call, is in
// raft_batched_durable_writes_test.cpp. Task 2 is write-behind: staging, the
// single-flight flush, failure, and truncation and compaction of staged
// entries. The conformance suite runs over write-behind in
// object_store_persistence_unit_test.cpp.

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

auto write_behind(object_persistence_options opts) -> object_persistence_options {
    opts.write_behind = true;
    return opts;
}

auto log_key(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return std::string(k_prefix) + "/log/" + std::string(20 - digits.size(), '0') + digits;
}

auto set_put_delay(const store_t& store, std::chrono::milliseconds delay) -> void {
    const std::lock_guard lock(store.state()->mu);
    store.state()->put_delay = delay;
}

template<typename Engine>
auto make(const store_t& store, const object_persistence_options& opts) -> Engine {
    return Engine{store, std::string(k_bucket), std::string(k_prefix), opts};
}

/// The indices a fresh engine over @p store loads.
template<typename Engine>
auto loaded(const store_t& store, const object_persistence_options& opts)
    -> std::vector<std::uint64_t> {
    auto engine = make<Engine>(store, opts);
    std::vector<std::uint64_t> out;
    for (const auto& entry : engine.get_log_entries(0, 1000)) {
        out.push_back(entry.index());
    }
    return out;
}

/// What a restart would read, without disturbing a fenced engine still
/// running: an unfenced reader writes nothing at construction.
auto peek(const store_t& store, const object_persistence_options& opts)
    -> std::vector<std::uint64_t> {
    object_persistence_options reader;
    reader.log = opts.log;
    reader.max_segment_entries = opts.max_segment_entries;
    return loaded<engine_t>(store, reader);
}

/// Segments of at most @p entries.
auto segmented_by(object_persistence_options opts, std::size_t entries)
    -> object_persistence_options {
    opts = segmented(opts);
    opts.max_segment_entries = entries;
    return opts;
}

/// One request at a time, so which object fails is deterministic.
auto serial(object_persistence_options opts) -> object_persistence_options {
    opts.append_concurrency = 1;
    return opts;
}

auto indices(std::uint64_t first, std::uint64_t last) -> std::vector<std::uint64_t> {
    std::vector<std::uint64_t> out;
    for (std::uint64_t i = first; i <= last; ++i) {
        out.push_back(i);
    }
    return out;
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

BOOST_AUTO_TEST_SUITE(object_store_group_commit_write_behind)

// Requirements 2.4, 3.1 and 3.4: an append stages with no request, reads see
// it, and nothing is durable until a barrier.
template<typename Engine>
auto staging_sends_nothing(const object_persistence_options& opts) -> void {
    {
        const store_t store;
        auto engine = make<Engine>(store, opts);
        BOOST_TEST((engine.durability() == durability_class::barrier));
        BOOST_TEST(describes_durable_storage(engine));
        store.clear_requests();
        for (std::uint64_t i = 1; i <= 3; ++i) {
            BOOST_TEST(engine.append_log_entry_sequenced(make_entry(1, i)) == i);
        }
        BOOST_TEST(store.request_log().empty());
        BOOST_TEST(engine.get_last_log_index() == 3U);
        BOOST_TEST(engine.get_log_entry(2).has_value());
        BOOST_TEST(engine.get_log_entries(1, 3).size() == 3U);
        BOOST_TEST(engine.durable_through() == 0U);
        BOOST_TEST(engine.staged_entry_count() == 3U);
    }
}

BOOST_AUTO_TEST_CASE(staging_sends_nothing_and_reads_see_staged_entries) {
    BOOST_TEST_CONTEXT("per_entry, unfenced") {
        staging_sends_nothing<engine_t>(write_behind(unfenced()));
    }
    BOOST_TEST_CONTEXT("segmented, fenced") {
        staging_sends_nothing<fenced_engine_t>(write_behind(segmented(fenced())));
    }
}

// Requirements 4.1, 4.4 and 4.5: one barrier flushes everything staged, not
// only what it names, as one run — one PUT under `segmented`, one per entry
// under `per_entry` — and publishes the run's last sequence.
template<typename Engine>
auto a_barrier_flushes_every_staged_entry(const object_persistence_options& opts,
                                          std::size_t expected_puts) -> void {
    const store_t store;
    auto engine = make<Engine>(store, opts);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        std::ignore = engine.append_log_entry_sequenced(make_entry(1, i));
    }
    store.clear_requests();
    engine.barrier_through(1);
    BOOST_TEST(store.count_requests("PUT") == expected_puts);
    BOOST_TEST(engine.durable_through() == 4U);
    BOOST_TEST(engine.flushes_issued() == 1U);
    BOOST_TEST(engine.staged_entry_count() == 0U);
    store.clear_requests();
    engine.barrier_through(4);
    BOOST_TEST(store.request_log().empty());
    BOOST_TEST(peek(store, opts) == indices(1, 4), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_barrier_flushes_every_staged_entry_as_one_run) {
    BOOST_TEST_CONTEXT("per_entry, unfenced") {
        a_barrier_flushes_every_staged_entry<engine_t>(write_behind(unfenced()), 4);
    }
    BOOST_TEST_CONTEXT("per_entry, fenced") {
        a_barrier_flushes_every_staged_entry<fenced_engine_t>(write_behind(fenced()), 4);
    }
    BOOST_TEST_CONTEXT("segmented, unfenced") {
        a_barrier_flushes_every_staged_entry<engine_t>(write_behind(segmented(unfenced())), 1);
    }
    BOOST_TEST_CONTEXT("segmented, fenced") {
        a_barrier_flushes_every_staged_entry<fenced_engine_t>(write_behind(segmented(fenced())), 1);
    }
}

// Requirement 4.2 and 7.2's engine half: callers that stage while a flush is
// in flight wait for it and then share one more. Each thread stages under a
// lock of its own, as `node` does under `_mutex`, and waits outside it.
BOOST_AUTO_TEST_CASE(concurrent_barriers_share_at_most_two_flushes) {
    const store_t store;
    const auto opts = write_behind(segmented(unfenced()));
    auto engine = make<engine_t>(store, opts);
    set_put_delay(store, std::chrono::milliseconds{400});
    store.clear_requests();

    std::mutex node_mutex;
    std::uint64_t next_index = 0;
    std::atomic<int> failures{0};
    auto submit = [&] {
        write_sequence seq = 0;
        {
            const std::lock_guard lock(node_mutex);
            seq = engine.append_log_entry_sequenced(make_entry(1, ++next_index));
        }
        try {
            engine.barrier_through(seq);
        } catch (const std::exception&) {
            ++failures;
        }
    };
    std::vector<std::jthread> threads;
    threads.emplace_back(submit);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    for (int i = 0; i < 7; ++i) {
        threads.emplace_back(submit);
    }
    threads.clear();

    BOOST_TEST(failures.load() == 0);
    BOOST_TEST(engine.durable_through() == 8U);
    BOOST_TEST(engine.flushes_issued() == 2U);
    BOOST_TEST(store.count_requests("PUT") == 2U);
    set_put_delay(store, std::chrono::milliseconds{0});
    BOOST_TEST(loaded<engine_t>(store, opts) == indices(1, 8), boost::test_tools::per_element());
}

// Requirements 5.1, 5.2 and 8.3: a partly acknowledged flush publishes its
// acknowledged prefix, records what else it sent as a stray, keeps the rest
// staged, does not latch, and the next barrier deletes the stray and retries.
template<typename Engine>
auto a_partial_flush_publishes_its_prefix_and_retries(const object_persistence_options& opts)
    -> void {
    const store_t store;
    auto engine = make<Engine>(store, opts);
    for (std::uint64_t i = 1; i <= 4; ++i) {
        std::ignore = engine.append_log_entry_sequenced(make_entry(1, i));
    }
    store.fail_puts_for_key(log_key(2));
    BOOST_CHECK_THROW(engine.barrier_through(4), std::runtime_error);
    BOOST_TEST(engine.durable_through() == 1U);
    BOOST_TEST(engine.staged_entry_count() == 3U);
    BOOST_TEST(engine.get_last_log_index() == 4U);
    BOOST_TEST((engine.stray_log_indices() == std::vector<std::uint64_t>{2}));
    BOOST_TEST(!engine.is_fenced());
    // What a crash now would read: the published prefix. Entry 2's object
    // never landed, so nothing above it is on the chain.
    BOOST_TEST(peek(store, opts) == indices(1, 1), boost::test_tools::per_element());

    store.fail_puts_for_key("");
    store.clear_requests();
    engine.barrier_through(4);
    const auto requests = store.request_log();
    BOOST_REQUIRE(!requests.empty());
    if constexpr (Engine::fencing == fencing_mode::compare_and_swap) {
        // A create-only re-PUT of its own landed object would be refused, so
        // the stray is deleted first.
        BOOST_TEST(requests.front() == "DELETE " + log_key(2));
    }
    BOOST_TEST(engine.durable_through() == 4U);
    BOOST_TEST(engine.stray_log_indices().empty());
    BOOST_TEST(peek(store, opts) == indices(1, 4), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_partially_acknowledged_flush_publishes_its_prefix_and_retries) {
    BOOST_TEST_CONTEXT("unfenced") {
        a_partial_flush_publishes_its_prefix_and_retries<engine_t>(
            write_behind(serial(unfenced())));
    }
    BOOST_TEST_CONTEXT("fenced") {
        a_partial_flush_publishes_its_prefix_and_retries<fenced_engine_t>(
            write_behind(serial(fenced())));
    }
}

// Requirement 5.1: a caller waiting on a flush that fails, and whose sequence
// that flush covered, gets the failure rather than starting a retry.
BOOST_AUTO_TEST_CASE(a_waiter_on_a_failed_flush_throws_rather_than_retrying) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(unfenced()));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 1));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 2));
    store.fail_puts_for_key(log_key(1));
    set_put_delay(store, std::chrono::milliseconds{400});
    std::optional<std::string> first_error;
    std::jthread first([&] {
        try {
            engine.barrier_through(1);
        } catch (const std::exception& err) {
            first_error = err.what();
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_CHECK_THROW(engine.barrier_through(2), std::runtime_error);
    first.join();
    BOOST_TEST(first_error.has_value());
    BOOST_TEST(engine.flushes_issued() == 1U);
    BOOST_TEST(engine.durable_through() == 0U);
}

// Requirements 5.4, 6.2: a truncation drops staged entries at or above its
// index without a request, and a barrier on a dropped sequence throws — even
// once later sequences are durable.
template<typename Engine>
auto truncation_drops_staged_entries(const object_persistence_options& opts) -> void {
    {
        const store_t store;
        auto engine = make<Engine>(store, opts);
        engine.barrier_through(engine.append_log_entry_sequenced(make_entry(1, 1)));
        const auto two = engine.append_log_entry_sequenced(make_entry(1, 2));
        const auto three = engine.append_log_entry_sequenced(make_entry(1, 3));
        store.clear_requests();
        engine.truncate_log(3);
        BOOST_TEST(store.request_log().empty());
        BOOST_TEST(engine.get_last_log_index() == 2U);
        BOOST_CHECK_THROW(engine.barrier_through(three), std::runtime_error);
        engine.barrier_through(two);
        const auto replacement = engine.append_log_entry_sequenced(make_entry(2, 3));
        engine.barrier_through(replacement);
        BOOST_CHECK_THROW(engine.barrier_through(three), std::runtime_error);
        auto reloaded = make<engine_t>(store, segmented_by(unfenced(), opts.max_segment_entries));
        BOOST_TEST(reloaded.get_last_log_index() == 3U);
        BOOST_TEST(reloaded.get_log_entry(3)->term() == 2U);
    }
}

BOOST_AUTO_TEST_CASE(truncation_drops_staged_entries_and_fails_their_waiters) {
    BOOST_TEST_CONTEXT("per_entry, unfenced") {
        truncation_drops_staged_entries<engine_t>(write_behind(unfenced()));
    }
    BOOST_TEST_CONTEXT("segmented, fenced") {
        truncation_drops_staged_entries<fenced_engine_t>(write_behind(segmented(fenced())));
    }
}

// Requirements 6.2 and 6.1: a truncation reaching into flushed objects acts on
// them after any running flush finishes, never during it.
BOOST_AUTO_TEST_CASE(truncation_waits_for_a_running_flush) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(unfenced()));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 1));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 2));
    set_put_delay(store, std::chrono::milliseconds{200});
    store.clear_requests();
    std::jthread flusher([&] { engine.barrier_through(2); });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    engine.truncate_log(2);
    flusher.join();
    const auto requests = store.request_log();
    BOOST_REQUIRE(requests.size() == 3U);
    BOOST_TEST(requests[0].starts_with("PUT "));
    BOOST_TEST(requests[1].starts_with("PUT "));
    BOOST_TEST(requests[2] == "DELETE " + log_key(2));
    set_put_delay(store, std::chrono::milliseconds{0});
    BOOST_TEST(loaded<engine_t>(store, write_behind(unfenced())) == indices(1, 1),
               boost::test_tools::per_element());
}

// Requirement 6.3: compaction drops staged entries below its index without a
// request.
BOOST_AUTO_TEST_CASE(compaction_drops_staged_entries_below_its_index) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(segmented(unfenced())));
    const auto one = engine.append_log_entry_sequenced(make_entry(1, 1));
    const auto two = engine.append_log_entry_sequenced(make_entry(1, 2));
    store.clear_requests();
    engine.delete_log_entries_before(2);
    BOOST_TEST(store.request_log().empty());
    BOOST_CHECK_THROW(engine.barrier_through(one), std::runtime_error);
    engine.barrier_through(two);
    BOOST_TEST((store.request_log() == std::vector<std::string>{"PUT " + log_key(2)}));
}

// Requirement 3.2: a plain append is a durable append, and a failed one
// leaves nothing in the log, like any failed append.
BOOST_AUTO_TEST_CASE(a_plain_append_flushes_and_a_failed_one_is_not_in_the_log) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(unfenced()));
    store.clear_requests();
    engine.append_log_entry(make_entry(1, 1));
    BOOST_TEST(store.count_requests("PUT") == 1U);
    BOOST_TEST(engine.durable_through() == 1U);
    store.fail_puts_for_key(log_key(2));
    BOOST_CHECK_THROW(engine.append_log_entry(make_entry(1, 2)), std::runtime_error);
    BOOST_TEST(engine.get_last_log_index() == 1U);
    BOOST_TEST(engine.staged_entry_count() == 0U);
    store.fail_puts_for_key("");
    engine.append_log_entry(make_entry(1, 2));
    BOOST_TEST(loaded<engine_t>(store, write_behind(unfenced())) == indices(1, 2),
               boost::test_tools::per_element());
}

// Requirement 3.3: a bulk append flushes what is staged first, then keeps the
// all-or-nothing contract it has without write-behind.
BOOST_AUTO_TEST_CASE(a_bulk_append_drains_the_stage_then_writes_its_run) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(segmented(unfenced())));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 1));
    store.clear_requests();
    engine.append_log_entries(make_run(1, 2, 3));
    BOOST_TEST((store.request_log() ==
                std::vector<std::string>{"PUT " + log_key(1), "PUT " + log_key(2)}));
    BOOST_TEST(engine.durable_through() == 4U);
    store.fail_next_puts(2);
    BOOST_CHECK_THROW(engine.append_log_entries(make_run(1, 5, 2)), std::runtime_error);
    BOOST_TEST(engine.get_last_log_index() == 4U);
}

// Requirement 1.4: accepted under either layout and either fencing mode, and
// only at the engine's own request — every other option keeps its meaning.
BOOST_AUTO_TEST_CASE(a_staged_append_must_continue_the_log) {
    const store_t store;
    auto engine = make<engine_t>(store, write_behind(unfenced()));
    std::ignore = engine.append_log_entry_sequenced(make_entry(1, 1));
    BOOST_CHECK_THROW(std::ignore = engine.append_log_entry_sequenced(make_entry(1, 3)),
                      std::invalid_argument);
    BOOST_CHECK_THROW(engine.barrier_through(99), std::invalid_argument);
    BOOST_TEST(engine.staged_entry_count() == 1U);
}

// Requirements 6.4 and 8.2: a crash at every request of a multi-chunk flush,
// with appends staging while it runs, reads back a prefix of the run by whole
// objects, never a staged entry, and the restarted engine's next append
// succeeds.
template<typename Engine>
auto a_crash_at_every_flush_request(object_persistence_options opts) -> void {
    for (std::size_t n = 0; n <= 4; ++n) {
        BOOST_TEST_CONTEXT("crash after " << n << " requests") {
            const store_t store;
            {
                auto engine = make<Engine>(store, opts);
                for (std::uint64_t i = 1; i <= 5; ++i) {
                    std::ignore = engine.append_log_entry_sequenced(make_entry(1, i));
                }
                set_put_delay(store, std::chrono::milliseconds{20});
                store.crash_after_requests(n);
                std::jthread stager([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                    try {
                        std::ignore = engine.append_log_entry_sequenced(make_entry(1, 6));
                        std::ignore = engine.append_log_entry_sequenced(make_entry(1, 7));
                    } catch (const std::exception&) {
                        // Staging never sends a request, but a latch could refuse it.
                    }
                });
                try {
                    engine.barrier_through(5);
                } catch (const std::exception&) {
                    // The crash.
                }
                stager.join();
            }
            set_put_delay(store, std::chrono::milliseconds{0});
            store.clear_crash();
            const auto got = loaded<Engine>(store, opts);
            // Chunks of two: [1,2] [3,4] [5]. Whole objects, from the bottom.
            const bool legal = got == indices(1, 0) || got == indices(1, 2) ||
                               got == indices(1, 4) || got == indices(1, 5);
            BOOST_TEST(legal, "loaded " << got.size() << " entries");
            auto restarted = make<Engine>(store, opts);
            const std::uint64_t next = restarted.get_last_log_index() + 1;
            BOOST_CHECK_NO_THROW(restarted.append_log_entry(make_entry(2, next)));
            BOOST_TEST(loaded<Engine>(store, opts).size() == next);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_crash_at_every_request_of_a_flush_reads_a_prefix_of_whole_objects) {
    BOOST_TEST_CONTEXT("unfenced") {
        a_crash_at_every_flush_request<engine_t>(write_behind(segmented_by(unfenced(), 2)));
    }
    BOOST_TEST_CONTEXT("fenced") {
        a_crash_at_every_flush_request<fenced_engine_t>(write_behind(segmented_by(fenced(), 2)));
    }
}

BOOST_AUTO_TEST_SUITE_END()
