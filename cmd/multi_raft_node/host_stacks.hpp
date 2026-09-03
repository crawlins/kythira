// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file host_stacks.hpp
/// @brief The `multi_raft` host's transport stacks and its run loop, as
///        templates — instantiated ONE PER TRANSLATION UNIT.
///
/// **Why this is a header and not the body of `main.cpp`.** These templates
/// were all instantiated in `main.cpp`, once per `(transport × wire
/// serializer)` pair: two transports and two serializers is four whole
/// `multi_raft` stacks in one translation unit. Measured on this project's own
/// CI compiler (`g++-13`, Release, stdexec backend), that translation unit
/// peaked at **10,974 MiB of compiler RSS** — the heaviest in the tree by a wide
/// margin, ahead of the 5.6 GiB `three_way_http_transport_equivalence_test`
/// that the `heavy_tu` Ninja job pool was created for. A GitHub runner has
/// 16 GiB, so one of these plus any two ordinary compiles exhausted it: the
/// `Full suite (stdexec future backend)` leg died as an 82-minute stall into
/// the 180-minute timeout on one PR and as `exit 143` VM reclamation on
/// another, with no compiler error involved anywhere.
///
/// It survived on `main` only because `ccache` answered for these objects.
/// Any change under `cmd/multi_raft_node/` — exactly what a PR touching the
/// host does — made them cold and the leg failed.
///
/// Splitting the instantiations across `run_httplib_json.cpp`,
/// `run_httplib_cbor.cpp`, `run_beast_json.cpp` and `run_beast_cbor.cpp` gives
/// four translation units of one stack each, which both fits and compiles in
/// parallel. Nothing here changed behaviour: the same templates, instantiated
/// the same way, in four files instead of one.
///
/// The declarations of those four entry points live in `host_runners.hpp`.

#include "config.hpp"
#include "control_server.hpp"
#include "kv_data_server.hpp"
#include "stop_signal.hpp"
#include "switchable_persistence.hpp"

// Shared with the in-process harness. See `main.cpp`'s file comment.
#include "multi_raft_kv_workload.hpp"
#include "multi_raft_transport_harness.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/cbor_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/test_state_machine.hpp>

#if defined(KYTHIRA_BENCH_HAS_BEAST)
#include <raft/beast_http_transport_impl.hpp>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::bench::host {

[[nodiscard]] inline auto url_map(const node_options& opt)
    -> std::unordered_map<std::uint64_t, std::string> {
    return {opt._peers.begin(), opt._peers.end()};
}

// ─────────────────────────────────────────────────────────────────────────────
// The transport stacks
// ─────────────────────────────────────────────────────────────────────────────
//
// One per transport the Tier B matrix sweeps (Requirement 1.5), each holding
// exactly one server and one client for this host. They reuse
// `harness_transport_types` rather than declaring a second bundle: a Tier C row
// whose transport was configured differently from the Tier B row it is compared
// against would differ in something nobody could see.

template<typename Serializer> struct httplib_stack {
    using bundle = kythira::testing::harness_transport_types<Serializer>;
    using client_type = kythira::cpp_httplib_client<bundle>;
    using server_type = kythira::cpp_httplib_server<bundle>;
    static constexpr std::string_view k_name = "cpp-httplib";

    explicit httplib_stack(const node_options& opt)
        : _server(opt._bind_address, opt._raft_port, server_config(opt), kythira::noop_metrics{}),
          _client(url_map(opt), client_config(opt), kythira::noop_metrics{}) {}

    auto server() -> server_type& { return _server; }
    auto client() -> client_type& { return _client; }
    auto shutdown() -> void {}

    static auto client_config(const node_options& opt) -> kythira::cpp_httplib_client_config {
        kythira::cpp_httplib_client_config cfg;
        cfg.connection_timeout = std::chrono::milliseconds{2000};
        cfg.request_timeout = opt._op_timeout;
        return cfg;
    }
    static auto server_config(const node_options& opt) -> kythira::cpp_httplib_server_config {
        kythira::cpp_httplib_server_config cfg;
        cfg.request_timeout = std::chrono::duration_cast<std::chrono::seconds>(
            opt._op_timeout + std::chrono::seconds{2});
        return cfg;
    }

    server_type _server;
    client_type _client;
};

