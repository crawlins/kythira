#pragma once

/// @file oci_object_storage_client.hpp
/// @brief Minimal OCI Object Storage client over `oci_http_client`'s raw-bytes
///        path — exactly the operations `object_store_persistence_engine` needs
///        plus the conditional-write refinement `fencing_mode::compare_and_swap`
///        requires.
///
/// Adds **no dependency**: it reuses `oci_signing`, `oci_client_config` and
/// `oci_http_client`, all of which are already in this tree and live-verified
/// by two shipped OCI providers. Instance Principal comes along for free —
/// `oci_http_client` selects it whenever `use_instance_principal` is set, and it
/// is the auth a node actually running on an OCI instance will use.
///
/// ## The one non-obvious piece of work, and why it lives next door
///
/// `oci_http_client::request()` parses **every** response as JSON. That was a
/// control-plane assumption — correct for Compute, Identity and the certificate
/// services, and wrong here, because a GetObject answering with a Raft snapshot
/// is not JSON and must not be asked to be. `request_raw()` was added alongside
/// it, additively, and this client is its first caller.
///
/// The signed **content type** became a parameter in the same change. Object
/// bodies are `application/octet-stream`; signing `application/json` while
/// sending something else yields `NotAuthenticated: Failed to verify the
/// HTTP(S) Signature`, an error that names neither side of the mismatch
/// (spike-notes.md Finding 13).
///
/// ## Reading the verdict from the service's own error code
///
/// OCI answers **412 `IfNoneMatchFailed`** to a rejected `if-none-match: *` and
/// **412 `IfMatchFailed`** to a rejected `if-match` — one status, two codes,
/// both meaning "you lost", and both live-confirmed including the conditional
/// DELETE that design.md had marked OPEN (Finding 13). Unlike Azure, the status
/// alone would be sufficient here; the code is read anyway, because a rule that
/// holds only on the provider where it happens to be redundant is not a rule.
///
/// ## Headers are matched case-insensitively, and that is load-bearing
///
/// **OCI spells its ETag header `etag`.** Task 0's probe looked up `ETag`, got
/// nothing, sent `if-match: ""`, was refused 412 on a *correct*-ETag write, and
/// the run read exactly like "OCI cannot do CAS" — the conclusion Alibaba OSS
/// genuinely earns. What caught it was a negative control, not the reading.
/// `oci_http_client::raw_response::header()` lowercases on the way in so this
/// client cannot repeat it.
///
/// ## Integrity, and why `version_is_content_md5` is *not* declared
///
/// Every PUT carries `Content-MD5` — base64 of the raw 16 digest bytes, which
/// OCI verifies and rejects with **400 `UnmatchedContentMD5`** (Finding 13; the
/// fifth distinct spelling of that error across five providers). That is
/// Requirement 7.1's service-side half.
///
/// Requirement 7.2's local half is unavailable: **OCI's ETag is a UUID**
/// (`9a86a492-43fa-4c8c-a80e-dbd650188ff8`), not a content digest, so declaring
/// `version_is_content_md5` would make the engine compare a UUID against an MD5
/// and reject every write. What OCI does return is `opc-content-md5` on GET,
/// matching the content — the route a future local check would take, named here
/// rather than built, because the engine's hook is written against the version.
///
/// ## Namespace: configured, or resolved once
///
/// The Object Storage namespace is a per-tenancy string that every path
/// contains. It is **both** configurable and resolvable — `GET /n/` returns it
/// as a bare JSON string — so a configured value wins outright and resolution
/// happens at construction, once, never per request (task 0.8).
///
/// ## A known environment hazard, recorded because it will look like a bug
///
/// The tenancy task 0 probed **intermittently declines valid requests with 404
/// `BucketNotFound`** ("...or you are not authorized"), at roughly 3-16%, across
/// every request shape. That is an open tenancy question and *not* a property of
/// Object Storage or of this client — but a real-tier suite that meets it will
/// look like a regression, so the note lives here as well as in spike-notes.md.
/// A `BucketNotFound` is deliberately **not** treated as an absent object.

#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>
#include <raft/oci_client_config.hpp>
#include <raft/oci_http_client.hpp>

