// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Kinesis as a service module (.kiro/specs/service-colocation/ task 5): what
// kinesis_gateway_node's run() did between its Raft transport and its signal
// wait, behind service_module<Host>, so that kinesis_gateway_node (one
// module, its own CBOR Beast transport, ids and data directory exactly as
// before) and kythira_node (beside other modules, on the node's shared
// listener, in Kinesis's group space) run the same code.
//
// `Host` is a service_node::node_host: the module's multi_raft host takes
// the node's client and `server_for(space)`. Kinesis's groups are the
// initial tiling only (its shards split inside the stream state machine,
// never as Raft groups), so the groups are `space.to_host(1..n)`.
//
// It publishes `kinesis.put_records` (kinesis_in_process.hpp): the producer
// calls, for a co-located module such as DynamoDB's streaming destination.
//
// Data directory (KINESIS_DATA_DIR stand-alone, the node's data directory's
// kinesis/ in kythira_node):
//   raft/<group>/   each group's Raft log
//   segments/       the node's record segment store

#include "config.hpp"
#ifdef KYTHIRA_KINESIS_GATEWAY_IAM
#include "iam_auth.hpp"
#endif
#include "tier_s3.hpp"

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_compat_profile.hpp>
#include <kythira/kinesis/kinesis_disk_guard.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_gateway.hpp>
#include <kythira/kinesis/kinesis_in_process.hpp>
#include <kythira/kinesis/kinesis_iterators.hpp>
#include <kythira/kinesis/kinesis_kms.hpp>
#include <kythira/kinesis/kinesis_kms_aws.hpp>
#include <kythira/kinesis/kinesis_lease_api.hpp>
#include <kythira/kinesis/kinesis_maintenance.hpp>
#include <kythira/kinesis/kinesis_on_demand.hpp>
#include <kythira/kinesis/kinesis_peer_client.hpp>
#include <kythira/kinesis/kinesis_raft_cluster.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_static_keys.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_tiering.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/module_interfaces.hpp>
#include <raft/module_registry.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/node_core.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/persistence.hpp>
#include <raft/record_segment_store.hpp>
#include <raft/service_module.hpp>
#include <raft/static_key_authorizer.hpp>
#include <raft/static_or_iam_authorizer.hpp>
#include <raft/txn_state_machine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::kinesis_node {

/// The routing key a command names, so the host's cross-shard admission
/// check runs on every proposal and the transaction layer knows which keys
/// an entry touches.
struct stream_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> std::string {
        return kinesis::command_key(kinesis::decode_command(command));
    }
};

/// Kinesis's multi_raft types on a node whose transport is `Host`'s.
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
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type =
        kythira::transport_client_handle<typename Host::client_type, node_id_type>;
    using network_server_type = typename Host::view_type;

    using persistence_engine_type =
        kythira::binary_file_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    /// Wrapped for tier-1 transactions, which resharding commits through
    /// (task 11): the wrapper locks keys a transaction holds and passes
    /// every other entry to the stream state machine.
    using state_machine_type =
        kythira::transactional_state_machine<kinesis::stream_state_machine<log_index_type>,
                                             std::string, stream_partitioner>;

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

