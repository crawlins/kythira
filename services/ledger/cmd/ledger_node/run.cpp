// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// ledger_node's stack (.kiro/specs/verifiable-ledger/ task 7c): multi_raft
// over Boost.Beast (mutual TLS by default), one RocksDB database for every
// group's state, a binary file Raft log per group, the ledger host over
// it, and the ledger API on an HTTPS server with client certificates.
//
// Data directory:
//   engine/                 the RocksDB database (catalog and every ledger)
//   raft/<group>/           each group's Raft log
//   snapshots/group-<id>/   checkpoints and incoming snapshot transfers, on
//                           the engine's filesystem
//   segments/<ledger id>/   each ledger's sealed segment files

#include "config.hpp"

#include <kythira/ledger/journal_signing.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_group_state_machine.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/ledger_host.hpp>
#include <kythira/ledger/ledger_http.hpp>

#include <raft/beast_http_transport_impl.hpp>
#include <raft/beast_https_server.hpp>
#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/rocksdb_kv_engine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::ledger_node {

namespace {

using engine_type = kythira::rocksdb_kv_engine;
using group_sm = kythira::ledger::basic_ledger_group_state_machine<engine_type>;
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

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = kythira::host_node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::ledger_node::serializer_type;

    using network_client_type =
        kythira::transport_client_handle<raft_stack::client_type, node_id_type>;
    using network_server_type =
        kythira::transport_server_handle<raft_stack::server_type, node_id_type>;

