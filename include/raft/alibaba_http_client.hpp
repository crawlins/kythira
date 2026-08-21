// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_http_client.hpp
/// @brief One-call-per-operation HTTP client for Alibaba Cloud's RPC-style
///        OpenAPI control plane (ESS, ECS, STS), signing with
///        `alibaba_signing` and speaking cpp-httplib.
///
/// The transport idiom is `oci_http_client`'s, deliberately: a fresh
/// `httplib::Client` per request (control-plane calls are infrequent; a
/// per-request client lets the quorum manager fan calls out without one
/// client per worker), `endpoint_override` replacing every derived host at
/// once so a single mock server can stand in for all services, exactly one
/// retry on throttling, and vendor error fields surfaced in the exception.
///
/// Three of that client's hard-won rules are carried over verbatim because
/// the failure modes are transport-level, not provider-level:
///
/// - **`Host` is set explicitly and is byte-identical to the signed value.**
///   cpp-httplib's own `Host` can carry `:443` (its `is_ssl()` is virtual
///   and false during base construction), and a signed-vs-sent mismatch is
///   an auth failure that names neither side.
/// - **`content-type` is withheld from the header map** — httplib sets it
///   from the body-overload argument, and supplying it twice puts two
///   headers on the wire.
/// - **The https construction is wrapped** so a build without
///   `CPPHTTPLIB_OPENSSL_SUPPORT` fails naming `CONFIG_HTTP_TRANSPORT_TLS`,
///   not with httplib's bare "'https' scheme is not supported."

#include <raft/alibaba_client_config.hpp>
#include <raft/alibaba_signing.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace kythira {

/// @brief Signs and sends RPC-style Alibaba OpenAPI calls, parsing the JSON
///        response and unwrapping vendor errors.
class alibaba_http_client {
public:
    explicit alibaba_http_client(alibaba_client_config cfg) : _cfg(std::move(cfg)) {}

    /// @brief One RPC call: `POST /` with the operation's parameters in the
    ///        query string (the RPC-style convention), `x-acs-action`/
    ///        `x-acs-version` naming the operation.
    ///
    /// @throws std::runtime_error on transport failure, on a non-2xx response
    ///         (carrying the vendor's `Code`/`Message`/`RequestId` when
    ///         present), or on a 2xx body that is not parseable JSON.
    /// @throws std::invalid_argument via `alibaba_signing::sign_request` when
    ///         the AccessKey pair is incomplete.
    [[nodiscard]] auto rpc(std::string_view service, std::string_view action,
                           std::string_view version,
                           const std::map<std::string, std::string>& params,
                           const std::string& body = {}) const -> boost::json::value {
        const std::string host = host_for(service);
        auto result = send_once(host, action, version, params, body);

        // Exactly one retry on throttling, and only on throttling — the
        // oci_http_client rationale: bounded so a throttled control plane
        // cannot turn a quorum poll into an unbounded stall.
        if (result && result->status == 429) {
            std::this_thread::sleep_for(retry_delay_from(*result));
            result = send_once(host, action, version, params, body);
        }

        if (!result) {
            throw std::runtime_error("alibaba_http_client: " + std::string(action) +
                                     " failed: " + httplib::to_string(result.error()));
        }
        if (result->status < 200 || result->status >= 300) {
            throw std::runtime_error(describe_error(*result, action));
        }
        if (result->body.empty()) {
            return {};
        }
        try {
            return boost::json::parse(result->body);
        } catch (const std::exception& e) {
            throw std::runtime_error("alibaba_http_client: " + std::string(action) +
                                     " returned an unparseable JSON body: " + e.what());
        }
    }

    /// @brief The host a service lives on. `endpoint_override` wins outright
    ///        for every service at once — the single-mock-server hook.
    ///
    /// Central defaults (`<service>.aliyuncs.com`) with ECS region-qualified:
    /// the vendor documents regional ECS endpoints, and Task 0.3 of the spec
    /// re-confirms this table against a live account when one exists. Public
    /// so a test can assert the derivation without issuing a request.
    [[nodiscard]] auto host_for(std::string_view service) const -> std::string {
        if (!_cfg.endpoint_override.empty()) {
            return _cfg.endpoint_override;
        }
        if (service == "ecs") {
            return "ecs." + _cfg.region + ".aliyuncs.com";
        }
        return std::string(service) + ".aliyuncs.com";
    }

