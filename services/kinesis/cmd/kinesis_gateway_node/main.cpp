// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `kinesis_gateway_node`: a Kinesis Data Streams-compatible front
///        end over a Kythira multi-Raft cluster
///        (`.kiro/specs/kinesis-compatible-streams/` task 6).
///
/// Configured entirely from the environment (`config.hpp`); stopped by
/// SIGINT or SIGTERM. `--check-config` reads the environment and prints
/// the settings without binding anything, and `--self-test` checks the
/// generated tables, so both work in a build tree with no cluster.

#include "config.hpp"
#include "stop_signal.hpp"
#ifdef KYTHIRA_KINESIS_GATEWAY_IAM
#include "iam_auth.hpp"
#endif

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_operations.hpp>

#include <folly/init/Init.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string_view>

namespace kythira::kinesis_node {

namespace {
std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;
}  // namespace

void on_signal(int) {
    g_stop.store(true);
    g_stop_cv.notify_all();
}

void wait_for_stop() {
    std::unique_lock lock(g_stop_mu);
    g_stop_cv.wait(lock, [] { return g_stop.load(); });
}

}  // namespace kythira::kinesis_node

namespace {

auto self_test() -> int {
    namespace k = kythira::kinesis;
    const auto& ops = k::operations;
    const bool sorted = std::is_sorted(
        ops.begin(), ops.end(), [](const auto& a, const auto& b) { return a._name < b._name; });
    if (!sorted) {
        std::fputs("self-test: operation table is not sorted\n", stderr);
        return 1;
    }
    const auto* subscribe = std::find_if(
        ops.begin(), ops.end(), [](const auto& o) { return o._name == "SubscribeToShard"; });
    if (subscribe == ops.end() || !subscribe->_event_stream) {
        std::fputs("self-test: SubscribeToShard missing or not an event stream\n", stderr);
        return 1;
    }
    // Every operation has its IAM actions.
    for (const auto& o : ops) {
        if (k::find_operation_actions(o._name) == nullptr) {
            std::fprintf(stderr, "self-test: no IAM actions for %.*s\n",
                         static_cast<int>(o._name.size()), o._name.data());
            return 1;
        }
    }
    const auto streams =
        std::count_if(ops.begin(), ops.end(), [](const auto& o) { return o._event_stream; });
    std::printf(
        "self-test ok: %zu operations (%td event stream), target prefix %.*s, botocore %.*s\n",
        ops.size(), streams, static_cast<int>(k::target_prefix.size()), k::target_prefix.data(),
        static_cast<int>(k::botocore_version.size()), k::botocore_version.data());
    return 0;
}

auto env(const char* name) -> const char* {
    return std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read before any thread starts
}

}  // namespace

auto main(int argc, char** argv) -> int {
    namespace kn = kythira::kinesis_node;
    const std::string_view arg = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (argc > 2) {
        std::cerr << kn::usage();
        return 2;
    }
    if (arg == "--help" || arg == "-h") {
        std::cout << kn::usage();
#ifdef KYTHIRA_KINESIS_GATEWAY_IAM
        std::cout << kn::iam_auth_usage();
#endif
        return 0;
    }
    if (arg == "--self-test") {
        return self_test();
    }
    if (!arg.empty() && arg != "--check-config") {
        std::cerr << "kinesis_gateway_node: unknown argument " << arg << "\n" << kn::usage();
        return 2;
    }

    kn::node_options opt;
    try {
        opt = kn::from_env(env);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    if (arg == "--check-config") {
        std::cout << kn::describe(opt);
        return 0;
    }

    folly::Init init(&argc, &argv);
    if (!kn::is_loopback_address(opt._bind_address)) {
        std::cerr << "kinesis_gateway_node: WARNING: Raft RPC is PLAINTEXT and UNAUTHENTICATED on "
                  << opt._bind_address << ":" << opt._raft_port
                  << " (KYTHIRA_ALLOW_PLAINTEXT_RAFT)\n";
    }
    std::signal(SIGINT, kn::on_signal);
    std::signal(SIGTERM, kn::on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        // The module starts IAM's replica for KINESIS_AUTH=iam or both
        // (kinesis_module.hpp), and refuses them in a build without it.
        return kn::run(opt);
    } catch (const std::exception& e) {
        std::cerr << "kinesis_gateway_node: " << e.what() << "\n";
        return 1;
    }
}
