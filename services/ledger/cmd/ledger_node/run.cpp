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
#include "export_store.hpp"

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
#include <raft/prometheus_metrics.hpp>
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

/// Samples every ledger this node hosts into the registry as gauges
/// (Requirement 11.1): the state machine's counts since open, its journal
/// bytes by where they live, and whether this node leads the ledger.
auto sample_metrics(kythira::prometheus_registry& reg, const ledger_host& host, raft_host& raft)
    -> void {
    using dims = std::vector<std::pair<std::string, std::string>>;
    const auto hs = host.stats();
    reg.observe_gauge("kythira_ledger_node_groups_created", {}, double(hs.groups_created));
    reg.observe_gauge("kythira_ledger_node_groups_destroyed", {}, double(hs.groups_destroyed));
    reg.observe_gauge("kythira_ledger_node_index_steps", {}, double(hs.index_steps));
    reg.observe_gauge("kythira_ledger_node_seals", {}, double(hs.seals));
    reg.observe_gauge("kythira_ledger_node_maintenance_errors", {}, double(hs.maintenance_errors));
    reg.observe_gauge("kythira_ledger_node_split_merge_vetoes", {}, double(hs.split_merge_vetoes));
    std::vector<kythira::ledger::ledger_record> ledgers;
    try {
        ledgers = host.catalog().list();
    } catch (const kythira::ledger::ledger_not_leader&) {
        return;  // the catalog group has not opened here yet
    }
    for (const auto& r : ledgers) {
        const auto sm = host.ledger(r.ledger_id);
        if (!sm || r.state != "active") {
            continue;
        }
        const auto st = sm->stats();
        const dims d{{"ledger", r.name}};
        reg.observe_gauge("kythira_ledger_blocks", d, double(st.blocks));
        reg.observe_gauge("kythira_ledger_blocks_applied", d, double(st.blocks_applied));
        for (const auto& [code, n] : st.rejections) {
            reg.observe_gauge("kythira_ledger_rejections", {{"ledger", r.name}, {"code", code}},
                              double(n));
        }
        reg.observe_gauge("kythira_ledger_hash_check_failures", d, double(st.hash_mismatches));
        for (const auto& [where, n] : {std::pair{"unsealed", st.journal_bytes_unsealed},
                                       std::pair{"local", st.journal_bytes_local},
                                       std::pair{"offloaded", st.journal_bytes_offloaded}}) {
            reg.observe_gauge("kythira_ledger_journal_bytes",
                              {{"ledger", r.name}, {"where", where}}, double(n));
        }
        reg.observe_gauge("kythira_ledger_segments", d, double(st.segments));
        reg.observe_gauge("kythira_ledger_indexes_building", d, double(st.indexes_building));
        bool leads = false;
        if (r.group) {
            const auto* n = raft.group_node(*r.group);
            leads = n != nullptr && n->is_leader();
        }
        reg.observe_gauge("kythira_ledger_leader", d, leads ? 1.0 : 0.0);
    }
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
    std::shared_ptr<kythira::prometheus_registry> metrics;
    if (opt.metrics_port != 0) {
        metrics = std::make_shared<kythira::prometheus_registry>();
    }
    ctx->on_open = [&log, self, metrics](const kythira::key_range& range,
                                         const group_sm::opened& sm) {
        const auto* l = std::get_if<std::shared_ptr<ledger_sm>>(&sm);
        if (l == nullptr) {
            return;
        }
        const auto id = kythira::ledger::ledger_id_of_range(range).value_or(range.start);
        (*l)->set_hash_mismatch_handler([&log, self, id,
                                         metrics](const kythira::ledger::ledger_hash_mismatch& m) {
            // The alert metric: a counter, so an alert rule fires on
            // any increase without waiting for the next sample.
            if (metrics) {
                metrics->observe_counter("kythira_ledger_hash_mismatch", {{"ledger_id", id}}, 1.0);
            }
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
    // Export jobs run on the maintenance thread of the ledger's leader and
    // write through this store; without one they finish as failed.
    const std::shared_ptr<const export_store> exports = make_export_store(opt);
    if (exports) {
        hopts.run_export = [exports](const ledger_host::ledger_sm& sm,
                                     const kythira::ledger::export_job& job) {
            return kythira::ledger::export_journal(sm, *exports, job);
        };
        log(kythira::log_level::info, "ledger_node: journal exports write to S3",
            {{"region", opt.export_s3_region}, {"endpoint", opt.export_s3_endpoint}});
    }
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
    // GetDigest's latency per ledger (Requirement 11.1), timed around the
    // whole API call: digest, signature and encoding.
    const auto handle = [&api, metrics](const kythira::ledger::api_request& r) {
        constexpr std::string_view prefix = "/v1/ledgers/";
        constexpr std::string_view suffix = "/digest";
        if (!metrics || r.method != "GET" || !r.path.starts_with(prefix) ||
            !r.path.ends_with(suffix) || r.path.size() <= prefix.size() + suffix.size()) {
            return api.handle(r);
        }
        const auto started = std::chrono::steady_clock::now();
        auto response = api.handle(r);
        if (response.status == 200) {
            const auto name =
                r.path.substr(prefix.size(), r.path.size() - prefix.size() - suffix.size());
            metrics->observe_duration("kythira_ledger_digest_latency", {{"ledger", name}},
                                      std::chrono::duration<double, std::milli>(
                                          std::chrono::steady_clock::now() - started)
                                          .count());
        }
        return response;
    };
    kythira::https::server server(
        http, kythira::ledger::make_ledger_handler(handle),
        [&log](bool error, std::string_view message) {
            log(error ? kythira::log_level::warning : kythira::log_level::info, message);
        });

    std::unique_ptr<kythira::prometheus_scrape_server> scrape;
    int status = 0;
    try {
        server.start();
        if (metrics) {
            sample_metrics(*metrics, *host, *raft);
            scrape = std::make_unique<kythira::prometheus_scrape_server>(metrics, opt.metrics_bind,
                                                                         opt.metrics_port);
        }
        std::cout << "ledger_node " << self << " ready: raft=" << opt.raft_port
                  << (opt.raft_tls ? " (mutual TLS)" : " (plaintext)") << " api=" << server.port()
                  << " members=" << opt.peers.size() << " replication=" << opt.replication
                  << " signing=" << (signer ? "on" : "off")
                  << " metrics=" << (scrape ? std::to_string(scrape->port()) : "off") << std::endl;
        auto next_sample = std::chrono::steady_clock::now() + opt.metrics_interval;
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            if (metrics && std::chrono::steady_clock::now() >= next_sample) {
                next_sample += opt.metrics_interval;
                try {
                    sample_metrics(*metrics, *host, *raft);
                } catch (const std::exception& e) {
                    log(kythira::log_level::warning, "ledger_node: metrics sample failed",
                        {{"error", e.what()}});
                }
            }
        }
        log(kythira::log_level::info, "ledger_node: stopping");
    } catch (const std::exception& e) {
        log(kythira::log_level::critical, "ledger_node: cannot serve", {{"error", e.what()}});
        status = 1;
    }

    // The order matters: no request may reach a host or group being torn
    // down.
    if (scrape) {
        scrape->stop();
    }
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