    /// @brief Every header one call will carry, `Host` and `Authorization`
    ///        included — public so the signed-versus-sent invariant is
    ///        assertable without a network (the oci_http_client precedent).
    [[nodiscard]] auto prepared_headers(const std::string& host, std::string_view action,
                                        std::string_view version,
                                        const std::map<std::string, std::string>& params,
                                        const std::string& body,
                                        std::chrono::system_clock::time_point when,
                                        std::string_view nonce) const
        -> std::map<std::string, std::string> {
        const std::string host_header = host_header_for(host);
        auto headers = alibaba_signing::sign_request(_cfg, "POST", host_header, "/", params, action,
                                                     version, body, when, nonce);
        headers["Host"] = host_header;
        return headers;
    }

private:
    [[nodiscard]] auto make_client(const std::string& host) const
        -> std::unique_ptr<httplib::Client> {
        const bool explicit_scheme = host.starts_with("http://") || host.starts_with("https://");
        const std::string origin = explicit_scheme ? host : "https://" + host;
        std::unique_ptr<httplib::Client> client;
        try {
            client = std::make_unique<httplib::Client>(origin);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "alibaba_http_client: could not create an HTTP client for " + origin + ": " +
                e.what() +
                " — Alibaba endpoints are https, which needs cpp-httplib compiled with "
                "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
        }
        client->set_connection_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
        client->set_read_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
        return client;
    }

    /// Bare hostname — what is signed and what goes on the wire, scheme and
    /// port stripped for the signed-vs-sent identity `prepared_headers`
    /// documents.
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

    [[nodiscard]] auto send_once(const std::string& host, std::string_view action,
                                 std::string_view version,
                                 const std::map<std::string, std::string>& params,
                                 const std::string& body) const -> httplib::Result {
        httplib::Headers headers;
        for (const auto& [name, value] :
             prepared_headers(host, action, version, params, body, std::chrono::system_clock::now(),
                              make_nonce())) {
            // content-type withheld: httplib sets it from the body overload's
            // argument (see the file comment).
            if (name == "content-type") {
                continue;
            }
            // The lowercase "host" is part of the SIGNED set (the canonical
            // request needs it) but must not also go on the wire, because
            // "Host" is added below from the same value: two Host headers
            // violate HTTP/1.1's MUST NOT, and while httplib's own server
            // tolerates it by taking the first, a stricter front end is
            // entitled to answer 400. Signing it and sending it are separate
            // concerns; only one belongs in the header map.
            if (name == "host") {
                continue;
            }
            headers.emplace(name, value);
        }

        auto client = make_client(host);
        const std::string target = "/" + query_suffix(params);
        return client->Post(target, headers, body, json_content_type);
    }

    /// The request-target's query part — the same percent-encoding the
    /// signature used, because the canonical query string and the sent query
    /// string must agree byte for byte.
    [[nodiscard]] static auto query_suffix(const std::map<std::string, std::string>& params)
        -> std::string {
        if (params.empty()) {
            return {};
        }
        return "?" + alibaba_signing::canonical_query_string(params);
    }

    /// A fresh 32-hex-char nonce per request (`x-acs-signature-nonce` is the
    /// vendor's replay guard). `random_device` seeds a per-call engine; the
    /// nonce needs uniqueness, not cryptographic strength — the signature
    /// itself carries the secret.
    [[nodiscard]] static auto make_nonce() -> std::string {
        static constexpr const char* k_hex = "0123456789abcdef";
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<int> dist(0, 15);
        std::string nonce;
        nonce.reserve(32);
        for (int i = 0; i < 32; ++i) {
            nonce.push_back(k_hex[dist(gen)]);
        }
        return nonce;
    }

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
            if (consumed != raw.size() || seconds <= 0) {
                return default_delay;
            }
            return std::min(std::chrono::seconds{seconds}, max_delay);
        } catch (const std::exception&) {
            return default_delay;
        }
    }

    /// Vendor errors are JSON `{"Code": ..., "Message": ..., "RequestId": ...}`
    /// (capitalised, unlike OCI's lowercase fields). Status kept alongside —
    /// the oci_http_client rationale.
    [[nodiscard]] static auto describe_error(const httplib::Response& response,
                                             std::string_view action) -> std::string {
        std::string detail;
        if (!response.body.empty()) {
            try {
                const auto parsed = boost::json::parse(response.body);
                if (const auto* obj = parsed.if_object(); obj != nullptr) {
                    for (const auto* field : {"Code", "Message", "RequestId"}) {
                        if (const auto* v = obj->if_contains(field);
                            v != nullptr && v->is_string()) {
                            detail += detail.empty() ? "" : ": ";
                            detail += std::string(v->get_string());
                        }
                    }
                }
            } catch (const std::exception&) {
                // Fall through to the raw body: an unparseable error body is
                // still the most informative thing available.
            }
            if (detail.empty()) {
                detail = response.body;
            }
        }
        std::string out = "alibaba_http_client: " + std::string(action) + " failed with HTTP " +
                          std::to_string(response.status);
        if (!detail.empty()) {
            out += ": " + detail;
        }
        return out;
    }

    static constexpr const char* json_content_type = "application/json";

    alibaba_client_config _cfg;
};

}  // namespace kythira
