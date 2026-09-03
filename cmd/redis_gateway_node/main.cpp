// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `redis_gateway_node`: a Redis-compatible cache front end over a
///        Kythira multi-Raft cluster (.kiro/specs/redis-compatible-kv/ task 10).
///
/// Two modes. `--hash-secret` reads a secret from stdin and prints the ACL
/// record for it, so the plaintext secret never has to be written into a
/// file or a command line. Otherwise the process is the daemon, configured
/// entirely from the environment (`config.hpp`), stopped by SIGINT/SIGTERM.

#include "config.hpp"
#include "host_runners.hpp"
#include "stop_signal.hpp"

#include <raft/redis_acl.hpp>

#include <folly/init/Init.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>

namespace kythira::redis_node {

std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;

void on_signal(int) {
    g_stop.store(true);
    g_stop_cv.notify_all();
}

void wait_for_stop() {
    std::unique_lock lock(g_stop_mu);
    g_stop_cv.wait(lock, [] { return g_stop.load(); });
}

}  // namespace kythira::redis_node

namespace {

auto hash_secret(int argc, char** argv) -> int {
    std::uint32_t iterations = kythira::redis_acl_default_iterations;
    if (argc >= 3) {
        try {
            iterations = static_cast<std::uint32_t>(std::stoul(argv[2]));
        } catch (const std::exception&) {
            std::cerr << "redis_gateway_node: --hash-secret iterations must be an integer\n";
            return 2;
        }
        if (iterations < 1000) {
            std::cerr << "redis_gateway_node: refusing fewer than 1000 iterations\n";
            return 2;
        }
    }
    std::string secret;
    if (!std::getline(std::cin, secret)) {
        std::cerr << "redis_gateway_node: --hash-secret reads the secret from stdin\n";
        return 2;
    }
    if (!secret.empty() && secret.back() == '\r') {
        secret.pop_back();
    }
    if (secret.empty()) {
        std::cerr << "redis_gateway_node: empty secret\n";
        return 2;
    }
    std::cout << kythira::redis_acl::hash_secret(secret, iterations) << '\n';
    return 0;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::cout << kythira::redis_node::usage();
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--hash-secret") == 0) {
        return hash_secret(argc, argv);
    }
    if (argc >= 2) {
        std::cerr << "redis_gateway_node: unknown argument " << argv[1] << "\n"
                  << kythira::redis_node::usage();
        return 2;
    }

    folly::Init init(&argc, &argv);

    kythira::redis_node::node_options opt;
    try {
        opt = kythira::redis_node::from_env();
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }

    std::signal(SIGINT, kythira::redis_node::on_signal);
    std::signal(SIGTERM, kythira::redis_node::on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        switch (opt._serializer) {
            case kythira::redis_node::wire_serializer::cbor:
                return kythira::redis_node::run_cbor(opt);
            case kythira::redis_node::wire_serializer::json:
                return kythira::redis_node::run_json(opt);
        }
    } catch (const std::exception& e) {
        std::cerr << "redis_gateway_node: " << e.what() << "\n";
        return 1;
    }
    return 2;
}
