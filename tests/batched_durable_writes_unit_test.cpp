// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE batched_durable_writes_unit_test
#include <boost/test/unit_test.hpp>

#include "mock_object_store.hpp"

#include <raft/file_persistence.hpp>
#include <raft/group_storage.hpp>
#include <raft/key_object_store.hpp>
#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Engine-level coverage for `.kiro/specs/batched-durable-writes/`: the two
// optional persistence calls (`append_log_entries`, `save_hard_state`) on the
// memory, file and object-store engines, and what the object-store engine does
// when a run fails part-way or a crash leaves a hole in its log.
//
// The raft-level half — that `node` actually routes a follower's run and its
// term/vote writes through these calls — is in
// raft_batched_durable_writes_test.cpp.

namespace {

using log_entry_t = kythira::log_entry<std::uint64_t, std::uint64_t>;
using memory_engine_t = kythira::memory_persistence_engine<>;
using file_engine_t = kythira::file_persistence_engine<>;
using store_t = kythira::mock_object_store;
using object_engine_t = kythira::object_store_persistence_engine<store_t>;
using fenced_engine_t = kythira::fenced_object_store_persistence_engine<store_t>;

constexpr std::string_view k_bucket = "kythira";
constexpr std::string_view k_prefix = "raft";

auto make_entry(std::uint64_t term, std::uint64_t index) -> log_entry_t {
    const std::string payload = "cmd-" + std::to_string(index);
    std::vector<std::byte> command;
    for (const char c : payload) {
        command.push_back(static_cast<std::byte>(c));
    }
    return log_entry_t{term, index, std::move(command)};
}

auto make_run(std::uint64_t term, std::uint64_t first, std::uint64_t count)
    -> std::vector<log_entry_t> {
    std::vector<log_entry_t> run;
    for (std::uint64_t i = 0; i < count; ++i) {
        run.push_back(make_entry(term, first + i));
    }
    return run;
}

auto log_key(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return std::string(k_prefix) + "/log/" + std::string(20 - digits.size(), '0') + digits;
}

auto make_object_engine(const store_t& store, kythira::object_persistence_options opts = {})
    -> object_engine_t {
    return object_engine_t{store, std::string(k_bucket), std::string(k_prefix), opts};
}

auto make_fenced_engine(const store_t& store) -> fenced_engine_t {
    kythira::object_persistence_options opts;
    opts.owner_id = "node-a";
    return fenced_engine_t{store, std::string(k_bucket), std::string(k_prefix), opts};
}

/// The mock store with its `supports_concurrent_requests` declaration hidden:
/// every operation forwards, nothing else. What an undeclared third-party
/// client looks like to the engine.
class undeclared_store {
public:
    explicit undeclared_store(store_t inner) : _inner(std::move(inner)) {}

