// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gcp_gcs_client.hpp
/// @brief Minimal Google Cloud Storage object client over `google-cloud-cpp`'s
///        `storage::Client` — exactly the operations
///        `object_store_persistence_engine` needs, plus the conditional-write
///        refinement `fencing_mode::compare_and_swap` requires.
///
/// The fifth and last provider client in this design, and the only one whose
/// version is not an ETag.
///
/// ## The version is a **generation**, and that is why `object_version` is opaque
///
/// S3, Azure, OCI and OSS all express a per-object version as an HTTP ETag.
/// GCS expresses it as a `generation`: a decimal counter GCS assigns on every
/// write (`1786971917733520` → `1786971917901538`, measured — spike-notes.md
/// Finding 12), carried as a **query parameter** rather than a header. An
/// engine that assumed "quoted hash in a header" would be wrong here and one
/// that assumed "decimal counter" would be wrong on the other four, which is
/// the concrete justification for `object_version` being a string the engine
/// never inspects. This client is the only place in the tree that parses one,
/// and it parses it back out of the same string it produced.
///
/// GCS's ETag is **not** the content MD5 — it is an opaque token
/// (`CL/I06jPqJYDEAE=`, measured) — so `version_is_content_md5` is deliberately
/// absent here and `static_assert`ed on the negative below. The content digest
/// is available, but as the `md5Hash` *metadata* field rather than as the
/// version, which is the route a future local check would take.
///
/// ## Zero retries, explicitly (Requirement 1.5 / 12.2)
///
/// `RetryPolicyOption` is set to `LimitedErrorCountRetryPolicy(0)`. Same reason
/// as every other client here: `object_store_persistence_engine` performs
/// exactly one PUT retry and its idempotency argument rests on knowing what was
/// re-sent and how often. A library retry underneath makes that argument
/// unverifiable from the outside, and a conditional write re-issued by a layer
/// that does not know its precondition is now stale is exactly how a benign
/// race becomes an unclearable fence latch. This one matters more than usual:
/// google-cloud-cpp's *default* storage retry policy is a 15-minute
/// `LimitedTimeRetryPolicy`, so leaving it alone would put a quarter-hour retry
/// loop under every call.
///
/// ## Options: storage does not take the options the other GCP clients take
///
/// `gcp_detail::make_base_options()` is deliberately **not** reused here, and
/// the reason is not stylistic. `storage::Client` has its own option set
/// (`storage::ClientOptionList`) which does not contain the two that helper
/// sets: the endpoint is `storage::RestEndpointOption`, not the generic
/// `google::cloud::EndpointOption`, and the stall bounds are
/// `storage::TransferStallTimeoutOption` / `storage::DownloadStallTimeoutOption`
/// — public options in the `storage` namespace, not the `rest_internal` ones
/// that helper reaches for. Passing that helper's output would set three
/// options the storage connection does not read, so the endpoint override would
/// be silently ignored and every unit case here would talk to real GCS. Only
/// the credentials half is genuinely shared, and it is one `if`.
///
/// ## Absence is a whitelist, and on GCS the whitelist cannot be a code
///
/// Every other client in this design tells "the object is absent" from "the
/// bucket is missing" by reading the service's own error code — `NoSuchKey` vs
/// `NoSuchBucket` on S3, `ObjectNotFound` vs `BucketNotFound` on OCI. **GCS
/// offers no such discriminator.** Both answers are `404` carrying the
/// identical machine-readable `reason: "notFound"`; only the human-readable
/// message differs, and that was measured against live GCS rather than read out
/// of documentation:
///
///     GET  <real bucket>/<absent key>  → 404 reason=notFound
///                                        "No such object: <bucket>/<key>"
///     GET  <absent bucket>/<any key>   → 404 reason=notFound
///                                        "The specified bucket does not exist."
///
/// So the discriminator here is the message prefix `No such object:`, and it is
/// a **whitelist**: a 404 that does not match — a missing bucket, a bare 404
/// with no payload at all — throws rather than reporting absence. That
/// direction is the whole point. Reading a misconfigured bucket name as "the
/// key is not written yet" would present to the engine as an empty Raft log,
/// which is the one failure it cannot detect at runtime; whereas if Google ever
/// rewords the message, absence starts *throwing*, which is loud, immediate and
/// safe. Matching prose is an unattractive thing to do and it is done here
/// because the alternative is worse, not because it is nice.
///
/// ## A zero-byte object has no readable version, and costs a second request
///
/// Reading a zero-byte object through `ReadObject` returns an ok status, an
/// empty body and **no response headers whatsoever** — measured against live
/// GCS, where the same run's five-byte read returned 22 headers including
/// `x-goog-generation`. With no bytes to stream the library never captures the
/// response, which is entirely reasonable and completely invisible until a
/// caller wants the version. Empty objects are legal here and the engine
/// already handles them, so `get_object` falls back to `GetObjectMetadata` on
/// that path. See `zero_byte_get` for how the window between the two calls is
/// closed.
///
/// ## The conditional DELETE's two different answers to a 404
///
/// GCS answers a DELETE of an absent object with 404, not 204, so
/// `delete_object` maps that back to success to get the idempotence the
/// engine's truncation path needs. The **conditional** delete cannot do the
/// same thing unconditionally, and the asymmetry is deliberate:
///
/// - `delete_object_if(if_absent)` — the caller asked to delete only if the
///   object does not exist. A 404 means the precondition **held**, so this
///   succeeds. (Measured: `ifGenerationMatch=0` against an existing object is
///   412 `conditionNotMet`, and against an absent one is 404.)
/// - `delete_object_if(if_version{g})` — the caller asked to delete only if the
///   object is exactly at generation `g`. A 404 means it is not there at all,
///   so the precondition **did not hold** and someone else won. Reporting that
///   as success would silently swallow exactly the lost race the fence exists
///   to detect, so it throws `object_precondition_failed`.
///
/// ## End-to-end integrity on the wire (Requirement 7.1)
///
/// Every insert carries `MD5HashValue`, which google-cloud-cpp sends as the
/// `md5Hash` field of the upload's metadata — base64 of the raw 16 digest
/// bytes, **not** hex. GCS verifies it and answers `400 invalid` on a mismatch
/// (measured, Finding 12), a fifth spelling of "your checksum is wrong" and a
/// notably uninformative one. MD5 hashing is *off* by default in this library
/// (`DisableMD5Hash` documents the default as disabled), so without this the
/// engine's writes would carry no digest the service checks.
///
/// The library's own crc32c is left enabled. It is an additional check on a
/// separate field rather than a competing framing of the body — unlike the AWS
/// SDK's flexible checksums, which can move the payload onto `aws-chunked`
/// trailers and which `aws_s3_client` therefore has to switch off.
///
/// ## Considered and rejected: a hand-rolled JSON API client
///
/// `azure_blob_client` and `alibaba_oss_client` speak their services' REST APIs
/// directly over httplib and take no SDK dependency, and doing the same here
/// was considered. The fork, so it is visible rather than re-derived:
///
/// - **For** hand-rolling: GCS would then work in a *default* build, like OCI
///   and Azure do, instead of only under the opt-in `gcp` vcpkg feature. The
///   JSON API itself is simple — this file's whole behaviour was elicited with
///   `curl`.
/// - **Against**, and decisive: authentication. Azure Blob is reachable with a
///   bearer token from a credential chain the tree already carries, and OCI and
///   OSS use request signing this tree already implements. GCS needs a
///   **service-account JWT**: build and RS256-sign an assertion, exchange it at
///   the OAuth2 token endpoint, cache and refresh. That is a new
///   security-critical component whose failure mode is silent, and
///   google-cloud-cpp already carries it — along with Application Default
///   Credentials, which is how a node on GCE actually authenticates and the
///   analogue of the Instance Principal support `oci_object_storage_client`
///   inherits for free.
///
/// The cost accepted is that GCS is the one provider whose client needs an
/// opt-in dependency. `KYTHIRA_HAS_GCP_STORAGE` is found independently of
/// `KYTHIRA_HAS_GCP_SDK` and `KYTHIRA_HAS_GCP_PRIVATECA`, so this costs a build
/// the storage component and not the entire Compute API surface.

