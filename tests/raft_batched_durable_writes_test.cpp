// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `node`'s side of `.kiro/specs/batched-durable-writes/`: which persistence
// calls a running cluster actually makes.
//
// Three properties, each on a real three-node cluster over the network
// simulator, with an engine that records every write:
//
//  1. A follower on an engine with `append_log_entries` and no barrier takes
//     the entries of an AppendEntries in one call, never one at a time.
//  2. On an engine without `save_hard_state`, a vote is written after the term
//     it was cast in. The grant path used to write the vote first; a crash
//     between the two writes then left the vote beside the previous term,
//     where it read as a second vote in a term the node had already voted in.
//  3. On an engine with `save_hard_state`, a follower that moves to a newer
//     term without voting stores "no vote", rather than leaving its previous
//     term's vote on storage beside the new term.
//  4. That holds however the node learns the term: a refused RequestVote or
//     an AppendEntries that brings no new entries stores it before the node
//     answers, which used to happen only when the log changed or it voted.
//  5. A candidate that steps down to its own term's leader keeps its vote for
//     itself, so it cannot vote a second time in that term.
//
// Engine-level behaviour of the two calls is in
// batched_durable_writes_unit_test.cpp.
#define BOOST_TEST_MODULE raft_batched_durable_writes_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// ── Folly global fixture ───────────────────────────────────────────────────

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_batched_durable_writes_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using memory_engine_t = kythira::memory_persistence_engine<>;
using log_entry_t = memory_engine_t::log_entry_t;
using snapshot_t = memory_engine_t::snapshot_t;

/// Every write an engine was asked to make, in order, and the state they left.
/// Shared between the engine `node` owns and the test, which reads it.
struct write_record {
    std::mutex mu;
    std::vector<std::string> ops;
    memory_engine_t state;

    auto note(std::string op) -> void { ops.push_back(std::move(op)); }

    [[nodiscard]] auto snapshot_ops() -> std::vector<std::string> {
        const std::lock_guard lock(mu);
        return ops;
    }
};

/// A persistence engine that records its writes and keeps its state in a
/// memory engine. It has **no barrier**, so `node` takes the path a cloud
/// object store takes. `Extended` adds `append_log_entries` and
/// `save_hard_state`; without it the engine has only the base concept.
template<bool Extended> class recording_engine {
public:
    using log_entry_t = ::log_entry_t;
    using snapshot_t = ::snapshot_t;

    explicit recording_engine(std::shared_ptr<write_record> record) : _r(std::move(record)) {}

    auto save_current_term(std::uint64_t term) -> void {
        const std::lock_guard lock(_r->mu);
        _r->note("term:" + std::to_string(term));
        _r->state.save_current_term(term);
    }
    auto load_current_term() -> std::uint64_t {
        const std::lock_guard lock(_r->mu);
        return _r->state.load_current_term();
    }
    auto save_voted_for(std::uint64_t node) -> void {
        const std::lock_guard lock(_r->mu);
        _r->note("vote:" + std::to_string(node));
        _r->state.save_voted_for(node);
    }
    auto load_voted_for() -> std::optional<std::uint64_t> {
        const std::lock_guard lock(_r->mu);
        return _r->state.load_voted_for();
    }
    auto append_log_entry(const log_entry_t& entry) -> void {
        const std::lock_guard lock(_r->mu);
        _r->note("append:" + std::to_string(entry.index()));
        _r->state.append_log_entry(entry);
    }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        const std::lock_guard lock(_r->mu);
        return _r->state.get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        const std::lock_guard lock(_r->mu);
        return _r->state.get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t {
        const std::lock_guard lock(_r->mu);
        return _r->state.get_last_log_index();
    }
    auto truncate_log(std::uint64_t index) -> void {
        const std::lock_guard lock(_r->mu);
        _r->note("truncate:" + std::to_string(index));
        _r->state.truncate_log(index);
    }
    auto save_snapshot(const snapshot_t& snap) -> void {
        const std::lock_guard lock(_r->mu);
        _r->state.save_snapshot(snap);
    }
    auto load_snapshot() -> std::optional<snapshot_t> {
        const std::lock_guard lock(_r->mu);
        return _r->state.load_snapshot();
    }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        const std::lock_guard lock(_r->mu);
        _r->state.delete_log_entries_before(index);
    }

    auto append_log_entries(std::span<const log_entry_t> entries) -> void
    requires Extended
    {
        const std::lock_guard lock(_r->mu);
        _r->note("append_run:" + std::to_string(entries.size()));
        _r->state.append_log_entries(entries);
    }
    auto save_hard_state(std::uint64_t term, std::optional<std::uint64_t> vote) -> void
    requires Extended
    {
        const std::lock_guard lock(_r->mu);
        _r->note("hard:" + std::to_string(term) + ":" +
                 (vote ? std::to_string(*vote) : std::string("none")));
        _r->state.save_hard_state(term, vote);
    }

