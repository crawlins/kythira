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

#include "backup_s3.hpp"
#include "run_cluster.hpp"
#ifdef KYTHIRA_DYNAMODB_GATEWAY_IAM
#include "iam_auth.hpp"
#endif

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/gateway.hpp>

#include <raft/memory_kv_engine.hpp>
#if defined(KYTHIRA_DYNAMODB_ROCKSDB)
#include <raft/rocksdb_kv_engine.hpp>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
// The Folly backend (the default, which defines no macro of its own) needs
// folly::Init before the first future: its timekeeper is a Folly singleton.
#include <folly/init/Init.h>
#endif

#include <filesystem>
#include <memory>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
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

#if defined(KYTHIRA_DYNAMODB_ROCKSDB)
/// One RocksDB database per node, the Raft logs beside it.
struct rocksdb_storage {
    using engine_type = kythira::rocksdb_kv_engine;
    using persistence_engine_type =
        kythira::binary_file_persistence_engine<dynamodb_gateway::node_id, std::uint64_t,
                                                std::uint64_t>;
    static constexpr std::string_view k_name = "rocksdb";
};
#endif

auto run_cluster_node(const kythira::dynamodb::gateway_config& cfg,
                      const std::function<void(bool, std::string_view)>& log) -> void {
    const auto wait = [&log] {
        const int sig = wait_for_signal();
        log(false, "signal " + std::to_string(sig) + ", stopping");
    };
    if (cfg.engine == "memory") {
        using storage = dynamodb_gateway::memory_storage;
        dynamodb_gateway::run_cluster<storage>(
            cfg, std::make_shared<storage::engine_type>(),
            [](std::uint64_t) { return storage::persistence_engine_type{}; }, log, wait);
        return;
    }
#if defined(KYTHIRA_DYNAMODB_ROCKSDB)
    // Layout: engine/ (the database) and raft/<group> (the Raft logs); the
    // strong host's (task 27) the same under strong/.
    const std::filesystem::path dir(cfg.data_dir);
    const auto storage_in = [](const std::filesystem::path& d) {
        std::filesystem::create_directories(d / "engine");
        std::filesystem::create_directories(d / "raft");
        return dynamodb_gateway::host_storage<rocksdb_storage>{
            std::make_shared<kythira::rocksdb_kv_engine>(d / "engine"), [d](std::uint64_t group) {
                return rocksdb_storage::persistence_engine_type{d / "raft" / std::to_string(group)};
            }};
    };
    auto local = storage_in(dir);
    std::optional<dynamodb_gateway::host_storage<rocksdb_storage>> strong;
    if (cfg.strong.enabled()) {
        strong = storage_in(dir / "strong");
    }
    dynamodb_gateway::run_cluster<rocksdb_storage>(
        cfg, std::move(local.engine), local.store_factory, log, wait, std::move(strong));
#else
    throw std::invalid_argument(
        "KYTHIRA_DYNAMODB_ENGINE=rocksdb: this build has no RocksDB (CONFIG_ENGINE_ROCKSDB)");
#endif
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
        if (!cfg.backups) {
            cfg.backups = dynamodb_gateway::make_s3_backup_store(cfg);
        }
        if (!cfg.buckets && cfg.bucket_dir.empty() && !cfg.backup_s3_bucket.empty()) {
            cfg.buckets = dynamodb_gateway::make_s3_bucket_resolver(cfg);
        }
    } catch (const std::exception& e) {
        std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
        return 2;
    }

    // IAM (task 28): the replica outlives the gateway that uses it.
#ifdef KYTHIRA_DYNAMODB_GATEWAY_IAM
    std::unique_ptr<dynamodb_gateway::iam_auth> iam;
#endif
    if (cfg.auth != ddb::auth_mode::static_keys) {
#ifdef KYTHIRA_DYNAMODB_GATEWAY_IAM
        try {
            iam = dynamodb_gateway::start_iam_auth_from_env();
            cfg.iam = iam->hooks();
        } catch (const std::exception& e) {
            std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
            return 2;
        }
#else
        std::cerr << "dynamodb_gateway_node: KYTHIRA_DYNAMODB_AUTH=iam or both needs a build "
                     "with CONFIG_DYNAMODB_GATEWAY_IAM\n";
        return 2;
#endif
    }

    const auto log = [](bool warning, std::string_view m) {
        std::cerr << (warning ? "[warn] " : "[info] ") << "dynamodb_gateway_node: " << m << "\n";
    };
    if (cfg.cluster.enabled()) {
        try {
            run_cluster_node(cfg, log);
        } catch (const std::exception& e) {
            std::cerr << "dynamodb_gateway_node: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    try {
        kythira::memory_kv_engine engine;
        std::optional<dynamodb_gateway::scratch_dir> scratch;
        const auto change_log = dynamodb_gateway::open_change_log(cfg, scratch);
        ddb::local_backend<> backend(engine, ddb::local_backend<>::system_clock_ms(),
                                     change_log.get());
        // One node on the memory engine keeps backups in memory too, unless
        // told where else (backup.hpp), and its export and import buckets
        // (export_import.hpp).
        if (!cfg.backups && cfg.backup_dir.empty()) {
            cfg.backups = std::make_shared<ddb::memory_backup_store>();
        }
        ddb::memory_buckets buckets;
        if (!cfg.buckets && cfg.bucket_dir.empty() && cfg.backup_dir.empty()) {
            cfg.buckets = buckets.resolver();
        }
        ddb::gateway<ddb::local_backend<>> gw(cfg, backend, log);
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
