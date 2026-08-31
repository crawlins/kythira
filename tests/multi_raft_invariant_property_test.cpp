// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_invariant_property_test.cpp
/// @brief The five safety invariants, checked after every operation of a
///        randomised workload (task 31 of `.kiro/specs/multi-raft/`).
///
/// A split/merge bug does not crash. It loses a key, or serves one from two
/// shards, and the system carries on looking healthy until somebody notices
/// their data is wrong — which is why these invariants are checked
/// mechanically rather than reasoned about:
///
///  * **I1 tiling.** Every point in the key space is owned by exactly one
///    shard. `shard_map::check_tiling()` names the first gap or overlap it
///    finds, so a failure says *where*.
///  * **I2 no loss or duplication.** A shadow `key → value` model, maintained
///    by the test, must equal the union of every shard's `get_state()` — and
///    no key may appear in two shards.
///  * **I3 epoch monotonicity.** A shard's epoch version never decreases.
///  * **I4 stale-epoch rejection.** A request computed against a superseded
///    epoch is always refused, never served.
///  * **I5 the round-trip law.** `absorb` is the exact inverse of
///    `split_state`; this is the contract that makes a split lossless, and the
///    only one the host cannot check for itself.
///
/// The workload is randomised but **seeded**: a failure reproduces by rerunning
/// the same binary. Each case prints its seed so a future failure can be
/// replayed without guesswork.

#define BOOST_TEST_MODULE multi_raft_invariant_property_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_invariant_property_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_epoch;
using kythira::tombstone_reason;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;

/// The host's shared-transport bundle. Note that the client and server named
/// here are the *shared* ones; `multi_raft` derives each group's scoped views
/// from them.
struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;

auto fast_raft_config() -> kythira::raft_configuration {
    kythira::raft_configuration c;
    c._election_timeout_min = std::chrono::milliseconds{40};
    c._election_timeout_max = std::chrono::milliseconds{80};
    c._heartbeat_interval = std::chrono::milliseconds{10};
    return c;
}

auto make_config(message_fabric& fabric, std::uint64_t node_id) -> config_type {
    config_type cfg{
        .node_id = node_id,
        .network_client = fabric_client{fabric, node_id},
        .network_server = fabric_server{fabric, node_id},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.config = fast_raft_config();
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 4;
    return cfg;
}

using kythira::shard_descriptor;
using kythira::shard_range;
using state_machine_type = host_types::state_machine_type;
using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;

constexpr group_id_type k_root = 1;

/// The state machine's command encoding, mirrored here so the test can build
/// commands without reaching into the state machine's private helpers.
auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    return state_machine_type::make_put_command(key, value);
}

/// @brief One host, one node, many shards, driven by hand.
///
/// A single node per group on purpose. These invariants are properties of the
/// routing table and of the state machines' contents, not of consensus; adding
/// replicas would add election timing to every case without adding a single
/// invariant, and a property test that fails intermittently for reasons
/// unrelated to the property is worse than no property test.
class invariant_host {
public:
    explicit invariant_host(std::function<void(config_type&)> tweak = {}) {
        config_type cfg{
            .node_id = 1,
            .network_client = fabric_client{_fabric, 1},
            .network_server = fabric_server{_fabric, 1},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{40};
        cfg.config._election_timeout_max = std::chrono::milliseconds{80};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        cfg.allocate_group_ids = [this](std::size_t n) {
            std::vector<group_id_type> out;
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(_next_id++);
            }
            return out;
        };
        if (tweak) {
            tweak(cfg);
        }
        _host = std::make_unique<host_type>(std::move(cfg));
        _host->create_group(descriptor_type{._group_id = k_root,
                                            ._range = kythira::unbounded_shard_range<key_type>(),
                                            ._epoch = kythira::shard_epoch{},
                                            ._voters = {1},
                                            ._learners = {},
                                            ._leader_hint = std::nullopt});
        _host->start();
    }

    ~invariant_host() { _host->stop(); }

