// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file durable_append_barrier_test.cpp
/// @brief `.kiro/specs/durable-append-barrier/` — the coverage test, written
///        first and deliberately failing.
///
/// The question this file asks is the one
/// `.kiro/specs/multi-raft-performance/` task 19 asked of a whole benchmark
/// host and got 19.9%/24.5% back from: **of the log entries this node
/// appended, how many did a durability barrier cover before the node
/// advertised them?**
///
/// It asks it at the smallest surface that reproduces the defect — one
/// `node<Types>` over a one-node cluster, with a file-backed engine behind a
/// counting handle — rather than through `multi_raft`, because the placement
/// mistake is in `node`'s append path and nothing about a host is needed to
/// show it.
///
/// The number this surface reports on the pre-change tree is **0%**, not the
/// 19.9–24.5% task 19 measured, and the difference is not a discrepancy: task
/// 19 supplied a `tick_batch_controller`, whose barrier caught the fifth of
/// the appends that happened to race into its window. There is no controller
/// here, and `file_persistence_engine` reaches `sync_log_and_directory()` from
/// `commit_batch()` and from nowhere else — so a node left to itself takes no
/// barrier at all. 0% is the honest floor of the same defect.

#define BOOST_TEST_MODULE durable_append_barrier_test
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>

#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/simulator_network.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

// The fault-injection cases at the bottom of this file need libfiu's control
// API as well as the FIU_ENABLE define that arms the `fiu_do_on` points inside
// `file_persistence.hpp`. Without libfiu those points compile away and the
// cases would assert on delays and failures that never happen, so they are
// compiled out with them — the same arrangement
// `multi_raft_crash_consistency_test` uses.
#ifdef FIU_ENABLE
#include <fiu-control.h>
#include <fiu.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("durable_append_barrier_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

#ifdef FIU_ENABLE
namespace {
/// libfiu needs `fiu_init` before any fault point is armed. Without it
/// `fiu_enable` succeeds and the first `fiu_fail` dereferences a null
/// dispatch table, which arrives as a segmentation fault rather than as an
/// error from the call that was actually wrong.
struct fiu_init_fixture {
    fiu_init_fixture() { ::fiu_init(0); }
};
}  // namespace
BOOST_GLOBAL_FIXTURE(fiu_init_fixture);
#endif

namespace {

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// The counter, on the outside of the engine
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What a durability row has to be able to say about itself.
///
/// Requirement 2.3 keeps this out of `include/`, and Requirement 3.3 is the
/// reason `_covered` exists beside `_entries` rather than the ratio being
/// computed as entries over barriers: a barrier that covered a fifth of the
/// entries divided into all of them reports a system fsyncing five times less
/// often than it does. That was the defect
/// `.kiro/specs/multi-raft-performance/` task 19 found in its own first draft.
struct barrier_counters {
    /// Entries handed to the store.
    std::atomic<std::uint64_t> _entries{0};
    /// `barrier_through()` calls the store completed for this node.
    std::atomic<std::uint64_t> _barriers{0};
    /// Of `_entries`, those a completed barrier covered.
    std::atomic<std::uint64_t> _covered{0};

    [[nodiscard]] auto covered_fraction() const -> double {
        const auto e = _entries.load();
        return e == 0 ? 0.0 : static_cast<double>(_covered.load()) / static_cast<double>(e);
    }
};

/// @brief A file-backed engine behind a handle that counts what it forwards.
///
/// A handle (shared state) rather than a value because `node` takes the engine
/// by value and moves it inside, so a test that wants to read the counters
/// afterwards has no other way to reach what it constructed. This is the same
/// reason `tests/multi_raft_transport_harness.hpp`'s
/// `benchmark_persistence_engine` is a handle.
///
/// **The question it asks changed with this spec** (task 8). It used to be
/// "was a batch open when this append passed through", which is the right
/// question for a batch opened around a tick and the wrong one for a barrier
/// taken at a response boundary. It is now "did a barrier whose covered
/// sequence reaches this entry's complete before the node advertised it",
/// which the wrapper can answer because it sees both calls.
class counting_file_engine {
public:
    using inner_type =
        kythira::file_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
    using log_entry_t = inner_type::log_entry_t;
    using snapshot_t = inner_type::snapshot_t;

