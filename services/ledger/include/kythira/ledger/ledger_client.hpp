// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A C++ client for the native ledger API (.kiro/specs/verifiable-ledger/
// task 7, Requirement 8.4).
//
//  - Bodies go both ways as Ion binary (Content-Type and Accept
//    application/ion), so documents keep their Ion types and anything read
//    can be re-hashed.
//  - The transport is a function, so tests and in-process callers can skip
//    sockets; https_transport() is the real one: Boost.Beast over TLS 1.2+,
//    presenting a client certificate (mTLS) and verifying the server
//    against a CA bundle and its host name. One connection per request:
//    the API's requests are small and the server is one thread per
//    connection, so pooling would buy little and pin server threads.
//  - Leader changes. A 503 not_leader naming a leader moves the client to
//    that leader and resends; one without a leader, or a connection that
//    fails before anything was sent, tries the next configured endpoint.
//    Both are bounded by max_redirects. A request that may have reached
//    the server (a failure after the connection was up) is never resent by
//    this layer, because a one-shot write could then apply twice.
//  - OCC retry. transact(ledger, fn) runs fn in a session and commits; on a
//    retryable conflict (occ_conflict, stale_block, session_expired, or a
//    leader change that took the session) it aborts what is left and runs
//    fn again on a fresh snapshot after a jittered exponential backoff, up
//    to max_attempts. fn must therefore be safe to run more than once: its
//    reads are the transaction's, its side effects outside the ledger are
//    its own business.

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_http.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::ledger {

/// An error answer from the API.
class ledger_client_error : public std::runtime_error {
public:
    ledger_client_error(int status, std::string code, const std::string& message, bool retryable,
                        std::optional<std::string> leader = std::nullopt)
        : std::runtime_error(code + ": " + message),
          status_(status),
          code_(std::move(code)),
          retryable_(retryable),
          leader_(std::move(leader)) {}
    [[nodiscard]] auto status() const -> int { return status_; }
    [[nodiscard]] auto code() const -> const std::string& { return code_; }
    [[nodiscard]] auto retryable() const -> bool { return retryable_; }
    [[nodiscard]] auto leader() const -> const std::optional<std::string>& { return leader_; }

private:
    int status_;
    std::string code_;
    bool retryable_;
    std::optional<std::string> leader_;
};

/// The transport could not reach the server. `sent` is false when the
/// failure came before any of the request was written (resolve, connect,
/// TLS handshake), so resending it elsewhere is safe.
class ledger_transport_error : public std::runtime_error {
public:
    ledger_transport_error(const std::string& what, bool sent)
        : std::runtime_error(what), sent_(sent) {}
    [[nodiscard]] auto sent() const -> bool { return sent_; }

private:
    bool sent_;
};

/// Sends one request to a base URL ("https://host:port") and returns the
/// answer; throws ledger_transport_error.
using ledger_transport = std::function<api_response(const std::string&, const api_request&)>;

struct https_transport_options {
    std::string ca_path;    // the server's CA bundle
    std::string cert_path;  // the client certificate chain (mTLS)
    std::string key_path;
    std::chrono::milliseconds timeout{std::chrono::seconds(30)};
};

namespace client_detail {

struct url_parts {
    bool tls = true;
    std::string host;
    std::string port;
};

inline auto parse_url(std::string_view url) -> url_parts {
    url_parts out;
    if (url.starts_with("https://")) {
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        out.tls = false;
        url.remove_prefix(7);
    } else {
        throw ledger_transport_error("ledger client: not an http(s) URL: " + std::string(url),
                                     false);
    }
    if (const auto slash = url.find('/'); slash != std::string_view::npos) {
        url = url.substr(0, slash);
    }
    if (url.starts_with('[')) {  // [v6]:port
        const auto close = url.find(']');
        if (close == std::string_view::npos) {
            throw ledger_transport_error("ledger client: bad IPv6 URL", false);
        }
        out.host = std::string(url.substr(1, close - 1));
        url.remove_prefix(close + 1);
        out.port = url.starts_with(':') ? std::string(url.substr(1)) : "";
    } else {
        const auto colon = url.rfind(':');
        out.host = std::string(url.substr(0, colon));
        out.port = colon == std::string_view::npos ? "" : std::string(url.substr(colon + 1));
    }
    if (out.port.empty()) {
        out.port = out.tls ? "443" : "80";
    }
    return out;
}

inline auto target_of(const api_request& req) -> std::string {
    std::string target;
    std::string_view path = req.path;
    // Encode each segment; the API's paths never carry '/' inside one.
    std::size_t at = 0;
    while (at <= path.size()) {
        const auto slash = path.find('/', at);
        const auto seg = path.substr(at, slash == std::string_view::npos ? slash : slash - at);
        target += percent_encode(seg);
        if (slash == std::string_view::npos) {
            break;
        }
        target.push_back('/');
        at = slash + 1;
    }
    char sep = '?';
    for (const auto& [k, v] : req.query) {
        target.push_back(sep);
        target += percent_encode(k);
        target.push_back('=');
        target += percent_encode(v);
        sep = '&';
    }
    return target;
}

}  // namespace client_detail

/// The real transport: HTTPS with a client certificate, or plain HTTP for
/// an http:// URL (tests only).
inline auto https_transport(https_transport_options options) -> ledger_transport {
    namespace asio = boost::asio;
    namespace ssl = asio::ssl;
    namespace beast = boost::beast;
    namespace http = beast::http;
    auto ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
    SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_2_VERSION);
    if (!options.ca_path.empty()) {
        ctx->load_verify_file(options.ca_path);
    } else {
        ctx->set_default_verify_paths();
    }
    ctx->set_verify_mode(ssl::verify_peer);
    if (!options.cert_path.empty()) {
        ctx->use_certificate_chain_file(options.cert_path);
        ctx->use_private_key_file(options.key_path, ssl::context::pem);
    }
    return [ctx, timeout = options.timeout](const std::string& base,
                                            const api_request& req) -> api_response {
        const auto url = client_detail::parse_url(base);
        http::request<http::string_body> out{http::string_to_verb(req.method),
                                             client_detail::target_of(req), 11};
        out.set(http::field::host, url.host);
        out.set(http::field::content_type,
                req.body_is_ion ? "application/ion" : "application/json");
        out.set(http::field::accept, req.want_ion ? "application/ion" : "application/json");
        out.body() = req.body;
        out.prepare_payload();

        asio::io_context ioc;
        http::response<http::string_body> res;
        beast::flat_buffer buf;
        bool sent = false;
        try {
            asio::ip::tcp::resolver resolver(ioc);
            const auto endpoints = resolver.resolve(url.host, url.port);
            if (url.tls) {
                beast::ssl_stream<beast::tcp_stream> stream(ioc, *ctx);
                if (SSL_set_tlsext_host_name(stream.native_handle(), url.host.c_str()) != 1) {
                    throw ledger_transport_error("ledger client: bad server name", false);
                }
                stream.set_verify_callback(ssl::host_name_verification(url.host));
                beast::get_lowest_layer(stream).expires_after(timeout);
                beast::get_lowest_layer(stream).connect(endpoints);
                stream.handshake(ssl::stream_base::client);
                sent = true;
                http::write(stream, out);
                http::read(stream, buf, res);
                beast::error_code ec;
                (void)stream.shutdown(ec);  // a peer that just closes is fine
            } else {
                beast::tcp_stream stream(ioc);
                stream.expires_after(timeout);
                stream.connect(endpoints);
                sent = true;
                http::write(stream, out);
                http::read(stream, buf, res);
            }
        } catch (const ledger_transport_error&) {
            throw;
        } catch (const std::exception& e) {
            throw ledger_transport_error(std::string("ledger client: ") + e.what(), sent);
        }
        api_response r;
        r.status = static_cast<int>(res.result_int());
        r.content_type = std::string(res[http::field::content_type]);
        r.body = std::move(res.body());
        return r;
    };
}