inline auto read_file(const std::string& path, std::string_view what) -> std::string {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("kinesis_gateway_node: cannot read " + std::string(what) + " " +
                                 path);
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

template<typename Host> class kinesis_module final : public service_node::service_module<Host> {
public:
    using types = module_host_types<Host>;
    using raft_host = kythira::multi_raft<types, std::string, std::uint64_t>;
    using raft_config = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using descriptor_type =
        kythira::shard_descriptor<std::uint64_t, std::string, kythira::host_node_id>;
    using cluster_type = kinesis::raft_stream_cluster<raft_host>;
    using authorizer_type = aws_auth::static_or_iam_authorizer;
    using front_end_type = kinesis::front_end<authorizer_type>;
    using gateway_type = kinesis::gateway<front_end_type>;
    using producer_type = kinesis::in_process_producer<front_end_type>;

    /// @param standalone kinesis_gateway_node's module: its own variables
    ///        for the node-wide settings and the data directory itself.
    ///        Otherwise kythira_node's: the node's settings fill in what the
    ///        module's variables leave unset (composite_env), and the data
    ///        directory is the node's kinesis/.
    explicit kinesis_module(bool standalone) : _standalone(standalone) {}

    kinesis_module(const kinesis_module&) = delete;
    auto operator=(const kinesis_module&) -> kinesis_module& = delete;
    kinesis_module(kinesis_module&&) = delete;
    auto operator=(kinesis_module&&) -> kinesis_module& = delete;
    ~kinesis_module() override { stop(); }

    [[nodiscard]] auto name() const -> std::string_view override { return "kinesis"; }
    [[nodiscard]] auto number() const -> std::uint8_t override {
        return service_node::module_numbers::kinesis;
    }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KINESIS_"; }
    /// IAM's evaluator, when co-located, answers before this module
    /// serves.
    [[nodiscard]] auto starts_after() const -> std::vector<std::string> override { return {"iam"}; }
    [[nodiscard]] auto data_subdir() const -> std::string override {
        return _standalone ? std::string() : std::string("kinesis");
    }

    /// Everything that can be wrong with the configuration fails here,
    /// before a port is bound: the variables, the key file, a key IAM also
    /// has, the compatibility profile.
    [[nodiscard]] auto configure(const service_node::env_lookup& env)
        -> std::optional<std::string> override {
        try {
            _opt = from_env(
                _standalone ? env : composite_env(service_node::module_tuning(env, "KINESIS_")));
            _logger.emplace(_opt._log_level);
            // IAM (KINESIS_AUTH=iam or both): the co-located IAM module's
            // evaluator when kythira_node runs one, taken at start;
            // otherwise the embedded replica, which outlives the gateway.
            std::optional<aws_auth::iam_hooks> hooks;
            _iam_in_process = !_standalone && _opt._auth != aws_auth::auth_mode::static_keys &&
                              service_node::co_located(env, "iam");
            if (_opt._auth != aws_auth::auth_mode::static_keys && !_iam_in_process) {
#ifdef KYTHIRA_KINESIS_GATEWAY_IAM
                _iam = start_iam_auth_from_env();
                hooks = _iam->hooks();
#else
                return std::string(
                    "KINESIS_AUTH=iam or both needs a build with CONFIG_KINESIS_GATEWAY_IAM");
#endif
            }
            _authorizer = std::make_unique<authorizer_type>(
                _opt._static_keys_file.empty()
                    ? std::vector<aws_auth::static_key>{}
                    : kinesis::parse_static_keys(
                          read_file(_opt._static_keys_file, "KINESIS_STATIC_KEYS_FILE")),
                _opt._partition);
            _authorizer->register_service(kinesis::kinesis_static_roles());
            // The Lease API authorizes on the DynamoDB actions KCL needs on
            // AWS.
            _authorizer->register_service(kinesis::lease_static_roles());
            if (!_iam_in_process) {
                _authorizer->set_mode(_opt._auth, std::move(hooks));
                if (auto clash = key_clash()) {
                    return clash;
                }
            }
            auto profile = kinesis::cluster_profile(_opt._compat);
            if (!profile) {
                return std::string("KINESIS_COMPAT refused");
            }
            _profile = *profile;
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::nullopt;
    }

    void register_groups(Host& host, const service_node::group_space& space) override {
        std::filesystem::create_directories(_opt._data_dir / "raft");
        std::filesystem::create_directories(_opt._data_dir / "segments");
        kythira::record_segment_store_options so;
        so._data_dir = _opt._data_dir / "segments";
        _store = std::make_unique<kythira::record_segment_store>(so);

        _peers = std::make_unique<kinesis::peer_client>(kinesis::peer_client_config{
            ._tls = !_opt._allow_plaintext,
            ._ca_path = _opt._peer_ca_path,
            ._timeout = _opt._command_timeout,
        });
        for (const auto& [id, gw] : _opt._peer_gateways) {
            if (id != _opt._node_id) {
                _other_gateways.push_back(gw);
            }
        }
        _transport = kinesis::make_segment_transport(*_peers, _opt._forward_secret);
        // Tiering (task 10): sealed segments past KINESIS_TIER_AFTER_HOURS go
        // to S3, and a read that needs one fetches it back.
#ifdef KYTHIRA_KINESIS_HAS_S3_TIER
        if (!_opt._tier_bucket.empty()) {
            _tier_owner = make_s3_tier(_opt);
        }
#endif
        _tier = _tier_owner ? _tier_owner->store() : kinesis::tier_store{};
        // Restore cannot finish without the active segment a snapshot names:
        // try every peer, then the tier, and fail the restore (Raft retries
        // it) when none has it.
        _fetch = [this](const kythira::record_segment_descriptor& d) {
            if (_store->fetch_segment(d, _other_gateways, _transport)) {
                return;
            }
            if (d._sealed && kinesis::fetch_from_tier(*_store, _tier, d)) {
                _fetched.note(d._stream, d._first_seq, wall_ms());
                return;
            }
            throw kythira::record_segment_error("kinesis: no peer or tier served segment " +
                                                d._stream + " at " + std::to_string(d._first_seq));
        };

        for (const auto& [id, url] : _opt._peers) {
            _voters.push_back(id);
        }
        _raft = std::make_unique<raft_host>(make_raft_config(host, space));

        // Initial tiling: cuts sorted and deduplicated, one group per range.
        auto cuts = _opt._shard_cuts;
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        std::optional<std::string> start;
        std::uint64_t group = 1;
        for (std::size_t i = 0; i <= cuts.size(); ++i) {
            std::optional<std::string> end =
                i < cuts.size() ? std::optional<std::string>{cuts[i]} : std::nullopt;
            _raft->create_group(descriptor_type{
                ._group_id = space.to_host(group++),
                ._range = kythira::shard_range<std::string>{._start = start, ._end = end},
                ._epoch = kythira::shard_epoch{},
                ._voters = _voters});
            start = end;
        }
        _groups = cuts.size() + 1;

        build_service();
    }

    void publish(service_node::module_registry& registry) override {
        registry.publish<service_node::aws_json_endpoint>(
            std::string(service_node::interface_names::kinesis_put_records), _producer);
    }

    void start(service_node::module_registry& registry) override {
        if (_iam_in_process) {
            // IAM starts first (starts_after), so its evaluator answers
            // before this module serves a request.
            auto iam = registry.lookup<service_node::iam_evaluator>(
                std::string(service_node::interface_names::iam_evaluator));
            if (!iam) {
                throw std::invalid_argument(
                    "KINESIS_AUTH=iam or both: the IAM module publishes no evaluator");
            }
            iam->add_actions(kinesis::kinesis_actions());
            // The Lease API's DynamoDB actions, as on AWS's lease table.
            iam->add_actions(kinesis::lease_actions());
            _authorizer->set_mode(_opt._auth, iam->hooks());
            if (auto clash = key_clash()) {
                throw std::invalid_argument(*clash);
            }
            _iam_evaluator = std::move(iam);
        }
        _raft->start();

        https::server_config hc;
        hc.bind_address = _opt._listen_host;
        hc.port = _opt._listen_port;
        hc.tls_cert_path = _opt._tls_cert_path;
        hc.tls_key_path = _opt._tls_key_path;
        hc.insecure_plaintext = _opt._allow_plaintext;
        hc.max_body_bytes = _opt._max_body_bytes;
        hc.max_connections = _opt._max_connections;
        // AWS SDKs subscribe to shards over HTTP/2 (task 13).
        hc.http2 = true;
        _server = std::make_unique<https::server>(
            hc,
            [this](const https::request& req, const https::request_context& ctx) {
                return _gateway->handle(req, ctx);
            },
            [this](bool warning, std::string_view message) {
                _logger->log(warning ? kythira::log_level::warning : kythira::log_level::info,
                             "kinesis gateway: " + std::string(message), {});
            });
        // SubscribeToShard answers with an event stream (task 13).
        _server->set_stream_handler(
            [this](const https::request& req, const https::request_context& ctx) {
                return _gateway->stream(req, ctx);
            });
        _server->start();

        _driving = true;
        _driver = std::thread([this] {
            while (_driving.load(std::memory_order_acquire)) {
                (void)_raft->tick();
                std::this_thread::sleep_for(_opt._tick_interval);
            }
        });

        // Retention and tiering: each node runs the pass; only a shard's
        // leader seals, trims and tiers it.
        _maintenance = std::make_unique<kinesis::stream_maintenance<cluster_type>>(
            *_cluster, *_store, _tier, _fetched,
            kinesis::maintenance_config{._tier_after_ms = _opt._tier_after_ms,
                                        ._keep_fetched_ms = std::int64_t{10} * 60 * 1000});
        // On-demand streams (task 12): each node splits the hot shards it
        // leads.
        _on_demand =
            std::make_unique<kinesis::on_demand_controller<cluster_type>>(*_cluster, *_service);
        _maintaining = true;
        _maintainer = std::thread([this] { maintain(); });
        _started = true;

        std::cout << "kinesis_gateway_node " << kythira::host_node_id_text(_opt._node_id)
                  << " ready: raft=" << _opt._raft_port << " kinesis=" << _server->port()
                  << (_opt._allow_plaintext ? " (http)" : " (https)") << " groups=" << _groups
                  << " voters=" << _voters.size() << std::endl;
    }

    /// No request in flight, in-process or over the listener, once the host
    /// stops.
    void drain() override {
        if (_producer) {
            _producer->close();
        }
        _maintaining.store(false, std::memory_order_release);
        if (_maintainer.joinable()) {
            _maintainer.join();
        }
        if (_server) {
            _server->stop();
        }
    }

    /// No tick once the host is gone.
    void stop() override {
        drain();
        _driving.store(false, std::memory_order_release);
        if (_driver.joinable()) {
            _driver.join();
        }
        if (_raft) {
            _raft->stop();
        }
        _started = false;
        // Everything that refers to the host goes before it.
        _on_demand.reset();
        _maintenance.reset();
        _server.reset();
        _leases.reset();
        _gateway.reset();
        _service.reset();
        _front_end.reset();
        _cluster.reset();
        _raft.reset();
    }

    [[nodiscard]] auto health() const -> service_node::module_health override {
        if (!_started) {
            return {false, "not started"};
        }
        return {true, {}};
    }

    [[nodiscard]] auto metrics() const -> std::vector<service_node::module_metric> override {
        if (!_started) {
            return {};
        }
        return {{"groups", static_cast<double>(_groups), {}},
                {"voters", static_cast<double>(_voters.size()), {}},
                {"raft_term_sum", service_node::raft_term_sum(*_raft), {}}};
    }

    /// The API's port once started (tests pass port 0).
    [[nodiscard]] auto api_port() const -> std::uint16_t { return _server ? _server->port() : 0; }

private:
    /// KINESIS_AUTH=both: an access key id may be in the key file or in
    /// IAM, not both.
    [[nodiscard]] auto key_clash() const -> std::optional<std::string> {
        if (const auto clash = _authorizer->conflicting_keys(); !clash.empty()) {
            return "KINESIS_AUTH=both: IAM also has the static key " + clash.front() +
                   "; an access key id may be in one or the other";
        }
        return std::nullopt;
    }

    [[nodiscard]] static auto wall_ms() -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    auto make_raft_config(Host& host, const service_node::group_space& space) -> raft_config {
        const auto raft_dir = _opt._data_dir / "raft";
        raft_config cfg{
            .node_id = _opt._node_id,
            .network_client = typename types::network_client_type{host.client()},
            .network_server = host.server_for(space),
            .store_factory =
                [raft_dir](const std::uint64_t& group) {
                    return
                        typename types::persistence_engine_type{raft_dir / std::to_string(group)};
                },
        };
        cfg.state_machine_factory = [this](const std::uint64_t&) {
            return typename types::state_machine_type(
                kinesis::stream_state_machine<std::uint64_t>(_store.get(), _fetch));
        };
        cfg.logger = kythira::console_logger{_opt._log_level};
        cfg.logger_factory = [level = _opt._log_level](const std::uint64_t&) {
            return kythira::console_logger{level};
        };
        cfg.config._election_timeout_min = _opt._election_timeout_min;
        cfg.config._election_timeout_max = _opt._election_timeout_max;
        cfg.config._heartbeat_interval = _opt._heartbeat_interval;
        cfg.config._rpc_timeout = _opt._rpc_timeout;
        cfg.config._max_append_bytes = _opt._max_body_bytes;
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.executor_stripes = _opt._executor_stripes;
        cfg.policy_interval = _opt._policy_interval;
        cfg.partitioner = kythira::make_partitioner<std::string>(stream_partitioner{});
        return cfg;
    }

    /// The cluster adapter, the front end, the service, the gateway and
    /// the Lease API: everything a request reaches, built before any
    /// listener opens.
    void build_service() {
        const auto gateways = _opt._peer_gateways;
        const auto address_of =
            [gateways](const kythira::host_node_id& id) -> std::optional<std::string> {
            const auto it = gateways.find(id);
            return it == gateways.end() ? std::nullopt : std::optional{it->second};
        };
        _guard = std::make_unique<kinesis::disk_guard>(_opt._data_dir / "segments",
                                                       _opt._disk_floor_bytes);
        _cluster =
            std::make_unique<cluster_type>(*_raft,
                                           kinesis::make_http_forwarder<kythira::host_node_id>(
                                               *_peers, _opt._forward_secret, address_of),
                                           _opt._command_timeout, &_write_limits, _guard.get());
        // Transaction ops (resharding, task 11) go to the leading gateway,
        // and the host's lock resolver asks other groups' leaders the same
        // way.
        _cluster->set_txn_forwarder(kinesis::make_http_txn_forwarder<kythira::host_node_id>(
            *_peers, _opt._forward_secret, address_of));
        _raft->set_txn_forwarder([this](const std::vector<std::string>& keys,
                                        const std::vector<std::byte>& envelope,
                                        std::chrono::milliseconds timeout) {
            return _cluster->txn_send(keys, envelope, timeout);
        });

        kinesis::front_end_config fc;
        fc._region = _opt._region;
        fc._partition = _opt._partition;
        _front_end = std::make_unique<front_end_type>(fc, *_authorizer);
        kinesis::kinesis_service_config sc;
        sc._cluster_profile = _profile;
        sc._limits = kinesis::kinesis_limits::for_region(_opt._region);
        // Every gateway must sign iterators with the same key, or a client's
        // next GetRecords on another node fails; a lone node without a
        // forwarding secret gets the service's random key.
        if (!_opt._iterator_keys_file.empty()) {
            sc._iterator_keys = kinesis::parse_iterator_keys(
                read_file(_opt._iterator_keys_file, "KINESIS_ITERATOR_KEYS_FILE"));
        } else if (!_opt._forward_secret.empty()) {
            sc._iterator_keys = kinesis::derived_iterator_key(_opt._forward_secret);
        }
        // Encryption at rest (task 14).
        if (_opt._kms == "local") {
            sc._kms = kinesis::key_provider(kinesis::local_kms_provider(_opt._kms_passphrase));
        } else if (_opt._kms == "aws") {
            kinesis::aws_kms_config kc;
            kc._region = _opt._kms_region;
            kc._endpoint = _opt._kms_endpoint;
            // Read once at startup: nothing in the process changes its
            // environment.
            const auto env = [](const char* name) {
                const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
                return v != nullptr ? std::string(v) : std::string();
            };
            kc._credentials = [creds = kinesis::aws_kms_credentials{
                                   env("AWS_ACCESS_KEY_ID"), env("AWS_SECRET_ACCESS_KEY"),
                                   env("AWS_SESSION_TOKEN")}] { return creds; };
            sc._kms = kinesis::key_provider(kinesis::aws_kms_provider(std::move(kc)));
        }
        _service = std::make_unique<kinesis::kinesis_service<cluster_type>>(*_cluster, sc);
        _service->register_with(*_front_end);
        // Authorization sees a stream's tags and resource policy (task 15).
        _front_end->set_resource_lookup(
            [this](std::string_view arn) { return _service->resource_facts_of(arn); });
        _gateway = std::make_unique<gateway_type>(
            *_front_end,
            [this](const std::string& key, const std::vector<std::byte>& command,
                   kinesis::admission mode) { return _cluster->propose_local(key, command, mode); },
            _store.get(), _opt._forward_secret);
        _leases = std::make_unique<kinesis::lease_api<cluster_type, authorizer_type>>(
            *_cluster, *_authorizer, fc);
        _gateway->set_lease_handler(
            [this](const kinesis::front_end_request& r) { return _leases->handle(r); });
        if (_opt._metrics) {
            _gateway->set_metrics_handler([this] { return _service->metrics_text(); });
        }
        _gateway->set_txn_handler(
            [this](const std::vector<std::string>& keys, const std::vector<std::byte>& envelope) {
                return _cluster->txn_submit_local(keys, envelope);
            });
        _producer = std::make_shared<producer_type>(*_front_end);
    }

    void maintain() {
        auto next = std::chrono::steady_clock::now() + _opt._maintenance_interval;
        while (_maintaining.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() < next) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                continue;
            }
            next = std::chrono::steady_clock::now() + _opt._maintenance_interval;
            const auto st = _maintenance->run_once();
            const auto od = _on_demand->run_once(_cluster->now_ms());
            if (od._splits + od._failures > 0) {
                log_info("kinesis on-demand: hot=" + std::to_string(od._hot) + " splits=" +
                         std::to_string(od._splits) + " failures=" + std::to_string(od._failures));
            }
            // A create whose caller lost it between the catalog commit and
            // ACTIVE (a leader change mid-create) is finished here after
            // half a minute in CREATING.
            const auto created = _service->repair_creates(_cluster->now_ms(), 30000);
            if (created > 0) {
                log_info("kinesis maintenance: finished " + std::to_string(created) +
                         " interrupted create(s)");
            }
            // A reshard whose caller died after its transaction committed is
            // finished here once it has sat UPDATING for a minute.
            const auto repaired = _service->repair_reshards(_cluster->now_ms(), 60000);
            if (repaired > 0) {
                log_info("kinesis maintenance: completed " + std::to_string(repaired) +
                         " abandoned reshard(s)");
            }
            if (st._seals + st._trims + st._tiered + st._objects_deleted + st._evicted +
                    st._failures >
                0) {
                log_info("kinesis maintenance: seals=" + std::to_string(st._seals) + " trims=" +
                         std::to_string(st._trims) + " tiered=" + std::to_string(st._tiered) +
                         " objects_deleted=" + std::to_string(st._objects_deleted) + " evicted=" +
                         std::to_string(st._evicted) + " failures=" + std::to_string(st._failures));
            }
        }
    }

    void log_info(const std::string& message) {
        _logger->log(kythira::log_level::info, message, {});
    }

    bool _standalone;
    node_options _opt;
    std::optional<kythira::console_logger> _logger;
#ifdef KYTHIRA_KINESIS_GATEWAY_IAM
    std::unique_ptr<iam_auth> _iam;
#endif
    /// KINESIS_AUTH=iam|both in kythira_node beside the IAM module.
    bool _iam_in_process{false};
    std::shared_ptr<service_node::iam_evaluator> _iam_evaluator;
    std::unique_ptr<authorizer_type> _authorizer;
    kinesis::kinesis_compat_profile _profile{};

    std::unique_ptr<kythira::record_segment_store> _store;
    std::unique_ptr<kinesis::peer_client> _peers;
    std::vector<std::string> _other_gateways;
    kythira::record_segment_store::fetch_transport _transport;
    std::unique_ptr<s3_tier> _tier_owner;
    kinesis::tier_store _tier;
    kinesis::fetched_segments _fetched;
    kinesis::segment_fetcher _fetch;
    std::vector<kythira::host_node_id> _voters;
    std::size_t _groups = 0;

    std::unique_ptr<raft_host> _raft;
    /// The leader's write buckets (task 8): what a stream with the
    /// throttling switch on meets when this node proposes for it.
    kinesis::write_throttle _write_limits;
    /// The disk guard (Requirement 11.4) on the segment disk.
    std::unique_ptr<kinesis::disk_guard> _guard;
    std::unique_ptr<cluster_type> _cluster;
    std::unique_ptr<front_end_type> _front_end;
    std::unique_ptr<kinesis::kinesis_service<cluster_type>> _service;
    std::unique_ptr<gateway_type> _gateway;
    std::unique_ptr<kinesis::lease_api<cluster_type, authorizer_type>> _leases;
    std::shared_ptr<producer_type> _producer;
    std::unique_ptr<https::server> _server;

    std::atomic<bool> _driving{false};
    std::thread _driver;
    std::unique_ptr<kinesis::stream_maintenance<cluster_type>> _maintenance;
    std::unique_ptr<kinesis::on_demand_controller<cluster_type>> _on_demand;
    std::atomic<bool> _maintaining{false};
    std::thread _maintainer;
    std::atomic<bool> _started{false};
};

}  // namespace kythira::kinesis_node
