// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file azure_blob_client.hpp
/// @brief Minimal Azure Blob Storage object client — Blob REST over httplib with
///        an AAD bearer token, and exactly the operations
///        `object_store_persistence_engine` needs plus the conditional-write
///        refinement `fencing_mode::compare_and_swap` requires.
///
/// ## Why REST rather than `azure-storage-blobs-cpp` (Requirement 13.1-13.2)
///
/// This is the one place in this design that departs from "use the SDK you
/// already have", so the reasoning is written down rather than assumed. The tree
/// carries `azure-core-cpp`, `azure-identity-cpp` and
/// `azure-security-keyvault-keys-cpp` **unconditionally** — but not
/// `azure-storage-blobs-cpp`, which would be a new dependency on the default
/// build for every developer. The surface needed here is four operations, and
/// AAD bearer tokens are already what `azure_client_config`'s credential chain
/// and CI's OIDC federation produce, so **no SharedKey signing is written at
/// all**.
///
/// The decision was explicitly refutable by task 0, and task 0 confirmed it
/// (spike-notes.md Finding 11): the probe *is* this hand-rolled surface, and
/// nothing about it was harder than documented. `azure-storage-blobs-cpp`
/// remains the recorded fallback if that ever stops being true.
///
/// ## The three details that are how a working client breaks (Finding 11)
///
/// Each of these is a plausible wrong guess, so each is pinned:
///
/// - **Success is 201 on PUT and 202 on DELETE**, not 200. A client that treats
///   only 200 as success fails every write it makes.
/// - **The machine-readable error code is the `x-ms-error-code` response
///   header**, present on every error, with the XML body's `<Code>` as a
///   fallback.
/// - **`x-ms-version` is pinned to a named, dated version.** An unpinned version
///   is how a working client breaks on a service update it was not present for.
///   `x-ms-blob-type: BlockBlob` is required on every PUT.
///
/// ## One provider, two statuses, both meaning "you lost"
///
/// Azure answers **409 `BlobAlreadyExists`** to a rejected `If-None-Match: *`
/// and **412 `ConditionNotMet`** to a rejected `If-Match` — the only provider in
/// this design that uses two different statuses for the two preconditions, and
/// **both must map to `object_precondition_failed`**. Mapping only the 412, as
/// Requirement 13.4 originally said, would let a create-only collision — the
/// exact case that catches a stale leader appending log entries — pass as an
/// ordinary error and never fence anything.
///
/// Set against S3's **409 `ConditionalRequestConflict`**, which means the
/// opposite ("benign race, retry"), the rule is not merely repeated but
/// reinforced: **the status alone is never sufficient; the client reads the
/// error code.** A bare 409 here and a bare 409 there mean opposite things.
///
/// ## Integrity, and why `version_is_content_md5` is *not* declared
///
/// Every PUT carries `Content-MD5` — base64 of the raw 16 digest bytes, which
/// Azure verifies and rejects with **400 `Md5Mismatch`** (live-confirmed,
/// Finding 11; the fourth distinct spelling of that error across five
/// providers). That is Requirement 7.1's service-side half.
///
/// Requirement 7.2's local half is **unavailable here**, and this is the
/// difference between Azure and S3/OSS that a client author is most likely to
/// get wrong: Azure's ETag is an **opaque timestamp token**
/// (`0x8DEFC5FC0A7294B`), not a content digest. A client that assumed the S3
/// shape would compare that token against an MD5 and fail every single write, so
/// `version_is_content_md5` is deliberately absent — the trait is a
/// *declaration by the client*, precisely so that this cannot be inferred
/// wrongly. What Azure does return is a `Content-MD5` **response** header on
/// GET, matching the content, so a future local check is possible from a
/// different header; it is named here as the available route rather than built,
/// because the engine's verification hook is written against the version.
///
/// ## DELETE is not idempotent on the wire, and is made so here
///
/// S3 and OSS answer 204 to a DELETE of an absent key. **Azure answers 404
/// `BlobNotFound`**, which this client maps to success — the concept requires an
/// idempotent delete, and the engine's log truncation depends on it. A 404
/// naming the *container* is still an error: a missing container is a
/// misconfiguration, not an absent blob.
///
/// ## Addressing
///
/// `https://<account>.blob.core.windows.net/<container>/<blob>` by default;
/// under `endpoint_override`, `<override>/<container>/<blob>`. The container is
/// the concept's `bucket` parameter, so it is passed per call rather than held
/// in the client — see `azure_blob_config` for why the account is not.
///
/// For Azurite, whose URL layout puts the account in the path, set
/// `endpoint_override` to include it
/// (`http://127.0.0.1:10000/devstoreaccount1`). That keeps one addressing rule
/// here instead of a second emulator-shaped one.

