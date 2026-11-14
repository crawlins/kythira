// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file iam_module.hpp
/// @brief IAM as a service module of kythira_node
///        (.kiro/specs/service-colocation/ task 8, Requirements 5.1-5.3
///        and 7.2).
///
/// The IAM Raft group is one group on the node's shared multi-raft host,
/// in IAM's group space, over the node's one Raft listener. Its serving
/// side is iam_node's (iam_service.hpp): IAM, STS and the admin routes on
/// IAM's own HTTPS listener, with the same KEK check and leader redirects.
///
/// It publishes `iam.evaluator` (module_interfaces.hpp): IAM's authorizer
/// over this member's state, so a co-located Kinesis, DynamoDB or Redis
/// authenticates and authorizes in-process instead of embedding a replica
/// of IAM's state. Each adds its action tables at its start, before it
/// serves; IAM starts before them (their `starts_after`).
///
/// Not yet: read replicas. The embedded replicas of stand-alone gateways
/// join iam_node's TCP-RPC group, and this member takes no learners
/// (`/v1/admin/replicas` answers InvalidInput). iam_node itself still runs
/// its own TCP-RPC group, with its data directory as before.
///
/// Settings (`KYTHIRA_IAM_*`; the node's `KYTHIRA_NODE_*` give the id,
/// the members, TLS and the data directory):
///   KYTHIRA_IAM_UNSEAL_KEY_FILE     the cluster's unseal passphrase (required)
///   KYTHIRA_IAM_HTTP_BIND / _PORT   IAM, STS and admin listener [0.0.0.0:8443]
///   KYTHIRA_IAM_TLS_CERT / _KEY     its certificate        [the node's]
///   KYTHIRA_IAM_CLIENT_CA           admin client CA        [the node's]
///   KYTHIRA_IAM_ADMIN_URI_SANS      admin certificate URI SANs, comma-separated
///   KYTHIRA_IAM_PEER_URLS           id=https://host:port,... for leader
///                                   redirects [each member's Raft host, this
///                                   node's IAM port]
///   KYTHIRA_IAM_ARN_PARTITION       [aws]
///   KYTHIRA_IAM_STS_REGIONS         comma-separated        [us-east-1]
///   KYTHIRA_IAM_OIDC_ALLOWED_HOSTS / _OIDC_PROXY / _OIDC_CA_FILE
///   KYTHIRA_IAM_KYTHIRA_ISSUER / _KYTHIRA_ISSUER_LOCAL
///   KYTHIRA_IAM_CREDENTIALS_EXCHANGE  0 or 1                [0]
///
/// Data directory: the node's iam/, with each group's Raft log in raft/.

#include "config.hpp"
#include "iam_service.hpp"

#include <kythira/iam/iam_redis_verifier.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/services/elasticache_actions.hpp>

#include <raft/binary_file_persistence.hpp>
#include <raft/console_logger.hpp>
#include <raft/credential_verifier.hpp>
#include <raft/future_default.hpp>
#include <raft/host_node_id.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/module_interfaces.hpp>
#include <raft/module_registry.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/multi_raft_transport_handles.hpp>
#include <raft/node_core.hpp>
#include <raft/service_module.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace iam_node {

namespace sn = kythira::service_node;

/// IAM's multi_raft types on a node whose transport is `Host`'s.
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
    using state_machine_type = kythira::iam::iam_state_machine;

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

namespace module_detail {

[[nodiscard]] inline auto split_list(const std::string& text) -> std::vector<std::string> {
    std::vector<std::string> out;
    std::string tok;
    for (const char c : text + ',') {
        if (c == ',') {
            if (!tok.empty()) {
                out.push_back(tok);
            }
            tok.clear();
        } else if (c != ' ') {
            tok += c;
        }
    }
    return out;
}

[[nodiscard]] inline auto port_of(const std::string& name, const std::string& text)
    -> std::uint16_t {
    const auto p = sn::node_detail::parse_port(text);
    if (!p) {
        throw std::invalid_argument(name + ": not a port: '" + text + "'");
    }
    return *p;
}

}  // namespace module_detail