private:
    std::shared_ptr<write_record> _r;
};

static_assert(!kythira::barriered_persistence_engine<recording_engine<true>>);
static_assert(kythira::bulk_append_persistence_engine<recording_engine<true>>);
static_assert(kythira::hard_state_persistence_engine<recording_engine<true>>);
static_assert(!kythira::bulk_append_persistence_engine<recording_engine<false>>);
static_assert(!kythira::hard_state_persistence_engine<recording_engine<false>>);

template<typename NodeId, typename Address> class preset_peer_discovery {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    auto register_node(NodeId, Address) -> kythira::future_default<void> {
        return kythira::future_factory_default::makeFuture();
    }
    [[nodiscard]] auto find_peers(std::chrono::milliseconds) const
        -> kythira::future_default<std::vector<kythira::peer_info<NodeId, Address>>> {
        return kythira::future_factory_default::makeFuture(
            std::vector<kythira::peer_info<NodeId, Address>>{});
    }
};

template<bool Extended> struct test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using raft_network_types = kythira::raft_simulator_network_types<std::string>;
    using network_client_type =
        kythira::simulator_network_client<raft_network_types, serializer_type,
                                          serialized_data_type>;
    using network_server_type =
        kythira::simulator_network_server<raft_network_types, serializer_type,
                                          serialized_data_type>;

    using persistence_engine_type = recording_engine<Extended>;
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

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

using sm_t = kythira::test_key_value_state_machine<std::uint64_t>;

constexpr std::uint64_t k_snapshot_index = 10;
constexpr std::uint64_t k_seed_term = 1;
constexpr std::uint64_t k_leader_trailing = 5;

template<typename Pred>
auto wait_until(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000})
    -> bool {
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return true;
}

/// Node 1 is the only node whose election timer can fire within a test: the
/// other two would otherwise race it, and a node 2 elected with node 3's vote
/// would truncate the entries node 1 is seeded with.
auto make_config(std::uint64_t id) -> kythira::raft_configuration {
    kythira::raft_configuration cfg;
    const bool candidate = id == 1;
    cfg._election_timeout_min = std::chrono::milliseconds{candidate ? 80 : 30000};
    cfg._election_timeout_max = std::chrono::milliseconds{candidate ? 160 : 40000};
    cfg._heartbeat_interval = std::chrono::milliseconds{26};
    cfg._rpc_timeout = std::chrono::milliseconds{200};
    cfg._bootstrap_retry_interval = std::chrono::milliseconds{200};
    cfg._bootstrap_peer_find_timeout = std::chrono::milliseconds{100};
    return cfg;
}

/// Every node starts from a snapshot naming {1, 2, 3} at term 1. Node 1 also
/// holds `k_leader_trailing` entries after it, which the others must receive
/// from it. `vote` is seeded as the node's vote in term 1.
auto seed(write_record& record, std::uint64_t id, std::optional<std::uint64_t> vote) -> void {
    record.state.save_current_term(k_seed_term);
    if (vote) {
        record.state.save_voted_for(*vote);
    }
    sm_t sm;
    record.state.save_snapshot(snapshot_t{
        ._last_included_index = k_snapshot_index,
        ._last_included_term = k_seed_term,
        ._configuration =
            kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt},
        ._state_machine_state = sm.get_state()});
    if (id == 1) {
        for (std::uint64_t i = 1; i <= k_leader_trailing; ++i) {
            record.state.append_log_entry(
                log_entry_t{k_seed_term, k_snapshot_index + i,
                            sm_t::make_put_command("seed" + std::to_string(i), "v")});
        }
    }
}

