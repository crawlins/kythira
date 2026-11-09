// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The native ledger API (.kiro/specs/verifiable-ledger/ task 7,
// Requirements 8.1 and 8.2), independent of the HTTP server and of Raft:
// ledger_api<Backend>::handle() maps one request to one response, so the
// daemon serves it over HTTPS with mTLS and the tests call it directly.
//
// Bodies are JSON, or Ion with Content-Type application/ion; JSON is Ion
// text, so both parse the same way and documents keep whatever types the
// client wrote. Responses are JSON (hashes as base64, Ion types
// down-converted), or Ion binary when the request accepts application/ion
// (use Ion to re-hash anything you read).
//
//   GET    /v1/ledgers                                    read  (any ledger)
//   POST   /v1/ledgers               {name, deletionProtection?}   admin on *
//   GET    /v1/ledgers/{L}                                read
//   PATCH  /v1/ledgers/{L}           {deletionProtection}          admin
//   DELETE /v1/ledgers/{L}                                admin
//   POST   /v1/ledgers/{L}/execute   {statements: [...]}           write
//   POST   /v1/ledgers/{L}/sessions                       write -> {token, snapshot}
//   POST   /v1/ledgers/{L}/sessions/{T}/get       {table, id}      write
//   POST   /v1/ledgers/{L}/sessions/{T}/scan      {table, after?, limit?}
//   POST   /v1/ledgers/{L}/sessions/{T}/lookup    {table, index, value, after?, limit?}
//   POST   /v1/ledgers/{L}/sessions/{T}/statements {statements: [...]}
//   POST   /v1/ledgers/{L}/sessions/{T}/insert    {table, value} -> {id}
//   POST   /v1/ledgers/{L}/sessions/{T}/commit
//   DELETE /v1/ledgers/{L}/sessions/{T}
//   GET    /v1/ledgers/{L}/tables                         read
//   GET    /v1/ledgers/{L}/tables/{t}/indexes             read
//   GET    /v1/ledgers/{L}/tables/{t}/documents/{id}      read
//   GET    /v1/ledgers/{L}/tables/{t}/documents?after=&limit=   read (a scan)
//   GET    /v1/ledgers/{L}/history/{t}[/{id}]?start=&end=&after=&limit=  read
//   GET    /v1/ledgers/{L}/digest                         read (signed, when a
//                                                          signing key is set)
//   GET    /v1/ledgers/{L}/blocks/{seq}?tip=              read (with proof)
//   GET    /v1/ledgers/{L}/revisions/{t}/{id}/{version}?tip=   read (with proof)
//   POST   /v1/ledgers/{L}/exports   {start, end, format, bucket, prefix}  admin
//   GET    /v1/ledgers/{L}/exports[/{job}]                read
//
// Tables and indexes are created with statements (create_table,
// create_index). Errors are {code, message, retryable}; the codes are the
// state machine's and the session manager's, with HTTP statuses 400, 403,
// 404, 409 (conflicts; retryable ones say so), 429, 503 (not the leader)
// and 507 (insufficient_storage).
//
// A commit holds the ledger's commit lock from prepare until its entry
// applies, so one transaction per ledger is in flight and prepare's block
// hash still matches when it applies; a stale_block (a leader change in
// between) re-prepares, up to three times.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/journal_export.hpp>
#include <kythira/ledger/journal_signing.hpp>
#include <kythira/ledger/journal_types.hpp>
#include <kythira/ledger/journal_verifier.hpp>
#include <kythira/ledger/ledger_catalog.hpp>
#include <kythira/ledger/ledger_sessions.hpp>
#include <kythira/ledger/ledger_state_machine.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// Milliseconds of an Ion timestamp written as text, at day, minute,
/// second or fractional precision with Z or an offset
/// (2026-11-09, 2026-11-09T12:00Z, 2026-11-09T12:00:00.5+01:00).
/// Fractions beyond milliseconds are truncated.
inline auto any_timestamp(std::string_view t) -> std::optional<std::int64_t> {
    if (auto exact = parse_timestamp(t)) {
        return exact;
    }
    std::size_t at = 0;
    const auto digits = [&](std::size_t n) -> std::optional<int> {
        if (at + n > t.size()) {
            return std::nullopt;
        }
        int v = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = t[at + i];
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            v = (v * 10) + (c - '0');
        }
        at += n;
        return v;
    };
    const auto expect = [&](char c) {
        if (at < t.size() && t[at] == c) {
            ++at;
            return true;
        }
        return false;
    };
    const auto y = digits(4);
    if (!y || !expect('-')) {
        return std::nullopt;
    }
    const auto mo = digits(2);
    if (!mo || !expect('-')) {
        return std::nullopt;
    }
    const auto d = digits(2);
    if (!d) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const year_month_day ymd{year{*y}, month{static_cast<unsigned>(*mo)},
                             day{static_cast<unsigned>(*d)}};
    if (!ymd.ok()) {
        return std::nullopt;
    }
    milliseconds total = sys_days{ymd}.time_since_epoch();
    if (at == t.size() || (t[at] == 'T' && at + 1 == t.size())) {
        return total.count();
    }
    if (!expect('T')) {
        return std::nullopt;
    }
    const auto h = digits(2);
    if (!h || *h > 23 || !expect(':')) {
        return std::nullopt;
    }
    const auto mi = digits(2);
    if (!mi || *mi > 59) {
        return std::nullopt;
    }
    total += hours{*h} + minutes{*mi};
    if (expect(':')) {
        const auto sec = digits(2);
        if (!sec || *sec > 59) {
            return std::nullopt;
        }
        total += seconds{*sec};
        if (expect('.')) {
            int ms = 0;
            int scale = 100;
            const std::size_t start = at;
            while (at < t.size() && t[at] >= '0' && t[at] <= '9') {
                ms += (t[at] - '0') * scale;
                scale /= 10;
                ++at;
            }
            if (at == start) {
                return std::nullopt;
            }
            total += milliseconds{ms};
        }
    }
    if (expect('Z')) {
        return at == t.size() ? std::optional(total.count()) : std::nullopt;
    }
    const bool minus = at < t.size() && t[at] == '-';
    if (!expect('+') && !expect('-')) {
        return std::nullopt;
    }
    const auto oh = digits(2);
    if (!oh || !expect(':')) {
        return std::nullopt;
    }
    const auto om = digits(2);
    if (!om || at != t.size()) {
        return std::nullopt;
    }
    const auto offset = hours{*oh} + minutes{*om};
    total += minus ? milliseconds(offset) : -milliseconds(offset);
    return total.count();
}