#include <boost/json.hpp>

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace oci_object_detail {

/// OCI's own error code for a rejected `if-none-match: *`. 412.
inline constexpr const char* k_if_none_match_failed = "IfNoneMatchFailed";

/// OCI's own error code for a rejected `if-match`. 412.
inline constexpr const char* k_if_match_failed = "IfMatchFailed";

/// 404 for the object.
inline constexpr const char* k_object_not_found = "ObjectNotFound";

/// 404 for the *bucket* — and the code this tenancy also returns for its
/// intermittent authorization flake, which is exactly why it must never be read
/// as "the object is absent".
inline constexpr const char* k_bucket_not_found = "BucketNotFound";

/// @brief Percent-encode one path segment set: unreserved characters and `/`
///        literal, everything else `%XX`.
[[nodiscard]] inline auto encode_key(std::string_view key) -> std::string {
    static constexpr const char* k_upper_hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
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

/// @brief Percent-encode a query-parameter value. Unlike a path segment, `/` is
///        **not** literal here.
/// @brief Percent-encodes a query-parameter value for OCI, leaving `/` alone.
///
/// **`/` must NOT be encoded**, and this is not a stylistic choice — encoding
/// it as `%2F` makes OCI answer `401 NotAuthenticated`. The signature is
/// computed over the request target, and OCI evidently canonicalises a
/// percent-encoded slash before verifying, so the service checks a target the
/// client never sent. Every listing this engine performs uses a prefix ending
/// in `/`, so this affected **every** `list_keys` call: OCI object persistence
/// could not recover a log at all.
///
/// Isolated against live Object Storage by varying only the prefix, which is
/// what turned "LIST is broken" into a one-character cause:
///
///     prefix=            → 200, 52 keys
///     prefix=heartbeat   → 200, 4 keys
///     prefix=heartbeat/  → 401 NotAuthenticated
///
/// `/` is a legal query character under RFC 3986 §3.4 (`query = *( pchar / "/"
/// / "?" )`), so leaving it literal is correct rather than a workaround.
///
/// Why 30 unit cases and a signature-verifying mock missed it: the mock
/// verifies the signature against the bytes that **arrived**, so it and the
/// client encode identically and always agree. Only a service that computes the
/// signature independently can disagree — which is the whole reason this tier
/// exists.
[[nodiscard]] inline auto encode_query_value(std::string_view value) -> std::string {
    static constexpr const char* k_upper_hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
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
[[nodiscard]] inline auto content_md5_base64(std::string_view bytes) -> std::string {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    // A default-constructed string_view has a null data pointer, which EVP must
    // not be handed even at length zero.
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.empty() ? "" : bytes.data());
    if (EVP_Digest(data, bytes.size(), digest.data(), &digest_len, EVP_md5(), nullptr) != 1) {
        throw std::runtime_error("oci_object_storage_client: MD5 computation failed");
    }
    // EVP_EncodeBlock writes 4 bytes per 3 input bytes, plus a NUL.
    std::string out(4 * ((static_cast<std::size_t>(digest_len) + 2) / 3) + 1, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), digest.data(),
                                        static_cast<int>(digest_len));
    if (written < 0) {
        throw std::runtime_error(
            "oci_object_storage_client: base64 encoding of the content MD5 failed");
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

/// @brief OCI's own error code from a JSON error body, or empty.
[[nodiscard]] inline auto error_code_of(const std::string& body) -> std::string {
    if (body.empty()) {
        return {};
    }
    try {
        const auto parsed = boost::json::parse(body);
        if (const auto* obj = parsed.if_object(); obj != nullptr) {
            if (const auto* code = obj->if_contains("code"); code != nullptr && code->is_string()) {
                return std::string(code->get_string());
            }
        }
    } catch (const std::exception&) {
        // An unparseable error body still fails with its status; there is
        // nothing useful to add and pretending otherwise would invent a code.
    }
    return {};
}

}  // namespace oci_object_detail

/// @brief Object Storage settings, alongside the shared OCI ones.
struct oci_object_storage_config {
    /// Region, auth material, endpoint override and timeout — the same struct
    /// the quorum manager and certificate provider take.
    oci_client_config oci;

    /// The tenancy's Object Storage namespace. When empty it is resolved once at
    /// construction via `GET /n/` and cached (task 0.8).
    std::string namespace_name;
};

/// @brief The object operations the persistence engine needs, over OCI Object
///        Storage.
class oci_object_storage_client {
public:
    explicit oci_object_storage_client(oci_object_storage_config cfg)
        : _http(std::make_shared<oci_http_client>(cfg.oci)),
          _namespace(std::move(cfg.namespace_name)) {
        if (_namespace.empty()) {
            _namespace = resolve_namespace();
        }
    }

    /// @brief The name this store answers to in error messages, metrics and a
    ///        backup manifest (`kythira::key_object_store`).
    [[nodiscard]] static auto provider_name() -> std::string_view { return "oci-objectstorage"; }

    /// @brief The namespace every path carries — configured or resolved.
    ///        Public so a test can assert it was resolved exactly once.
    [[nodiscard]] auto namespace_name() const -> const std::string& { return _namespace; }

    /// @brief PUT one object, unconditionally. No retry — the engine owns write
    ///        retries.
    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        fiu_do_on("raft/oci/objectstorage/put_object",
                  throw std::runtime_error("chaos: raft/oci/objectstorage/put_object " + key););
        return send_put(bucket, key, bytes, std::nullopt);
    }

    /// @brief PUT one object only if `pre` holds, throwing
    ///        `object_precondition_failed` when the service rejects it.
    ///
    /// Issued exactly once: a conditional PUT's precondition is stale by
    /// construction if a previous attempt landed.
    auto put_object_if(const std::string& bucket, const std::string& key, std::string_view bytes,
                       const precondition& pre) const -> put_result {
        fiu_do_on("raft/oci/objectstorage/put_object_if",
                  throw std::runtime_error("chaos: raft/oci/objectstorage/put_object_if " + key););
        return send_put(bucket, key, bytes, pre);
    }

    /// @brief GET one object; `nullopt` when the object is absent.
    ///
    /// A 404 naming the **bucket** is an error, not an absence — both because a
    /// missing bucket is a misconfiguration and because this tenancy's
    /// intermittent authorization flake answers `BucketNotFound` to perfectly
    /// valid requests. Reading that as "the object is not there" would let a
    /// flake present as an empty Raft log.
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        fiu_do_on("raft/oci/objectstorage/get_object",
                  throw std::runtime_error("chaos: raft/oci/objectstorage/get_object " + key););
        const auto response = _http->request_raw("objectstorage", "GET", object_path(bucket, key));
        if (is_absent_object(response)) {
            return std::nullopt;
        }
        require_success(response, "GetObject", key);
        return get_result{response.body, response.header("etag")};
    }

    /// @brief DELETE one object. OCI answers 204 for a present object and 404
    ///        `ObjectNotFound` for an absent one; the latter is success here,
    ///        because the concept requires an idempotent delete.
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        fiu_do_on("raft/oci/objectstorage/delete_object",
                  throw std::runtime_error("chaos: raft/oci/objectstorage/delete_object " + key););
        send_delete(bucket, key, std::nullopt);
    }

    /// @brief DELETE one object only if `pre` holds. A stale `if-match` is
    ///        refused **and the object survives** — live-verified, and the cell
    ///        design.md had marked OPEN (Finding 13).
    auto delete_object_if(const std::string& bucket, const std::string& key,
                          const precondition& pre) const -> void {
        fiu_do_on(
            "raft/oci/objectstorage/delete_object_if",
            throw std::runtime_error("chaos: raft/oci/objectstorage/delete_object_if " + key););
        send_delete(bucket, key, pre);
    }

    /// @brief Every key under `prefix`, paged through `nextStartWith` to
    ///        exhaustion.
    ///
    /// A page whose body is not a JSON object carrying `objects` throws rather
    /// than yielding nothing: recovery reads the log from one prefixed LIST, and
    /// "zero objects" and "I could not read this" are otherwise the same answer
    /// — which to a persistence engine means an empty log.
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        fiu_do_on("raft/oci/objectstorage/list_keys",
                  throw std::runtime_error("chaos: raft/oci/objectstorage/list_keys " + prefix););
        std::vector<std::string> keys;
        std::string start;
        while (true) {
            std::string path = list_path(bucket) + "?limit=1000&prefix=" +
                               oci_object_detail::encode_query_value(prefix);
            if (!start.empty()) {
                path += "&start=" + oci_object_detail::encode_query_value(start);
            }
            const auto response = _http->request_raw("objectstorage", "GET", path);
            require_success(response, "ListObjects", prefix);

            boost::json::value parsed;
            try {
                parsed = boost::json::parse(response.body);
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "oci_object_storage_client: ListObjects " + prefix +
                    " returned an unparseable body — refusing to read it as an empty bucket: " +
                    e.what());
            }
            const auto* obj = parsed.if_object();
            if (obj == nullptr) {
                throw std::runtime_error(
                    "oci_object_storage_client: ListObjects " + prefix +
                    " returned a body that is not a JSON object — refusing to read an "
                    "unrecognised listing as an empty bucket");
            }
            const auto* listed = obj->if_contains("objects");
            if (listed == nullptr || !listed->is_array()) {
                throw std::runtime_error(
                    "oci_object_storage_client: ListObjects " + prefix +
                    " returned no 'objects' array — refusing to treat a structurally unexpected "
                    "listing as complete");
            }
            for (const auto& entry : listed->get_array()) {
                const auto* entry_obj = entry.if_object();
                const auto* name = entry_obj == nullptr ? nullptr : entry_obj->if_contains("name");
                if (name == nullptr || !name->is_string()) {
                    throw std::runtime_error("oci_object_storage_client: ListObjects " + prefix +
                                             " returned an entry with no 'name' string");
                }
                keys.emplace_back(name->get_string());
            }

            // Absent, null or empty all mean "that was the last page". OCI
            // returns the field only when there is more.
            const auto* next = obj->if_contains("nextStartWith");
            if (next == nullptr || !next->is_string() || next->get_string().empty()) {
                return keys;
            }
            start = std::string(next->get_string());
        }
    }

    /// @brief The object path for a key. Public for derivation tests — the
    ///        namespace and the `/n/.../b/.../o/` shape are exactly the sort of
    ///        thing that is wrong once and then wrong everywhere.
    [[nodiscard]] auto object_path(const std::string& bucket, const std::string& key) const
        -> std::string {
        return "/n/" + _namespace + "/b/" + bucket + "/o/" + oci_object_detail::encode_key(key);
    }

    /// @brief The listing path for a bucket — note the **absent** trailing
    ///        slash, which is a different resource from `object_path`'s.
    [[nodiscard]] auto list_path(const std::string& bucket) const -> std::string {
        return "/n/" + _namespace + "/b/" + bucket + "/o";
    }

