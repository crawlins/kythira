// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file iam_service.hpp
/// @brief One IAM member's serving side over a Raft node it does not own:
///        the admin-cluster adapter the admin operations call, the
///        authorizer, the IAM, STS and admin HTTPS handler, and the
///        maintenance loop (KEK check, read replicas).
///
/// iam_node runs it over its own TCP-RPC node (main.cpp), and the IAM
/// module (iam_module.hpp, .kiro/specs/service-colocation/ task 8) over
/// its group on kythira_node's shared multi-raft host. Whoever owns the
/// node stops it before `stop()`: stopping the node rejects every pending
/// proposal and read, which may be all that wakes a handler or the
/// maintenance loop blocked on one once quorum is gone.
///
/// Read replicas need the owner's transport (an address to reach a
/// learner at, a trust policy naming it); without `replica_hooks::
/// make_reachable` the member takes none.

#include "config.hpp"

#include <kythira/iam/iam_admin.hpp>
#include <kythira/iam/iam_authorizer.hpp>
#include <kythira/iam/iam_oidc_jwks.hpp>
#include <kythira/iam/iam_query_api.hpp>
#include <kythira/iam/iam_replica.hpp>
#include <kythira/iam/iam_routing.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>
#include <kythira/iam/iam_sts_api.hpp>
#include <kythira/iam/services/iam_actions.hpp>
#include <kythira/iam/services/sts_actions.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/host_node_id.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace iam_node {

namespace iam = kythira::iam;
namespace admin = kythira::iam::admin;
namespace routing = kythira::iam::routing;
namespace https = kythira::https;
namespace http = boost::beast::http;

// A proposal or linearisable read on the admin path. Generous, as in
// ca_cluster_node: a loaded CI host can stretch a three-node round trip far
// past what an idle one needs.
inline constexpr auto k_command_timeout = std::chrono::milliseconds(30000);

inline auto id_text(const node_id_type& id) -> std::string {
    return kythira::host_node_id_text(id);
}

inline auto json_reply(const https::request& req, unsigned status, const boost::json::object& body)
    -> https::response {
    https::response res{static_cast<http::status>(status), req.version()};
    res.set(http::field::content_type, "application/json");
    res.set(http::field::cache_control, "no-store");
    res.body() = boost::json::serialize(body);
    res.keep_alive(req.keep_alive());
    return res;
}

inline auto query_error(const https::request& req, bool sts, unsigned status, std::string_view code,
                        std::string_view message) -> https::response {
    https::response res{static_cast<http::status>(status), req.version()};
    res.set(http::field::content_type, "text/xml");
    const auto id = routing::request_id();
    res.set("x-amzn-RequestId", id);
    res.body() = routing::query_error_xml(sts, status < 500, code, message, id);
    res.keep_alive(req.keep_alive());
    return res;
}

/// What the node's owner gives the member for read replicas.
struct replica_hooks {
    /// Makes the learner `id` reachable at `record`'s RPC address (and
    /// trusted, over TLS). Called again whenever the record changes.
    /// Empty: the member takes no read replicas.
    std::function<void(const node_id_type& id, const iam::replica_record& record)> make_reachable;
};

