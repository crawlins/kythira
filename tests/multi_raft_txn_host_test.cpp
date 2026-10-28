// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_txn_host_test.cpp
/// @brief The multi-raft host's transaction surface (task 5 of
///        `.kiro/specs/multi-raft-transactions/`).
///
/// Two hosts, identical except for the state machine in their type bundle:
/// one wraps `test_key_value_state_machine` in `transactional_state_machine`,
/// the other does not. Each host's store records the `normal` entries it is
/// asked to append, so the tests can assert on the log bytes themselves:
///
/// - the unwrapped host logs a plain command byte for byte as submitted
///   (Requirement 1.2: nothing changes unless the application opts in);
/// - the wrapped host logs the same command inside a `plain` envelope, and
///   turns a `key_locked` result into `key_locked_exception` (Requirement 2);
/// - `submit_txn` checks every key against the local range and refuses a
///   stray one with the descriptors now covering it (Requirement 3.5), and
///   keeps `submit_command`'s not-leader hint.

#define BOOST_TEST_MODULE multi_raft_txn_host_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_kv_workload.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>
#include <raft/txn_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_txn_host_test"), nullptr};
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
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::txn_id;
using kythira::txn_result_status;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::kv_partitioner;
using kythira::testing::kv_put;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using kv_sm = kythira::test_key_value_state_machine<std::uint64_t>;
using txn_sm = kythira::transactional_state_machine<kv_sm, key_type, kv_partitioner>;

/// The `normal` commands a host's stores were asked to append, in order.
struct command_log {
    std::mutex _mu;
    std::vector<std::vector<std::byte>> _commands;

    auto record(const std::vector<std::byte>& c) -> void {
        std::lock_guard lock(_mu);
        _commands.push_back(c);
    }
    [[nodiscard]] auto contains(const std::vector<std::byte>& c) -> bool {
        std::lock_guard lock(_mu);
        return std::find(_commands.begin(), _commands.end(), c) != _commands.end();
    }
};

/// `memory_persistence_engine`, plus a note of every `normal` entry appended.
class recording_persistence_engine {
public:
    using inner_type =
        kythira::memory_persistence_engine<std::uint64_t, std::uint64_t, std::uint64_t>;
    using log_entry_t = inner_type::log_entry_t;
    using snapshot_t = inner_type::snapshot_t;

    recording_persistence_engine() = default;
    explicit recording_persistence_engine(std::shared_ptr<command_log> log)
        : _log(std::move(log)) {}

    auto save_current_term(std::uint64_t term) -> void { _inner.save_current_term(term); }
    auto load_current_term() -> std::uint64_t { return _inner.load_current_term(); }
    auto save_voted_for(std::uint64_t node) -> void { _inner.save_voted_for(node); }
    auto load_voted_for() -> std::optional<std::uint64_t> { return _inner.load_voted_for(); }
    auto append_log_entry(const log_entry_t& entry) -> void {
        if (_log && entry.type() == kythira::entry_type::normal) {
            _log->record(entry.command());
        }
        _inner.append_log_entry(entry);
    }
    auto get_log_entry(std::uint64_t index) -> std::optional<log_entry_t> {
        return _inner.get_log_entry(index);
    }
    auto get_log_entries(std::uint64_t start, std::uint64_t end) -> std::vector<log_entry_t> {
        return _inner.get_log_entries(start, end);
    }
    auto get_last_log_index() -> std::uint64_t { return _inner.get_last_log_index(); }
    auto truncate_log(std::uint64_t index) -> void { _inner.truncate_log(index); }
    auto save_snapshot(const snapshot_t& snap) -> void { _inner.save_snapshot(snap); }
    auto load_snapshot() -> std::optional<snapshot_t> { return _inner.load_snapshot(); }
    auto delete_log_entries_before(std::uint64_t index) -> void {
        _inner.delete_log_entries_before(index);
    }

private:
    inner_type _inner;
    std::shared_ptr<command_log> _log;
};