#include <raft/fault_injection.hpp>
#include <raft/gcp_client_config.hpp>
#include <raft/key_object_store.hpp>

#ifdef KYTHIRA_HAS_GCP_STORAGE

#include <google/cloud/credentials.h>
#include <google/cloud/options.h>
#include <google/cloud/status.h>
#include <google/cloud/storage/client.h>
#include <google/cloud/storage/hashing_options.h>
#include <google/cloud/storage/options.h>
#include <google/cloud/storage/retry_policy.h>

#include <charconv>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace gcp_gcs_detail {

namespace gcs = ::google::cloud::storage;

/// GCS's own reason for a rejected `ifGenerationMatch`, on both the upload and
/// the delete path. 412.
inline constexpr std::string_view k_condition_not_met = "conditionNotMet";

/// The prefix of the 404 message that means "this object is not there", as
/// opposed to the 404 that means "this bucket is not there". GCS gives both the
/// same `reason`, so this string is the only discriminator available — see the
/// file comment for why matching it is nonetheless the safe choice.
inline constexpr std::string_view k_absent_object_message = "No such object:";

/// @brief `std::string_view` → `absl::string_view`, spelled out.
///
/// The two are the same type only when Abseil is configured to alias
/// `std::string_view`; going through `(data, size)` compiles either way.
[[nodiscard]] inline auto to_absl(std::string_view s) -> absl::string_view {
    return absl::string_view(s.data(), s.size());
}

