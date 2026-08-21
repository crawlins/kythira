// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_s3_client.hpp
/// @brief Minimal AWS S3 object client over `Aws::S3::S3Client` — exactly the
///        operations `object_store_persistence_engine` needs, plus the
///        conditional-write refinement that `fencing_mode::compare_and_swap`
///        requires.
///
/// This is the one provider in this design where the capability is a **pure
/// addition to an already-paid-for build**: `aws-sdk-cpp` is an unconditional
/// dependency of this tree, its `s3` feature is already listed, and
/// `CMakeLists.txt`'s `kythira_find_optional(AWS_SDK AWSSDK COMPONENTS core ec2
/// autoscaling iam s3 sts)` already resolves it. So unlike Alibaba OSS and Azure
/// Blob there is no hand-rolled signer here, and unlike GCS there is no new
/// dependency.
///
/// ## Zero retries, explicitly (Requirement 1.5 / 12.2)
///
/// The SDK's retry strategy is forced to **zero** retries. This is not a
/// performance choice: `object_store_persistence_engine` performs exactly one
/// PUT retry and its idempotency argument rests on knowing precisely what was
/// re-sent and how many times. An SDK retry underneath that makes the argument
/// unverifiable from the outside — the engine would see one call and the
/// service would see three, and a conditional PUT re-issued by a layer that
/// does not know the precondition is stale is exactly how a benign race becomes
/// an unclearable fence latch. The unit suite counts requests against a failing
/// endpoint rather than trusting this comment.
///
/// For the same reason this client does **not** replicate the single read retry
/// `alibaba_oss_client` performs on 5xx. That client hand-rolls its transport
/// and had no other retry anywhere; here a hidden retry above a
/// deliberately-zeroed SDK strategy would reintroduce the opacity the zeroing
/// exists to remove. Requirement 2.5 says reads *may* retry once, not must.
///
/// ## Reading a verdict from the service's own error code, never the status class
///
/// S3 answers **412 `PreconditionFailed`** when a precondition is rejected and
/// **409 `ConditionalRequestConflict`** when two conditional requests race and
/// the caller should retry. Those two mean opposite things, and this is the
/// single most consequential mapping in this file: 412 latches the engine's
/// fence permanently and unrecoverably, 409 must not. Note also that Alibaba
/// OSS uses 409 for the *opposite*, latching meaning (`FileAlreadyExists`), so
/// a bare status is not portable even within this design — the code is read
/// first and the status only as a fallback when no code arrived.
///
/// The 409 path was **not elicited live**: the task-0 probe raced 8, 16 and 32
/// concurrent create-only PUTs at one fresh key and every one of 53 losers got
/// 412 (spike-notes.md Finding 10). That is evidence about the common case and
/// not evidence that 409 cannot happen — AWS documents it — so the mapping
/// rests on documentation and is pinned by a unit case rather than deleted as
/// unreachable.
///
/// Neither code is modelled in `Aws::S3::S3Errors` (there is no
/// `PRECONDITION_FAILED` enumerator in this SDK), so both arrive as
/// `S3Errors::UNKNOWN` carrying the wire code in `GetExceptionName()`. Reading
/// the error type enum alone would collapse them into one another.
///
/// ## End-to-end integrity on the wire (Requirement 7.1)
///
/// Every PUT carries an explicit `Content-MD5`, base64 of the raw 16 digest
/// bytes — **not** the hex spelling the ETag uses. S3 verifies it and answers
/// **400 `BadDigest`** on a mismatch (confirmed live, Finding 10).
///
/// The SDK would otherwise add a CRC32 of its own: its
/// `requestChecksumCalculation` defaults to `WHEN_SUPPORTED`, which computes a
/// checksum per request and can move the body onto `aws-chunked` trailer
/// framing to carry it. That default is switched to `WHEN_REQUIRED` here, so
/// the integrity check on the wire is the one this file names, in the encoding
/// task 0.5 verified, rather than whatever the SDK's default is on the day of
/// the build. An SDK default that silently changes the framing of every write
/// is precisely the class of thing this design pins.
///
/// ## `version_is_content_md5`, and the one bucket configuration that breaks it
///
/// S3's ETag **is** the lowercase MD5 hex of the content for a single-part
/// upload, so this client declares `version_is_content_md5` and the engine
/// verifies every PUT's returned version locally against its own digest
/// (Requirement 7.2). That equality does **not** hold for a bucket with SSE-KMS
/// or SSE-C default encryption, where S3 returns an ETag that is not a content
/// digest — against such a bucket every write would fail verification, and the
/// remedy is `object_persistence_options::verify_checksums = false`, which
/// leaves the service-side `Content-MD5` check above untouched. Multipart is a
/// documented non-goal of this engine, so the other ETag-is-not-an-MD5 case
/// cannot arise here.
///
/// ## One stated residual: a listing with no `IsTruncated` at all
///
/// `alibaba_oss_client` owns its XML parser and therefore *throws* when a
/// listing response carries no `<IsTruncated>` element, on the rule that a
/// listing it cannot interpret must never be read as "no more keys". The SDK's
/// generated parser instead **defaults the field to false**, which this client
/// cannot distinguish from an explicit false. Real S3 always sends the element,
/// so this is a difference in what a *malformed* response does, not in what a
/// real one does — but it is a capability the hand-rolled client has and this
/// one does not, and it is recorded rather than glossed. The truncated-but-no-
/// continuation-token case, which is the one an S3-compatible service can
/// plausibly get wrong, **is** caught below.
///
/// ## Addressing
///
/// Virtual-host style against real S3; **path-style whenever
/// `endpoint_override` is set**, so a local mock server or LocalStack needs no
/// wildcard DNS — the same rule `alibaba_oss_client` follows and for the same
/// reason. Setting an override also disables IMDS config lookup: a request
/// aimed at `127.0.0.1` has no business stalling on an instance-metadata probe.
///
/// One thing measured rather than assumed, because it changes what the setting
/// is *for*: deleting `useVirtualAddressing = false` does **not** break a mock
/// addressed by IP. The SDK's endpoint rules already fall back to path style
/// when the endpoint host is a literal address, so a `127.0.0.1` mock works
/// either way. The setting earns its place on the **hostname** case —
/// LocalStack at `http://localstack:4566`, or any named mock — where virtual
/// addressing would ask for `<bucket>.localstack` and need wildcard DNS that
/// does not exist. The unit suite therefore pins the hostname case
/// specifically; an IP-only test would have passed with the line removed.
///
/// ## SDK lifecycle
///
/// `Aws::InitAPI` / `Aws::ShutdownAPI` are the caller's, exactly as for
/// `aws_ec2_quorum_manager` and `aws_asg_quorum_manager`. Constructing this
/// client without an initialised SDK is undefined behaviour in the SDK, not
/// something this header can check.

