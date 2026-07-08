#pragma once

/// @file acme_test_server.hpp
/// @brief Self-contained RFC 8555 (ACME) mock server backed by a single
///        `certificate_authority` instance — implements the wire protocol
///        needed to drive `acme_certificate_provider` through the full
///        happy path and standard failure modes, without depending on a
///        real, rate-limited, publicly-routable CA (Requirement 18.7).
///
/// Mirrors `ca_test_fixture`'s "construct it and go" shape. Every
/// JWS-signed request's signature is verified against the calling account's
/// registered public key (or, for a brand-new account, the JWK embedded in
/// the request itself) before any state change; a bad signature or a
/// stale/reused nonce is rejected with the matching RFC 8555 problem
/// document (Requirement 18.1/18.10 error handling).

#include <raft/acme_jws.hpp>
#include <raft/certificate_authority.hpp>
#include <raft/fault_injection.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#ifdef KYTHIRA_HAS_LDNS
#include <ldns/ldns.h>
#endif

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace raft::testing {

namespace acme_test_server_detail {

[[nodiscard]] inline auto random_url_safe_token() -> std::string {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<unsigned char> bytes(16);
    for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));
    return acme_jws::base64url_encode(bytes);
}

[[nodiscard]] inline auto ends_with(const std::string& value, const std::string& suffix) -> bool {
    if (suffix.size() > value.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

// Test-only override for the mDNS-capability probe below — nullopt (the
// default) means "use the real /etc/nsswitch.conf-based probe"; set to force
// a result so Property 22 doesn't depend on the CI machine's actual resolver
// configuration (task 35's "test-only injection point").
inline auto mdns_capability_override() -> std::optional<bool>& {
    static std::optional<bool> value;
    return value;
}

// Static capability check (not a live resolution attempt — a resolution
// failure for a nonexistent sentinel name is indistinguishable from "no mDNS
// resolver at all," which is exactly the ambiguity Requirement 20.6 wants
// avoided): looks for an mDNS NSS module (mdns, mdns4, mdns6,
// mdns4_minimal, mdns6_minimal, ...) on the "hosts:" line of
// /etc/nsswitch.conf, matching how Linux's nss-mdns/Avahi integration is
// actually configured.
[[nodiscard]] inline auto nsswitch_has_mdns_module() -> bool {
    std::ifstream f("/etc/nsswitch.conf");
    if (!f.is_open()) return false;  // can't determine -> fail closed (report unavailable)
    std::string line;
    while (std::getline(f, line)) {
        auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        if (line.compare(first, 6, "hosts:") != 0) continue;
        return line.find("mdns") != std::string::npos;
    }
    return false;
}

[[nodiscard]] inline auto mdns_capability_available() -> bool {
    if (mdns_capability_override().has_value()) return *mdns_capability_override();
    return nsswitch_has_mdns_module();
}

}  // namespace acme_test_server_detail

/// Test-only override for `acme_test_server`'s mDNS-capability probe
/// (Property 22, task 35). Pass `true`/`false` to force the probe's result
/// for every `acme_test_server` instance for the remainder of the process,
/// or `std::nullopt` to restore the real `/etc/nsswitch.conf`-based probe.
inline void set_mdns_capability_override_for_test(std::optional<bool> value) {
    acme_test_server_detail::mdns_capability_override() = value;
}

/// RFC 8555 problem document ("application/problem+json") error, thrown by
/// route handlers and translated into the matching HTTP status + body.
struct acme_problem : std::runtime_error {
    std::string type;  // e.g. "urn:ietf:params:acme:error:malformed"
    int http_status;
    acme_problem(std::string acme_type, int status, const std::string& detail)
        : std::runtime_error(detail), type(std::move(acme_type)), http_status(status) {}
};

struct acme_test_server_options {
    // false only via the fault-injection bypass (skip_challenge_validation)
    // in FIU-enabled builds — challenges are always "validated for real"
    // otherwise; this field exists for documentation/clarity, not gating.
    bool validate_challenges{true};
    std::string bind_address{"127.0.0.1"};
    int port{0};  // 0 = OS-assigned
    // Overrides the well-known/DNS validation target port for http-01
    // (production ACME always uses 80; tests need a non-privileged port
    // — the identifier's own advertised port, supplied per-order by
    // whatever stood up the http-01 responder).
    int http01_validation_port{80};
};

class acme_test_server {
public:
    using options = acme_test_server_options;

    explicit acme_test_server(options opts = {}) : _opts(std::move(opts)) {
        setup_routes();
        _actual_port = _server.bind_to_any_port(_opts.bind_address);
        if (_actual_port <= 0) {
            throw std::runtime_error("acme_test_server: failed to bind to " + _opts.bind_address);
        }
        _base_url = "http://" + _opts.bind_address + ":" + std::to_string(_actual_port);
        _server_thread = std::jthread([this](std::stop_token) { _server.listen_after_bind(); });
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!_server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~acme_test_server() {
        _server.stop();
        if (_server_thread.joinable()) _server_thread.join();
    }

    acme_test_server(const acme_test_server&) = delete;
    acme_test_server& operator=(const acme_test_server&) = delete;
    acme_test_server(acme_test_server&&) = delete;
    acme_test_server& operator=(acme_test_server&&) = delete;

    [[nodiscard]] auto directory_url() const -> std::string { return _base_url + "/directory"; }
    [[nodiscard]] auto root_certificate_pem() const -> const std::string& {
        return _ca.root_certificate_pem();
    }
    [[nodiscard]] auto base_url() const -> const std::string& { return _base_url; }

    /// Test-only introspection (Property 21): the status of `challenge_type`
    /// ("http-01"/"dns-01") on the most recent authorization issued for
    /// `identifier_value`, or `std::nullopt` if no such authorization/
    /// challenge exists. "pending" means the client never engaged that
    /// challenge type at all — lets a test distinguish "dispatched to the
    /// wrong challenge type" from "dispatched correctly but validation
    /// itself failed" without needing the whole order to succeed.
    [[nodiscard]] auto challenge_status_for(const std::string& identifier_value,
                                            const std::string& challenge_type)
        -> std::optional<std::string> {
        std::lock_guard lock(_mu);
        std::optional<std::string> result;
        for (const auto& [id, authz] : _authorizations) {
            (void)id;
            if (std::string(authz.identifier.at("value").as_string()) != identifier_value) continue;
            for (const auto& c : authz.challenges) {
                if (c.type == challenge_type) result = c.status;
            }
        }
        return result;
    }

    /// Test-only introspection (Property 22): the machine-readable error type
    /// (e.g. "mdnsResolverUnavailable") recorded against the most recent
    /// invalid attempt of `challenge_type` for `identifier_value`, or
    /// `std::nullopt` if that challenge never failed (or was never
    /// attempted). Lets a test assert on *why* validation failed, not just
    /// that it did — Requirement 20.6 requires mDNS-unavailable and RFC 2136
    /// UPDATE failures to be distinguishable from each other and from a
    /// generic failure.
    [[nodiscard]] auto challenge_error_for(const std::string& identifier_value,
                                           const std::string& challenge_type)
        -> std::optional<std::string> {
        std::lock_guard lock(_mu);
        std::optional<std::string> result;
        for (const auto& [id, authz] : _authorizations) {
            (void)id;
            if (std::string(authz.identifier.at("value").as_string()) != identifier_value) continue;
            for (const auto& c : authz.challenges) {
                if (c.type == challenge_type) result = c.error;
            }
        }
        return result;
    }

private:
    struct account_record {
        std::string url;
        boost::json::object jwk;
        acme_jws::evp_pkey_ptr public_key;
    };

    struct challenge_record {
        std::string type;
        std::string token;
        std::string status{"pending"};
        std::string authz_id;
        std::optional<std::string> error;  // set on a failed validation attempt (Property 22)
    };

    struct authorization_record {
        std::string id;
        boost::json::object identifier;
        std::string status{"pending"};
        std::vector<challenge_record> challenges;
    };

    struct order_record {
        std::string id;
        std::string status{"pending"};
        std::vector<boost::json::object> identifiers;
        std::vector<std::string> authorization_ids;
        std::optional<std::string> certificate_pem;
        std::optional<std::string> chain_pem;
    };

    options _opts;
    certificate_authority _ca;
    httplib::Server _server;
    std::jthread _server_thread;
    int _actual_port{0};
    std::string _base_url;

    // Recursive: several handlers hold _mu across a call to send_json()/
    // issue_nonce(), which themselves lock _mu to insert the issued nonce —
    // simpler than restructuring every handler to release-then-reacquire
    // around each response write, and this is test-only, low-contention code.
    std::recursive_mutex _mu;
    std::set<std::string> _valid_nonces;
    std::map<std::string, account_record> _accounts_by_url;  // key: account URL path
    std::uint64_t _next_account_id{1};
    std::map<std::string, authorization_record> _authorizations;  // key: authz id
    std::uint64_t _next_authz_id{1};
    std::map<std::string, order_record> _orders;  // key: order id
    std::uint64_t _next_order_id{1};

    // ── Helpers ──────────────────────────────────────────────────────────────

    auto issue_nonce() -> std::string {
        std::lock_guard lock(_mu);
        auto nonce = acme_test_server_detail::random_url_safe_token();
        _valid_nonces.insert(nonce);
        return nonce;
    }

    // Verifies a POST body's compact JWS: signature (against the embedded
    // "jwk" for new-account, or the registered account behind "kid"
    // otherwise), nonce freshness (single-use), and that "url" matches the
    // request's own URL (RFC 8555 §6.4 — prevents replaying a signed request
    // against a different endpoint). Throws acme_problem on any failure.
    // Returns the verified payload as a parsed JSON value (boost::json::null
    // for an empty POST-as-GET payload) plus the resolved account URL
    // ("" for a not-yet-existing new-account request).
    struct authenticated_request {
        boost::json::value payload;
        std::string account_url;
        EVP_PKEY* key{nullptr};  // borrowed — owned by _accounts_by_url or a locally-parsed jwk
        acme_jws::evp_pkey_ptr owned_key;  // populated only for the embedded-jwk (new-account) path
    };

    [[nodiscard]] auto authenticate(const httplib::Request& req) -> authenticated_request {
        boost::json::value outer;
        try {
            outer = boost::json::parse(req.body);
        } catch (const std::exception&) {
            throw acme_problem("urn:ietf:params:acme:error:malformed", 400,
                               "request body is not valid JSON");
        }
        if (!outer.is_object() || !outer.as_object().contains("protected") ||
            !outer.as_object().contains("payload") || !outer.as_object().contains("signature")) {
            throw acme_problem("urn:ietf:params:acme:error:malformed", 400,
                               "request body is not a JWS in flattened JSON serialization");
        }
        const auto& obj = outer.as_object();
        std::string compact = std::string(obj.at("protected").as_string()) + "." +
                              std::string(obj.at("payload").as_string()) + "." +
                              std::string(obj.at("signature").as_string());

        boost::json::object header;
        try {
            header = boost::json::parse(
                         acme_jws::base64url_decode_string(obj.at("protected").as_string()))
                         .as_object();
        } catch (const std::exception&) {
            throw acme_problem("urn:ietf:params:acme:error:malformed", 400,
                               "protected header is not valid JSON");
        }

        std::string expected_url = _base_url + req.path;
        if (!header.contains("url") || header.at("url").as_string() != expected_url) {
            throw acme_problem("urn:ietf:params:acme:error:malformed", 400,
                               "protected header \"url\" does not match the request URL");
        }

        {
            std::lock_guard lock(_mu);
            if (!header.contains("nonce")) {
                throw acme_problem("urn:ietf:params:acme:error:badNonce", 400, "missing nonce");
            }
            std::string nonce = std::string(header.at("nonce").as_string());
            auto it = _valid_nonces.find(nonce);
            if (it == _valid_nonces.end()) {
                throw acme_problem("urn:ietf:params:acme:error:badNonce", 400,
                                   "unknown or reused nonce");
            }
            _valid_nonces.erase(it);
        }

        authenticated_request result;
        if (header.contains("jwk")) {
            result.owned_key = acme_jws::public_key_from_jwk(header.at("jwk").as_object());
            result.key = result.owned_key.get();
            // Not yet known to have an account — resolved by the caller
            // (new-account looks up-or-creates by thumbprint).
        } else if (header.contains("kid")) {
            std::string kid = std::string(header.at("kid").as_string());
            std::lock_guard lock(_mu);
            auto it = _accounts_by_url.find(kid);
            if (it == _accounts_by_url.end()) {
                throw acme_problem("urn:ietf:params:acme:error:accountDoesNotExist", 400,
                                   "unknown account: " + kid);
            }
            result.key = it->second.public_key.get();
            result.account_url = kid;
        } else {
            throw acme_problem("urn:ietf:params:acme:error:malformed", 400,
                               "protected header has neither \"jwk\" nor \"kid\"");
        }

        acme_jws::verified_jws verified;
        try {
            verified = acme_jws::verify(compact, result.key);
        } catch (const std::exception& ex) {
            throw acme_problem("urn:ietf:params:acme:error:badSignature", 400, ex.what());
        }
        result.payload = verified.payload.empty() ? boost::json::value(nullptr)
                                                  : boost::json::parse(verified.payload);
        return result;
    }

    auto send_problem(httplib::Response& res, const acme_problem& p) -> void {
        res.status = p.http_status;
        boost::json::object body;
        body["type"] = p.type;
        body["detail"] = std::string(p.what());
        res.set_content(boost::json::serialize(body), "application/problem+json");
    }

    auto send_json(httplib::Response& res, const boost::json::value& body, int status = 200)
        -> void {
        res.status = status;
        res.set_header("Replay-Nonce", issue_nonce());
        res.set_content(boost::json::serialize(body), "application/json");
    }

    [[nodiscard]] auto authorization_json(const authorization_record& authz) const
        -> boost::json::object {
        boost::json::object obj;
        obj["status"] = authz.status;
        obj["identifier"] = authz.identifier;
        boost::json::array challenges;
        for (const auto& c : authz.challenges) {
            boost::json::object cj;
            cj["type"] = c.type;
            cj["url"] = _base_url + "/challenge/" + authz.id + "/" + c.type;
            cj["token"] = c.token;
            cj["status"] = c.status;
            if (c.error.has_value()) {
                // Mirrors RFC 8555 §7.1.3's per-challenge "error" problem
                // document, distinguishing WHY validation failed rather than
                // just that it did (Requirement 20.6).
                cj["error"] = boost::json::object{{"type", *c.error}};
            }
            challenges.push_back(cj);
        }
        obj["challenges"] = challenges;
        return obj;
    }

    [[nodiscard]] auto order_json(const order_record& order) const -> boost::json::object {
        boost::json::object obj;
        obj["status"] = order.status;
        boost::json::array ids;
        for (const auto& i : order.identifiers) ids.push_back(i);
        obj["identifiers"] = ids;
        boost::json::array authzs;
        for (const auto& id : order.authorization_ids) {
            authzs.push_back(boost::json::string(_base_url + "/authz/" + id));
        }
        obj["authorizations"] = authzs;
        obj["finalize"] = _base_url + "/finalize/" + order.id;
        if (order.certificate_pem.has_value()) {
            obj["certificate"] = _base_url + "/cert/" + order.id;
        }
        return obj;
    }

    // Recomputes an order's status from its authorizations: "ready" once
    // every authorization is valid, "invalid" if any authorization is
    // invalid, otherwise left as "pending"/"processing"/"valid" (the latter
    // only ever set by finalize()).
    auto recompute_order_status(order_record& order) -> void {
        if (order.status == "valid") return;
        bool any_invalid = false;
        bool all_valid = true;
        for (const auto& authz_id : order.authorization_ids) {
            auto it = _authorizations.find(authz_id);
            if (it == _authorizations.end()) continue;
            if (it->second.status == "invalid") any_invalid = true;
            if (it->second.status != "valid") all_valid = false;
        }
        if (any_invalid) {
            order.status = "invalid";
        } else if (all_valid) {
            order.status = "ready";
        }
    }

    struct validation_result {
        bool valid{false};
        // Machine-readable failure reason (Requirement 20.6) — empty for a
        // successful validation OR a generic/unspecified failure; set to a
        // specific value only where the failure mode is itself
        // distinguishable (currently just "mdnsResolverUnavailable").
        std::optional<std::string> error;
    };

    // Real challenge validation (Requirement 18.8): an outbound HTTP GET to
    // the identifier's well-known path for http-01, or a real DNS TXT query
    // for dns-01 — unless the fault-injection bypass is active, in which
    // case validation always succeeds without any outbound I/O.
    [[nodiscard]] auto validate_challenge(const authorization_record& authz,
                                          const challenge_record& challenge) -> validation_result {
        bool skip = false;
        fiu_do_on("raft/acme/test_server/skip_challenge_validation", skip = true;);
        if (skip) return {true, std::nullopt};

        std::string identifier_value = std::string(authz.identifier.at("value").as_string());
        std::string expected_key_authorization =
            challenge.token + "." + _server_thumbprint_for_test_only;

        if (challenge.type == "http-01") {
            // Requirement 20.2: `.local` identifiers resolve via the same
            // ordinary getaddrinfo()-based resolution as any other hostname
            // (httplib::Client's connect() below does exactly that — no
            // custom mDNS client code) — but WHEN the host has no
            // mDNS-capable resolver configured, fail with a distinguishable
            // error up front rather than letting the connection attempt
            // produce a generic, indistinguishable failure (a plain DNS-only
            // resolver would also fail to resolve a ".local" name, but for
            // an entirely different, unrelated reason).
            if (acme_test_server_detail::ends_with(identifier_value, ".local") &&
                !acme_test_server_detail::mdns_capability_available()) {
                return {false, std::string("mdnsResolverUnavailable")};
            }

            httplib::Client client(identifier_value, _opts.http01_validation_port);
            client.set_connection_timeout(5, 0);
            client.set_read_timeout(5, 0);
            auto res = client.Get("/.well-known/acme-challenge/" + challenge.token);
            if (!res || res->status != 200) return {false, std::nullopt};
            // Trim trailing whitespace/newlines the responder may have added.
            std::string body = res->body;
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
            return {body == expected_key_authorization, std::nullopt};
        }

        if (challenge.type == "dns-01") {
#ifdef KYTHIRA_HAS_LDNS
            std::string qname = "_acme-challenge." + identifier_value + ".";
            std::string expected_digest = acme_jws::base64url_encode([&] {
                std::array<unsigned char, 32> digest{};
                SHA256(reinterpret_cast<const unsigned char*>(expected_key_authorization.data()),
                       expected_key_authorization.size(), digest.data());
                return std::vector<unsigned char>(digest.begin(), digest.end());
            }());

            ldns_resolver* res = nullptr;
            if (ldns_resolver_new_frm_file(&res, nullptr) != LDNS_STATUS_OK || res == nullptr)
                return {false, std::nullopt};
            std::unique_ptr<ldns_resolver, void (*)(ldns_resolver*)> res_guard{res,
                                                                               ldns_resolver_free};

            ldns_rdf* qname_rdf = ldns_dname_new_frm_str(qname.c_str());
            if (qname_rdf == nullptr) return {false, std::nullopt};
            ldns_pkt* pkt =
                ldns_resolver_query(res, qname_rdf, LDNS_RR_TYPE_TXT, LDNS_RR_CLASS_IN, LDNS_RD);
            ldns_rdf_deep_free(qname_rdf);
            if (pkt == nullptr) return {false, std::nullopt};
            std::unique_ptr<ldns_pkt, void (*)(ldns_pkt*)> pkt_guard{pkt, ldns_pkt_free};

            ldns_rr_list* answers =
                ldns_pkt_rr_list_by_type(pkt, LDNS_RR_TYPE_TXT, LDNS_SECTION_ANSWER);
            if (answers == nullptr) return {false, std::nullopt};
            std::unique_ptr<ldns_rr_list, void (*)(ldns_rr_list*)> answers_guard{
                answers, ldns_rr_list_deep_free};

            for (std::size_t i = 0; i < ldns_rr_list_rr_count(answers); ++i) {
                ldns_rr* rr = ldns_rr_list_rr(answers, i);
                ldns_rdf* rdf = ldns_rr_rdf(rr, 0);
                if (rdf == nullptr) continue;
                char* str = ldns_rdf2str(rdf);
                if (str == nullptr) continue;
                std::string txt_value(str);
                free(str);  // NOLINT(cppcoreguidelines-no-malloc) — ldns allocates with malloc
                // ldns_rdf2str wraps TXT strings in quotes.
                if (!txt_value.empty() && txt_value.front() == '"' && txt_value.back() == '"') {
                    txt_value = txt_value.substr(1, txt_value.size() - 2);
                }
                if (txt_value == expected_digest) return {true, std::nullopt};
            }
            return {false, std::nullopt};
#else
            return {false,
                    std::nullopt};  // No DNS validation capability built in — always fails closed.
#endif
        }

        return {false, std::nullopt};
    }

    // The account thumbprint used for THIS validation call. Set transiently
    // by challenge_handler() before calling validate_challenge() — the
    // expected key authorization depends on which account owns the order,
    // which validate_challenge() itself doesn't otherwise have access to.
    std::string _server_thumbprint_for_test_only;

    auto setup_routes() -> void {
        _server.Get("/directory", [this](const httplib::Request&, httplib::Response& res) {
            boost::json::object dir;
            dir["newNonce"] = _base_url + "/new-nonce";
            dir["newAccount"] = _base_url + "/new-account";
            dir["newOrder"] = _base_url + "/new-order";
            res.set_content(boost::json::serialize(dir), "application/json");
        });

        // httplib dispatches HEAD requests to the matching GET handler and
        // strips the body automatically — RFC 8555's newNonce endpoint
        // accepts either method, so registering GET alone covers both.
        _server.Get("/new-nonce", [this](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
            res.set_header("Replay-Nonce", issue_nonce());
        });

        _server.Post("/new-account", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto authed = authenticate(req);
                std::string thumbprint = acme_jws::jwk_thumbprint(authed.key);

                std::lock_guard lock(_mu);
                // Idempotent: reuse the existing account if this key was seen before.
                // Keyed by the FULL absolute URL — the same value sent back as
                // "Location" and expected in a subsequent request's "kid".
                for (auto& [url, acct] : _accounts_by_url) {
                    if (acme_jws::jwk_thumbprint(acct.public_key.get()) == thumbprint) {
                        send_json(res, boost::json::object{{"status", "valid"}}, 200);
                        res.set_header("Location", url);
                        return;
                    }
                }
                std::string account_url = _base_url + "/acct/" + std::to_string(_next_account_id++);
                account_record acct;
                acct.url = account_url;
                acct.jwk = acme_jws::jwk_from_public_key(authed.key);
                acct.public_key = acme_jws::public_key_from_jwk(acct.jwk);
                _accounts_by_url.emplace(account_url, std::move(acct));

                send_json(res, boost::json::object{{"status", "valid"}}, 201);
                res.set_header("Location", account_url);
            } catch (const acme_problem& p) {
                send_problem(res, p);
            }
        });

        _server.Post("/new-order", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto authed = authenticate(req);
                if (authed.account_url.empty()) {
                    throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                       "new-order requires an existing account (\"kid\")");
                }
                const auto& payload = authed.payload.as_object();
                const auto& identifiers = payload.at("identifiers").as_array();

                std::lock_guard lock(_mu);
                order_record order;
                order.id = std::to_string(_next_order_id++);
                for (const auto& ident : identifiers) {
                    order.identifiers.push_back(ident.as_object());
                    authorization_record authz;
                    authz.id = std::to_string(_next_authz_id++);
                    authz.identifier = ident.as_object();
                    authz.challenges.push_back({"http-01",
                                                acme_test_server_detail::random_url_safe_token(),
                                                "pending", authz.id});
                    // IP identifiers (RFC 8738) never get a dns-01 option —
                    // there is no sensible "_acme-challenge.<ip>." zone.
                    if (authz.identifier.at("type").as_string() != "ip") {
                        authz.challenges.push_back(
                            {"dns-01", acme_test_server_detail::random_url_safe_token(), "pending",
                             authz.id});
                    }
                    order.authorization_ids.push_back(authz.id);
                    _authorizations.emplace(authz.id, std::move(authz));
                }
                std::string order_id = order.id;
                _orders.emplace(order_id, std::move(order));

                send_json(res, order_json(_orders.at(order_id)), 201);
                res.set_header("Location", _base_url + "/order/" + order_id);
            } catch (const acme_problem& p) {
                send_problem(res, p);
            } catch (const std::exception& ex) {
                send_problem(res,
                             acme_problem("urn:ietf:params:acme:error:malformed", 400, ex.what()));
            }
        });

        _server.Post(R"(/authz/(\d+))",
                     [this](const httplib::Request& req, httplib::Response& res) {
                         try {
                             auto authed = authenticate(req);
                             if (authed.account_url.empty()) {
                                 throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                                    "requires an account");
                             }
                             std::string id = req.matches[1];
                             std::lock_guard lock(_mu);
                             auto it = _authorizations.find(id);
                             if (it == _authorizations.end()) {
                                 throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                                    "unknown authorization: " + id);
                             }
                             send_json(res, authorization_json(it->second));
                         } catch (const acme_problem& p) {
                             send_problem(res, p);
                         }
                     });

        _server.Post(R"(/challenge/(\d+)/([\w-]+))", [this](const httplib::Request& req,
                                                            httplib::Response& res) {
            try {
                auto authed = authenticate(req);
                if (authed.account_url.empty()) {
                    throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                       "requires an account");
                }
                std::string authz_id = req.matches[1];
                std::string challenge_type = req.matches[2];

                std::string thumbprint;
                {
                    std::lock_guard lock(_mu);
                    auto acct_it = _accounts_by_url.find(authed.account_url);
                    if (acct_it == _accounts_by_url.end()) {
                        throw acme_problem("urn:ietf:params:acme:error:accountDoesNotExist", 400,
                                           "unknown account");
                    }
                    thumbprint = acme_jws::jwk_thumbprint(acct_it->second.public_key.get());
                }

                authorization_record authz_copy;
                challenge_record* challenge_ptr = nullptr;
                {
                    std::lock_guard lock(_mu);
                    auto authz_it = _authorizations.find(authz_id);
                    if (authz_it == _authorizations.end()) {
                        throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                           "unknown authorization");
                    }
                    for (auto& c : authz_it->second.challenges) {
                        if (c.type == challenge_type) challenge_ptr = &c;
                    }
                    if (challenge_ptr == nullptr) {
                        throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                           "unknown challenge type");
                    }
                    challenge_ptr->status = "processing";
                    authz_copy = authz_it->second;
                }

                _server_thumbprint_for_test_only = thumbprint;
                challenge_record challenge_snapshot;
                for (const auto& c : authz_copy.challenges) {
                    if (c.type == challenge_type) challenge_snapshot = c;
                }
                auto result = validate_challenge(authz_copy, challenge_snapshot);
                bool valid = result.valid;

                {
                    std::lock_guard lock(_mu);
                    auto authz_it = _authorizations.find(authz_id);
                    if (authz_it != _authorizations.end()) {
                        for (auto& c : authz_it->second.challenges) {
                            if (c.type == challenge_type) {
                                c.status = valid ? "valid" : "invalid";
                                c.error = valid ? std::nullopt : result.error;
                            }
                        }
                        authz_it->second.status = valid ? "valid" : "invalid";
                        for (auto& [oid, order] : _orders) {
                            (void)oid;
                            if (std::find(order.authorization_ids.begin(),
                                          order.authorization_ids.end(),
                                          authz_id) != order.authorization_ids.end()) {
                                recompute_order_status(order);
                            }
                        }
                    }
                }

                std::lock_guard lock(_mu);
                send_json(res, authorization_json(_authorizations.at(authz_id)));
            } catch (const acme_problem& p) {
                send_problem(res, p);
            }
        });

        _server.Post(R"(/order/(\d+))",
                     [this](const httplib::Request& req, httplib::Response& res) {
                         try {
                             auto authed = authenticate(req);
                             if (authed.account_url.empty()) {
                                 throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                                    "requires an account");
                             }
                             std::string order_id = req.matches[1];
                             std::lock_guard lock(_mu);
                             auto it = _orders.find(order_id);
                             if (it == _orders.end()) {
                                 throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                                    "unknown order: " + order_id);
                             }
                             recompute_order_status(it->second);
                             send_json(res, order_json(it->second));
                         } catch (const acme_problem& p) {
                             send_problem(res, p);
                         }
                     });

        _server.Post(R"(/finalize/(\d+))", [this](const httplib::Request& req,
                                                  httplib::Response& res) {
            try {
                auto authed = authenticate(req);
                if (authed.account_url.empty()) {
                    throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                       "requires an account");
                }
                std::string order_id = req.matches[1];

                std::lock_guard lock(_mu);
                auto it = _orders.find(order_id);
                if (it == _orders.end()) {
                    throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                       "unknown order");
                }
                order_record& order = it->second;
                recompute_order_status(order);
                if (order.status != "ready") {
                    throw acme_problem(
                        "urn:ietf:params:acme:error:orderNotReady", 403,
                        "order is not ready for finalization (status=" + order.status + ")");
                }

                std::string csr_b64url =
                    std::string(authed.payload.as_object().at("csr").as_string());
                auto csr_der = acme_jws::base64url_decode(csr_b64url);
                const unsigned char* p = csr_der.data();
                X509_REQ* raw_req = d2i_X509_REQ(
                    nullptr, &p, static_cast<long>(csr_der.size()));  // NOLINT(google-runtime-int)
                if (raw_req == nullptr) {
                    throw acme_problem("urn:ietf:params:acme:error:badCSR", 400, "unparseable CSR");
                }
                std::unique_ptr<X509_REQ, void (*)(X509_REQ*)> csr_guard{raw_req, X509_REQ_free};

                BIO* bio = BIO_new(BIO_s_mem());
                PEM_write_bio_X509_REQ(bio, raw_req);
                BUF_MEM* mem = nullptr;
                BIO_get_mem_ptr(bio, &mem);
                std::string csr_pem(mem->data, mem->length);
                BIO_free(bio);

                csr_signing_options sign_opts;
                for (const auto& ident : order.identifiers) {
                    std::string type = std::string(ident.at("type").as_string());
                    std::string value = std::string(ident.at("value").as_string());
                    if (type == "ip") {
                        sign_opts.ip_addresses.push_back(value);
                    } else {
                        sign_opts.dns_names.push_back(value);
                    }
                }
                sign_opts.server_auth = true;
                sign_opts.client_auth = false;

                auto material = _ca.sign_csr(csr_pem, sign_opts);
                order.certificate_pem = material.certificate_pem;
                order.chain_pem = material.certificate_pem + _ca.root_certificate_pem();
                order.status = "valid";

                send_json(res, order_json(order));
            } catch (const acme_problem& p) {
                send_problem(res, p);
            } catch (const std::exception& ex) {
                send_problem(res,
                             acme_problem("urn:ietf:params:acme:error:malformed", 400, ex.what()));
            }
        });

        _server.Post(R"(/cert/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto authed = authenticate(req);
                if (authed.account_url.empty()) {
                    throw acme_problem("urn:ietf:params:acme:error:unauthorized", 401,
                                       "requires an account");
                }
                std::string order_id = req.matches[1];
                std::lock_guard lock(_mu);
                auto it = _orders.find(order_id);
                if (it == _orders.end() || !it->second.chain_pem.has_value()) {
                    throw acme_problem("urn:ietf:params:acme:error:malformed", 404,
                                       "no certificate for order");
                }
                res.set_header("Replay-Nonce", issue_nonce());
                res.set_content(*it->second.chain_pem, "application/pem-certificate-chain");
            } catch (const acme_problem& p) {
                send_problem(res, p);
            }
        });
    }
};

}  // namespace raft::testing