    auto put_object(const std::string& b, const std::string& k, std::string_view v) const
        -> kythira::put_result {
        return _inner.put_object(b, k, v);
    }
    [[nodiscard]] auto get_object(const std::string& b, const std::string& k) const
        -> std::optional<kythira::get_result> {
        return _inner.get_object(b, k);
    }
    auto delete_object(const std::string& b, const std::string& k) const -> void {
        _inner.delete_object(b, k);
    }
    [[nodiscard]] auto list_keys(const std::string& b, const std::string& p) const
        -> std::vector<std::string> {
        return _inner.list_keys(b, p);
    }
    [[nodiscard]] static auto provider_name() -> std::string_view { return "undeclared"; }

private:
    store_t _inner;
};

/// A fresh, empty directory under the system temp dir, removed on scope exit.
struct temp_dir {
    std::filesystem::path path;
    temp_dir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("kythira-batched-writes-" + std::to_string(rd()) + std::to_string(rd()));
        std::filesystem::remove_all(path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

}  // namespace

// ── The concepts ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(batched_durable_writes_concepts)

BOOST_AUTO_TEST_CASE(every_shipped_engine_has_both_extensions) {
    static_assert(kythira::bulk_append_persistence_engine<memory_engine_t>);
    static_assert(kythira::hard_state_persistence_engine<memory_engine_t>);
    static_assert(kythira::bulk_append_persistence_engine<file_engine_t>);
    static_assert(kythira::hard_state_persistence_engine<file_engine_t>);
    static_assert(kythira::bulk_append_persistence_engine<object_engine_t>);
    static_assert(kythira::hard_state_persistence_engine<object_engine_t>);
    static_assert(kythira::bulk_append_persistence_engine<fenced_engine_t>);
    static_assert(kythira::hard_state_persistence_engine<fenced_engine_t>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(the_group_scoped_wrapper_forwards_both_extensions) {
    using scoped_t = kythira::group_scoped_persistence<memory_engine_t>;
    static_assert(kythira::bulk_append_persistence_engine<scoped_t>);
    static_assert(kythira::hard_state_persistence_engine<scoped_t>);

    scoped_t scoped{7, memory_engine_t{}};
    const auto run = make_run(1, 1, 3);
    scoped.append_log_entries(run);
    scoped.save_hard_state(std::uint64_t{4}, std::optional<std::uint64_t>{2});
    BOOST_TEST(scoped.get_last_log_index() == 3U);
    BOOST_TEST(scoped.load_current_term() == 4U);
    BOOST_TEST((scoped.load_voted_for() == std::optional<std::uint64_t>{2}));
}

BOOST_AUTO_TEST_CASE(only_a_store_that_declares_it_is_called_concurrently) {
    static_assert(kythira::concurrent_key_object_store<store_t>);
    static_assert(kythira::key_object_store<undeclared_store>);
    static_assert(!kythira::concurrent_key_object_store<undeclared_store>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Memory and file engines ──────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(batched_durable_writes_local_engines)

BOOST_AUTO_TEST_CASE(the_memory_engine_records_no_vote) {
    memory_engine_t engine;
    engine.save_hard_state(3, std::optional<std::uint64_t>{2});
    BOOST_TEST((engine.load_voted_for() == std::optional<std::uint64_t>{2}));
    engine.save_hard_state(4, std::nullopt);
    BOOST_TEST(engine.load_current_term() == 4U);
    BOOST_TEST(!engine.load_voted_for().has_value());
}

BOOST_AUTO_TEST_CASE(the_memory_engine_appends_a_run) {
    memory_engine_t engine;
    const auto run = make_run(2, 1, 5);
    engine.append_log_entries(run);
    BOOST_TEST(engine.get_last_log_index() == 5U);
    BOOST_TEST(engine.get_log_entry(3)->term() == 2U);
}

BOOST_AUTO_TEST_CASE(the_file_engine_round_trips_no_vote_across_a_restart) {
    temp_dir dir;
    {
        file_engine_t engine{dir.path};
        engine.save_hard_state(5, std::optional<std::uint64_t>{3});
    }
    {
        file_engine_t engine{dir.path};
        BOOST_TEST(engine.load_current_term() == 5U);
        BOOST_TEST((engine.load_voted_for() == std::optional<std::uint64_t>{3}));
        engine.save_hard_state(6, std::nullopt);
    }
    file_engine_t engine{dir.path};
    BOOST_TEST(engine.load_current_term() == 6U);
    BOOST_TEST(!engine.load_voted_for().has_value());
}

// An unchanged slot is not rewritten: removing the file behind the engine's back
// and saving the same value again must leave it removed.
BOOST_AUTO_TEST_CASE(the_file_engine_skips_an_unchanged_slot) {
    temp_dir dir;
    file_engine_t engine{dir.path};
    engine.save_hard_state(5, std::optional<std::uint64_t>{3});
    std::filesystem::remove(dir.path / "term");
    engine.save_hard_state(5, std::optional<std::uint64_t>{4});
    BOOST_TEST(!std::filesystem::exists(dir.path / "term"));
    BOOST_TEST(std::filesystem::exists(dir.path / "voted_for"));
}

BOOST_AUTO_TEST_CASE(the_file_engine_appends_a_run_that_survives_a_restart) {
    temp_dir dir;
    {
        file_engine_t engine{dir.path};
        engine.append_log_entry(make_entry(1, 1));
        const auto run = make_run(2, 2, 4);
        engine.append_log_entries(run);
        BOOST_TEST(engine.get_last_log_index() == 5U);
    }
    file_engine_t engine{dir.path};
    BOOST_TEST(engine.get_last_log_index() == 5U);
    BOOST_TEST(engine.get_log_entry(1)->term() == 1U);
    BOOST_TEST(engine.get_log_entry(5)->term() == 2U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Object-store engine: hard state ──────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(batched_durable_writes_object_store_hard_state)

BOOST_AUTO_TEST_CASE(a_new_term_and_vote_write_the_term_first) {
    store_t store;
    auto engine = make_object_engine(store);
    store.clear_requests();
    engine.save_hard_state(2, std::optional<std::uint64_t>{1});
    const std::vector<std::string> expected{"PUT raft/term", "PUT raft/voted_for"};
    BOOST_TEST(store.request_log() == expected, boost::test_tools::per_element());
}

// The common vote-granting case: the term is already on the store, so the vote
// is the only PUT — one round trip where save_voted_for + save_current_term
// cost two.
BOOST_AUTO_TEST_CASE(a_vote_at_the_stored_term_is_one_put) {
    store_t store;
    auto engine = make_object_engine(store);
    engine.save_hard_state(2, std::nullopt);
    store.clear_requests();
    engine.save_hard_state(2, std::optional<std::uint64_t>{3});
    const std::vector<std::string> expected{"PUT raft/voted_for"};
    BOOST_TEST(store.request_log() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(an_unchanged_pair_writes_nothing) {
    store_t store;
    auto engine = make_object_engine(store);
    engine.save_hard_state(2, std::optional<std::uint64_t>{3});
    store.clear_requests();
    engine.save_hard_state(2, std::optional<std::uint64_t>{3});
    BOOST_TEST(store.request_log().empty());
}

BOOST_AUTO_TEST_CASE(no_vote_is_stored_as_none_and_read_back_as_no_vote) {
    store_t store;
    {
        auto engine = make_object_engine(store);
        engine.save_hard_state(2, std::optional<std::uint64_t>{3});
        engine.save_hard_state(3, std::nullopt);
    }
    BOOST_TEST(store.body("raft/voted_for") == "none");
    auto engine = make_object_engine(store);
    BOOST_TEST(engine.load_current_term() == 3U);
    BOOST_TEST(!engine.load_voted_for().has_value());
}

// A failed vote PUT leaves the new term beside the old vote, never the reverse,
// and a retry writes only the vote.
BOOST_AUTO_TEST_CASE(a_failed_vote_put_leaves_the_new_term_and_the_old_vote) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.write_retries = 0;
    auto engine = make_object_engine(store, opts);
    engine.save_hard_state(2, std::optional<std::uint64_t>{1});
    store.fail_puts_for_key("raft/voted_for");
    BOOST_CHECK_THROW(engine.save_hard_state(3, std::optional<std::uint64_t>{2}),
                      std::runtime_error);
    BOOST_TEST(store.body("raft/term") == "3");
    BOOST_TEST(store.body("raft/voted_for") == "1");

    store.fail_puts_for_key("");
    store.clear_requests();
    engine.save_hard_state(3, std::optional<std::uint64_t>{2});
    const std::vector<std::string> expected{"PUT raft/voted_for"};
    BOOST_TEST(store.request_log() == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_fenced_engine_keeps_the_chokepoint) {
    store_t store;
    auto engine = make_fenced_engine(store);
    engine.save_hard_state(2, std::optional<std::uint64_t>{1});
    // Another writer moves the term on behind this engine's back.
    store.seed("raft/term", "9");
    BOOST_CHECK_THROW(engine.save_hard_state(3, std::nullopt), kythira::persistence_fenced_error);
    BOOST_TEST(engine.is_fenced());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Object-store engine: bulk append ─────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(batched_durable_writes_object_store_append)

BOOST_AUTO_TEST_CASE(a_run_is_one_put_per_entry_and_survives_a_restart) {
    store_t store;
    {
        auto engine = make_object_engine(store);
        store.clear_requests();
        const auto run = make_run(1, 1, 6);
        engine.append_log_entries(run);
        BOOST_TEST(store.count_requests("PUT") == 6U);
        BOOST_TEST(engine.get_last_log_index() == 6U);
    }
    auto engine = make_object_engine(store);
    BOOST_TEST(engine.get_last_log_index() == 6U);
    BOOST_TEST(engine.get_log_entry(4)->term() == 1U);
}

// The point of the call: the entries of a run wait on the store together.
BOOST_AUTO_TEST_CASE(a_run_keeps_append_concurrency_puts_in_flight) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.append_concurrency = 4;
    auto engine = make_object_engine(store, opts);
    store.state()->put_delay = std::chrono::milliseconds{40};

    const auto run = make_run(1, 1, 12);
    const auto started = std::chrono::steady_clock::now();
    engine.append_log_entries(run);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    BOOST_TEST(store.state()->max_puts_in_flight > 1U);
    BOOST_TEST(store.state()->max_puts_in_flight <= 4U);
    // Twelve sequential PUTs would take 480 ms; three waves of four take ~120.
    BOOST_TEST(elapsed < std::chrono::milliseconds{400});
    BOOST_TEST(engine.get_last_log_index() == 12U);
}

BOOST_AUTO_TEST_CASE(a_store_that_does_not_declare_concurrency_gets_one_put_at_a_time) {
    store_t inner;
    kythira::object_persistence_options opts;
    opts.append_concurrency = 8;
    kythira::object_store_persistence_engine<undeclared_store> engine{
        undeclared_store{inner}, std::string(k_bucket), std::string(k_prefix), opts};
    inner.state()->put_delay = std::chrono::milliseconds{5};
    const auto run = make_run(1, 1, 6);
    engine.append_log_entries(run);
    BOOST_TEST(inner.state()->max_puts_in_flight == 1U);
    BOOST_TEST(engine.get_last_log_index() == 6U);
}

BOOST_AUTO_TEST_CASE(append_concurrency_zero_is_rejected) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.append_concurrency = 0;
    BOOST_CHECK_THROW((void)make_object_engine(store, opts), std::invalid_argument);
}

// A run that fails part-way leaves nothing in the log, records what it sent as
// strays, and the retry deletes nothing it is about to overwrite.
BOOST_AUTO_TEST_CASE(a_failed_run_leaves_the_log_unchanged_and_the_retry_lands) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.write_retries = 0;
    opts.append_concurrency = 1;
    auto engine = make_object_engine(store, opts);
    engine.append_log_entry(make_entry(1, 1));

    store.fail_puts_for_key(log_key(4));
    const auto run = make_run(2, 2, 4);
    BOOST_CHECK_THROW(engine.append_log_entries(run), std::runtime_error);
    BOOST_TEST(engine.get_last_log_index() == 1U);
    BOOST_TEST(!engine.get_log_entry(2).has_value());
    const std::vector<std::uint64_t> sent{2, 3, 4};
    BOOST_TEST(engine.stray_log_indices() == sent, boost::test_tools::per_element());

    store.fail_puts_for_key("");
    store.clear_requests();
    engine.append_log_entries(run);
    BOOST_TEST(store.count_requests("DELETE") == 0U);
    BOOST_TEST(engine.get_last_log_index() == 5U);
    BOOST_TEST(engine.stray_log_indices().empty());
}

// The case strays exist for: the retry is a shorter, different log. Without the
// cleanup, the old entries 3-4 would sit above the new entry 2 and a restart
// would read them back as part of the log.
BOOST_AUTO_TEST_CASE(strays_are_deleted_before_a_shorter_log_is_written) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.write_retries = 0;
    opts.append_concurrency = 1;
    {
        auto engine = make_object_engine(store, opts);
        engine.append_log_entry(make_entry(1, 1));
        store.fail_puts_for_key(log_key(5));
        const auto run = make_run(2, 2, 4);
        BOOST_CHECK_THROW(engine.append_log_entries(run), std::runtime_error);
        store.fail_puts_for_key("");

        // A new leader replaces the run with one entry from a later term.
        engine.append_log_entry(make_entry(3, 2));
        BOOST_TEST(!store.has(log_key(3)));
        BOOST_TEST(!store.has(log_key(4)));
        BOOST_TEST(!store.has(log_key(5)));
    }
    auto engine = make_object_engine(store, opts);
    BOOST_TEST(engine.get_last_log_index() == 2U);
    BOOST_TEST(engine.get_log_entry(2)->term() == 3U);
}

// Under compare_and_swap every log PUT is create-only, so a stray left by a
// failed run would refuse the retry and latch the engine as if a second writer
// existed. Deleting strays first is what keeps that from happening.
BOOST_AUTO_TEST_CASE(a_fenced_engine_retries_a_failed_run_without_latching) {
    store_t store;
    auto engine = make_fenced_engine(store);
    engine.append_log_entry(make_entry(1, 1));

    store.fail_puts_for_key(log_key(3));
    const auto run = make_run(2, 2, 3);
    BOOST_CHECK_THROW(engine.append_log_entries(run), std::runtime_error);
    BOOST_TEST(!engine.is_fenced());

    store.fail_puts_for_key("");
    engine.append_log_entries(run);
    BOOST_TEST(!engine.is_fenced());
    BOOST_TEST(engine.get_last_log_index() == 4U);
}

BOOST_AUTO_TEST_CASE(a_fenced_run_refused_by_another_writer_latches) {
    store_t store;
    auto engine = make_fenced_engine(store);
    // Another writer already put an object at index 3.
    store.seed(log_key(3), "{}");
    const auto run = make_run(1, 1, 4);
    BOOST_CHECK_THROW(engine.append_log_entries(run), kythira::persistence_fenced_error);
    BOOST_TEST(engine.is_fenced());
    BOOST_TEST(engine.get_last_log_index() == 0U);
}

// What a crash part-way through a concurrent run leaves: objects above a hole.
// They are not loaded — node indexes its log by position, so a hole would shift
// every entry above it — and they are deleted before the next log write.
BOOST_AUTO_TEST_CASE(objects_past_a_hole_are_not_loaded_and_are_deleted_on_the_next_write) {
    store_t store;
    {
        auto engine = make_object_engine(store);
        const auto run = make_run(1, 1, 5);
        engine.append_log_entries(run);
    }
    store.delete_object(std::string(k_bucket), log_key(3));

    auto engine = make_object_engine(store);
    BOOST_TEST(engine.get_last_log_index() == 2U);
    const std::vector<std::uint64_t> strays{4, 5};
    BOOST_TEST(engine.stray_log_indices() == strays, boost::test_tools::per_element());
    BOOST_TEST(store.has(log_key(4)));  // construction writes nothing

    engine.append_log_entry(make_entry(2, 3));
    BOOST_TEST(!store.has(log_key(4)));
    BOOST_TEST(!store.has(log_key(5)));
    BOOST_TEST(engine.stray_log_indices().empty());
}

// Highest first, so a crash part-way through leaves a contiguous log.
BOOST_AUTO_TEST_CASE(truncation_deletes_from_the_top_down) {
    store_t store;
    auto engine = make_object_engine(store);
    const auto run = make_run(1, 1, 4);
    engine.append_log_entries(run);
    store.clear_requests();
    engine.truncate_log(2);
    const std::vector<std::string> expected{"DELETE " + log_key(4), "DELETE " + log_key(3),
                                            "DELETE " + log_key(2)};
    BOOST_TEST(store.request_log() == expected, boost::test_tools::per_element());
    BOOST_TEST(engine.get_last_log_index() == 1U);
}

BOOST_AUTO_TEST_CASE(an_oversized_entry_fails_the_run_before_anything_is_sent) {
    store_t store;
    kythira::object_persistence_options opts;
    opts.max_object_bytes = 64;
    auto engine = make_object_engine(store, opts);
    auto run = make_run(1, 1, 3);
    run[2] = log_entry_t{1, 3, std::vector<std::byte>(200, std::byte{'x'})};
    store.clear_requests();
    BOOST_CHECK_THROW(engine.append_log_entries(run), std::runtime_error);
    BOOST_TEST(store.count_requests("PUT") == 0U);
    BOOST_TEST(engine.stray_log_indices().empty());
}

BOOST_AUTO_TEST_SUITE_END()
