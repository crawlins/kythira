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

#include <kythira/iam/iam_admin.hpp>
#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_query_api.hpp>
#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/services/iam_actions.hpp>
#include <kythira/iam/services/sts_actions.hpp>
#include <kythira/iam/iam_routing.hpp>
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

using iam_node::node_id_type;
namespace iam = kythira::iam;
namespace admin = kythira::iam::admin;
namespace routing = kythira::iam::routing;
namespace https = kythira::https;
namespace http = boost::beast::http;

struct raft_types_plain : kythira::basic_tcp_raft_types<node_id_type> {
    using state_machine_type = iam::iam_state_machine;
};

struct raft_types_tls : kythira::basic_tcp_raft_types<node_id_type> {
    using state_machine_type = iam::iam_state_machine;
    using network_client_type = kythira::basic_tls_tcp_rpc_client<node_id_type>;
    using network_server_type = kythira::basic_tls_tcp_rpc_server<node_id_type>;
};

// A proposal or linearisable read on the admin path. Generous, as in
// ca_cluster_node: a loaded CI host can stretch a three-node round trip far
// past what an idle one needs.
constexpr auto k_command_timeout = std::chrono::milliseconds(30000);

std::atomic<bool> g_stop{false};
std::mutex g_stop_mu;
std::condition_variable g_stop_cv;
void on_signal(int) {
    g_stop = true;
    g_stop_cv.notify_all();
}