template<bool Extended> struct cluster {
    using types = test_types<Extended>;
    using node_t = kythira::node<types>;
    using sim_t = network_simulator::NetworkSimulator<typename types::raft_network_types>;

    sim_t sim;
    std::vector<std::shared_ptr<write_record>> records;
    std::vector<std::unique_ptr<node_t>> nodes;

    /// `connected` lists the nodes wired to each other at the start; the rest
    /// are joined later with `connect()`.
    explicit cluster(std::vector<std::uint64_t> connected,
                     std::optional<std::uint64_t> node3_vote = std::nullopt) {
        sim.start();
        for (const auto from : connected) {
            for (const auto to : connected) {
                if (from != to) {
                    sim.add_edge(std::to_string(from), std::to_string(to),
                                 network_simulator::NetworkEdge{});
                }
            }
        }
        for (std::uint64_t id = 1; id <= 3; ++id) {
            auto record = std::make_shared<write_record>();
            seed(*record, id, id == 3 ? node3_vote : std::nullopt);
            records.push_back(record);
            auto net = sim.create_node(std::to_string(id));
            nodes.push_back(std::make_unique<node_t>(kythira::node_config<types>{
                .node_id = id,
                .network_client =
                    typename types::network_client_type{net, typename types::serializer_type{}},
                .network_server =
                    typename types::network_server_type{net, typename types::serializer_type{}},
                .persistence = recording_engine<Extended>{record},
                .logger = kythira::console_logger{kythira::log_level::info},
                .metrics = typename types::metrics_type{},
                .membership = typename types::membership_manager_type{},
                .config = make_config(id),
                .self_address = std::to_string(id),
                .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{}}));
        }
        for (auto& n : nodes) {
            n->start();
        }
        // Writes made while starting up are not what these cases are about.
        for (auto& r : records) {
            const std::lock_guard lock(r->mu);
            r->ops.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{180});
        nodes[0]->check_election_timeout();
    }

    ~cluster() {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            (*it)->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    auto leader() -> node_t& { return *nodes[0]; }

    auto connect(std::uint64_t id) -> void {
        for (std::uint64_t other = 1; other <= 3; ++other) {
            if (other != id) {
                sim.add_edge(std::to_string(id), std::to_string(other),
                             network_simulator::NetworkEdge{});
                sim.add_edge(std::to_string(other), std::to_string(id),
                             network_simulator::NetworkEdge{});
            }
        }
    }

    /// Ticks of the last pump_until() and the longest one, for diagnostics.
    std::size_t pump_ticks = 0;
    std::chrono::milliseconds longest_tick{0};

    /// Drives heartbeats until `pred` holds or `deadline` passes.
    template<typename Pred> auto pump_until(Pred pred, std::chrono::milliseconds deadline) -> bool {
        pump_ticks = 0;
        longest_tick = std::chrono::milliseconds{0};
        return wait_until(
            [&] {
                const auto start = std::chrono::steady_clock::now();
                leader().check_heartbeat_timeout();
                ++pump_ticks;
                longest_tick =
                    std::max(longest_tick, std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - start));
                return pred();
            },
            deadline);
    }

    /// A client on a simulator node of the test's own, wired only to `id`,
    /// for sending that member RPCs by hand.
    auto client_to(std::uint64_t id) -> typename types::network_client_type {
        auto net = sim.create_node("9");
        sim.add_edge("9", std::to_string(id), network_simulator::NetworkEdge{});
        sim.add_edge(std::to_string(id), "9", network_simulator::NetworkEdge{});
        return typename types::network_client_type{net, typename types::serializer_type{}};
    }

    /// Whether node `id`'s engine holds every entry the leader started with.
    auto caught_up(std::uint64_t id) -> bool {
        auto& r = *records[id - 1];
        const std::lock_guard lock(r.mu);
        return r.state.get_last_log_index() >= k_snapshot_index + k_leader_trailing;
    }
};

