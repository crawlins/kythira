// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file shard_placement_driver.hpp
/// @brief The cluster-scope authority over shard placement: reports, operators,
///        the `shard_placement_driver` concept, and the no-op default.
///
/// This is the shard-aware extension of `quorum_manager`, which already models
/// a cluster-scope authority (`topology()`, `provision_node()`,
/// `assess_quorum()`) but knows nothing about ranges. A shard's leader can see
/// that its own shard is large; only something that sees every shard on every
/// machine can decide that the *cluster* is unbalanced, and that is the whole
/// reason this layer exists.
///
/// ### Operators are advisory
///
/// The single most important property here, and the one that shapes every
/// signature: an operator is a **suggestion**. TiKV states this outright —
/// "operators are only suggestions to the Region leader, which can be skipped"
/// — and it is what makes the design tolerate a driver that is out of date,
/// partitioned, or simply wrong. A leader whose preconditions no longer hold
/// drops the operator and logs it; the driver notices on the next heartbeat and
/// reissues. Nothing has to be undone, because nothing was promised.
///
/// The alternative — a driver that commands — would need the driver to be
/// consistent with every shard's Raft log, which is precisely the problem Raft
/// exists to solve and would put a second consensus system in the path of the
/// first.
///
/// ### Batching is not an optimisation
///
/// `report_shard_heartbeat` takes a *vector* of reports, one call per interval
/// for the whole process. At 1000 shards and a 10-second interval, per-shard
/// calls would be 100 RPS of control-plane traffic from one machine, against
/// 0.1 RPS batched. A system whose control-plane load grows with shard count
/// stops working at exactly the scale sharding was adopted for.

#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>
#include <raft/shard_types.hpp>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

