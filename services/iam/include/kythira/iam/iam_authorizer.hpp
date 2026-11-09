// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file iam_authorizer.hpp
/// @brief IAM's implementation of the core `aws_request_authorizer` concept
///        (`.kiro/specs/iam-compatible-auth/` task 13, Requirement 7).
///
///   - `authenticate` parses the SigV4 claim, finds the access key in the
///     state machine's index, opens its sealed secret, and checks the
///     signature through the signing-key cache. `ASIA` keys (temporary
///     credentials) need task 17's session-token decoder; until it lands
///     they are refused as an unknown token.
///   - `authorize` refuses an action no registered service declares,
///     gathers the principal's policies from the state machine, adds the
///     global condition keys and calls `evaluate()` — the only place a
///     decision is made (Requirement 6.7).
///   - Every decision becomes one audit line on a bounded queue drained by
///     one writer thread, so a slow sink never delays a request; lines that
///     do not fit are counted, not waited for. A line names the principal,
///     key id, action, resource and deciding statement, and never a
///     secret, signature or session token.
///
/// The authorizer reads the state machine through `state_reader`, which the
/// caller implements with whatever lock guards it: `iam_node`'s Raft node,
/// or a front end's replica.

#include <raft/aws_request_authorizer.hpp>
#include <raft/sigv4_verifier.hpp>

#include <kythira/iam/iam_conditions.hpp>
#include <kythira/iam/iam_evaluator.hpp>
#include <kythira/iam/iam_policy.hpp>
#include <kythira/iam/iam_seal.hpp>
#include <kythira/iam/iam_service_registry.hpp>
#include <kythira/iam/iam_state_machine.hpp>

#include <boost/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::iam {

/// Runs `body` with the state machine held stable for reading.
using state_reader = std::function<void(const std::function<void(const iam_state_machine&)>&)>;

/// The audit trail of decisions: a bounded queue and one writer thread.
class audit_log {
public:
    using sink = std::function<void(std::string_view line)>;

    explicit audit_log(sink out, std::size_t capacity = 8192)
        : _out(std::move(out)), _capacity(capacity), _writer([this] { run(); }) {}

    audit_log(const audit_log&) = delete;
    auto operator=(const audit_log&) -> audit_log& = delete;
    audit_log(audit_log&&) = delete;
    auto operator=(audit_log&&) -> audit_log& = delete;

    ~audit_log() {
        {
            const std::lock_guard lock(_mu);
            _stopping = true;
        }
        _cv.notify_one();
        _writer.join();
    }