#include <raft/aws_client_config.hpp>
#include <raft/fault_injection.hpp>
#include <raft/key_object_store.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

namespace aws_s3_detail {

/// S3's own error code for a rejected precondition. 412.
inline constexpr const char* k_precondition_failed = "PreconditionFailed";

/// S3's own error code for two conditional requests racing — **retryable**,
/// and emphatically not a precondition failure. 409.
inline constexpr const char* k_conditional_conflict = "ConditionalRequestConflict";

/// 404 for the object. Absence is an answer for a persistence load path.
inline constexpr const char* k_no_such_key = "NoSuchKey";

/// 404 for the *bucket*. Also a 404, and emphatically not "the object is
/// absent" — a missing bucket is a misconfiguration that must surface, not a
/// key the engine may treat as unwritten.
inline constexpr const char* k_no_such_bucket = "NoSuchBucket";

/// @brief `std::string` → `Aws::String`, spelled out rather than relying on an
///        implicit conversion.
///
/// The two are the same type only while the SDK is built without
/// `USE_AWS_MEMORY_MANAGEMENT`; with it, `Aws::String` carries a different
/// allocator and no converting constructor exists. Going through
/// `(data, size)` compiles either way and is the only form that also preserves
/// embedded nulls.
[[nodiscard]] inline auto to_aws(std::string_view s) -> Aws::String {
    return Aws::String(s.data(), s.size());
}

/// @brief `Aws::String` → `std::string`; see `to_aws`.
[[nodiscard]] inline auto from_aws(const Aws::String& s) -> std::string {
    return std::string(s.data(), s.size());
}

/// The HTTP status an error arrived with, as a plain integer.
[[nodiscard]] inline auto http_status(const Aws::S3::S3Error& err) -> int {
    return static_cast<int>(err.GetResponseCode());
}

/// @brief Did the service evaluate a precondition and reject it?
///
/// Reads the code first and the status only when no code arrived, which is the
/// rule every provider in this design follows. The 409 test is written **before**
/// the 412 fallback deliberately: a future S3 change that answered
/// `ConditionalRequestConflict` with some other 4xx must still not latch.
[[nodiscard]] inline auto is_precondition_failure(const Aws::S3::S3Error& err) -> bool {
    const auto& code = err.GetExceptionName();
    if (code == k_conditional_conflict) {
        return false;
    }
    if (code == k_precondition_failed) {
        return true;
    }
    return code.empty() && http_status(err) == 412;
}

/// @brief Is this the benign conditional-request race S3 asks the caller to
///        retry?
///
/// Reported as an ordinary exception so the engine's own retry (and only the
/// engine's) can decide. Mapping it to `object_precondition_failed` would
/// convert a transient race into a permanent fence latch that no operator
/// action clears.
[[nodiscard]] inline auto is_conditional_conflict(const Aws::S3::S3Error& err) -> bool {
    return err.GetExceptionName() == k_conditional_conflict;
}

/// @brief Is this "the object does not exist", as opposed to any other 404?
[[nodiscard]] inline auto is_absent_object(const Aws::S3::S3Error& err) -> bool {
    const auto& code = err.GetExceptionName();
    if (code == k_no_such_bucket) {
        return false;
    }
    if (code == k_no_such_key) {
        return true;
    }
    return code.empty() && http_status(err) == 404;
}

/// @brief One sentence carrying everything a reader needs: the operation, the
///        subject, the status **and** the service's own code and message.
[[nodiscard]] inline auto describe(std::string_view op, const std::string& subject,
                                   const Aws::S3::S3Error& err) -> std::string {
    std::string out = "aws_s3_client: ";
    out += op;
    out += ' ';
    out += subject;
    out += " failed with HTTP ";
    out += std::to_string(http_status(err));
    if (!err.GetExceptionName().empty()) {
        out += ": ";
        out += aws_s3_detail::from_aws(err.GetExceptionName());
    }
    if (!err.GetMessage().empty()) {
        out += ": ";
        out += aws_s3_detail::from_aws(err.GetMessage());
    }
    return out;
}

/// Base64 of the raw 16 MD5 bytes — the `Content-MD5` encoding. Distinct from
/// the ETag's hex, and confusing the two is the obvious mistake here.
[[nodiscard]] inline auto content_md5_base64(std::string_view bytes) -> Aws::String {
    const Aws::String payload(bytes.data(), bytes.size());
    return Aws::Utils::HashingUtils::Base64Encode(Aws::Utils::HashingUtils::CalculateMD5(payload));
}

}  // namespace aws_s3_detail

