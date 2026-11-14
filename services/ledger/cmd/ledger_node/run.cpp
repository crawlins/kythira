// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// ledger_node's stack (.kiro/specs/verifiable-ledger/ task 7c): its Raft
// transport, Boost.Beast with the JSON wire (mutual TLS by default), and
// the ledger module (ledger_module.hpp) as a one-module node
// (.kiro/specs/service-colocation/ task 4): stand-alone, so its group ids,
// data directory, variables and ports are exactly those it had before it
// was a module.

#include "config.hpp"
#include "ledger_module.hpp"

#include <raft/beast_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/module_registry.hpp>
#include <raft/node_core.hpp>
#include <raft/node_host.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kythira::ledger_node {

namespace {

using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;

// Set by the signal handler, polled by run(): nothing but a lock-free store
// happens in the handler.
std::atomic<bool> g_stop{false};

extern "C" void on_signal(int /*signal*/) {
    g_stop.store(true);
}

/// The Raft transport: one io_context, its threads, the Beast server and
/// client.
struct raft_stack {
    using bundle = kythira::host_transport_types<serializer_type>;
    using client_type = kythira::boost_beast_client<bundle, kythira::host_node_id>;
    using server_type = kythira::boost_beast_server<bundle, kythira::host_node_id>;

    static auto server_config(const node_options& opt) -> kythira::boost_beast_server_config {
        kythira::boost_beast_server_config c;
        // An AppendEntries carries up to a few MiB of transactions, JSON
        // encoded; snapshot chunks are smaller.
        c.max_request_body_size =
            std::max<std::size_t>(std::size_t{32} << 20U, opt.max_body_bytes / 3 * 4 * 2);
        if (opt.raft_tls) {
            c.enable_ssl = true;
            c.ssl_cert_path = opt.raft_cert;
            c.ssl_key_path = opt.raft_key;
            c.ca_cert_path = opt.raft_ca;
            c.require_client_cert = true;
        }
        return c;
    }

    static auto client_config(const node_options& opt) -> kythira::boost_beast_client_config {
        kythira::boost_beast_client_config c;
        c.request_timeout = std::max(opt.rpc_timeout * 2, std::chrono::milliseconds{5000});
        if (opt.raft_tls) {
            c.ca_cert_path = opt.raft_ca;
            c.client_cert_path = opt.raft_cert;
            c.client_key_path = opt.raft_key;
        }
        return c;
    }

    explicit raft_stack(const node_options& opt)
        : work(boost::asio::make_work_guard(*ioc)),
          server(*ioc, opt.raft_bind, opt.raft_port, server_config(opt), kythira::noop_metrics{}),
          client(*ioc,
                 std::unordered_map<kythira::host_node_id, std::string>{opt.peers.begin(),
                                                                        opt.peers.end()},
                 client_config(opt), kythira::noop_metrics{}, ioc) {
        // A peer that reaches this node says where it is, so a member that
        // restarted at a new address is found before its name resolves.
        server.set_peer_seen_handler(client.peer_seen_handler());
        const auto n = std::max(2U, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < n; ++i) {
            threads.emplace_back([this] { ioc->run(); });
        }
    }
    ~raft_stack() { shutdown(); }
    raft_stack(const raft_stack&) = delete;
    auto operator=(const raft_stack&) -> raft_stack& = delete;
    raft_stack(raft_stack&&) = delete;
    auto operator=(raft_stack&&) -> raft_stack& = delete;

    auto shutdown() -> void {
        if (stopped.exchange(true)) {
            return;
        }
        work.reset();
        ioc->stop();
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads.clear();
    }

    std::shared_ptr<boost::asio::io_context> ioc{std::make_shared<boost::asio::io_context>()};
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work;
    server_type server;
    client_type client;
    std::vector<std::thread> threads;
    std::atomic<bool> stopped{false};
};

using standalone_host = service_node::node_host<raft_stack::client_type, raft_stack::server_type,
                                                kythira::host_node_id>;

}  // namespace

auto run(const node_options& opt) -> int {
    node_log log(opt.log_level);
    raft_stack stack(opt);
    service_node::node_config node;
    node.node_id = kythira::host_node_id_text(opt.node_id);
    node.data_dir = opt.data_dir;
    node.raft_bind = opt.raft_bind;
    node.raft_port = opt.raft_port;
    standalone_host host(node, opt.node_id, stack.client, stack.server);

    service_node::module_plan<standalone_host> plan;
    plan.modules.push_back(std::make_unique<ledger_module<standalone_host>>(true));
    plan.spaces.push_back(
        service_node::group_space::standalone(service_node::module_numbers::ledger));
    service_node::node_runner<standalone_host> runner(
        std::move(plan), node, [&log](bool error, std::string_view line) {
            log(error ? kythira::log_level::error : kythira::log_level::info, line);
        });

    int status = 0;
    try {
        runner.start(host, process_env());
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    } catch (const std::invalid_argument& e) {
        // A setting the module refused; main() read the same ones first.
        std::cerr << e.what() << '\n';
        status = 2;
    } catch (const std::exception& e) {
        log(kythira::log_level::critical, "ledger_node: cannot serve", {{"error", e.what()}});
        status = 1;
    }
    // The module drains its API, then stops its host and groups, before the
    // transport goes.
    runner.stop([&stack] { stack.shutdown(); });
    return status;
}

auto install_signal_handlers() -> void {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

}  // namespace kythira::ledger_node