struct ledger_client_options {
    /// Base URLs of the cluster's members; the first is tried first.
    std::vector<std::string> endpoints;
    /// Redirects and endpoint changes allowed for one request.
    int max_redirects = 5;
    /// Attempts transact() makes before giving up on conflicts.
    int max_attempts = 8;
    std::chrono::milliseconds base_backoff{10};
    std::chrono::milliseconds max_backoff{1000};
};

class ledger_client {
public:
    ledger_client(ledger_client_options options, ledger_transport transport)
        : options_(std::move(options)), transport_(std::move(transport)) {
        if (options_.endpoints.empty()) {
            throw std::invalid_argument("ledger client: no endpoints");
        }
        current_ = options_.endpoints.front();
    }

    /// The member the client sends to now (moves on redirects).
    [[nodiscard]] auto endpoint() const -> const std::string& { return current_; }

    // ── Ledgers ─────────────────────────────────────────────────────────────

    auto list_ledgers() -> ion_value { return call("GET", "/v1/ledgers"); }

    auto create_ledger(std::string_view name, bool deletion_protection = true) -> ion_value {
        ion_value body = ion_value::make_struct();
        body.set("name", ion_value::string(name))
            .set("deletionProtection", ion_value::boolean(deletion_protection));
        return call("POST", "/v1/ledgers", &body);
    }