/// @brief The object operations the persistence engine needs, over the AWS SDK.
///
/// Satisfies `key_object_store` and `conditional_key_object_store`; the
/// `static_assert`s live beside the definition rather than in a test, so a
/// signature change that breaks the concept is a compile error in this header.
class aws_s3_client {
public:
    /// @brief S3's ETag is the lowercase MD5 hex of single-part content, so the
    ///        engine may verify every PUT locally (Requirement 7.2). See the
    ///        file comment for the SSE-KMS/SSE-C caveat.
    static constexpr bool version_is_content_md5 = true;

    explicit aws_s3_client(aws_client_config cfg) : _cfg(std::move(cfg)) {
        // IMDS config lookup is disabled whenever a mock or LocalStack endpoint
        // is configured: that target is on the loopback and a metadata probe
        // there can only waste the request timeout.
        Aws::Client::ClientConfigurationInitValues init;
        init.shouldDisableIMDS = !_cfg.endpoint_override.empty();
        Aws::S3::S3ClientConfiguration client_cfg{init};

        if (!_cfg.region.empty()) {
            client_cfg.region = _cfg.region;
        }
        if (!_cfg.endpoint_override.empty()) {
            client_cfg.endpointOverride = _cfg.endpoint_override;
            // Path-style, so a local mock needs no wildcard DNS for
            // `<bucket>.<host>`. Real S3 keeps virtual-host addressing.
            client_cfg.useVirtualAddressing = false;
        }
        const auto ms = static_cast<long>(_cfg.api_timeout.count() * 1000);
        client_cfg.requestTimeoutMs = ms;
        client_cfg.connectTimeoutMs = ms;

        // Requirement 1.5/12.2. `DefaultRetryStrategy(0)` refuses every retry;
        // it is spelled out rather than left to the SDK's default of 10.
        client_cfg.retryStrategy =
            Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(k_allocation_tag, /*maxRetries=*/0L);

        // The SDK's flexible-checksum default (`WHEN_SUPPORTED`) would add a
        // CRC32 of its own choosing and may reframe the body to carry it. This
        // client sends an explicit `Content-MD5` instead — see the file comment.
        client_cfg.checksumConfig.requestChecksumCalculation =
            Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;

        if (_cfg.credentials_provider) {
            _s3 =
                std::make_shared<Aws::S3::S3Client>(_cfg.credentials_provider, nullptr, client_cfg);
        } else {
            _s3 = std::make_shared<Aws::S3::S3Client>(client_cfg);
        }
    }

