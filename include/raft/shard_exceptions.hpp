// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file shard_exceptions.hpp
/// @brief Typed errors for the multi-Raft sharding layer.
///
/// Every one of these derives from `shard_exception`, which derives from the
/// existing `raft_exception` hierarchy, so a caller that wants to treat the
/// whole layer uniformly can, and a caller that wants to act on one specific
/// failure — retry after repairing a routing cache, back off through a merge,
/// give up on a shard the state machine refuses to cut — can do that instead.
///
/// The concrete types are templates because they carry a group id, and the
/// group id type is a template parameter of the whole layer. `shard_exception`
/// exists as a non-template base precisely so that a `catch` site which does
/// not know the instantiation still has something to name.
///
/// See `.kiro/specs/multi-raft/design.md` §5.5 and §8.

#include <raft/exceptions.hpp>
#include <raft/shard_types.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kythira {

/// @brief Base for every error raised by the sharding layer.
class shard_exception : public raft_exception {
public:
    explicit shard_exception(const std::string& message) : raft_exception(message) {}
};

/// @brief A request named an epoch that is no longer current for its range.
///
/// This is TiKV's `EpochNotMatch`, with one refinement: the rejection carries
/// the *current descriptors for the range that was targeted*, so a client
/// repairs its routing map from the rejection itself rather than making a
/// round trip to the placement driver. That is the difference between a split
/// costing every in-flight client one extra hop and costing them a
/// control-plane query each.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Must satisfy `node_id`.
template<raft_group_id GroupId, shard_key Key, typename NodeId = std::uint64_t>
requires node_id<NodeId>
class shard_epoch_mismatch_exception : public shard_exception {
public:
    using descriptor_type = shard_descriptor<GroupId, Key, NodeId>;

    shard_epoch_mismatch_exception(GroupId group, shard_epoch requested, shard_epoch current,
                                   std::vector<descriptor_type> current_descriptors)
        : shard_exception(
              "shard " + detail::describe_value(group) + ": epoch mismatch, request " +
              describe_epoch(requested) + " but shard is at " + describe_epoch(current) + " (" +
              std::to_string(current_descriptors.size()) + " current descriptor(s) attached)"),
          _group_id(std::move(group)),
          _requested_epoch(requested),
          _current_epoch(current),
          _current_descriptors(std::move(current_descriptors)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto requested_epoch() const -> shard_epoch { return _requested_epoch; }
    [[nodiscard]] auto current_epoch() const -> shard_epoch { return _current_epoch; }

    /// @brief The descriptors now covering the targeted range.
    ///
    /// Feed these straight into `shard_map::upsert_all()` and retry.
    [[nodiscard]] auto current_descriptors() const -> const std::vector<descriptor_type>& {
        return _current_descriptors;
    }

private:
    [[nodiscard]] static auto describe_epoch(const shard_epoch& e) -> std::string {
        return "{v=" + std::to_string(e._version) + ",cv=" + std::to_string(e._conf_version) + "}";
    }

    GroupId _group_id;
    shard_epoch _requested_epoch;
    shard_epoch _current_epoch;
    std::vector<descriptor_type> _current_descriptors;
};

/// @brief A request named an epoch *newer* than this replica knows about.
///
/// Distinct from a mismatch on purpose. A mismatch means the caller is behind
/// and the attached descriptors repair it; being ahead means *this replica* is
/// behind, and there is nothing to attach — the right response is to wait for
/// replication rather than to hand the caller a routing table that is older
/// than the one it already has.
template<raft_group_id GroupId> class shard_epoch_ahead_exception : public shard_exception {
public:
    shard_epoch_ahead_exception(GroupId group, shard_epoch requested, shard_epoch local)
        : shard_exception("shard " + detail::describe_value(group) +
                          ": request epoch {v=" + std::to_string(requested._version) +
                          ",cv=" + std::to_string(requested._conf_version) +
                          "} is ahead of this replica's {v=" + std::to_string(local._version) +
                          ",cv=" + std::to_string(local._conf_version) + "}"),
          _group_id(std::move(group)),
          _requested_epoch(requested),
          _local_epoch(local) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto requested_epoch() const -> shard_epoch { return _requested_epoch; }
    [[nodiscard]] auto local_epoch() const -> shard_epoch { return _local_epoch; }

private:
    GroupId _group_id;
    shard_epoch _requested_epoch;
    shard_epoch _local_epoch;
};

/// @brief A merge was asked for between two shards that are not adjacent.
template<raft_group_id GroupId, shard_key Key>
class shard_not_adjacent_exception : public shard_exception {
public:
    shard_not_adjacent_exception(GroupId source, GroupId target, shard_range<Key> source_range,
                                 shard_range<Key> target_range)
        : shard_exception("merge " + detail::describe_value(source) + " " +
                          detail::describe_range(source_range) + " into " +
                          detail::describe_value(target) + " " +
                          detail::describe_range(target_range) + ": ranges are not adjacent"),
          _source_group_id(std::move(source)),
          _target_group_id(std::move(target)),
          _source_range(std::move(source_range)),
          _target_range(std::move(target_range)) {}

