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
#include <raft/oci_signing.hpp>

#include <boost/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
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
/// (Requirement 5.2) without owning one client per worker.
class oci_http_client {
public:
    explicit oci_http_client(oci_client_config cfg) : _cfg(std::move(cfg)) {}

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

    /// @brief The regional host a service lives on.
    ///
    /// `endpoint_override` wins outright when set, and is the whole reason a
    /// mock server can stand in for OCI: it replaces the host for *every*
    /// service rather than per service, which is what a single test server
    /// needs. Public so a test can assert the derivation without issuing a
    /// request.
    [[nodiscard]] auto host_for(std::string_view service) const -> std::string {
        if (!_cfg.endpoint_override.empty()) {
            return _cfg.endpoint_override;
        }
        return std::string(service) + "." + _cfg.region + ".oraclecloud.com";
    }

private:
    /// TLS unless the override names a plain-HTTP origin. A mock server on
    /// localhost is the only case that is not `https`, and spelling the scheme
    /// into `endpoint_override` is how a caller asks for it — nothing here
    /// silently downgrades a real regional endpoint.
    [[nodiscard]] auto make_client(const std::string& host) const
        -> std::unique_ptr<httplib::Client> {
        const bool explicit_scheme = host.starts_with("http://") || host.starts_with("https://");
        const std::string origin = explicit_scheme ? host : "https://" + host;

        auto client = std::make_unique<httplib::Client>(origin);
        client->set_connection_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
        client->set_read_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
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
                                 std::string_view path, const std::string& body) const
        -> httplib::Result {
        auto signed_headers =
            oci_signing::sign_request(_cfg, method, path, host_header_for(host), body);

        httplib::Headers headers;
        for (const auto& [name, value] : signed_headers) {
            headers.emplace(name, value);
        }

        auto client = make_client(host);
        const std::string target(path);
        if (method == "GET") {
            return client->Get(target, headers);
        }
        if (method == "DELETE") {
            return client->Delete(target, headers, body, json_content_type);
        }
        if (method == "PUT") {
            return client->Put(target, headers, body, json_content_type);
        }
        if (method == "POST") {
            return client->Post(target, headers, body, json_content_type);
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
};

}  // namespace kythira