/// An Ion timestamp value at any precision, as milliseconds.
inline auto timestamp_value_millis(const ion_value& v) -> std::optional<std::int64_t> {
    const auto text = v.as_timestamp_text();
    return text ? any_timestamp(*text) : std::nullopt;
}

/// One API request, as the transport hands it over.
struct api_request {
    std::string method;
    std::string path;                          // without the query string
    std::map<std::string, std::string> query;  // decoded
    std::string body;
    bool body_is_ion = false;  // Content-Type application/ion
    bool want_ion = false;     // Accept application/ion
    std::string principal;     // the client certificate's subject
};

struct api_response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

enum class ledger_role : std::uint8_t {
    read,
    write,
    admin
};

/// Per-ledger roles by certificate subject (Requirement 8.2). A grant on
/// "*" covers every ledger; admin implies write implies read.
class ledger_roles {
public:
    auto grant(std::string subject, std::string ledger, ledger_role role) -> ledger_roles& {
        grants_[std::move(subject)][std::move(ledger)] = role;
        return *this;
    }

    [[nodiscard]] auto allows(const std::string& subject, const std::string& ledger,
                              ledger_role needed) const -> bool {
        const auto it = grants_.find(subject);
        if (it == grants_.end()) {
            return false;
        }
        for (const auto& key : {ledger, std::string("*")}) {
            if (const auto g = it->second.find(key);
                g != it->second.end() && static_cast<int>(g->second) >= static_cast<int>(needed)) {
                return true;
            }
        }
        return false;
    }

private:
    std::map<std::string, std::map<std::string, ledger_role>> grants_;
};

/// The request reached a member that is not the leader of the group it
/// needs; `leader` is a URL to retry at, when one is known.
class ledger_not_leader : public std::runtime_error {
public:
    explicit ledger_not_leader(std::optional<std::string> leader)
        : std::runtime_error("not the leader"), leader_(std::move(leader)) {}
    [[nodiscard]] auto leader() const -> const std::optional<std::string>& { return leader_; }

private:
    std::optional<std::string> leader_;
};