/// @brief Did the service evaluate a precondition and reject it?
///
/// Reads GCS's own `reason` first and falls back to the status code only when
/// no reason arrived, which is the rule every client in this design follows: a
/// status *class* is not a reason.
[[nodiscard]] inline auto is_precondition_failure(const google::cloud::Status& status) -> bool {
    const auto& reason = status.error_info().reason();
    if (reason == k_condition_not_met) {
        return true;
    }
    return reason.empty() && status.code() == google::cloud::StatusCode::kFailedPrecondition;
}

/// @brief Is this "the object does not exist", as opposed to any other 404?
///
/// The whitelist described in the file comment. A 404 for a missing *bucket*
/// carries the same `reason: "notFound"` as a missing object, so the message is
/// consulted; anything that is not recognisably an absent object is not
/// absence, and the caller throws.
///
/// The search is a **substring** search and not a prefix match, which is not
/// laziness. google-cloud-cpp's retry loop rewrites the message on the way out:
/// what the service sent as `No such object: b/k` arrives here as
/// `Permanent error, with a last message of No such object: b/k`. A prefix
/// match therefore recognises nothing at all — it was written that way first,
/// and every absence case in the suite failed. The original text is embedded
/// verbatim in each of the wrapper's forms ("Permanent error, …",
/// "Retry policy exhausted, …", "Retry loop cancelled, …"), so searching for it
/// works whether the status was wrapped or not.
[[nodiscard]] inline auto is_absent_object(const google::cloud::Status& status) -> bool {
    if (status.code() != google::cloud::StatusCode::kNotFound) {
        return false;
    }
    return status.message().find(k_absent_object_message) != std::string::npos;
}

/// @brief One sentence carrying the operation, the subject, the library's
///        status code, GCS's own reason and its message.
[[nodiscard]] inline auto describe(std::string_view op, const std::string& subject,
                                   const google::cloud::Status& status) -> std::string {
    std::string out = "gcp_gcs_client: ";
    out += op;
    out += ' ';
    out += subject;
    out += " failed with ";
    out += google::cloud::StatusCodeToString(status.code());
    if (!status.error_info().reason().empty()) {
        out += ": ";
        out += status.error_info().reason();
    }
    if (!status.message().empty()) {
        out += ": ";
        out += status.message();
    }
    return out;
}

