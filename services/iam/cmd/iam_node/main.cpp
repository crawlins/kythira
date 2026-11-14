// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// iam_node: hosts the IAM Raft group and serves IAM, STS and the Kythira
// admin routes on one HTTPS listener (.kiro/specs/iam-compatible-auth/
// Requirement 8, design Component 7).
//
// It follows ca_cluster_node:
//
//  - Raft over TCP, mutual TLS when --rpc-tls-* is given. Each member
//    presents its Kythira CA node certificate, and a peer is accepted only
//    with the reserved peer SAN of a configured member, bound to that
//    member's node id (tls_rpc_trust_policy::peer_node_ids).
//  - The unseal passphrase comes from --unseal-key-file. The KEK is derived
//    from it and the salt the first create_account records, and checked
//    against a stored sealed secret: a member started with the wrong
//    passphrase exits instead of sealing secrets no other member can open.
//  - A follower answers an admin request with a 307 to the leader's address,
//    and 503 while no leader is known. /healthz answers on every member.
//
// The public listener is the core Beast HTTPS server
// (raft/beast_https_server.hpp). It asks for, but does not require, a client
// certificate: /v1/admin/* needs one whose URI SAN is on --admin-uri-san,
// while SigV4 clients present none. IAM and STS requests are routed by their
// credential scope. IAM requests are authenticated with SigV4 and handled by
// iam_query_api.hpp on the leader (a follower redirects); actions it does not
// serve yet, and every STS action until task 17, answer NotImplemented in
// AWS's Query error shape.
//
// Read replicas: `kythira-iam replica-add` registers a replica (a node
// started with --replica) in the IAM state, and the leader adds it as a Raft
// learner. Every leader, including one elected later, reads the registered
// replicas from the state and makes each reachable, so the address list
// lives in the replicated log rather than in each member's flags. A replica
// reports its lag (seconds since it last held every committed change) in
// /healthz and as `kythira_iam_replica_lag_seconds` on /metrics, and past
// --iam-max-staleness-secs answers IAM and STS requests ServiceUnavailable.

#include "config.hpp"
#include "iam_service.hpp"

#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/ca_http_helpers.hpp>
#include <raft/console_logger.hpp>
#include <raft/file_persistence.hpp>
#include <raft/membership.hpp>
#include <raft/metrics.hpp>
#include <raft/quorum_management.hpp>
#include <raft/raft.hpp>
#include <raft/tcp_raft_types.hpp>
#include <raft/tcp_rpc.hpp>
#include <raft/tls_tcp_rpc.hpp>

#include <boost/json.hpp>

#include <openssl/pem.h>
#include <openssl/x509v3.h>

