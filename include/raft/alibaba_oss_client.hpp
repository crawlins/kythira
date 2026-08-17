#pragma once

/// @file alibaba_oss_client.hpp
/// @brief Minimal Alibaba Cloud OSS object client — V4 header signing
///        (OSS4-HMAC-SHA256) and exactly the four operations the OSS
///        persistence engine needs: PutObject, GetObject, DeleteObject,
///        ListObjectsV2.
///
/// Hand-rolled for the same reasons as the control-plane client
/// (`alibaba_http_client.hpp`), plus one specific to persistence: the
/// engine's durability contract (spec Requirement 15.2) rides on knowing
/// exactly when a write is acknowledged and what a retry re-sends — behavior
/// that must not live inside a vendor SDK's opaque retry policy. Writes here
/// never auto-retry (Requirement 2.5); the engine owns write retries because
/// it knows its PUTs are idempotent full-object overwrites.
///
/// **Signing scheme status** (spike-notes.md Finding 2): the canonical-form
/// structure is confirmed against the vendor's worked example — the
/// documented canonical request hashes to the documented string-to-sign
/// value exactly. The final signature of that example is *not*
/// reproducible because the vendor masked the example's AccessKeySecret, so
/// the key-derivation chain below follows the documented algorithm
/// (`aliyun_v4` prefix, date → region → "oss" → "aliyun_v4_request") with a
/// self-derived unit-test vector; live confirmation is Task 0.2's remaining
/// half, pending an account.
///
/// Canonical request (parts joined with `"\n"`):
///
///     VERB
///     CanonicalURI            (always the path-style resource:
///                              /{bucket}/{key}, key percent-encoded with
///                              '/' preserved — regardless of how the
///                              request is addressed on the wire)
///     CanonicalQueryString    (sorted, percent-encoded)
///     CanonicalHeaders        ("{lowercase-name}:{value}\n" each, sorted:
///                              content-md5/content-type when present and
///                              every x-oss-* header; note the trailing \n)
///     AdditionalHeaders       (extra signed headers; this client signs
///                              none, so the line is empty)
///     HashedPayload           ("UNSIGNED-PAYLOAD" — the only value OSS V4
///                              currently supports)
///
/// string-to-sign = "OSS4-HMAC-SHA256\n{timestamp}\n{date}/{region}/oss/
/// aliyun_v4_request\n" + hex-SHA256(canonical request); the signing key is
/// the four-step HMAC chain above; the signature is lowercase hex.
///
/// Addressing: virtual-host style (`{bucket}.oss-{region}.aliyuncs.com`) by
/// default; **path-style under `endpoint_override`** (Requirement 2.3) so a
/// local mock server needs no wildcard DNS. The canonical URI is the same
/// path-style resource either way, which is what makes the two addressings
/// sign identically.
///
/// Listings request `encoding-type=url`, so keys arrive percent-encoded
/// ASCII and are decoded here — sidestepping XML entity handling for
/// arbitrary key bytes without an XML dependency.

#include <raft/alibaba_client_config.hpp>
#include <raft/alibaba_signing.hpp>
#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>

#include <httplib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

namespace alibaba_oss_detail {

/// `x-oss-date` basic-format timestamp (`YYYYMMDD'T'HHMMSS'Z'`) and its date
/// prefix — the scope's first element.
[[nodiscard]] inline auto iso8601_basic_utc(std::chrono::system_clock::time_point when)
    -> std::string {
    const std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::array<char, 24> buf{};
    const int written = std::snprintf(buf.data(), buf.size(), "%04d%02d%02dT%02d%02d%02dZ",
                                      tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                                      tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    if (written <= 0) {
        throw std::runtime_error("alibaba_oss_client: failed to format x-oss-date");
    }
    return std::string(buf.data(), static_cast<std::size_t>(written));
}

/// Percent-encode an object key: unreserved characters and `/` literal (the
/// path separator is part of the resource, not data), everything else %XX.
[[nodiscard]] inline auto encode_key(std::string_view key) -> std::string {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == '/') {
            out.push_back(c);
        } else {
            out += alibaba_signing::detail::percent_encode(std::string_view(&c, 1));
        }
    }
    return out;
}

/// Decode the percent-encoding `encoding-type=url` listings carry. A stray
/// `%` not followed by two hex digits throws — a listing this client cannot
/// interpret must never silently drop keys (Requirement 2.4).
[[nodiscard]] inline auto percent_decode(std::string_view in) -> std::string {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (in[i] != '%') {
            out.push_back(in[i]);
            continue;
        }
        if (i + 2 >= in.size() || hex_value(in[i + 1]) < 0 || hex_value(in[i + 2]) < 0) {
            throw std::runtime_error("alibaba_oss_client: malformed percent-encoding in listing");
        }
        out.push_back(static_cast<char>((hex_value(in[i + 1]) << 4) | hex_value(in[i + 2])));
        i += 2;
    }
    return out;
}