/// @brief A generation string back to the integer GCS wants in
///        `ifGenerationMatch`.
///
/// The only place in this tree that looks inside an `object_version`, and it is
/// reading back a string this same client produced. A value that is not a
/// decimal generation is a caller error rather than a service failure —
/// `std::invalid_argument` rather than an ordinary runtime error — and it must
/// not be quietly coerced to 0, which is the wire spelling of "must not exist"
/// and would turn an overwrite CAS into a create-only.
[[nodiscard]] inline auto parse_generation(const object_version& version, const std::string& key)
    -> std::int64_t {
    std::int64_t generation = 0;
    const char* const begin = version.data();
    const char* const end = begin + version.size();
    const auto [ptr, ec] = std::from_chars(begin, end, generation);
    if (ec != std::errc{} || ptr != end || version.empty()) {
        throw std::invalid_argument(
            "gcp_gcs_client: object version \"" + version + "\" for key " + key +
            " is not a GCS generation — a generation is a decimal counter, and coercing an"
            " unparseable version to 0 would silently turn an overwrite precondition into a"
            " create-only one");
    }
    return generation;
}

}  // namespace gcp_gcs_detail

/// @brief The object operations the persistence engine needs, over
///        `google::cloud::storage::Client`.
///
/// Satisfies `key_object_store` and `conditional_key_object_store`; the
/// `static_assert`s live beside the definition rather than in a test, so a
/// signature change that breaks a concept is a compile error in this header.
class gcp_gcs_client {
public:
    explicit gcp_gcs_client(gcp_client_config cfg) : _cfg(std::move(cfg)) {
        namespace gcs = ::google::cloud::storage;

        google::cloud::Options opts;
        if (!_cfg.credentials_json.empty()) {
            opts.set<google::cloud::UnifiedCredentialsOption>(
                google::cloud::MakeServiceAccountCredentials(_cfg.credentials_json));
        } else {
            opts.set<google::cloud::UnifiedCredentialsOption>(
                google::cloud::MakeGoogleDefaultCredentials());
        }
        if (!_cfg.endpoint_override.empty()) {
            // `storage::RestEndpointOption`, not `google::cloud::EndpointOption`
            // — see the file comment. This one override covers both the
            // metadata and the upload URL prefixes.
            opts.set<gcs::RestEndpointOption>(_cfg.endpoint_override);
        }
        // Object operations name a bucket and never a project, so an empty
        // `project_id` is not fatal here even though it is required by every
        // other GCP component in this tree — refusing to construct over a
        // configuration that works would be a check for its own sake.
        if (!_cfg.project_id.empty()) {
            opts.set<gcs::ProjectIdOption>(_cfg.project_id);
        }
        if (_cfg.api_timeout.count() > 0) {
            // A stall bound, not a total-duration bound: a transfer that keeps
            // making progress stays alive. That is what makes it safe to apply
            // to an arbitrarily large object.
            opts.set<gcs::TransferStallTimeoutOption>(_cfg.api_timeout);
            opts.set<gcs::DownloadStallTimeoutOption>(_cfg.api_timeout);
        }
        // Requirement 1.5/12.2, and see the file comment: the default this
        // replaces is a 15-minute retry loop, not "no retries".
        opts.set<gcs::RetryPolicyOption>(
            std::make_shared<gcs::LimitedErrorCountRetryPolicy>(/*maximum_failures=*/0));

        _client = std::make_shared<gcs::Client>(std::move(opts));
    }

    /// @brief The name this store answers to in error messages, metrics and a
    ///        backup manifest (`kythira::key_object_store`).
    [[nodiscard]] static auto provider_name() -> std::string_view { return "gcs"; }

