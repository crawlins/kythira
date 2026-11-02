// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file multi_version_state_machine.hpp
/// @brief The `multi_version_state_machine` concept of
///        `.kiro/specs/multi-raft-transactions/` tier 2 (Requirement 8.1).
///
/// A state machine that keeps, per key, the values committed at each
/// timestamp, so a snapshot read at timestamp `ts` sees exactly the writes
/// committed at or below `ts`. `transactional_state_machine` under
/// `mvcc::on` (task 12) wraps one; every write then carries the commit
/// timestamp the oracle gave its transaction.

#include <raft/types.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <vector>

namespace kythira {

/// @brief Requirement 8.1.
///
/// - `apply_at(command, commit_ts)` applies a command as committed at
///   `commit_ts`. It is a pure function of its arguments, as `apply` is.
/// - `read_at(key, ts)` is the value the newest version at or below `ts`
///   holds, or nothing if that version is a deletion or there is none.
/// - `latest_commit_ts(key)` is the timestamp of the key's newest version,
///   deletions included: what first-committer-wins compares a transaction's
///   start timestamp against (Requirement 8.3).
/// - `collect_garbage(safe_point)` drops every version older than the newest
///   one at or below `safe_point`, so reads at or above it are unchanged
///   (Requirement 8.6).
template<typename SM, typename Key>
concept multi_version_state_machine =
    requires(SM& sm, const SM& csm, const std::vector<std::byte>& command, const Key& key,
             std::uint64_t ts) {
        { sm.apply_at(command, ts) } -> std::same_as<std::vector<std::byte>>;
        { csm.read_at(key, ts) };
        { csm.latest_commit_ts(key) } -> std::same_as<std::optional<std::uint64_t>>;
        { sm.collect_garbage(ts) } -> std::same_as<std::size_t>;
    };

}  // namespace kythira