template<typename SM> struct host_types {
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

    using persistence_engine_type = recording_persistence_engine;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = SM;

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

using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;

auto descriptor(group_id_type g, std::optional<key_type> start, std::optional<key_type> end,
                std::vector<std::uint64_t> voters = {1}) -> descriptor_type {
    return descriptor_type{
        ._group_id = g,
        ._range = shard_range<key_type>{._start = std::move(start), ._end = std::move(end)},
        ._epoch = shard_epoch{._version = 0, ._conf_version = 0},
        ._voters = std::move(voters),
        ._learners = {},
        ._leader_hint = std::nullopt};
}

/// A host over two static shards split at "m", both led locally.
template<typename SM> struct two_shard_host {
    using host_type = multi_raft<host_types<SM>, key_type, group_id_type>;
    using config_type = multi_raft_config<host_types<SM>, key_type, group_id_type>;

    message_fabric _fabric{2};
    std::shared_ptr<command_log> _log = std::make_shared<command_log>();
    std::unique_ptr<host_type> _host;

    explicit two_shard_host(bool two_shards = true, std::vector<std::uint64_t> voters = {1}) {
        config_type cfg{
            .node_id = 1,
            .network_client = fabric_client{_fabric, 1},
            .network_server = fabric_server{_fabric, 1},
            .store_factory =
                [log = _log](const group_id_type&) { return recording_persistence_engine{log}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{40};
        cfg.config._election_timeout_max = std::chrono::milliseconds{80};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
        cfg.route_retry_backoff = std::chrono::milliseconds{1};
        _host = std::make_unique<host_type>(std::move(cfg));
        if (two_shards) {
            _host->create_group(descriptor(1, std::nullopt, key_type{"m"}, voters));
            _host->create_group(descriptor(2, key_type{"m"}, std::nullopt, voters));
        } else {
            _host->create_group(descriptor(1, std::nullopt, std::nullopt, voters));
        }
        _host->start();
    }
    ~two_shard_host() { _host->stop(); }
    two_shard_host(const two_shard_host&) = delete;
    auto operator=(const two_shard_host&) -> two_shard_host& = delete;

    auto tick_until(auto predicate) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (std::chrono::steady_clock::now() < deadline) {
            _host->tick();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        return predicate();
    }

    auto lead_all() -> bool {
        return tick_until([&] {
            return _host->group_node(1)->is_leader() &&
                   (_host->group_node(2) == nullptr || _host->group_node(2)->is_leader());
        });
    }
};

/// Resolve `future`, returning its value or the exception it failed with.
template<typename Future>
auto settle(Future&& f) -> std::pair<std::vector<std::byte>, std::exception_ptr> {
    if (!f.wait(std::chrono::milliseconds{3000})) {
        return {{}, std::make_exception_ptr(std::runtime_error("settle: never resolved"))};
    }
    try {
        return {std::forward<Future>(f).get(), nullptr};
    } catch (...) {
        return {{}, std::current_exception()};
    }
}

auto txn_bytes(const kythira::txn_envelope<key_type>& e) -> std::vector<std::byte> {
    return kythira::encode_txn_envelope<key_type>(e);
}

auto as_result(const std::vector<std::byte>& b) -> kythira::txn_result<key_type> {
    return kythira::decode_txn_result<key_type>(b);
}

auto kv_value(auto& host, group_id_type g, const key_type& k) -> std::optional<std::string> {
    return host._host->group_node(g)->with_state_machine(
        [&](auto& sm) -> std::optional<std::string> {
            if constexpr (kythira::is_transactional_state_machine_v<std::decay_t<decltype(sm)>>) {
                return sm.inner().get_value(k);
            } else {
                return sm.get_value(k);
            }
        });
}

const txn_id t1{0, 1};

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_txn_host)

BOOST_AUTO_TEST_CASE(an_unwrapped_host_logs_a_plain_command_byte_for_byte,
                     *boost::unit_test::timeout(60)) {
    two_shard_host<kv_sm> env;
    BOOST_REQUIRE(env.lead_all());
    const auto command = kv_put("apple", "1");
    const auto [value, err] =
        settle(env._host->submit_command(key_type{"apple"}, command, std::chrono::seconds{2}));
    BOOST_REQUIRE(err == nullptr);
    BOOST_TEST(env._log->contains(command));
    BOOST_TEST(!env._log->contains(txn_bytes(kythira::txn_plain{command})));
    BOOST_TEST((kv_value(env, 1, "apple") == std::optional<std::string>("1")));
}

BOOST_AUTO_TEST_CASE(a_wrapped_host_envelopes_a_plain_command_and_returns_its_result,
                     *boost::unit_test::timeout(60)) {
    two_shard_host<txn_sm> env;
    BOOST_REQUIRE(env.lead_all());
    const auto command = kv_put("apple", "1");
    const auto [value, err] =
        settle(env._host->submit_command(key_type{"apple"}, command, std::chrono::seconds{2}));
    BOOST_REQUIRE(err == nullptr);
    BOOST_TEST(value.empty());  // a PUT's own result, not an encoded txn_result
    BOOST_TEST(env._log->contains(txn_bytes(kythira::txn_plain{command})));
    BOOST_TEST(!env._log->contains(command));
    BOOST_TEST((kv_value(env, 1, "apple") == std::optional<std::string>("1")));

    // The partitioner form and the cross-shard admission check both read the
    // application's command, not the envelope.
    BOOST_CHECK(
        settle(env._host->submit_command(kv_put("zebra", "2"), std::chrono::seconds{2})).second ==
        nullptr);
    const auto stray = settle(env._host->submit_command(key_type{"apple"}, kv_put("zebra", "3"),
                                                        std::chrono::seconds{2}))
                           .second;
    BOOST_CHECK(stray != nullptr);
    using cross_shard_error = kythira::cross_shard_command_exception<group_id_type, key_type>;
    BOOST_CHECK_THROW(std::rethrow_exception(stray), cross_shard_error);
}

BOOST_AUTO_TEST_CASE(a_plain_command_on_a_locked_key_raises_key_locked,
                     *boost::unit_test::timeout(60)) {
    two_shard_host<txn_sm> env;
    BOOST_REQUIRE(env.lead_all());
    const auto prepared = settle(env._host->submit_txn(
        {"apple", "banana"},
        txn_bytes(kythira::txn_prepare<key_type>{
            t1,
            "apple",
            {{"apple", kv_put("apple", "t")}, {"banana", kv_put("banana", "t")}},
            {},
            0,
            1'000'000,
            true}),
        std::chrono::seconds{2}));
    BOOST_REQUIRE(prepared.second == nullptr);
    BOOST_TEST((as_result(prepared.first)._status == txn_result_status::ok));

    const auto err = settle(env._host->submit_command(key_type{"banana"}, kv_put("banana", "p"),
                                                      std::chrono::seconds{2}))
                         .second;
    BOOST_REQUIRE(err != nullptr);
    try {
        std::rethrow_exception(err);
    } catch (const kythira::key_locked_exception<key_type>& e) {
        BOOST_TEST(e.key() == "banana");
        BOOST_TEST((e.holder() == t1));
        BOOST_TEST(e.primary() == "apple");
    } catch (...) {
        BOOST_FAIL("expected key_locked_exception");
    }
    BOOST_TEST(!kv_value(env, 1, "banana").has_value());

    // Committing through the host releases the locks and applies both writes.
    const auto committed = settle(env._host->submit_txn(
        {"apple"}, txn_bytes(kythira::txn_commit_primary<key_type>{t1, "apple", 9}),
        std::chrono::seconds{2}));
    BOOST_REQUIRE(committed.second == nullptr);
    BOOST_TEST((as_result(committed.first)._status == txn_result_status::ok));
    BOOST_TEST((kv_value(env, 1, "apple") == std::optional<std::string>("t")));
    BOOST_TEST((kv_value(env, 1, "banana") == std::optional<std::string>("t")));
    BOOST_CHECK(settle(env._host->submit_command(key_type{"banana"}, kv_put("banana", "p"),
                                                 std::chrono::seconds{2}))
                    .second == nullptr);
}

BOOST_AUTO_TEST_CASE(submit_txn_refuses_keys_outside_the_shard_with_their_descriptors,
                     *boost::unit_test::timeout(60)) {
    two_shard_host<txn_sm> env;
    BOOST_REQUIRE(env.lead_all());
    const auto [value, err] = settle(env._host->submit_txn(
        {"apple", "zebra"},
        txn_bytes(kythira::txn_prepare<key_type>{
            t1,
            "apple",
            {{"apple", kv_put("apple", "t")}, {"zebra", kv_put("zebra", "t")}},
            {},
            0,
            1'000'000,
            true}),
        std::chrono::seconds{2}));
    using epoch_error = kythira::shard_epoch_mismatch_exception<group_id_type, key_type>;
    BOOST_REQUIRE(err != nullptr);
    try {
        std::rethrow_exception(err);
    } catch (const epoch_error& e) {
        BOOST_TEST(e.group_id() == 1U);
        // Both shards the request touched, so the coordinator re-partitions
        // from the refusal alone.
        BOOST_REQUIRE_EQUAL(e.current_descriptors().size(), 2U);
        BOOST_TEST(e.current_descriptors()[0]._group_id == 1U);
        BOOST_TEST(e.current_descriptors()[1]._group_id == 2U);
    } catch (...) {
        BOOST_FAIL("expected shard_epoch_mismatch_exception");
    }
    // Nothing was proposed.
    BOOST_TEST(env._host->load_counters(1).second == 0U);
    BOOST_TEST(!env._log->contains(txn_bytes(kythira::txn_prepare<key_type>{
        t1,
        "apple",
        {{"apple", kv_put("apple", "t")}, {"zebra", kv_put("zebra", "t")}},
        {},
        0,
        1'000'000,
        true})));
}

BOOST_AUTO_TEST_CASE(submit_txn_with_no_keys_is_refused, *boost::unit_test::timeout(60)) {
    two_shard_host<txn_sm> env;
    BOOST_REQUIRE(env.lead_all());
    const auto err =
        settle(env._host->submit_txn({}, txn_bytes(kythira::txn_gc{0}), std::chrono::seconds{2}))
            .second;
    BOOST_CHECK_THROW(std::rethrow_exception(err), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(submit_txn_on_a_follower_carries_the_leader_hint,
                     *boost::unit_test::timeout(60)) {
    // Three voters, only this one present: it can never lead.
    two_shard_host<txn_sm> env(false, {1, 2, 3});
    for (int i = 0; i < 20; ++i) {
        env._host->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_REQUIRE(!env._host->group_node(1)->is_leader());
    const auto err =
        settle(env._host->submit_txn(
                   {"apple"}, txn_bytes(kythira::txn_commit_primary<key_type>{t1, "apple", 1}),
                   std::chrono::milliseconds{500}))
            .second;
    using not_leader_error = kythira::shard_not_leader_exception<group_id_type, std::uint64_t>;
    BOOST_CHECK_THROW(std::rethrow_exception(err), not_leader_error);
}

BOOST_AUTO_TEST_CASE(txn_defaults_carry_requirement_10_1_values) {
    using config_type = typename two_shard_host<txn_sm>::config_type;
    static_assert(std::is_same_v<decltype(config_type::txn_defaults), kythira::txn_options>);
    const kythira::txn_options defaults{};
    BOOST_TEST((defaults.isolation == kythira::txn_isolation::read_committed));
    BOOST_TEST(defaults.lock_ttl.count() == 3000);
    BOOST_TEST(defaults.timeout.count() == 10000);
    BOOST_TEST(defaults.max_retries == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
