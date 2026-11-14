// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// dynamodb_gateway_node: the DynamoDB-compatible daemon
// (.kiro/specs/dynamodb-compatible-api/ task 7).
//
// Reads its settings from the environment (gateway.hpp lists them), builds
// the backend and serves the front end until SIGINT or SIGTERM. Without
// KYTHIRA_DYNAMODB_PEERS the backend is local_backend on memory_kv_engine:
// one node, no Raft, nothing kept across a restart. With it, the node joins
// a Raft cluster (run_cluster.hpp) on the memory engine or, in a build with
// RocksDB, on a database under the data directory.
//
// The daemon is a node with its one module fixed
// (.kiro/specs/service-colocation/ task 6, Requirement 3.1): the DynamoDB
// module (dynamodb_module.hpp) in a stand-alone group space on the
// daemon's own JSON Beast transport, so its group ids, variables and data
// directory are exactly what they were.

#include "dynamodb_module.hpp"
#include "run_cluster.hpp"
#ifdef KYTHIRA_DYNAMODB_GATEWAY_IAM
#include "iam_auth.hpp"
#endif

#include <kythira/dynamodb/gateway.hpp>

#include <raft/node_core.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// The Folly backend (the default, which defines no macro of its own) needs
// folly::Init before the first future: its timekeeper is a Folly singleton.
#include <folly/init/Init.h>
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace {

constexpr std::string_view k_usage =
    "usage: dynamodb_gateway_node [--help]\n"
    "\n"
    "The Kythira DynamoDB-compatible daemon. Point an AWS SDK, the AWS CLI or\n"
    "KCL at it with --endpoint-url. Settings come from the environment:\n"
    "\n";

auto wait_for_signal() -> int {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    int sig = 0;
    sigwait(&set, &sig);
    return sig;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    namespace ddb = kythira::dynamodb;
    if (argc > 1) {
        const std::string_view arg(argv[1]);
        const bool help = argc == 2 && arg == "--help";
        std::FILE* out = help ? stdout : stderr;
        std::fputs(k_usage.data(), out);
        std::fwrite(ddb::k_gateway_env_help.data(), 1, ddb::k_gateway_env_help.size(), out);
#ifdef KYTHIRA_DYNAMODB_GATEWAY_IAM
        std::fputs(dynamodb_gateway::iam_auth_usage().c_str(), out);
#endif
        return help ? 0 : 2;
    }

    // Block the stop signals in every thread, so sigwait() below receives
    // them; the server's threads inherit the mask.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    // After the mask, so any thread Folly starts leaves SIGTERM to sigwait().
    const folly::Init folly_init(&argc, &argv);
#endif

    const kythira::dynamodb::env_lookup env =
        [](std::string_view name) -> std::optional<std::string> {
        const std::string n(name);
        const char* v = std::getenv(n.c_str());  // NOLINT(concurrency-mt-unsafe): before any thread
        return v == nullptr ? std::nullopt : std::optional<std::string>(v);
    };
    // Read here first so a bad setting exits 2 before anything is built;
    // the module reads the same ones.
    auto parsed = ddb::gateway_config_from_env(env);
    if (auto* e = std::get_if<std::string>(&parsed)) {
        std::cerr << "dynamodb_gateway_node: " << *e << "\n";
        return 2;
    }
    const auto& cfg = std::get<ddb::gateway_config>(parsed);

    namespace sn = kythira::service_node;
    using dynamodb_gateway::standalone_host;
    // The Raft transport. A node with no peers has no Raft: its server is
    // never started, so it binds nothing.
    std::optional<dynamodb_gateway::host_spec> spec;
    try {
        if (cfg.cluster.enabled()) {
            spec = dynamodb_gateway::cluster_spec(cfg.cluster);
        }
    } catch (const std::exception& e) {
        std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
        return 2;
    }
    dynamodb_gateway::beast_stack stack(
        spec ? spec->bind : std::string("127.0.0.1"), spec ? spec->port : std::uint16_t{0},
        spec ? spec->urls : std::unordered_map<dynamodb_gateway::node_id, std::string>{});
    sn::node_config node;
    node.node_id = cfg.cluster.node_id;
    node.data_dir = cfg.data_dir;
    node.raft_bind = spec ? spec->bind : std::string();
    node.raft_port = spec ? spec->port : std::uint16_t{0};
    standalone_host host(node, spec ? spec->self : kythira::host_node_id_from_number(1),
                         *stack._client, stack._server);

    sn::module_plan<standalone_host> plan;
    plan.modules.push_back(
        std::make_unique<dynamodb_gateway::dynamodb_module<standalone_host>>(true));
    plan.spaces.push_back(sn::group_space::standalone(sn::module_numbers::dynamodb));
    sn::node_runner<standalone_host> runner(
        std::move(plan), node, [](bool error, std::string_view line) {
            std::cerr << (error ? "[warn] " : "[info] ") << "dynamodb_gateway_node: " << line
                      << "\n";
        });
    int status = 0;
    try {
        runner.start(host, env);
        const int sig = wait_for_signal();
        std::cerr << "[info] dynamodb_gateway_node: signal " << sig << ", stopping\n";
    } catch (const std::invalid_argument& e) {
        std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
        status = 2;
    } catch (const std::exception& e) {
        std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
        status = 1;
    }
    // The module stops its gateway, backends and hosts, then the transport
    // goes.
    runner.stop([&stack] { stack.shutdown(); });
    return status;
}