#include <raft/azure_client_config.hpp>
#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK

#include <azure/core/context.hpp>
#include <azure/core/credentials/credentials.hpp>

#include <httplib.h>

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace azure_blob_detail {

/// Azure's own error code for a rejected `If-None-Match: *`. **409**, not 412.
inline constexpr const char* k_blob_already_exists = "BlobAlreadyExists";

/// Azure's own error code for a rejected `If-Match`. 412.
inline constexpr const char* k_condition_not_met = "ConditionNotMet";

/// 404 for the blob. Absence is an answer for a load path, and success for a
/// DELETE.
inline constexpr const char* k_blob_not_found = "BlobNotFound";

/// 404 for the *container*. Also a 404, and emphatically not "the blob is
/// absent" — a missing container is a misconfiguration that must surface.
inline constexpr const char* k_container_not_found = "ContainerNotFound";

/// @brief Percent-encode a blob name: unreserved characters and `/` literal
///        (the path separator is part of the resource, not data).
[[nodiscard]] inline auto encode_blob_name(std::string_view name) -> std::string {
    static constexpr const char* k_upper_hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        const bool unreserved = (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
                                (b >= '0' && b <= '9') || b == '-' || b == '_' || b == '.' ||
                                b == '~' || b == '/';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(k_upper_hex[b >> 4U]);
            out.push_back(k_upper_hex[b & 0x0FU]);
        }
    }
    return out;
}

/// @brief Base64 of the raw 16 MD5 bytes — the `Content-MD5` encoding.
///
/// Azure's ETag is *not* this digest in any spelling, which is exactly why this
/// client sends the header and does not declare `version_is_content_md5`.
[[nodiscard]] inline auto content_md5_base64(std::string_view bytes) -> std::string {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    // A default-constructed string_view has a null data pointer, which EVP must
    // not be handed even at length zero.
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.empty() ? "" : bytes.data());
    if (EVP_Digest(data, bytes.size(), digest.data(), &digest_len, EVP_md5(), nullptr) != 1) {
        throw std::runtime_error("azure_blob_client: MD5 computation failed");
    }
    // EVP_EncodeBlock writes 4 bytes per 3 input bytes, plus a NUL.
    std::string out(4 * ((static_cast<std::size_t>(digest_len) + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), digest.data(),
                                        static_cast<int>(digest_len));
    if (written < 0) {
        throw std::runtime_error("azure_blob_client: base64 encoding of the content MD5 failed");
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

/// @brief Every text content of a flat `<element>` in an XML body — the bounded
///        scanning Requirement 13.5 asks for instead of an XML dependency.
///
/// The same approach `alibaba_oss_client` uses, and the same hard rule: an
/// unterminated element throws rather than yielding a short answer.
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
            throw std::runtime_error("azure_blob_client: unterminated <" + std::string(element) +
                                     "> in response body");
        }
        out.emplace_back(body.substr(value_start, end - value_start));
        pos = end + close.size();
    }
    return out;
}

/// @brief Decode the five predefined XML entities.
///
/// Blob listings have **no `encoding-type` parameter** — the escape hatch
/// `alibaba_oss_client` uses to sidestep entity handling entirely does not exist
/// on this API — so a blob name containing `&` or `<` arrives escaped and must
/// be unescaped here. The engine's own keys never contain one; a *foreign* key
/// under the same prefix can, and returning it mangled would make the listing
/// disagree with the store.
[[nodiscard]] inline auto xml_unescape(std::string_view in) -> std::string {
    static const std::pair<std::string_view, char> k_entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out.push_back(in[i]);
            ++i;
            continue;
        }
        bool matched = false;
        for (const auto& [entity, ch] : k_entities) {
            if (in.substr(i, entity.size()) == entity) {
                out.push_back(ch);
                i += entity.size();
                matched = true;
                break;
            }
        }
        if (!matched) {
            // A bare '&' is not well-formed XML, but mangling it is worse than
            // carrying it through: this client does not own the names it lists.
            out.push_back('&');
            ++i;
        }
    }
    return out;
}

}  // namespace azure_blob_detail

