// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// ACE-OAuth (RFC 9200) credential provisioning (coap-transport-security
// spec, Requirement 6). A pure "populate credentials" step run before
// channel-security provider construction — it has no knowledge of
// coap_auth_mode and produces exactly the credential struct the target
// profile needs, after which the ordinary dtls_psk/oscore provider takes
// over unaware of how its credentials were obtained (Property 6).
//
// Uses the project's existing HTTP client (cpp-httplib) against the
// configured Authorization Server token endpoint, per Requirement 6.5 —
// not a second HTTP stack, and not a CoAP exchange (the AS token endpoint
// is a standard HTTPS call in ACE-OAuth, distinct from the CoAP resources
// the rest of this transport talks to).

#include <raft/coap_security.hpp>
#include <raft/http_origin_policy.hpp>

#include <httplib.h>
#include <boost/json.hpp>

#include <cctype>
#include <string>
#include <variant>

namespace kythira {

namespace detail {

inline auto hex_decode(const std::string& hex) -> std::vector<std::byte> {
    if (hex.size() % 2 != 0) {
        throw coap_credential_bootstrap_error("ace-oauth", "odd-length hex field in AS response");
    }
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        throw coap_credential_bootstrap_error("ace-oauth",
                                              "non-hex character in AS response field");
    };
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        out.push_back(static_cast<std::byte>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    }
    return out;
}

// Splits a full URL's origin ("scheme://host:port") from its path, since
// httplib::Client is constructed against the origin and Post() takes only
// the path.
inline auto split_origin_and_path(const std::string& url) -> std::pair<std::string, std::string> {
    auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw coap_credential_bootstrap_error("ace-oauth",
                                              "as_token_endpoint is not an absolute URL: " + url);
    }
    auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        return {url, "/"};
    }
    return {url.substr(0, path_start), url.substr(path_start)};
}

// The request carries client_secret and the reply carries key material, so
// the endpoint must be https; plain http passes only on a loopback host and
// only with allow_plain_http_loopback set. Anything else is a config error,
// raised before any network I/O.
inline auto require_secure_token_endpoint(const ace_oauth_config& config) -> void {
    const auto& url = config.as_token_endpoint;
    if (http_origin::is_https(url)) {
        return;
    }
    if (http_origin::is_loopback_http(url)) {
        if (config.allow_plain_http_loopback) {
            return;
        }
        throw coap_security_config_error(
            "security.ace_bootstrap.as_token_endpoint " + url +
            " is plain http; use https, or set allow_plain_http_loopback (tests only)");
    }
    throw coap_security_config_error("security.ace_bootstrap.as_token_endpoint " + url +
                                     " must be https (plain http is accepted only for a "
                                     "loopback host with allow_plain_http_loopback set)");
}

}  // namespace detail

