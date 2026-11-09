// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_embedded_replica.hpp
/// @brief A front end's whole IAM side in one object: a read replica of the
///        IAM Raft group over mutual-TLS RPC, the unsealing key, the action
///        registry and the authorizer (`.kiro/specs/iam-compatible-auth/`
///        Requirement 7.3, task 21).
///
/// `iam_replica` (task 13) is the learner; this wraps it with what every
/// front end would otherwise build again from `iam_node`'s replica mode:
///
///   iam::embedded_replica_config cfg;   // from the front end's own flags
///   iam::embedded_replica replica(std::move(cfg));
///   replica.start();          // then `kythira-iam replica-add` admits it
///   iam::iam_redis_verifier v(replica.authorizer(), {...});
///
/// The replica's RPC certificate must carry its node id's reserved peer
/// SAN (`peer_identity_name`), as an `iam_node` member's does: the voters
/// bind each registered replica's certificate to its id. Plaintext RPC is
/// not offered: a front end's replica holds the unsealing passphrase.

#include <kythira/iam/iam_admin.hpp>
#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/ca_http_helpers.hpp>
#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/host_node_id.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/quorum_management.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::iam {

/// One voter of the IAM group, as the replica reaches it.
struct replica_voter {
    host_node_id id;
    std::string rpc_host;
    std::uint16_t rpc_port = 0;
};

struct embedded_replica_config {
    /// This replica's id in the IAM group, registered with `replica-add`.
    host_node_id node_id;
    std::string rpc_address = "0.0.0.0";
    std::uint16_t rpc_port = 0;
    std::vector<replica_voter> voters;
    std::filesystem::path data_dir;
    /// The cluster's unsealing passphrase, which opens the access-key and
    /// STS-key secrets the authorizer verifies signatures with.
    std::string unseal_key_file;
    std::string rpc_tls_cert;
    std::string rpc_tls_key;
    std::string rpc_tls_ca;
    std::chrono::milliseconds max_staleness = replica_freshness::k_default_max_staleness;
    std::string partition = "aws";
    log_level log = log_level::warning;
};

/// `<id>:<host>:<port>[,...]`, the voters' Raft RPC addresses. An `@...`
/// suffix (an `iam_node --peers` entry's HTTPS URL) is ignored, so the same
/// list can be given to both.
[[nodiscard]] inline auto parse_replica_voters(std::string_view text)
    -> std::vector<replica_voter> {
    std::vector<replica_voter> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const auto comma = text.find(',', pos);
        auto item = text.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = comma == std::string_view::npos ? text.size() + 1 : comma + 1;
        if (item.empty()) {
            continue;
        }
        item = item.substr(0, item.find('@'));
        const auto port_colon = item.rfind(':');
        const auto host_colon = port_colon == std::string_view::npos
                                    ? std::string_view::npos
                                    : item.rfind(':', port_colon - 1);
        if (host_colon == std::string_view::npos || host_colon == 0 ||
            port_colon == host_colon + 1 || port_colon + 1 == item.size()) {
            throw std::invalid_argument("IAM voter '" + std::string(item) +
                                        "' is not <id>:<host>:<port>");
        }
        auto id = parse_host_peer_node_id(item.substr(0, host_colon));
        if (!id) {
            throw std::invalid_argument("IAM voter id: " + id.error());
        }
        const auto port_text = item.substr(port_colon + 1);
        unsigned long port = 0;
        for (const char c : port_text) {
            if (c < '0' || c > '9' || port > 65535) {
                port = 65536;
                break;
            }
            port = port * 10 + static_cast<unsigned long>(c - '0');
        }
        if (port == 0 || port > 65535) {
            throw std::invalid_argument("IAM voter port '" + std::string(port_text) +
                                        "' is not 1-65535");
        }
        out.push_back({*id, std::string(item.substr(host_colon + 1, port_colon - host_colon - 1)),
                       static_cast<std::uint16_t>(port)});
    }
    return out;
}

/// The first line of the unseal key file, which must not be empty.
[[nodiscard]] inline auto read_unseal_passphrase_file(const std::string& path) -> std::string {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("cannot open the IAM unseal key file " + path);
    }
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line.empty()) {
        throw std::runtime_error("the IAM unseal key file " + path + " is empty");
    }
    return line;
}

/// The Raft types of the embedded replica: TLS TCP RPC and file persistence,
/// as an `iam_node` member with RPC TLS.
struct embedded_replica_types : basic_tcp_raft_types<host_node_id> {
    using state_machine_type = iam_state_machine;
    using network_client_type = basic_tls_tcp_rpc_client<host_node_id>;
    using network_server_type = basic_tls_tcp_rpc_server<host_node_id>;
};