    /// Queues a line, or counts it as dropped when the queue is full.
    void post(std::string line) {
        {
            const std::lock_guard lock(_mu);
            if (_queue.size() >= _capacity) {
                _dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            _queue.push_back(std::move(line));
        }
        _cv.notify_one();
    }

    /// Lines dropped because the queue was full.
    [[nodiscard]] auto dropped() const noexcept -> std::uint64_t {
        return _dropped.load(std::memory_order_relaxed);
    }

    /// Blocks until every queued line has reached the sink (tests, shutdown).
    void flush() {
        std::unique_lock lock(_mu);
        _idle_cv.wait(lock, [&] { return _queue.empty() && !_writing; });
    }

private:
    void run() {
        std::uint64_t reported = 0;
        std::unique_lock lock(_mu);
        for (;;) {
            _cv.wait(lock, [&] { return _stopping || !_queue.empty(); });
            if (_queue.empty() && _stopping) {
                return;
            }
            auto batch = std::move(_queue);
            _queue.clear();
            _writing = true;
            lock.unlock();
            for (const auto& line : batch) {
                _out(line);
            }
            if (const auto d = dropped(); d != reported) {
                _out(R"({"audit":"dropped","lines":)" + std::to_string(d - reported) + "}");
                reported = d;
            }
            lock.lock();
            _writing = false;
            _idle_cv.notify_all();
        }
    }

    sink _out;
    std::size_t _capacity;
    std::mutex _mu;
    std::condition_variable _cv;
    std::condition_variable _idle_cv;
    std::deque<std::string> _queue;
    bool _stopping = false;
    bool _writing = false;
    std::atomic<std::uint64_t> _dropped{0};
    std::thread _writer;  // last: starts once the rest is built
};

namespace detail {

/// The policies `evaluate()` needs for `who`, with the documents they point
/// at kept alive alongside.
struct gathered {
    std::vector<std::shared_ptr<const policy_document>> keep;
    std::vector<named_policy> identity;
    std::optional<named_policy> boundary;
    std::vector<named_policy> session;
    std::vector<std::pair<std::string, std::string>> tags;  // the entity's own tags
};

inline void add_parsed(gathered& g, std::vector<named_policy>& into, std::string name,
                       std::string_view json, policy_type type) {
    try {
        auto doc = std::make_shared<const policy_document>(
            parse_policy(json, type, {.max_size = std::max<std::size_t>(json.size(), 1)}));
        into.push_back({std::move(name), doc.get()});
        g.keep.push_back(std::move(doc));
    } catch (const policy_error&) {
        // Stored documents were validated when written; one that no longer
        // parses grants nothing rather than failing the whole request.
    }
}

inline void add_managed(gathered& g, std::vector<named_policy>& into, const iam_state_machine& sm,
                        const std::string& arn) {
    if (auto doc = sm.compiled_policy(arn)) {
        into.push_back({arn, doc.get()});
        g.keep.push_back(std::move(doc));
    }
}

template<class Entity>
void add_entity_policies(gathered& g, const iam_state_machine& sm, const Entity& e,
                         std::string_view label) {
    for (const auto& [_, p] : e.inline_policies) {
        add_parsed(g, g.identity, std::string(label) + ":" + p.name, p.document,
                   policy_type::identity);
    }
    for (const auto& arn : e.attached) {
        add_managed(g, g.identity, sm, arn);
    }
}

[[nodiscard]] inline auto gather(const iam_state_machine& sm,
                                 const aws_auth::principal_context& who) -> gathered {
    gathered g;
    const auto add_user = [&](std::string_view name) {
        const auto* u = sm.user(who.account, name);
        if (u == nullptr) {
            return;
        }
        add_entity_policies(g, sm, *u, "user/" + u->name);
        for (const auto& group_name : u->groups) {
            if (const auto* grp = sm.group(who.account, group_name)) {
                add_entity_policies(g, sm, *grp, "group/" + grp->name);
            }
        }
        if (u->boundary) {
            std::vector<named_policy> b;
            add_managed(g, b, sm, *u->boundary);
            if (!b.empty()) {
                g.boundary = b.front();
            }
        }
        g.tags.assign(u->tags.begin(), u->tags.end());
    };
    switch (who.type) {
        case aws_auth::principal_type::root:
            break;
        case aws_auth::principal_type::user:
            add_user(who.username);
            break;
        case aws_auth::principal_type::federated_user: {
            // The federating user's current policies, as AWS applies them.
            const auto slash = who.user_arn.rfind('/');
            add_user(slash == std::string::npos ? who.user_arn : who.user_arn.substr(slash + 1));
            g.tags.clear();
            break;
        }
        case aws_auth::principal_type::assumed_role:
            if (const auto* r = sm.role(who.account, who.role_name)) {
                add_entity_policies(g, sm, *r, "role/" + r->name);
                if (r->boundary) {
                    std::vector<named_policy> b;
                    add_managed(g, b, sm, *r->boundary);
                    if (!b.empty()) {
                        g.boundary = b.front();
                    }
                }
                g.tags.assign(r->tags.begin(), r->tags.end());
            }
            break;
    }
    for (std::size_t i = 0; i < who.session_policies.size(); ++i) {
        add_parsed(g, g.session, "session[" + std::to_string(i) + "]", who.session_policies[i],
                   policy_type::session);
    }
    return g;
}

[[nodiscard]] inline auto to_principal(const aws_auth::principal_context& who) -> principal {
    principal p;
    p.account = who.account;
    p.arn = who.arn;
    switch (who.type) {
        case aws_auth::principal_type::root:
            p.kind = principal_kind::root;
            break;
        case aws_auth::principal_type::user:
            p.kind = principal_kind::user;
            break;
        case aws_auth::principal_type::assumed_role:
            p.kind = principal_kind::role_session;
            p.role_arn = who.role_arn;
            break;
        case aws_auth::principal_type::federated_user:
            p.kind = principal_kind::federated_user;
            p.user_arn = who.user_arn;
            break;
    }
    return p;
}

}  // namespace detail

class iam_authorizer {
public:
    using clock_fn = std::function<std::chrono::system_clock::time_point()>;
    using kek_fn = std::function<const seal_key&(std::string_view salt)>;