/// The member's settings from the node's and IAM's variables.
/// @throws std::invalid_argument naming the variable that is wrong.
[[nodiscard]] inline auto config_from_env(const sn::env_lookup& env) -> config {
    namespace md = module_detail;
    auto parsed = sn::node_config_from_env(env);
    if (auto* error = std::get_if<std::string>(&parsed)) {
        throw std::invalid_argument(*error);
    }
    const auto& node = std::get<sn::node_config>(parsed);
    const auto get = [&env](const char* name) { return env(name).value_or(""); };

    config cfg;
    cfg.log_name = "kythira_node iam";
    auto id = kythira::parse_host_node_id(node.node_id);
    if (!id) {
        throw std::invalid_argument("KYTHIRA_NODE_ID: " + id.error());
    }
    cfg.node_id = std::move(id).value();
    cfg.data_dir = (node.data_dir / "iam").string();
    cfg.insecure_plaintext = node.insecure_plaintext;
    cfg.max_replicas = 0;

    cfg.unseal_key_file = get("KYTHIRA_IAM_UNSEAL_KEY_FILE");
    if (cfg.unseal_key_file.empty()) {
        throw std::invalid_argument("KYTHIRA_IAM_UNSEAL_KEY_FILE is required");
    }
    cfg.http_address = env("KYTHIRA_IAM_HTTP_BIND").value_or(cfg.http_address);
    if (auto v = env("KYTHIRA_IAM_HTTP_PORT")) {
        cfg.http_port = md::port_of("KYTHIRA_IAM_HTTP_PORT", *v);
    }
    if (!node.insecure_plaintext) {
        cfg.tls_cert_path = env("KYTHIRA_IAM_TLS_CERT").value_or(node.tls_cert);
        cfg.tls_key_path = env("KYTHIRA_IAM_TLS_KEY").value_or(node.tls_key);
        cfg.client_ca_path = env("KYTHIRA_IAM_CLIENT_CA").value_or(node.tls_ca);
        // The node's Raft CA: what the Kythira issuer's keys are fetched
        // under when no OIDC CA is given, as iam_node's --rpc-tls-ca.
        cfg.rpc_tls_ca_path = node.tls_ca;
    }
    for (const auto& san : md::split_list(get("KYTHIRA_IAM_ADMIN_URI_SANS"))) {
        if (san.find("://") == std::string::npos) {
            throw std::invalid_argument("KYTHIRA_IAM_ADMIN_URI_SANS: not an absolute URI: " + san);
        }
        cfg.admin_uri_sans.insert(san);
    }
    if (!cfg.admin_uri_sans.empty() && node.insecure_plaintext) {
        throw std::invalid_argument(
            "KYTHIRA_IAM_ADMIN_URI_SANS needs TLS: admin clients authenticate with a certificate");
    }

    std::map<std::string, std::string> urls;
    for (const auto& item : md::split_list(get("KYTHIRA_IAM_PEER_URLS"))) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw std::invalid_argument("KYTHIRA_IAM_PEER_URLS: expected id=url, got '" + item +
                                        "'");
        }
        urls[item.substr(0, eq)] = item.substr(eq + 1);
    }
    const std::string scheme = node.insecure_plaintext ? "http://" : "https://";
    for (const auto& p : node.peers) {
        if (p.id == node.node_id) {
            continue;
        }
        auto pid = kythira::parse_host_node_id(p.id);
        if (!pid) {
            throw std::invalid_argument("KYTHIRA_NODE_PEERS: " + pid.error());
        }
        peer_info info;
        info.node_id = std::move(pid).value();
        info.rpc_host = p.host;
        info.rpc_port = p.port;
        const auto url = urls.find(p.id);
        info.http_address =
            url != urls.end() ? url->second : scheme + p.host + ":" + std::to_string(cfg.http_port);
        cfg.peers.push_back(std::move(info));
    }

    if (auto v = env("KYTHIRA_IAM_ARN_PARTITION")) {
        cfg.arn_partition = *v;
    }
    if (auto v = env("KYTHIRA_IAM_STS_REGIONS")) {
        cfg.sts_regions = md::split_list(*v);
        if (cfg.sts_regions.empty()) {
            throw std::invalid_argument("KYTHIRA_IAM_STS_REGIONS needs at least one region");
        }
    }
    cfg.oidc_allowed_hosts = md::split_list(get("KYTHIRA_IAM_OIDC_ALLOWED_HOSTS"));
    cfg.oidc_proxy = get("KYTHIRA_IAM_OIDC_PROXY");
    cfg.oidc_ca_file = get("KYTHIRA_IAM_OIDC_CA_FILE");
    cfg.kythira_issuer = get("KYTHIRA_IAM_KYTHIRA_ISSUER");
    cfg.kythira_issuer_local = get("KYTHIRA_IAM_KYTHIRA_ISSUER_LOCAL");
    for (const auto* url : {&cfg.kythira_issuer, &cfg.kythira_issuer_local}) {
        if (!url->empty() && !url->starts_with("https://")) {
            throw std::invalid_argument(
                "KYTHIRA_IAM_KYTHIRA_ISSUER and _KYTHIRA_ISSUER_LOCAL "
                "must be https URLs");
        }
    }
    if (auto v = env("KYTHIRA_IAM_CREDENTIALS_EXCHANGE")) {
        if (*v != "0" && *v != "1") {
            throw std::invalid_argument("KYTHIRA_IAM_CREDENTIALS_EXCHANGE: expected 0 or 1");
        }
        cfg.credentials_exchange = *v == "1";
    }
    if ((!cfg.kythira_issuer_local.empty() || cfg.credentials_exchange) &&
        cfg.kythira_issuer.empty()) {
        throw std::invalid_argument(
            "KYTHIRA_IAM_KYTHIRA_ISSUER_LOCAL and _CREDENTIALS_EXCHANGE need "
            "KYTHIRA_IAM_KYTHIRA_ISSUER");
    }
    if (cfg.credentials_exchange && node.insecure_plaintext) {
        throw std::invalid_argument(
            "KYTHIRA_IAM_CREDENTIALS_EXCHANGE needs TLS: the client certificate is the credential");
    }
    return cfg;
}

