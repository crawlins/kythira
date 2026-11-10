// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// dynamodb_gateway_node: the DynamoDB-compatible daemon
// (.kiro/specs/dynamodb-compatible-api/ task 7).
//
// Reads its settings from the environment (gateway.hpp lists them), builds
// the backend and serves the front end until SIGINT or SIGTERM. So far the
// backend is local_backend on memory_kv_engine: one node, no Raft, nothing
// kept across a restart.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/gateway.hpp>

#include <raft/memory_kv_engine.hpp>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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
        return help ? 0 : 2;
    }

    // Block the stop signals in every thread, so sigwait() below receives
    // them; the server's threads inherit the mask.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);

    auto parsed =
        ddb::gateway_config_from_env([](std::string_view name) -> std::optional<std::string> {
            const std::string n(name);
            const char* v =
                std::getenv(n.c_str());  // NOLINT(concurrency-mt-unsafe): before any thread
            return v == nullptr ? std::nullopt : std::optional<std::string>(v);
        });
    if (auto* e = std::get_if<std::string>(&parsed)) {
        std::cerr << "dynamodb_gateway_node: " << *e << "\n";
        return 2;
    }
    auto& cfg = std::get<ddb::gateway_config>(parsed);

    try {
        kythira::memory_kv_engine engine;
        ddb::local_backend<> backend(engine);
        ddb::gateway<ddb::local_backend<>> gw(cfg, backend, [](bool warning, std::string_view m) {
            std::cerr << (warning ? "[warn] " : "[info] ") << "dynamodb_gateway_node: " << m
                      << "\n";
        });
        gw.start();
        std::cerr << "[info] dynamodb_gateway_node: serving "
                  << (cfg.http.insecure_plaintext ? "http" : "https") << " on "
                  << cfg.http.bind_address << ":" << gw.port() << ", engine " << cfg.engine
                  << " (nothing is kept across a restart)\n";
        const int sig = wait_for_signal();
        std::cerr << "[info] dynamodb_gateway_node: signal " << sig << ", stopping\n";
        gw.stop();
    } catch (const std::exception& e) {
        std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