    invariant_host(const invariant_host&) = delete;
    auto operator=(const invariant_host&) -> invariant_host& = delete;

    [[nodiscard]] auto host() -> host_type& { return *_host; }

    auto await_leaders() -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            _host->tick();
            bool all = true;
            for (auto g : _host->group_ids()) {
                auto* n = _host->group_node(g);
                if (n == nullptr || !n->is_leader()) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    /// Resolve a future while keeping the host ticking underneath it.
    template<typename Future> auto settle(Future&& f) -> std::exception_ptr {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{6};
        while (!f.wait(std::chrono::milliseconds{2}) &&
               std::chrono::steady_clock::now() < deadline) {
            _host->tick();
        }
        if (!f.wait(std::chrono::milliseconds{200})) {
            return std::make_exception_ptr(std::runtime_error("never resolved"));
        }
        try {
            std::ignore = std::move(f).get();
            return nullptr;
        } catch (...) {
            return std::current_exception();
        }
    }

    /// Write through the routing layer, recording it in the shadow model on
    /// success. A failed write is NOT recorded — that is the whole point of
    /// keeping the model here rather than assuming every submission landed.
    auto put(const key_type& key, const std::string& value) -> bool {
        auto error = settle(
            _host->submit_command(key, put_command(key, value), std::chrono::milliseconds{3000}));
        if (error == nullptr) {
            _model[key] = value;
            return true;
        }
        return false;
    }

    [[nodiscard]] auto model() const -> const std::map<key_type, std::string>& { return _model; }

private:
    message_fabric _fabric{2};
    std::unique_ptr<host_type> _host;
    std::map<key_type, std::string> _model;
    group_id_type _next_id{100};
};

// ── the invariants ───────────────────────────────────────────────────────────

/// I1: every point in the key space is owned by exactly one shard.
auto describe_bound(const std::optional<key_type>& b) -> std::string {
    return b.has_value() ? *b : std::string{"inf"};
}

auto dump(host_type& host) -> std::string {
    std::string out = "\n  routing rows:";
    for (const auto& d : host.shard_map_snapshot().descriptors()) {
        out += "\n    g=" + std::to_string(d._group_id) + " [" + describe_bound(d._range._start) +
               "," + describe_bound(d._range._end) + ") v=" + std::to_string(d._epoch._version);
    }
    out += "\n  local groups:";
    for (auto g : host.group_ids()) {
        auto d = host.local_descriptor(g);
        out += "\n    g=" + std::to_string(g);
        if (d.has_value()) {
            out += " [" + describe_bound(d->_range._start) + "," + describe_bound(d->_range._end) +
                   ") v=" + std::to_string(d->_epoch._version);
        }
    }
    return out;
}

auto check_tiling(host_type& host, const std::string& where) -> void {
    const auto problem = host.shard_map_snapshot().check_tiling();
    BOOST_REQUIRE_MESSAGE(!problem.has_value(), "I1 tiling broken after " << where << ": "
                                                                          << problem.value_or("")
                                                                          << dump(host));
}

/// The union of every local shard's state, and the shards each key was found
/// in. Returned together so I2's two halves — completeness and uniqueness —
/// come from one walk.
struct union_state {
    std::map<key_type, std::string> _merged;
    std::map<key_type, std::vector<group_id_type>> _owners;
};

auto collect(host_type& host) -> union_state {
    union_state out;
    for (auto g : host.group_ids()) {
        auto* node = host.group_node(g);
        if (node == nullptr) {
            continue;
        }
        std::map<key_type, std::string> store;
        node->with_state_machine([&](state_machine_type& sm) {
            store = state_machine_type::deserialize_store(sm.get_state());
        });
        for (const auto& [k, v] : store) {
            out._merged[k] = v;
            out._owners[k].push_back(g);
        }
    }
    return out;
}

/// I2: no loss and no duplication, against the shadow model.
auto check_no_loss_or_duplication(host_type& host, const std::map<key_type, std::string>& model,
                                  const std::string& where) -> void {
    const auto state = collect(host);

    for (const auto& [key, value] : model) {
        auto it = state._merged.find(key);
        BOOST_REQUIRE_MESSAGE(it != state._merged.end(),
                              "I2 lost key '" << key << "' after " << where);
        BOOST_REQUIRE_MESSAGE(it->second == value,
                              "I2 wrong value for '" << key << "' after " << where << ": expected '"
                                                     << value << "', found '" << it->second << "'");
    }
    BOOST_REQUIRE_MESSAGE(
        state._merged.size() == model.size(),
        "I2 invented " << state._merged.size() - model.size() << " key(s) after " << where);

    for (const auto& [key, owners] : state._owners) {
        BOOST_REQUIRE_MESSAGE(owners.size() == 1, "I2 key '" << key << "' is held by "
                                                             << owners.size() << " shards after "
                                                             << where);
    }
}

/// I2, continued: a key must live in the shard whose range actually contains
/// it. A key in the right *count* of shards but the wrong one is still a bug —
/// the next split would move it somewhere else again.
auto check_keys_are_where_routing_says(host_type& host, const std::string& where) -> void {
    for (auto g : host.group_ids()) {
        auto* node = host.group_node(g);
        auto descriptor = host.local_descriptor(g);
        if (node == nullptr || !descriptor.has_value()) {
            continue;
        }
        std::map<key_type, std::string> store;
        node->with_state_machine([&](state_machine_type& sm) {
            store = state_machine_type::deserialize_store(sm.get_state());
        });
        for (const auto& [k, _] : store) {
            BOOST_REQUIRE_MESSAGE(descriptor->_range.contains(k),
                                  "I2 key '" << k << "' sits in shard " << g
                                             << " whose range does not contain it, after "
                                             << where);
        }
    }
}

/// I3: a shard's epoch version never decreases.
class epoch_watcher {
public:
    auto check(host_type& host, const std::string& where) -> void {
        for (auto g : host.group_ids()) {
            auto descriptor = host.local_descriptor(g);
            if (!descriptor.has_value()) {
                continue;
            }
            const auto version = descriptor->_epoch._version;
            auto it = _seen.find(g);
            if (it != _seen.end()) {
                BOOST_REQUIRE_MESSAGE(version >= it->second, "I3 epoch went backwards for shard "
                                                                 << g << " after " << where << ": "
                                                                 << it->second << " -> "
                                                                 << version);
            }
            _seen[g] = version;
        }
    }

private:
    std::map<group_id_type, std::uint64_t> _seen;
};

auto check_all(host_type& host, const std::map<key_type, std::string>& model, epoch_watcher& epochs,
               const std::string& where) -> void {
    check_tiling(host, where);
    check_no_loss_or_duplication(host, model, where);
    check_keys_are_where_routing_says(host, where);
    epochs.check(host, where);
}

/// A deterministic key alphabet: two lowercase letters, so lexicographic order
/// is easy to reason about and split keys are always interior.
auto key_at(std::size_t index) -> key_type {
    return key_type{static_cast<char>('a' + (index / 26) % 26),
                    static_cast<char>('a' + index % 26)};
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_invariant_property)

// ── I5: the round-trip law, on the state machine alone ───────────────────────

BOOST_AUTO_TEST_CASE(absorb_is_the_exact_inverse_of_split_state, *boost::unit_test::timeout(120)) {
    // The one invariant the host cannot check for itself: it is a contract the
    // *application's* state machine has to keep, and a state machine that
    // breaks it loses data on every split with nothing to notice.
    constexpr std::uint64_t seed = 20260826;
    std::mt19937 rng{seed};
    BOOST_TEST_MESSAGE("seed=" << seed);

    for (int trial = 0; trial < 50; ++trial) {
        state_machine_type sm;
        const auto count = 1 + (rng() % 60);
        for (std::size_t i = 0; i < count; ++i) {
            sm.apply(put_command(key_at(i), "v" + std::to_string(i)), i + 1);
        }
        const auto before = sm.get_state();

        // One to three cut points, all interior.
        std::vector<key_type> cuts;
        const auto cut_count = 1 + (rng() % 3);
        for (std::size_t i = 0; i < cut_count && count > 1; ++i) {
            cuts.push_back(key_at(1 + (rng() % (count - 1))));
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        if (cuts.empty()) {
            continue;
        }

        auto parts = sm.split_state(cuts);
        BOOST_REQUIRE_EQUAL(parts.size(), cuts.size() + 1);

        // Reassemble: the leftmost part is the survivor, and it absorbs the
        // others in order. `absorb`'s range argument names what is arriving.
        state_machine_type reassembled;
        reassembled.restore_from_snapshot(parts.front(), count);
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const auto start = cuts[i - 1];
            const auto end = i < cuts.size() ? std::optional<key_type>{cuts[i]} : std::nullopt;
            reassembled.absorb(parts[i], shard_range<key_type>{._start = start, ._end = end});
        }

        BOOST_REQUIRE_MESSAGE(
            reassembled.get_state() == before,
            "I5 round-trip law broken on trial " << trial << " with " << cuts.size() << " cut(s)");
    }
}

// ── I1-I3 under a randomised split workload ──────────────────────────────────

BOOST_AUTO_TEST_CASE(a_randomised_split_workload_preserves_every_invariant,
                     *boost::unit_test::timeout(600)) {
    constexpr std::uint64_t seed = 987654321;
    std::mt19937 rng{seed};
    BOOST_TEST_MESSAGE("seed=" << seed);

    invariant_host h;
    BOOST_REQUIRE(h.await_leaders());

    epoch_watcher epochs;
    check_all(h.host(), h.model(), epochs, "start");

    for (int step = 0; step < 40; ++step) {
        const auto label = "step " + std::to_string(step);

        // Write a handful of keys through the routing layer.
        for (int i = 0; i < 4; ++i) {
            const auto key = key_at(rng() % 60);
            BOOST_REQUIRE_MESSAGE(h.put(key, "v" + std::to_string(step)),
                                  "write of '" << key << "' failed at " << label);
        }
        check_all(h.host(), h.model(), epochs, label + " after writes");

        // Split a randomly chosen shard, letting the state machine pick where.
        auto groups = h.host().group_ids();
        if (groups.size() < 6) {
            const auto target = groups[rng() % groups.size()];
            auto error =
                h.settle(h.host().split_shard(target, {}, std::chrono::milliseconds{4000}));
            // A refusal is a legitimate outcome — a shard with fewer than two
            // keys has nowhere to cut — and the invariants must hold either
            // way, which is why nothing is asserted about `error` here.
            std::ignore = error;
            BOOST_REQUIRE(h.await_leaders());
            check_all(h.host(), h.model(), epochs, label + " after split");
        }
    }

    BOOST_CHECK_GT(h.host().applied_split_count(), 0u);
    BOOST_CHECK_GT(h.host().group_count(), 1u);
}

// ── I4: stale-epoch requests are always rejected ─────────────────────────────

BOOST_AUTO_TEST_CASE(a_request_at_a_superseded_epoch_is_always_rejected,
                     *boost::unit_test::timeout(300)) {
    invariant_host h;
    BOOST_REQUIRE(h.await_leaders());
    for (std::size_t i = 0; i < 12; ++i) {
        BOOST_REQUIRE(h.put(key_at(i), "v"));
    }

    const auto before = h.host().local_descriptor(k_root).value()._epoch;
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_root, {key_at(6)},
                                                std::chrono::milliseconds{4000})) == nullptr);
    BOOST_REQUIRE(h.await_leaders());

    const auto after = h.host().local_descriptor(k_root).value()._epoch;
    BOOST_REQUIRE_GT(after._version, before._version);

    // The pre-split epoch names a range that no longer exists. Serving this
    // would apply a command to a shard the client did not mean.
    auto error = h.settle(h.host().submit_command(k_root, before, put_command(key_at(1), "x"),
                                                  std::chrono::milliseconds{2000}));
    BOOST_REQUIRE_MESSAGE(error != nullptr, "I4 a stale-epoch write was served");

    bool typed = false;
    try {
        std::rethrow_exception(error);
    } catch (const kythira::shard_exception&) {
        typed = true;
    } catch (...) {
    }
    BOOST_CHECK_MESSAGE(typed, "I4 rejection must be a typed shard error the client can act on");

    // The current epoch still works, so the rejection is about staleness and
    // not about the shard being broken.
    BOOST_CHECK(h.settle(h.host().submit_command(k_root, after, put_command(key_at(1), "y"),
                                                 std::chrono::milliseconds{2000})) == nullptr);
}

