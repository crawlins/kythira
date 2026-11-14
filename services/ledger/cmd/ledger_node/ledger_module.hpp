// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

// The ledger as a service module (.kiro/specs/service-colocation/ task 4):
// what ledger_node's run() did between its Raft transport and its signal
// loop, behind service_module<Host> so that ledger_node (one module, its
// own JSON Beast transport, ids and data directory exactly as before) and
// kythira_node (beside other modules, on the node's shared listener, in
// the ledger's group space) run the same code.
//
// `Host` is a service_node::node_host: the module's multi_raft host takes
// the node's client and `server_for(space)`, and ledger_host maps the
// catalog's group ids into the space (ledger_host::options::group_base).
//
// Data directory (KYTHIRA_LEDGER_DATA_DIR stand-alone, the node's data
// directory's ledger/ in kythira_node):
//   engine/                 the RocksDB database (catalog and every ledger)
//   raft/<group>/           each group's Raft log
//   snapshots/group-<id>/   checkpoints and incoming snapshot transfers
//   segments/<ledger id>/   each ledger's sealed segment files

#include "config.hpp"
#include "export_store.hpp"

#include <kythira/ledger/journal_signing.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_group_state_machine.hpp>
#include <kythira/ledger/ledger_hash.hpp>
#include <kythira/ledger/ledger_host.hpp>
#include <kythira/ledger/ledger_http.hpp>

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
#include <raft/service_module.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::ledger_node {

using engine_type = kythira::rocksdb_kv_engine;
using group_sm = kythira::ledger::basic_ledger_group_state_machine<engine_type>;

/// The ledger's multi_raft types on a node whose transport is `Host`'s.
template<typename Host> struct module_host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = kythira::host_node_id;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    /// Not used by the host: the wire is the node's transport's.
    using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;

    using network_client_type =
        kythira::transport_client_handle<typename Host::client_type, node_id_type>;
    using network_server_type = typename Host::view_type;

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

