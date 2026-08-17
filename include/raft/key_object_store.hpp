#pragma once

/// @file key_object_store.hpp
/// @brief The small store interface `object_store_persistence_engine` needs —
///        five operations, plus a conditional-write refinement used only by
///        fencing.
///
/// ## Why a concept rather than five engines
///
/// S3, Azure Blob, GCS, OCI Object Storage and Alibaba OSS do **not** differ
/// semantically. All five are a flat key→bytes store with per-key atomic
/// whole-object writes, no rename, no append, and no cross-key transaction.
/// What differs between them is authentication, error wire format, listing
/// pagination, and the spelling of the conditional-write precondition — none
/// of which is Raft-correctness logic. So the Raft-correctness body is written
/// once, generic over this concept, and each provider contributes only a
/// client. (Contrast the quorum managers and certificate providers, which are
/// independent siblings *because* a scaling group and an instance pool differ
/// in capacity model, lifecycle vocabulary and liveness signal.)
///
/// ## Deliberately not required
///
/// Multipart upload, copy, server-side-encryption configuration, bucket
/// creation, lifecycle policy management, tagging and versioning are all
/// absent. Buckets are operator-owned prerequisites in every sibling provider
/// spec in this tree and remain so here: the engine reads and writes objects,
/// it does not administer storage.
///
/// ## Retry-free at this layer
///
/// Every client satisfying this concept must be retry-free for mutating
/// operations, or expose an explicit knob set to zero retries. The engine owns
/// write retries, and its idempotency argument depends on knowing exactly what
/// was re-sent — a vendor SDK's opaque retry policy makes that argument
/// unverifiable from the outside.
///
/// ## `object_version`, and why it is opaque
///
/// S3, Azure Blob, OCI and OSS express a per-object version as an HTTP ETag;
/// GCS expresses it as a numeric `generation`, which is not an ETag and is in
/// fact stronger (a monotonically assigned integer naming a specific object
/// version, with `0` as the "does not exist" sentinel). Carrying both as an
/// opaque string is exactly what lets one engine predicate a conditional write
/// on either without ever inspecting it.
///
/// ## Bucket and key parameter types
///
/// The requires-expression below names `const std::string&` for `bucket` and
/// `key` rather than `std::string_view`. That is the *more permissive* of the
/// two: a client whose parameters are `std::string_view` still satisfies it
/// (string→string_view converts), while a client whose parameters are
/// `const std::string&` would fail a `string_view`-argument check, because
/// `std::string`'s converting constructor from `string_view` is explicit. The
/// engine builds every key as a `std::string` anyway, so nothing is lost, and
/// `alibaba_oss_client` — the live-verified reference this design generalizes —
/// keeps its signatures.

#include <concepts>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// @brief Opaque per-object version: an HTTP ETag on S3/Azure/OCI/OSS, a
///        decimal generation number on GCS. The engine never inspects it.
using object_version = std::string;

/// @brief What a successful PUT reports back.
///
/// A store that cannot report a version reports an empty one, which disqualifies
/// it from `fencing_mode::compare_and_swap` and nothing else.
struct put_result {
    object_version version;
};

/// @brief A fetched object and the version it was read at.
struct get_result {
    std::string body;
    object_version version;
};

/// @brief Thrown **only** when a service evaluated a precondition and rejected
///        it.
///
/// A distinct type because the engine's whole fencing behaviour rests on
/// telling "you lost the race" apart from "the network broke". A conditional
/// operation reports a rejected precondition by throwing this, never by a
/// sentinel return: a sentinel is silently ignorable at exactly the call sites
/// where ignoring it is a safety violation.
///
/// Note for client authors: a *benign* conditional-request race that the
/// service asks you to retry — S3's `409 ConditionalRequestConflict`, as
/// distinct from its `412` — is **not** this exception. Mapping it here would
/// turn a retryable race into a permanent, unclearable fence latch.
class object_precondition_failed : public std::runtime_error {
public:
    object_precondition_failed(std::string provider, std::string key, object_version expected,
                               const std::string& detail)
        : std::runtime_error(
              provider + ": precondition failed on " + key +
              (expected.empty() ? " (expected: absent)" : " (expected version: " + expected + ")") +
              (detail.empty() ? "" : ": " + detail)),
          _provider(std::move(provider)),
          _key(std::move(key)),
          _expected(std::move(expected)) {}

