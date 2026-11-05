// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Cloud credentials from a Kythira OIDC token
// (`.kiro/specs/iam-compatible-auth/` Requirement 2, task 3).
//
// A node holding a Kythira-CA client certificate asks the CA cluster for an
// identity token (POST /v1/oidc/token, task 2) and trades it with a cloud's
// token service for short-lived credentials. No cloud key is stored.
//
//   kythira_oidc_token_source   one cached token per audience
//   aws_web_identity_source     sts:AssumeRoleWithWebIdentity
//   gcp_workload_identity_source  STS token exchange, then optional
//                               service-account impersonation
//
// Every source shares one caching rule (refreshing_value): serve the cached
// value until 80% of its lifetime has passed; then the first caller
// refreshes while everyone else keeps getting the still-valid value; a
// caller that finds nothing valid waits for the single in-flight refresh.
// A failed refresh keeps the old value in service until it really expires
// and backs off before the next attempt.
//
// The interface is synchronous on purpose (a deviation from the design's
// future-returning concept, recorded in tasks.md): every consumer is an SDK
// credential hook (Aws::Auth::AWSCredentialsProvider::GetAWSCredentials and
// its peers), which is itself synchronous, and the hot path is a cache hit
// under one mutex.
//
// Request building and response parsing are pure functions, so tests check
// the exact wire format against local mock endpoints.

#include <boost/json.hpp>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace kythira::cloud_credentials {

using clock = std::chrono::system_clock;

/// Thrown when a token or credential request is refused or malformed. The
/// message never contains a token or secret.
class exchange_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Caching
// ---------------------------------------------------------------------------

struct refresh_policy {
    /// Refresh once this fraction of the lifetime has passed (2.1: "less than
    /// 20% left").
    double refresh_at_fraction{0.8};
    std::chrono::milliseconds first_retry{std::chrono::seconds(1)};
    std::chrono::milliseconds max_retry{std::chrono::minutes(1)};
};