namespace kythira {

// ─────────────────────────────────────────────────────────────────────────────
// Identity allocation
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One cluster-unique group id, optionally with a suggested placement.
///
/// The id is the part that cannot be invented locally. Two partitions that each
/// mint "group 7" produce two shards with the same identity and no way to tell
/// them apart afterwards, so allocation is a cluster-scope operation even in a
/// deployment that never rebalances. TiKV makes the same call (`AskBatchSplit`)
/// for the same reason.
///
/// `_suggested_voters` is advisory in the ordinary sense of this file: empty
/// means "inherit the parent's replica set", which is what a split does unless
/// the driver has a reason to place the child elsewhere.
template<raft_group_id GroupId, typename NodeId> struct shard_id_allocation {
    GroupId _group_id{};
    std::vector<NodeId> _suggested_voters{};
    std::vector<NodeId> _suggested_learners{};

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto suggested_voters() const -> const std::vector<NodeId>& {
        return _suggested_voters;
    }

    [[nodiscard]] auto operator==(const shard_id_allocation&) const -> bool = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// Reports
// ─────────────────────────────────────────────────────────────────────────────

/// @brief What one shard's leader tells the driver, per interval.
///
/// Mirrors TiKV's region heartbeat field for field wherever Kythira can measure
/// the field. Sent by the **leader only**: a follower's view of size and load is
/// the leader's view delayed, and N-1 copies of a stale report cost bandwidth to
/// tell the driver nothing.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Node identifier type.
template<raft_group_id GroupId, shard_key Key, typename NodeId> struct shard_report {
    /// The leader's authoritative descriptor: id, range, epoch, membership.
    shard_descriptor<GroupId, Key, NodeId> _descriptor{};
    /// Who is reporting. Redundant with `_descriptor._leader_hint` on purpose:
    /// the hint is a cache that may be stale, this is a fact about the sender.
    NodeId _leader{};

    /// Replicas the leader believes are down — TiKV's `down_peers`. This is the
    /// input to replacement, and it is deliberately the *leader's belief*
    /// rather than a cluster fact: the leader is the only party with per-peer
    /// progress, and a driver that waited for certainty would never act.
    std::size_t _down_replica_count{0};
    std::vector<NodeId> _down_replicas{};
    /// Replicas that exist but have not yet caught up — TiKV's `pending_peers`.
    /// A driver that ignores these will keep adding replicas to a shard that is
    /// already busy catching three of them up.
    std::vector<NodeId> _pending_replicas{};

    std::size_t _approximate_size_bytes{0};
    std::size_t _approximate_key_count{0};
    /// @brief `false` when the state machine has no sizing hooks.
    ///
    /// Kept distinct from "zero", exactly as in `shard_stats`: a driver that
    /// read an absent measurement as an empty shard would merge away live data.
    bool _size_available{false};

    double _read_qps{0.0};
    double _write_qps{0.0};
    double _read_bytes_per_sec{0.0};
    double _write_bytes_per_sec{0.0};

    /// What the shard is doing right now. A driver that knows a shard is
    /// mid-merge can decline to compute an operator that would only be skipped.
    shard_operation_state _operation{shard_operation_state::stable};

    /// The term the report was produced in, so a driver can discard a report
    /// from a leader that has since been deposed.
    std::uint64_t _term{0};

    [[nodiscard]] auto group_id() const -> const GroupId& { return _descriptor._group_id; }
    [[nodiscard]] auto epoch() const -> const shard_epoch& { return _descriptor._epoch; }
    [[nodiscard]] auto descriptor() const -> const shard_descriptor<GroupId, Key, NodeId>& {
        return _descriptor;
    }
    [[nodiscard]] auto leader() const -> const NodeId& { return _leader; }
};

/// @brief What one machine tells the driver about itself, per interval.
///
/// Mirrors TiKV's store heartbeat. One per process, not one per shard: these
/// are properties of the machine, and repeating them per shard would make the
/// report size quadratic in the thing being reported on.
///
/// @tparam NodeId           Node identifier type.
/// @tparam PlacementGroupId Failure-domain label type; the existing
///                          `placement_group_id` from `quorum_management.hpp`,
///                          reused rather than reinvented so that a cluster's
///                          rack and zone vocabulary is the same one the quorum
///                          manager already provisions against.
template<typename NodeId, typename PlacementGroupId = std::string>
requires placement_group_id<PlacementGroupId>
struct node_report {
    NodeId _node_id{};

    std::uint64_t _capacity_bytes{0};
    std::uint64_t _available_bytes{0};
    std::uint64_t _used_bytes{0};

    std::size_t _shard_count{0};
    std::size_t _leader_count{0};

    double _read_qps{0.0};
    double _write_qps{0.0};
    double _read_bytes_per_sec{0.0};
    double _write_bytes_per_sec{0.0};

    std::size_t _sending_snapshot_count{0};
    std::size_t _receiving_snapshot_count{0};
    std::size_t _applying_snapshot_count{0};

    /// @brief The machine is asking not to be given more work.
    ///
    /// A single flag rather than a set of thresholds, because the machine is
    /// the only party that knows why it is unhappy and the driver only needs to
    /// know that it is. TiKV's `is_busy`.
    bool _overloaded{false};

    std::vector<PlacementGroupId> _labels{};
    std::chrono::milliseconds _uptime{};

    [[nodiscard]] auto node_id() const -> const NodeId& { return _node_id; }
    [[nodiscard]] auto overloaded() const -> bool { return _overloaded; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Operators
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Add a replica of the shard on `_node`.
///
/// `_as_learner` defaults true: Raft's own membership-change safety argument
/// prefers catching a new member up as a non-voter first, so that adding it
/// cannot enlarge the quorum before it can answer.
template<typename NodeId> struct add_replica_operator {
    NodeId _node{};
    bool _as_learner{true};
    [[nodiscard]] auto operator==(const add_replica_operator&) const -> bool = default;
};

template<typename NodeId> struct remove_replica_operator {
    NodeId _node{};
    [[nodiscard]] auto operator==(const remove_replica_operator&) const -> bool = default;
};

/// @brief Move leadership of the shard to `_to`.
///
/// The cheapest rebalance there is: no data moves, and the target already holds
/// the log. It is how a driver evens out leader count across machines, and it
/// is what `scatter` is built from.
template<typename NodeId> struct transfer_leader_operator {
    NodeId _to{};
    [[nodiscard]] auto operator==(const transfer_leader_operator&) const -> bool = default;
};

/// @brief Split the shard, optionally at named keys.
///
/// Empty `_at_keys` means "you choose", the same convention `split_shard` uses,
/// so a driver that knows a shard is too big without knowing its key
/// distribution can still say so.
template<shard_key Key> struct split_operator {
    std::vector<Key> _at_keys{};
    [[nodiscard]] auto operator==(const split_operator&) const -> bool = default;
};

template<raft_group_id GroupId> struct merge_operator {
    GroupId _into{};
    [[nodiscard]] auto operator==(const merge_operator&) const -> bool = default;
};

/// @brief Spread this shard's leadership away from wherever it is concentrated.
///
/// Carries no target: the point of scatter is that the *host* picks, from the
/// replica set it can see and the leader counts it already tracks. A driver
/// that wanted a specific placement would send `transfer_leader`.
struct scatter_operator {
    [[nodiscard]] auto operator==(const scatter_operator&) const -> bool = default;
};

/// @brief The operator variant of design §7 / Requirement 14.4.
template<raft_group_id GroupId, shard_key Key, typename NodeId>
using shard_operator_kind =
    std::variant<add_replica_operator<NodeId>, remove_replica_operator<NodeId>,
                 transfer_leader_operator<NodeId>, split_operator<Key>, merge_operator<GroupId>,
                 scatter_operator>;

/// @brief One operator, addressed to one shard, computed against one epoch.
///
/// The epoch is what makes an advisory operator safe rather than merely
/// tolerable. A driver computes "split group 7" from a heartbeat; by the time
/// the operator arrives, group 7 may have already split, and the range the
/// driver reasoned about no longer exists. Comparing the carried epoch against
/// the shard's current one turns that from a wrong action into a discarded
/// message (Requirement 14.6).
///
/// `_operation_id` exists so that the discard is *reportable*: an operator that
/// vanishes silently is indistinguishable from one that was never sent, and an
/// operator debugging a driver that "does nothing" needs to tell those apart.
template<raft_group_id GroupId, shard_key Key, typename NodeId> struct shard_operation {
    GroupId _group_id{};
    std::uint64_t _operation_id{0};
    /// The epoch the driver computed this against.
    shard_epoch _epoch{};
    shard_operator_kind<GroupId, Key, NodeId> _operator{};

    [[nodiscard]] auto group_id() const -> const GroupId& { return _group_id; }
    [[nodiscard]] auto operation_id() const -> std::uint64_t { return _operation_id; }
    [[nodiscard]] auto epoch() const -> const shard_epoch& { return _epoch; }
    [[nodiscard]] auto kind() const -> const shard_operator_kind<GroupId, Key, NodeId>& {
        return _operator;
    }

    /// @brief A stable name for logs and metric dimensions.
    [[nodiscard]] auto name() const -> const char* {
        return std::visit(
            []<typename Op>(const Op&) -> const char* {
                if constexpr (std::same_as<Op, add_replica_operator<NodeId>>) {
                    return "add_replica";
                } else if constexpr (std::same_as<Op, remove_replica_operator<NodeId>>) {
                    return "remove_replica";
                } else if constexpr (std::same_as<Op, transfer_leader_operator<NodeId>>) {
                    return "transfer_leader";
                } else if constexpr (std::same_as<Op, split_operator<Key>>) {
                    return "split";
                } else if constexpr (std::same_as<Op, merge_operator<GroupId>>) {
                    return "merge";
                } else {
                    return "scatter";
                }
            },
            _operator);
    }
};

/// @brief Why a leader declined an operator it was given.
///
/// Every value is a *normal* outcome, not an error: the driver's information
/// was one heartbeat old, and one heartbeat is exactly how out of date it is
/// allowed to be. Counting them by reason is how an operator distinguishes "the
/// driver and the cluster disagree about epochs" (fine, self-correcting) from
/// "every operator is refused because the shard never leaves `merging_source`"
/// (a stuck merge, and a page).
enum class skipped_operator_reason : std::uint8_t {
    stale_epoch,     ///< Computed against an epoch the shard has moved past.
    unknown_shard,   ///< No local replica; the driver's placement view is stale.
    not_leader,      ///< This host no longer leads the shard.
    shard_busy,      ///< Mid-split or mid-merge; the arbiter refused.
    unsupported,     ///< The host cannot perform this operator at all.
    precondition,    ///< Operator-specific precondition failed (e.g. no such peer).
    driver_disabled  ///< The automatic channels are globally off.
};

/// @brief Stream insertion, so a reason can be a metric dimension unedited.
inline auto operator<<(std::ostream& os, skipped_operator_reason r) -> std::ostream& {
    switch (r) {
        case skipped_operator_reason::stale_epoch:
            return os << "stale_epoch";
        case skipped_operator_reason::unknown_shard:
            return os << "unknown_shard";
        case skipped_operator_reason::not_leader:
            return os << "not_leader";
        case skipped_operator_reason::shard_busy:
            return os << "shard_busy";
        case skipped_operator_reason::unsupported:
            return os << "unsupported";
        case skipped_operator_reason::precondition:
            return os << "precondition";
        case skipped_operator_reason::driver_disabled:
            return os << "driver_disabled";
        default:
            return os << "unknown";
    }
}

/// @brief `to_string` for `skipped_operator_reason`.
[[nodiscard]] inline auto to_string(skipped_operator_reason r) -> const char* {
    switch (r) {
        case skipped_operator_reason::stale_epoch:
            return "stale_epoch";
        case skipped_operator_reason::unknown_shard:
            return "unknown_shard";
        case skipped_operator_reason::not_leader:
            return "not_leader";
        case skipped_operator_reason::shard_busy:
            return "shard_busy";
        case skipped_operator_reason::unsupported:
            return "unsupported";
        case skipped_operator_reason::precondition:
            return "precondition";
        case skipped_operator_reason::driver_disabled:
            return "driver_disabled";
        default:
            return "unknown";
    }
}

/// @brief What became of one operator, for the host's counters.
struct operator_outcome {
    std::uint64_t _operation_id{0};
    bool _accepted{false};
    skipped_operator_reason _reason{skipped_operator_reason::precondition};
};

// ─────────────────────────────────────────────────────────────────────────────
// The concept
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A cluster-scope authority over shard identity and placement.
///
/// Every method returns a future: a driver is remote by nature, and a
/// synchronous call would put a network round trip inside a tick.
///
/// `Key` and `NodeId` are unconstrained template parameters with their
/// requirements expressed in the body. A concept may not carry associated
/// constraints on its own parameter list, so `shard_key Key` in the header
/// would be ill-formed.
///
/// @tparam D       The concrete driver.
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Node identifier type.
template<typename D, typename GroupId, typename Key, typename NodeId>
concept shard_placement_driver =
    requires(D& d, std::size_t n, const std::vector<shard_report<GroupId, Key, NodeId>>& sr,
             const node_report<NodeId>& nr, const shard_descriptor<GroupId, Key, NodeId>& desc,
             const std::vector<shard_descriptor<GroupId, Key, NodeId>>& descs) {
        requires raft_group_id<GroupId>;
        requires shard_key<Key>;

        /// Cluster-unique ids for a split's children. Fewer than `n` returned means
        /// the authority is unavailable and the split is abandoned, never queued.
        {
            d.allocate_shard_ids(n)
        } -> kythira::future<std::vector<shard_id_allocation<GroupId, NodeId>>>;

        /// One call per interval carrying every local leader's report; the response
        /// is the operator list.
        {
            d.report_shard_heartbeat(sr)
        } -> kythira::future<std::vector<shard_operation<GroupId, Key, NodeId>>>;

        { d.report_node_heartbeat(nr) } -> kythira::future<void>;

        /// Told immediately rather than waited for on the next heartbeat: a split
        /// changes the routing table for the whole cluster, and up to a heartbeat
        /// interval of clients holding a descriptor for a range that no longer
        /// exists is a cost with no benefit.
        { d.report_split(desc, descs) } -> kythira::future<void>;
        { d.report_merge(desc, desc) } -> kythira::future<void>;
    };

// ─────────────────────────────────────────────────────────────────────────────
// no_op_shard_placement_driver
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The default: a locally reserved id range and no operators.
///
/// The same shape, and the same purpose, as `no_op_quorum_manager`. A static
/// deployment — one that pre-splits at deploy time and never rebalances — is a
/// legitimate and common configuration, and it should not require standing up a
/// control plane. Requirement 14.7.
///
/// Ids come from a range the operator reserved for this machine. That is safe
/// precisely because it is *reserved*: uniqueness is guaranteed by the operator
/// partitioning the id space across machines, exactly as it would be by a
/// central allocator, just decided once at deploy time instead of per split. If
/// the range is exhausted, `allocate_shard_ids` returns fewer ids than asked
/// for, which the host already treats as "the authority is unavailable" and
/// abandons the split — a refusal, never a duplicate id.
///
/// @tparam GroupId Must satisfy `raft_group_id`.
/// @tparam Key     Must satisfy `shard_key`.
/// @tparam NodeId  Node identifier type.
template<raft_group_id GroupId = std::uint64_t, shard_key Key = std::string,
         typename NodeId = std::uint64_t>
class no_op_shard_placement_driver {
public:
    using group_id_type = GroupId;
    using key_type = Key;
    using node_id_type = NodeId;
    using allocation_type = shard_id_allocation<GroupId, NodeId>;
    using operation_type = shard_operation<GroupId, Key, NodeId>;
    using report_type = shard_report<GroupId, Key, NodeId>;

    /// @brief Constructs a driver owning `[first, last)` of the id space.
    ///
    /// The half-open form is deliberate: `last` is the first id this machine
    /// may *not* use, so two adjacent ranges can be written down without an
    /// off-by-one argument about whether they overlap.
    no_op_shard_placement_driver(GroupId first_id, GroupId last_id)
        : _next(std::move(first_id)), _last(std::move(last_id)) {}

    no_op_shard_placement_driver() = default;

    /// @brief Hands out up to `count` ids from the reserved range.
    auto allocate_shard_ids(std::size_t count)
        -> kythira::future_default<std::vector<allocation_type>> {
        std::vector<allocation_type> out;
        std::lock_guard lock(_mutex);
        for (std::size_t i = 0; i < count && _next < _last; ++i) {
            out.push_back(allocation_type{._group_id = _next});
            ++_next;
        }
        _allocated += out.size();
        return kythira::future_factory_default::makeFuture(std::move(out));
    }

    /// @brief Accepts the reports and returns no operators.
    auto report_shard_heartbeat(const std::vector<report_type>& reports)
        -> kythira::future_default<std::vector<operation_type>> {
        {
            std::lock_guard lock(_mutex);
            ++_shard_heartbeats;
            _last_shard_report_count = reports.size();
        }
        return kythira::future_factory_default::makeFuture(std::vector<operation_type>{});
    }

    auto report_node_heartbeat(const node_report<NodeId>&) -> kythira::future_default<void> {
        {
            std::lock_guard lock(_mutex);
            ++_node_heartbeats;
        }
        return kythira::future_factory_default::makeFuture();
    }

    auto report_split(const shard_descriptor<GroupId, Key, NodeId>&,
                      const std::vector<shard_descriptor<GroupId, Key, NodeId>>&)
        -> kythira::future_default<void> {
        {
            std::lock_guard lock(_mutex);
            ++_splits_reported;
        }
        return kythira::future_factory_default::makeFuture();
    }

    auto report_merge(const shard_descriptor<GroupId, Key, NodeId>&,
                      const shard_descriptor<GroupId, Key, NodeId>&)
        -> kythira::future_default<void> {
        {
            std::lock_guard lock(_mutex);
            ++_merges_reported;
        }
        return kythira::future_factory_default::makeFuture();
    }

    /// @name Diagnostics
    /// @{
    [[nodiscard]] auto allocated_count() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _allocated;
    }
    [[nodiscard]] auto remaining_ids() const -> bool {
        std::lock_guard lock(_mutex);
        return _next < _last;
    }
    [[nodiscard]] auto shard_heartbeat_count() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _shard_heartbeats;
    }
    [[nodiscard]] auto node_heartbeat_count() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _node_heartbeats;
    }
    [[nodiscard]] auto last_shard_report_count() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _last_shard_report_count;
    }
    [[nodiscard]] auto splits_reported() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _splits_reported;
    }
    [[nodiscard]] auto merges_reported() const -> std::size_t {
        std::lock_guard lock(_mutex);
        return _merges_reported;
    }
    /// @}

private:
    mutable std::mutex _mutex;
    GroupId _next{};
    GroupId _last{};
    std::size_t _allocated{0};
    std::size_t _shard_heartbeats{0};
    std::size_t _node_heartbeats{0};
    std::size_t _last_shard_report_count{0};
    std::size_t _splits_reported{0};
    std::size_t _merges_reported{0};
};

static_assert(shard_placement_driver<no_op_shard_placement_driver<>, std::uint64_t, std::string,
                                     std::uint64_t>);

}  // namespace kythira