    auto describe_ledger(std::string_view name) -> ion_value {
        return call("GET", ledger_path(name));
    }

    auto set_deletion_protection(std::string_view name, bool on) -> ion_value {
        ion_value body = ion_value::make_struct();
        body.set("deletionProtection", ion_value::boolean(on));
        return call("PATCH", ledger_path(name), &body);
    }

    auto delete_ledger(std::string_view name) -> ion_value {
        return call("DELETE", ledger_path(name));
    }

    // ── Writes ──────────────────────────────────────────────────────────────

    /// One-shot transaction: the statements commit together or not at all.
    /// Returns the commit result (blockAddress, documentIds, ...).
    auto execute(std::string_view ledger, std::vector<ion_value> statements) -> ion_value {
        ion_value body = ion_value::make_struct();
        ion_value list = ion_value::make_list();
        for (auto& st : statements) {
            list.push_back(std::move(st));
        }
        body.set("statements", std::move(list));
        return call("POST", ledger_path(ledger) + "/execute", &body);
    }

    class transaction;

    /// Runs fn(transaction&) and commits, retrying on conflicts (see the
    /// file comment). Returns the commit result.
    template<typename Fn> auto transact(std::string_view ledger, Fn&& fn) -> ion_value;

    /// Attempts the last transact() call made.
    [[nodiscard]] auto last_attempts() const -> int { return last_attempts_; }

    // ── Reads ───────────────────────────────────────────────────────────────

    auto tables(std::string_view ledger) -> ion_value {
        return call("GET", ledger_path(ledger) + "/tables");
    }

    auto get_document(std::string_view ledger, std::string_view table, std::string_view id)
        -> std::optional<ion_value> {
        try {
            return call("GET", ledger_path(ledger) + "/tables/" + std::string(table) +
                                   "/documents/" + std::string(id));
        } catch (const ledger_client_error& e) {
            if (e.code() == "document_not_found" || e.code() == "not_found") {
                return std::nullopt;
            }
            throw;
        }
    }

    auto scan(std::string_view ledger, std::string_view table,
              std::optional<std::string> after = std::nullopt, std::size_t limit = 100)
        -> ion_value {
        std::map<std::string, std::string> q{{"limit", std::to_string(limit)}};
        if (after) {
            q["after"] = *after;
        }
        return call("GET", ledger_path(ledger) + "/tables/" + std::string(table) + "/documents",
                    nullptr, std::move(q));
    }

    /// A document's history (id set) or a table's; query keys start, end,
    /// after and limit as the API takes them.
    auto history(std::string_view ledger, std::string_view table,
                 std::optional<std::string> id = std::nullopt,
                 std::map<std::string, std::string> query = {}) -> ion_value {
        std::string path = ledger_path(ledger) + "/history/" + std::string(table);
        if (id) {
            path += "/" + *id;
        }
        return call("GET", path, nullptr, std::move(query));
    }

    auto digest(std::string_view ledger) -> ion_value {
        return call("GET", ledger_path(ledger) + "/digest");
    }

    auto block(std::string_view ledger, std::uint64_t seq,
               std::optional<std::uint64_t> tip = std::nullopt) -> ion_value {
        return call("GET", ledger_path(ledger) + "/blocks/" + std::to_string(seq), nullptr,
                    tip_query(tip));
    }

    auto revision(std::string_view ledger, std::string_view table, std::string_view id,
                  std::uint64_t version, std::optional<std::uint64_t> tip = std::nullopt)
        -> ion_value {
        return call("GET",
                    ledger_path(ledger) + "/revisions/" + std::string(table) + "/" +
                        std::string(id) + "/" + std::to_string(version),
                    nullptr, tip_query(tip));
    }

    // ── Exports ─────────────────────────────────────────────────────────────

    /// spec: {start, end, format, bucket, prefix}.
    auto start_export(std::string_view ledger, const ion_value& spec) -> ion_value {
        return call("POST", ledger_path(ledger) + "/exports", &spec);
    }

    auto get_export(std::string_view ledger, std::string_view job) -> ion_value {
        return call("GET", ledger_path(ledger) + "/exports/" + std::string(job));
    }