/// A value with an expiry, refreshed by `fetch` under the rule above.
/// Thread-safe. `now` is injectable for tests.
template<typename T> class refreshing_value {
public:
    struct timed {
        T value;
        clock::time_point expires_at;
    };
    using fetch_fn = std::function<timed()>;
    using now_fn = std::function<clock::time_point()>;

    explicit refreshing_value(
        fetch_fn fetch, refresh_policy policy = {}, now_fn now = [] { return clock::now(); })
        : _fetch(std::move(fetch)), _policy(policy), _now(std::move(now)) {}

    /// The current value. Throws exchange_error (or whatever `fetch` throws)
    /// only when no unexpired value exists and the refresh fails.
    [[nodiscard]] auto get() -> T {
        std::unique_lock lock(_mu);
        for (;;) {
            const auto now = _now();
            const bool valid = _current && now < _current->expires_at;
            const bool fresh = valid && now < _refresh_at;
            if (fresh) {
                return _current->value;
            }
            if (_refreshing) {
                if (valid) {
                    return _current->value;  // someone else is refreshing
                }
                _cv.wait(lock, [&] { return !_refreshing; });
                continue;
            }
            if (valid && now < _retry_after) {
                return _current->value;  // backing off after a failure
            }
            if (!valid && now < _retry_after && _last_error) {
                std::rethrow_exception(_last_error);
            }
            _refreshing = true;
            lock.unlock();
            std::optional<timed> got;
            std::exception_ptr err;
            try {
                got = _fetch();
            } catch (...) {
                err = std::current_exception();
            }
            lock.lock();
            _refreshing = false;
            const auto after = _now();
            const bool fetched = got.has_value();
            if (fetched) {
                const auto life = got->expires_at - after;
                _refresh_at = after + std::chrono::duration_cast<clock::duration>(
                                          life * _policy.refresh_at_fraction);
                _current = std::move(got);
                _failures = 0;
                _retry_after = {};
                _last_error = nullptr;
            } else {
                auto delay = _policy.first_retry;
                for (int i = 0; i < _failures && delay < _policy.max_retry; ++i) {
                    delay *= 2;
                }
                delay = std::min(delay, _policy.max_retry);
                _retry_after = after + delay;
                ++_failures;
                _last_error = err;
            }
            _cv.notify_all();
            if (!fetched && !(_current && after < _current->expires_at)) {
                std::rethrow_exception(err);
            }
            return _current->value;
        }
    }

    [[nodiscard]] auto consecutive_failures() const -> int {
        std::lock_guard lock(_mu);
        return _failures;
    }

private:
    fetch_fn _fetch;
    refresh_policy _policy;
    now_fn _now;
    mutable std::mutex _mu;
    std::condition_variable _cv;
    std::optional<timed> _current;
    clock::time_point _refresh_at{};
    clock::time_point _retry_after{};
    bool _refreshing{false};
    int _failures{0};
    std::exception_ptr _last_error;
};

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace detail {

/// application/x-www-form-urlencoded, RFC 3986 unreserved kept as is.
[[nodiscard]] inline auto form_escape(std::string_view in) -> std::string {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

[[nodiscard]] inline auto form_body(
    std::initializer_list<std::pair<std::string_view, std::string_view>> fields) -> std::string {
    std::string out;
    for (const auto& [k, v] : fields) {
        if (!out.empty()) {
            out += '&';
        }
        out += form_escape(k);
        out += '=';
        out += form_escape(v);
    }
    return out;
}

/// "2026-11-05T06:00:00Z", with optional fractional seconds. std::nullopt
/// for anything else.
[[nodiscard]] inline auto parse_rfc3339_utc(std::string_view s)
    -> std::optional<clock::time_point> {
    if (s.size() < 20 || s.back() != 'Z') {
        return std::nullopt;
    }
    std::tm tm{};
    int consumed = 0;
    const std::string text(s);
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &consumed) != 6 ||
        consumed != 19) {
        return std::nullopt;
    }
    std::string_view rest = std::string_view(text).substr(19);
    if (rest.starts_with('.')) {
        rest.remove_prefix(1);
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9') {
            rest.remove_prefix(1);
        }
    }
    if (rest != "Z") {
        return std::nullopt;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return clock::from_time_t(timegm(&tm));
}