    /// @brief The name this store answers to in error messages, metrics and a
    ///        backup manifest (`kythira::key_object_store`).
    [[nodiscard]] static auto provider_name() -> std::string_view { return "s3"; }

    /// @brief PUT one object, unconditionally. No retry — the engine owns
    ///        write retries.
    auto put_object(const std::string& bucket, const std::string& key, std::string_view bytes) const
        -> put_result {
        fiu_do_on("raft/aws/s3/put_object",
                  throw std::runtime_error("chaos: raft/aws/s3/put_object " + key););
        return send_put(bucket, key, bytes, std::nullopt);
    }

    /// @brief PUT one object only if `pre` holds, throwing
    ///        `object_precondition_failed` when the service rejects it.
    ///
    /// Issued exactly once. A conditional PUT is not idempotent in the way an
    /// unconditional one is: its precondition is stale by construction if a
    /// previous attempt landed, so a retry here would either be a no-op or a
    /// spurious latch.
    auto put_object_if(const std::string& bucket, const std::string& key, std::string_view bytes,
                       const precondition& pre) const -> put_result {
        fiu_do_on("raft/aws/s3/put_object_if",
                  throw std::runtime_error("chaos: raft/aws/s3/put_object_if " + key););
        return send_put(bucket, key, bytes, pre);
    }

