#pragma once

/// @file oci_http_client.hpp
/// @brief The signed-JSON transport both OCI providers speak the REST API
///        through (Requirements 1.7-1.9).
///
/// Thin on purpose. Everything protocol-shaped lives elsewhere — signing in
/// `oci_signing.hpp`, HTTP in `httplib`, JSON in `boost::json` — so what is
/// left here is exactly the three things an OCI call needs that none of those
/// provide: which host a service lives on, that every request is signed before
/// it goes out, and that a non-2xx becomes a C++ exception carrying OCI's own
/// error fields rather than a status code the caller has to interpret.
///
/// Mirrors `acme_detail::make_client`'s timeout/TLS setup, since that is the
/// other place in this tree that drives a third-party REST API over `httplib`
/// and the two have no reason to differ.

#include <raft/oci_client_config.hpp>
#include <raft/oci_federation.hpp>
#include <raft/oci_signing.hpp>

#include <boost/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace kythira {

/// @brief Signed JSON requests against an OCI service endpoint.
///
/// One instance is safe to share across threads, and `request` is `const` to say
/// so: it holds no connection between calls, building a fresh `httplib::Client`
/// per request the way the ACME provider does, and reads nothing but the
/// immutable config. Building a client per call is deliberate — OCI
/// control-plane calls are infrequent (a quorum poll, a certificate issue), so
/// connection reuse would buy little and would add a pooling lifetime question
/// to a class whose whole value is being obvious. It is also what lets
/// `oci_instance_pool_quorum_manager` fan `GetInstance` out across a worker pool
/// (Requirement 5.2) without owning one client per worker. The one piece of
/// mutable state — Instance Principal's cached federation token — lives behind
/// `instance_principal_signer`'s own mutex, so the sharing claim survives it.
class oci_http_client {
public:
    explicit oci_http_client(oci_client_config cfg) : _cfg(std::move(cfg)) {
        // Constructed eagerly (though it does no I/O until the first sign)
        // so the auth-mode decision is taken exactly once, here, rather than
        // re-derived per request.
        if (_cfg.use_instance_principal) {
            _instance_principal = std::make_shared<oci_federation::instance_principal_signer>(
                oci_federation::instance_principal_signer::options_from(_cfg));
        }
    }

