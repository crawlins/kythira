// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file kythira_node_host_test.cpp
/// @brief `kythira_node`'s host over its real transport
///        (.kiro/specs/service-colocation/ task 3).
///
/// Three nodes on loopback, each with the node's Beast stack, a
/// `kythira_node_host` and a `node_runner` driving two test modules. Each
/// module runs its own `multi_raft` host, with its own state machine type,
/// in its own composite group space, through `node_host::server_for`. Both
/// modules elect and commit over the one Raft listener per node, and the
/// node's health endpoint reports both.

#define BOOST_TEST_MODULE kythira_node_host_test
#include <boost/test/unit_test.hpp>

#include "test_ports.hpp"

#include <raft/console_logger.hpp>
#include <raft/examples/counter_state_machine.hpp>
#include <raft/future_default.hpp>
#include <raft/kythira_node_host.hpp>
#include <raft/metrics.hpp>
#include <raft/module_registry.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/node_core.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("kythira_node_host_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

namespace sn = kythira::service_node;
using host_type = sn::kythira_node_host;
using node_id = kythira::host_node_id;

template<typename StateMachine> struct module_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = sn::kythira_node_stack::serializer_type;

    using network_client_type =
        kythira::transport_client_handle<sn::kythira_node_stack::client_type, node_id_type>;
    using network_server_type = host_type::view_type;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = StateMachine;

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

struct one_key {
    [[nodiscard]] auto key_of(const std::vector<std::byte>&) const -> std::string { return "k"; }
};

auto bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

auto text(const std::vector<std::byte>& b) -> std::string {
    std::string out;
    for (const auto x : b) {
        out.push_back(static_cast<char>(x));
    }
    return out;
}

