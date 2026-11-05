// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Publishing the OIDC discovery document and JWKS to an object-store bucket
// (`.kiro/specs/iam-compatible-auth/` Requirement 1.9, task 2).
//
// AWS and GCP fetch an issuer's documents from `<iss>/.well-known/...` on a
// publicly trusted HTTPS origin. Where the CA cluster cannot be that origin,
// the operator points --oidc-issuer-url at a bucket's public HTTPS URL and
// gives --oidc-publish-url as the same bucket in the store's own scheme. The
// leader then uploads both documents through the existing object-store
// clients whenever the JWKS changes, so `iss`, the discovery URL and
// `jwks_uri` all keep naming the issuer URL and nothing else moves.
//
// The JWKS changes without any command being applied: a successor appears
// at its publish_after and a retiring key disappears at its retire_after.
// So the trigger is "the document differs from the last one uploaded", not
// "a key command committed". A periodic refresh puts back objects someone
// deleted, and failures back off exponentially.
//
// Everything here is pure except publish_documents(), which only calls the
// store it is given.

#include <raft/jwt.hpp>
#include <raft/oidc_issuer.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::oidc {

/// Where --oidc-publish-url points. `provider` is the store's
/// provider_name(): "s3", "gcs", "azure-blob", "oci-objectstorage" or "oss".
struct publish_target {
    std::string provider;
    std::string bucket;  // the container, for Azure
    std::string prefix;  // no leading or trailing '/'; may be empty

    friend auto operator==(const publish_target&, const publish_target&) -> bool = default;
};

/// URL schemes, in the spelling each provider's own tools use.
struct publish_scheme {
    std::string_view scheme;
    std::string_view provider;
};
inline constexpr std::array<publish_scheme, 5> k_publish_schemes{{
    {"s3", "s3"},
    {"gs", "gcs"},
    {"az", "azure-blob"},
    {"oci", "oci-objectstorage"},
    {"oss", "oss"},
}};

/// Parses `<scheme>://<bucket>[/<prefix>]`. Returns the target, or an error
/// message for the operator.
[[nodiscard]] inline auto parse_publish_url(std::string_view url)
    -> std::variant<publish_target, std::string> {
    const auto sep = url.find("://");
    if (sep == std::string_view::npos) {
        return std::string("the publish URL must look like s3://<bucket>[/<prefix>]");
    }
    const auto scheme = url.substr(0, sep);
    publish_target t;
    for (const auto& s : k_publish_schemes) {
        if (s.scheme == scheme) {
            t.provider = std::string(s.provider);
        }
    }
    if (t.provider.empty()) {
        return "unknown scheme \"" + std::string(scheme) +
               "\"; use s3://, gs://, az://, oci:// or oss://";
    }
    for (char c : url) {
        const auto b = static_cast<unsigned char>(c);
        if (b <= 0x20 || b >= 0x7f || c == '?' || c == '#' || c == '\\') {
            return std::string(
                "the publish URL must be printable ASCII without spaces, '?', '#' or '\\'");
        }
    }
    auto rest = url.substr(sep + 3);
    const auto slash = rest.find('/');
    t.bucket = std::string(rest.substr(0, slash));
    if (t.bucket.empty()) {
        return std::string("the publish URL has no bucket");
    }
    if (slash != std::string_view::npos) {
        auto prefix = rest.substr(slash + 1);
        while (prefix.ends_with('/')) {
            prefix.remove_suffix(1);
        }
        t.prefix = std::string(prefix);
    }
    const std::string padded = "/" + t.prefix + "/";
    if (padded.find("//") != std::string::npos && !t.prefix.empty()) {
        return std::string("the publish URL's prefix has an empty path segment");
    }
    if (padded.find("/./") != std::string::npos || padded.find("/../") != std::string::npos) {
        return std::string("the publish URL's prefix has a dot segment");
    }
    return t;
}