/// The group could not confirm an entry: it timed out, or leadership moved
/// while it was in flight. The entry may still commit, so the request is
/// not safe to resend blindly (`retryable` false) unless it was a read.
class ledger_unavailable : public std::runtime_error {
public:
    ledger_unavailable(std::string code, const std::string& message, bool retryable)
        : std::runtime_error(message), code_(std::move(code)), retryable_(retryable) {}
    [[nodiscard]] auto code() const -> const std::string& { return code_; }
    [[nodiscard]] auto retryable() const -> bool { return retryable_; }

private:
    std::string code_;
    bool retryable_;
};

/// What the API needs of one ledger hosted (and led) here.
template<typename StateMachine> struct ledger_handle {
    const StateMachine* sm = nullptr;
    ledger_session_manager<StateMachine>* sessions = nullptr;
    /// Propose an entry to the ledger's group and return its apply result
    /// once applied here; throws ledger_not_leader.
    std::function<std::vector<std::byte>(std::vector<std::byte>)> propose;
    std::mutex* commit_mutex = nullptr;
    /// Run an export job that has just been recorded (in the background;
    /// the runner proposes its finish entry).
    std::function<void(const export_job&)> start_export;
    /// The ledger group's current term, when it has its own; unset means
    /// the backend's term().
    std::function<std::uint64_t()> term;
};