/// A module running one single-shard group of `StateMachine` on its own
/// host, ticked by its own thread.
template<typename StateMachine> class raft_module final : public sn::service_module<host_type> {
public:
    using types = module_types<StateMachine>;
    using raft_host = kythira::multi_raft<types, std::string, std::uint64_t>;

    raft_module(std::string name, std::uint8_t number) : _name(std::move(name)), _number(number) {}

    [[nodiscard]] auto name() const -> std::string_view override { return _name; }
    [[nodiscard]] auto number() const -> std::uint8_t override { return _number; }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "TEST_"; }
    [[nodiscard]] auto configure(const sn::env_lookup&) -> std::optional<std::string> override {
        return std::nullopt;
    }

    void register_groups(host_type& host, const sn::group_space& space) override {
        kythira::multi_raft_config<types, std::string, std::uint64_t> cfg{
            .node_id = host.node_id(),
            .network_client = typename types::network_client_type(host.client()),
            .network_server = host.server_for(space),
            .store_factory =
                [](const std::uint64_t&) { return typename types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{300};
        cfg.config._election_timeout_max = std::chrono::milliseconds{600};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{50};
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<std::string>(one_key{});
        _host = std::make_unique<raft_host>(std::move(cfg));
        std::vector<node_id> voters;
        for (const auto& p : host.config().peers) {
            voters.push_back(sn::kythira_node_stack::parse_id(p.id));
        }
        _group = space.to_host(1);
        _host->create_group(kythira::shard_descriptor<std::uint64_t, std::string, node_id>{
            ._group_id = _group,
            ._range =
                kythira::shard_range<std::string>{._start = std::nullopt, ._end = std::nullopt},
            ._epoch = kythira::shard_epoch{},
            ._voters = voters});
    }

    void start(sn::module_registry&) override {
        _host->start();
        _running = true;
        _ticker = std::thread([this] {
            while (_running) {
                _host->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        });
    }

    void stop() override {
        _running = false;
        if (_ticker.joinable()) {
            _ticker.join();
        }
        if (_host) {
            _host->stop();
        }
    }

    [[nodiscard]] auto health() const -> sn::module_health override {
        return {_running.load(), {}};
    }

    [[nodiscard]] auto is_leader() const -> bool {
        auto* g = _host->group_node(_group);
        return g != nullptr && g->is_leader();
    }

    /// Commits `command` if this node leads; nothing otherwise.
    auto submit(const std::vector<std::byte>& command) -> std::optional<std::vector<std::byte>> {
        if (!is_leader()) {
            return std::nullopt;
        }
        auto f = _host->submit_command("k", command, std::chrono::milliseconds{3000});
        if (!f.wait(std::chrono::milliseconds{3000})) {
            return std::nullopt;
        }
        try {
            return std::move(f).get();
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] auto holds(std::uint64_t group) const -> bool {
        return _host->group_node(group) != nullptr;
    }
    [[nodiscard]] auto group() const -> std::uint64_t { return _group; }

private:
    std::string _name;
    std::uint8_t _number;
    std::unique_ptr<raft_host> _host;
    std::uint64_t _group = 0;
    std::atomic<bool> _running{false};
    std::thread _ticker;
};

using kv_module = raft_module<kythira::test_key_value_state_machine<std::uint64_t>>;
using counter_module = raft_module<kythira::examples::counter_state_machine>;

/// One node: the stack, the host and a runner with both modules.
struct test_node {
    test_node(const sn::node_config& c, sn::module_catalog<host_type>& catalog)
        : stack(c),
          host(c, sn::kythira_node_stack::parse_id(c.node_id), stack.client(), stack.server()) {
        auto planned = sn::plan_modules(catalog, {"kv", "counter"}, true);
        BOOST_REQUIRE(std::holds_alternative<sn::module_plan<host_type>>(planned));
        auto& plan = std::get<sn::module_plan<host_type>>(planned);
        for (auto& m : plan.modules) {
            if (m->name() == "kv") {
                kv = static_cast<kv_module*>(m.get());
            } else {
                counter = static_cast<counter_module*>(m.get());
            }
        }
        runner.emplace(std::move(plan), c);
        runner->start(host, [](std::string_view) { return std::optional<std::string>{}; });
    }

    test_node(const test_node&) = delete;
    auto operator=(const test_node&) -> test_node& = delete;
    test_node(test_node&&) = delete;
    auto operator=(test_node&&) -> test_node& = delete;
    ~test_node() { stop(); }

    void stop() {
        if (runner) {
            runner->stop([this] { stack.shutdown(); });
        }
    }

    sn::kythira_node_stack stack;
    host_type host;
    std::optional<sn::node_runner<host_type>> runner;
    kv_module* kv = nullptr;
    counter_module* counter = nullptr;
};

template<typename Pick>
auto commit(std::vector<std::unique_ptr<test_node>>& nodes, Pick pick,
            const std::vector<std::byte>& command) -> std::optional<std::vector<std::byte>> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& n : nodes) {
            if (auto r = pick(*n).submit(command)) {
                return r;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return std::nullopt;
}

}  // namespace

BOOST_AUTO_TEST_CASE(two_modules_commit_over_one_listener) {
    sn::module_catalog<host_type> catalog;
    // Module numbers from the composite range no real module uses.
    const sn::module_registrar<host_type> kv_reg(
        "kv", [] { return std::make_unique<kv_module>("kv", 200); }, catalog);
    const sn::module_registrar<host_type> counter_reg(
        "counter", [] { return std::make_unique<counter_module>("counter", 201); }, catalog);

    const auto base = kythira::testing::free_port_block(3);
    const auto data =
        std::filesystem::temp_directory_path() / ("kythira_node_host_test_" + std::to_string(base));
    std::vector<sn::node_peer> peers;
    for (std::uint16_t i = 0; i < 3; ++i) {
        peers.push_back({std::to_string(i + 1), "127.0.0.1", static_cast<std::uint16_t>(base + i)});
    }
    std::vector<std::unique_ptr<test_node>> nodes;
    for (std::uint16_t i = 0; i < 3; ++i) {
        sn::node_config c;
        c.node_id = std::to_string(i + 1);
        c.peers = peers;
        c.raft_bind = "127.0.0.1";
        c.raft_port = static_cast<std::uint16_t>(base + i);
        c.insecure_plaintext = true;
        c.data_dir = data / c.node_id;
        nodes.push_back(std::make_unique<test_node>(c, catalog));
    }

    const auto kv_pick = [](test_node& n) -> kv_module& { return *n.kv; };
    const auto counter_pick = [](test_node& n) -> counter_module& { return *n.counter; };

    std::optional<std::vector<std::byte>> r;
    for (int i = 0; i < 3; ++i) {
        r = commit(nodes, counter_pick, bytes("INC"));
        BOOST_REQUIRE(r.has_value());
    }
    BOOST_TEST(text(*r) == "3");
    // test_key_value_state_machine: put is type 1, length-prefixed key and value.
    std::vector<std::byte> put{std::byte{1}, std::byte{1},   std::byte{0},  std::byte{0},
                               std::byte{0}, std::byte{'a'}, std::byte{1},  std::byte{0},
                               std::byte{0}, std::byte{0},   std::byte{'1'}};
    BOOST_TEST(commit(nodes, kv_pick, put).has_value());

    for (auto& n : nodes) {
        BOOST_TEST(n->kv->group() == sn::group_space::composite(200).to_host(1));
        BOOST_TEST(n->counter->group() == sn::group_space::composite(201).to_host(1));
        BOOST_TEST(!n->kv->holds(n->counter->group()));
        BOOST_TEST(n->host.mux().running_views() == 2U);
        BOOST_TEST(std::filesystem::is_directory(n->host.config().data_dir / "kv"));
        const auto [status, body] = n->runner->health_report();
        BOOST_TEST(status == 200U);
    }
    BOOST_TEST(nodes[0]->host.mux().unrouted_count() == 0U);

    for (auto& n : nodes) {
        n->stop();
    }
    nodes.clear();
    std::filesystem::remove_all(data);
}