    counting_file_engine(fs::path dir, barrier_counters* counters, bool barriers_enabled = true)
        : _state(std::make_shared<state>(std::move(dir), counters, barriers_enabled)) {}

    auto save_current_term(std::uint64_t term) -> void { _state->_inner.save_current_term(term); }
    auto load_current_term() -> std::uint64_t { return _state->_inner.load_current_term(); }
    auto save_voted_for(std::uint64_t node) -> void { _state->_inner.save_voted_for(node); }
    auto load_voted_for() -> std::optional<std::uint64_t> {
        return _state->_inner.load_voted_for();
    }

    auto append_log_entry(const log_entry_t& entry) -> void {
        (void)append_log_entry_sequenced(entry);
    }

    auto append_log_entry_sequenced(const log_entry_t& entry) -> kythira::write_sequence {
        const auto seq = _state->_inner.append_log_entry_sequenced(entry);
        _state->_counters->_entries.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(_state->_mu);
            _state->_pending.push_back(seq);
        }
        return seq;
    }

    auto barrier_through(kythira::write_sequence seq) -> void {
        _state->_inner.barrier_through(seq);
        _state->_counters->_barriers.fetch_add(1, std::memory_order_relaxed);
        settle();
    }

    [[nodiscard]] auto durable_through() const -> kythira::write_sequence {
        return _state->_inner.durable_through();
    }

    [[nodiscard]] auto durability() const -> kythira::durability_class {
        return _state->_inner.durability();
    }

    [[nodiscard]] auto barriers_issued() const -> std::uint64_t {
        return _state->_inner.barriers_issued();
    }

    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        return _state->_inner.get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        return _state->_inner.get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t { return _state->_inner.get_last_log_index(); }
    auto truncate_log(std::uint64_t index) -> void { _state->_inner.truncate_log(index); }
    auto save_snapshot(const snapshot_t& snap) -> void { _state->_inner.save_snapshot(snap); }
    auto load_snapshot() -> std::optional<snapshot_t> { return _state->_inner.load_snapshot(); }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        _state->_inner.delete_log_entries_before(index);
    }

private:
    /// Move every pending sequence a completed barrier now covers into
    /// `_covered`. A sequence no barrier ever reaches simply stays pending and
    /// is reported as uncovered, which is Requirement 2.4: an entry appended
    /// while no barrier is pending is uncovered now, not optimistically
    /// credited to a barrier that may never come.
    auto settle() -> void {
        const auto durable = _state->_inner.durable_through();
        std::lock_guard lock(_state->_mu);
        std::uint64_t newly = 0;
        auto it = _state->_pending.begin();
        while (it != _state->_pending.end()) {
            if (*it <= durable) {
                ++newly;
                it = _state->_pending.erase(it);
            } else {
                ++it;
            }
        }
        if (newly != 0) {
            _state->_counters->_covered.fetch_add(newly, std::memory_order_relaxed);
        }
    }

    struct state {
        state(fs::path dir, barrier_counters* counters, bool barriers_enabled)
            : _inner(std::move(dir), barriers_enabled), _counters(counters) {}
        inner_type _inner;
        barrier_counters* _counters;
        std::mutex _mu;
        std::vector<kythira::write_sequence> _pending;
    };

    std::shared_ptr<state> _state;
};

// ─────────────────────────────────────────────────────────────────────────────
// One node, one group, a real directory
// ─────────────────────────────────────────────────────────────────────────────

struct test_raft_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::simulator_network_client<kythira::raft_simulator_network_types<node_id_type>,
                                          serializer_type, serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<kythira::raft_simulator_network_types<node_id_type>,
                                          serializer_type, serialized_data_type>;
    using persistence_engine_type = counting_file_engine;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;
    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;
};

using node_type = kythira::node<test_raft_types>;
using raft_network_types = kythira::raft_simulator_network_types<test_raft_types::node_id_type>;