template<typename RaftNode> class iam_service {
public:
    /// @param cfg, node Outlive this object.
    /// @param transport How the start-up line describes the Raft side.
    iam_service(const config& cfg, std::string passphrase, RaftNode& node, replica_hooks replicas,
                std::string transport)
        : _cfg(cfg),
          _keks(std::move(passphrase)),
          _node(&node),
          _replicas(std::move(replicas)),
          _transport(std::move(transport)),
          _freshness([n = _node] { return n->last_caught_up(); },
                     std::chrono::duration_cast<std::chrono::milliseconds>(cfg.max_staleness)),
          _registry(make_registry()),
          _authz(
              [n = _node](const std::function<void(const iam::iam_state_machine&)>& f) {
                  n->with_state_machine([&](iam::iam_state_machine& sm) { f(sm); });
              },
              [this](std::string_view salt) -> const iam::seal_key& { return _keks.get(salt); },
              _registry),
          _query_cluster{this},
          _api(_query_cluster, _authz),
          _jwks(make_fetcher(cfg)),
          _sts_api(_query_cluster, _authz, &_jwks) {
        if (_cfg.replica) {
            _authz.set_freshness([this] { return _freshness.fresh(); });
        }
        if (!_cfg.kythira_issuer_local.empty()) {
            // The Kythira issuer's keys straight from a CA member, trusted
            // through --oidc-ca-file when given, else the cluster's own CA,
            // never the system store.
            iam::oidc::fetch_rules local;
            local.ca_file = !_cfg.oidc_ca_file.empty()      ? _cfg.oidc_ca_file
                            : !_cfg.rpc_tls_ca_path.empty() ? _cfg.rpc_tls_ca_path
                                                            : _cfg.client_ca_path;
            _jwks.set_direct(issuer_name(), _cfg.kythira_issuer_local, make_fetcher_with(local));
        }
    }

    iam_service(const iam_service&) = delete;
    auto operator=(const iam_service&) -> iam_service& = delete;
    iam_service(iam_service&&) = delete;
    auto operator=(iam_service&&) -> iam_service& = delete;
    ~iam_service() { stop(); }

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

    /// The maintenance loop and the HTTPS listener; the node is started.
    void start() {
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
            [name = _cfg.log_name](bool warning, std::string_view m) {
                if (warning) {
                    std::cerr << "[warn] " << name << ": " << m << "\n";
                }
            });
        _server->start();
        std::cerr << "[info] " << _cfg.log_name << " listening: node=" << id_text(_cfg.node_id)
                  << " " << _transport << " http=" << _cfg.http_address << ":" << _server->port()
                  << " replica=" << (_cfg.replica ? "true" : "false") << "\n";
        if (_cfg.admin_uri_sans.empty()) {
            std::cerr << "[warn] " << _cfg.log_name
                      << ": no admin URI SAN given; /v1/admin/* refuses every request\n";
        }
    }

    /// The listener and the maintenance loop; the owner has stopped the
    /// node first.
    void stop() {
        _stopping = true;
        _stop_cv.notify_all();
        if (_server) {
            _server->stop();
        }
        if (_maintenance.joinable()) {
            _maintenance.join();
        }
    }

    /// The HTTPS listener's port once started (tests pass port 0).
    [[nodiscard]] auto http_port() const -> std::uint16_t { return _server ? _server->port() : 0; }

    /// IAM's authorizer over this member's state, and the action tables
    /// it evaluates.
    [[nodiscard]] auto authorizer() -> iam::iam_authorizer& { return _authz; }
    [[nodiscard]] auto registry() -> iam::iam_service_registry& { return _registry; }

    /// Whether the member has checked its KEK against a sealed secret.
    [[nodiscard]] auto unsealed() const -> bool { return _unsealed.load(); }