auto count_prefix(const std::vector<std::string>& ops, std::string_view prefix) -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        ops.begin(), ops.end(), [&](const std::string& op) { return op.starts_with(prefix); }));
}

auto join(const std::vector<std::string>& ops) -> std::string {
    std::string out;
    for (const auto& op : ops) {
        out += out.empty() ? op : " " + op;
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_batched_durable_writes)

BOOST_AUTO_TEST_CASE(a_follower_takes_an_append_entries_run_in_one_call,
                     *boost::unit_test::timeout(30)) {
    cluster<true> c{{1, 2, 3}};
    BOOST_REQUIRE(
        wait_until([&] { return c.leader().is_leader(); }, std::chrono::milliseconds{4000}));
    BOOST_REQUIRE(c.pump_until([&] { return c.caught_up(2) && c.caught_up(3); },
                               std::chrono::milliseconds{5000}));

    for (std::uint64_t id : {2U, 3U}) {
        const auto ops = c.records[id - 1]->snapshot_ops();
        BOOST_TEST_INFO_SCOPE("node " << id << ": " << join(ops));
        BOOST_TEST(count_prefix(ops, "append:") == 0U);
        BOOST_TEST(count_prefix(ops, "append_run:") >= 1U);
        // The leader's whole seeded tail arrives in a run, not entry by entry.
        const bool multi = std::any_of(ops.begin(), ops.end(), [](const std::string& op) {
            return op.starts_with("append_run:") && op != "append_run:1";
        });
        BOOST_TEST(multi);
        // And the term and vote go through the combined call, never the pair.
        BOOST_TEST(count_prefix(ops, "term:") == 0U);
        BOOST_TEST(count_prefix(ops, "vote:") == 0U);
        BOOST_TEST(count_prefix(ops, "hard:") >= 1U);
    }
}

BOOST_AUTO_TEST_CASE(without_save_hard_state_a_vote_is_written_after_its_term,
                     *boost::unit_test::timeout(30)) {
    cluster<false> c{{1, 2, 3}};
    BOOST_REQUIRE(
        wait_until([&] { return c.leader().is_leader(); }, std::chrono::milliseconds{4000}));

    // Node 1 may need more than one election to win, so a voter can hold
    // several term/vote pairs, and a later term can arrive without a vote.
    // What holds for every grant is that its term was written immediately
    // before the vote; the old order wrote the vote first.
    for (std::uint64_t id : {2U, 3U}) {
        const auto ops = c.records[id - 1]->snapshot_ops();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (!ops[i].starts_with("vote:")) {
                continue;
            }
            BOOST_TEST_INFO("node " << id << ", op " << i << " of " << join(ops));
            BOOST_TEST((i > 0 && ops[i - 1].starts_with("term:")));
        }
    }
}