template<typename Host> class iam_module final : public sn::service_module<Host> {
public:
    using types = module_host_types<Host>;
    using raft_host = kythira::multi_raft<types, std::string, std::uint64_t>;
    using raft_config = kythira::multi_raft_config<types, std::string, std::uint64_t>;
    using group_node_type = typename raft_host::group_node_type;
    using service_type = iam_service<group_node_type>;

    iam_module() = default;
    iam_module(const iam_module&) = delete;
    auto operator=(const iam_module&) -> iam_module& = delete;
    iam_module(iam_module&&) = delete;
    auto operator=(iam_module&&) -> iam_module& = delete;
    ~iam_module() override { stop(); }

    [[nodiscard]] auto name() const -> std::string_view override { return "iam"; }
    [[nodiscard]] auto number() const -> std::uint8_t override { return sn::module_numbers::iam; }
    [[nodiscard]] auto env_prefix() const -> std::string_view override { return "KYTHIRA_IAM_"; }
    [[nodiscard]] auto data_subdir() const -> std::string override { return "iam"; }

    /// The settings and the unseal passphrase, before any port is bound.
    [[nodiscard]] auto configure(const sn::env_lookup& env) -> std::optional<std::string> override {
        try {
            _cfg = config_from_env(env);
            _passphrase = read_unseal_passphrase(_cfg.unseal_key_file);
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::nullopt;
    }

    /// IAM's one group, voted by every member, at the space's first id.
    void register_groups(Host& host, const sn::group_space& space) override {
        const auto raft_dir = std::filesystem::path(_cfg.data_dir) / "raft";
        std::filesystem::create_directories(raft_dir);
        raft_config rc{
            .node_id = _cfg.node_id,
            .network_client = typename types::network_client_type{host.client()},
            .network_server = host.server_for(space),
            .store_factory =
                [raft_dir](const std::uint64_t& group) {
                    return
                        typename types::persistence_engine_type{raft_dir / std::to_string(group)};
                },
        };
        rc.state_machine_factory = [partition = _cfg.arn_partition](const std::uint64_t&) {
            return kythira::iam::iam_state_machine(
                kythira::iam::iam_config{.partition = partition});
        };
        rc.automatic_split_merge_enabled = false;  // one group, never moved
        rc.hibernation = kythira::hibernation_mode::off;
        rc.logger = kythira::console_logger{kythira::log_level::info};
        rc.logger_factory = [](const std::uint64_t&) {
            return kythira::console_logger{kythira::log_level::info};
        };
        rc.config._election_timeout_min = _cfg.election_timeout_min;
        rc.config._election_timeout_max = _cfg.election_timeout_max;
        rc.config._heartbeat_interval = _cfg.heartbeat_interval;
        rc.config._rpc_timeout = _cfg.rpc_timeout;
        _raft = std::make_unique<raft_host>(std::move(rc));

        _group = space.to_host(1);
        std::vector<kythira::host_node_id> voters = _cfg.voter_ids();
        if (!_raft->has_group(_group)) {
            _raft->create_group(
                kythira::shard_descriptor<std::uint64_t, std::string, kythira::host_node_id>{
                    ._group_id = _group,
                    ._range = kythira::shard_range<std::string>{._start = std::nullopt,
                                                                ._end = std::nullopt},
                    ._epoch = kythira::shard_epoch{},
                    ._voters = std::move(voters)});
        }
        auto* node = _raft->group_node(_group);
        if (node == nullptr) {
            throw std::runtime_error("iam: the IAM group did not open on this node");
        }
        _service = std::make_unique<service_type>(_cfg, _passphrase, *node, replica_hooks{},
                                                  "group=" + std::to_string(_group));
        _evaluator = std::make_shared<evaluator>(*this);
    }

    void publish(sn::module_registry& registry) override {
        registry.publish<sn::iam_evaluator>(std::string(sn::interface_names::iam_evaluator),
                                            _evaluator);
    }

    void start(sn::module_registry& /*registry*/) override {
        _raft->start();
        _raft_running = true;
        _driving = true;
        _driver = std::thread([this] {
            while (_driving.load(std::memory_order_acquire)) {
                try {
                    (void)_raft->tick();
                } catch (const std::exception& e) {
                    std::cerr << "[error] " << _cfg.log_name << ": tick failed: " << e.what()
                              << "\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        _service->start();
        _started = true;
    }

    void stop() override {
        _started = false;
        _driving.store(false, std::memory_order_release);
        if (_driver.joinable()) {
            _driver.join();
        }
        // The group first: stopping it rejects every pending proposal and
        // read a handler or the maintenance loop may be blocked on.
        if (_raft && _raft_running) {
            _raft->stop();
            _raft_running = false;
        }
        if (_service) {
            _service->stop();
        }
    }

    [[nodiscard]] auto health() const -> sn::module_health override {
        if (!_started) {
            return {false, "not started"};
        }
        return {true, {}};
    }

    [[nodiscard]] auto metrics() const -> std::vector<sn::module_metric> override {
        if (!_raft || !_service) {
            return {};
        }
        const auto* node = _raft->group_node(_group);
        return {{"leader", node != nullptr && node->is_leader() ? 1.0 : 0.0, {}},
                {"unsealed", _service->unsealed() ? 1.0 : 0.0, {}}};
    }

    /// IAM's listener's port once started (tests pass port 0).
    [[nodiscard]] auto http_port() const -> std::uint16_t {
        return _service ? _service->http_port() : 0;
    }

private:
    /// `iam.evaluator` over this member: the authorizer reads this
    /// member's applied state, as an embedded replica's reads its own.
    class evaluator final : public sn::iam_evaluator {
    public:
        explicit evaluator(iam_module& m) : _m(&m) {}

        void add_actions(const kythira::aws_auth::aws_action_table& table) override {
            _m->_service->registry().merge(table);
        }

        [[nodiscard]] auto hooks() -> kythira::aws_auth::iam_hooks override {
            kythira::aws_auth::iam_hooks h;
            auto* service = _m->_service.get();
            auto* node = _m->_raft->group_node(_m->_group);
            h.authenticate = [service](const kythira::sigv4::request& req,
                                       const kythira::sigv4::service_rules& rules) {
                return service->authorizer().authenticate(req, rules);
            };
            h.authorize = [service](const kythira::aws_auth::principal_context& who,
                                    const kythira::aws_auth::authz_request& r) {
                return service->authorizer().authorize(who, r);
            };
            h.has_key = [node](std::string_view id) {
                return node->with_state_machine([&](kythira::iam::iam_state_machine& sm) {
                    return sm.access_key(id).second != nullptr;
                });
            };
            return h;
        }

        [[nodiscard]] auto credential_verifier(const sn::elasticache_scope& scope)
            -> kythira::credential_verifier_fn override {
            _m->_service->registry().merge(kythira::iam::services::elasticache_actions());
            kythira::iam::redis_verifier_config v;
            v.account = scope.account;
            v.region = scope.region;
            v.cache_name = scope.cache_name;
            v.partition = _m->_cfg.arn_partition;
            auto verifier = std::make_shared<kythira::iam::iam_redis_verifier>(
                _m->_service->authorizer(), std::move(v));
            return [verifier](const kythira::credential_request& r) { return verifier->verify(r); };
        }

    private:
        iam_module* _m;
    };

    config _cfg;
    std::string _passphrase;
    std::unique_ptr<raft_host> _raft;
    std::uint64_t _group = 0;
    std::unique_ptr<service_type> _service;
    std::shared_ptr<evaluator> _evaluator;
    bool _raft_running = false;
    std::atomic<bool> _driving{false};
    std::thread _driver;
    std::atomic<bool> _started{false};
};

}  // namespace iam_node
