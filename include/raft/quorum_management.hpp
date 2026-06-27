#pragma once

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

// Placement groups partition cluster nodes by failure domain: availability
// zones, physical racks, network spines, etc.  The GroupId type must be
// regular (value-semantic, copyable, equality-comparable) and totally ordered
// so that it can be used as a map key or sorted for stable iteration.
template<typename T>
concept placement_group_id = std::regular<T> && std::totally_ordered<T>;

// ============================================================================
// Quorum health types
// ============================================================================

// Classification of current quorum health, ordered by severity.
enum class quorum_status : std::uint8_t {
    // All nodes responsive and cluster is at desired size.
    healthy,
    // Quorum is intact but the cluster is below desired size (e.g. one node
    // terminated and not yet replaced).  A quorum manager should provision a
    // replacement.
    degraded,
    // One additional failure would lose quorum.  Immediate action required.
    critical,
    // Majority of nodes are unreachable; the cluster cannot make progress.
    lost,
};

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

// Pairs a node identity with its placement group.  Passed to assess_quorum
// so that the quorum manager can produce per-group health breakdowns without
// maintaining internal state about which nodes it provisioned.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct node_placement {
    NodeId node_id;
    GroupId group_id;
};

// Per-group slice of an assess_quorum result.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct placement_group_health {
    GroupId group_id;
    // Nodes in this group that responded within the assessment window.
    std::size_t live_count;
    // Target number of nodes for this group as declared in desired_topology.
    std::size_t target_count;
    // Node IDs in this group that did not respond.
    std::vector<NodeId> unreachable_nodes;
};

// Full assessment report returned by quorum_manager::assess_quorum.
template<typename NodeId, typename GroupId>
requires node_id<NodeId> && placement_group_id<GroupId>
struct quorum_health {
    // Overall quorum status across all placement groups.
    quorum_status status;
    // Total live nodes across all groups.
    std::size_t live_node_count;
    // Total nodes supplied to assess_quorum.
    std::size_t total_node_count;
    // Node IDs that did not respond, regardless of group.
    std::vector<NodeId> unreachable_nodes;
    // Per-group health breakdown.  Empty when group information is unavailable.
    std::vector<placement_group_health<NodeId, GroupId>> groups;
};

// ============================================================================
// Desired topology types
// ============================================================================

// Target node count for one placement group.
template<typename GroupId>
requires placement_group_id<GroupId>
struct placement_group_target {
    GroupId group_id;
    std::size_t target_count;
};

// The full target topology: one entry per placement group.  The orchestrator
// compares this against the live topology from assess_quorum to decide when
// and where to provision replacement nodes.
template<typename GroupId>
requires placement_group_id<GroupId>
struct desired_topology {
    std::vector<placement_group_target<GroupId>> groups;

    // Sum of target_count across all placement groups.
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

// Models a component that can observe quorum health and drive infrastructure-
// level remediation: provisioning replacement nodes and decommissioning broken
// ones.  Implementations are environment-specific (AWS EC2, GCP, bare-metal
// bastion scripts, …); the concept defines the interface that the Raft
// orchestration layer calls.
//
// All operations that touch external infrastructure return kythira::Future<T>
// because they may block on network I/O or cloud API calls.
//
// assess_quorum takes the full cluster membership annotated with placement
// groups.  The caller (orchestrator) supplies this mapping because it tracked
// which group was specified when each node was provisioned.  Receiving it
// explicitly keeps the quorum manager stateless with respect to topology.
//
// provision_node takes an explicit target_group so the orchestrator can
// direct replacements to the group that lost a node.  The optional replacing
// hint names the specific node being replaced, which cloud implementations may
// use to copy instance type, subnet, or tags.
//
// decommission_node terminates and/or deregisters a node so that it cannot
// rejoin as a ghost.  It must be idempotent.
//
// desired_topology is synchronous: it is a pure policy value with no I/O.
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

// Safe default for environments where auto-provisioning is not available
// (static clusters, development, bare-metal without orchestration).
//
// assess_quorum reports all supplied nodes as live and computes per-group
// counts from the input.  It has no mechanism to probe liveness so it assumes
// the best.  Target counts per group are looked up from the constructed
// topology.
//
// provision_node always returns an exceptional Future.  Callers must handle
// the failure; no cluster expansion is possible through this manager.
//
// decommission_node is a no-op.  The caller is responsible for removing the
// node from the cluster configuration via the normal Raft membership path.
template<typename NodeId = std::uint64_t, typename Address = std::string,
         typename GroupId = std::string>
requires node_id<NodeId> && placement_group_id<GroupId>
class no_op_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = GroupId;

    explicit no_op_quorum_manager(desired_topology<GroupId> topology = {})
        : _topology(std::move(topology)) {}

    auto assess_quorum(const std::vector<node_placement<NodeId, GroupId>>& cluster)
        -> kythira::Future<quorum_health<NodeId, GroupId>> {
        // Build per-group health by scanning the cluster list.
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

    auto provision_node(GroupId, std::optional<NodeId>)
        -> kythira::Future<peer_info<NodeId, Address>> {
        return kythira::FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
            std::runtime_error("no_op_quorum_manager: provisioning is not supported"));
    }

    auto decommission_node(const NodeId&) -> kythira::Future<void> {
        return kythira::FutureFactory::makeFuture();
    }

    [[nodiscard]] auto topology() const -> kythira::desired_topology<GroupId> { return _topology; }

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