// Present in every CMake build, which links folly whatever the future
// backend; absent only in a folly-free build of the Boost backend.
#if __has_include(<folly/init/Init.h>)
#include <folly/init/Init.h>
#define KYTHIRA_IAM_NODE_HAS_FOLLY_INIT 1
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {

using iam_node::id_text;
using iam_node::node_id_type;
namespace iam = kythira::iam;

struct raft_types_plain : kythira::basic_tcp_raft_types<node_id_type> {
    using state_machine_type = iam::iam_state_machine;
};

struct raft_types_tls : kythira::basic_tcp_raft_types<node_id_type> {
    using state_machine_type = iam::iam_state_machine;
    using network_client_type = kythira::basic_tls_tcp_rpc_client<node_id_type>;
    using network_server_type = kythira::basic_tls_tcp_rpc_server<node_id_type>;
};

std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;
void on_signal(int) {
    g_stop = true;
    g_stop_cv.notify_all();
}

auto read_file(const std::string& path) -> std::string {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("cannot read " + path);
    }
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Whether the certificate at `path` carries `name` as a DNS or URI SAN: a
// node certificate without its own reserved peer SAN would be refused by
// every peer, so iam_node says so at startup rather than failing to elect.
auto cert_names(const std::string& path, const std::string& name) -> bool {
    const auto pem = read_file(path);
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return false;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (cert == nullptr) {
        return false;
    }
    auto* names =
        static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    bool found = false;
    for (int i = 0; names != nullptr && i < sk_GENERAL_NAME_num(names) && !found; ++i) {
        const GENERAL_NAME* gn = sk_GENERAL_NAME_value(names, i);
        const ASN1_STRING* s = nullptr;
        if (gn->type == GEN_DNS) {
            s = gn->d.dNSName;
        } else if (gn->type == GEN_URI) {
            s = gn->d.uniformResourceIdentifier;
        }
        if (s != nullptr) {
            found = std::string_view(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                                     static_cast<std::size_t>(ASN1_STRING_length(s))) == name;
        }
    }
    GENERAL_NAMES_free(names);
    X509_free(cert);
    return found;
}

/// One running member: the Raft node on its TCP RPC transport, its timers,
/// and the serving side over it (iam_service.hpp).
template<typename Types> class member {
public:
    using raft_node_t = kythira::node<Types>;
    static constexpr bool k_rpc_tls =
        std::is_same_v<typename Types::network_client_type,
                       kythira::basic_tls_tcp_rpc_client<node_id_type>>;

    member(const iam_node::config& cfg, std::string passphrase,
           kythira::basic_tls_tcp_rpc_config<node_id_type> tls)
        : _cfg(cfg),
          _tls(std::move(tls)),
          _rpc_client(make_client()),
          _node(make_node()),
          _service(
              cfg, std::move(passphrase), *_node,
              {.make_reachable = [this](const node_id_type& id,
                                        const iam::replica_record& r) { make_reachable(id, r); }},
              "rpc=" + cfg.rpc_address + ":" + std::to_string(cfg.rpc_port) +
                  " rpc_tls=" + (k_rpc_tls ? "true" : "false")) {}

    member(const member&) = delete;
    auto operator=(const member&) -> member& = delete;
    member(member&&) = delete;
    auto operator=(member&&) -> member& = delete;
    ~member() = default;

    void start() {
        if (_cfg.replica) {
            _node->set_cluster_configuration(_cfg.voter_ids(), {_cfg.node_id});
        } else {
            _node->set_cluster_configuration(_cfg.voter_ids());
        }
        _node->start();
        // Raft's timers, driven as cmd/chaos_node and ca_cluster_node do.
        _election_timer = std::thread([this] {
            while (!_stopping.load()) {
                std::this_thread::sleep_for(_cfg.election_timeout_min / 2);
                if (!_stopping.load()) {
                    _node->check_election_timeout();
                }
            }
        });
        _heartbeat_timer = std::thread([this] {
            while (!_stopping.load()) {
                std::this_thread::sleep_for(_cfg.heartbeat_interval);
                if (!_stopping.load()) {
                    _node->check_heartbeat_timeout();
                }
            }
        });
        _service.start();
    }

    void stop() {
        _stopping = true;
        // First: stop() rejects every pending submit_command()/read_state()
        // future, which may be all that wakes an admin handler or the
        // maintenance loop blocked in .get() once quorum is gone (the
        // shutdown hang ca_cluster_node fixed the same way).
        _node->stop();
        _service.stop();
        for (auto* t : {&_election_timer, &_heartbeat_timer}) {
            if (t->joinable()) {
                t->join();
            }
        }
    }

private:
    auto make_client() -> std::optional<typename Types::network_client_type> {
        if constexpr (k_rpc_tls) {
            return typename Types::network_client_type(_tls);
        } else {
            return typename Types::network_client_type();
        }
    }

    auto make_node() -> std::unique_ptr<raft_node_t> {
        kythira::raft_configuration rc;
        rc._election_timeout_min = _cfg.election_timeout_min;
        rc._election_timeout_max = _cfg.election_timeout_max;
        rc._heartbeat_interval = _cfg.heartbeat_interval;
        rc._rpc_timeout = _cfg.rpc_timeout;

        auto server = [&] {
            if constexpr (k_rpc_tls) {
                return typename Types::network_server_type(_cfg.rpc_port, _tls, _cfg.rpc_address);
            } else {
                return typename Types::network_server_type(_cfg.rpc_port, _cfg.rpc_address);
            }
        }();
        for (const auto& p : _cfg.peers) {
            _rpc_client->add_peer(p.node_id, p.rpc_host, p.rpc_port);
        }
        if constexpr (k_rpc_tls) {
            // Copies share the live transport (tls_tcp_rpc.hpp), so these
            // reach what node<Types> ends up owning.
            _rpc_server_handle.emplace(server);
            _rpc_client_handle.emplace(*_rpc_client);
        } else {
            // The plain client is move-only; it asks this resolver for any
            // node its own table does not name, which is how it finds a
            // replica registered after start.
            _rpc_client->set_peer_resolver(
                [addresses = _addresses](const node_id_type& id)
                    -> std::optional<std::pair<std::string, std::uint16_t>> {
                    const std::lock_guard lock(addresses->mu);
                    const auto it = addresses->by_id.find(id);
                    if (it == addresses->by_id.end()) {
                        return std::nullopt;
                    }
                    return it->second;
                });
        }
        // Learners share the default placement group with the voters; the
        // group's learner capacity is --max-replicas (non-voting-nodes:
        // without a declared target the core admits no learner at all).
        kythira::desired_topology<std::string> topology;
        topology.groups.push_back({.group_id = std::string{},
                                   .target_count = _cfg.voter_ids().size(),
                                   .learner_capacity = _cfg.max_replicas});
        kythira::node_config<Types> ncfg{
            .node_id = _cfg.node_id,
            .network_client = std::move(*_rpc_client),
            .network_server = std::move(server),
            .persistence = kythira::file_persistence_engine<node_id_type>(_cfg.data_dir),
            .logger = kythira::console_logger{kythira::log_level::info},
            .metrics = kythira::noop_metrics{},
            .membership = kythira::default_membership_manager<node_id_type>{},
            .state_machine =
                iam::iam_state_machine(iam::iam_config{.partition = _cfg.arn_partition}),
            .config = rc,
            .quorum_manager = kythira::no_op_quorum_manager<node_id_type, std::string, std::string>(
                std::move(topology)),
        };
        _rpc_client.reset();
        return std::make_unique<raft_node_t>(std::move(ncfg));
    }

    // Every configured member's reserved peer name, plus each registered
    // replica's: the only certificates RPC TLS accepts, each bound to the
    // node it names.
    auto trust_policy(const std::map<std::string, iam::replica_record>& replicas)
        -> kythira::basic_tls_rpc_trust_policy<node_id_type> {
        std::map<std::string, node_id_type> ids;
        ids[raft::testing::peer_identity_name(_cfg.node_id)] = _cfg.node_id;
        for (const auto& p : _cfg.peers) {
            ids[raft::testing::peer_identity_name(p.node_id)] = p.node_id;
        }
        for (const auto& [text, _] : replicas) {
            if (auto id = kythira::parse_host_peer_node_id(text)) {
                ids[raft::testing::peer_identity_name(*id)] = *id;
            }
        }
        return _tls.trust_policy.binding_peer_node_ids(std::move(ids));
    }

    // A registered replica's RPC address, for the plain client's resolver
    // or the TLS client, and over TLS a trust policy naming it.
    void make_reachable(const node_id_type& id, const iam::replica_record& r) {
        {
            const std::lock_guard alock(_addresses->mu);
            _addresses->by_id[id] = {r.rpc_host, r.rpc_port};
        }
        _reachable[r.node_id] = r;
        if constexpr (k_rpc_tls) {
            _rpc_client_handle->add_peer(id, r.rpc_host, r.rpc_port);
            auto policy = trust_policy(_reachable);
            _rpc_client_handle->reload_trust_policy(policy);
            _rpc_server_handle->reload_trust_policy(policy);
        }
    }

    const iam_node::config& _cfg;
    kythira::basic_tls_tcp_rpc_config<node_id_type> _tls;
    // Registered replicas' RPC addresses, which the plain client's resolver
    // reads.
    struct address_book {
        std::mutex mu;
        std::map<node_id_type, std::pair<std::string, std::uint16_t>> by_id;
    };
    std::shared_ptr<address_book> _addresses = std::make_shared<address_book>();
    // The replicas made reachable so far (iam_service serializes calls).
    std::map<std::string, iam::replica_record> _reachable;
    std::optional<typename Types::network_client_type> _rpc_client;
    // TLS only: handles sharing the live transports node<Types> owns, for
    // adding a replica's address and reloading the trust policy.
    std::optional<typename Types::network_client_type> _rpc_client_handle;
    std::optional<typename Types::network_server_type> _rpc_server_handle;
    std::unique_ptr<raft_node_t> _node;
    iam_node::iam_service<raft_node_t> _service;
    std::thread _election_timer;
    std::thread _heartbeat_timer;
    std::atomic<bool> _stopping{false};
};

template<typename Types>
auto run(const iam_node::config& cfg, std::string passphrase,
         kythira::basic_tls_tcp_rpc_config<node_id_type> tls) -> int {
    member<Types> m(cfg, std::move(passphrase), std::move(tls));
    m.start();
    {
        std::unique_lock lock(g_stop_mu);
        g_stop_cv.wait(lock, [] { return g_stop.load(); });
    }
    std::cerr << "[info] iam_node: stopping\n";
    m.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef KYTHIRA_IAM_NODE_HAS_FOLLY_INIT
    // As in ca_cluster_node: folly::Init with only the program name, so its
    // flag parser never sees this binary's flags. It registers the
    // Timekeeper the RPC client's retries use.
    int folly_argc = 1;
    char* folly_argv_storage[] = {argv[0]};
    char** folly_argv = folly_argv_storage;
    folly::Init folly_init(&folly_argc, &folly_argv, false);
#endif

    iam_node::config cfg;
    try {
        cfg = iam_node::parse_args(argc, argv);
    } catch (const std::invalid_argument& e) {
        if (*e.what() != '\0') {
            std::cerr << "iam_node: " << e.what() << "\n\n";
        }
        std::cerr << iam_node::k_usage;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "iam_node: " << e.what() << "\n";
        return 1;
    }

    std::string passphrase;
    try {
        passphrase = iam_node::read_unseal_passphrase(cfg.unseal_key_file);
    } catch (const std::exception& e) {
        std::cerr << "iam_node: " << e.what() << "\n";
        return 1;
    }

    (void)std::signal(SIGTERM, on_signal);
    (void)std::signal(SIGINT, on_signal);
    (void)std::signal(SIGPIPE, SIG_IGN);

    try {
        if (cfg.rpc_tls()) {
            const auto own = raft::testing::peer_identity_name(cfg.node_id);
            if (!cert_names(cfg.rpc_tls_cert_path, own)) {
                std::cerr << "iam_node: --rpc-tls-cert does not carry this node's reserved peer "
                             "SAN "
                          << own << "; peers would refuse it\n";
                return 1;
            }
            kythira::basic_tls_tcp_rpc_config<node_id_type> tls;
            tls.cert_path = cfg.rpc_tls_cert_path;
            tls.key_path = cfg.rpc_tls_key_path;
            std::map<std::string, node_id_type> ids;
            ids[own] = cfg.node_id;
            for (const auto& p : cfg.peers) {
                ids[raft::testing::peer_identity_name(p.node_id)] = p.node_id;
            }
            tls.trust_policy = kythira::ca_root_only<node_id_type>(read_file(cfg.rpc_tls_ca_path))
                                   .binding_peer_node_ids(std::move(ids));
            return run<raft_types_tls>(cfg, std::move(passphrase), std::move(tls));
        }
        std::cerr << "[warn] iam_node: Raft RPC is PLAINTEXT and UNAUTHENTICATED on "
                  << cfg.rpc_address << ":" << cfg.rpc_port
                  << (cfg.allow_plaintext_rpc ? " (--allow-plaintext-rpc)" : " (loopback only)")
                  << "\n";
        return run<raft_types_plain>(cfg, std::move(passphrase), {});
    } catch (const std::exception& e) {
        std::cerr << "iam_node: failed to start: " << e.what() << "\n";
        return 1;
    }
}