/// The object key for an issuer path such as k_jwks_path, under the prefix.
[[nodiscard]] inline auto publish_key(const publish_target& t, std::string_view issuer_path)
    -> std::string {
    const auto path = issuer_path.substr(issuer_path.starts_with('/') ? 1 : 0);
    return t.prefix.empty() ? std::string(path) : t.prefix + "/" + std::string(path);
}

struct published_documents {
    std::string discovery;
    std::string jwks;

    friend auto operator==(const published_documents&, const published_documents&)
        -> bool = default;
};

/// Both documents as the issuer itself serves them at `now`. std::nullopt
/// while the JWKS is empty: uploading an empty key set would make every
/// relying party reject every token until the next upload.
[[nodiscard]] inline auto documents_to_publish(const std::vector<oidc_key_record>& keys,
                                               std::int64_t now, std::string_view issuer,
                                               jose::jws_alg alg)
    -> std::optional<published_documents> {
    auto jwks = jwks_document(keys, now);
    if (jwks.at("keys").as_array().empty()) {
        return std::nullopt;
    }
    const std::string jwks_uri = std::string(issuer) + std::string(k_jwks_path);
    return published_documents{boost::json::serialize(discovery_document(issuer, jwks_uri, alg)),
                               boost::json::serialize(jwks)};
}

/// Uploads both documents: the JWKS first, so the discovery document never
/// points at a key set that is not there yet. Throws whatever the store
/// throws.
template<typename Store>
requires requires(const Store& s, const std::string& b, const std::string& k,
                  std::string_view body) { s.put_object(b, k, body); }
void publish_documents(const Store& store, const publish_target& target,
                       const published_documents& docs) {
    store.put_object(target.bucket, publish_key(target, k_jwks_path), docs.jwks);
    store.put_object(target.bucket, publish_key(target, k_discovery_path), docs.discovery);
}

/// When the leader should upload. Driven with steady-clock seconds so a
/// wall-clock step cannot stall or storm the uploads.
class publish_schedule {
public:
    struct config {
        std::chrono::seconds refresh{std::chrono::hours(6)};
        std::chrono::seconds first_retry{std::chrono::seconds(5)};
        std::chrono::seconds max_retry{std::chrono::minutes(5)};
    };

    publish_schedule() = default;
    explicit publish_schedule(config cfg) : _cfg(cfg) {}

    /// True if `docs` should be uploaded now: they differ from the last
    /// successful upload (and no retry is pending), a pending retry is due,
    /// or the refresh interval has passed since the last success.
    [[nodiscard]] auto due(const published_documents& docs, std::int64_t steady_now) const -> bool {
        if (_retry_at) {
            return steady_now >= *_retry_at;
        }
        if (!_last || *_last != docs) {
            return true;
        }
        return steady_now >= _last_success + _cfg.refresh.count();
    }

    void succeeded(const published_documents& docs, std::int64_t steady_now) {
        _last = docs;
        _last_success = steady_now;
        _retry_at.reset();
        _consecutive_failures = 0;
    }

    void failed(std::int64_t steady_now) {
        auto delay = _cfg.first_retry.count();
        for (int i = 0; i < _consecutive_failures && delay < _cfg.max_retry.count(); ++i) {
            delay *= 2;
        }
        delay = std::min<std::int64_t>(delay, _cfg.max_retry.count());
        _retry_at = steady_now + delay;
        ++_consecutive_failures;
    }

    /// Forget the last upload, e.g. on gaining leadership: a previous leader
    /// may have uploaded something else since.
    void reset() {
        _last.reset();
        _retry_at.reset();
        _consecutive_failures = 0;
    }

    [[nodiscard]] auto consecutive_failures() const -> int { return _consecutive_failures; }
    [[nodiscard]] auto retry_at() const -> std::optional<std::int64_t> { return _retry_at; }

private:
    config _cfg{};
    std::optional<published_documents> _last;
    std::int64_t _last_success{0};
    std::optional<std::int64_t> _retry_at;
    int _consecutive_failures{0};
};

}  // namespace kythira::oidc