    /// One request; throws ledger_client_error for an error answer.
    auto call(std::string method, std::string path, const ion_value* body = nullptr,
              std::map<std::string, std::string> query = {}) -> ion_value {
        api_request req;
        req.method = std::move(method);
        req.path = std::move(path);
        req.query = std::move(query);
        req.body_is_ion = true;
        req.want_ion = true;
        if (body != nullptr) {
            const auto bytes = body->to_binary();
            req.body.assign(bytes.begin(), bytes.end());
        }
        const auto res = send(req);
        if (res.status < 200 || res.status >= 300) {
            throw error_from(res);
        }
        if (res.body.empty()) {
            return ion_value::make_struct();
        }
        return parse(res.body);
    }

private:
    static auto ledger_path(std::string_view name) -> std::string {
        return "/v1/ledgers/" + std::string(name);
    }

    static auto tip_query(std::optional<std::uint64_t> tip) -> std::map<std::string, std::string> {
        if (!tip) {
            return {};
        }
        return {{"tip", std::to_string(*tip)}};
    }

    static auto parse(const std::string& body) -> ion_value {
        return ion_value::parse(
            std::span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    }

    static auto error_from(const api_response& res) -> ledger_client_error {
        std::string code = "http_" + std::to_string(res.status);
        std::string message = "HTTP " + std::to_string(res.status);
        bool retryable = res.status == 503;
        std::optional<std::string> leader;
        try {
            const auto v = parse(res.body);
            if (v.is_struct()) {
                auto text = [&v](std::string_view f) -> std::optional<std::string> {
                    const ion_value* x = v.find(f);
                    return x != nullptr ? x->as_text() : std::nullopt;
                };
                code = text("code").value_or(code);
                message = text("message").value_or(message);
                leader = text("leader");
                if (const ion_value* r = v.find("retryable"); r != nullptr) {
                    retryable = r->as_bool().value_or(retryable);
                }
            }
        } catch (const std::exception&) {
            // Not an API error body (e.g. the server's own 413): keep the
            // status-derived code.
        }
        return {res.status, std::move(code), message, retryable, std::move(leader)};
    }

    /// Sends to the current member, following leader redirects and moving
    /// past members that cannot be reached.
    auto send(const api_request& req) -> api_response {
        std::size_t next_index = 0;
        for (int hop = 0;; ++hop) {
            try {
                auto res = transport_(current_, req);
                if (res.status != 503 || hop >= options_.max_redirects) {
                    return res;
                }
                const auto err = error_from(res);
                if (err.code() != "not_leader") {
                    return res;
                }
                if (err.leader() && *err.leader() != current_) {
                    current_ = *err.leader();
                } else {
                    current_ = next_endpoint(next_index);
                }
            } catch (const ledger_transport_error& e) {
                if (e.sent() || hop >= options_.max_redirects) {
                    throw;
                }
                current_ = next_endpoint(next_index);
            }
        }
    }

    auto next_endpoint(std::size_t& index) -> std::string {
        const auto& eps = options_.endpoints;
        for (std::size_t tried = 0; tried < eps.size(); ++tried) {
            const auto& candidate = eps[index++ % eps.size()];
            if (candidate != current_) {
                return candidate;
            }
        }
        return current_;
    }

    static auto retryable_conflict(const ledger_client_error& e) -> bool {
        if (e.code() == "occ_conflict" || e.code() == "stale_block" ||
            e.code() == "session_expired" || e.code() == "not_leader") {
            return true;
        }
        return e.status() == 409 && e.retryable();
    }

    auto backoff(int attempt, std::mt19937_64& rng) const -> void {
        const auto cap = std::min<std::int64_t>(
            options_.max_backoff.count(),
            options_.base_backoff.count() * (std::int64_t{1} << std::min(attempt, 20)));
        std::uniform_int_distribution<std::int64_t> pick(0, std::max<std::int64_t>(cap, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(pick(rng)));
    }

    ledger_client_options options_;
    ledger_transport transport_;
    std::string current_;
    int last_attempts_ = 0;
};

/// One session: reads at a snapshot, buffered writes, then commit.
/// Obtained from ledger_client::transact.
class ledger_client::transaction {
public:
    transaction(ledger_client& client, std::string ledger)
        : client_(&client), base_("/v1/ledgers/" + std::move(ledger) + "/sessions") {
        const auto v = client_->call("POST", base_);
        token_ = text(v, "token");
        if (const ion_value* s = v.find("snapshot"); s != nullptr) {
            snapshot_ = static_cast<std::uint64_t>(s->as_int64().value_or(0));
        }
        open_ = true;
    }

    transaction(const transaction&) = delete;
    auto operator=(const transaction&) -> transaction& = delete;
    transaction(transaction&&) = delete;
    auto operator=(transaction&&) -> transaction& = delete;
    ~transaction() { abandon(); }

    [[nodiscard]] auto snapshot() const -> std::uint64_t { return snapshot_; }

    /// The document's current revision at the snapshot, if it exists.
    auto get(std::string_view table, std::string_view id) -> std::optional<ion_value> {
        ion_value body = ion_value::make_struct();
        body.set("table", ion_value::string(table)).set("id", ion_value::string(id));
        auto v = op("get", body);
        if (const ion_value* d = v.find("document"); d != nullptr) {
            return *d;
        }
        return std::nullopt;
    }

    auto scan(std::string_view table, std::optional<std::string> after = std::nullopt,
              std::size_t limit = 100) -> ion_value {
        ion_value body = ion_value::make_struct();
        body.set("table", ion_value::string(table))
            .set("limit", ion_value::integer(static_cast<std::int64_t>(limit)));
        if (after) {
            body.set("after", ion_value::string(*after));
        }
        return op("scan", body);
    }

    auto lookup(std::string_view table, std::string_view index, const ion_value& value,
                std::optional<std::string> after = std::nullopt, std::size_t limit = 100)
        -> ion_value {
        ion_value body = ion_value::make_struct();
        body.set("table", ion_value::string(table))
            .set("index", ion_value::string(index))
            .set("value", value)
            .set("limit", ion_value::integer(static_cast<std::int64_t>(limit)));
        if (after) {
            body.set("after", ion_value::string(*after));
        }
        return op("lookup", body);
    }

    /// Buffers an insert and returns the new document's id.
    auto insert(std::string_view table, const ion_value& value) -> std::string {
        ion_value body = ion_value::make_struct();
        body.set("table", ion_value::string(table)).set("value", value);
        return text(op("insert", body), "id");
    }

    /// Buffers statements (journal_types.hpp's statements:: builders).
    auto add(std::vector<ion_value> statements) -> void {
        ion_value list = ion_value::make_list();
        for (auto& st : statements) {
            list.push_back(std::move(st));
        }
        ion_value body = ion_value::make_struct();
        body.set("statements", std::move(list));
        (void)op("statements", body);
    }

    auto add(ion_value statement) -> void {
        std::vector<ion_value> one;
        one.push_back(std::move(statement));
        add(std::move(one));
    }

    /// Commits. The server ends the session either way, so nothing is
    /// left to abort after this.
    auto commit() -> ion_value {
        open_ = false;
        return client_->call("POST", base_ + "/" + token_ + "/commit");
    }

    /// Aborts the session if it is still open; never throws.
    auto abandon() noexcept -> void {
        if (!open_) {
            return;
        }
        open_ = false;
        try {
            (void)client_->call("DELETE", base_ + "/" + token_);
        } catch (...) {  // NOLINT(bugprone-empty-catch): the session expires anyway
        }
    }

private:
    auto op(std::string_view name, const ion_value& body) -> ion_value {
        try {
            return client_->call("POST", base_ + "/" + token_ + "/" + std::string(name), &body);
        } catch (const ledger_client_error& e) {
            if (e.code() == "session_expired" || e.code() == "not_leader") {
                open_ = false;  // nothing left to abort
            }
            throw;
        }
    }

    static auto text(const ion_value& v, std::string_view f) -> std::string {
        const ion_value* x = v.find(f);
        return x != nullptr ? x->as_text().value_or("") : "";
    }

    ledger_client* client_;
    std::string base_;
    std::string token_;
    std::uint64_t snapshot_ = 0;
    bool open_ = false;
};

template<typename Fn> auto ledger_client::transact(std::string_view ledger, Fn&& fn) -> ion_value {
    std::mt19937_64 rng{std::random_device{}()};
    for (int attempt = 1;; ++attempt) {
        std::optional<transaction> txn;
        try {
            txn.emplace(*this, std::string(ledger));
            fn(*txn);
            auto result = txn->commit();
            last_attempts_ = attempt;
            return result;
        } catch (const ledger_client_error& e) {
            if (txn) {
                txn->abandon();
            }
            if (!retryable_conflict(e) || attempt >= options_.max_attempts) {
                last_attempts_ = attempt;
                throw;
            }
        }
        backoff(attempt, rng);
    }
}

}  // namespace kythira::ledger