class embedded_replica {
public:
    /// Throws std::invalid_argument or std::runtime_error on a bad
    /// configuration; nothing is listening until `start()`.
    explicit embedded_replica(embedded_replica_config cfg,
                              std::function<void(iam_service_registry&)> register_actions = {})
        : _cfg(std::move(cfg)), _keks(read_unseal_passphrase_file(_cfg.unseal_key_file)) {
        if (_cfg.voters.empty()) {
            throw std::invalid_argument("the IAM replica needs the IAM voters");
        }
        if (_cfg.rpc_port == 0) {
            throw std::invalid_argument("the IAM replica needs an RPC port");
        }
        if (_cfg.rpc_tls_cert.empty() || _cfg.rpc_tls_key.empty() || _cfg.rpc_tls_ca.empty()) {
            throw std::invalid_argument(
                "the IAM replica needs an RPC TLS certificate, key and CA: it holds the "
                "unsealing passphrase, so its RPC is never plaintext");
        }
        if (_cfg.data_dir.empty()) {
            throw std::invalid_argument("the IAM replica needs a data directory");
        }
        if (register_actions) {
            register_actions(_registry);
        }
        std::filesystem::create_directories(_cfg.data_dir);
        _replica = std::make_unique<iam_replica<embedded_replica_types>>(
            make_node_config(), voter_ids(), _cfg.max_staleness);
        _authz = std::make_unique<iam_authorizer>(
            _replica->reader(),
            [this](std::string_view salt) -> const seal_key& { return _keks.get(salt); },
            _registry);
        _authz->set_freshness([r = _replica.get()] { return r->freshness().fresh(); });
    }

    embedded_replica(const embedded_replica&) = delete;
    auto operator=(const embedded_replica&) -> embedded_replica& = delete;
    embedded_replica(embedded_replica&&) = delete;
    auto operator=(embedded_replica&&) -> embedded_replica& = delete;
    ~embedded_replica() { stop(); }

    void start() { _replica->start(); }
    void stop() {
        if (_replica) {
            _replica->stop();
        }
    }

    [[nodiscard]] auto authorizer() -> iam_authorizer& { return *_authz; }
    [[nodiscard]] auto freshness() const -> const replica_freshness& {
        return _replica->freshness();
    }
    [[nodiscard]] auto replica() -> iam_replica<embedded_replica_types>& { return *_replica; }

private:
    [[nodiscard]] auto voter_ids() const -> std::vector<host_node_id> {
        std::vector<host_node_id> ids;
        ids.reserve(_cfg.voters.size());
        for (const auto& v : _cfg.voters) {
            ids.push_back(v.id);
        }
        return ids;
    }

    [[nodiscard]] static auto read_file(const std::string& path) -> std::string {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            throw std::runtime_error("cannot read " + path);
        }
        return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    }

    [[nodiscard]] auto make_node_config() -> node_config<embedded_replica_types> {
        basic_tls_tcp_rpc_config<host_node_id> tls;
        tls.cert_path = _cfg.rpc_tls_cert;
        tls.key_path = _cfg.rpc_tls_key;
        std::map<std::string, host_node_id> ids;
        ids[raft::testing::peer_identity_name(_cfg.node_id)] = _cfg.node_id;
        for (const auto& v : _cfg.voters) {
            ids[raft::testing::peer_identity_name(v.id)] = v.id;
        }
        tls.trust_policy = ca_root_only<host_node_id>(read_file(_cfg.rpc_tls_ca))
                               .binding_peer_node_ids(std::move(ids));
        embedded_replica_types::network_client_type client(tls);
        for (const auto& v : _cfg.voters) {
            client.add_peer(v.id, v.rpc_host, v.rpc_port);
        }
        embedded_replica_types::network_server_type server(_cfg.rpc_port, tls, _cfg.rpc_address);
        // A learner never campaigns; these only matter if it is ever
        // promoted, which the IAM leader never does.
        raft_configuration rc;
        desired_topology<std::string> topology;
        topology.groups.push_back(
            {.group_id = std::string{}, .target_count = _cfg.voters.size(), .learner_capacity = 1});
        return node_config<embedded_replica_types>{
            .node_id = _cfg.node_id,
            .network_client = std::move(client),
            .network_server = std::move(server),
            .persistence = file_persistence_engine<host_node_id>(_cfg.data_dir),
            .logger = console_logger{_cfg.log},
            .metrics = noop_metrics{},
            .membership = default_membership_manager<host_node_id>{},
            .state_machine = iam_state_machine(iam_config{.partition = _cfg.partition}),
            .config = rc,
            .quorum_manager =
                no_op_quorum_manager<host_node_id, std::string, std::string>(std::move(topology)),
        };
    }

    embedded_replica_config _cfg;
    admin::kek_cache _keks;
    iam_service_registry _registry;
    std::unique_ptr<iam_replica<embedded_replica_types>> _replica;
    std::unique_ptr<iam_authorizer> _authz;
};

}  // namespace kythira::iam