/// @brief Blob-specific settings, alongside the shared Azure ones.
///
/// `azure_client_config` is shared with the two quorum managers and the Key
/// Vault CA provider, and carries a **subscription, resource group and ARM
/// endpoint** — none of which a data-plane blob call has any use for. Rather
/// than widen that struct with three storage fields every other consumer would
/// ignore, the storage-specific settings live here and the shared credential
/// chain and timeout are taken by embedding it, which is what Requirement 13.1
/// actually asks for.
struct azure_blob_config {
    /// Credential chain and `api_timeout`. `subscription_id`, `resource_group`,
    /// `location` and `arm_endpoint_override` are unused by this client.
    azure_client_config azure;

    /// Storage account name — the `<account>` in
    /// `https://<account>.blob.core.windows.net`. Required unless
    /// `endpoint_override` names the whole origin.
    std::string account;

    /// When non-empty, replaces `https://<account>.blob.core.windows.net`
    /// entirely. For Azurite, include the account segment
    /// (`http://127.0.0.1:10000/devstoreaccount1`).
    std::string endpoint_override;

    /// The pinned, dated REST API version sent as `x-ms-version` on every
    /// request. Defaults to the version task 0 verified live (Finding 11).
    /// Changing it is a deliberate act — an *unpinned* version is how a working
    /// client breaks on a service update.
    std::string api_version{"2023-11-03"};

    /// The AAD scope the bearer token is requested for. Storage data-plane
    /// access, not ARM.
    std::string token_scope{"https://storage.azure.com/.default"};
};

/// @brief The object operations the persistence engine needs, over Blob REST.
class azure_blob_client {
public:
    explicit azure_blob_client(azure_blob_config cfg)
        : _cfg(std::move(cfg)), _token(std::make_shared<cached_token>()) {
        if (_cfg.account.empty() && _cfg.endpoint_override.empty()) {
            throw std::invalid_argument(
                "azure_blob_client: account is empty and no endpoint_override was given — one of "
                "the two must name where blobs live");
        }
        if (_cfg.api_version.empty()) {
            throw std::invalid_argument(
                "azure_blob_client: api_version is empty — x-ms-version must be a named, dated "
                "version, because an unpinned one is how a working client breaks on a service "
                "update");
        }
        _credential =
            _cfg.azure.credential ? _cfg.azure.credential : make_default_credential_chain();
    }

    /// @brief The name this store answers to in error messages, metrics and a
    ///        backup manifest (`kythira::key_object_store`).
    [[nodiscard]] static auto provider_name() -> std::string_view { return "azure-blob"; }