inline auto read_text(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("ledger_node: cannot read " + path.string());
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

template<typename Host> class ledger_module final : public service_node::service_module<Host> {
public:
    using types = module_host_types<Host>;
    using raft_host = kythira::multi_raft<types, std::string, std::uint64_t>;
    using raft_config = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using ledger_host = kythira::ledger::basic_ledger_host<raft_host, engine_type>;
    using ledger_sm = typename ledger_host::ledger_sm;
    using api_type = kythira::ledger::ledger_api<ledger_host, ledger_sm>;

    /// @param standalone ledger_node's module: its own variables for the
    ///        node-wide settings and the data directory itself. Otherwise
    ///        kythira_node's: the node's settings fill in what the
    ///        KYTHIRA_LEDGER_* variables leave unset (composite_env), and
    ///        the data directory is the node's ledger/.
    explicit ledger_module(bool standalone) : standalone_(standalone) {}

    ledger_module(const ledger_module&) = delete;
    auto operator=(const ledger_module&) -> ledger_module& = delete;
    ledger_module(ledger_module&&) = delete;
    auto operator=(ledger_module&&) -> ledger_module& = delete;
    ~ledger_module() override { stop(); }

    [[nodiscard]] auto name() const -> std::string_view override { return "ledger"; }
    [[nodiscard]] auto number() const -> std::uint8_t override {
        return service_node::module_numbers::ledger;
    }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_LEDGER_"; }
    [[nodiscard]] auto data_subdir() const -> std::string override {
        return standalone_ ? std::string() : std::string("ledger");
    }

    [[nodiscard]] auto configure(const service_node::env_lookup& env)
        -> std::optional<std::string> override {
        try {
            opt_ = from_env(standalone_ ? env_fn(env) : composite_env(env));
            log_.emplace(opt_.log_level);
            // Everything that can fail on a bad file, before a port is
            // bound.
            roles_.emplace(kythira::ledger::ledger_roles::parse(read_text(opt_.roles_file)));
            if (!opt_.signing_key.empty()) {
                signer_ = std::make_shared<const kythira::ledger::digest_signer>(
                    kythira::ledger::digest_signer::from_files(opt_.signing_key,
                                                               opt_.signing_chain));
            }
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::nullopt;
    }

    void register_groups(Host& host, const service_node::group_space& space) override {
        for (const auto* sub : {"engine", "raft", "snapshots", "segments"}) {
            std::filesystem::create_directories(opt_.data_dir / sub);
        }
        self_ = kythira::host_node_id_text(opt_.node_id);
        if (opt_.metrics_port != 0) {
            prom_ = std::make_shared<kythira::prometheus_registry>();
        }
        ctx_ = make_context();
        raft_ = std::make_unique<raft_host>(make_raft_config(host, space));

        typename ledger_host::options hopts;
        for (const auto& [id, url] : opt_.peers) {
            hopts.members.push_back(id);
        }
        hopts.replication = opt_.replication;
        hopts.api_urls = {opt_.api_urls.begin(), opt_.api_urls.end()};
        hopts.propose_timeout = opt_.propose_timeout;
        hopts.maintenance_interval = opt_.maintenance_interval;
        hopts.group_base = space.first();
        // Export jobs run on the maintenance thread of the ledger's leader
        // and write through this store; without one they finish as failed.
        exports_ = make_export_store(opt_);
        if (exports_) {
            hopts.run_export = [exports = exports_](const ledger_sm& sm,
                                                    const kythira::ledger::export_job& job) {
                return kythira::ledger::export_journal(sm, *exports, job);
            };
            (*log_)(kythira::log_level::info, "ledger_node: journal exports write to S3",
                    {{"region", opt_.export_s3_region}, {"endpoint", opt_.export_s3_endpoint}});
        }
        host_ = std::make_unique<ledger_host>(*raft_, ctx_, hopts);
        api_ = std::make_unique<api_type>(*host_, *roles_);
        if (signer_) {
            api_->set_digest_signer(signer_);
        }
        host_->bootstrap();
    }

    void start(service_node::module_registry& /*registry*/) override {
        raft_->start();
        driving_ = true;
        driver_ = std::thread([this] {
            while (driving_.load(std::memory_order_acquire)) {
                try {
                    (void)raft_->tick();
                } catch (const std::exception& e) {
                    (*log_)(kythira::log_level::error, "ledger_node: tick failed",
                            {{"error", e.what()}});
                }
                std::this_thread::sleep_for(opt_.tick_interval);
            }
        });
        host_->start();

        auto http = kythira::ledger::ledger_http_limits();
        http.bind_address = opt_.api_bind;
        http.port = opt_.api_port;
        http.tls_cert_path = opt_.tls_cert;
        http.tls_key_path = opt_.tls_key;
        http.client_ca_path = opt_.client_ca;
        http.max_body_bytes = opt_.max_body_bytes;
        server_ = std::make_unique<kythira::https::server>(
            http,
            kythira::ledger::make_ledger_handler(
                [this](const kythira::ledger::api_request& r) { return handle(r); }),
            [this](bool error, std::string_view message) {
                (*log_)(error ? kythira::log_level::warning : kythira::log_level::info, message);
            });
        server_->start();
        if (prom_) {
            sample_metrics();
            scrape_ = std::make_unique<kythira::prometheus_scrape_server>(prom_, opt_.metrics_bind,
                                                                          opt_.metrics_port);
            sampling_ = true;
            sampler_ = std::thread([this] { sample_loop(); });
        }
        started_ = true;
        std::cout << "ledger_node " << self_ << " ready: raft=" << opt_.raft_port
                  << (opt_.raft_tls ? " (mutual TLS)" : " (plaintext)")
                  << " api=" << server_->port() << " members=" << opt_.peers.size()
                  << " replication=" << opt_.replication << " signing=" << (signer_ ? "on" : "off")
                  << " metrics=" << (scrape_ ? std::to_string(scrape_->port()) : "off")
                  << std::endl;
    }

    /// No new API request reaches a host or group being torn down.
    void drain() override {
        if (log_ && started_) {
            (*log_)(kythira::log_level::info, "ledger_node: stopping");
        }
        stop_sampler();
        if (scrape_) {
            scrape_->stop();
        }
        if (server_) {
            server_->stop();
        }
    }

    void stop() override {
        drain();
        if (host_) {
            host_->stop();
        }
        driving_.store(false, std::memory_order_release);
        if (driver_.joinable()) {
            driver_.join();
        }
        if (raft_) {
            raft_->stop();
        }
        api_.reset();
        host_.reset();
        raft_.reset();
        started_ = false;
    }

    [[nodiscard]] auto health() const -> service_node::module_health override {
        if (!started_) {
            return {false, "not started"};
        }
        return {true, {}};
    }

    [[nodiscard]] auto metrics() const -> std::vector<service_node::module_metric> override {
        if (!host_) {
            return {};
        }
        const auto hs = host_->stats();
        return {{"groups_created", double(hs.groups_created), {}},
                {"groups_destroyed", double(hs.groups_destroyed), {}},
                {"seals", double(hs.seals), {}},
                {"maintenance_errors", double(hs.maintenance_errors), {}}};
    }

    /// The API's port once started (tests pass port 0).
    [[nodiscard]] auto api_port() const -> std::uint16_t { return server_ ? server_->port() : 0; }

private:
    auto make_context() -> std::shared_ptr<typename group_sm::context> {
        // Every ledger's segments in its own directory; the hash-mismatch
        // alert (Requirement 3.3, 11.2) logged with the block and both
        // hashes.
        const auto segments = opt_.data_dir / "segments";
        auto ctx = ledger_host::make_context(
            [segments, bytes = opt_.segment_bytes](const std::string& ledger_id) {
                kythira::ledger::ledger_options o;
                o.segment_directory = segments / ledger_id;
                o.segment_bytes = bytes;
                return o;
            });
        ctx->on_open = [this](const kythira::key_range& range,
                              const typename group_sm::opened& sm) {
            const auto* l = std::get_if<std::shared_ptr<ledger_sm>>(&sm);
            if (l == nullptr) {
                return;
            }
            const auto id = kythira::ledger::ledger_id_of_range(range).value_or(range.start);
            (*l)->set_hash_mismatch_handler(
                [this, id](const kythira::ledger::ledger_hash_mismatch& m) {
                    // The alert metric: a counter, so an alert rule fires on
                    // any increase without waiting for the next sample.
                    if (prom_) {
                        prom_->observe_counter("kythira_ledger_hash_mismatch", {{"ledger_id", id}},
                                               1.0);
                    }
                    (*log_)(kythira::log_level::critical, "ledger: block hash mismatch",
                            {{"ledger", id},
                             {"block", std::to_string(m.seq)},
                             {"leader_hash", kythira::ledger::to_hex(m.leader)},
                             {"replica_hash", kythira::ledger::to_hex(m.replica)},
                             {"replica", self_}});
                });
        };
        return ctx;
    }

    auto make_raft_config(Host& host, const service_node::group_space& space) -> raft_config {
        const auto raft_dir = opt_.data_dir / "raft";
        raft_config cfg{
            .node_id = opt_.node_id,
            .network_client = typename types::network_client_type{host.client()},
            .network_server = host.server_for(space),
            .store_factory =
                [raft_dir](const std::uint64_t& group) {
                    return
                        typename types::persistence_engine_type{raft_dir / std::to_string(group)};
                },
        };
        cfg.state_engine = std::make_shared<engine_type>(opt_.data_dir / "engine");
        cfg.state_machine_factory = [ctx = ctx_](const std::uint64_t&) { return group_sm(ctx); };
        cfg.automatic_split_merge_enabled = false;  // ledger groups never move
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.logger = kythira::console_logger{opt_.log_level};
        cfg.logger_factory = [level = opt_.log_level](const std::uint64_t&) {
            return kythira::console_logger{level};
        };
        cfg.config._election_timeout_min = opt_.election_timeout_min;
        cfg.config._election_timeout_max = opt_.election_timeout_max;
        cfg.config._heartbeat_interval = opt_.heartbeat_interval;
        cfg.config._rpc_timeout = opt_.rpc_timeout;
        cfg.config._snapshot_staging_directory = opt_.data_dir / "snapshots";
        cfg.config._snapshot_threshold_bytes = opt_.snapshot_threshold_bytes;
        cfg.config._snapshot_disk_margin_bytes = opt_.snapshot_disk_margin_bytes;
        return cfg;
    }

    /// GetDigest's latency per ledger (Requirement 11.1), timed around the
    /// whole API call: digest, signature and encoding.
    auto handle(const kythira::ledger::api_request& r) -> kythira::ledger::api_response {
        constexpr std::string_view prefix = "/v1/ledgers/";
        constexpr std::string_view suffix = "/digest";
        if (!prom_ || r.method != "GET" || !r.path.starts_with(prefix) ||
            !r.path.ends_with(suffix) || r.path.size() <= prefix.size() + suffix.size()) {
            return api_->handle(r);
        }
        const auto started = std::chrono::steady_clock::now();
        auto response = api_->handle(r);
        if (response.status == 200) {
            const auto name =
                r.path.substr(prefix.size(), r.path.size() - prefix.size() - suffix.size());
            prom_->observe_duration("kythira_ledger_digest_latency", {{"ledger", name}},
                                    std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - started)
                                        .count());
        }
        return response;
    }

    /// Samples every ledger this node hosts into the registry as gauges
    /// (Requirement 11.1): the state machine's counts since open, its
    /// journal bytes by where they live, and whether this node leads the
    /// ledger.
    auto sample_metrics() -> void {
        auto& reg = *prom_;
        using dims = std::vector<std::pair<std::string, std::string>>;
        const auto hs = host_->stats();
        reg.observe_gauge("kythira_ledger_node_groups_created", {}, double(hs.groups_created));
        reg.observe_gauge("kythira_ledger_node_groups_destroyed", {}, double(hs.groups_destroyed));
        reg.observe_gauge("kythira_ledger_node_index_steps", {}, double(hs.index_steps));
        reg.observe_gauge("kythira_ledger_node_seals", {}, double(hs.seals));
        reg.observe_gauge("kythira_ledger_node_maintenance_errors", {},
                          double(hs.maintenance_errors));
        reg.observe_gauge("kythira_ledger_node_split_merge_vetoes", {},
                          double(hs.split_merge_vetoes));
        std::vector<kythira::ledger::ledger_record> ledgers;
        try {
            ledgers = host_->catalog().list();
        } catch (const kythira::ledger::ledger_not_leader&) {
            return;  // the catalog group has not opened here yet
        }
        for (const auto& r : ledgers) {
            const auto sm = host_->ledger(r.ledger_id);
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
                const auto* n = raft_->group_node(host_->host_group(*r.group));
                leads = n != nullptr && n->is_leader();
            }
            reg.observe_gauge("kythira_ledger_leader", d, leads ? 1.0 : 0.0);
        }
    }

    auto sample_loop() -> void {
        std::unique_lock lock(sample_mutex_);
        while (!sample_cv_.wait_for(lock, opt_.metrics_interval, [this] { return !sampling_; })) {
            try {
                sample_metrics();
            } catch (const std::exception& e) {
                (*log_)(kythira::log_level::warning, "ledger_node: metrics sample failed",
                        {{"error", e.what()}});
            }
        }
    }

    auto stop_sampler() -> void {
        {
            const std::lock_guard lock(sample_mutex_);
            sampling_ = false;
        }
        sample_cv_.notify_all();
        if (sampler_.joinable()) {
            sampler_.join();
        }
    }

    bool standalone_;
    node_options opt_;
    std::optional<node_log> log_;
    std::optional<kythira::ledger::ledger_roles> roles_;
    std::shared_ptr<const kythira::ledger::digest_signer> signer_;
    std::string self_;
    std::shared_ptr<kythira::prometheus_registry> prom_;
    std::shared_ptr<typename group_sm::context> ctx_;
    std::shared_ptr<const export_store> exports_;
    std::unique_ptr<raft_host> raft_;
    std::unique_ptr<ledger_host> host_;
    std::unique_ptr<api_type> api_;
    std::unique_ptr<kythira::https::server> server_;
    std::unique_ptr<kythira::prometheus_scrape_server> scrape_;
    std::atomic<bool> driving_{false};
    std::thread driver_;
    std::mutex sample_mutex_;
    std::condition_variable sample_cv_;
    bool sampling_ = false;
    std::thread sampler_;
    std::atomic<bool> started_{false};
};

}  // namespace kythira::ledger_node