/// A temp directory that removes itself, unique per process and per name.
struct temp_dir {
    fs::path path;
    explicit temp_dir(const std::string& name) {
        path = fs::temp_directory_path() /
               ("kythira_dab_" + name + "_" + std::to_string(static_cast<unsigned>(::getpid())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

auto fast_config() -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    cfg._election_timeout_min = std::chrono::milliseconds{60};
    cfg._election_timeout_max = std::chrono::milliseconds{120};
    cfg._heartbeat_interval = std::chrono::milliseconds{20};
    cfg._rpc_timeout = std::chrono::milliseconds{80};
    return cfg;
}

/// A one-node cluster: quorum is one, so a submitted command commits without
/// any peer having to exist. Callers MUST stop() the node before it is
/// destroyed — `node` does not join its own threads.
auto start_single_node(network_simulator::NetworkSimulator<raft_network_types>& simulator,
                       const fs::path& dir, barrier_counters* counters)
    -> std::unique_ptr<node_type> {
    simulator.start();
    auto sim_node = simulator.create_node(1);

    auto n = std::make_unique<node_type>(
        std::uint64_t{1},
        test_raft_types::network_client_type{sim_node, test_raft_types::serializer_type{}},
        test_raft_types::network_server_type{sim_node, test_raft_types::serializer_type{}},
        counting_file_engine{dir, counters},
        test_raft_types::logger_type{kythira::log_level::error}, test_raft_types::metrics_type{},
        test_raft_types::membership_manager_type{}, fast_config());

    n->set_cluster_configuration({1});
    n->start();
    return n;
}

/// `node` has no clock of its own — `check_election_timeout()` and
/// `check_heartbeat_timeout()` are documented as "call from an external timer
/// loop", and in production that loop is `multi_raft::tick()`. This is the
/// smallest stand-in for it.
class node_driver {
public:
    explicit node_driver(node_type& n)
        : _thread([this, &n] {
              while (_running.load(std::memory_order_acquire)) {
                  n.check_election_timeout();
                  n.check_heartbeat_timeout();
                  std::this_thread::sleep_for(std::chrono::milliseconds{5});
              }
          }) {}

    ~node_driver() { stop(); }

    auto stop() -> void {
        if (_running.exchange(false, std::memory_order_acq_rel) && _thread.joinable()) {
            _thread.join();
        }
    }

    node_driver(const node_driver&) = delete;
    auto operator=(const node_driver&) -> node_driver& = delete;

private:
    std::atomic<bool> _running{true};
    std::thread _thread;
};

auto await_leader(node_type& n, std::chrono::milliseconds budget) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (n.is_leader()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return n.is_leader();
}

using engine_type = counting_file_engine::inner_type;

auto make_entry(std::uint64_t term, std::uint64_t index) -> engine_type::log_entry_t {
    engine_type::log_entry_t e;
    e._term = term;
    e._index = index;
    e._command = std::vector<std::byte>{std::byte{0xAB}, std::byte{0xCD}};
    return e;
}

auto payload(std::size_t i, std::size_t value_bytes) -> std::vector<std::byte> {
    return test_raft_types::state_machine_type::make_put_command("k" + std::to_string(i),
                                                                 std::string(value_bytes, 'v'));
}

constexpr std::size_t k_commands = 32;

}  // namespace

BOOST_AUTO_TEST_SUITE(durable_append_barrier)

/// **Requirement 1.1, 2.1, 2.2, 2.4 — and the test that must fail first.**
///
/// A leader appends `k_commands` entries to a file-backed log. Every one of
/// them is advertised: a one-node cluster's own append is immediately a
/// majority, so `match_index` and the commit index move the instant the entry
/// is in the log. Requirement 1.1 says a barrier must have covered it by then.
///
/// On the pre-change tree this fails at 0 covered of 33 (32 commands plus the
/// no-op entry a new leader appends), because nothing on the append path takes
/// a barrier at all.
BOOST_AUTO_TEST_CASE(every_appended_entry_is_covered_by_a_barrier, *boost::unit_test::timeout(60)) {
    temp_dir dir{"leader"};
    barrier_counters counters;

    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_single_node(sim, dir.path, &counters);
    node_driver driver{*node};

    BOOST_REQUIRE(await_leader(*node, std::chrono::seconds{10}));

    for (std::size_t i = 0; i < k_commands; ++i) {
        auto f = node->submit_command(payload(i, 64), std::chrono::seconds{5});
        std::move(f).get();
    }

    driver.stop();
    node->stop();

    const auto entries = counters._entries.load();
    const auto covered = counters._covered.load();
    BOOST_TEST_MESSAGE("entries=" << entries << " covered=" << covered
                                  << " barriers=" << counters._barriers.load()
                                  << " fraction=" << counters.covered_fraction());

    BOOST_REQUIRE_GE(entries, k_commands);
    // Requirement 2.2: anything other than 1.0 in a configuration that claims
    // durability is a failure, not a slow row.
    BOOST_CHECK_EQUAL(covered, entries);
}

/// **Requirement 5.4 — an engine that does not barrier is not called durable.**
///
/// The `buffered` arm exists so the measurement can price the encode-and-write
/// half separately from the fsync half. It writes through to the operating
/// system and takes no barrier, so it survives a process death and loses
/// everything to a power cut. What must never happen is for it to be *read* as
/// durable, and the answer to that question is a method rather than an
/// assumption.
BOOST_AUTO_TEST_CASE(a_buffered_engine_refuses_the_word_durable, *boost::unit_test::timeout(30)) {
    temp_dir dir{"buffered"};

    engine_type barriered{dir.path / "a", true};
    engine_type buffered{dir.path / "b", false};
    kythira::memory_persistence_engine<> in_memory;

    BOOST_CHECK(kythira::describes_durable_storage(barriered));
    BOOST_CHECK(!kythira::describes_durable_storage(buffered));
    BOOST_CHECK(!kythira::describes_durable_storage(in_memory));

    BOOST_CHECK(barriered.durability() == kythira::durability_class::barrier);
    BOOST_CHECK(buffered.durability() == kythira::durability_class::buffered);
    BOOST_CHECK(in_memory.durability() == kythira::durability_class::none);

    // And the buffered arm really does take no barrier, which is what makes it
    // cheaper and what makes it not durable.
    auto e = make_entry(1, 1);
    const auto seq = buffered.append_log_entry_sequenced(e);
    buffered.barrier_through(seq);
    BOOST_CHECK_EQUAL(buffered.barriers_issued(), 0u);
}

/// **Requirement 5.1 — the memory engine's barrier costs nothing.**
///
/// Not a timing assertion, which would be a flake. The claim is structural:
/// `barrier_through` is an empty function and `durable_through` is a member
/// read, so a memory-backed configuration reports full coverage without any
/// barrier existing to be paid for. Every Tier A and Tier B row in this project
/// runs on this engine, and this is why none of them moved.
BOOST_AUTO_TEST_CASE(a_memory_engine_barrier_is_free_and_covers_everything,
                     *boost::unit_test::timeout(30)) {
    kythira::memory_persistence_engine<> engine;
    kythira::write_sequence last{0};
    for (std::uint64_t i = 1; i <= 16; ++i) {
        last = engine.append_log_entry_sequenced(make_entry(1, i));
    }
    engine.barrier_through(last);
    BOOST_CHECK_EQUAL(engine.durable_through(), last);
    BOOST_CHECK(engine.durability() == kythira::durability_class::none);
}

/// **Requirement 3.1 — one barrier serving several appends, in its simplest
/// form.**
///
/// The batch form, which is what a follower gets for free: an AppendEntries
/// carrying N entries appends N times and barriers once. Deterministic, with
/// no threads and no injected timing, because the coalescing here is structural
/// rather than a race that happened to be won.
///
/// The threaded form — several *concurrent* appends joining one barrier — is in
/// the fault-injection suite, where a delay inside the syscall makes it
/// deterministic too.
BOOST_AUTO_TEST_CASE(one_barrier_covers_a_whole_batch_of_appends, *boost::unit_test::timeout(60)) {
    temp_dir dir{"batch"};
    engine_type engine{dir.path};

    constexpr std::uint64_t k_batch = 24;
    kythira::write_sequence last{0};
    for (std::uint64_t i = 1; i <= k_batch; ++i) {
        last = engine.append_log_entry_sequenced(make_entry(1, i));
    }
    BOOST_CHECK_EQUAL(engine.barriers_issued(), 0u);

    engine.barrier_through(last);

    BOOST_CHECK_EQUAL(engine.barriers_issued(), 1u);
    BOOST_CHECK_GE(engine.durable_through(), last);
    // Both halves, because either alone is satisfiable by a broken
    // implementation: one barrier is cheap if it covered nothing, and full
    // coverage is easy if every append paid for its own.
    for (std::uint64_t i = 1; i <= k_batch; ++i) {
        BOOST_CHECK(engine.get_log_entry(i).has_value());
    }
}

/// **Requirement 1.1/1.2 — task 4's crash test. A counter is evidence; a
/// restart is proof.**
///
/// A child process appends and barriers, then dies by `_exit` — no destructors,
/// no stream flush, no clean shutdown of any kind. The parent reopens the same
/// directory in a fresh engine and asserts every entry is there.
///
/// **What this does and does not prove.** It proves nothing an advertised
/// append needs is stranded in this process's own memory at the moment it is
/// advertised, and that recovery reads it all back. It does not prove
/// power-cut durability: only the `fsync` does that, and the coverage test
/// above is what asserts the `fsync` happened. The two together are the claim;
/// neither alone is.
BOOST_AUTO_TEST_CASE(entries_survive_a_process_death_with_no_clean_shutdown,
                     *boost::unit_test::timeout(60)) {
    temp_dir dir{"crash"};
    constexpr std::uint64_t k_entries = 20;

    // Forked before any node, simulator or executor exists in this test case:
    // the child touches nothing but the engine, so it inherits no thread it
    // would have to reason about.
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        engine_type engine{dir.path};
        for (std::uint64_t i = 1; i <= k_entries; ++i) {
            const auto seq = engine.append_log_entry_sequenced(make_entry(3, i));
            engine.barrier_through(seq);
        }
        // Straight out, with the engine still alive and nothing unwound.
        ::_exit(0);
    }

    int status = 0;
    BOOST_REQUIRE_EQUAL(::waitpid(pid, &status, 0), pid);
    BOOST_REQUIRE(WIFEXITED(status));
    BOOST_REQUIRE_EQUAL(WEXITSTATUS(status), 0);

    engine_type recovered{dir.path};
    BOOST_CHECK_EQUAL(recovered.get_last_log_index(), k_entries);
    for (std::uint64_t i = 1; i <= k_entries; ++i) {
        auto e = recovered.get_log_entry(i);
        BOOST_REQUIRE(e.has_value());
        BOOST_CHECK_EQUAL(e->term(), 3u);
    }
}

/// **Requirement 1.1 — the leader's own append is durable before it is
/// counted, across a restart too.**
///
/// A node that comes back from its store holds everything the store returned,
/// so `_durable_log_index` starts at the last recovered index. Without that a
/// restarted leader would withhold its own acknowledgement for entries it
/// demonstrably has, forever: nothing re-barriers an entry that is already on
/// disk.
BOOST_AUTO_TEST_CASE(a_restarted_node_commits_again, *boost::unit_test::timeout(90)) {
    temp_dir dir{"restart"};
    barrier_counters first_run;

    {
        network_simulator::NetworkSimulator<raft_network_types> sim;
        auto node = start_single_node(sim, dir.path, &first_run);
        node_driver driver{*node};
        BOOST_REQUIRE(await_leader(*node, std::chrono::seconds{10}));
        for (std::size_t i = 0; i < 8; ++i) {
            std::move(node->submit_command(payload(i, 32), std::chrono::seconds{5})).get();
        }
        driver.stop();
        node->stop();
    }

    barrier_counters second_run;
    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_single_node(sim, dir.path, &second_run);
    node_driver driver{*node};
    BOOST_REQUIRE(await_leader(*node, std::chrono::seconds{10}));

    // The proof that the watermark was restored: a command submitted after the
    // restart still commits, which needs this node's own acknowledgement for
    // an index above everything it recovered.
    for (std::size_t i = 0; i < 4; ++i) {
        std::move(node->submit_command(payload(100 + i, 32), std::chrono::seconds{5})).get();
    }
    driver.stop();
    node->stop();

    BOOST_TEST_MESSAGE("second run entries=" << second_run._entries.load()
                                             << " covered=" << second_run._covered.load());
    BOOST_CHECK_EQUAL(second_run._covered.load(), second_run._entries.load());
}

#ifdef FIU_ENABLE

// ─────────────────────────────────────────────────────────────────────────────
// The ordering rule, and the coalescing it makes safe
// ─────────────────────────────────────────────────────────────────────────────
//
// Both of these need a barrier that takes a knowable amount of time, because
// both are about what happens to a write that arrives *while one is running*.
// `raft/persistence/barrier_delay` carries its duration in the fail number.

namespace {

/// RAII around one fiu fault point, so a failed assertion cannot leave it
/// armed for the next test case.
class armed_fault {
public:
    armed_fault(const char* name, int failnum) : _name(name) {
        BOOST_REQUIRE_EQUAL(::fiu_enable(name, failnum, nullptr, 0), 0);
    }
    ~armed_fault() { ::fiu_disable(_name); }
    armed_fault(const armed_fault&) = delete;
    auto operator=(const armed_fault&) -> armed_fault& = delete;

private:
    const char* _name;
};

}  // namespace

/// **Requirement 3.4 — a waiter is never released by a barrier that began
/// before its own write landed, and it is impossible rather than unlikely.**
///
/// The shape of the test is the shape of the bug it excludes. One thread takes
/// a barrier and is held inside the syscall. A second write lands *during* that
/// syscall — bytes the `fsync` provably never saw. If the implementation
/// sampled the highest sequence **after** the syscall instead of before it,
/// the running barrier would publish a watermark covering that second write and
/// the second waiter would be released having never been made durable, with one
/// `fsync` for two writes.
///
/// So the assertion is a syscall count: the second waiter must have cost a
/// second barrier. One would mean the rule was broken.
BOOST_AUTO_TEST_CASE(a_barrier_does_not_cover_a_write_that_raced_into_its_syscall,
                     *boost::unit_test::timeout(60)) {
    temp_dir dir{"ordering"};
    engine_type engine{dir.path};

    constexpr int k_delay_ms = 300;
    armed_fault delay{"raft/persistence/barrier_delay", k_delay_ms};

    const auto seq_a = engine.append_log_entry_sequenced(make_entry(1, 1));
    std::atomic<bool> a_done{false};
    std::thread first([&] {
        engine.barrier_through(seq_a);
        a_done.store(true, std::memory_order_release);
    });

    // Inside the syscall now, and staying there for the rest of the delay.
    std::this_thread::sleep_for(std::chrono::milliseconds{k_delay_ms / 3});
    BOOST_CHECK(!a_done.load(std::memory_order_acquire));

    const auto seq_b = engine.append_log_entry_sequenced(make_entry(1, 2));
    BOOST_REQUIRE_GT(seq_b, seq_a);
    engine.barrier_through(seq_b);

    first.join();

    // Two writes, two barriers. A single barrier here would mean the second
    // write was credited to a syscall that started before it existed.
    BOOST_CHECK_GE(engine.barriers_issued(), 2u);
    BOOST_CHECK_GE(engine.durable_through(), seq_b);
}

/// **Requirement 3.1/3.2 — several concurrent appends satisfied by one barrier,
/// and both halves asserted.**
///
/// Fewer barriers than appends, *and* every append still covered. Either alone
/// is satisfiable by a broken implementation: a barrier that covers nothing is
/// very cheap, and a barrier per append covers everything.
///
/// The injected delay is what makes the coalescing deterministic rather than a
/// race this machine happened to win. Requirement 3.2 is the reason it can
/// coalesce at all — `barrier_through` releases the engine's lock across the
/// syscall, so the other appends land and the other waiters queue while one
/// `fsync` is in flight.
BOOST_AUTO_TEST_CASE(concurrent_appends_coalesce_into_fewer_barriers,
                     *boost::unit_test::timeout(60)) {
    temp_dir dir{"groupcommit"};
    engine_type engine{dir.path};

    constexpr int k_delay_ms = 200;
    constexpr std::size_t k_threads = 8;
    armed_fault delay{"raft/persistence/barrier_delay", k_delay_ms};

    std::atomic<kythira::write_sequence> highest{0};
    std::vector<std::thread> writers;
    writers.reserve(k_threads);
    for (std::size_t i = 0; i < k_threads; ++i) {
        writers.emplace_back([&engine, &highest, i] {
            const auto seq =
                engine.append_log_entry_sequenced(make_entry(1, static_cast<std::uint64_t>(i + 1)));
            auto prev = highest.load(std::memory_order_relaxed);
            while (prev < seq && !highest.compare_exchange_weak(prev, seq)) {
            }
            engine.barrier_through(seq);
        });
    }
    for (auto& t : writers) {
        t.join();
    }

    const auto barriers = engine.barriers_issued();
    BOOST_TEST_MESSAGE("appends=" << k_threads << " barriers=" << barriers);
    BOOST_CHECK_LT(barriers, k_threads);
    BOOST_CHECK_GE(barriers, 1u);
    BOOST_CHECK_GE(engine.durable_through(), highest.load());
    for (std::uint64_t i = 1; i <= k_threads; ++i) {
        BOOST_CHECK(engine.get_log_entry(i).has_value());
    }
}

/// **Requirement 1.5 — a failed barrier is surfaced, and the append is not
/// advertised.**
///
/// At the engine, the failure is an exception rather than a log line. At the
/// node, it reaches the client as a failed future — and the entry does not
/// commit, because `_durable_log_index` was never advanced and the leader
/// therefore withholds its own acknowledgement from the quorum that would have
/// committed it. In a one-node cluster that acknowledgement is the entire
/// quorum, which is what makes the effect visible here at all.
BOOST_AUTO_TEST_CASE(a_failed_barrier_is_surfaced_and_not_advertised,
                     *boost::unit_test::timeout(60)) {
    {
        temp_dir dir{"failengine"};
        engine_type engine{dir.path};
        armed_fault fail{"raft/persistence/barrier", 1};
        const auto seq = engine.append_log_entry_sequenced(make_entry(1, 1));
        BOOST_CHECK_THROW(engine.barrier_through(seq), kythira::persistence_exception);
    }

    temp_dir dir{"failnode"};
    barrier_counters counters;
    network_simulator::NetworkSimulator<raft_network_types> sim;
    auto node = start_single_node(sim, dir.path, &counters);
    node_driver driver{*node};
    BOOST_REQUIRE(await_leader(*node, std::chrono::seconds{10}));

    // Settle first. A new leader appends a no-op entry, which is barriered like
    // everything else and commits a tick or two later; arming the fault while
    // that is still in flight would make this case assert on the no-op rather
    // than on the command it is about.
    const auto settled = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < settled &&
           node->debug_state().commit_index != node->last_log_index()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto submitted_index = node->last_log_index() + 1;

    armed_fault fail{"raft/persistence/barrier", 1};

    bool threw = false;
    try {
        std::move(node->submit_command(payload(1, 32), std::chrono::seconds{3})).get();
    } catch (const std::exception&) {
        threw = true;
    }
    BOOST_CHECK(threw);

    // Give the driver several ticks to commit it if it were going to. It must
    // not: the leader never advanced `_durable_log_index` past the failed
    // barrier, so it withholds its own acknowledgement — and in a one-node
    // cluster that acknowledgement is the whole quorum.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    BOOST_CHECK_LT(node->debug_state().commit_index, submitted_index);

    driver.stop();
    node->stop();
}

#endif  // FIU_ENABLE

BOOST_AUTO_TEST_SUITE_END()