    /// @brief PUT one object, unconditionally. No retry — the engine owns write
    ///        retries.
    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        fiu_do_on("raft/gcp/gcs/put_object",
                  throw std::runtime_error("chaos: raft/gcp/gcs/put_object " + key););
        return send_insert(bucket, key, bytes, std::nullopt);
    }

    /// @brief PUT one object only if `pre` holds, throwing
    ///        `object_precondition_failed` when GCS rejects it.
    ///
    /// Issued exactly once. A conditional write is not idempotent the way an
    /// unconditional one is: its precondition is stale by construction if a
    /// previous attempt landed, so a retry here would either be a no-op or a
    /// spurious latch.
    auto put_object_if(const std::string& bucket, const std::string& key, std::string_view bytes,
                       const precondition& pre) const -> put_result {
        fiu_do_on("raft/gcp/gcs/put_object_if",
                  throw std::runtime_error("chaos: raft/gcp/gcs/put_object_if " + key););
        return send_insert(bucket, key, bytes, pre);
    }

    /// @brief GET one object; `nullopt` when the key is absent (absence is an
    ///        answer, not an error, for a persistence load path).
    ///
    /// A 404 that is not recognisably an absent *object* throws — see the file
    /// comment on why the whitelist points this way.
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        fiu_do_on("raft/gcp/gcs/get_object",
                  throw std::runtime_error("chaos: raft/gcp/gcs/get_object " + key););
        auto stream = _client->ReadObject(bucket, key);
        if (!stream.status().ok()) {
            if (gcp_gcs_detail::is_absent_object(stream.status())) {
                return std::nullopt;
            }
            throw std::runtime_error(gcp_gcs_detail::describe("ReadObject", key, stream.status()));
        }

        std::string body{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};

        // Checked *again* after draining, and this is not belt-and-braces: the
        // request succeeds as soon as headers arrive, so a download that dies
        // partway leaves an ok status here and a short body there. Returning
        // that would hand the engine a truncated Raft log entry it has no way
        // to recognise — the read-path analogue of the short-listing rule.
        if (!stream.status().ok()) {
            throw std::runtime_error(gcp_gcs_detail::describe("ReadObject", key, stream.status()));
        }

        const auto& generation = stream.generation();
        if (generation.has_value()) {
            return get_result{std::move(body), std::to_string(*generation)};
        }
        if (!body.empty()) {
            // Bytes arrived but no `x-goog-generation` did. The version read
            // here is what a fenced engine later predicates an
            // `ifGenerationMatch` on, so a versionless read is not a harmless
            // degradation — it would turn the next conditional write into a
            // create-only. Nothing known produces this; it is loud rather than
            // guessed at.
            throw std::runtime_error(
                "gcp_gcs_client: ReadObject " + key + " returned " + std::to_string(body.size()) +
                " bytes but no generation — refusing to report a versionless read that a"
                " conditional write would later be predicated on");
        }
        return zero_byte_get(bucket, key);
    }

    /// @brief DELETE one object.
    ///
    /// GCS answers 404 for an absent object where S3 and OSS answer 204, so the
    /// idempotence the engine's truncation path needs is supplied here rather
    /// than by the service. A 404 for a missing *bucket* still throws: it goes
    /// through the same whitelist, so this cannot swallow a misconfiguration.
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        fiu_do_on("raft/gcp/gcs/delete_object",
                  throw std::runtime_error("chaos: raft/gcp/gcs/delete_object " + key););
        send_delete(bucket, key, std::nullopt);
    }

    /// @brief DELETE one object only if `pre` holds. A stale `ifGenerationMatch`
    ///        is refused **and the object survives** — measured, Finding 12.
    ///
    /// Unlike `aws_s3_client`, `if_absent` is expressible here:
    /// `ifGenerationMatch=0` is accepted on a delete. See the file comment for
    /// why the two preconditions read a 404 differently.
    auto delete_object_if(const std::string& bucket, const std::string& key,
                          const precondition& pre) const -> void {
        fiu_do_on("raft/gcp/gcs/delete_object_if",
                  throw std::runtime_error("chaos: raft/gcp/gcs/delete_object_if " + key););
        send_delete(bucket, key, pre);
    }

    /// @brief Every key under `prefix`.
    ///
    /// A page that cannot be fetched **throws**: recovery reads the log from one
    /// prefixed LIST and cannot tell "40 entries" from "60 entries, 40 shown",
    /// so a short listing here is silent data loss.
    ///
    /// Pagination is the library's, which is a real difference from
    /// `alibaba_oss_client` and `aws_s3_client` and is recorded rather than
    /// glossed: there is no continuation token to inspect, so the
    /// truncated-with-no-token case those two guard against cannot be checked
    /// from here. What *is* checked is the failure that reaches this loop — the
    /// reader reports a failed page as a bad `StatusOr` on the iterator, and
    /// every element's status is tested before its name is taken.
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        namespace gcs = ::google::cloud::storage;
        fiu_do_on("raft/gcp/gcs/list_keys",
                  throw std::runtime_error("chaos: raft/gcp/gcs/list_keys " + prefix););
        std::vector<std::string> keys;
        for (auto& item : _client->ListObjects(bucket, gcs::Prefix(prefix))) {
            if (!item) {
                throw std::runtime_error(
                    gcp_gcs_detail::describe("ListObjects", prefix, item.status()) +
                    " — refusing to treat a listing that could not be completed as complete");
            }
            keys.push_back(item->name());
        }
        return keys;
    }