BOOST_AUTO_TEST_CASE(every_epoch_a_split_produced_is_strictly_ahead_of_its_parent,
                     *boost::unit_test::timeout(300)) {
    // I3 stated positively rather than as "never decreases": a split has to
    // ADVANCE the version, or a client holding the parent's descriptor would
    // pass the epoch check against a child.
    invariant_host h;
    BOOST_REQUIRE(h.await_leaders());
    for (std::size_t i = 0; i < 12; ++i) {
        BOOST_REQUIRE(h.put(key_at(i), "v"));
    }

    const auto parent = h.host().local_descriptor(k_root).value()._epoch;
    BOOST_REQUIRE(h.settle(h.host().split_shard(k_root, {key_at(4), key_at(8)},
                                                std::chrono::milliseconds{4000})) == nullptr);
    BOOST_REQUIRE(h.await_leaders());

    for (auto g : h.host().group_ids()) {
        const auto epoch = h.host().local_descriptor(g).value()._epoch;
        BOOST_CHECK_MESSAGE(epoch._version > parent._version,
                            "I3 shard " << g << " kept version " << epoch._version
                                        << " at or below the parent's " << parent._version);
    }
}

// ── the whole set under a split-and-merge workload ───────────────────────────

BOOST_AUTO_TEST_CASE(a_randomised_split_and_merge_workload_preserves_every_invariant,
                     *boost::unit_test::timeout(600)) {
    constexpr std::uint64_t seed = 424242;
    std::mt19937 rng{seed};
    BOOST_TEST_MESSAGE("seed=" << seed);

    invariant_host h;
    BOOST_REQUIRE(h.await_leaders());

    epoch_watcher epochs;
    for (std::size_t i = 0; i < 24; ++i) {
        BOOST_REQUIRE(h.put(key_at(i), "v0"));
    }
    check_all(h.host(), h.model(), epochs, "seeded");

    for (int step = 0; step < 30; ++step) {
        const auto label = "step " + std::to_string(step);
        auto groups = h.host().group_ids();
        std::sort(groups.begin(), groups.end());

        const bool want_merge = groups.size() > 1 && (rng() % 2) == 0;
        if (want_merge) {
            // Merge a shard into its left-adjacent sibling, if the routing map
            // has one. A refusal is fine; the invariants are the subject.
            const auto target = groups[rng() % groups.size()];
            auto map = h.host().shard_map_snapshot();
            auto sibling = map.left_sibling(target);
            if (sibling.has_value()) {
                std::ignore = h.settle(h.host().merge_shards(sibling->_group_id, target,
                                                             std::chrono::milliseconds{4000}));
                BOOST_REQUIRE(h.await_leaders());
            }
        } else {
            const auto target = groups[rng() % groups.size()];
            std::ignore =
                h.settle(h.host().split_shard(target, {}, std::chrono::milliseconds{4000}));
            BOOST_REQUIRE(h.await_leaders());
        }

        // Some writes after the shape change, so the next check exercises
        // routing against the new map rather than only the old contents.
        for (int i = 0; i < 3; ++i) {
            const auto key = key_at(rng() % 24);
            std::ignore = h.put(key, "v" + std::to_string(step));
        }

        check_all(h.host(), h.model(), epochs, label);
    }

    // The workload has to have actually done something, or the invariants held
    // vacuously.
    BOOST_CHECK_GT(h.host().applied_split_count(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