    iam_authorizer(state_reader state, kek_fn kek, const iam_service_registry& registry,
                   audit_log* audit = nullptr, clock_fn clock = nullptr)
        : _state(std::move(state)),
          _kek(std::move(kek)),
          _registry(&registry),
          _audit(audit),
          _clock(clock ? std::move(clock)
                       : clock_fn([] { return std::chrono::system_clock::now(); })) {}

    /// Requirement 7.4: on a replica, whether its state is recent enough
    /// to answer from (`replica_freshness::fresh`). Unset, the state is
    /// the leader's own and always is.
    void set_freshness(std::function<bool()> fresh) { _fresh = std::move(fresh); }

    /// Requirement 7.1: who signed `req`, or the AWS error to answer with.
    [[nodiscard]] auto authenticate(const sigv4::request& req, const sigv4::service_rules& rules)
        -> aws_auth::authn_result {
        if (stale()) {
            // Fail closed: a stale replica may still hold a key since
            // deleted or deactivated.
            return sigv4::aws_error{std::string(k_service_unavailable),
                                    "The IAM state on this node is stale; retry shortly.", 503};
        }
        const auto now = std::chrono::floor<std::chrono::seconds>(_clock());
        auto parsed = sigv4::parse(req, rules, now);
        if (!parsed) {
            return parsed.error();
        }
        const auto& c = *parsed;
        if (c.security_token.has_value() || c.access_key_id.starts_with("ASIA")) {
            // Temporary credentials: task 17's session-token decoder.
            return sigv4::invalid_client_token_id(rules.protocol);
        }
        std::optional<aws_auth::principal_context> who;
        std::optional<std::string> secret;
        _state([&](const iam_state_machine& sm) {
            const auto [acct, key] = sm.access_key(c.access_key_id);
            if (acct == nullptr || key == nullptr || !key->active) {
                return;
            }
            secret = open_secret(_kek(sm.kek_salt()), key->sealed_secret, key->id);
            aws_auth::principal_context p;
            p.account = acct->id;
            p.access_key_id = key->id;
            if (key->owner.empty()) {
                p.type = aws_auth::principal_type::root;
                p.arn = "arn:" + sm.config().partition + ":iam::" + acct->id + ":root";
                p.userid = acct->id;
            } else {
                const auto* u = sm.user(acct->id, key->owner);
                if (u == nullptr) {
                    return;
                }
                p.type = aws_auth::principal_type::user;
                p.arn = sm.arn(acct->id, "user", u->path, u->name);
                p.userid = u->id;
                p.username = u->name;
                p.tags.assign(u->tags.begin(), u->tags.end());
            }
            who = std::move(p);
        });
        if (!who || !secret) {
            return sigv4::invalid_client_token_id(rules.protocol);
        }
        const auto key = _keys.get(c.access_key_id, *secret, c.scope);
        OPENSSL_cleanse(secret->data(), secret->size());
        if (!sigv4::verify(c, key)) {
            return sigv4::signature_mismatch(rules.protocol);
        }
        return std::move(*who);
    }

    /// Requirement 7.1-7.2: whether `who` may do `r`.
    [[nodiscard]] auto authorize(const aws_auth::principal_context& who,
                                 const aws_auth::authz_request& r) -> aws_auth::authz_decision {
        aws_auth::authz_decision out;
        if (stale()) {
            out.reason = "the IAM replica is stale";
            record(who, r, out);
            return out;
        }
        if (_registry->find(r.action) == nullptr) {
            out.reason = "the action is not registered by any service";
            record(who, r, out);
            return out;
        }
        evaluation_request req;
        req.who = detail::to_principal(who);
        req.action = r.action;
        req.resource = r.resource;
        req.resource_account = r.resource_account;
        switch (r.resource_kind) {
            case aws_auth::resource_policy_kind::ordinary:
                req.resource_kind = resource_policy_kind::ordinary;
                break;
            case aws_auth::resource_policy_kind::trust:
                req.resource_kind = resource_policy_kind::trust;
                break;
            case aws_auth::resource_policy_kind::key:
                req.resource_kind = resource_policy_kind::key;
                break;
        }
        std::shared_ptr<const policy_document> resource_doc;
        if (r.resource_policy) {
            try {
                resource_doc = std::make_shared<const policy_document>(parse_policy(
                    *r.resource_policy,
                    r.resource_kind == aws_auth::resource_policy_kind::trust
                        ? policy_type::trust
                        : policy_type::resource,
                    {.max_size = std::max<std::size_t>(r.resource_policy->size(), 1)}));
                req.resource_policy = named_policy{"resource", resource_doc.get()};
            } catch (const policy_error&) {
                // An unreadable resource policy grants nothing.
            }
        }
        detail::gathered g;
        _state([&](const iam_state_machine& sm) { g = detail::gather(sm, who); });
        req.identity = std::move(g.identity);
        req.boundary = g.boundary;
        req.session = std::move(g.session);
        req.context = context_for(who, r, g.tags);

        const auto d = evaluate(req);
        out.allowed = d.allowed();
        out.explicit_deny = d.result == decision::outcome::explicit_deny;
        if (d.deciding) {
            out.policy = d.deciding->policy;
            out.sid = d.deciding->sid;
        }
        out.reason = d.reason;
        record(who, r, out);
        return out;
    }