/// Extract every text content of `<element>` from an XML body — the bounded
/// scanning Requirement 2.4 asks for instead of an XML dependency. Only flat,
/// non-nested elements are handled, which is what ListObjectsV2's `<Key>`,
/// `<IsTruncated>`, and `<NextContinuationToken>` are.
[[nodiscard]] inline auto xml_values(std::string_view body, std::string_view element)
    -> std::vector<std::string> {
    const std::string open = "<" + std::string(element) + ">";
    const std::string close = "</" + std::string(element) + ">";
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        const auto start = body.find(open, pos);
        if (start == std::string_view::npos) {
            break;
        }
        const auto value_start = start + open.size();
        const auto end = body.find(close, value_start);
        if (end == std::string_view::npos) {
            throw std::runtime_error("alibaba_oss_client: unterminated <" + std::string(element) +
                                     "> in listing response");
        }
        out.emplace_back(body.substr(value_start, end - value_start));
        pos = end + close.size();
    }
    return out;
}

}  // namespace alibaba_oss_detail

/// @brief The four object operations the persistence engine needs, V4-signed.
class alibaba_oss_client {
public:
    explicit alibaba_oss_client(alibaba_client_config cfg) : _cfg(std::move(cfg)) {}

    /// @brief The name this store answers to in error messages, metrics and a
    ///        backup manifest (`kythira::key_object_store`).
    [[nodiscard]] static auto provider_name() -> std::string_view { return "oss"; }