// Node 3 voted for itself in term 1 and is cut off while node 1 is elected with
// node 2's vote. When it rejoins it moves to node 1's term, and what it stores
// must not be its term-1 vote for itself. It may have voted for node 1 in the
// new term — node 1 retries a RequestVote that failed while node 3 was cut off,
// and one can land after the reconnect — but it never voted for itself there.
BOOST_AUTO_TEST_CASE(a_follower_that_moves_terms_does_not_keep_its_previous_vote,
                     *boost::unit_test::timeout(30)) {
    cluster<true> c{{1, 2}, std::optional<std::uint64_t>{3}};
    BOOST_REQUIRE(
        wait_until([&] { return c.leader().is_leader(); }, std::chrono::milliseconds{4000}));
    const auto term = c.leader().get_current_term();
    BOOST_REQUIRE(term > k_seed_term);

    c.connect(3);
    // Node 3 stores the leader's term when it first hears it and its entries
    // later; the test reads both, so it waits for both.
    auto& rec = *c.records[2];
    const bool settled = c.pump_until(
        [&] {
            if (!c.caught_up(3)) {
                return false;
            }
            const std::lock_guard lock(rec.mu);
            return rec.state.load_current_term() == term;
        },
        std::chrono::milliseconds{5000});
    if (!settled) {
        for (std::uint64_t id = 1; id <= 3; ++id) {
            auto& r = *c.records[id - 1];
            const std::lock_guard lock(r.mu);
            const auto v = r.state.load_voted_for();
            std::cerr << "node " << id << ": live term " << c.nodes[id - 1]->get_current_term()
                      << " state " << static_cast<int>(c.nodes[id - 1]->get_state())
                      << ", stored term " << r.state.load_current_term() << " vote "
                      << (v ? std::to_string(*v) : "none") << " last "
                      << r.state.get_last_log_index() << ", ops: " << join(r.ops) << std::endl;
        }
        std::cerr << "pump: " << c.pump_ticks << " ticks, longest " << c.longest_tick.count()
                  << "ms" << std::endl;
        BOOST_FAIL("node 3 did not settle at term " << term);
    }

    const std::lock_guard lock(rec.mu);
    BOOST_TEST(rec.state.load_current_term() == term);
    BOOST_TEST_INFO_SCOPE("node 3: " << join(rec.ops));
    const auto vote = rec.state.load_voted_for();
    BOOST_TEST((!vote.has_value() || *vote == 1U));
    BOOST_TEST(count_prefix(rec.ops, "hard:" + std::to_string(term) + ":none") +
                   count_prefix(rec.ops, "hard:" + std::to_string(term) + ":1") >=
               1U);
}

constexpr auto k_rpc_wait = std::chrono::milliseconds{2000};

// Node 3 voted for itself in term 1 and refuses a RequestVote from a higher
// term because the candidate's log is behind its own. Refusing still moves it
// to that term, and the term and "no vote" are stored before it answers.
BOOST_AUTO_TEST_CASE(a_refused_vote_from_a_newer_term_stores_the_term,
                     *boost::unit_test::timeout(30)) {
    cluster<true> c{{}, std::optional<std::uint64_t>{3}};
    auto client = c.client_to(3);
    constexpr std::uint64_t newer = k_seed_term + 4;

    const auto response =
        client
            .send_request_vote(
                3,
                kythira::request_vote_request<>{
                    ._term = newer, ._candidate_id = 2, ._last_log_index = 0, ._last_log_term = 0},
                k_rpc_wait)
            .get();
    BOOST_TEST(!response.vote_granted());
    BOOST_TEST(response.term() == newer);

    auto& rec = *c.records[2];
    const std::lock_guard lock(rec.mu);
    BOOST_TEST_INFO_SCOPE("node 3: " << join(rec.ops));
    BOOST_TEST(rec.state.load_current_term() == newer);
    BOOST_TEST(!rec.state.load_voted_for().has_value());
    BOOST_TEST(count_prefix(rec.ops, "hard:" + std::to_string(newer) + ":none") == 1U);
}

// The same for an AppendEntries from a newer term that brings nothing node 3
// lacks: a heartbeat at its snapshot boundary. The log does not change, which
// was the only AppendEntries path that stored the term.
BOOST_AUTO_TEST_CASE(a_heartbeat_from_a_newer_term_stores_the_term,
                     *boost::unit_test::timeout(30)) {
    cluster<true> c{{}, std::optional<std::uint64_t>{3}};
    auto client = c.client_to(3);
    constexpr std::uint64_t newer = k_seed_term + 5;

    const auto response = client
                              .send_append_entries(3,
                                                   kythira::append_entries_request<>{
                                                       ._term = newer,
                                                       ._leader_id = 1,
                                                       ._prev_log_index = k_snapshot_index,
                                                       ._prev_log_term = k_seed_term,
                                                       ._entries = {},
                                                       ._leader_commit = k_snapshot_index},
                                                   k_rpc_wait)
                              .get();
    BOOST_TEST(response.success());
    BOOST_TEST(response.term() == newer);

    auto& rec = *c.records[2];
    const std::lock_guard lock(rec.mu);
    BOOST_TEST_INFO_SCOPE("node 3: " << join(rec.ops));
    BOOST_TEST(rec.state.load_current_term() == newer);
    BOOST_TEST(!rec.state.load_voted_for().has_value());
    BOOST_TEST(count_prefix(rec.ops, "append") == 0U);
    BOOST_TEST(count_prefix(rec.ops, "hard:" + std::to_string(newer) + ":none") == 1U);
}