    /// Drop the signing keys derived for a key whose secret or status
    /// changed (the replica's apply loop calls this).
    void forget_key(std::string_view access_key_id) { _keys.invalidate(access_key_id); }

    static constexpr std::string_view k_service_unavailable = "ServiceUnavailable";

private:
    [[nodiscard]] auto stale() const -> bool { return _fresh && !_fresh(); }

    [[nodiscard]] auto context_for(
        const aws_auth::principal_context& who, const aws_auth::authz_request& r,
        const std::vector<std::pair<std::string, std::string>>& entity_tags) const -> context_keys {
        request_facts f;
        f.now = _clock();
        f.secure_transport = r.secure_transport;
        f.source_ip = r.source_ip;
        f.user_agent = r.user_agent;
        f.requested_region = r.region;
        // aws:PrincipalArn names the role, not the session, for a role
        // session (the global condition keys page).
        f.principal_arn =
            who.type == aws_auth::principal_type::assumed_role ? who.role_arn : who.arn;
        f.principal_account = who.account;
        f.principal_type = std::string(aws_auth::to_string(who.type));
        f.userid = who.userid;
        f.username = who.type == aws_auth::principal_type::user ? who.username : std::string{};
        // Session tags override the entity's tags with the same key.
        f.principal_tags = entity_tags;
        for (const auto& [k, v] : who.tags) {
            std::erase_if(f.principal_tags, [&](const auto& t) {
                return detail::ascii_lower(t.first) == detail::ascii_lower(k);
            });
            f.principal_tags.emplace_back(k, v);
        }
        f.request_tags = r.request_tags;
        f.resource_tags = r.resource_tags;
        f.resource_account = r.resource_account.empty() ? who.account : r.resource_account;
        f.token_issue_time = who.token_issue_time;
        f.source_identity = who.source_identity;
        auto ctx = global_condition_keys(f);
        for (const auto& [key, values] : r.keys) {
            if (values.size() == 1) {
                ctx.set(key, values.front());
            } else {
                ctx.set_multi(key, values);
            }
        }
        return ctx;
    }

    void record(const aws_auth::principal_context& who, const aws_auth::authz_request& r,
                const aws_auth::authz_decision& d) {
        if (_audit == nullptr) {
            return;
        }
        boost::json::object line{
            {"time",
             std::chrono::duration_cast<std::chrono::milliseconds>(_clock().time_since_epoch())
                 .count()},
            {"principal", who.arn},
            {"account", who.account},
            {"access_key_id", who.access_key_id},
            {"action", r.action},
            {"resource", r.resource},
            {"decision",
             d.allowed ? "allow" : (d.explicit_deny ? "explicit_deny" : "implicit_deny")},
            {"reason", d.reason},
        };
        if (!d.policy.empty()) {
            line["policy"] = d.policy;
        }
        if (!d.sid.empty()) {
            line["sid"] = d.sid;
        }
        if (!r.source_ip.empty()) {
            line["source_ip"] = r.source_ip;
        }
        _audit->post(boost::json::serialize(line));
    }

    state_reader _state;
    kek_fn _kek;
    const iam_service_registry* _registry;
    audit_log* _audit;
    clock_fn _clock;
    std::function<bool()> _fresh;
    sigv4::signing_key_cache _keys;
};

static_assert(aws_auth::aws_request_authorizer<iam_authorizer>);

}  // namespace kythira::iam