private:
    /// @brief The version of a **zero-byte** object, which a download cannot
    ///        report, fetched from its metadata instead.
    ///
    /// Measured against live GCS rather than inferred: reading a zero-byte
    /// object through `ReadObject` yields an ok status, an empty body, and
    /// **no response headers at all** — `headers().size() == 0`, so there is no
    /// `x-goog-generation` to read. The same read of a five-byte object in the
    /// same run returned 22 headers including the generation. The library has
    /// no bytes to stream and so never captures the response, which is a
    /// perfectly reasonable thing for it to do and completely invisible until a
    /// caller needs the version.
    ///
    /// Empty objects are legal in this store and the engine handles them (an
    /// empty snapshot body is a case it already tests), so refusing to read one
    /// is not an option. The cost is a second round trip on exactly this path.
    ///
    /// The `size() != 0` check is what closes the window between the two calls:
    /// if the object grew in between, this metadata describes bytes that were
    /// never read, and returning its generation would name a version for
    /// content the caller does not have. A 404 in between means it was deleted,
    /// which is reported as the absence it now is — a caller that acts on that
    /// will attempt a create-only write, which is refused loudly if the object
    /// came back, rather than an overwrite that would not be.
    [[nodiscard]] auto zero_byte_get(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        auto metadata = _client->GetObjectMetadata(bucket, key);
        if (!metadata) {
            if (gcp_gcs_detail::is_absent_object(metadata.status())) {
                return std::nullopt;
            }
            throw std::runtime_error(
                gcp_gcs_detail::describe("GetObjectMetadata", key, metadata.status()) +
                " — needed because a zero-byte download reports no generation");
        }
        if (metadata->size() != 0) {
            throw std::runtime_error(
                "gcp_gcs_client: " + key + " read as zero bytes but its metadata reports " +
                std::to_string(metadata->size()) +
                " — the object changed between the download and the metadata fetch, and naming a"
                " version for content that was never read would be worse than failing");
        }
        return get_result{std::string{}, std::to_string(metadata->generation())};
    }

    [[nodiscard]] auto send_insert(const std::string& bucket, const std::string& key,
                                   std::string_view bytes,
                                   const std::optional<precondition>& pre) const -> put_result {
        namespace gcs = ::google::cloud::storage;

        // An unset `ComplexOption` is not sent, so the unconditional and
        // conditional paths are one call rather than two that can drift.
        gcs::IfGenerationMatch if_generation;
        object_version expected;
        if (pre) {
            if_generation = gcs::IfGenerationMatch(generation_for(*pre, key, expected));
        }

        auto metadata = _client->InsertObject(
            bucket, key, gcp_gcs_detail::to_absl(bytes),
            gcs::MD5HashValue(gcs::ComputeMD5Hash(gcp_gcs_detail::to_absl(bytes))), if_generation);

        if (!metadata) {
            const auto& status = metadata.status();
            if (pre && gcp_gcs_detail::is_precondition_failure(status)) {
                throw object_precondition_failed(
                    std::string(provider_name()), key, expected,
                    gcp_gcs_detail::describe("InsertObject", key, status));
            }
            throw std::runtime_error(gcp_gcs_detail::describe("InsertObject", key, status));
        }
        return put_result{std::to_string(metadata->generation())};
    }

    auto send_delete(const std::string& bucket, const std::string& key,
                     const std::optional<precondition>& pre) const -> void {
        namespace gcs = ::google::cloud::storage;

        gcs::IfGenerationMatch if_generation;
        object_version expected;
        if (pre) {
            if_generation = gcs::IfGenerationMatch(generation_for(*pre, key, expected));
        }

        const auto status = _client->DeleteObject(bucket, key, if_generation);
        if (status.ok()) {
            return;
        }
        if (pre && gcp_gcs_detail::is_precondition_failure(status)) {
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             gcp_gcs_detail::describe("DeleteObject", key, status));
        }
        if (gcp_gcs_detail::is_absent_object(status)) {
            // Unconditional, or "delete only if absent": nothing is there and
            // nothing needed to be, so the caller got what it asked for.
            if (!pre || std::holds_alternative<if_absent>(*pre)) {
                return;
            }
            // "Delete only if at generation g", and it is not there at all. The
            // precondition did not hold, and calling that success would swallow
            // precisely the lost race the fence exists to detect.
            throw object_precondition_failed(std::string(provider_name()), key, expected,
                                             gcp_gcs_detail::describe("DeleteObject", key, status));
        }
        throw std::runtime_error(gcp_gcs_detail::describe("DeleteObject", key, status));
    }

    /// @brief The `ifGenerationMatch` value for `pre`, recording in @p expected
    ///        the version to name in an `object_precondition_failed`.
    ///
    /// `0` is GCS's wire spelling of "the object must not exist", which is why
    /// `parse_generation` refuses to produce it from an unparseable version.
    [[nodiscard]] static auto generation_for(const precondition& pre, const std::string& key,
                                             object_version& expected) -> std::int64_t {
        return std::visit(
            [&](const auto& p) -> std::int64_t {
                using T = std::decay_t<decltype(p)>;
                if constexpr (std::is_same_v<T, if_absent>) {
                    return 0;
                } else {
                    expected = p.expected;
                    return gcp_gcs_detail::parse_generation(p.expected, key);
                }
            },
            pre);
    }

    gcp_client_config _cfg;
    std::shared_ptr<google::cloud::storage::Client> _client;
};

static_assert(key_object_store<gcp_gcs_client>, "gcp_gcs_client must satisfy key_object_store");
static_assert(
    conditional_key_object_store<gcp_gcs_client>,
    "gcp_gcs_client must satisfy conditional_key_object_store — GCS expresses every conditional "
    "primitive through ifGenerationMatch, conditional delete included (spike-notes.md Finding 12), "
    "so fencing_mode::compare_and_swap must remain available for it");
static_assert(!content_md5_versioned_store<gcp_gcs_client>,
              "gcp_gcs_client's version is a generation counter and its ETag is an opaque token — "
              "neither is the content MD5, so the engine must not verify PUTs against a locally "
              "computed digest for this store");

}  // namespace kythira

#endif  // KYTHIRA_HAS_GCP_STORAGE