    /// @brief GET one object; `nullopt` when the key is absent (absence is an
    ///        answer, not an error, for a persistence load path).
    [[nodiscard]] auto get_object(const std::string& bucket, const std::string& key) const
        -> std::optional<get_result> {
        fiu_do_on("raft/aws/s3/get_object",
                  throw std::runtime_error("chaos: raft/aws/s3/get_object " + key););
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(aws_s3_detail::to_aws(bucket));
        req.SetKey(aws_s3_detail::to_aws(key));

        auto outcome = _s3->GetObject(req);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            if (aws_s3_detail::is_absent_object(err)) {
                return std::nullopt;
            }
            throw std::runtime_error(aws_s3_detail::describe("GetObject", key, err));
        }
        // `<< rdbuf()` sets failbit on an empty body and copies nothing, which
        // is the correct result for a zero-byte object; the stream's own state
        // is deliberately not consulted.
        std::ostringstream body;
        body << outcome.GetResult().GetBody().rdbuf();
        return get_result{body.str(), aws_s3_detail::from_aws(outcome.GetResult().GetETag())};
    }

    /// @brief DELETE one object. S3 answers 204 for present and absent keys
    ///        alike, which is exactly the idempotence the engine's truncation
    ///        path wants.
    auto delete_object(const std::string& bucket, const std::string& key) const -> void {
        fiu_do_on("raft/aws/s3/delete_object",
                  throw std::runtime_error("chaos: raft/aws/s3/delete_object " + key););
        send_delete(bucket, key, std::nullopt);
    }

    /// @brief DELETE one object only if `pre` holds. A stale `If-Match` is
    ///        refused **and the object survives** — verified live, and the
    ///        negative control Alibaba OSS failed (spike-notes.md Findings 3
    ///        and 10).
    ///
    /// `if_absent` throws `std::invalid_argument`: S3's DeleteObject models
    /// `If-Match` only, so that precondition has no wire spelling and the only
    /// alternative would be to drop it silently.
    auto delete_object_if(const std::string& bucket, const std::string& key,
                          const precondition& pre) const -> void {
        fiu_do_on("raft/aws/s3/delete_object_if",
                  throw std::runtime_error("chaos: raft/aws/s3/delete_object_if " + key););
        send_delete(bucket, key, pre);
    }

    /// @brief Every key under `prefix`, paged through `ListObjectsV2`
    ///        continuation tokens to exhaustion.
    ///
    /// A page that cannot be fetched throws and a truncated page with no
    /// continuation token throws: recovery reads the log from one prefixed
    /// LIST and cannot tell "40 entries" from "60 entries, 40 shown", so a
    /// short listing here is silent data loss.
    [[nodiscard]] auto list_keys(const std::string& bucket, const std::string& prefix) const
        -> std::vector<std::string> {
        fiu_do_on("raft/aws/s3/list_keys",
                  throw std::runtime_error("chaos: raft/aws/s3/list_keys " + prefix););
        std::vector<std::string> keys;
        Aws::String continuation;
        while (true) {
            Aws::S3::Model::ListObjectsV2Request req;
            req.SetBucket(aws_s3_detail::to_aws(bucket));
            req.SetPrefix(aws_s3_detail::to_aws(prefix));
            if (!continuation.empty()) {
                req.SetContinuationToken(continuation);
            }

            auto outcome = _s3->ListObjectsV2(req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error(
                    aws_s3_detail::describe("ListObjectsV2", prefix, outcome.GetError()));
            }
            const auto& result = outcome.GetResult();
            for (const auto& object : result.GetContents()) {
                keys.push_back(aws_s3_detail::from_aws(object.GetKey()));
            }
            if (!result.GetIsTruncated()) {
                return keys;
            }
            // This guard stops two different failures, and the second is the
            // one that is easy to miss: without it an empty token is carried
            // back into the next iteration, which re-issues the *identical*
            // request and gets the identical truncated answer — an infinite
            // loop holding the engine's mutex, not merely a short listing.
            // (Found by deleting the guard: the mutant did not fail the suite,
            // it hung it.)
            if (result.GetNextContinuationToken().empty()) {
                throw std::runtime_error(
                    "aws_s3_client: ListObjectsV2 " + prefix +
                    " reported a truncated listing with no continuation token — refusing to"
                    " treat a structurally unexpected listing as complete");
            }
            continuation = result.GetNextContinuationToken();
        }
    }