    using persistence_engine_type =
        kythira::binary_file_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = group_sm;

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

using raft_host = kythira::multi_raft<host_types, std::string, std::uint64_t>;
using raft_config = kythira::multi_raft_config<host_types, std::string, std::uint64_t>;
using ledger_host = kythira::ledger::basic_ledger_host<raft_host, engine_type>;
using ledger_sm = ledger_host::ledger_sm;
using api_type = kythira::ledger::ledger_api<ledger_host, ledger_sm>;

/// The node's own log lines, from any thread (console_logger locks).
class node_log {
public:
    using fields = std::vector<std::pair<std::string_view, std::string_view>>;
    explicit node_log(kythira::log_level level) : logger_(level) {}
    auto operator()(kythira::log_level level, std::string_view message, const fields& f = {})
        -> void {
        logger_.log(level, message, f);
    }

private:
    kythira::console_logger logger_;
};

auto read_text(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ledger_node: cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto make_raft_config(const node_options& opt, raft_stack& stack,
                      const std::shared_ptr<group_sm::context>& ctx) -> raft_config {
    const auto raft_dir = opt.data_dir / "raft";
    raft_config cfg{
        .node_id = opt.node_id,
        .network_client = host_types::network_client_type{stack.client},
        .network_server = host_types::network_server_type{stack.server},
        .store_factory =
            [raft_dir](const std::uint64_t& group) {
                return host_types::persistence_engine_type{raft_dir / std::to_string(group)};
            },
    };
    cfg.state_engine = std::make_shared<engine_type>(opt.data_dir / "engine");
    cfg.state_machine_factory = [ctx](const std::uint64_t&) { return group_sm(ctx); };
    cfg.automatic_split_merge_enabled = false;  // ledger groups never move
    cfg.hibernation = kythira::hibernation_mode::off;
    cfg.logger = kythira::console_logger{opt.log_level};
    cfg.logger_factory = [level = opt.log_level](const std::uint64_t&) {
        return kythira::console_logger{level};
    };
    cfg.config._election_timeout_min = opt.election_timeout_min;
    cfg.config._election_timeout_max = opt.election_timeout_max;
    cfg.config._heartbeat_interval = opt.heartbeat_interval;
    cfg.config._rpc_timeout = opt.rpc_timeout;
    cfg.config._snapshot_staging_directory = opt.data_dir / "snapshots";
    cfg.config._snapshot_threshold_bytes = opt.snapshot_threshold_bytes;
    cfg.config._snapshot_disk_margin_bytes = opt.snapshot_disk_margin_bytes;
    return cfg;
}

}  // namespace

auto run(const node_options& opt) -> int {
    node_log log(opt.log_level);

    // Everything that can fail on a bad file or directory, before a port
    // is bound.
    const auto roles = kythira::ledger::ledger_roles::parse(read_text(opt.roles_file));
    std::shared_ptr<const kythira::ledger::digest_signer> signer;
    if (!opt.signing_key.empty()) {
        signer = std::make_shared<const kythira::ledger::digest_signer>(
            kythira::ledger::digest_signer::from_files(opt.signing_key, opt.signing_chain));
    }
    for (const auto* sub : {"engine", "raft", "snapshots", "segments"}) {
        std::filesystem::create_directories(opt.data_dir / sub);
    }

    // Every ledger's segments in its own directory; the hash-mismatch
    // alert (Requirement 3.3, 11.2) logged with the block and both hashes.
    const auto segments = opt.data_dir / "segments";
    auto ctx = ledger_host::make_context([segments, &opt](const std::string& ledger_id) {
        kythira::ledger::ledger_options o;
        o.segment_directory = segments / ledger_id;
        o.segment_bytes = opt.segment_bytes;
        return o;
    });
    const auto self = kythira::host_node_id_text(opt.node_id);
    ctx->on_open = [&log, self](const kythira::key_range& range, const group_sm::opened& sm) {
        const auto* l = std::get_if<std::shared_ptr<ledger_sm>>(&sm);
        if (l == nullptr) {
            return;
        }
        const auto id = kythira::ledger::ledger_id_of_range(range).value_or(range.start);
        (*l)->set_hash_mismatch_handler(
            [&log, self, id](const kythira::ledger::ledger_hash_mismatch& m) {
                log(kythira::log_level::critical, "ledger: block hash mismatch",
                    {{"ledger", id},
                     {"block", std::to_string(m.seq)},
                     {"leader_hash", kythira::ledger::to_hex(m.leader)},
                     {"replica_hash", kythira::ledger::to_hex(m.replica)},
                     {"replica", self}});
            });
    };

    raft_stack stack(opt);
    auto raft = std::make_unique<raft_host>(make_raft_config(opt, stack, ctx));

    ledger_host::options hopts;
    for (const auto& [id, url] : opt.peers) {
        hopts.members.push_back(id);
    }
    hopts.replication = opt.replication;
    hopts.api_urls = {opt.api_urls.begin(), opt.api_urls.end()};
    hopts.propose_timeout = opt.propose_timeout;
    hopts.maintenance_interval = opt.maintenance_interval;
    auto host = std::make_unique<ledger_host>(*raft, ctx, hopts);
    api_type api(*host, roles);
    if (signer) {
        api.set_digest_signer(signer);
    }

    host->bootstrap();
    raft->start();
    std::atomic<bool> driving{true};
    std::thread driver([&] {
        while (driving.load(std::memory_order_acquire)) {
            try {
                (void)raft->tick();
            } catch (const std::exception& e) {
                log(kythira::log_level::error, "ledger_node: tick failed", {{"error", e.what()}});
            }
            std::this_thread::sleep_for(opt.tick_interval);
        }
    });
    host->start();

    auto http = kythira::ledger::ledger_http_limits();
    http.bind_address = opt.api_bind;
    http.port = opt.api_port;
    http.tls_cert_path = opt.tls_cert;
    http.tls_key_path = opt.tls_key;
    http.client_ca_path = opt.client_ca;
    http.max_body_bytes = opt.max_body_bytes;
    kythira::https::server server(
        http, kythira::ledger::make_ledger_handler([&api](const auto& r) { return api.handle(r); }),
        [&log](bool error, std::string_view message) {
            log(error ? kythira::log_level::warning : kythira::log_level::info, message);
        });

    int status = 0;
    try {
        server.start();
        std::cout << "ledger_node " << self << " ready: raft=" << opt.raft_port
                  << (opt.raft_tls ? " (mutual TLS)" : " (plaintext)") << " api=" << server.port()
                  << " members=" << opt.peers.size() << " replication=" << opt.replication
                  << " signing=" << (signer ? "on" : "off") << std::endl;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        log(kythira::log_level::info, "ledger_node: stopping");
    } catch (const std::exception& e) {
        log(kythira::log_level::critical, "ledger_node: cannot serve", {{"error", e.what()}});
        status = 1;
    }

    // The order matters: no request may reach a host or group being torn
    // down.
    server.stop();
    host->stop();
    driving.store(false, std::memory_order_release);
    driver.join();
    raft->stop();
    host.reset();
    raft.reset();
    stack.shutdown();
    return status;
}

auto install_signal_handlers() -> void {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cppcoreguidelines-pro-type-cstyle-cast)
}

}  // namespace kythira::ledger_node