    /// @brief PUT one blob, unconditionally. No retry — the engine owns write
    ///        retries.
    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        fiu_do_on("raft/azure/blob/put_object",
                  throw std::runtime_error("chaos: raft/azure/blob/put_object " + key););
        return send_put(bucket, key, bytes, std::nullopt);
    }

    /// @brief PUT one blob only if `pre` holds, throwing
    ///        `object_precondition_failed` when the service rejects it.
    ///
    /// Issued exactly once. A conditional PUT's precondition is stale by
    /// construction if a previous attempt landed, so a retry would either be a
    /// no-op or a spurious latch.
    auto put_object_if(const std::string& bucket, const std::string& key, std::string_view bytes,
                       const precondition& pre) const -> put_result {
        fiu_do_on("raft/azure/blob/put_object_if",
                  throw std::runtime_error("chaos: raft/azure/blob/put_object_if " + key););
        return send_put(bucket, key, bytes, pre);
    }

    /// @brief GET one blob; `nullopt` when the blob is absent.
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        fiu_do_on("raft/azure/blob/get_object",
                  throw std::runtime_error("chaos: raft/azure/blob/get_object " + key););
        auto result = send(bucket, key, "GET", {}, {}, {});
        if (result && result->status == 404 &&
            error_code_of(result) != azure_blob_detail::k_container_not_found) {
            return std::nullopt;
        }
        require_success(result, "GetBlob", key);
        return get_result{result->body, result->get_header_value("ETag")};
    }

    /// @brief DELETE one blob, idempotently.
    ///
    /// Azure answers **404 `BlobNotFound`** for an absent blob where S3 and OSS
    /// answer 204; that 404 is success here, because the concept requires an
    /// idempotent delete and the engine's truncation path depends on it.
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        fiu_do_on("raft/azure/blob/delete_object",
                  throw std::runtime_error("chaos: raft/azure/blob/delete_object " + key););
        send_delete(bucket, key, std::nullopt);
    }

    /// @brief DELETE one blob only if `pre` holds.
    ///
    /// `if_version` is live-verified: a stale `If-Match` is refused with 412
    /// `ConditionNotMet` **and the blob survives** (Finding 11). `if_absent`
    /// sends `If-None-Match: *`, which Delete Blob accepts as a conditional
    /// header but which task 0 **did not exercise** — the engine never issues
    /// that combination, and it is recorded as unverified rather than presented
    /// as confirmed.
    auto delete_object_if(const std::string& bucket, const std::string& key,
                          const precondition& pre) const -> void {
        fiu_do_on("raft/azure/blob/delete_object_if",
                  throw std::runtime_error("chaos: raft/azure/blob/delete_object_if " + key););
        send_delete(bucket, key, pre);
    }

    /// @brief Every blob name under `prefix`, paged through `NextMarker` to
    ///        exhaustion.
    ///
    /// Blob has **no `IsTruncated` flag**: an absent or empty `<NextMarker>` is
    /// how the service says "that was the last page". So the structural guard
    /// cannot be "is the flag there" — it is instead that the body must actually
    /// be an `<EnumerationResults` document. Without that check, any unexpected
    /// 200 body parses as *zero blobs*, which a persistence engine reads as an
    /// empty log rather than as an error.
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        fiu_do_on("raft/azure/blob/list_keys",
                  throw std::runtime_error("chaos: raft/azure/blob/list_keys " + prefix););
        std::vector<std::string> keys;
        std::string marker;
        while (true) {
            std::map<std::string, std::string> query{
                {"restype", "container"},
                {"comp", "list"},
                {"prefix", prefix},
            };
            if (!marker.empty()) {
                query.emplace("marker", marker);
            }
            auto result = send(bucket, "", "GET", query, {}, {});
            require_success(result, "ListBlobs", prefix);

            if (result->body.find("<EnumerationResults") == std::string::npos) {
                throw std::runtime_error(
                    "azure_blob_client: ListBlobs " + prefix +
                    " returned a body that is not an <EnumerationResults> document — refusing to "
                    "read an unrecognised response as an empty container");
            }
            for (const auto& name : azure_blob_detail::xml_values(result->body, "Name")) {
                keys.push_back(azure_blob_detail::xml_unescape(name));
            }

            const auto next = azure_blob_detail::xml_values(result->body, "NextMarker");
            if (next.empty() || next.front().empty()) {
                return keys;
            }
            marker = next.front();
        }
    }

    /// @brief Where a request for this container goes, and the path it uses.
    ///        Public for derivation tests.
    [[nodiscard]] auto origin_and_path(const std::string& bucket, const std::string& key) const
        -> std::pair<std::string, std::string> {
        const std::string suffix =
            key.empty() ? "" : "/" + azure_blob_detail::encode_blob_name(key);
        if (!_cfg.endpoint_override.empty()) {
            return {_cfg.endpoint_override, "/" + bucket + suffix};
        }
        return {"https://" + _cfg.account + ".blob.core.windows.net", "/" + bucket + suffix};
    }

    /// @brief Every header one call will carry, bearer token included. Public so
    ///        a test can assert the pinned version and the blob type without
    ///        standing up a server.
    [[nodiscard]] auto prepared_headers(std::string_view method, std::string_view content_md5,
                                        std::size_t content_length) const
        -> std::map<std::string, std::string> {
        std::map<std::string, std::string> headers{
            {"x-ms-version", _cfg.api_version},
            {"Authorization", "Bearer " + bearer_token()},
        };
        if (method == "PUT") {
            headers.emplace("x-ms-blob-type", "BlockBlob");
            // A bodiless PUT still needs an explicit Content-Length: 0
            // (Finding 11). httplib sets it from the body it is given, so this
            // is here for the header map's own completeness and for the case
            // where a caller inspects what would be sent.
            headers.emplace("Content-Length", std::to_string(content_length));
            if (!content_md5.empty()) {
                headers.emplace("Content-MD5", std::string(content_md5));
            }
        }
        return headers;
    }