    [[nodiscard]] auto source_group_id() const -> const GroupId& { return _source_group_id; }
    [[nodiscard]] auto target_group_id() const -> const GroupId& { return _target_group_id; }
    [[nodiscard]] auto source_range() const -> const shard_range<Key>& { return _source_range; }
    [[nodiscard]] auto target_range() const -> const shard_range<Key>& { return _target_range; }

private:
    GroupId _source_group_id;
    GroupId _target_group_id;
    shard_range<Key> _source_range;
    shard_range<Key> _target_range;
};

/// @brief The shard is already in an operation, or in a state that admits none.
///
/// Carries the operation-state name rather than an enum so that this header
/// does not have to depend on the arbiter's state type — the arbiter is a much
/// later phase and depending on it here would invert the layering.
template<raft_group_id GroupId> class shard_busy_exception : public shard_exception {
public:
    shard_busy_exception(GroupId group, std::string state)
        : shard_exception("shard " + detail::describe_value(group) + ": busy in state '" + state +
                          "'"),
          _group_id(std::move(group)),
          _state(std::move(state)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto state() const -> const std::string& { return _state; }

private:
    GroupId _group_id;
    std::string _state;
};

/// @brief A merge was asked for between shards whose replicas are not colocated.
///
/// Each target replica absorbs state from the source replica *on its own
/// machine*, so a target replica with no local source peer cannot apply
/// `merge_commit`. This fails fast rather than shipping state across the
/// network mid-merge; `merge_options::_auto_align` is the opt-in that asks the
/// placement driver to colocate first.
template<raft_group_id GroupId, typename NodeId = std::uint64_t>
requires node_id<NodeId>
class shard_alignment_required_exception : public shard_exception {
public:
    shard_alignment_required_exception(GroupId source, GroupId target,
                                       std::vector<NodeId> source_members,
                                       std::vector<NodeId> target_members)
        : shard_exception("merge " + detail::describe_value(source) + " into " +
                          detail::describe_value(target) + ": replica sets are not colocated (" +
                          std::to_string(source_members.size()) + " source member(s), " +
                          std::to_string(target_members.size()) +
                          " target member(s)); align them first or pass "
                          "merge_options::_auto_align"),
          _source_group_id(std::move(source)),
          _target_group_id(std::move(target)),
          _source_members(std::move(source_members)),
          _target_members(std::move(target_members)) {}

    [[nodiscard]] auto source_group_id() const -> const GroupId& { return _source_group_id; }
    [[nodiscard]] auto target_group_id() const -> const GroupId& { return _target_group_id; }
    [[nodiscard]] auto source_members() const -> const std::vector<NodeId>& {
        return _source_members;
    }
    [[nodiscard]] auto target_members() const -> const std::vector<NodeId>& {
        return _target_members;
    }

private:
    GroupId _source_group_id;
    GroupId _target_group_id;
    std::vector<NodeId> _source_members;
    std::vector<NodeId> _target_members;
};

/// @brief A split key was named that does not lie inside the shard's range.
template<raft_group_id GroupId, shard_key Key>
class split_key_out_of_range_exception : public shard_exception {
public:
    split_key_out_of_range_exception(GroupId group, Key key, shard_range<Key> range)
        : shard_exception("shard " + detail::describe_value(group) + " " +
                          detail::describe_range(range) + ": split key " +
                          detail::describe_value(key) + " is outside the range"),
          _group_id(std::move(group)),
          _key(std::move(key)),
          _range(std::move(range)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto key() const -> const Key& { return _key; }
    [[nodiscard]] auto range() const -> const shard_range<Key>& { return _range; }

private:
    GroupId _group_id;
    Key _key;
    shard_range<Key> _range;
};

/// @brief Every candidate split key was vetoed and the state machine suggested none.
///
/// A state machine that vetoes everything gets a visible complaint rather than
/// silence — the split is not merely skipped, it fails with the count of keys
/// that were considered.
template<raft_group_id GroupId> class no_valid_split_key_exception : public shard_exception {
public:
    no_valid_split_key_exception(GroupId group, std::size_t candidates_considered)
        : shard_exception("shard " + detail::describe_value(group) + ": no valid split key — all " +
                          std::to_string(candidates_considered) +
                          " candidate(s) were vetoed by the state machine and it suggested none"),
          _group_id(std::move(group)),
          _candidates_considered(candidates_considered) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto candidates_considered() const -> std::size_t {
        return _candidates_considered;
    }

private:
    GroupId _group_id;
    std::size_t _candidates_considered;
};

/// @brief The shard has applied `merge_prepare` and is frozen for the merge.
///
/// Proposals and reads are rejected until the merge commits (the range moves
/// to the survivor) or rolls back (the source resumes). A client should back
/// off and retry; the retry will either succeed against the source again or
/// be redirected by an epoch mismatch.
template<raft_group_id GroupId> class shard_merging_exception : public shard_exception {
public:
    shard_merging_exception(GroupId group, GroupId target)
        : shard_exception("shard " + detail::describe_value(group) + ": frozen for merge into " +
                          detail::describe_value(target) + "; retry after the merge resolves"),
          _group_id(std::move(group)),
          _target_group_id(std::move(target)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto target_group_id() const -> const GroupId& { return _target_group_id; }

private:
    GroupId _group_id;
    GroupId _target_group_id;
};

/// @brief The local replica of the resolved shard is not its leader.
///
/// Carries the leader hint, taking MicroRaft's contract directly — "when
/// clients contact non-leaders, responses include the leader's endpoint,
/// enabling client-side routing" — so a leader change costs one extra hop
/// rather than a control-plane round trip.
///
/// The hint is `nullopt` while the group has not yet heard from any leader,
/// which is a real state (an election in progress) and not an error: a caller
/// with no hint should retry any voter rather than give up.
template<raft_group_id GroupId, typename NodeId = std::uint64_t>
requires node_id<NodeId>
class shard_not_leader_exception : public shard_exception {
public:
    shard_not_leader_exception(GroupId group, std::optional<NodeId> leader_hint)
        : shard_exception("shard " + detail::describe_value(group) +
                          ": this replica is not the leader" +
                          (leader_hint.has_value() ? "; try " + detail::describe_value(*leader_hint)
                                                   : "; no leader is known yet")),
          _group_id(std::move(group)),
          _leader_hint(std::move(leader_hint)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto leader_hint() const -> const std::optional<NodeId>& { return _leader_hint; }

private:
    GroupId _group_id;
    std::optional<NodeId> _leader_hint;
};

/// @brief A command's routing key falls outside the shard it was admitted to.
///
/// There is no distributed transaction in this design, and pretending
/// otherwise would be the worst possible failure mode: a command silently
/// applied to a shard that does not own its key produces a state no invariant
/// in the system catches. Rejecting is the only honest answer.
template<raft_group_id GroupId, shard_key Key>
class cross_shard_command_exception : public shard_exception {
public:
    cross_shard_command_exception(GroupId group, Key key, shard_range<Key> range)
        : shard_exception("shard " + detail::describe_value(group) + " " +
                          detail::describe_range(range) + ": command key " +
                          detail::describe_value(key) +
                          " belongs to another shard; cross-shard commands are not supported"),
          _group_id(std::move(group)),
          _key(std::move(key)),
          _range(std::move(range)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto key() const -> const Key& { return _key; }
    [[nodiscard]] auto range() const -> const shard_range<Key>& { return _range; }

private:
    GroupId _group_id;
    Key _key;
    shard_range<Key> _range;
};

/// @brief No shard is known to own the key, or no local replica exists for the group.
template<raft_group_id GroupId> class unknown_shard_exception : public shard_exception {
public:
    explicit unknown_shard_exception(GroupId group)
        : shard_exception("shard " + detail::describe_value(group) +
                          ": no local replica and no routing row"),
          _group_id(std::move(group)) {}

    unknown_shard_exception(GroupId group, std::string detail_text)
        : shard_exception("shard " + detail::describe_value(group) + ": " + detail_text),
          _group_id(std::move(group)) {}

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }

private:
    GroupId _group_id;
};

/// @brief No shard in the routing map owns the given key.
///
/// Separate from `unknown_shard_exception` because the caller has a key, not a
/// group id, and the repair is a routing-map refresh rather than a replica
/// lookup.
template<shard_key Key> class unrouted_key_exception : public shard_exception {
public:
    explicit unrouted_key_exception(Key key)
        : shard_exception("no shard owns key " + detail::describe_value(key) +
                          "; routing map has a gap there"),
          _key(std::move(key)) {}

    [[nodiscard]] auto key() const -> const Key& { return _key; }

private:
    Key _key;
};

}  // namespace kythira