    /// @brief PUT one object. Success is a 2xx and nothing else; no retry
    ///        (the engine owns write retries — Requirement 2.5).
    ///
    /// Returns the response ETag as the object's `object_version`. OSS's ETag
    /// is carried through verbatim, quotes included: it is an opaque token that
    /// goes back to the service unmodified in a future precondition, so
    /// normalising it here would only create a way for the two spellings to
    /// disagree.
    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        fiu_do_on("raft/alibaba/oss/put_object",
                  throw std::runtime_error("chaos: raft/alibaba/oss/put_object " + key););
        auto result = send(bucket, key, "PUT", {}, std::string(bytes));
        require_2xx(result, "PutObject", key);
        return {result->get_header_value("ETag")};
    }

    /// @brief GET one object; `nullopt` on 404 (absent is an answer, not an
    ///        error, for a persistence load path).
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        fiu_do_on("raft/alibaba/oss/get_object",
                  throw std::runtime_error("chaos: raft/alibaba/oss/get_object " + key););
        auto result = send(bucket, key, "GET", {}, {});
        if (result && result->status == 404) {
            return std::nullopt;
        }
        // One read retry on 5xx (Requirement 2.5: reads MAY retry once).
        if (result && result->status >= 500) {
            result = send(bucket, key, "GET", {}, {});
        }
        require_2xx(result, "GetObject", key);
        return get_result{result->body, result->get_header_value("ETag")};
    }

    /// @brief DELETE one object. OSS answers 204 for present and absent keys
    ///        alike, which is exactly what the engine's idempotent truncation
    ///        wants.
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        fiu_do_on("raft/alibaba/oss/delete_object",
                  throw std::runtime_error("chaos: raft/alibaba/oss/delete_object " + key););
        auto result = send(bucket, key, "DELETE", {}, {});
        require_2xx(result, "DeleteObject", key);
    }

    /// @brief Every key under `prefix`, paginated to exhaustion
    ///        (ListObjectsV2, `encoding-type=url`). Any structurally
    ///        unexpected page throws — a truncated listing in a persistence
    ///        engine is silent data loss (Requirement 2.4).
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        fiu_do_on("raft/alibaba/oss/list_keys",
                  throw std::runtime_error("chaos: raft/alibaba/oss/list_keys " + prefix););
        std::vector<std::string> keys;
        std::string continuation;
        while (true) {
            std::map<std::string, std::string> query{
                {"list-type", "2"},
                {"prefix", prefix},
                {"encoding-type", "url"},
            };
            if (!continuation.empty()) {
                query.emplace("continuation-token", continuation);
            }
            auto result = send(bucket, "", "GET", query, {});
            if (result && result->status >= 500) {
                result = send(bucket, "", "GET", query, {});
            }
            require_2xx(result, "ListObjectsV2", prefix);

            for (const auto& encoded : alibaba_oss_detail::xml_values(result->body, "Key")) {
                keys.push_back(alibaba_oss_detail::percent_decode(encoded));
            }

            const auto truncated = alibaba_oss_detail::xml_values(result->body, "IsTruncated");
            if (truncated.size() != 1 || (truncated[0] != "true" && truncated[0] != "false")) {
                throw std::runtime_error(
                    "alibaba_oss_client: ListObjectsV2 response carries no usable <IsTruncated>"
                    " — refusing to treat a structurally unexpected listing as complete");
            }
            if (truncated[0] == "false") {
                return keys;
            }
            const auto next = alibaba_oss_detail::xml_values(result->body, "NextContinuationToken");
            if (next.size() != 1 || next[0].empty()) {
                throw std::runtime_error(
                    "alibaba_oss_client: truncated ListObjectsV2 response carries no"
                    " <NextContinuationToken>");
            }
            continuation = next[0];
        }
    }

    /// @brief Every header one call will carry — public for the same
    ///        signed-versus-sent assertability as the control-plane client.
    [[nodiscard]] auto prepared_headers(const std::string& bucket, const std::string& key,
                                        std::string_view method,
                                        const std::map<std::string, std::string>& query,
                                        std::chrono::system_clock::time_point when,
                                        std::string_view content_type = {}) const
        -> std::map<std::string, std::string> {
        if (_cfg.access_key_id.empty()) {
            throw std::invalid_argument("alibaba_oss_client: access_key_id is empty");
        }
        if (_cfg.access_key_secret.empty()) {
            throw std::invalid_argument("alibaba_oss_client: access_key_secret is empty");
        }
        const std::string timestamp = alibaba_oss_detail::iso8601_basic_utc(when);
        const std::string date = timestamp.substr(0, 8);
        const std::string scope = date + "/" + _cfg.region + "/oss/aliyun_v4_request";

        // The signed set. `content-type` is signed WHEN THE REQUEST CARRIES
        // ONE — the exact inverse of the rule oci_http_client follows, and
        // the reason this client's first live call returned
        // SignatureDoesNotMatch.
        //
        // Both clients face the same httplib behaviour: its body overloads
        // set Content-Type from their own argument, so the header reaches the
        // wire whether or not the caller put it in the header map. OCI's
        // scheme does not sign content-type, so there the fix was to withhold
        // it and let httplib own it. OSS V4 *does* sign Content-Type when
        // present, so here the same behaviour means the value must be folded
        // into the signature instead — otherwise the server canonicalises a
        // header the client never signed.
        //
        // OSS named this itself: its 403 body echoes the CanonicalRequest it
        // computed, which differed from ours by exactly the
        // `content-type:application/octet-stream` line (spike-notes.md
        // Finding 2). No local test could have caught it — the mock tier's
        // server does not verify signatures, and the golden vector could not
        // be reproduced because the vendor masked its example's secret.
        std::map<std::string, std::string> signed_headers{
            {"x-oss-content-sha256", "UNSIGNED-PAYLOAD"},
            {"x-oss-date", timestamp},
        };
        if (!content_type.empty()) {
            signed_headers.emplace("content-type", std::string(content_type));
        }
        if (!_cfg.security_token.empty()) {
            signed_headers.emplace("x-oss-security-token", _cfg.security_token);
        }

        const std::string resource =
            "/" + bucket + (key.empty() ? "/" : "/" + alibaba_oss_detail::encode_key(key));
        std::string canonical_headers;
        for (const auto& [name, value] : signed_headers) {
            canonical_headers += name;
            canonical_headers.push_back(':');
            canonical_headers += value;
            canonical_headers.push_back('\n');
        }
        std::string canonical;
        canonical += method;
        canonical.push_back('\n');
        canonical += resource;
        canonical.push_back('\n');
        canonical += alibaba_signing::canonical_query_string(query);
        canonical.push_back('\n');
        canonical += canonical_headers;
        canonical.push_back('\n');
        // AdditionalHeaders: none signed beyond the always-signed set.
        canonical.push_back('\n');
        canonical += "UNSIGNED-PAYLOAD";

        const std::string string_to_sign = "OSS4-HMAC-SHA256\n" + timestamp + "\n" + scope + "\n" +
                                           alibaba_signing::detail::sha256_hex(canonical);

        using alibaba_signing::detail::hmac_sha256;
        std::string signing_key = hmac_sha256("aliyun_v4" + _cfg.access_key_secret, date);
        signing_key = hmac_sha256(signing_key, _cfg.region);
        signing_key = hmac_sha256(signing_key, "oss");
        signing_key = hmac_sha256(signing_key, "aliyun_v4_request");
        const std::string signature =
            alibaba_signing::detail::to_hex(hmac_sha256(signing_key, string_to_sign));

        auto headers = signed_headers;
        headers.emplace("Authorization", "OSS4-HMAC-SHA256 Credential=" + _cfg.access_key_id + "/" +
                                             scope + ",Signature=" + signature);
        return headers;
    }

    /// @brief Where a request for this bucket goes on the wire, and the path
    ///        it uses: virtual-host style against real OSS, path-style under
    ///        `endpoint_override` (Requirement 2.3). Public for derivation
    ///        tests.
    [[nodiscard]] auto origin_and_path(const std::string& bucket, const std::string& key) const
        -> std::pair<std::string, std::string> {
        const std::string encoded = key.empty() ? "/" : "/" + alibaba_oss_detail::encode_key(key);
        if (!_cfg.endpoint_override.empty()) {
            return {_cfg.endpoint_override, "/" + bucket + encoded};
        }
        return {"https://" + bucket + ".oss-" + _cfg.region + ".aliyuncs.com", encoded};
    }