private:
private:
    /// The IAM Query API's view of the cluster: the leader's applied state
    /// and its proposals. A follower redirects before it gets here.
    struct query_cluster_adapter {
        iam_service* m;
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

    /// AssumeRoleWithWebIdentity's JWKS fetcher (task 19). Without
    /// cpp-httplib's TLS client every fetch fails, which the action
    /// reports as IDPCommunicationError.
    static auto make_fetcher(const iam_node::config& cfg) -> iam::oidc::fetcher {
        iam::oidc::fetch_rules rules;
        rules.allowed_hosts = cfg.oidc_allowed_hosts;
        rules.proxy = cfg.oidc_proxy;
        rules.ca_file = cfg.oidc_ca_file;
        return make_fetcher_with(std::move(rules));
    }

    static auto make_fetcher_with(iam::oidc::fetch_rules rules) -> iam::oidc::fetcher {
#ifdef KYTHIRA_IAM_HAS_HTTPS_FETCHER
        return iam::oidc::https_fetcher(std::move(rules));
#else
        (void)rules;
        return [](const std::string&) {
            return iam::oidc::fetch_result{0, {}, "built without an HTTPS client"};
        };
#endif
    }

    /// The Kythira issuer as IAM names OIDC providers: host/path, no scheme.
    [[nodiscard]] auto issuer_name() const -> std::string {
        return _cfg.kythira_issuer.substr(std::string_view("https://").size());
    }

    /// `POST /v1/iam/credentials` (task 20, Requirement 12.3): role
    /// credentials for the verified client certificate, with no JWT step.
    auto handle_credentials_exchange(const https::request& req, const https::request_context& ctx)
        -> https::response {
        const auto json = [&](unsigned status, std::string body) {
            https::response res{static_cast<http::status>(status), req.version()};
            res.set(http::field::content_type, "application/json");
            res.body() = std::move(body);
            res.keep_alive(req.keep_alive());
            return res;
        };
        if (!_cfg.credentials_exchange) {
            return json(404, R"({"code":"NotFound","message":"The credentials exchange is off."})");
        }
        if (!ctx.tls || !ctx.client_verified) {
            return json(
                403,
                R"({"code":"AccessDenied","message":"A Kythira-CA client certificate is required."})");
        }
        if (!_node->is_leader()) {
            return redirect_to_leader(req);
        }
        const auto form = kythira::aws_query::form::parse(std::string_view(req.body()));
        if (!form) {
            return json(
                400,
                R"({"code":"MalformedQueryString","message":"The form body could not be decoded."})");
        }
        iam::query::request_facts facts;
        facts.request_id = routing::request_id();
        facts.source_ip = ctx.peer_address;
        facts.user_agent = std::string(req[http::field::user_agent]);
        facts.secure_transport = true;
        const auto r = _sts_api.exchange_certificate(
            {ctx.client_uri_sans, ctx.client_dns_sans, ctx.client_serial_hex}, issuer_name(), *form,
            facts);
        auto res = json(r.status, r.body);
        res.set("x-amzn-RequestId", facts.request_id);
        return res;
    }

    static auto make_registry() -> iam::iam_service_registry {
        iam::iam_service_registry r;
        r.add(iam::services::iam_actions());
        r.add(iam::services::sts_actions());
        return r;
    }

    // Makes a registered replica reachable from this leader and, once,
    // admits it as a learner. Safe to repeat: an id already in the
    // configuration is left alone.
    void reach_replica(const iam::replica_record& r, bool throw_on_failure) {
        if (!_replicas.make_reachable) {
            if (throw_on_failure) {
                throw std::invalid_argument("this IAM member takes no read replicas");
            }
            return;
        }
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
                _replicas.make_reachable(*id, r);
                _reachable[r.node_id] = r;
            }
        }
        try {
            _node->add_learner(*id).get();
            std::cerr << "[info] " << _cfg.log_name << ": replica " << r.node_id << " ("
                      << r.rpc_host << ":" << r.rpc_port << ") added as a learner\n";
        } catch (const std::exception& e) {
            if (std::string_view(e.what()).find("already in configuration") !=
                std::string_view::npos) {
                return;
            }
            if (throw_on_failure) {
                throw;
            }
            std::cerr << "[warn] " << _cfg.log_name << ": adding replica " << r.node_id
                      << " as a learner failed, will retry: " << e.what() << "\n";
        }
    }

    // Once a second: check the KEK as soon as the salt exists, and on the
    // leader make every registered replica reachable.
    void maintain() {
        while (!_stopping.load()) {
            check_kek();
            if (_replicas.make_reachable && _node->is_leader()) {
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
            std::unique_lock lock(_stop_mu);
            _stop_cv.wait_for(lock, std::chrono::seconds(1), [this] { return _stopping.load(); });
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
            std::cerr << "[fatal] " << _cfg.log_name
                      << ": the unseal passphrase does not open this cluster's "
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
                    std::cerr << "[info] " << _cfg.log_name << ": account "
                              << boost::json::serialize(r.body.at("account_id"))
                              << " created by admin\n";
                }
            } else if (path == "/v1/admin/sts-keys/rotate" && req.method() == http::verb::post) {
                r = admin::rotate_sts_key(*this, body);
            } else if (path == "/v1/admin/replicas" && req.method() == http::verb::post) {
                if (!_replicas.make_reachable) {
                    return json_reply(req, 400,
                                      {{"error", "InvalidInput"},
                                       {"message", "This IAM member takes no read replicas."}});
                }
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
        return handle_query(req, ctx, false);
    }

    /// STS (task 17): its calls only read state and mint sealed tokens, but
    /// they are served by the leader like IAM so a just-written role or
    /// trust policy is always seen.
    auto handle_sts(const https::request& req, const https::request_context& ctx)
        -> https::response {
        return handle_query(req, ctx, true);
    }

    /// The form of an unsigned request that STS answers unsigned, if `req`
    /// is one.
    static auto unsigned_sts_form(const https::request& req)
        -> std::optional<kythira::aws_query::form> {
        const auto target = std::string_view(req.target());
        const auto q = target.find('?');
        auto form = kythira::aws_query::form::parse(
            req.method() == http::verb::post
                ? std::string_view(req.body())
                : (q == std::string_view::npos ? std::string_view{} : target.substr(q + 1)));
        if (!form || !iam::sts::sts_query_api<query_cluster_adapter>::unsigned_allowed(*form)) {
            return std::nullopt;
        }
        return form;
    }

    auto handle_unsigned_sts(const https::request& req, const https::request_context& ctx,
                             const kythira::aws_query::form& form) -> https::response {
        if (!_node->is_leader()) {
            return redirect_to_leader(req);
        }
        iam::query::request_facts facts;
        facts.request_id = routing::request_id();
        facts.source_ip = ctx.peer_address;
        facts.user_agent = std::string(req[http::field::user_agent]);
        facts.secure_transport = ctx.tls;
        const auto r = _sts_api.handle_unsigned(form, facts);
        https::response res{static_cast<http::status>(r.status), req.version()};
        res.set(http::field::content_type, "text/xml");
        res.set("x-amzn-RequestId", facts.request_id);
        res.body() = r.body;
        res.keep_alive(req.keep_alive());
        return res;
    }

    /// One SigV4-signed Query API request, for IAM or STS: their signing
    /// service names, error XML namespaces and handlers differ.
    auto handle_query(const https::request& req, const https::request_context& ctx, bool sts)
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
        const auto who = _authz.authenticate(sr, {.service = sts ? "sts" : "iam"});
        if (!who) {
            const auto& e = who.error();
            return query_error(req, sts, static_cast<unsigned>(e.http_status), e.code, e.message);
        }
        const auto form = kythira::aws_query::form::parse(
            req.method() == http::verb::post ? std::string_view(req.body()) : sr.raw_query);
        if (!form) {
            return query_error(req, sts, 400, "MalformedQueryString",
                               "The request parameters could not be decoded.");
        }
        iam::query::request_facts facts;
        facts.request_id = routing::request_id();
        facts.source_ip = ctx.peer_address;
        facts.user_agent = std::string(req[http::field::user_agent]);
        facts.secure_transport = ctx.tls;
        const auto r = sts ? _sts_api.handle(*who, *form, facts) : _api.handle(*who, *form, facts);
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
        if (path == "/v1/iam/credentials" && req.method() == http::verb::post) {
            return handle_credentials_exchange(req, ctx);
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
                return handle_sts(req, ctx);
            case routing::route::unknown_service:
                return query_error(req, false, 400, "InvalidAction",
                                   "This endpoint serves the IAM and STS APIs only.");
            case routing::route::unauthenticated:
                // AssumeRoleWithWebIdentity is unsigned: the token is the
                // credential (INV-STS-B7).
                if (auto form = unsigned_sts_form(req)) {
                    return handle_unsigned_sts(req, ctx, *form);
                }
                break;
        }
        return query_error(req, false, 403, "MissingAuthenticationToken",
                           "Request is missing Authentication Token");
    }

    const config& _cfg;
    admin::kek_cache _keks;
    RaftNode* _node;
    replica_hooks _replicas;
    std::string _transport;
    iam::replica_freshness _freshness;
    iam::iam_service_registry _registry;
    iam::iam_authorizer _authz;
    query_cluster_adapter _query_cluster;
    iam::query::iam_query_api<query_cluster_adapter> _api;
    iam::oidc::jwks_cache _jwks;
    iam::sts::sts_query_api<query_cluster_adapter> _sts_api;
    std::unique_ptr<https::server> _server;
    std::thread _maintenance;
    std::atomic<bool> _stopping{false};
    std::mutex _stop_mu;
    std::condition_variable _stop_cv;
    std::atomic<bool> _unsealed{false};
    std::mutex _replicas_mu;
    std::map<std::string, iam::replica_record> _reachable;
    std::set<std::string> _admitted;
};

}  // namespace iam_node