// Requests a token from `config.as_token_endpoint` via the client
// credentials grant, and shapes the response into the credential struct
// `config.target_profile` names. Throws coap_security_config_error, before
// any network I/O, for an endpoint that is not https (see
// require_secure_token_endpoint), and coap_credential_bootstrap_error on
// any network error, non-2xx status, or malformed/missing response field
// (Requirement 6.3) — never falls back to an unauthenticated session
// (Property 8).
inline auto run_ace_token_exchange(const ace_oauth_config& config)
    -> std::variant<psk_credentials, oscore_credentials> {
    detail::require_secure_token_endpoint(config);
    auto [origin, path] = detail::split_origin_and_path(config.as_token_endpoint);

    httplib::Client client(origin);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client.enable_server_certificate_verification(true);
    if (config.as_ca_bundle_pem.has_value()) {
        client.load_ca_cert_store(config.as_ca_bundle_pem->data(), config.as_ca_bundle_pem->size());
    }
#else
    if (http_origin::is_https(origin)) {
        throw coap_credential_bootstrap_error(
            "ace-oauth",
            "https token endpoint needs cpp-httplib built with "
            "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
    }
#endif

    boost::json::object request_body{
        {"grant_type", "client_credentials"},
        {"client_id", config.client_id},
        {"client_secret", config.client_secret},
        {"scope", config.scope},
        {"ace_profile",
         config.target_profile == ace_target_profile::oscore ? "coap_oscore" : "coap_dtls"},
    };

    auto res = client.Post(path, boost::json::serialize(request_body), "application/json");
    if (!res) {
        throw coap_credential_bootstrap_error(
            "ace-oauth", "AS token request failed: " + httplib::to_string(res.error()));
    }
    if (res->status < 200 || res->status >= 300) {
        throw coap_credential_bootstrap_error(
            "ace-oauth", "AS returned status " + std::to_string(res->status) + ": " + res->body);
    }

    boost::json::value parsed;
    try {
        parsed = boost::json::parse(res->body);
    } catch (const std::exception& e) {
        throw coap_credential_bootstrap_error(
            "ace-oauth", std::string("AS response is not valid JSON: ") + e.what());
    }
    if (!parsed.is_object()) {
        throw coap_credential_bootstrap_error("ace-oauth", "AS response is not a JSON object");
    }
    const auto& obj = parsed.as_object();

    auto require_string = [&](const char* key) -> std::string {
        const auto* it = obj.find(key);
        if (it == obj.end() || !it->value().is_string()) {
            throw coap_credential_bootstrap_error(
                "ace-oauth", std::string("AS response missing required field '") + key + "'");
        }
        return std::string(it->value().as_string());
    };

    if (config.target_profile == ace_target_profile::dtls_psk) {
        psk_credentials creds;
        creds.identity = require_string("psk_identity");
        creds.key = detail::hex_decode(require_string("psk_key_hex"));
        return creds;
    }

    oscore_credentials creds;
    creds.sender_id = detail::hex_decode(require_string("sender_id_hex"));
    creds.recipient_id = detail::hex_decode(require_string("recipient_id_hex"));
    creds.master_secret = detail::hex_decode(require_string("master_secret_hex"));
    if (const auto* it = obj.find("master_salt_hex"); it != obj.end() && it->value().is_string()) {
        creds.master_salt = detail::hex_decode(std::string(it->value().as_string()));
    }
    if (const auto* it = obj.find("aead_algorithm"); it != obj.end() && it->value().is_string()) {
        creds.aead_algorithm = std::string(it->value().as_string());
    }
    creds.bootstrap_method = oscore_bootstrap::static_provisioned;
    return creds;
}

// The ACE step every CoAP backend runs at construction, after
// translate_legacy_fields() and before it chooses a channel. One function so
// the backends cannot drift apart again (coap-alternate-backend-security-parity
// Requirements 5 and 6).
//
// Checks that the requested profile and security.mode agree before any
// network I/O, then, if ace_bootstrap is set, runs the token exchange and
// replaces config.credentials with what the AS issued. A mismatch or an
// ambiguous config throws coap_security_config_error; a failed exchange lets
// coap_credential_bootstrap_error propagate. Never falls back to static or
// absent credentials.
inline auto resolve_ace_bootstrap(coap_security_config& config) -> void {
    if (!config.ace_bootstrap) {
        return;
    }
    const auto& ace = *config.ace_bootstrap;
    const auto wanted = ace.target_profile == ace_target_profile::oscore ? coap_auth_mode::oscore
                                                                         : coap_auth_mode::dtls_psk;
    if (config.mode != wanted) {
        throw coap_security_config_error(
            "security.ace_bootstrap.target_profile == " + to_string(wanted) +
            " requires security.mode == " + to_string(wanted) + ", but security.mode is " +
            to_string(config.mode));
    }
    // The AS always issues a static context, so an EDHOC request alongside
    // it would otherwise be dropped without a word.
    if (const auto* osc = std::get_if<oscore_credentials>(&config.credentials);
        osc != nullptr && osc->bootstrap_method == oscore_bootstrap::edhoc) {
        throw coap_security_config_error(
            "security.ace_bootstrap and an EDHOC bootstrap in security.credentials are both "
            "set; configure one way of obtaining the OSCORE context");
    }
    // The AS knows nothing about where this node keeps its OSCORE counters,
    // so the issued context inherits the configured ones. Dropping them would
    // restart the Sender Sequence Number at 0 whenever the AS hands back the
    // same Master Secret after a restart.
    std::string sequence_state_dir;
    bool volatile_sequence_state = false;
    if (const auto* osc = std::get_if<oscore_credentials>(&config.credentials); osc != nullptr) {
        sequence_state_dir = osc->sequence_state_dir;
        volatile_sequence_state = osc->volatile_sequence_state;
    }
    auto result = run_ace_token_exchange(ace);
    if (auto* osc = std::get_if<oscore_credentials>(&result); osc != nullptr) {
        osc->sequence_state_dir = std::move(sequence_state_dir);
        osc->volatile_sequence_state = volatile_sequence_state;
    }
    std::visit([&](auto&& creds) { config.credentials = std::forward<decltype(creds)>(creds); },
               std::move(result));
}

}  // namespace kythira