/// The text of the first `<tag>…</tag>` inside `xml` (no attributes, no
/// nesting of the same tag), XML entities decoded. Enough for the fixed
/// shape of an STS response.
[[nodiscard]] inline auto xml_text(std::string_view xml, std::string_view tag)
    -> std::optional<std::string> {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const auto b = xml.find(open);
    if (b == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = b + open.size();
    const auto e = xml.find(close, start);
    if (e == std::string_view::npos) {
        return std::nullopt;
    }
    std::string raw(xml.substr(start, e - start));
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            continue;
        }
        static constexpr std::pair<std::string_view, char> entities[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool matched = false;
        for (const auto& [name, ch] : entities) {
            if (std::string_view(raw).substr(i).starts_with(name)) {
                out.push_back(ch);
                i += name.size() - 1;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out.push_back('&');
        }
    }
    return out;
}

/// POSTs to `base_url` + `path` (base is "https://host[:port]" or, for tests,
/// "http://…"). Returns status and body; throws exchange_error on a
/// transport failure.
struct http_reply {
    int status{0};
    std::string body;
};

[[nodiscard]] inline auto post(const std::string& base_url, const std::string& path,
                               const httplib::Headers& headers, const std::string& body,
                               const std::string& content_type,
                               std::chrono::seconds timeout = std::chrono::seconds(30))
    -> http_reply {
    httplib::Client client(base_url);
    client.set_connection_timeout(timeout);
    client.set_read_timeout(timeout);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    client.enable_server_certificate_verification(true);
#endif
    auto res = client.Post(path, headers, body, content_type);
    if (!res) {
        throw exchange_error("request to " + base_url + path +
                             " failed: " + httplib::to_string(res.error()));
    }
    return {res->status, res->body};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Kythira OIDC token source (2.1)
// ---------------------------------------------------------------------------

struct oidc_token {
    std::string token;
    clock::time_point expires_at;
};

struct oidc_token_source_config {
    /// The CA cluster's client API, "https://host:port". A follower answers
    /// with a 308 to the leader, which the client follows.
    std::string ca_url;
    std::string client_cert_path;  // the node's or workload's Kythira-CA certificate
    std::string client_key_path;
    std::string ca_bundle_path;                    // verifies the CA cluster's listener
    std::optional<std::chrono::seconds> lifetime;  // default: the issuer's (1 hour)
    std::chrono::seconds timeout{std::chrono::seconds(10)};
};

/// Parses the token endpoint's 200 body.
[[nodiscard]] inline auto parse_oidc_token_response(std::string_view body, clock::time_point now)
    -> oidc_token {
    boost::system::error_code ec;
    auto v = boost::json::parse(body, ec);
    if (ec || !v.is_object()) {
        throw exchange_error("oidc token response is not a JSON object");
    }
    const auto& o = v.as_object();
    const auto* token = o.if_contains("token");
    const auto* expires_in = o.if_contains("expires_in");
    if (token == nullptr || !token->is_string() || token->as_string().empty() ||
        expires_in == nullptr || !expires_in->is_int64() || expires_in->as_int64() <= 0) {
        throw exchange_error("oidc token response lacks token or expires_in");
    }
    return {std::string(token->as_string()), now + std::chrono::seconds(expires_in->as_int64())};
}

/// Fetches one token for `audience` over mTLS.
[[nodiscard]] inline auto fetch_oidc_token(const oidc_token_source_config& cfg,
                                           const std::string& audience) -> oidc_token {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    std::string body = detail::form_body({{"audience", audience}});
    if (cfg.lifetime) {
        body += "&lifetime=" + std::to_string(cfg.lifetime->count());
    }
    // A follower answers 308 with the leader's address. httplib's own
    // redirect handling builds a fresh client for another host without the
    // client certificate, so follow it here, keeping the certificate.
    std::string base = cfg.ca_url;
    for (int hop = 0;; ++hop) {
        httplib::Client client(base, cfg.client_cert_path, cfg.client_key_path);
        if (!cfg.ca_bundle_path.empty()) {
            client.set_ca_cert_path(cfg.ca_bundle_path);
        }
        client.enable_server_certificate_verification(true);
        client.set_connection_timeout(cfg.timeout);
        client.set_read_timeout(cfg.timeout);
        auto res = client.Post("/v1/oidc/token", body, "application/x-www-form-urlencoded");
        if (!res) {
            throw exchange_error("oidc token request to " + base +
                                 " failed: " + httplib::to_string(res.error()));
        }
        if ((res->status == 307 || res->status == 308) && hop < 3) {
            const auto location = res->get_header_value("Location");
            const auto scheme_end = location.find("://");
            const auto path_start = scheme_end == std::string::npos
                                        ? std::string::npos
                                        : location.find('/', scheme_end + 3);
            if (!location.starts_with("https://") || path_start == std::string::npos ||
                location.substr(path_start) != "/v1/oidc/token") {
                throw exchange_error("oidc token request redirected somewhere unexpected");
            }
            base = location.substr(0, path_start);
            continue;
        }
        if (res->status != 200) {
            throw exchange_error("oidc token request refused with " + std::to_string(res->status) +
                                 ": " + res->body.substr(0, 200));
        }
        return parse_oidc_token_response(res->body, clock::now());
    }
#else
    (void)cfg;
    (void)audience;
    throw exchange_error("built without TLS support: the OIDC token endpoint needs mTLS");
#endif
}

/// One cached token per audience (Requirement 2.1).
class kythira_oidc_token_source {
public:
    using fetch_fn = std::function<oidc_token(const std::string& audience)>;

    explicit kythira_oidc_token_source(oidc_token_source_config cfg, refresh_policy policy = {})
        : kythira_oidc_token_source(
              [cfg = std::move(cfg)](const std::string& aud) { return fetch_oidc_token(cfg, aud); },
              policy) {}

    /// For tests and for callers that reach the endpoint some other way.
    explicit kythira_oidc_token_source(
        fetch_fn fetch, refresh_policy policy = {},
        refreshing_value<std::string>::now_fn now = [] { return clock::now(); })
        : _fetch(std::move(fetch)), _policy(policy), _now(std::move(now)) {}

    [[nodiscard]] auto token(const std::string& audience) -> std::string {
        refreshing_value<std::string>* slot = nullptr;
        {
            std::lock_guard lock(_mu);
            auto it = _by_audience.find(audience);
            if (it == _by_audience.end()) {
                auto fetch = _fetch;
                it = _by_audience
                         .emplace(audience, std::make_unique<refreshing_value<std::string>>(
                                                [fetch, audience] {
                                                    auto t = fetch(audience);
                                                    return refreshing_value<std::string>::timed{
                                                        std::move(t.token), t.expires_at};
                                                },
                                                _policy, _now))
                         .first;
            }
            slot = it->second.get();
        }
        return slot->get();
    }

private:
    fetch_fn _fetch;
    refresh_policy _policy;
    refreshing_value<std::string>::now_fn _now;
    std::mutex _mu;
    std::map<std::string, std::unique_ptr<refreshing_value<std::string>>> _by_audience;
};

// ---------------------------------------------------------------------------
// Credentials and the source concept
// ---------------------------------------------------------------------------

struct aws_credentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
    clock::time_point expires_at;
};

struct gcp_access_token {
    std::string access_token;
    clock::time_point expires_at;
};

/// Anything that hands out current credentials of type `Credentials`,
/// refreshing them itself.
template<typename S, typename Credentials>
concept cloud_credential_source = requires(S& s) {
    { s.current() } -> std::same_as<Credentials>;
};

// ---------------------------------------------------------------------------
// AWS: sts:AssumeRoleWithWebIdentity (2.2)
// ---------------------------------------------------------------------------

struct aws_web_identity_config {
    std::string role_arn;
    std::string role_session_name{"kythira"};
    std::string audience{"sts.amazonaws.com"};
    std::chrono::seconds duration{std::chrono::hours(1)};
    /// Regional STS endpoint, e.g. "https://sts.us-east-1.amazonaws.com".
    std::string sts_endpoint{"https://sts.amazonaws.com"};
};

/// The form body of the (unsigned) Query call.
[[nodiscard]] inline auto assume_role_with_web_identity_body(const aws_web_identity_config& cfg,
                                                             std::string_view web_identity_token)
    -> std::string {
    const auto duration = std::to_string(cfg.duration.count());
    return detail::form_body({{"Action", "AssumeRoleWithWebIdentity"},
                              {"Version", "2011-06-15"},
                              {"RoleArn", cfg.role_arn},
                              {"RoleSessionName", cfg.role_session_name},
                              {"WebIdentityToken", web_identity_token},
                              {"DurationSeconds", duration}});
}

/// Parses the XML response; throws exchange_error with STS's own code and
/// message for an error response.
[[nodiscard]] inline auto parse_assume_role_with_web_identity(int status, std::string_view xml)
    -> aws_credentials {
    if (status != 200) {
        const auto code = detail::xml_text(xml, "Code").value_or("HTTP " + std::to_string(status));
        const auto message = detail::xml_text(xml, "Message").value_or("");
        throw exchange_error("AssumeRoleWithWebIdentity refused: " + code +
                             (message.empty() ? "" : ": " + message));
    }
    const auto cred_start = xml.find("<Credentials>");
    if (cred_start == std::string_view::npos) {
        throw exchange_error("AssumeRoleWithWebIdentity response has no Credentials");
    }
    const auto creds = xml.substr(cred_start);
    auto akid = detail::xml_text(creds, "AccessKeyId");
    auto secret = detail::xml_text(creds, "SecretAccessKey");
    auto token = detail::xml_text(creds, "SessionToken");
    auto exp = detail::xml_text(creds, "Expiration");
    if (!akid || !secret || !token || !exp || akid->empty() || secret->empty()) {
        throw exchange_error("AssumeRoleWithWebIdentity response lacks a credential field");
    }
    auto when = detail::parse_rfc3339_utc(*exp);
    if (!when) {
        throw exchange_error("AssumeRoleWithWebIdentity Expiration is not RFC 3339");
    }
    return {std::move(*akid), std::move(*secret), std::move(*token), *when};
}

class aws_web_identity_source {
public:
    aws_web_identity_source(
        std::shared_ptr<kythira_oidc_token_source> tokens, aws_web_identity_config cfg,
        refresh_policy policy = {},
        refreshing_value<aws_credentials>::now_fn now = [] { return clock::now(); })
        : _cache(
              [tokens = std::move(tokens), cfg = std::move(cfg)] {
                  const auto jwt = tokens->token(cfg.audience);
                  auto reply = detail::post(cfg.sts_endpoint, "/", {{"Accept", "application/xml"}},
                                            assume_role_with_web_identity_body(cfg, jwt),
                                            "application/x-www-form-urlencoded");
                  auto creds = parse_assume_role_with_web_identity(reply.status, reply.body);
                  const auto exp = creds.expires_at;
                  return refreshing_value<aws_credentials>::timed{std::move(creds), exp};
              },
              policy, std::move(now)) {}

    [[nodiscard]] auto current() -> aws_credentials { return _cache.get(); }

private:
    refreshing_value<aws_credentials> _cache;
};

// ---------------------------------------------------------------------------
// GCP: STS token exchange, then optional impersonation (2.2)
// ---------------------------------------------------------------------------

inline constexpr std::string_view k_gcp_token_exchange_grant =
    "urn:ietf:params:oauth:grant-type:token-exchange";
inline constexpr std::string_view k_gcp_access_token_type =
    "urn:ietf:params:oauth:token-type:access_token";
inline constexpr std::string_view k_jwt_token_type = "urn:ietf:params:oauth:token-type:jwt";
inline constexpr std::string_view k_gcp_cloud_platform_scope =
    "https://www.googleapis.com/auth/cloud-platform";

struct gcp_workload_identity_config {
    /// The provider's full resource name, which is also the OIDC audience:
    /// "//iam.googleapis.com/projects/<n>/locations/global/workloadIdentityPools/<pool>/providers/<p>".
    std::string provider_audience;
    /// Empty: use the federated token directly. Otherwise the service
    /// account to impersonate.
    std::string service_account_email;
    std::string scope{k_gcp_cloud_platform_scope};
    std::chrono::seconds impersonation_lifetime{std::chrono::hours(1)};
    std::string sts_endpoint{"https://sts.googleapis.com"};
    std::string iamcredentials_endpoint{"https://iamcredentials.googleapis.com"};
};

[[nodiscard]] inline auto gcp_sts_exchange_body(const gcp_workload_identity_config& cfg,
                                                std::string_view subject_token) -> std::string {
    return detail::form_body({{"grant_type", k_gcp_token_exchange_grant},
                              {"audience", cfg.provider_audience},
                              {"scope", cfg.scope},
                              {"requested_token_type", k_gcp_access_token_type},
                              {"subject_token", subject_token},
                              {"subject_token_type", k_jwt_token_type}});
}

[[nodiscard]] inline auto parse_gcp_sts_exchange(int status, std::string_view body,
                                                 clock::time_point now) -> gcp_access_token {
    boost::system::error_code ec;
    auto v = boost::json::parse(body, ec);
    if (ec || !v.is_object()) {
        throw exchange_error("GCP STS response is not JSON (HTTP " + std::to_string(status) + ")");
    }
    const auto& o = v.as_object();
    if (status != 200) {
        std::string what = "GCP STS refused (HTTP " + std::to_string(status) + ")";
        if (const auto* e = o.if_contains("error"); e != nullptr && e->is_string()) {
            what += ": " + std::string(e->as_string());
        }
        if (const auto* d = o.if_contains("error_description"); d != nullptr && d->is_string()) {
            what += ": " + std::string(d->as_string());
        }
        throw exchange_error(what);
    }
    const auto* tok = o.if_contains("access_token");
    const auto* exp = o.if_contains("expires_in");
    if (tok == nullptr || !tok->is_string() || tok->as_string().empty() || exp == nullptr ||
        !exp->is_int64() || exp->as_int64() <= 0) {
        throw exchange_error("GCP STS response lacks access_token or expires_in");
    }
    return {std::string(tok->as_string()), now + std::chrono::seconds(exp->as_int64())};
}

[[nodiscard]] inline auto gcp_generate_access_token_path(std::string_view service_account)
    -> std::string {
    return "/v1/projects/-/serviceAccounts/" + detail::form_escape(service_account) +
           ":generateAccessToken";
}

[[nodiscard]] inline auto gcp_generate_access_token_body(const gcp_workload_identity_config& cfg)
    -> std::string {
    return boost::json::serialize(boost::json::object{
        {"scope", boost::json::array{boost::json::string(cfg.scope)}},
        {"lifetime", std::to_string(cfg.impersonation_lifetime.count()) + "s"}});
}

[[nodiscard]] inline auto parse_gcp_generate_access_token(int status, std::string_view body)
    -> gcp_access_token {
    boost::system::error_code ec;
    auto v = boost::json::parse(body, ec);
    if (status != 200) {
        std::string what = "generateAccessToken refused (HTTP " + std::to_string(status) + ")";
        if (!ec && v.is_object()) {
            if (const auto* e = v.as_object().if_contains("error");
                e != nullptr && e->is_object()) {
                if (const auto* m = e->as_object().if_contains("message");
                    m != nullptr && m->is_string()) {
                    what += ": " + std::string(m->as_string());
                }
            }
        }
        throw exchange_error(what);
    }
    if (ec || !v.is_object()) {
        throw exchange_error("generateAccessToken response is not a JSON object");
    }
    const auto& o = v.as_object();
    const auto* tok = o.if_contains("accessToken");
    const auto* exp = o.if_contains("expireTime");
    if (tok == nullptr || !tok->is_string() || tok->as_string().empty() || exp == nullptr ||
        !exp->is_string()) {
        throw exchange_error("generateAccessToken response lacks accessToken or expireTime");
    }
    auto when = detail::parse_rfc3339_utc(exp->as_string());
    if (!when) {
        throw exchange_error("generateAccessToken expireTime is not RFC 3339");
    }
    return {std::string(tok->as_string()), *when};
}

class gcp_workload_identity_source {
public:
    gcp_workload_identity_source(
        std::shared_ptr<kythira_oidc_token_source> tokens, gcp_workload_identity_config cfg,
        refresh_policy policy = {},
        refreshing_value<gcp_access_token>::now_fn now = [] { return clock::now(); })
        : _cache(
              [tokens = std::move(tokens), cfg = std::move(cfg), now] {
                  const auto jwt = tokens->token(cfg.provider_audience);
                  auto sts = detail::post(cfg.sts_endpoint, "/v1/token", {},
                                          gcp_sts_exchange_body(cfg, jwt),
                                          "application/x-www-form-urlencoded");
                  auto federated = parse_gcp_sts_exchange(sts.status, sts.body, now());
                  if (cfg.service_account_email.empty()) {
                      const auto exp = federated.expires_at;
                      return refreshing_value<gcp_access_token>::timed{std::move(federated), exp};
                  }
                  auto imp = detail::post(cfg.iamcredentials_endpoint,
                                          gcp_generate_access_token_path(cfg.service_account_email),
                                          {{"Authorization", "Bearer " + federated.access_token}},
                                          gcp_generate_access_token_body(cfg), "application/json");
                  auto token = parse_gcp_generate_access_token(imp.status, imp.body);
                  const auto exp = token.expires_at;
                  return refreshing_value<gcp_access_token>::timed{std::move(token), exp};
              },
              policy, now) {}

    [[nodiscard]] auto current() -> gcp_access_token { return _cache.get(); }

private:
    refreshing_value<gcp_access_token> _cache;
};

static_assert(cloud_credential_source<aws_web_identity_source, aws_credentials>);
static_assert(cloud_credential_source<gcp_workload_identity_source, gcp_access_token>);

}  // namespace kythira::cloud_credentials
