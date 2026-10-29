// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// A follower's AppendEntries barrier, taken with `_mutex` released, outlived
// by a newer leader's AppendEntries that truncates the entries it covered.
//
// A barriered engine that can block (the object store with write-behind)
// makes `handle_append_entries` drop the node's lock while it waits, so that
// concurrent appends join one flush. Over a transport that serves RPCs
// concurrently, a newer leader's AppendEntries can run in that window: it
// steps the node to the newer term, truncates the conflicting entry and
// appends its own. The first request then woke up and answered **success in
// the old term**, so the old leader counted this node as holding an entry it
// had just thrown away, and could commit and acknowledge a write that the
// newer leader's log does not contain. It also raised `_durable_log_index`
// over the replacement entry, so a later heartbeat was answered success with
// no barrier at all even when the replacement's own barrier had failed.
//
// The simulator transport serves one RPC at a time, so it cannot interleave
// the two; this file registers the node's handlers on a server of its own and
// calls them from two threads, with an engine whose barrier the test holds
// open. Found hunting the lost acknowledged write in
// `multi_raft_node_s3_process_test` (write-behind, leader SIGKILL; see
// doc/TODO.md).
#define BOOST_TEST_MODULE raft_barrier_outlived_by_new_term_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

#include <raft/persistence.hpp>
#include <raft/raft.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>

#endif
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = 1;
        char* argv0[] = {const_cast<char*>("raft_barrier_outlived_by_new_term_test"), nullptr};
        char** argv = argv0;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif

namespace {

using memory_engine_t =
    kythira::memory_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
using log_entry_t = memory_engine_t::log_entry_t;
using snapshot_t = memory_engine_t::snapshot_t;
using sm_t = kythira::test_key_value_state_machine<std::uint64_t>;
using ae_request_t = kythira::append_entries_request<>;
using ae_response_t = kythira::append_entries_response<>;

constexpr std::uint64_t k_snapshot_index = 10;
constexpr std::uint64_t k_seed_term = 1;
constexpr auto k_wait = std::chrono::seconds{10};

/// An AppendEntries running on a thread of its own; `get()` joins it.
class in_flight_append {
public:
    explicit in_flight_append(std::function<ae_response_t()> call)
        : _thread([this, call = std::move(call)] { _response = call(); }) {}
    auto get() -> ae_response_t {
        _thread.join();
        BOOST_REQUIRE(_response.has_value());
        return *_response;
    }

private:
    std::optional<ae_response_t> _response;
    std::thread _thread;
};

/// State shared by the engine `node` owns and the test.
struct engine_state {
    std::mutex mu;
    std::condition_variable cv;
    memory_engine_t store;
    kythira::write_sequence seq{0};

    /// The next barrier blocks until `release_held` is set.
    bool hold_next{false};
    bool holding{false};
    bool release_held{false};
    /// Barriers after the held one that throw instead of returning.
    std::size_t refuse{0};
    std::size_t barriers_taken{0};
};

/// A memory engine with a barrier that blocks, as the object store's does
/// under write-behind, and that the test can hold open or refuse.
class gated_engine {
public:
    using log_entry_t = ::log_entry_t;
    using snapshot_t = ::snapshot_t;

    explicit gated_engine(std::shared_ptr<engine_state> s) : _s(std::move(s)) {}

    auto save_current_term(std::uint64_t term) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.save_current_term(term);
    }
    auto load_current_term() -> std::uint64_t {
        const std::lock_guard lock(_s->mu);
        return _s->store.load_current_term();
    }
    auto save_voted_for(std::uint64_t node) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.save_voted_for(node);
    }
    auto load_voted_for() -> std::optional<std::uint64_t> {
        const std::lock_guard lock(_s->mu);
        return _s->store.load_voted_for();
    }
    auto append_log_entry(const log_entry_t& entry) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.append_log_entry(entry);
    }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        const std::lock_guard lock(_s->mu);
        return _s->store.get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        const std::lock_guard lock(_s->mu);
        return _s->store.get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t {
        const std::lock_guard lock(_s->mu);
        return _s->store.get_last_log_index();
    }
    auto truncate_log(std::uint64_t index) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.truncate_log(index);
    }
    auto save_snapshot(const snapshot_t& snap) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.save_snapshot(snap);
    }
    auto load_snapshot() -> std::optional<snapshot_t> {
        const std::lock_guard lock(_s->mu);
        return _s->store.load_snapshot();
    }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        const std::lock_guard lock(_s->mu);
        _s->store.delete_log_entries_before(index);
    }

    auto append_log_entry_sequenced(const log_entry_t& entry) -> kythira::write_sequence {
        const std::lock_guard lock(_s->mu);
        _s->store.append_log_entry(entry);
        return ++_s->seq;
    }
    auto barrier_through(kythira::write_sequence) -> void {
        std::unique_lock lock(_s->mu);
        ++_s->barriers_taken;
        if (_s->hold_next) {
            _s->hold_next = false;
            _s->holding = true;
            _s->cv.notify_all();
            _s->cv.wait(lock, [&] { return _s->release_held; });
            _s->holding = false;
            return;
        }
        if (_s->refuse > 0) {
            --_s->refuse;
            throw std::runtime_error("gated_engine: barrier refused");
        }
    }
    [[nodiscard]] auto durable_through() const -> kythira::write_sequence {
        const std::lock_guard lock(_s->mu);
        return _s->seq;
    }
    [[nodiscard]] static auto durability() -> kythira::durability_class {
        return kythira::durability_class::barrier;
    }