    /// The store that rejected the write (`provider_name()`).
    [[nodiscard]] auto provider() const noexcept -> const std::string& { return _provider; }
    /// The object key whose precondition failed.
    [[nodiscard]] auto key() const noexcept -> const std::string& { return _key; }
    /// The version the caller predicated the write on; empty for "must not exist".
    [[nodiscard]] auto expected_version() const noexcept -> const object_version& {
        return _expected;
    }

private:
    std::string _provider;
    std::string _key;
    object_version _expected;
};

// clang-format off
/// @brief The five operations the persistence engine needs from an object store.
///
/// | Operation | Semantics |
/// |---|---|
/// | `put_object`    | Whole-object overwrite; returns the stored object's version and never partially writes |
/// | `get_object`    | `nullopt` for absent — absence is an answer, not an error |
/// | `delete_object` | Idempotent: deleting an absent key succeeds |
/// | `list_keys`     | Fully paginated; a listing that cannot be completed **throws** rather than truncating |
/// | `provider_name` | `"s3"`, `"azure-blob"`, `"gcs"`, `"oci-objectstorage"`, `"oss"` — used in error messages, metrics and the backup manifest |
///
/// The listing rule is the one worth restating: a short listing is silent data
/// loss in a persistence engine, because recovery reads the log from a single
/// prefixed LIST and cannot tell "40 entries" from "60 entries, 40 shown".
// clang-format on
template<typename S>
concept key_object_store = requires(const S& store, const std::string& bucket,
                                    const std::string& key, std::string_view bytes) {
    { store.put_object(bucket, key, bytes) } -> std::same_as<put_result>;
    { store.get_object(bucket, key) } -> std::same_as<std::optional<get_result>>;
    { store.delete_object(bucket, key) } -> std::same_as<void>;
    { store.list_keys(bucket, key) } -> std::same_as<std::vector<std::string>>;
    { store.provider_name() } -> std::convertible_to<std::string_view>;
};

/// @brief "Create only if the object does not exist."
///
/// Spelled `If-None-Match: *` on S3/Azure/OCI, `ifGenerationMatch=0` on GCS,
/// `x-oss-forbid-overwrite: true` on OSS.
struct if_absent {};

/// @brief "Replace only if the object is exactly at this version."
///
/// Spelled `If-Match: <etag>` on S3/Azure/OCI, `ifGenerationMatch=<generation>`
/// on GCS.
struct if_version {
    object_version expected;
};

/// @brief The precondition a conditional operation is predicated on.
using precondition = std::variant<if_absent, if_version>;

/// @brief A store that can predicate a write on the object's current version.
///
/// Required at **compile time** by `fencing_mode::compare_and_swap`. A provider
/// that cannot express the precondition therefore loses the *option*, rather
/// than silently degrading to unconditional writes — a fence that is believed
/// in and absent is worse than no fence at all, and this refinement exists to
/// make that failure a compile error.
template<typename S>
concept conditional_key_object_store =
    key_object_store<S> &&
    requires(const S& store, const std::string& bucket, const std::string& key,
             std::string_view bytes, const precondition& pre) {
        { store.put_object_if(bucket, key, bytes, pre) } -> std::same_as<put_result>;
        { store.delete_object_if(bucket, key, pre) } -> std::same_as<void>;
    };

/// @brief A store whose `put_result::version` **is** the MD5 of the content it
///        just stored, for a single-part upload.
///
/// True on exactly two of the five providers this design targets — S3 (lowercase
/// hex) and Alibaba OSS (uppercase hex) — and false on the other three, whose
/// version is an opaque token: Azure's ETag is a timestamp token, OCI's is a
/// **UUID** and GCS's is opaque (spike-notes.md Findings 4, 10-13). A client
/// that assumed the S3 shape would compare an opaque token against an MD5 and
/// fail every write, which is why this is a **declaration by the client** rather
/// than something the engine may infer.
///
/// Declared by adding `static constexpr bool version_is_content_md5 = true;`. A
/// store that says nothing is simply not one of these — the first conjunct
/// short-circuits, so the member is genuinely optional and no existing client
/// needs touching. Where it *is* declared,
/// `object_store_persistence_engine` verifies every PUT's returned version
/// against the digest it computed locally (Requirement 7.2), comparing
/// case-insensitively and ignoring surrounding quotes because the two providers
/// disagree on case and at least one returns the ETag quoted.
///
/// This says nothing about the **service-side** check of Requirement 7.1, which
/// every one of the five supports and each spells differently. That one is a
/// header the client sends and the service evaluates; it cannot be expressed
/// here, because the engine does not speak HTTP.
template<typename S>
concept content_md5_versioned_store =
    key_object_store<S> && requires { S::version_is_content_md5; } && S::version_is_content_md5;

}  // namespace kythira