#if defined(KYTHIRA_BENCH_HAS_BEAST)
template<typename Serializer> struct beast_stack {
    using bundle = kythira::testing::harness_transport_types<Serializer>;
    using client_type = kythira::boost_beast_client<bundle>;
    using server_type = kythira::boost_beast_server<bundle>;
    static constexpr std::string_view k_name = "beast";

    explicit beast_stack(const node_options& opt)
        : _work(boost::asio::make_work_guard(*_ioc)),
          _server(*_ioc, opt._bind_address, opt._raft_port, kythira::boost_beast_server_config{},
                  kythira::noop_metrics{}),
          _client(*_ioc, url_map(opt), kythira::boost_beast_client_config{},
                  kythira::noop_metrics{}, _ioc) {
        const auto threads = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < threads; ++i) {
            _io_threads.emplace_back([this] { _ioc->run(); });
        }
    }

    ~beast_stack() { shutdown(); }
    beast_stack(const beast_stack&) = delete;
    auto operator=(const beast_stack&) -> beast_stack& = delete;

    auto server() -> server_type& { return _server; }
    auto client() -> client_type& { return _client; }

    /// Called only after the host has been stopped: a running host still has
    /// RPCs on these threads. `_ioc` is deliberately not reset — dropping this
    /// object's reference is all shutdown owes, and the context is destroyed by
    /// whichever reference goes last. That is `.kiro/specs/multi-raft-
    /// performance/` task 5a's finding, and it applies here for the same
    /// reason it applies in the harness.
    auto shutdown() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _work.reset();
        _ioc->stop();
        for (auto& t : _io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _io_threads.clear();
    }

    std::shared_ptr<boost::asio::io_context> _ioc{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
    server_type _server;
    client_type _client;
    std::vector<std::thread> _io_threads;
    std::atomic<bool> _stopped{false};
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// The host's `raft_types`
// ─────────────────────────────────────────────────────────────────────────────

/// Deliberately the same shape as `tests/multi_raft_transport_harness.hpp`'s
/// `kv_host_types`, down to holding the *node-internal* serializer at JSON
/// while the wire serializer is the swept axis. Holding the node-internal one
/// fixed is what keeps rows comparable, and diverging from the harness here
/// would put a second uncontrolled difference into every Tier B → Tier C delta.
template<typename Stack> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    // The harness's non-owning handles, not references and not values. A
    // reference member would make `multi_raft_config` non-assignable, and the
    // transports are not movable. Constructed with a null counter pointer:
    // `.kiro/specs/multi-raft-performance/` Requirement 8.2 keeps measurement
    // counters out of the measured process, so this host counts nothing about
    // itself.
    using network_client_type =
        kythira::testing::transport_client_handle<typename Stack::client_type>;
    using network_server_type =
        kythira::testing::transport_server_handle<typename Stack::server_type>;

    using persistence_engine_type = kythira::bench::switchable_persistence;
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

/// @brief Build, start, serve, and shut down in the order that does not
///        terminate the process.
///
/// **The shutdown ordering is not optional** (Requirement 1.4). `kv_cluster::
/// shutdown()` is the reference implementation and this follows it rather than
/// rediscovering it: stop the data and control servers so nothing new arrives,
/// stop the host so its groups' nodes are stopped before they are destroyed,
/// then release the transport. A host destroyed with a group still running
/// terminates the process, because `~group_state` destroys unstopped nodes
/// through a deferred closure's reference — a failure mode that is a crash
/// rather than a failed assertion, which is why it has its own test.
template<typename Stack> auto run_host(const node_options& opt) -> int {
    using types = host_types<Stack>;
    using host_type = kythira::multi_raft<types, std::string, std::uint64_t>;
    using config_type = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using descriptor_type = kythira::shard_descriptor<std::uint64_t, std::string, std::uint64_t>;

    Stack stack{opt};

    config_type cfg{
        .node_id = opt._node_id,
        .network_client = typename types::network_client_type{stack.client()},
        .network_server = typename types::network_server_type{stack.server()},
        .store_factory =
            [&opt](const std::uint64_t& group) { return kythira::bench::make_store(opt, group); },
    };
    cfg.config._election_timeout_min = opt._election_timeout_min;
    cfg.config._election_timeout_max = opt._election_timeout_max;
    cfg.config._heartbeat_interval = opt._heartbeat_interval;
    cfg.config._rpc_timeout = opt._rpc_timeout;
    // Every one of these matches `kv_cluster::make_config`, and for the same
    // reasons: hibernation off (a measurement whose population hibernated
    // mid-window would be measuring hibernation), the policy phase and
    // automatic split/merge off (neither may land inside a timed window), and
    // the partitioner shared with the harness.
    cfg.hibernation = kythira::hibernation_mode::off;
    cfg.executor_stripes = opt._executor_stripes;
    cfg.policy_interval = std::chrono::hours{1};
    cfg.split_merge_interval = std::chrono::hours{1};
    cfg.automatic_split_merge_enabled = false;
    cfg.heartbeat_interval = std::chrono::milliseconds{0};
    cfg.partitioner = kythira::make_partitioner<std::string>(kythira::testing::kv_partitioner{});

    auto host = std::make_unique<host_type>(std::move(cfg));

    // **Pre-split into N ranges** (Requirement 4.4), tiled exactly as
    // `kv_shard_ranges` tiles them for the in-process rows, so Tier C and
    // Tier B cover the key space identically.
    const auto ranges = kythira::testing::kv_shard_ranges(opt._groups, opt._key_count);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        host->create_group(descriptor_type{._group_id = static_cast<std::uint64_t>(i + 1),
                                           ._range = ranges[i],
                                           ._epoch = kythira::shard_epoch{},
                                           ._voters = opt._voters});
    }

    host->start();

    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            host->tick();
            std::this_thread::sleep_for(opt._tick_interval);
        }
    });

    // Hoisted: `types` is dependent, so `types::serializer_type{}` inside the
    // braced-init below would need a `typename` the parser cannot supply there.
    typename types::serializer_type node_serializer{};
    kythira::bench::kv_data_server<host_type> data{*host,           opt._bind_address,
                                                   opt._data_port,  node_serializer.media_type(),
                                                   opt._op_timeout, opt._data_threads};
    kythira::bench::control_server<host_type> control{*host, opt, std::string(Stack::k_name),
                                                      opt._bind_address, opt._control_port};
    data.start();
    control.start();

    std::cout << "multi_raft_node " << opt._node_id << " ready: raft=" << opt._raft_port
              << " data=" << opt._data_port << " control=" << opt._control_port
              << " transport=" << Stack::k_name << " groups=" << opt._groups << std::endl;

    wait_for_stop();

    // The ordering. Nothing above this line may be reordered below it.
    data.stop();
    control.stop();
    driving.store(false, std::memory_order_release);
    if (driver.joinable()) {
        driver.join();
    }
    host->stop();
    host.reset();
    stack.shutdown();
    return 0;
}

}  // namespace kythira::bench::host