private:
    std::shared_ptr<engine_state> _s;
};

static_assert(kythira::barriered_persistence_engine<gated_engine>);

/// The handlers `node` registers, kept for the test to call on threads of its
/// own: the concurrency of a real RPC server, without one.
struct registered_handlers {
    std::function<ae_response_t(const ae_request_t&)> append_entries;
};

class capturing_server {
public:
    explicit capturing_server(std::shared_ptr<registered_handlers> h) : _h(std::move(h)) {}

    template<typename F> auto register_request_vote_handler(F) -> void {}
    template<typename F> auto register_install_snapshot_handler(F) -> void {}
    auto register_append_entries_handler(std::function<ae_response_t(const ae_request_t&)> f)
        -> void {
        _h->append_entries = std::move(f);
    }
    auto start() -> void { _running = true; }
    auto stop() -> void { _running = false; }
    [[nodiscard]] auto is_running() const -> bool { return _running; }

private:
    std::shared_ptr<registered_handlers> _h;
    bool _running{false};
};

/// A client that reaches nobody. The node under test is a follower that never
/// campaigns, so it has nothing to send.
class unreachable_client {
public:
    template<typename Request>
    auto send_request_vote(std::uint64_t, const Request&, std::chrono::milliseconds)
        -> kythira::future_default<kythira::request_vote_response<>> {
        return fail<kythira::request_vote_response<>>();
    }
    template<typename Request>
    auto send_append_entries(std::uint64_t, const Request&, std::chrono::milliseconds)
        -> kythira::future_default<ae_response_t> {
        return fail<ae_response_t>();
    }
    template<typename Request>
    auto send_install_snapshot(std::uint64_t, const Request&, std::chrono::milliseconds)
        -> kythira::future_default<kythira::install_snapshot_response<>> {
        return fail<kythira::install_snapshot_response<>>();
    }

private:
    template<typename T> static auto fail() -> kythira::future_default<T> {
        kythira::promise_default<T> promise;
        auto future = promise.getFuture();
        promise.setException(
            std::make_exception_ptr(std::runtime_error("unreachable_client: no peers")));
        return future;
    }
};

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

struct test_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = unreachable_client;
    using network_server_type = capturing_server;

    using persistence_engine_type = gated_engine;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = sm_t;
    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type>;
    using append_entries_request_type = ae_request_t;
    using append_entries_response_type = ae_response_t;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type>;
    using install_snapshot_response_type = kythira::install_snapshot_response<term_id_type>;

    using address_type = std::string;
    using peer_discovery_type = preset_peer_discovery<node_id_type, address_type>;
    using cluster_join_request_type = kythira::cluster_join_request<node_id_type, address_type>;
    using cluster_join_response_type = kythira::cluster_join_response<node_id_type, address_type>;
};

using node_t = kythira::node<test_types>;

/// Node 3 of {1, 2, 3}, a follower at term 1 holding a snapshot through 10
/// and nothing after it. It never campaigns within a test.
struct follower {
    std::shared_ptr<engine_state> state = std::make_shared<engine_state>();
    std::shared_ptr<registered_handlers> handlers = std::make_shared<registered_handlers>();
    std::unique_ptr<node_t> node;

