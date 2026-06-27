#pragma once

/// @file quorum_management.hpp
/// @brief Quorum-health types, the `quorum_manager` concept, and the no-op default implementation.

#include <raft/future.hpp>
#include <raft/peer_discovery.hpp>
#include <raft/types.hpp>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace kythira {

// ============================================================================
// Placement group identity concept
// ============================================================================

/// @brief Concept constraining a placement-group identifier.
///
/// Placement groups partition cluster nodes by failure domain: availability
/// zones, physical racks, network spines, etc.  The type must be value-semantic
/// (regular) and totally ordered so it can serve as a map key or sorted range.
template<typename T>
concept placement_group_id = std::regular<T> && std::totally_ordered<T>;

// ============================================================================
// Quorum health types
// ============================================================================

/// @brief Classification of current quorum health, ordered by increasing severity.
enum class quorum_status : std::uint8_t {
    healthy,   ///< All nodes responsive and the cluster is at desired size.
    degraded,  ///< Quorum is intact but the cluster is below desired size; a replacement should be
               ///< provisioned.
    critical,  ///< One additional failure would lose quorum; immediate action required.
    lost,      ///< Majority of nodes are unreachable; the cluster cannot make progress.
};

/// @brief Stream insertion operator for `quorum_status`.
inline std::ostream& operator<<(std::ostream& os, quorum_status s) {
    switch (s) {
        case quorum_status::healthy:
            return os << "healthy";
        case quorum_status::degraded:
            return os << "degraded";
        case quorum_status::critical:
            return os << "critical";
        case quorum_status::lost:
            return os << "lost";
        default:
            return os << "unknown";
    }
}

/// @brief Associates a node identity with its placement group.
///
/// Passed to `assess_quorum` so the quorum manager can produce per-group health
/// breakdowns without maintaining internal topology state.
///
/// @tparam NodeId  Must satisfy `node_id`.
/// @tparam GroupId Must satisfy `placement_group_id`.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct node_placement {
    NodeId node_id;    ///< Identifier of the cluster node.
    GroupId group_id;  ///< Placement group this node belongs to.
};

/// @brief Per-group slice of a `quorum_health` assessment result.
/// @tparam NodeId  Must satisfy `node_id`.
/// @tparam GroupId Must satisfy `placement_group_id`.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct placement_group_health {
    GroupId group_id;        ///< Placement group identifier.
    std::size_t live_count;  ///< Nodes in this group that responded within the assessment window.
    std::size_t
        target_count;  ///< Target node count for this group as declared in `desired_topology`.
    std::vector<NodeId> unreachable_nodes;  ///< Node IDs in this group that did not respond.
};

/// @brief Full assessment report returned by `quorum_manager::assess_quorum`.
/// @tparam NodeId  Must satisfy `node_id`.
/// @tparam GroupId Must satisfy `placement_group_id`.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct quorum_health {
    quorum_status status;                   ///< Overall quorum status across all placement groups.
    std::size_t live_node_count;            ///< Total live nodes across all groups.
    std::size_t total_node_count;           ///< Total nodes supplied to `assess_quorum`.
    std::vector<NodeId> unreachable_nodes;  ///< Node IDs that did not respond, regardless of group.
    std::vector<placement_group_health<NodeId, GroupId>>
        groups;  ///< Per-group health breakdown; empty when group information is unavailable.
};

// ============================================================================
// Desired topology types
// ============================================================================

/// @brief Target node count for one placement group.
/// @tparam GroupId Must satisfy `placement_group_id`.
template<typename GroupId>
requires placement_group_id<GroupId>
struct placement_group_target {
    GroupId group_id;          ///< Placement group identifier.
    std::size_t target_count;  ///< Desired number of nodes in this group.
};

/// @brief Full target topology: one entry per placement group.
///
/// The orchestrator compares this against the live topology from `assess_quorum`
/// to decide when and where to provision replacement nodes.
///
/// @tparam GroupId Must satisfy `placement_group_id`.
template<typename GroupId>
requires placement_group_id<GroupId>
struct desired_topology {
    std::vector<placement_group_target<GroupId>> groups;  ///< One target entry per placement group.

    /// @brief Returns the sum of `target_count` across all placement groups.
    [[nodiscard]] auto total_size() const -> std::size_t {
        std::size_t n = 0;
        for (const auto& g : groups) {
            n += g.target_count;
        }
        return n;
    }
};

// ============================================================================
// quorum_manager concept
// ============================================================================