auto id_text(const node_id_type& id) -> std::string {
    return kythira::host_node_id_text(id);
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

auto json_reply(const https::request& req, unsigned status, const boost::json::object& body)
    -> https::response {
    https::response res{static_cast<http::status>(status), req.version()};
    res.set(http::field::content_type, "application/json");
    res.set(http::field::cache_control, "no-store");
    res.body() = boost::json::serialize(body);
    res.keep_alive(req.keep_alive());
    return res;
}

auto query_error(const https::request& req, bool sts, unsigned status, std::string_view code,
                 std::string_view message) -> https::response {
    https::response res{static_cast<http::status>(status), req.version()};
    res.set(http::field::content_type, "text/xml");
    const auto id = routing::request_id();
    res.set("x-amzn-RequestId", id);
    res.body() = routing::query_error_xml(sts, status < 500, code, message, id);
    res.keep_alive(req.keep_alive());
    return res;
}

/// One running member: the Raft node, the admin-cluster adapter the admin
/// operations call, the maintenance loop and the HTTPS handler.
template<typename Types> class member {
public:
    using raft_node_t = kythira::node<Types>;
    static constexpr bool k_rpc_tls =
        std::is_same_v<typename Types::network_client_type,
                       kythira::basic_tls_tcp_rpc_client<node_id_type>>;

    member(const iam_node::config& cfg, std::string passphrase,
           kythira::basic_tls_tcp_rpc_config<node_id_type> tls)
        : _cfg(cfg),
          _keks(std::move(passphrase)),
          _tls(std::move(tls)),
          _rpc_client(make_client()),
          _node(make_node()),
          _freshness([n = _node.get()] { return n->last_caught_up(); },
                     std::chrono::duration_cast<std::chrono::milliseconds>(cfg.max_staleness)),
          _registry(make_registry()),
          _authz(
              [n = _node.get()](const std::function<void(const iam::iam_state_machine&)>& f) {
                  n->with_state_machine([&](iam::iam_state_machine& sm) { f(sm); });
              },
              [this](std::string_view salt) -> const iam::seal_key& { return _keks.get(salt); },
              _registry),
          _query_cluster{this},
          _api(_query_cluster, _authz) {
        if (_cfg.replica) {
            _authz.set_freshness([this] { return _freshness.fresh(); });
        }
    }

    member(const member&) = delete;
    auto operator=(const member&) -> member& = delete;
    member(member&&) = delete;
    auto operator=(member&&) -> member& = delete;
    ~member() = default;

    // ── admin_cluster ──

    auto read() -> iam::iam_state_machine {
        const auto bytes = _node->read_state(k_command_timeout).get();
        iam::iam_state_machine sm(iam::iam_config{.partition = _cfg.arn_partition});
        sm.restore_from_snapshot(bytes, 0);
        return sm;
    }
    auto submit(const std::vector<std::byte>& cmd) -> std::vector<std::byte> {
        return _node->submit_command(cmd, k_command_timeout).get();
    }
    auto kek(std::string_view salt) -> const iam::seal_key& { return _keks.get(salt); }
    void add_learner(const iam::replica_record& r) { reach_replica(r, true); }

    // ── lifecycle ──

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
        _maintenance = std::thread([this] { maintain(); });
        https::server_config scfg;
        scfg.bind_address = _cfg.http_address;
        scfg.port = _cfg.http_port;
        scfg.tls_cert_path = _cfg.tls_cert_path;
        scfg.tls_key_path = _cfg.tls_key_path;
        scfg.client_ca_path = _cfg.client_ca_path;
        scfg.insecure_plaintext = _cfg.insecure_plaintext;
        _server = std::make_unique<https::server>(
            scfg,
            [this](const https::request& req, const https::request_context& ctx) {
                return handle(req, ctx);
            },
            [](bool warning, std::string_view m) {
                if (warning) {
                    std::cerr << "[warn] iam_node: " << m << "\n";
                }
            });
        _server->start();
        std::cerr << "[info] iam_node listening: node=" << id_text(_cfg.node_id)
                  << " rpc=" << _cfg.rpc_address << ":" << _cfg.rpc_port
                  << " http=" << _cfg.http_address << ":" << _server->port()
                  << " rpc_tls=" << (k_rpc_tls ? "true" : "false")
                  << " replica=" << (_cfg.replica ? "true" : "false") << "\n";
        if (_cfg.admin_uri_sans.empty()) {
            std::cerr << "[warn] iam_node: no --admin-uri-san given; /v1/admin/* refuses "
                         "every request\n";
        }
    }

    void stop() {
        _stopping = true;
        g_stop_cv.notify_all();
        // First: stop() rejects every pending submit_command()/read_state()
        // future, which may be all that wakes an admin handler or the
        // maintenance loop blocked in .get() once quorum is gone (the
        // shutdown hang ca_cluster_node fixed the same way).
        _node->stop();
        if (_server) {
            _server->stop();
        }
        for (auto* t : {&_election_timer, &_heartbeat_timer, &_maintenance}) {
            if (t->joinable()) {
                t->join();
            }
        }
    }

private:
    /// The IAM Query API's view of the cluster: the leader's applied state
    /// and its proposals. A follower redirects before it gets here.
    struct query_cluster_adapter {
        member* m;
        void read(const std::function<void(const iam::iam_state_machine&)>& f) {
            const auto done = m->_node->with_leader_state_machine([&](iam::iam_state_machine& sm) {
                f(sm);
                return true;
            });
            if (!done) {
                throw std::runtime_error("this member is no longer the leader");
            }
        }
        auto submit(const std::vector<std::byte>& cmd) -> std::vector<std::byte> {
            return m->submit(cmd);
        }
        auto kek(std::string_view salt) -> const iam::seal_key& { return m->kek(salt); }
    };

    static auto make_registry() -> iam::iam_service_registry {
        iam::iam_service_registry r;
        r.add(iam::services::iam_actions());
        r.add(iam::services::sts_actions());
        return r;
    }

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

    // Makes a registered replica reachable from this leader and, once,
    // admits it as a learner. Safe to repeat: an id already in the
    // configuration is left alone.
    void reach_replica(const iam::replica_record& r, bool throw_on_failure) {
        auto id = kythira::parse_host_peer_node_id(r.node_id);
        if (!id) {
            if (throw_on_failure) {
                throw std::invalid_argument("replica node id: " + id.error());
            }
            return;
        }
        {
            const std::lock_guard lock(_replicas_mu);
            const auto known = _reachable.find(r.node_id);
            if (known == _reachable.end() || !(known->second == r)) {
                {
                    const std::lock_guard alock(_addresses->mu);
                    _addresses->by_id[*id] = {r.rpc_host, r.rpc_port};
                }
                _reachable[r.node_id] = r;
                if constexpr (k_rpc_tls) {
                    _rpc_client_handle->add_peer(*id, r.rpc_host, r.rpc_port);
                    auto policy = trust_policy(_reachable);
                    _rpc_client_handle->reload_trust_policy(policy);
                    _rpc_server_handle->reload_trust_policy(policy);
                }
            }
        }
        try {
            _node->add_learner(*id).get();
            std::cerr << "[info] iam_node: replica " << r.node_id << " (" << r.rpc_host << ":"
                      << r.rpc_port << ") added as a learner\n";
        } catch (const std::exception& e) {
            if (std::string_view(e.what()).find("already in configuration") !=
                std::string_view::npos) {
                return;
            }
            if (throw_on_failure) {
                throw;
            }
            std::cerr << "[warn] iam_node: adding replica " << r.node_id
                      << " as a learner failed, will retry: " << e.what() << "\n";
        }
    }

    // Once a second: check the KEK as soon as the salt exists, and on the
    // leader make every registered replica reachable.
    void maintain() {
        while (!_stopping.load() && !g_stop.load()) {
            check_kek();
            if (_node->is_leader()) {
                auto replicas = _node->with_leader_state_machine(
                    [](iam::iam_state_machine& sm) { return sm.replicas(); });
                if (replicas) {
                    for (const auto& [_, r] : *replicas) {
                        bool reached = false;
                        {
                            const std::lock_guard lock(_replicas_mu);
                            const auto it = _reachable.find(r.node_id);
                            reached = it != _reachable.end() && it->second == r &&
                                      _admitted.contains(r.node_id);
                        }
                        if (!reached) {
                            reach_replica(r, false);
                            const std::lock_guard lock(_replicas_mu);
                            _admitted.insert(r.node_id);
                        }
                    }
                }
            } else {
                const std::lock_guard lock(_replicas_mu);
                _admitted.clear();  // a later term re-checks the configuration
            }
            std::unique_lock lock(g_stop_mu);
            g_stop_cv.wait_for(lock, std::chrono::seconds(1),
                               [this] { return _stopping.load() || g_stop.load(); });
        }
    }

    void check_kek() {
        if (_unsealed.load()) {
            return;
        }
        auto facts = _node->with_state_machine([](iam::iam_state_machine& sm) {
            return std::pair{sm.kek_salt(), admin::first_sealed(sm)};
        });
        if (facts.first.empty()) {
            return;  // no account yet: nothing is sealed
        }
        if (!admin::kek_opens(facts.second, _keks.get(facts.first))) {
            std::cerr << "[fatal] iam_node: the unseal passphrase does not open this cluster's "
                         "sealed secrets: wrong --unseal-key-file\n";
            std::_Exit(1);
        }
        if (facts.second) {
            _unsealed = true;
        }
    }

    auto health(const https::request& req) -> https::response {
        auto local = _node->with_state_machine([](iam::iam_state_machine& sm) {
            return boost::json::object{
                {"accounts", static_cast<std::int64_t>(sm.accounts().size())},
                {"replicas", static_cast<std::int64_t>(sm.replicas().size())},
                {"applied", sm.last_applied()},
                {"sealing_salt_set", !sm.kek_salt().empty()}};
        });
        // known_leader() is the leader a follower last heard from; a leader
        // hears from none, so it names itself here.
        const bool leading = _node->is_leader();
        const auto leader = leading ? std::optional(_cfg.node_id) : _node->known_leader();
        local["node_id"] = id_text(_cfg.node_id);
        local["role"] = leading ? "leader" : (_cfg.replica ? "replica" : "follower");
        local["leader"] = leader ? boost::json::value(id_text(*leader)) : boost::json::value();
        local["term"] = _node->get_current_term();
        local["unsealed"] = _unsealed.load();
        if (_cfg.replica) {
            // Requirement 7.4: how far behind this replica can be.
            const auto lag = _freshness.lag();
            local["lag_seconds"] = lag ? boost::json::value(lag->count()) : boost::json::value();
            local["fresh"] = _freshness.fresh();
        }
        return json_reply(req, 200, local);
    }

    auto redirect_to_leader(const https::request& req) -> https::response {
        const auto leader = _node->known_leader();
        if (leader && !(*leader == _cfg.node_id)) {
            if (auto address = _cfg.http_address_for(*leader)) {
                https::response res{http::status::temporary_redirect, req.version()};
                res.set(http::field::location, *address + std::string(req.target()));
                res.set(http::field::content_type, "application/json");
                res.body() = boost::json::serialize(
                    boost::json::object{{"error", "NotLeader"}, {"leader", id_text(*leader)}});
                res.keep_alive(req.keep_alive());
                return res;
            }
        }
        return json_reply(
            req, 503,
            {{"error", "NoLeader"}, {"message", "No leader is known yet; retry shortly."}});
    }

    auto handle_admin(const https::request& req, const https::request_context& ctx)
        -> https::response {
        if (!routing::admin_allowed(ctx.client_verified, ctx.client_uri_sans,
                                    _cfg.admin_uri_sans)) {
            return json_reply(req, 403,
                              {{"error", "AccessDenied"},
                               {"message",
                                "Admin routes need a client certificate whose URI "
                                "SAN is on --admin-uri-san."}});
        }
        if (!_node->is_leader()) {
            return redirect_to_leader(req);
        }
        const auto target = std::string_view(req.target());
        const auto path = target.substr(0, target.find('?'));
        boost::json::object body;
        if (req.method() == http::verb::post && !req.body().empty()) {
            boost::system::error_code ec;
            auto parsed = boost::json::parse(req.body(), ec);
            if (ec || !parsed.is_object()) {
                return json_reply(
                    req, 400,
                    {{"error", "InvalidInput"}, {"message", "The body must be a JSON object."}});
            }
            body = std::move(parsed.as_object());
        }
        try {
            admin::reply r;
            if (path == "/v1/admin/accounts" && req.method() == http::verb::get) {
                r = admin::list_accounts(*this);
            } else if (path == "/v1/admin/accounts" && req.method() == http::verb::post) {
                r = admin::create_account(*this, body);
                if (r.status == 201) {
                    std::cerr << "[info] iam_node: account "
                              << boost::json::serialize(r.body.at("account_id"))
                              << " created by admin\n";
                }
            } else if (path == "/v1/admin/sts-keys/rotate" && req.method() == http::verb::post) {
                r = admin::rotate_sts_key(*this, body);
            } else if (path == "/v1/admin/replicas" && req.method() == http::verb::post) {
                const auto* id = body.if_contains("node_id");
                if (id != nullptr && id->is_string() &&
                    !kythira::parse_host_peer_node_id(std::string(id->as_string()))) {
                    return json_reply(req, 400,
                                      {{"error", "InvalidInput"},
                                       {"message", "node_id is not a node id this cluster uses."}});
                }
                r = admin::add_replica(*this, body);
            } else {
                return json_reply(req, 404, {{"error", "NotFound"}, {"message", "No such route."}});
            }
            return json_reply(req, r.status, r.body);
        } catch (const std::exception& e) {
            // Lost leadership, no quorum, or a timeout: nothing the client
            // did. The message names no secret.
            return json_reply(req, 503, {{"error", "Unavailable"}, {"message", e.what()}});
        }
    }

    /// The IAM Query API (Requirement 9): SigV4 authentication, then the
    /// action, which authorizes itself. Served by the leader; a follower
    /// redirects, as it does for admin writes.
    auto handle_iam(const https::request& req, const https::request_context& ctx)
        -> https::response {
        if (!_node->is_leader()) {
            return redirect_to_leader(req);
        }
        const std::string method(req.method_string());
        const std::string target(req.target());
        const auto q = target.find('?');
        kythira::sigv4::request sr;
        sr.method = method;
        sr.raw_path = std::string_view(target).substr(0, q);
        sr.raw_query =
            q == std::string::npos ? std::string_view{} : std::string_view(target).substr(q + 1);
        for (const auto& h : req) {
            sr.headers.emplace_back(std::string(h.name_string()), std::string(h.value()));
        }
        sr.body = std::string_view(req.body());
        const auto who = _authz.authenticate(sr, {.service = "iam"});
        if (!who) {
            const auto& e = who.error();
            return query_error(req, false, static_cast<unsigned>(e.http_status), e.code, e.message);
        }
        const auto form = kythira::aws_query::form::parse(
            req.method() == http::verb::post ? std::string_view(req.body()) : sr.raw_query);
        if (!form) {
            return query_error(req, false, 400, "MalformedQueryString",
                               "The request parameters could not be decoded.");
        }
        iam::query::request_facts facts;
        facts.request_id = routing::request_id();
        facts.source_ip = ctx.peer_address;
        facts.user_agent = std::string(req[http::field::user_agent]);
        facts.secure_transport = ctx.tls;
        const auto r = _api.handle(*who, *form, facts);
        https::response res{static_cast<http::status>(r.status), req.version()};
        res.set(http::field::content_type, "text/xml");
        res.set("x-amzn-RequestId", facts.request_id);
        res.body() = r.body;
        res.keep_alive(req.keep_alive());
        return res;
    }

    auto handle(const https::request& req, const https::request_context& ctx) -> https::response {
        const auto target = std::string_view(req.target());
        const auto q = target.find('?');
        const auto path = target.substr(0, q);
        const auto query = q == std::string_view::npos ? std::string_view{} : target.substr(q + 1);
        const auto auth = req[http::field::authorization];
        if (_cfg.replica && path == "/metrics" && req.method() == http::verb::get) {
            https::response res{http::status::ok, req.version()};
            res.set(http::field::content_type, "text/plain; version=0.0.4");
            res.body() = _freshness.metrics_text();
            res.keep_alive(req.keep_alive());
            return res;
        }
        const auto r = routing::route_of(path, std::string_view(auth.data(), auth.size()), query);
        if ((r == routing::route::iam || r == routing::route::sts) && _cfg.replica &&
            !_freshness.fresh()) {
            // Fail closed (Requirement 7.4): a stale replica may still hold
            // a key or policy since changed.
            return query_error(req, r == routing::route::sts, 503,
                               iam::iam_authorizer::k_service_unavailable,
                               "The IAM state on this node is stale; retry shortly.");
        }
        switch (r) {
            case routing::route::health:
                return health(req);
            case routing::route::admin:
                return handle_admin(req, ctx);
            case routing::route::iam:
                return handle_iam(req, ctx);
            case routing::route::sts:
                return query_error(req, true, 501, "NotImplemented",
                                   "This STS action is not available on this endpoint yet.");
            case routing::route::unknown_service:
                return query_error(req, false, 400, "InvalidAction",
                                   "This endpoint serves the IAM and STS APIs only.");
            case routing::route::unauthenticated:
                break;
        }
        return query_error(req, false, 403, "MissingAuthenticationToken",
                           "Request is missing Authentication Token");
    }

    const iam_node::config& _cfg;
    admin::kek_cache _keks;
    kythira::basic_tls_tcp_rpc_config<node_id_type> _tls;
    // Registered replicas' RPC addresses, which the plain client's resolver
    // reads.
    struct address_book {
        std::mutex mu;
        std::map<node_id_type, std::pair<std::string, std::uint16_t>> by_id;
    };
    std::shared_ptr<address_book> _addresses = std::make_shared<address_book>();
    std::optional<typename Types::network_client_type> _rpc_client;
    // TLS only: handles sharing the live transports node<Types> owns, for
    // adding a replica's address and reloading the trust policy.
    std::optional<typename Types::network_client_type> _rpc_client_handle;
    std::optional<typename Types::network_server_type> _rpc_server_handle;
    std::unique_ptr<raft_node_t> _node;
    iam::replica_freshness _freshness;
    iam::iam_service_registry _registry;
    iam::iam_authorizer _authz;
    query_cluster_adapter _query_cluster;
    iam::query::iam_query_api<query_cluster_adapter> _api;
    std::unique_ptr<https::server> _server;
    std::thread _election_timer;
    std::thread _heartbeat_timer;
    std::thread _maintenance;
    std::atomic<bool> _stopping{false};
    std::atomic<bool> _unsealed{false};
    std::mutex _replicas_mu;
    std::map<std::string, iam::replica_record> _reachable;
    std::set<std::string> _admitted;
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
