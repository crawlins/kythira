// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `kythira_node`: the services named in `KYTHIRA_SERVICES`, from the
///        ones compiled in, on one Raft listener, identity, peer list and
///        health endpoint (.kiro/specs/service-colocation/ task 3,
///        Requirements 2 and 4).
///
/// Each module runs its own multi-raft host in its own group space
/// (`node_host.hpp`); this file only plans the modules, builds the node's
/// transport, and runs them until SIGINT or SIGTERM. The modules a build
/// compiles in register themselves (`module_registry.hpp`), so adding one
/// changes nothing here.

#include <raft/kythira_node_host.hpp>
#include <raft/module_registry.hpp>
#include <raft/node_core.hpp>
#include <raft/node_endpoint.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// Before the first future: Folly's timekeeper is a Folly singleton.
#include <folly/init/Init.h>
#endif

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace {

namespace sn = kythira::service_node;
using host_type = sn::kythira_node_host;

auto env_lookup() -> sn::env_lookup {
    return [](std::string_view name) -> std::optional<std::string> {
        const char* v = std::getenv(std::string(name).c_str());  // NOLINT(concurrency-mt-unsafe)
        if (v == nullptr || *v == '\0') {
            return std::nullopt;
        }
        return std::string(v);
    };
}

auto usage() -> std::string {
    auto& catalog = sn::module_catalog<host_type>::global();
    std::string out =
        "usage: kythira_node\n"
        "\n"
        "Runs the services named in KYTHIRA_SERVICES on one Raft listener.\n"
        "\n"
        "  KYTHIRA_SERVICES                 comma-separated services to run (required)\n" +
        sn::node_config_usage() + "\nCompiled-in services and their settings:\n";
    const auto names = catalog.names();
    if (names.empty()) {
        out += "  none\n";
    }
    for (const auto& name : names) {
        const auto m = catalog.make(name);
        out += "  " + name + "  (" + std::string(m->env_prefix()) + "*)\n";
    }
    return out;
}

/// One line per event on stderr, `level=... <line>`; modules' lines carry
/// `service=<name>` (`module_log`).
auto stderr_log() -> sn::log_sink {
    auto mutex = std::make_shared<std::mutex>();
    return [mutex](bool warning, std::string_view line) {
        const std::lock_guard lock(*mutex);
        std::cerr << (warning ? "level=warn " : "level=info ") << line << '\n';
    };
}

auto run() -> int {
    const auto log = stderr_log();
    const auto env = env_lookup();
    auto parsed = sn::node_config_from_env(env);
    if (auto* e = std::get_if<std::string>(&parsed)) {
        std::cerr << "kythira_node: " << *e << '\n';
        return 2;
    }
    auto config = std::get<sn::node_config>(std::move(parsed));
    auto planned = sn::plan_modules(sn::module_catalog<host_type>::global(),
                                    sn::split_services(env("KYTHIRA_SERVICES").value_or("")), true);
    if (auto* e = std::get_if<std::string>(&planned)) {
        std::cerr << "kythira_node: " << *e << '\n';
        return 2;
    }
    const auto id = kythira::parse_host_node_id(config.node_id);
    if (!id) {
        std::cerr << "kythira_node: KYTHIRA_NODE_ID: " << id.error() << '\n';
        return 2;
    }

    // Before any thread starts, so every thread inherits the mask and the
    // signal reaches wait_for_stop_signal.
    sn::block_stop_signals();

    sn::kythira_node_stack stack(config);
    host_type host(config, id.value(), stack.client(), stack.server());
    sn::node_runner<host_type> runner(std::get<sn::module_plan<host_type>>(std::move(planned)),
                                      config, log);
    sn::node_endpoint<host_type> endpoint(runner, log);
    try {
        runner.start(host, env);
        endpoint.start();
    } catch (const std::exception& e) {
        log(true, std::string("service=node stopping: ") + e.what());
        runner.stop([&stack] { stack.shutdown(); });
        return 1;
    }
    log(false, "service=node serving; metrics on port " + std::to_string(endpoint.port()));

    const int sig = sn::wait_for_stop_signal();
    log(false, "service=node signal " + std::to_string(sig) + ", stopping");
    endpoint.stop();
    runner.stop([&stack] { stack.shutdown(); });
    return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(
            argv[i]);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (arg == "--help" || arg == "-h") {
            std::cout << usage();
            return 0;
        }
        std::cerr << "kythira_node: unknown argument '" << arg << "'\n" << usage();
        return 2;
    }
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    const folly::Init folly_init(&argc, &argv);
#endif
    try {
        return run();
    } catch (const std::exception& e) {
        std::cerr << "kythira_node: " << e.what() << '\n';
        return 1;
    }
}