// Node 1 campaigns with node 2's pre-vote but not its vote, so it stays a
// candidate that has voted for itself. Node 2 then claims the term as its
// leader; node 1 steps down within the term and must still refuse node 3,
// whose log is as long as its own. Stepping down used to clear the vote in
// memory, and node 1 granted it: two votes from one node in one term, which
// is how a term gets two leaders.
BOOST_AUTO_TEST_CASE(a_candidate_that_steps_down_in_its_term_keeps_its_vote,
                     *boost::unit_test::timeout(30)) {
    using types = test_types<true>;
    using sim_t = network_simulator::NetworkSimulator<types::raft_network_types>;
    sim_t sim;
    sim.start();
    for (const auto* peer : {"2", "9"}) {
        sim.add_edge("1", peer, network_simulator::NetworkEdge{});
        sim.add_edge(peer, "1", network_simulator::NetworkEdge{});
    }

    // Node 2 is a stand-in that grants pre-votes and refuses votes.
    types::network_server_type node2{sim.create_node("2"), types::serializer_type{}};
    node2.register_request_pre_vote_handler([](const kythira::request_pre_vote_request<>& r) {
        return kythira::request_pre_vote_response<>{._term = r.term() - 1, ._vote_granted = true};
    });
    node2.register_request_vote_handler([](const kythira::request_vote_request<>& r) {
        return kythira::request_vote_response<>{._term = r.term(), ._vote_granted = false};
    });
    node2.start();

    auto record = std::make_shared<write_record>();
    seed(*record, 1, std::nullopt);
    auto net = sim.create_node("1");
    kythira::node<types> node1{kythira::node_config<types>{
        .node_id = 1,
        .network_client = types::network_client_type{net, types::serializer_type{}},
        .network_server = types::network_server_type{net, types::serializer_type{}},
        .persistence = recording_engine<true>{record},
        .logger = kythira::console_logger{kythira::log_level::error},
        .metrics = types::metrics_type{},
        .membership = types::membership_manager_type{},
        .config = make_config(1),
        .self_address = "1",
        .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{}}};
    node1.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{180});
    node1.check_election_timeout();
    BOOST_REQUIRE(wait_until([&] { return node1.get_state() == kythira::server_state::candidate; },
                             std::chrono::milliseconds{4000}));
    const auto term = node1.get_current_term();

    types::network_client_type client{sim.create_node("9"), types::serializer_type{}};
    const auto heartbeat = client
                               .send_append_entries(1,
                                                    kythira::append_entries_request<>{
                                                        ._term = term,
                                                        ._leader_id = 2,
                                                        ._prev_log_index = k_snapshot_index,
                                                        ._prev_log_term = k_seed_term,
                                                        ._entries = {},
                                                        ._leader_commit = k_snapshot_index},
                                                    k_rpc_wait)
                               .get();
    BOOST_TEST(heartbeat.success());
    BOOST_REQUIRE(node1.get_state() == kythira::server_state::follower);
    BOOST_REQUIRE(node1.get_current_term() == term);

    const auto vote =
        client
            .send_request_vote(1,
                               kythira::request_vote_request<>{
                                   ._term = term,
                                   ._candidate_id = 3,
                                   ._last_log_index = k_snapshot_index + k_leader_trailing,
                                   ._last_log_term = k_seed_term},
                               k_rpc_wait)
            .get();
    BOOST_TEST(!vote.vote_granted());

    {
        const std::lock_guard lock(record->mu);
        BOOST_TEST_INFO_SCOPE("node 1: " << join(record->ops));
        BOOST_TEST(record->state.load_current_term() == term);
        BOOST_TEST((record->state.load_voted_for() == std::optional<std::uint64_t>{1}));
    }
    node1.stop();
    node2.stop();
}

BOOST_AUTO_TEST_SUITE_END()
