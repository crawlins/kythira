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

}  // namespace detail

// Requests a token from `config.as_token_endpoint` via the client
// credentials grant, and shapes the response into the credential struct
// `config.target_profile` names. Throws coap_credential_bootstrap_error on
// any network error, non-2xx status, or malformed/missing response field
// (Requirement 6.3) — never falls back to an unauthenticated session
// (Property 8).
inline auto run_ace_token_exchange(const ace_oauth_config& config)
    -> std::variant<psk_credentials, oscore_credentials> {
    auto [origin, path] = detail::split_origin_and_path(config.as_token_endpoint);

    httplib::Client client(origin);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);

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

}  // namespace kythira
