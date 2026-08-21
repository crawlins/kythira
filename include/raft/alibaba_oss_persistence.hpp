// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_oss_persistence.hpp
/// @brief Raft persistent state in an Alibaba Cloud OSS bucket — the generic
///        object-store engine, instantiated over `alibaba_oss_client`.
///
/// **The engine's body, its documentation and its correctness argument now live
/// in `object_store_persistence.hpp`**: the object layout and its 20-digit
/// padding rationale, the durability contract and its fsync-equivalence
/// argument, the one-object-per-log-entry rationale, the in-memory mirror, the
/// mirror-after-acknowledgement ordering, the single idempotent-PUT retry, and
/// the corruption-is-fatal contrast with `file_persistence_engine`. Read that
/// header first; this one holds only what is specific to OSS.
///
/// This engine was live-verified end to end against a real `ap-southeast-1`
/// bucket on August 14, 2026 — all four real cases, including a fresh engine
/// reading back another engine's writes. That evidence is now evidence about
/// the *generic* engine over the OSS client, which is why the hoist is only
/// complete once the real suite has been re-run live against it
/// (`.kiro/specs/cloud-object-persistence/` task 19).
///
/// ## What is OSS-specific
///
/// * **Construction takes `{alibaba_client_config, bucket, prefix}`**, not a
///   store — an operator configures credentials, region and endpoint, and the
///   OSS client is built from them here. This is the constructor that shipped,
///   preserved deliberately: it is the whole surface every existing caller and
///   test uses.
/// * **The `raft/alibaba/oss/*` fault points live on `alibaba_oss_client`**
///   (`put_object`, `get_object`, `delete_object`, `list_keys`), so chaos
///   configurations written against this engine keep working. The generic
///   engine additionally carries provider-independent
///   `raft/objstore/{put,get,delete,list}_object` fault points at the store
///   boundary; a fault enabled on either name fails the operation, the OSS
///   names firing inside the client and the generic ones just outside it.
/// * **Single-writer, and on OSS it cannot be otherwise.** Exactly one process
///   may own a `{bucket, prefix}` pair. This engine performs no fencing, and
///   **OSS cannot support the compare-and-swap fencing mode at all**: measured
///   live on August 16, 2026, `If-Match` on PutObject is rejected
///   `400 NotImplemented` for a *current* ETag as well as a stale one, so
///   there is no ETag-predicated write to build a fence on
///   (`.kiro/specs/cloud-object-persistence/spike-notes.md` Finding 1). Under
///   Requirement 9.8 that makes `alibaba_oss_client` a `key_object_store` and
///   **not** a `conditional_key_object_store`, so `compare_and_swap` will be a
///   compile error here rather than a silent no-op.
///
///   What OSS does have is a create-only header, `x-oss-forbid-overwrite`,
///   which rejects with **409 `FileAlreadyExists`** — note 409 and not 412,
///   and note that S3 uses 409 for the opposite meaning (a benign race to
///   retry), so no client may map a bare 409 without reading the error code.
///   Its conditional *delete* is worse than absent: a stale `If-Match` returns
///   204 and deletes the object anyway.

#include <raft/alibaba_client_config.hpp>
#include <raft/alibaba_oss_client.hpp>
#include <raft/object_store_persistence.hpp>
#include <raft/persistence.hpp>
#include <raft/types.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace kythira {

/// @brief Raft persistent state stored as individual OSS objects.
///
/// A thin derived type rather than an alias: an alias could not preserve the
/// shipped `{alibaba_client_config, bucket, prefix}` constructor, and that
/// constructor is the engine's entire public surface.
///
/// @tparam NodeId   Node identifier type; defaults to `std::uint64_t`.
/// @tparam TermId   Term number type; defaults to `std::uint64_t`.
/// @tparam LogIndex Log index type; defaults to `std::uint64_t`.
template<typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class alibaba_oss_persistence_engine
    : public object_store_persistence_engine<alibaba_oss_client, NodeId, TermId, LogIndex> {
public:
    using base = object_store_persistence_engine<alibaba_oss_client, NodeId, TermId, LogIndex>;
    using log_entry_t = typename base::log_entry_t;
    using snapshot_t = typename base::snapshot_t;

    /// @brief Construct over `{bucket, prefix}` in the configured account and
    ///        load the mirror.
    ///
    /// @throws std::invalid_argument if a required field is empty.
    /// @throws std::runtime_error    if the listing or any GET fails, or if any
    ///                               object under the prefix fails to parse —
    ///                               the message names the offending key.
    ///
    /// An empty prefix is the normal cold-start case: the standard empty state
    /// (term 0, no vote, empty log, no snapshot) is established and **no object
    /// is written** until the first mutation.
    alibaba_oss_persistence_engine(alibaba_client_config cfg, std::string bucket,
                                   std::string prefix, object_persistence_options opts = {})
        : base(alibaba_oss_client(std::move(cfg)), std::move(bucket), std::move(prefix), opts) {}

    alibaba_oss_persistence_engine(alibaba_oss_persistence_engine&&) noexcept = default;
    auto operator=(alibaba_oss_persistence_engine&&) -> alibaba_oss_persistence_engine& = delete;
    alibaba_oss_persistence_engine(const alibaba_oss_persistence_engine&) = delete;
    auto operator=(const alibaba_oss_persistence_engine&)
        -> alibaba_oss_persistence_engine& = delete;
    ~alibaba_oss_persistence_engine() = default;
};

/// The client satisfies the store concept — checked here rather than in the
/// client header so `key_object_store.hpp` stays the only thing the client
/// depends on from this spec.
static_assert(key_object_store<alibaba_oss_client>,
              "alibaba_oss_client must satisfy the key_object_store concept");

/// …and, deliberately, **not** the conditional refinement. OSS has no overwrite
/// compare-and-swap at all: `If-Match` on PutObject is rejected
/// `400 NotImplemented` for a *current* ETag as well as a stale one
/// (`.kiro/specs/cloud-object-persistence/spike-notes.md` Finding 1, verified
/// live August 16, 2026), so there is no ETag-predicated write to build a fence
/// on. Requirement 9.8 therefore fires here rather than anywhere else:
/// `fencing_mode::compare_and_swap` is a **compile error** for this engine, not a
/// runtime degradation to unconditional writes. This assertion is the guard on
/// that — adding a `put_object_if` to the client that did not genuinely fence
/// would fail here, where it is a decision, rather than in production, where it
/// is a corrupted log.
static_assert(!conditional_key_object_store<alibaba_oss_client>,
              "alibaba_oss_client must NOT satisfy conditional_key_object_store — OSS has no "
              "overwrite compare-and-swap (spike-notes.md Finding 1)");

/// The concept is checked at file scope, so a signature drift is a compile
/// error in every translation unit that includes this header — not just in
/// whichever test happens to instantiate it.
static_assert(persistence_engine<alibaba_oss_persistence_engine<>, std::uint64_t, std::uint64_t,
                                 std::uint64_t, log_entry<>, snapshot<>>,
              "alibaba_oss_persistence_engine must satisfy the persistence_engine concept");

}  // namespace kythira