private:
    /// One token and the instant it stops being usable. Shared through a
    /// `shared_ptr` so the client stays copyable — the engine holds its store by
    /// value — without every copy re-authenticating.
    struct cached_token {
        std::mutex mu;
        std::string value;
        std::chrono::system_clock::time_point expires_at{};
    };

    /// @brief The bearer token, fetched on first use and re-fetched a margin
    ///        before it expires.
    ///
    /// The margin exists because a token that is valid when the header is built
    /// can still be expired when the service evaluates it; refreshing only on
    /// 401 would turn every expiry into one failed write, and a failed write in
    /// this engine is not free.
    [[nodiscard]] auto bearer_token() const -> std::string {
        const std::lock_guard lock(_token->mu);
        const auto now = std::chrono::system_clock::now();
        if (!_token->value.empty() && now + k_token_refresh_margin < _token->expires_at) {
            return _token->value;
        }
        Azure::Core::Credentials::TokenRequestContext ctx;
        ctx.Scopes = {_cfg.token_scope};
        const auto token = _credential->GetToken(ctx, Azure::Core::Context{});
        _token->value = token.Token;
        // `Azure::DateTime` is NOT a `system_clock` time_point: its clock counts
        // 100 ns ticks from **year 0001**, so reading `time_since_epoch()` and
        // calling the result a Unix duration puts every token about two
        // millennia in the future — which reads as "never near expiry", caches
        // the first token forever, and fails every write an hour later when the
        // real token expires. The explicit conversion operator is the only
        // correct route. (Caught by the refresh-margin unit case, which is
        // exactly the kind of bug no round-trip test would ever see.)
        _token->expires_at = static_cast<std::chrono::system_clock::time_point>(token.ExpiresOn);
        return _token->value;
    }

    [[nodiscard]] auto send_put(const std::string& bucket, const std::string& key,
                                std::string_view bytes,
                                const std::optional<precondition>& pre) const -> put_result {
        std::map<std::string, std::string> conditional;
        object_version expected;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        conditional.emplace("If-None-Match", "*");
                    } else {
                        conditional.emplace("If-Match", p.expected);
                        expected = p.expected;
                    }
                },
                *pre);
        }
        auto result = send(bucket, key, "PUT", {}, conditional, bytes);
        if (pre && is_precondition_failure(result)) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             failure_detail(result, "PutBlob", key));
        }
        require_success(result, "PutBlob", key);
        return put_result{result->get_header_value("ETag")};
    }

    auto send_delete(const std::string& bucket, const std::string& key,
                     const std::optional<precondition>& pre) const -> void {
        std::map<std::string, std::string> conditional;
        object_version expected;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        conditional.emplace("If-None-Match", "*");
                    } else {
                        conditional.emplace("If-Match", p.expected);
                        expected = p.expected;
                    }
                },
                *pre);
        }
        auto result = send(bucket, key, "DELETE", {}, conditional, {});
        if (pre && is_precondition_failure(result)) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             failure_detail(result, "DeleteBlob", key));
        }
        // An absent blob is a satisfied postcondition, not a failure — but only
        // when it is the *blob* that is missing.
        if (result && result->status == 404 &&
            error_code_of(result) == azure_blob_detail::k_blob_not_found) {
            return;
        }
        require_success(result, "DeleteBlob", key);
    }

    [[nodiscard]] auto send(const std::string& bucket, const std::string& key,
                            std::string_view method,
                            const std::map<std::string, std::string>& query,
                            const std::map<std::string, std::string>& conditional,
                            std::string_view body) const -> httplib::Result {
        const auto [origin, path] = origin_and_path(bucket, key);

        const std::string content_md5 =
            (method == "PUT") ? azure_blob_detail::content_md5_base64(body) : std::string{};

        httplib::Headers headers;
        for (const auto& [name, value] : prepared_headers(method, content_md5, body.size())) {
            // httplib derives Content-Length from the body it is handed; supplying
            // it here as well puts two on the wire.
            if (name == "Content-Length") {
                continue;
            }
            headers.emplace(name, value);
        }
        for (const auto& [name, value] : conditional) {
            headers.emplace(name, value);
        }

        std::unique_ptr<httplib::Client> client;
        try {
            client = std::make_unique<httplib::Client>(origin);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "azure_blob_client: could not create an HTTP client for " + origin + ": " +
                e.what() +
                " — blob endpoints are https, which needs cpp-httplib compiled with "
                "CPPHTTPLIB_OPENSSL_SUPPORT (CONFIG_HTTP_TRANSPORT_TLS=y)");
        }
        const auto timeout = static_cast<time_t>(_cfg.azure.api_timeout.count());
        client->set_connection_timeout(timeout, 0);
        client->set_read_timeout(timeout, 0);

        std::string target = path;
        if (!query.empty()) {
            target += "?";
            bool first = true;
            for (const auto& [name, value] : query) {
                if (!first) {
                    target += "&";
                }
                first = false;
                target += name + "=" + azure_blob_detail::encode_blob_name(value);
            }
        }

        if (method == "PUT") {
            return client->Put(target, headers, std::string(body), k_put_content_type);
        }
        if (method == "GET") {
            return client->Get(target, headers);
        }
        if (method == "DELETE") {
            return client->Delete(target, headers);
        }
        throw std::invalid_argument("azure_blob_client: unsupported HTTP method: " +
                                    std::string(method));
    }

    /// @brief The service's own error code: the `x-ms-error-code` header first,
    ///        the XML body's `<Code>` as a fallback.
    ///
    /// The header is the documented machine-readable channel and is present on
    /// every error; the body is read only because a proxy or an emulator may
    /// drop the header, and an error whose code is unreadable must not silently
    /// become a different verdict.
    [[nodiscard]] static auto error_code_of(const httplib::Result& result) -> std::string {
        if (!result) {
            return {};
        }
        const auto header = result->get_header_value("x-ms-error-code");
        if (!header.empty()) {
            return header;
        }
        try {
            const auto codes = azure_blob_detail::xml_values(result->body, "Code");
            if (!codes.empty()) {
                return codes.front();
            }
        } catch (const std::exception&) {
            // An unterminated error body still fails with its status below.
        }
        return {};
    }

    /// @brief Did the service evaluate a precondition and reject it?
    ///
    /// **Both** of Azure's two answers count, and each is recognised by its own
    /// error code rather than by its status: 409 `BlobAlreadyExists` for a lost
    /// create-only race, 412 `ConditionNotMet` for a lost `If-Match`. The
    /// status is consulted only when no code arrived at all, and then only for
    /// 412 — a bare 409 is far too ambiguous across providers to guess at.
    [[nodiscard]] static auto is_precondition_failure(const httplib::Result& result) -> bool {
        if (!result) {
            return false;
        }
        const auto code = error_code_of(result);
        if (code == azure_blob_detail::k_blob_already_exists ||
            code == azure_blob_detail::k_condition_not_met) {
            return true;
        }
        return code.empty() && result->status == 412;
    }

    [[nodiscard]] static auto failure_detail(const httplib::Result& result, std::string_view op,
                                             const std::string& subject) -> std::string {
        if (!result) {
            return "azure_blob_client: " + std::string(op) + " " + subject +
                   " failed: " + httplib::to_string(result.error());
        }
        std::string out = "azure_blob_client: " + std::string(op) + " " + subject +
                          " failed with HTTP " + std::to_string(result->status);
        const auto code = error_code_of(result);
        if (!code.empty()) {
            out += ": " + code;
        }
        try {
            const auto messages = azure_blob_detail::xml_values(result->body, "Message");
            if (!messages.empty()) {
                out += ": " + messages.front();
            }
        } catch (const std::exception&) {
        }
        return out;
    }

    /// Success is **201 on PUT and 202 on DELETE**, not 200 (Finding 11). Any
    /// 2xx is accepted rather than the exact code, because narrowing to one
    /// status per verb is a second way to break on a service update — but the
    /// two documented codes are named here so a reader knows what to expect.
    static auto require_success(const httplib::Result& result, std::string_view op,
                                const std::string& subject) -> void {
        if (result && result->status >= 200 && result->status < 300) {
            return;
        }
        throw std::runtime_error(failure_detail(result, op, subject));
    }

    /// Every PUT's content type. Fixed: these blobs are opaque serialized Raft
    /// state, never a negotiated media type.
    static constexpr const char* k_put_content_type = "application/octet-stream";

    /// How long before a token's stated expiry it is replaced.
    static constexpr std::chrono::seconds k_token_refresh_margin{300};

    azure_blob_config _cfg;
    std::shared_ptr<Azure::Core::Credentials::TokenCredential> _credential;
    std::shared_ptr<cached_token> _token;
};

static_assert(key_object_store<azure_blob_client>,
              "azure_blob_client must satisfy key_object_store");
static_assert(conditional_key_object_store<azure_blob_client>,
              "azure_blob_client must satisfy conditional_key_object_store — Azure Blob expresses "
              "every conditional primitive (spike-notes.md Finding 11), so "
              "fencing_mode::compare_and_swap must remain available for it");
static_assert(!content_md5_versioned_store<azure_blob_client>,
              "azure_blob_client must NOT declare version_is_content_md5: Azure's ETag is an "
              "opaque timestamp token, and comparing it against a content digest would fail "
              "every write");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AZURE_SDK