    follower() {
        state->store.save_current_term(k_seed_term);
        sm_t sm;
        state->store.save_snapshot(snapshot_t{
            ._last_included_index = k_snapshot_index,
            ._last_included_term = k_seed_term,
            ._configuration =
                kythira::cluster_configuration<std::uint64_t>{{1, 2, 3}, false, std::nullopt},
            ._state_machine_state = sm.get_state()});

        kythira::raft_configuration cfg;
        cfg._election_timeout_min = std::chrono::milliseconds{60000};
        cfg._election_timeout_max = std::chrono::milliseconds{70000};
        cfg._heartbeat_interval = std::chrono::milliseconds{50};
        cfg._rpc_timeout = std::chrono::milliseconds{200};
        node = std::make_unique<node_t>(kythira::node_config<test_types>{
            .node_id = 3,
            .network_client = unreachable_client{},
            .network_server = capturing_server{handlers},
            .persistence = gated_engine{state},
            .logger = kythira::console_logger{kythira::log_level::info},
            .metrics = test_types::metrics_type{},
            .membership = test_types::membership_manager_type{},
            .config = cfg,
            .self_address = "3",
            .peer_discovery = preset_peer_discovery<std::uint64_t, std::string>{}});
        node->start();
        BOOST_REQUIRE(handlers->append_entries);
    }
    ~follower() {
        release();
        node->stop();
    }
    follower(const follower&) = delete;
    auto operator=(const follower&) -> follower& = delete;
    follower(follower&&) = delete;
    auto operator=(follower&&) -> follower& = delete;

    auto append(const ae_request_t& request) -> ae_response_t {
        return handlers->append_entries(request);
    }

    /// Hold the next barrier open and send `request` on a thread of its own;
    /// returns once its barrier is waiting.
    auto append_held(const ae_request_t& request) -> std::unique_ptr<in_flight_append> {
        {
            const std::lock_guard lock(state->mu);
            state->hold_next = true;
        }
        auto response =
            std::make_unique<in_flight_append>([this, request] { return append(request); });
        std::unique_lock lock(state->mu);
        BOOST_REQUIRE(state->cv.wait_for(lock, k_wait, [&] { return state->holding; }));
        return response;
    }

    auto release() -> void {
        const std::lock_guard lock(state->mu);
        state->release_held = true;
        state->cv.notify_all();
    }
};

/// One entry at index 11 in `term`, sent by `leader` in `term`.
auto carrying_entry_11(std::uint64_t term, std::uint64_t leader) -> ae_request_t {
    return ae_request_t{
        ._term = term,
        ._leader_id = leader,
        ._prev_log_index = k_snapshot_index,
        ._prev_log_term = k_seed_term,
        ._entries = {log_entry_t{term, k_snapshot_index + 1,
                                 sm_t::make_put_command("k" + std::to_string(term), "v")}},
        ._leader_commit = k_snapshot_index};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(raft_barrier_outlived_by_new_term)

// Leader 1 (term 2) sends entry 11; its barrier is held open. Leader 2
// (term 3) then replaces entry 11 with its own. Leader 1's request must not be
// answered success in term 2: node 3 no longer holds the entry it carried.
BOOST_AUTO_TEST_CASE(an_old_leaders_append_is_not_acknowledged_after_a_newer_one_truncated_it,
                     *boost::unit_test::timeout(60)) {
    follower f;
    auto old_leader = f.append_held(carrying_entry_11(2, 1));

    const auto newer = f.append(carrying_entry_11(3, 2));
    BOOST_TEST(newer.success());
    BOOST_TEST(newer.term() == 3U);

    f.release();
    const auto answer = old_leader->get();
    BOOST_TEST(!answer.success(), "the term-2 append was acknowledged after term 3 replaced it");
    BOOST_TEST(answer.term() == 3U);

    const std::lock_guard lock(f.state->mu);
    const auto held = f.state->store.get_log_entry(k_snapshot_index + 1);
    BOOST_REQUIRE(held.has_value());
    BOOST_TEST(held->term() == 3U);
}

// The same race with the newer leader's barrier failing: node 3 holds its
// entry 11 but has not made it durable. The old request's barrier, which
// covered the entry it replaced, must not count for it, so the newer leader's
// heartbeat has to take a barrier of its own before it is answered success.
BOOST_AUTO_TEST_CASE(a_barrier_for_truncated_entries_does_not_cover_their_replacement,
                     *boost::unit_test::timeout(60)) {
    follower f;
    auto old_leader = f.append_held(carrying_entry_11(2, 1));
    {
        const std::lock_guard lock(f.state->mu);
        f.state->refuse = 1;
    }
    BOOST_TEST(!f.append(carrying_entry_11(3, 2)).success());

    f.release();
    BOOST_TEST(!old_leader->get().success());

    std::size_t before = 0;
    {
        const std::lock_guard lock(f.state->mu);
        before = f.state->barriers_taken;
    }
    const auto heartbeat = f.append(ae_request_t{._term = 3,
                                                 ._leader_id = 2,
                                                 ._prev_log_index = k_snapshot_index + 1,
                                                 ._prev_log_term = 3,
                                                 ._entries = {},
                                                 ._leader_commit = k_snapshot_index});
    BOOST_TEST(heartbeat.success());
    const std::lock_guard lock(f.state->mu);
    BOOST_TEST(f.state->barriers_taken == before + 1,
               "the heartbeat was answered on the truncated entry's barrier");
}

BOOST_AUTO_TEST_SUITE_END()