private:
    [[nodiscard]] auto send(const std::string& bucket, const std::string& key,
                            std::string_view method,
                            const std::map<std::string, std::string>& query,
                            const std::string& body) const -> httplib::Result {
        const auto [origin, path] = origin_and_path(bucket, key);

        // Only PUT carries a body, and therefore only PUT carries the
        // Content-Type httplib derives from its body overload — so it is the
        // only method whose signature must cover one.
        const std::string_view content_type = (method == "PUT") ? k_put_content_type : "";

        httplib::Headers headers;
        for (const auto& [name, value] : prepared_headers(
                 bucket, key, method, query, std::chrono::system_clock::now(), content_type)) {
            // content-type is withheld from the map httplib is given: it sets
            // that header itself from the Put() argument below, and supplying
            // it twice puts two on the wire. It is still *signed* above,
            // which is the distinction that matters.
            if (name == "content-type") {
                continue;
            }
            headers.emplace(name, value);
        }

        std::unique_ptr<httplib::Client> client;
        try {
            client = std::make_unique<httplib::Client>(origin);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "alibaba_oss_client: could not create an HTTP client for " + origin + ": " +
                e.what() +
                " — OSS endpoints are https, which needs cpp-httplib compiled with "
                "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
        }
        client->set_connection_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);
        client->set_read_timeout(static_cast<time_t>(_cfg.api_timeout.count()), 0);

        std::string target = path;
        if (!query.empty()) {
            target += "?" + alibaba_signing::canonical_query_string(query);
        }
        if (method == "PUT") {
            // The payload hash is UNSIGNED-PAYLOAD and content-type is not in
            // the signed set, so httplib owning the content-type header is
            // harmless here (unlike a signed-content-type scheme).
            return client->Put(target, headers, body, k_put_content_type);
        }
        if (method == "GET") {
            return client->Get(target, headers);
        }
        if (method == "DELETE") {
            return client->Delete(target, headers);
        }
        throw std::invalid_argument("alibaba_oss_client: unsupported HTTP method: " +
                                    std::string(method));
    }

    static auto require_2xx(const httplib::Result& result, std::string_view op,
                            const std::string& subject) -> void {
        if (!result) {
            throw std::runtime_error("alibaba_oss_client: " + std::string(op) + " " + subject +
                                     " failed: " + httplib::to_string(result.error()));
        }
        if (result->status < 200 || result->status >= 300) {
            std::string detail;
            if (!result->body.empty()) {
                // OSS errors are XML; Code/Message extracted with the same
                // bounded scanner as listings (Requirement 2.6).
                for (const auto* element : {"Code", "Message"}) {
                    try {
                        auto values = alibaba_oss_detail::xml_values(result->body, element);
                        if (!values.empty()) {
                            detail += detail.empty() ? "" : ": ";
                            detail += values.front();
                        }
                    } catch (const std::exception&) {
                        // An unterminated error body still fails below with
                        // the status; nothing useful to add.
                    }
                }
                if (detail.empty()) {
                    detail = result->body;
                }
            }
            std::string out = "alibaba_oss_client: " + std::string(op) + " " + subject +
                              " failed with HTTP " + std::to_string(result->status);
            if (!detail.empty()) {
                out += ": " + detail;
            }
            throw std::runtime_error(out);
        }
    }

    /// The content type every PUT carries — signed (see prepared_headers)
    /// and handed to httplib's body overload, which must be the same string.
    static constexpr const char* k_put_content_type = "application/octet-stream";

    alibaba_client_config _cfg;
};

}  // namespace kythira