/// @brief Concept modelling a component that observes quorum health and drives infrastructure-level
///        remediation.
///
/// Implementations are environment-specific (AWS EC2, GCP, bare-metal bastion scripts, …).
/// All I/O-bound operations return `kythira::Future<T>` to avoid blocking the Raft event loop.
///
/// ### Caller responsibilities
/// - `assess_quorum`: The caller supplies the full cluster membership annotated with placement
///   groups because it tracked which group was specified when each node was provisioned.  This
///   keeps the quorum manager stateless with respect to topology.
/// - `provision_node`: The caller passes an explicit `target_group` to direct replacements to the
///   group that lost a node.  The optional `replacing` hint names the specific node being replaced.
/// - `decommission_node`: Must be idempotent.
/// - `topology`: Synchronous; returns a pure policy value with no I/O.
///
/// @tparam Q       Concrete quorum manager type.
/// @tparam NodeId  Must satisfy `node_id` and match `Q::node_id_type`.
/// @tparam Address Network address type; must match `Q::address_type`.
/// @tparam GroupId Must satisfy `placement_group_id` and match `Q::placement_group_id_type`.
template<typename Q, typename NodeId, typename Address, typename GroupId>
concept quorum_manager =
    requires(Q& mgr, const std::vector<node_placement<NodeId, GroupId>>& cluster,
             const NodeId& node_id, GroupId target_group, std::optional<NodeId> replacing) {
        typename Q::node_id_type;
        typename Q::address_type;
        typename Q::placement_group_id_type;
        requires std::same_as<typename Q::node_id_type, NodeId>;
        requires std::same_as<typename Q::address_type, Address>;
        requires std::same_as<typename Q::placement_group_id_type, GroupId>;

        {
            mgr.assess_quorum(cluster)
        } -> std::same_as<kythira::Future<quorum_health<NodeId, GroupId>>>;

        {
            mgr.provision_node(target_group, replacing)
        } -> std::same_as<kythira::Future<peer_info<NodeId, Address>>>;

        { mgr.decommission_node(node_id) } -> std::same_as<kythira::Future<void>>;

        { mgr.topology() } -> std::same_as<desired_topology<GroupId>>;

        {
            mgr.maintain_quorum(cluster)
        } -> std::same_as<kythira::Future<quorum_health<NodeId, GroupId>>>;
    };

// ============================================================================
// no_op_quorum_manager
// ============================================================================

/// @brief Safe default quorum manager for static clusters and development environments.
///
/// `assess_quorum` reports all supplied nodes as live and computes per-group counts
/// from the input, making the optimistic assumption that every known node is healthy.
///
/// `provision_node` always returns an exceptional `Future`; callers must handle the failure
/// — no cluster expansion is possible through this manager.
///
/// `decommission_node` is a no-op.  The caller is responsible for removing the node
/// from the cluster configuration via the normal Raft membership change path.
///
/// @tparam NodeId  Node identifier type; defaults to `uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
/// @tparam GroupId Placement-group identifier type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string,
         typename GroupId = std::string>
requires node_id<NodeId> && placement_group_id<GroupId>
class no_op_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = GroupId;

    /// @brief Constructs the manager with an optional target topology.
    /// @param topology Desired node counts per group; used to populate `target_count` fields in
    /// health reports.
    explicit no_op_quorum_manager(desired_topology<GroupId> topology = {})
        : _topology(std::move(topology)) {}

    /// @brief Reports all supplied nodes as live; computes per-group counts from the input.
    /// @param cluster Full cluster membership with placement-group annotations.
    /// @return An immediately-resolved `Future` containing the health report.
    auto assess_quorum(const std::vector<node_placement<NodeId, GroupId>>& cluster)
        -> kythira::Future<quorum_health<NodeId, GroupId>> {
        std::vector<placement_group_health<NodeId, GroupId>> group_health;
        for (const auto& np : cluster) {
            auto it = std::ranges::find(group_health, np.group_id,
                                        &placement_group_health<NodeId, GroupId>::group_id);
            if (it == group_health.end()) {
                std::size_t target = 0;
                for (const auto& gt : _topology.groups) {
                    if (gt.group_id == np.group_id) {
                        target = gt.target_count;
                        break;
                    }
                }
                group_health.push_back(
                    {.group_id = np.group_id, .live_count = 1, .target_count = target});
            } else {
                it->live_count++;
            }
        }

        quorum_health<NodeId, GroupId> report{
            .status = quorum_status::healthy,
            .live_node_count = cluster.size(),
            .total_node_count = cluster.size(),
            .unreachable_nodes = {},
            .groups = std::move(group_health),
        };
        return kythira::FutureFactory::makeFuture(std::move(report));
    }

    /// @brief Always returns an exceptional `Future`; provisioning is not supported.
    /// @throws std::runtime_error (via the Future) unconditionally.
    auto provision_node(GroupId, std::optional<NodeId>)
        -> kythira::Future<peer_info<NodeId, Address>> {
        return kythira::FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
            std::runtime_error("no_op_quorum_manager: provisioning is not supported"));
    }

    /// @brief No-op; the caller should remove the node via the Raft membership path.
    /// @return An immediately-resolved void `Future`.
    auto decommission_node(const NodeId&) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    /// @brief Returns the desired topology supplied at construction.
    [[nodiscard]] auto topology() const -> kythira::desired_topology<GroupId> { return _topology; }

    /// @brief Delegates to `assess_quorum`; no remediation is performed.
    auto maintain_quorum(const std::vector<node_placement<NodeId, GroupId>>& cluster)
        -> kythira::Future<quorum_health<NodeId, GroupId>> {
        return assess_quorum(cluster);
    }

private:
    kythira::desired_topology<GroupId> _topology;
};

static_assert(quorum_manager<no_op_quorum_manager<std::uint64_t, std::string, std::string>,
                             std::uint64_t, std::string, std::string>);

}  // namespace kythira