/// Backend requirements:
///   b.catalog()                    -> const basic_ledger_catalog<E>&
///   b.propose_catalog(bytes)       -> bytes (the catalog's apply result)
///   b.open(record)                 -> std::optional<ledger_handle<SM>>
///                                     (nullopt: not hosted here)
///   b.term()                       -> std::uint64_t (this leader's term;
///                                     a handle's own term() wins)
///   b.now_ms()                     -> std::int64_t (wall clock)
/// and optionally
///   b.place_ledger(name)           -> std::vector<std::string> (the voters
///                                     a new ledger's group is placed on)
///   b.confirm_missing(name)        -> void; throws ledger_not_leader when
///                                     only the catalog leader can say a
///                                     ledger does not exist
template<typename Backend, typename StateMachine> class ledger_api {
public:
    ledger_api(Backend& backend, ledger_roles roles)
        : backend_(&backend), roles_(std::move(roles)) {}

    /// Sign every digest GetDigest returns (Requirement 4.5). Off (null)
    /// unless the node is configured with a CA-issued signing key.
    auto set_digest_signer(std::shared_ptr<const digest_signer> signer) -> void {
        signer_ = std::move(signer);
    }

    auto handle(const api_request& req) -> api_response {
        try {
            return route(req);
        } catch (const api_error& e) {
            return error(e.status, e.code, e.what(), e.retryable);
        } catch (const ledger_not_leader& e) {
            api_response r = error(503, "not_leader", "this member is not the leader", true);
            if (e.leader()) {
                r.body = error_body("not_leader", "retry at the leader", true)
                             .set("leader", ion_value::string(*e.leader()))
                             .to_json();
            }
            return r;
        } catch (const ledger_unavailable& e) {
            return error(503, e.code(), e.what(), e.retryable());
        } catch (const ledger_session_error& e) {
            return error(status_for(e.code()), e.code(), e.what(), e.retryable());
        } catch (const ledger_query_error& e) {
            return error(404, "not_found", e.what(), false);
        } catch (const ledger_segment_error& e) {
            return error(500, "segment_unreadable", e.what(), true);
        } catch (const ledger_format_error& e) {
            return error(400, "invalid_request", e.what(), false);
        } catch (const ion_value_error& e) {
            return error(400, "invalid_request", e.what(), false);
        }
    }

    /// The HTTP status for a rejection code.
    static auto status_for(std::string_view code) -> int {
        static const std::set<std::string_view> not_found{
            "table_not_found",  "document_not_found", "ledger_not_found", "index_not_found",
            "export_not_found", "segment_not_found",  "not_found"};
        static const std::set<std::string_view> conflict{
            "occ_conflict",  "stale_block",     "session_expired",    "version_mismatch",
            "table_exists",  "document_exists", "index_exists",       "ledger_exists",
            "table_active",  "table_inactive",  "deletion_protected", "ledger_deleted",
            "ledger_active", "export_exists",   "export_finished",    "index_limit"};
        if (not_found.contains(code)) {
            return 404;
        }
        if (conflict.contains(code)) {
            return 409;
        }
        if (code == "too_many_sessions") {
            return 429;
        }
        if (code == "insufficient_storage") {
            return 507;
        }
        if (code == "forbidden") {
            return 403;
        }
        return 400;
    }

private:
    struct api_error : std::runtime_error {
        api_error(int s, std::string c, const std::string& m, bool r = false)
            : std::runtime_error(m), status(s), code(std::move(c)), retryable(r) {}
        int status;
        std::string code;
        bool retryable;
    };

    // ── Routing ─────────────────────────────────────────────────────────────

    static auto split(std::string_view path) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at < path.size()) {
            if (path[at] == '/') {
                ++at;
                continue;
            }
            const auto end = path.find('/', at);
            out.emplace_back(path.substr(at, end == std::string_view::npos ? end : end - at));
            at = end == std::string_view::npos ? path.size() : end;
        }
        return out;
    }

    auto route(const api_request& req) -> api_response {
        const auto p = split(req.path);
        if (p.size() < 2 || p[0] != "v1" || p[1] != "ledgers") {
            throw api_error(404, "not_found", "no such route");
        }
        const auto& m = req.method;
        if (p.size() == 2) {
            if (m == "GET") {
                return list_ledgers(req);
            }
            if (m == "POST") {
                return create_ledger(req);
            }
            throw method_not_allowed();
        }
        const std::string& name = p[2];
        if (p.size() == 3) {
            if (m == "GET") {
                return ok(record_for(req, name, ledger_role::read).to_ion(), req);
            }
            if (m == "PATCH") {
                return update_ledger(req, name);
            }
            if (m == "DELETE") {
                return delete_ledger(req, name);
            }
            throw method_not_allowed();
        }
        const std::string& what = p[3];
        if (what == "execute" && p.size() == 4 && m == "POST") {
            return execute(req, name);
        }
        if (what == "sessions") {
            return sessions(req, name, p);
        }
        if (m != "GET" && what != "exports") {
            throw method_not_allowed();
        }
        if (what == "tables") {
            return tables(req, name, p);
        }
        if (what == "history" && (p.size() == 5 || p.size() == 6)) {
            return history(req, name, p);
        }
        if (what == "digest" && p.size() == 4) {
            return digest(req, name);
        }
        if (what == "blocks" && p.size() == 5) {
            return block_proof(req, name, p[4]);
        }
        if (what == "revisions" && p.size() == 7) {
            return revision_proof(req, name, p);
        }
        if (what == "exports") {
            return exports(req, name, p);
        }
        throw api_error(404, "not_found", "no such route");
    }

    static auto method_not_allowed() -> api_error {
        return {405, "method_not_allowed", "method not allowed on this route"};
    }

    // ── Catalog ─────────────────────────────────────────────────────────────

    auto list_ledgers(const api_request& req) -> api_response {
        ion_value list = ion_value::make_list();
        for (const auto& r : backend_->catalog().list()) {
            if (roles_.allows(req.principal, r.name, ledger_role::read)) {
                list.push_back(r.to_ion());
            }
        }
        ion_value v = ion_value::make_struct();
        v.set("ledgers", std::move(list));
        return ok(v, req);
    }

    auto create_ledger(const api_request& req) -> api_response {
        require(req, "*", ledger_role::admin);
        const ion_value body = parse_body(req);
        const auto name = text_arg(body, "name");
        bool protection = true;
        if (const ion_value* d = body.find("deletionProtection"); d != nullptr) {
            protection = d->as_bool().value_or(true);
        }
        if (!valid_ledger_name(name)) {
            throw api_error(400, "invalid_request",
                            "a ledger name is 1-32 letters, digits and hyphens, from a letter");
        }
        std::vector<std::string> voters;
        if constexpr (requires { backend_->place_ledger(name); }) {
            voters = backend_->place_ledger(name);
        }
        return catalog_change(
            req, catalog_entries::create(name, new_id(), protection, backend_->now_ms(), voters),
            201);
    }

    auto update_ledger(const api_request& req, const std::string& name) -> api_response {
        require(req, name, ledger_role::admin);
        const ion_value body = parse_body(req);
        const ion_value* d = body.find("deletionProtection");
        if (d == nullptr || !d->as_bool()) {
            throw api_error(400, "invalid_request", "deletionProtection is a bool");
        }
        return catalog_change(req, catalog_entries::update(name, *d->as_bool()), 200);
    }

    auto delete_ledger(const api_request& req, const std::string& name) -> api_response {
        require(req, name, ledger_role::admin);
        return catalog_change(req, catalog_entries::remove(name, backend_->now_ms()), 200);
    }

    auto catalog_change(const api_request& req, std::vector<std::byte> entry, int status)
        -> api_response {
        const auto result = catalog_result::decode(backend_->propose_catalog(std::move(entry)));
        if (!result.ok()) {
            throw api_error(status_for(result.code), result.code, result.message);
        }
        api_response r = ok(result.record->to_ion(), req);
        r.status = status;
        return r;
    }

    auto record_for(const api_request& req, const std::string& name, ledger_role role)
        -> ledger_record {
        require(req, name, role);
        auto r = backend_->catalog().get(name);
        if (!r || r->state == "purged") {
            // A catalog replica that is not the leader may not have applied
            // a create the client has already seen succeed: such a backend
            // sends the request to the catalog leader instead.
            if constexpr (requires { backend_->confirm_missing(name); }) {
                backend_->confirm_missing(name);
            }
            throw api_error(404, "ledger_not_found", "no ledger " + name);
        }
        return *r;
    }

    /// The term sessions are tied to: the ledger group's when the backend
    /// gives one (a multi-group node), else the backend's.
    auto term_of(const ledger_handle<StateMachine>& h) -> std::uint64_t {
        return h.term ? h.term() : backend_->term();
    }

    /// An active ledger's handle, for data routes.
    auto handle_for(const api_request& req, const std::string& name, ledger_role role)
        -> ledger_handle<StateMachine> {
        const auto r = record_for(req, name, role);
        if (r.state != "active") {
            throw api_error(409, "ledger_deleted", "ledger " + name + " is deleted");
        }
        auto h = backend_->open(r);
        if (!h) {
            throw ledger_not_leader(std::nullopt);
        }
        return std::move(*h);
    }

    auto require(const api_request& req, const std::string& ledger, ledger_role role) -> void {
        if (!roles_.allows(req.principal, ledger, role)) {
            throw api_error(403, "forbidden",
                            "the client certificate has no " + role_name(role) + " role on " +
                                (ledger == "*" ? std::string("all ledgers") : ledger));
        }
    }

    static auto role_name(ledger_role r) -> std::string {
        switch (r) {
            case ledger_role::read:
                return "read";
            case ledger_role::write:
                return "write";
            case ledger_role::admin:
                return "admin";
        }
        return "";
    }

    // ── Writes ──────────────────────────────────────────────────────────────

    auto execute(const api_request& req, const std::string& name) -> api_response {
        auto h = handle_for(req, name, ledger_role::write);
        const ion_value body = parse_body(req);
        const auto pending = h.sessions->execute_pending(term_of(h), statement_list(body));
        return commit_result(req, commit(h, pending));
    }

    auto commit(ledger_handle<StateMachine>& h, const pending_transaction& pending)
        -> apply_result {
        const std::lock_guard lock(*h.commit_mutex);
        apply_result last;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto prepared = pending.prepare(*h.sm, backend_->now_ms());
            if (!prepared.entry) {
                return prepared.rejection;
            }
            last = apply_result::decode(h.propose(prepared.command));
            if (last.code != "stale_block") {
                return last;
            }
        }
        return last;
    }

    auto commit_result(const api_request& req, const apply_result& r) -> api_response {
        if (r.committed) {
            return ok(r.to_ion(), req);
        }
        api_response out;
        out.status = status_for(r.code);
        ion_value v = error_body(r.code, r.message, retryable_rejection(r.code));
        if (r.statement) {
            v.set("statement", ion_value::integer(static_cast<std::int64_t>(*r.statement)));
        }
        out.body = render(v, req.want_ion);
        out.content_type = content_type(req.want_ion);
        return out;
    }

    auto sessions(const api_request& req, const std::string& name,
                  const std::vector<std::string>& p) -> api_response {
        auto h = handle_for(req, name, ledger_role::write);
        if (p.size() == 4) {
            if (req.method != "POST") {
                throw method_not_allowed();
            }
            const auto token = h.sessions->begin(term_of(h));
            ion_value v = ion_value::make_struct();
            v.set("token", ion_value::string(token))
                .set("snapshot",
                     ion_value::integer(static_cast<std::int64_t>(h.sessions->snapshot(token))));
            api_response r = ok(v, req);
            r.status = 201;
            return r;
        }
        const std::string& token = p[4];
        if (p.size() == 5) {
            if (req.method != "DELETE") {
                throw method_not_allowed();
            }
            h.sessions->abort(token);
            return ok(ion_value::make_struct(), req);
        }
        if (p.size() != 6 || req.method != "POST") {
            throw api_error(404, "not_found", "no such route");
        }
        const std::string& op = p[5];
        if (op == "commit") {
            return commit_result(req, commit(h, h.sessions->finish(token)));
        }
        const ion_value body = parse_body(req);
        if (op == "get") {
            const auto r = h.sessions->get(token, text_arg(body, "table"), text_arg(body, "id"));
            ion_value v = ion_value::make_struct();
            if (r) {
                v.set("document", r->to_ion());
            }
            return ok(v, req);
        }
        if (op == "scan") {
            return ok(page_ion(h.sessions->scan(token, text_arg(body, "table"),
                                                optional_text(body, "after"), limit_arg(body))),
                      req);
        }
        if (op == "lookup") {
            const ion_value* value = body.find("value");
            if (value == nullptr) {
                throw api_error(400, "invalid_request", "lookup needs a value");
            }
            return ok(
                page_ion(h.sessions->lookup(token, text_arg(body, "table"), text_arg(body, "index"),
                                            *value, optional_text(body, "after"), limit_arg(body))),
                req);
        }
        if (op == "statements") {
            for (const auto& st : statement_list(body)) {
                h.sessions->add(token, st);
            }
            return ok(ion_value::make_struct(), req);
        }
        if (op == "insert") {
            const ion_value* value = body.find("value");
            if (value == nullptr) {
                throw api_error(400, "invalid_request", "insert needs a value");
            }
            const auto id = h.sessions->insert(token, text_arg(body, "table"), *value);
            ion_value v = ion_value::make_struct();
            v.set("id", ion_value::string(id));
            return ok(v, req);
        }
        throw api_error(404, "not_found", "no such session operation");
    }

    // ── Reads ───────────────────────────────────────────────────────────────

    auto tables(const api_request& req, const std::string& name, const std::vector<std::string>& p)
        -> api_response {
        auto h = handle_for(req, name, ledger_role::read);
        if (p.size() == 4) {
            ion_value list = ion_value::make_list();
            for (const auto& t : h.sm->tables()) {
                ion_value v = ion_value::make_struct();
                v.set("name", ion_value::string(t.name))
                    .set("active", ion_value::boolean(t.active));
                list.push_back(std::move(v));
            }
            ion_value v = ion_value::make_struct();
            v.set("tables", std::move(list));
            return ok(v, req);
        }
        const std::string& table = p[4];
        if (p.size() == 6 && p[5] == "indexes") {
            ion_value list = ion_value::make_list();
            for (const auto& i : h.sm->indexes(table)) {
                ion_value v = ion_value::make_struct();
                v.set("name", ion_value::string(i.name))
                    .set("path", journal_detail::path_to_ion(i.path))
                    .set("active", ion_value::boolean(i.active));
                list.push_back(std::move(v));
            }
            ion_value v = ion_value::make_struct();
            v.set("indexes", std::move(list));
            return ok(v, req);
        }
        if (p.size() == 6 && p[5] == "documents") {
            scan_options o;
            o.after = query_text(req, "after");
            o.limit = query_limit(req);
            return ok(page_ion(h.sm->scan_table_at(table, h.sm->block_count(), o)), req);
        }
        if (p.size() == 7 && p[5] == "documents") {
            const auto r = h.sm->get_document(table, p[6]);
            if (!r) {
                throw api_error(404, "document_not_found", "no document " + table + "/" + p[6]);
            }
            return ok(r->to_ion(), req);
        }
        throw api_error(404, "not_found", "no such route");
    }

    auto history(const api_request& req, const std::string& name, const std::vector<std::string>& p)
        -> api_response {
        auto h = handle_for(req, name, ledger_role::read);
        const auto page = h.sm->history(p[4], p.size() == 6 ? std::optional(p[5]) : std::nullopt,
                                        query_time(req, "start"), query_time(req, "end"),
                                        query_text(req, "after"), query_limit(req));
        ion_value list = ion_value::make_list();
        for (const auto& r : page.revisions) {
            list.push_back(r.to_ion());
        }
        ion_value v = ion_value::make_struct();
        v.set("revisions", std::move(list));
        if (page.next) {
            v.set("next", ion_value::string(*page.next));
        }
        return ok(v, req);
    }

    auto digest(const api_request& req, const std::string& name) -> api_response {
        auto h = handle_for(req, name, ledger_role::read);
        const auto d = h.sm->get_digest();
        if (!d) {
            throw api_error(404, "not_found", "the ledger has no blocks yet");
        }
        const saved_digest saved{d->tip, d->digest};
        if (signer_) {
            return ok(signer_->sign(name, saved, backend_->now_ms()).to_ion(), req);
        }
        return ok(saved.to_ion(), req);
    }

    auto block_proof(const api_request& req, const std::string& name, const std::string& seq)
        -> api_response {
        auto h = handle_for(req, name, ledger_role::read);
        const auto n = number(seq, "block sequence");
        const auto tip = query_number(req, "tip").value_or(h.sm->block_count() - 1);
        auto p = h.sm->get_block_proof(n, tip);
        return ok(proof_document{std::move(p.blk), std::nullopt, std::move(p.proof), p.digest, tip}
                      .to_ion(),
                  req);
    }

    auto revision_proof(const api_request& req, const std::string& name,
                        const std::vector<std::string>& p) -> api_response {
        auto h = handle_for(req, name, ledger_role::read);
        const auto version = static_cast<std::int64_t>(number(p[6], "version"));
        const auto tip = query_number(req, "tip").value_or(h.sm->block_count() - 1);
        auto r = h.sm->get_revision_proof(p[4], p[5], version, tip);
        if (!r) {
            throw api_error(404, "document_not_found", "no such revision");
        }
        return ok(
            proof_document{std::nullopt, std::move(r->rev), std::move(r->proof), r->digest, tip}
                .to_ion(),
            req);
    }

    auto exports(const api_request& req, const std::string& name, const std::vector<std::string>& p)
        -> api_response {
        if (req.method == "POST" && p.size() == 4) {
            auto h = handle_for(req, name, ledger_role::admin);
            const ion_value body = parse_body(req);
            export_job job;
            job.job_id = new_id();
            job.start_ms = time_arg(body, "start");
            job.end_ms = time_arg(body, "end");
            const auto f = export_format_from(text_arg(body, "format"));
            if (!f) {
                throw api_error(400, "invalid_request", "format is ion_text, ion_binary or json");
            }
            job.format = *f;
            job.bucket = text_arg(body, "bucket");
            job.prefix = optional_text(body, "prefix").value_or("");
            const auto out = h.propose([&] {
                const auto bytes = encode_export_start(job);
                std::vector<std::byte> cmd(bytes.size());
                std::memcpy(cmd.data(), bytes.data(), bytes.size());
                return cmd;
            }());
            const ion_value result = ion_value::parse(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(out.data()), out.size()));
            if (const ion_value* s = result.find("status");
                s == nullptr || s->as_text().value_or("") != "export_started") {
                const auto rejected = apply_result::decode(out);
                throw api_error(status_for(rejected.code), rejected.code, rejected.message);
            }
            const auto recorded = h.sm->get_export(job.job_id);
            if (recorded && h.start_export) {
                h.start_export(*recorded);
            }
            api_response r = ok(recorded ? recorded->to_ion() : job.to_ion(), req);
            r.status = 202;
            return r;
        }
        if (req.method != "GET") {
            throw method_not_allowed();
        }
        auto h = handle_for(req, name, ledger_role::read);
        if (p.size() == 5) {
            const auto job = h.sm->get_export(p[4]);
            if (!job) {
                throw api_error(404, "export_not_found", "no export job " + p[4]);
            }
            return ok(job->to_ion(), req);
        }
        ion_value list = ion_value::make_list();
        for (const auto& j : h.sm->exports()) {
            list.push_back(j.to_ion());
        }
        ion_value v = ion_value::make_struct();
        v.set("exports", std::move(list));
        return ok(v, req);
    }

    // ── Arguments ───────────────────────────────────────────────────────────

    static auto parse_body(const api_request& req) -> ion_value {
        if (req.body.empty()) {
            return ion_value::make_struct();
        }
        ion_value v =
            req.body_is_ion
                ? ion_value::parse(std::span<const std::uint8_t>(
                      reinterpret_cast<const std::uint8_t*>(req.body.data()), req.body.size()))
                : ion_value::parse(std::string_view(req.body));
        if (!v.is_struct()) {
            throw api_error(400, "invalid_request", "the body is an object");
        }
        return v;
    }

    static auto text_arg(const ion_value& body, std::string_view name) -> std::string {
        const ion_value* v = body.find(name);
        if (v == nullptr || !v->as_text()) {
            throw api_error(400, "invalid_request", std::string(name) + " is a string");
        }
        return *v->as_text();
    }

    static auto optional_text(const ion_value& body, std::string_view name)
        -> std::optional<std::string> {
        const ion_value* v = body.find(name);
        return v == nullptr ? std::nullopt : v->as_text();
    }

    static auto limit_arg(const ion_value& body) -> std::size_t {
        const ion_value* v = body.find("limit");
        if (v == nullptr) {
            return 100;
        }
        const auto n = v->as_int64();
        if (!n || *n < 1 || *n > 1000) {
            throw api_error(400, "invalid_request", "limit is 1-1000");
        }
        return static_cast<std::size_t>(*n);
    }

    static auto time_arg(const ion_value& body, std::string_view name) -> std::int64_t {
        const ion_value* v = body.find(name);
        if (v == nullptr) {
            throw api_error(400, "invalid_request", std::string(name) + " is a timestamp");
        }
        if (auto t = timestamp_value_millis(*v)) {
            return *t;
        }
        if (auto text = v->as_text()) {  // JSON has no timestamps: a string
            if (auto t = any_timestamp(*text)) {
                return *t;
            }
        }
        throw api_error(400, "invalid_request", std::string(name) + " is a timestamp");
    }

    static auto statement_list(const ion_value& body) -> std::vector<ion_value> {
        const ion_value* list = body.find("statements");
        if (list == nullptr || !list->is_sequence() || list->items().empty()) {
            throw api_error(400, "invalid_request", "statements is a non-empty list");
        }
        for (const auto& st : list->items()) {
            (void)parse_statement(st);  // shape errors before anything is buffered
        }
        return list->items();
    }

    static auto query_text(const api_request& req, const std::string& name)
        -> std::optional<std::string> {
        const auto it = req.query.find(name);
        return it == req.query.end() ? std::nullopt : std::optional(it->second);
    }

    static auto number(const std::string& text, std::string_view what) -> std::uint64_t {
        std::uint64_t n = 0;
        if (text.empty() || text.size() > 19 ||
            !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
            throw api_error(400, "invalid_request", std::string(what) + " is a number");
        }
        for (const char c : text) {
            n = (n * 10) + static_cast<std::uint64_t>(c - '0');
        }
        return n;
    }

    static auto query_number(const api_request& req, const std::string& name)
        -> std::optional<std::uint64_t> {
        const auto t = query_text(req, name);
        return t ? std::optional(number(*t, name)) : std::nullopt;
    }

    static auto query_limit(const api_request& req) -> std::size_t {
        const auto n = query_number(req, "limit").value_or(100);
        if (n < 1 || n > 1000) {
            throw api_error(400, "invalid_request", "limit is 1-1000");
        }
        return static_cast<std::size_t>(n);
    }

    static auto query_time(const api_request& req, const std::string& name)
        -> std::optional<std::int64_t> {
        const auto t = query_text(req, name);
        if (!t) {
            return std::nullopt;
        }
        if (auto ms = any_timestamp(*t)) {
            return ms;
        }
        throw api_error(400, "invalid_request", name + " is a timestamp");
    }

    // ── Responses ───────────────────────────────────────────────────────────

    static auto page_ion(const scan_page& page) -> ion_value {
        ion_value list = ion_value::make_list();
        for (const auto& r : page.documents) {
            list.push_back(r.to_ion());
        }
        ion_value v = ion_value::make_struct();
        v.set("documents", std::move(list));
        if (page.next) {
            v.set("next", ion_value::string(*page.next));
        }
        return v;
    }

    static auto content_type(bool ion) -> std::string {
        return ion ? "application/ion" : "application/json";
    }

    static auto render(const ion_value& v, bool ion) -> std::string {
        if (ion) {
            const auto bytes = v.to_binary();
            return {bytes.begin(), bytes.end()};
        }
        return v.to_json();
    }

    static auto ok(const ion_value& v, const api_request& req) -> api_response {
        return {200, content_type(req.want_ion), render(v, req.want_ion)};
    }

    static auto error_body(std::string_view code, std::string_view message, bool retryable)
        -> ion_value {
        ion_value v = ion_value::make_struct();
        v.set("code", ion_value::string(code))
            .set("message", ion_value::string(message))
            .set("retryable", ion_value::boolean(retryable));
        return v;
    }

    static auto error(int status, std::string_view code, std::string_view message, bool retryable)
        -> api_response {
        return {status, "application/json", error_body(code, message, retryable).to_json()};
    }

    Backend* backend_;
    ledger_roles roles_;
    std::shared_ptr<const digest_signer> signer_;
};

}  // namespace kythira::ledger