    /// @brief Send a signed request and return the parsed JSON response.
    ///
    /// @param service      OCI service name selecting the regional host —
    ///                     `"iaas"` for Compute/VCN, `"certificatesmanagement"`,
    ///                     `"certificates"`, `"vaults"`. Passed per call rather
    ///                     than fixed at construction because a single provider
    ///                     legitimately talks to more than one service.
    /// @param method       `"GET"`, `"POST"`, `"PUT"`, `"DELETE"`.
    /// @param path         Path *and query* exactly as it goes on the wire.
    ///                     This is what gets signed, so a caller that signs one
    ///                     spelling and sends another gets a 401 rather than a
    ///                     mismatch anything local can see.
    /// @param body         Request body; empty selects the bodyless signing
    ///                     form (see `oci_signing.hpp`).
    ///
    /// @return The response body parsed as JSON, or a JSON `null` when the
    ///         service answers 2xx with an empty body — which several OCI
    ///         delete/terminate operations do. Returning null rather than
    ///         throwing keeps "succeeded with nothing to say" distinct from
    ///         "answered with something unparseable".
    ///
    /// @throws std::runtime_error on a transport failure, on a non-2xx status
    ///         (carrying OCI's own `code` and `message`), or on a 2xx body that
    ///         is present but not parseable as JSON.
    /// @throws std::invalid_argument via `oci_signing::sign_request` when the
    ///         config lacks a field the selected auth mode requires.
    [[nodiscard]] auto request(std::string_view service, std::string_view method,
                               std::string_view path, const std::string& body = {}) const
        -> boost::json::value {
        const std::string host = host_for(service);
        auto result = send_once(host, method, path, body);

        // Requirement 1.9: exactly one retry on 429, and only on 429.
        //
        // One rather than a backoff loop because there is no SDK here to
        // inherit a retry policy from, and a general framework is a larger
        // thing than this spec needs — the AWS managers get the same behaviour
        // by delegating to their SDK's built-in policy. Bounded at one so a
        // throttled control plane cannot turn a quorum poll into an unbounded
        // stall; a second 429 is the caller's problem to see.
        if (result && result->status == 429) {
            std::this_thread::sleep_for(retry_delay_from(*result));
            result = send_once(host, method, path, body);
        }

        if (!result) {
            throw std::runtime_error(std::string("oci_http_client: ") + std::string(method) + " " +
                                     std::string(path) +
                                     " failed: " + httplib::to_string(result.error()));
        }
        if (result->status < 200 || result->status >= 300) {
            throw std::runtime_error(describe_error(*result, method, path));
        }
        if (result->body.empty()) {
            return {};
        }
        try {
            return boost::json::parse(result->body);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("oci_http_client: ") + std::string(method) + " " +
                                     std::string(path) +
                                     " returned an unparseable JSON body: " + e.what());
        }
    }

    /// @brief What a raw request answered with, unparsed.
    ///
    /// Deliberately not a `boost::json::value`: this exists precisely because
    /// `request()` cannot carry object bytes.
    struct raw_response {
        int status{};
        /// Response headers, lowercased on the way in, because OCI spells its
        /// ETag header `etag` and its digest `opc-content-md5` while other
        /// services capitalise — a plain map lookup on the wrong spelling
        /// silently yields an empty string, which is how task 0's probe once
        /// sent an empty precondition and read the result as "CAS unsupported".
        std::map<std::string, std::string> headers;
        std::string body;

        /// Case-insensitive header read; empty when absent.
        [[nodiscard]] auto header(std::string_view name) const -> std::string {
            std::string lowered(name);
            for (auto& c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const auto it = headers.find(lowered);
            return it == headers.end() ? std::string{} : it->second;
        }
    };

    /// @brief Send a signed request and return status, headers and body
    ///        **unparsed**.
    ///
    /// Additive: `request()` above is untouched, and every existing caller keeps
    /// its parsed-JSON behaviour. This path exists because parsing every
    /// response as JSON was a **control-plane assumption** — correct for
    /// Compute, Identity and the certificate services, and wrong for Object
    /// Storage, which is a data plane whose bodies are arbitrary bytes. A
    /// GetObject answering with a Raft snapshot is not JSON and must not be
    /// asked to be.
    ///
    /// Three differences from `request()`, each deliberate:
    ///
    /// - **The caller decides what a status means.** No status is turned into an
    ///   exception here, because "404" is an *answer* for a persistence load
    ///   path and an error for a control-plane GET, and only the caller knows
    ///   which it is. A transport failure — no response at all — still throws,
    ///   since there is nothing to hand back.
    /// - **The content type is a parameter**, and it is signed. Object bodies
    ///   are `application/octet-stream`; signing `application/json` while
    ///   sending something else yields `NotAuthenticated: Failed to verify the
    ///   HTTP(S) Signature`, which names neither side of the mismatch.
    /// - **No 429 retry.** `request()` retries once on 429 (Requirement 1.9);
    ///   here the caller owns retries, because `object_store_persistence_engine`
    ///   owns write retries and its idempotency argument depends on knowing
    ///   exactly what was re-sent.
    ///
    /// @throws std::runtime_error only when no response arrived at all.
    /// @param extra_headers Additional headers to put on the wire. These are
    ///                      **not** signed, and do not need to be: OCI's
    ///                      `Authorization` names the signed set explicitly in
    ///                      its `headers=` list, so anything outside it travels
    ///                      unsigned by design. Object Storage's conditional
    ///                      headers (`if-match`, `if-none-match`) and its
    ///                      `Content-MD5` all arrive this way.
    [[nodiscard]] auto request_raw(
        std::string_view service, std::string_view method, std::string_view path,
        const std::string& body = {}, std::string_view content_type = "application/octet-stream",
        const std::map<std::string, std::string>& extra_headers = {}) const -> raw_response {
        const std::string host = host_for(service);
        auto result = send_once(host, method, path, body, content_type, extra_headers);
        if (!result) {
            throw std::runtime_error(std::string("oci_http_client: ") + std::string(method) + " " +
                                     std::string(path) +
                                     " failed: " + httplib::to_string(result.error()));
        }
        raw_response out;
        out.status = result->status;
        out.body = result->body;
        for (const auto& [name, value] : result->headers) {
            std::string lowered = name;
            for (auto& c : lowered) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            out.headers.emplace(std::move(lowered), value);
        }
        return out;
    }

    /// @brief The regional host a service lives on.
    ///
    /// `endpoint_override` wins outright when set, and is the whole reason a
    /// mock server can stand in for OCI: it replaces the host for *every*
    /// service rather than per service, which is what a single test server
    /// needs. Public so a test can assert the derivation without issuing a
    /// request.
    /// The domain is **per service**, and cannot be derived from one template.
    ///
    /// Determined by probing a live tenancy, because guessing here is what
    /// produced two separate defects. The matrix, for the four services these
    /// providers use in `us-phoenix-1`:
    ///
    /// | service | `{svc}.{region}.oraclecloud.com` | `{svc}.{region}.oci.oraclecloud.com` |
    /// |---|---|---|
    /// | `iaas` | **works** | 404 on `GetVnic`/`GetSubnet` |
    /// | `identity` | works | works |
    /// | `certificatesmanagement` | **no DNS record** | works |
    /// | `certificates` | **no DNS record** | works |
    /// | `objectstorage` | **works** (401 unauthenticated) | **TLS hostname verification fails** |
    ///
    /// So Core Services (`iaas`) predates the `oci` label and must **not**
    /// carry it, while the certificate services only exist with it. `iaas` is
    /// the trap: the `oci` form resolves and serves *some* operations — enough
    /// that instances, instance pools and tags all worked — while 404ing
    /// `GetVnic`, which `provision_node` needs to return an address at all. A
    /// uniform template is wrong in one direction or the other whichever way it
    /// is written.
    ///
    /// The mock tier cannot see any of this: `endpoint_override` replaces the
    /// whole host, so the derivation is never exercised.
    [[nodiscard]] auto host_for(std::string_view service) const -> std::string {
        if (!_cfg.endpoint_override.empty()) {
            return _cfg.endpoint_override;
        }
        return std::string(service) + "." + _cfg.region + std::string(domain_suffix_for(service));
    }

    /// The realm suffix for one service — see @ref host_for for the evidence.
    ///
    /// Unknown services default to the `oci` form, which is the newer
    /// convention and what the certificate services use. A service that turns
    /// out to need the bare form belongs in the list, and until then
    /// `endpoint_override` is the escape hatch.
    ///
    /// **`objectstorage` was added to that list after it shipped in the wrong
    /// one**, and the way it was missed is worth keeping. Task 10's suite is
    /// thorough — 30 cases — but every one of them points `endpoint_override`
    /// at a local server, and this function's own comment already said the mock
    /// tier cannot see the derivation. So the defect was invisible until task
    /// 16 ran the client against the real service, where it failed on the very
    /// first request with "SSL server hostname verification failed" and OCI
    /// object storage was simply non-functional. Measured, both forms, in
    /// `us-phoenix-1`:
    ///
    ///     objectstorage.us-phoenix-1.oraclecloud.com      → HTTP 401
    ///     objectstorage.us-phoenix-1.oci.oraclecloud.com  → TLS verify failure
    ///
    /// The default is the trap, not the list: a service that is never named
    /// gets a plausible host that may not exist, and nothing local can tell.
    [[nodiscard]] static auto domain_suffix_for(std::string_view service) -> std::string_view {
        if (service == "iaas" || service == "objectstorage") {
            return ".oraclecloud.com";
        }
        return ".oci.oraclecloud.com";
    }

    /// @brief Every header a request will carry, `Host` included.
    ///
    /// Public because it is the one place the signed-versus-sent invariant can
    /// be checked without a network: the `Host` here is both what goes into the
    /// signature and what goes on the wire, and a test can assert they are the
    /// same string.
    ///
    /// **`Host` is set explicitly, and that is load-bearing.** It is one of the
    /// signed headers, so a request only verifies when the `Host` on the wire is
    /// byte-identical to the signed one — and leaving httplib to choose it does
    /// not give that. cpp-httplib builds its `Host` in `ClientImpl`'s
    /// constructor initializer list via
    /// `make_host_and_port_string(host_, port, is_ssl())`, where `is_ssl()` is
    /// *virtual*: during base-class construction it resolves to
    /// `ClientImpl::is_ssl()`, which is `false` even for an `SSLClient`. Port
    /// 443 is therefore not recognised as the default and httplib appends
    /// `:443`, while the signature covers the bare hostname. Every real OCI call
    /// failed that way — `NotAuthenticated: Failed to verify the HTTP(S)
    /// Signature` — and nothing local could see it, because the mock tier speaks
    /// plain http on a non-default port, where the two spellings agree.
    ///
    /// httplib only supplies `Host` when the caller has not, so setting it here
    /// wins, and removes the whole signs-one-thing-sends-another class rather
    /// than tracking httplib's idea of a default port.
    [[nodiscard]] auto prepared_headers(const std::string& host, std::string_view method,
                                        std::string_view path, const std::string& body,
                                        std::string_view content_type = "application/json") const
        -> std::map<std::string, std::string> {
        const std::string host_header = host_header_for(host);
        // The auth-mode fork, in exactly one place: Instance Principal signs
        // with the federated session token (oci_federation.hpp), API keys
        // with the config's own key. Everything after the signature — Host,
        // the content-type handling below — is identical.
        auto headers = _instance_principal
                           ? _instance_principal->sign(method, path, host_header, body,
                                                       std::time(nullptr), content_type)
                           : oci_signing::sign_request(_cfg, method, path, host_header, body,
                                                       std::time(nullptr), content_type);
        headers["Host"] = host_header;
        return headers;
    }

private:
    /// TLS unless the override names a plain-HTTP origin. A mock server on
    /// localhost is the only case that is not `https`, and spelling the scheme
    /// into `endpoint_override` is how a caller asks for it — nothing here
    /// silently downgrades a real regional endpoint.
    ///
    /// The construction is wrapped because cpp-httplib **throws** on an https
    /// origin when it was compiled without `CPPHTTPLIB_OPENSSL_SUPPORT`, and its
    /// own message ("'https' scheme is not supported.") names neither this
    /// client nor the switch that turns it on. Every OCI endpoint is https, so
    /// in such a build every real call fails here — while the whole test suite
    /// stays green, since it points `endpoint_override` at a plain-http local
    /// server. Rethrowing with the flag named is the difference between a
    /// one-line fix and an afternoon.
    [[nodiscard]] auto make_client(const std::string& host) const
        -> std::unique_ptr<httplib::Client> {
        const bool explicit_scheme = host.starts_with("http://") || host.starts_with("https://");
        const std::string origin = explicit_scheme ? host : "https://" + host;

        std::unique_ptr<httplib::Client> client;
        try {
            client = std::make_unique<httplib::Client>(origin);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "oci_http_client: could not create an HTTP client for " + origin + ": " + e.what() +
                " — OCI endpoints are https, which needs cpp-httplib compiled with "
                "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
        }
        client->set_connection_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
        client->set_read_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);

        // **Do not let httplib re-encode the request target.** Its default is
        // to percent-encode what it is given, and callers here hand it a target
        // that is *already* encoded — and, decisively, already **signed** in
        // that exact form. Re-encoding turns a signed `%2F` into a sent `%252F`
        // and OCI answers `401 NotAuthenticated`, because the signature covers
        // a request target the service never received.
        //
        // Honest provenance: this was found while chasing a live `401
        // NotAuthenticated` on LIST, and it is **not** what caused that — the
        // cause was `encode_query_value` encoding `/`, fixed there. Kept
        // regardless, because "sign exactly what you send" is the invariant the
        // whole signing scheme rests on, and leaving httplib free to rewrite
        // the target after signing is a latent version of the same bug waiting
        // for the first prefix that contains a space.
        //
        // Invisible below this tier for the same reason: the signature-verifying
        // mocks check the signature against the bytes that *arrived*, so both
        // sides re-encode consistently and agree. Only a service that signs
        // independently can disagree.
        client->set_url_encode(false);
        return client;
    }

    /// The `Host` header value, and what gets signed — the bare hostname, with
    /// any scheme and port stripped. Signing the origin instead would sign a
    /// string the service never sees.
    [[nodiscard]] static auto host_header_for(const std::string& host) -> std::string {
        std::string bare = host;
        for (const auto* prefix : {"https://", "http://"}) {
            if (bare.starts_with(prefix)) {
                bare.erase(0, std::string_view(prefix).size());
                break;
            }
        }
        if (const auto slash = bare.find('/'); slash != std::string::npos) {
            bare.erase(slash);
        }
        return bare;
    }

    [[nodiscard]] auto send_once(const std::string& host, std::string_view method,
                                 std::string_view path, const std::string& body,
                                 std::string_view content_type = json_content_type,
                                 const std::map<std::string, std::string>& extra_headers = {}) const
        -> httplib::Result {
        httplib::Headers headers;
        for (const auto& [name, value] : extra_headers) {
            headers.emplace(name, value);
        }
        for (const auto& [name, value] : prepared_headers(host, method, path, body, content_type)) {
            // `content-type` is deliberately withheld from httplib's header
            // map. Every cpp-httplib body overload takes the content type as a
            // separate argument and sets the header from it — there is no
            // `Put(path, headers, body)` — so adding ours too puts **two**
            // `Content-Type: application/json` headers on the wire, and OCI
            // answers 400 with an empty body, which is as unhelpful as it
            // sounds. The value is identical either way and the signature
            // covers the value, not the header's letter case, so letting
            // httplib own the header costs nothing.
            //
            // The mock tier cannot see this: cpp-httplib's *server* accepts
            // duplicate headers, and `get_header_value` returns the first, so
            // the POST test asserting `content-type == application/json`
            // passed with both present.
            if (name == "content-type") {
                continue;
            }
            headers.emplace(name, value);
        }

        auto client = make_client(host);
        const std::string target(path);
        const std::string content_type_arg(content_type);
        if (method == "GET") {
            return client->Get(target, headers);
        }
        if (method == "DELETE") {
            return client->Delete(target, headers, body, content_type_arg);
        }
        if (method == "PUT") {
            return client->Put(target, headers, body, content_type_arg);
        }
        if (method == "POST") {
            return client->Post(target, headers, body, content_type_arg);
        }
        throw std::invalid_argument("oci_http_client: unsupported HTTP method: " +
                                    std::string(method));
    }

    /// Requirement 1.9's delay: the service's own `retry-after` when it sent
    /// one, else one second.
    ///
    /// A malformed or absurd `retry-after` falls back to the default rather
    /// than being trusted — the header is remote input, and a parse that threw
    /// or a value of hours would each turn throttling into an outage. Capped
    /// for the same reason.
    [[nodiscard]] static auto retry_delay_from(const httplib::Response& response)
        -> std::chrono::seconds {
        constexpr std::chrono::seconds default_delay{1};
        constexpr std::chrono::seconds max_delay{30};

        const auto raw = response.get_header_value("retry-after");
        if (raw.empty()) {
            return default_delay;
        }
        try {
            std::size_t consumed = 0;
            const long long seconds = std::stoll(raw, &consumed);
            // Only a bare delta-seconds value is honoured. `retry-after` may
            // also carry an HTTP date, which needs a clock this class does not
            // have and a skew argument it should not be making.
            if (consumed != raw.size() || seconds <= 0) {
                return default_delay;
            }
            return std::min(std::chrono::seconds{seconds}, max_delay);
        } catch (const std::exception&) {
            return default_delay;
        }
    }

    /// Requirement 1.8: surface OCI's own `code`/`message`.
    ///
    /// The status is included alongside them rather than replaced by them,
    /// because the two answer different questions — `code` says what OCI
    /// thinks went wrong, the status says how the caller should treat it — and
    /// a body that is missing, empty, or not the documented shape is exactly
    /// the case where the status is all there is.
    [[nodiscard]] static auto describe_error(const httplib::Response& response,
                                             std::string_view method, std::string_view path)
        -> std::string {
        std::string detail;
        if (!response.body.empty()) {
            try {
                const auto parsed = boost::json::parse(response.body);
                if (const auto* obj = parsed.if_object(); obj != nullptr) {
                    const auto* code = obj->if_contains("code");
                    const auto* message = obj->if_contains("message");
                    if (code != nullptr && code->is_string()) {
                        detail = std::string(code->get_string());
                    }
                    if (message != nullptr && message->is_string()) {
                        detail += detail.empty() ? "" : ": ";
                        detail += std::string(message->get_string());
                    }
                }
            } catch (const std::exception&) {
                // Fall through to the raw body below: an unparseable error body
                // is still the most informative thing available, and hiding it
                // behind "could not parse" would discard the actual message.
            }
            if (detail.empty()) {
                detail = response.body;
            }
        }

        std::string out = "oci_http_client: " + std::string(method) + " " + std::string(path) +
                          " failed with HTTP " + std::to_string(response.status);
        if (!detail.empty()) {
            out += ": " + detail;
        }
        return out;
    }

    static constexpr const char* json_content_type = "application/json";

    oci_client_config _cfg;

    /// Non-null exactly when `_cfg.use_instance_principal`. A `shared_ptr` so
    /// `prepared_headers` can stay `const` while the signer maintains its
    /// token cache behind its own mutex.
    std::shared_ptr<oci_federation::instance_principal_signer> _instance_principal;
};

}  // namespace kythira