private:
    [[nodiscard]] auto resolve_namespace() const -> std::string {
        const auto response = _http->request_raw("objectstorage", "GET", "/n/");
        require_success(response, "GetNamespace", "/n/");
        try {
            const auto parsed = boost::json::parse(response.body);
            if (parsed.is_string()) {
                return std::string(parsed.get_string());
            }
        } catch (const std::exception& e) {
            throw std::runtime_error(
                std::string("oci_object_storage_client: GET /n/ returned an unparseable body while "
                            "resolving the Object Storage namespace: ") +
                e.what());
        }
        throw std::runtime_error(
            "oci_object_storage_client: GET /n/ did not return a bare JSON string — the namespace "
            "cannot be resolved, so configure it explicitly");
    }

    [[nodiscard]] auto send_put(const std::string& bucket, const std::string& key,
                                std::string_view bytes,
                                const std::optional<precondition>& pre) const -> put_result {
        object_version expected;

        // OCI carries both preconditions as request headers, and
        // `oci_http_client` signs a fixed set — so the conditional headers are
        // added by the client below rather than by the signer. They are NOT part
        // of the signed set, which is why they can be.
        std::map<std::string, std::string> conditional;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        conditional.emplace("if-none-match", "*");
                    } else {
                        conditional.emplace("if-match", p.expected);
                        expected = p.expected;
                    }
                },
                *pre);
        }
        conditional.emplace("Content-MD5", oci_object_detail::content_md5_base64(bytes));

        const auto response =
            _http->request_raw("objectstorage", "PUT", object_path(bucket, key), std::string(bytes),
                               "application/octet-stream", conditional);
        if (pre && is_precondition_failure(response)) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             describe(response, "PutObject", key));
        }
        require_success(response, "PutObject", key);
        return put_result{response.header("etag")};
    }

    auto send_delete(const std::string& bucket, const std::string& key,
                     const std::optional<precondition>& pre) const -> void {
        object_version expected;
        std::map<std::string, std::string> conditional;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        conditional.emplace("if-none-match", "*");
                    } else {
                        conditional.emplace("if-match", p.expected);
                        expected = p.expected;
                    }
                },
                *pre);
        }
        const auto response =
            _http->request_raw("objectstorage", "DELETE", object_path(bucket, key), {},
                               "application/octet-stream", conditional);
        if (pre && is_precondition_failure(response)) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             describe(response, "DeleteObject", key));
        }
        // An absent object is a satisfied postcondition — but only when it is
        // the *object* that is missing.
        if (is_absent_object(response)) {
            return;
        }
        require_success(response, "DeleteObject", key);
    }

    /// @brief Did the service evaluate a precondition and reject it?
    ///
    /// Both of OCI's codes count. The status alone would in fact be sufficient
    /// on this provider — both are 412 — but keying on the code anyway is what
    /// keeps one rule across five providers where Azure needs two statuses and
    /// S3's 409 means the opposite.
    [[nodiscard]] static auto is_precondition_failure(const oci_http_client::raw_response& response)
        -> bool {
        const auto code = oci_object_detail::error_code_of(response.body);
        if (code == oci_object_detail::k_if_none_match_failed ||
            code == oci_object_detail::k_if_match_failed) {
            return true;
        }
        return code.empty() && response.status == 412;
    }

    /// @brief Is this "the object does not exist", as opposed to any other 404?
    ///
    /// **Absence is decided by a whitelist**, which is the property that
    /// matters and is stronger than it looks: an *unknown* code is never
    /// absence, so a 404 this client has never seen before is an error rather
    /// than a silently empty log. `BucketNotFound` is named explicitly anyway —
    /// it is both a real misconfiguration and the code this tenancy's
    /// intermittent authorization flake produces, and it is the one a reader
    /// most needs to see excluded — but the exclusion is *redundant* given the
    /// whitelist below, and a mutant that deletes it changes nothing. What
    /// protects the behaviour is the whitelist; the named check protects the
    /// reader.
    [[nodiscard]] static auto is_absent_object(const oci_http_client::raw_response& response)
        -> bool {
        if (response.status != 404) {
            return false;
        }
        const auto code = oci_object_detail::error_code_of(response.body);
        if (code == oci_object_detail::k_bucket_not_found) {
            return false;
        }
        return code.empty() || code == oci_object_detail::k_object_not_found;
    }

    [[nodiscard]] static auto describe(const oci_http_client::raw_response& response,
                                       std::string_view op, const std::string& subject)
        -> std::string {
        std::string out = "oci_object_storage_client: " + std::string(op) + " " + subject +
                          " failed with HTTP " + std::to_string(response.status);
        const auto code = oci_object_detail::error_code_of(response.body);
        if (!code.empty()) {
            out += ": " + code;
        } else if (!response.body.empty()) {
            out += ": " + response.body;
        }
        return out;
    }

    static auto require_success(const oci_http_client::raw_response& response, std::string_view op,
                                const std::string& subject) -> void {
        if (response.status >= 200 && response.status < 300) {
            return;
        }
        throw std::runtime_error(describe(response, op, subject));
    }

    std::shared_ptr<oci_http_client> _http;
    std::string _namespace;
};

static_assert(key_object_store<oci_object_storage_client>,
              "oci_object_storage_client must satisfy key_object_store");
static_assert(conditional_key_object_store<oci_object_storage_client>,
              "oci_object_storage_client must satisfy conditional_key_object_store — OCI expresses "
              "every conditional primitive including the conditional delete (spike-notes.md "
              "Finding 13), so fencing_mode::compare_and_swap must remain available for it");
static_assert(!content_md5_versioned_store<oci_object_storage_client>,
              "oci_object_storage_client must NOT declare version_is_content_md5: OCI's ETag is a "
              "UUID, and comparing it against a content digest would fail every write");

}  // namespace kythira