private:
    [[nodiscard]] auto send_put(const std::string& bucket, const std::string& key,
                                std::string_view bytes,
                                const std::optional<precondition>& pre) const -> put_result {
        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(aws_s3_detail::to_aws(bucket));
        req.SetKey(aws_s3_detail::to_aws(key));
        req.SetContentType(k_content_type);
        req.SetContentMD5(aws_s3_detail::content_md5_base64(bytes));

        auto body = Aws::MakeShared<Aws::StringStream>(k_allocation_tag);
        body->write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        req.SetBody(body);

        object_version expected;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        req.SetIfNoneMatch("*");
                    } else {
                        req.SetIfMatch(aws_s3_detail::to_aws(p.expected));
                        expected = p.expected;
                    }
                },
                *pre);
        }

        auto outcome = _s3->PutObject(req);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            if (pre && aws_s3_detail::is_precondition_failure(err)) {
                throw object_precondition_failed(std::string(provider_name()), key, expected,
                                                 aws_s3_detail::describe("PutObject", key, err));
            }
            throw std::runtime_error(aws_s3_detail::describe("PutObject", key, err));
        }
        return put_result{aws_s3_detail::from_aws(outcome.GetResult().GetETag())};
    }

    auto send_delete(const std::string& bucket, const std::string& key,
                     const std::optional<precondition>& pre) const -> void {
        Aws::S3::Model::DeleteObjectRequest req;
        req.SetBucket(aws_s3_detail::to_aws(bucket));
        req.SetKey(aws_s3_detail::to_aws(key));

        object_version expected;
        if (pre) {
            std::visit(
                [&](const auto& p) {
                    using T = std::decay_t<decltype(p)>;
                    if constexpr (std::is_same_v<T, if_absent>) {
                        // S3's DeleteObject models `If-Match` and **not**
                        // `If-None-Match` — "delete only if it does not exist"
                        // is not expressible on the wire. Refusing loudly is
                        // the whole doctrine of Requirement 9.8 applied one
                        // level down: the alternative is to drop the
                        // precondition and issue an unconditional DELETE,
                        // which is a caller believing in a guard that is not
                        // there. The engine never issues this combination —
                        // its DELETEs are unconditional by design — so this is
                        // a guard against a future caller, not a live path.
                        throw std::invalid_argument(
                            "aws_s3_client: DeleteObject cannot express a create-only"
                            " precondition — S3 models If-Match on DeleteObject and not"
                            " If-None-Match, and issuing the DELETE unconditionally instead"
                            " would silently drop the guard (key: " +
                            key + ")");
                    } else {
                        req.SetIfMatch(aws_s3_detail::to_aws(p.expected));
                        expected = p.expected;
                    }
                },
                *pre);
        }

        auto outcome = _s3->DeleteObject(req);
        if (!outcome.IsSuccess()) {
            const auto& err = outcome.GetError();
            if (pre && aws_s3_detail::is_precondition_failure(err)) {
                throw object_precondition_failed(std::string(provider_name()), key, expected,
                                                 aws_s3_detail::describe("DeleteObject", key, err));
            }
            throw std::runtime_error(aws_s3_detail::describe("DeleteObject", key, err));
        }
    }

    /// Every PUT's content type. Fixed: these objects are opaque serialized
    /// Raft state, never a negotiated media type.
    static constexpr const char* k_content_type = "application/octet-stream";

    /// The SDK's allocation tag for everything this client allocates through
    /// `Aws::MakeShared`.
    static constexpr const char* k_allocation_tag = "aws_s3_client";

    aws_client_config _cfg;
    std::shared_ptr<Aws::S3::S3Client> _s3;
};

static_assert(key_object_store<aws_s3_client>, "aws_s3_client must satisfy key_object_store");
static_assert(
    conditional_key_object_store<aws_s3_client>,
    "aws_s3_client must satisfy conditional_key_object_store — S3 expresses every "
    "conditional primitive (spike-notes.md Finding 10), so fencing_mode::compare_and_swap "
    "must remain available for it");
static_assert(content_md5_versioned_store<aws_s3_client>,
              "aws_s3_client's ETag is the content MD5 for single-part uploads");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK
