// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file alibaba_ess_quorum_manager.hpp
/// @brief `quorum_manager` backed by an Alibaba Cloud Auto Scaling (ESS)
///        scaling group (`.kiro/specs/alibaba-cloud-services/`,
///        Requirements 3-10).
///
/// Two siblings, split by concern. The **semantics** are
/// `aws_asg_quorum_manager`'s — a scaling group whose DesiredCapacity is the
/// only provisioning lever, NodeIds carried in tags because the cloud's own
/// instance identifiers give nothing derivable, and the zone caveat below. The
/// **transport** is `oci_instance_pool_quorum_manager`'s — a hand-rolled
/// signed HTTP client, futures produced by `future_factory_default`, and
/// exceptions flattened into exceptional futures at every public entry point.
///
/// **ESS chooses the zone, so `provision_node`'s `target_group` is a
/// preference, not a placement.** A scaling group balances new instances
/// across the vSwitches it is configured with, and there is no per-zone
/// capacity knob anywhere in `ModifyScalingGroup` — the identical limitation
/// `aws_asg_quorum_manager` has, for the identical reason. When the instance
/// ESS launches lands in a different zone from the requested one this manager
/// **proceeds and logs the actual zone** rather than failing: capacity that
/// exists in the wrong failure domain still beats no capacity (Requirement
/// 7.3). A deployment that needs strict per-zone placement runs **one scaling
/// group per zone** and therefore one manager per zone — that is a config
/// topology, not a code change. Note the deliberate contrast with
/// `oci_instance_pool_quorum_manager`, which *rejects* an unknown
/// `target_group`: OCI's pool exposes its placement configurations, so the
/// mismatch is knowable before the launch; ESS's is only knowable after.
///
/// **Tag writes here are additive, unlike the OCI manager's.** ECS
/// `TagResources` creates-or-overwrites exactly the keys it is given and
/// leaves every other tag on the instance alone, so there is **no
/// read-merge-write cycle in this file and there must never be one**. OCI's
/// `UpdateInstance` replaces the whole `freeformTags` map, which is why
/// `oci_instance_pool_quorum_manager::merged_tags` exists and why that spec
/// needed a correctness property about it. Copying that shape across would
/// add a `DescribeInstances` round trip per tag write to defend against a
/// hazard this API does not have. This paragraph exists so a future reader
/// comparing the two files does not "fix" the difference.
///
/// **Liveness is ESS `InService` + ECS `Running`, with no heartbeat.** Also
/// the AWS bar rather than the OCI one: OCI grew a node-side heartbeat writer
/// because its spec demanded application-level liveness, and this provider
/// deliberately does not (design.md Non-Goals). A wedged kythira process on a
/// `Running` instance therefore reads as live here.
///
/// Header-only and always compiled — there is no Alibaba SDK to detect. The
/// Kconfig symbol `CONFIG_ALIBABA_QUORUM_MANAGER` selects whether the *tests*
/// are built, not whether this header works.

#include <raft/alibaba_client_config.hpp>
#include <raft/alibaba_http_client.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace kythira {

namespace alibaba_ess_detail {

/// The well-known ECS tag keys (Requirement 5.1).
///
/// Hyphenated, matching the OCI manager's keys rather than the AWS manager's
/// colon-separated ones — ECS tag keys accept both, and agreeing with the
/// newer sibling keeps one spelling across the two hand-rolled providers.
/// Named constants because the scan side and the write side must agree
/// exactly: a typo in one of the two produces a manager that tags instances it
/// will never find again.
inline constexpr const char* tag_cluster = "kythira-cluster";
inline constexpr const char* tag_node_id = "kythira-node-id";

/// ESS OpenAPI version (`ess.aliyuncs.com`).
inline constexpr const char* ess_version = "2014-08-28";
/// ECS OpenAPI version (`ecs.<region>.aliyuncs.com`).
inline constexpr const char* ecs_version = "2014-05-26";

/// `DescribeScalingInstances` page size. 50 is the documented maximum; asking
/// for it minimises the number of signed round trips a large group costs.
inline constexpr int scaling_instance_page_size = 50;
/// `DescribeInstances` accepts at most 100 IDs in its `InstanceIds` array.
/// Batching to that limit is why this manager needs no concurrent fan-out
/// helper like `oci_detail::parallel_for` — ECS answers for 100 instances in
/// one call where OCI's `GetInstance` answers for one.
inline constexpr std::size_t ecs_describe_batch = 100;

/// Read a string field, or `""` when it is absent or not a string.
///
/// Absent and wrong-typed collapse into the same answer on purpose: every
/// caller here treats a missing field as "this instance does not tell me
/// that", and a control plane that answered with a number where a string was
/// documented is the same situation from the caller's point of view.
[[nodiscard]] inline auto json_string(const boost::json::value& v, std::string_view key)
    -> std::string {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return {};
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr || !field->is_string()) {
        return {};
    }
    return std::string(field->get_string());
}

/// Read an integral field, or `fallback` when it is absent or not a number.
[[nodiscard]] inline auto json_int(const boost::json::value& v, std::string_view key,
                                   std::int64_t fallback = 0) -> std::int64_t {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return fallback;
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr) {
        return fallback;
    }
    if (field->is_int64()) {
        return field->get_int64();
    }
    if (field->is_uint64()) {
        return static_cast<std::int64_t>(field->get_uint64());
    }
    if (field->is_double()) {
        return static_cast<std::int64_t>(field->get_double());
    }
    return fallback;
}

/// Alibaba's RPC-style JSON wraps every list twice: `{"Outer": {"Inner":
/// [...]}}`, the JSON projection of the XML the API was designed around.
/// Returns `nullptr` when either level is missing, so callers read as "for
/// each element, if any" rather than as defensive unwrapping.
[[nodiscard]] inline auto nested_array(const boost::json::value& v, std::string_view outer,
                                       std::string_view inner) -> const boost::json::array* {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return nullptr;
    }
    const auto* outer_field = obj->if_contains(outer);
    if (outer_field == nullptr) {
        return nullptr;
    }
    const auto* outer_obj = outer_field->if_object();
    if (outer_obj == nullptr) {
        return nullptr;
    }
    const auto* inner_field = outer_obj->if_contains(inner);
    if (inner_field == nullptr) {
        return nullptr;
    }
    return inner_field->if_array();
}

/// The first address out of an ECS `{"IpAddress": ["..."]}` wrapper.
[[nodiscard]] inline auto first_ip(const boost::json::value& v, std::string_view key)
    -> std::string {
    const auto* obj = v.if_object();
    if (obj == nullptr) {
        return {};
    }
    const auto* field = obj->if_contains(key);
    if (field == nullptr) {
        return {};
    }
    const auto* wrapper = field->if_object();
    if (wrapper == nullptr) {
        return {};
    }
    const auto* list = wrapper->if_contains("IpAddress");
    if (list == nullptr) {
        return {};
    }
    const auto* arr = list->if_array();
    if (arr == nullptr || arr->empty()) {
        return {};
    }
    const auto& first = arr->front();
    return first.is_string() ? std::string(first.get_string()) : std::string{};
}

/// Read an ECS `{"Tags": {"Tag": [{"TagKey": k, "TagValue": v}]}}` block into a
/// plain map, skipping entries that do not carry a string key.
[[nodiscard]] inline auto tags_of(const boost::json::value& v)
    -> std::map<std::string, std::string> {
    std::map<std::string, std::string> out;
    const auto* arr = nested_array(v, "Tags", "Tag");
    if (arr == nullptr) {
        return out;
    }
    for (const auto& entry : *arr) {
        auto key = json_string(entry, "TagKey");
        if (key.empty()) {
            continue;
        }
        out.insert_or_assign(std::move(key), json_string(entry, "TagValue"));
    }
    return out;
}

/// One scaling-group member as this manager needs it: the ESS half (lifecycle)
/// and the ECS half (everything else) joined on the instance ID.
struct instance_view {
    std::string id;                             ///< ECS instance ID, e.g. `i-bp1...`.
    std::string lifecycle_state;                ///< ESS: `InService`, `Pending`, `Removing`, ...
    std::string status;                         ///< ECS: `Running`, `Starting`, `Stopped`, ...
    std::string zone_id;                        ///< ECS: e.g. `cn-hangzhou-b`.
    std::string private_ip;                     ///< VPC private IP (classic inner IP as fallback).
    std::map<std::string, std::string> tags{};  ///< Full ECS tag map, verbatim.
};

}  // namespace alibaba_ess_detail

// ============================================================================
// alibaba_ess_quorum_manager_config
// ============================================================================

/// @brief Configuration for @ref alibaba_ess_quorum_manager (Requirement 3.1).
///
/// Shorter than the AWS/OCI configs by exactly the fields describing *what to
/// launch* — image, instance type, vSwitch, security group, user data. Those
/// live in the scaling configuration the group already references, the same
/// reason `aws_asg_quorum_manager_config` defers them to the ASG's launch
/// template. This manager never calls `CreateScalingGroup` or
/// `CreateScalingConfiguration` (Requirement 3.3).
struct alibaba_ess_quorum_manager_config {
    /// Credentials, region and endpoint override.
    alibaba_client_config alibaba{};
    /// Logical cluster name, written to the `kythira-cluster` tag and the
    /// filter every read applies before counting anything. Required.
    std::string cluster_name;
    /// ID of a pre-existing ESS scaling group. Required.
    std::string scaling_group_id;
    /// TCP port the kythira process listens on; written into the returned
    /// address.
    std::uint16_t node_port{7000};
    /// Target node counts per Zone ID.
    desired_topology<std::string> topology{};
    /// Maximum wait for a newly launched instance to reach `InService` +
    /// `Running`, and the bound on the decommission consistency poll.
    std::chrono::seconds provision_timeout{300};
    /// Interval between `DescribeScalingInstances` polls.
    std::chrono::milliseconds poll_interval{5000};
    /// Extra ECS tags written onto every adopted instance. Cannot override the
    /// two well-known keys — they are applied last.
    std::map<std::string, std::string> extra_tags{};
};

// ============================================================================
// alibaba_ess_quorum_manager
// ============================================================================

/// @brief `quorum_manager` driving an ESS scaling group's DesiredCapacity.
///
/// @tparam NodeId  Node identifier type; defaults to `std::uint64_t`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class alibaba_ess_quorum_manager {
public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;  ///< A Zone ID, e.g. `cn-hangzhou-b`.

    /// @brief Validates the config and probes the scaling group
    ///        (Requirement 3.2).
    ///
    /// The `DescribeScalingGroups` call is a fail-fast probe, not a formality:
    /// a wrong group ID or a credential that cannot read ESS is a
    /// configuration error, and the worst possible moment to discover one is
    /// the first remediation — the moment the cluster is already degraded.
    ///
    /// Field validation happens **before** the probe, so a deployment with a
    /// blank line in its env file is told which field is blank rather than
    /// which HTTP call failed.
    ///
    /// @throws std::invalid_argument on a missing required field.
    /// @throws std::runtime_error when the probe fails or the group is absent.
    explicit alibaba_ess_quorum_manager(alibaba_ess_quorum_manager_config cfg)
        : _cfg(std::move(cfg)), _http(_cfg.alibaba) {
        const auto require = [](const std::string& value, const char* field) {
            if (value.empty()) {
                throw std::invalid_argument(std::string("alibaba_ess_quorum_manager: ") + field +
                                            " must be non-empty");
            }
        };
        // `region` derives the ECS host and rides in every RPC as `RegionId`;
        // `endpoint_override` replaces the derived host outright, which is the
        // one configuration where an empty region is legitimate (it is how the
        // mock tier runs).
        if (_cfg.alibaba.endpoint_override.empty()) {
            require(_cfg.alibaba.region, "alibaba.region");
        }
        require(_cfg.cluster_name, "cluster_name");
        require(_cfg.scaling_group_id, "scaling_group_id");
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("alibaba_ess_quorum_manager: node_port must be non-zero");
        }

        boost::json::value described;
        try {
            described = describe_scaling_group();
        } catch (const std::exception& ex) {
            throw std::runtime_error(
                std::string("alibaba_ess_quorum_manager: DescribeScalingGroups failed for ") +
                _cfg.scaling_group_id + ": " + ex.what());
        }
        const auto* groups =
            alibaba_ess_detail::nested_array(described, "ScalingGroups", "ScalingGroup");
        if (groups == nullptr || groups->empty()) {
            throw std::runtime_error("alibaba_ess_quorum_manager: no scaling group named " +
                                     _cfg.scaling_group_id + " exists in region " +
                                     _cfg.alibaba.region);
        }
    }

    alibaba_ess_quorum_manager(const alibaba_ess_quorum_manager&) = delete;
    auto operator=(const alibaba_ess_quorum_manager&) -> alibaba_ess_quorum_manager& = delete;
    alibaba_ess_quorum_manager(alibaba_ess_quorum_manager&&) = default;
    auto operator=(alibaba_ess_quorum_manager&&) -> alibaba_ess_quorum_manager& = default;
    ~alibaba_ess_quorum_manager() = default;

    /// @brief Classifies every supplied node as live or unreachable
    ///        (Requirements 6.1-6.5).
    ///
    /// One `DescribeScalingInstances` walk for lifecycle states, then one
    /// `DescribeInstances` per 100 members for status/zone/IP/tags. Live is
    /// ESS `InService` **and** ECS `Running`; there is no heartbeat rule
    /// (Requirement 6.2).
    ///
    /// Reads only — this call modifies no Alibaba resource. A transport or API
    /// failure becomes an exceptional future rather than a health object
    /// claiming everything is down (Requirement 6.5): "the control plane is
    /// unreachable" and "every node is dead" call for opposite responses, and
    /// only one of them is worth terminating instances over.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/alibaba/ess/list_instances",
                      throw std::runtime_error("fault: raft/alibaba/ess/list_instances"););

            // An empty cluster is healthy by definition and costs no API call.
            // Not merely an optimisation: a caller polling an empty cluster
            // should not need Alibaba reachable at all.
            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::map<std::string, bool> live_map;
            for (const auto& inst : cluster_members()) {
                const auto id_tag = inst.tags.find(alibaba_ess_detail::tag_node_id);
                if (id_tag == inst.tags.end()) {
                    // Requirement 5.4: in the group and ours, but not yet
                    // adopted. It has no NodeId to be counted under.
                    continue;
                }
                live_map[id_tag->second] = is_live(inst);
            }
            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("alibaba_ess_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// @brief Assess, decommission what is unreachable, refill each zone's
    ///        deficit (Requirements 9.1-9.2).
    ///
    /// The same six steps every sibling implements, returning the
    /// **pre**-remediation health for the same reason they do: the caller
    /// asked what state the cluster was in, and answering with the state after
    /// this call's own repairs would hide the fault that triggered them.
    ///
    /// Individual remediation failures are logged and stepped over
    /// (Requirement 9.2) — one instance that will not terminate must not stop
    /// the other four zones from being refilled. An *assessment* failure is
    /// different and aborts outright: provisioning against an unknown cluster
    /// state is how a transient API error turns into a doubled cluster.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/alibaba/ess/maintain_quorum",
                      throw std::runtime_error("fault: raft/alibaba/ess/maintain_quorum"););
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        quorum_health<NodeId, std::string> pre_health;
        try {
            pre_health = std::move(assess_quorum(cluster)).get();
        } catch (...) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::current_exception());
        }

        std::map<std::string, NodeId> last_replaced;
        for (const auto& nid : pre_health.unreachable_nodes) {
            std::string group;
            for (const auto& np : cluster) {
                if (np.node_id == nid) {
                    group = np.group_id;
                    break;
                }
            }
            try {
                std::move(decommission_node(nid)).get();
                last_replaced[group] = nid;
            } catch (const std::exception& ex) {
                std::cerr << "[alibaba_ess_quorum_manager::maintain_quorum] decommission of "
                          << node_id_str(nid) << " failed: " << ex.what() << "\n";
            }
        }

        for (const auto& gt : _cfg.topology.groups) {
            std::size_t live = 0;
            for (const auto& gh : pre_health.groups) {
                if (gh.group_id == gt.group_id) {
                    live = gh.live_count;
                    break;
                }
            }
            const auto deficit =
                static_cast<std::ptrdiff_t>(gt.target_count) - static_cast<std::ptrdiff_t>(live);
            for (std::ptrdiff_t i = 0; i < deficit; ++i) {
                std::optional<NodeId> hint;
                if (const auto it = last_replaced.find(gt.group_id); it != last_replaced.end()) {
                    hint = it->second;
                }
                try {
                    std::move(provision_node(gt.group_id, hint)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[alibaba_ess_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// @brief Grows the group by one, adopts the new instance, and returns its
    ///        address (Requirements 7.1-7.6).
    ///
    /// `ModifyScalingGroup` with DesiredCapacity+1 rather than
    /// `ScaleWithAdjustment`: the target is absolute, so the pathological
    /// retry — the request succeeded but its response was lost — re-asserts the
    /// same number instead of adding a second instance. That is the whole
    /// argument, and it is `aws_asg_quorum_manager`'s `SetDesiredCapacity`
    /// argument verbatim.
    ///
    /// The new instance is identified by *absence from the pre-growth
    /// snapshot* rather than by lacking a `kythira-node-id` tag: a scaling
    /// group may legitimately hold a second cluster's instances, and those are
    /// tagged for their own cluster, not untagged.
    ///
    /// @param target_group A Zone ID. A **preference** — see the file comment
    ///        and Requirement 7.3; a mismatch logs and proceeds.
    /// @param replacing Diagnostic only. The new instance always receives a
    ///        fresh NodeId (Requirement 7.5): reusing the retired one would
    ///        resurrect an identity the Raft log may still be reasoning about.
    auto provision_node(std::string target_group, std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        const auto started = std::chrono::steady_clock::now();
        try {
            fiu_do_on("raft/alibaba/ess/modify_capacity",
                      throw std::runtime_error("fault: raft/alibaba/ess/modify_capacity"););

            if (replacing.has_value()) {
                std::cerr << "[alibaba_ess_quorum_manager::provision_node] replacing node "
                          << node_id_str(*replacing) << " in " << target_group << "\n";
            }

            const auto snapshot = list_scaling_instances();
            const auto original_capacity = current_desired_capacity(snapshot.size());
            std::vector<std::string> known;
            known.reserve(snapshot.size());
            for (const auto& [id, lifecycle] : snapshot) {
                known.push_back(id);
            }

            set_desired_capacity(original_capacity + 1);

            std::optional<alibaba_ess_detail::instance_view> launched;
            std::vector<alibaba_ess_detail::instance_view> members;
            const auto deadline = started + _cfg.provision_timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                try {
                    members = describe_members();
                } catch (const std::exception& ex) {
                    // A failed poll is not a failed provision; the deadline is
                    // what bounds this loop.
                    std::cerr << "[alibaba_ess_quorum_manager::provision_node] poll failed: "
                              << ex.what() << "\n";
                    continue;
                }
                for (const auto& inst : members) {
                    if (std::ranges::find(known, inst.id) != known.end()) {
                        continue;
                    }
                    // Requirement 7.2's bar, which is also the liveness bar:
                    // an instance ESS still counts as `Pending` has no usable
                    // address, and tagging one that is still being built is
                    // how the OCI sibling learned to wait.
                    if (!is_live(inst) || inst.private_ip.empty()) {
                        continue;
                    }
                    launched = inst;
                    break;
                }
                if (launched.has_value()) {
                    break;
                }
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started);
            if (!launched.has_value()) {
                // Best-effort capacity rollback, exactly as the AWS and OCI
                // siblings do on this path: with no identified instance there
                // is nothing specific to remove, and leaving the group one
                // instance larger than the cluster believes turns a timeout
                // into a permanent capacity drift.
                try {
                    set_desired_capacity(original_capacity);
                } catch (const std::exception& ex) {
                    std::cerr
                        << "[alibaba_ess_quorum_manager::provision_node] capacity rollback to "
                        << original_capacity << " failed: " << ex.what() << "\n";
                }
                throw std::runtime_error(
                    "timed out after " + std::to_string(elapsed.count()) + "s (limit " +
                    std::to_string(_cfg.provision_timeout.count()) +
                    "s) waiting for a new InService/Running instance in scaling group " +
                    _cfg.scaling_group_id + " for zone " + target_group);
            }

            if (launched->zone_id != target_group) {
                // Requirement 7.3. Not an error: ESS placed the capacity, the
                // caller gets told where it actually landed, and a deployment
                // that cannot tolerate the drift runs one group per zone.
                std::cerr << "[alibaba_ess_quorum_manager::provision_node] scaling group "
                          << _cfg.scaling_group_id << " placed " << launched->id << " in zone "
                          << launched->zone_id << ", not the requested " << target_group
                          << "; proceeding (ESS chooses placement — one group per zone is the "
                             "only way to constrain it)\n";
            }

            // Computed from the same listing the candidate came from rather
            // than a fresh one: two scans could disagree, and the one that
            // matters is the one that identified the instance.
            const NodeId new_id = next_node_id_from(members);

            std::map<std::string, std::string> tags = _cfg.extra_tags;
            // Written last so an operator cannot redirect a managed instance by
            // supplying its own `kythira-node-id` in `extra_tags`.
            tags[alibaba_ess_detail::tag_cluster] = _cfg.cluster_name;
            tags[alibaba_ess_detail::tag_node_id] = node_id_str(new_id);
            try {
                tag_instance(launched->id, tags);
            } catch (...) {
                // We grew the group and cannot record who owns the result;
                // leaving it running is how a provisioning bug becomes a bill
                // (the OCI sibling's lesson, learned against live infra).
                best_effort_remove(launched->id);
                throw;
            }

            auto addr =
                static_cast<Address>(launched->private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("alibaba_ess_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// @brief Removes the instance carrying @p node_id from the group and
    ///        terminates it (Requirements 8.1-8.4).
    ///
    /// `RemoveInstances` rather than `DetachInstances` + a capacity decrement:
    /// it terminates the instance **and** shrinks DesiredCapacity in one call,
    /// so there is no window in which the group is one instance short of its
    /// target and helpfully launches a replacement for the node we just
    /// decided to retire.
    ///
    /// Idempotent (Requirement 8.3): a NodeId no instance carries resolves
    /// successfully without any mutating call. A second delete of an
    /// already-gone node is the common remediation race, not an error — and a
    /// caller retrying after a partial failure must not be told the second
    /// attempt failed.
    ///
    /// The group's MinSize is ESS's business, not this class's: removing the
    /// last instance is attempted, and a MinSize refusal surfaces as the
    /// vendor's own error (Requirement 8.4).
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/alibaba/ess/remove_instances",
                      throw std::runtime_error("fault: raft/alibaba/ess/remove_instances"););

            const auto found = find_instance(node_id);
            if (!found.has_value()) {
                return future_factory_default::makeFuture();
            }

            try {
                std::map<std::string, std::string> params{
                    {"ScalingGroupId", _cfg.scaling_group_id},
                    {"InstanceId.1", found->id},
                    // The documented default, restated explicitly because the
                    // whole point of choosing this call is the atomic
                    // capacity decrement — a default that changed under us
                    // would be silent, and this makes it a visible parameter.
                    {"DecreaseDesiredCapacity", "true"},
                };
                (void)ess("RemoveInstances", std::move(params));
            } catch (const std::exception& ex) {
                // Only "that instance is not here" is forgiven, and only
                // because it is the same idempotency case as an absent tag,
                // observed one listing later. Anything else is a real failure
                // the caller has to see: quietly succeeding would leave a
                // billed instance running with the orchestrator believing it
                // gone.
                const std::string what = ex.what();
                if (what.find("NotFound") == std::string::npos &&
                    what.find("not found") == std::string::npos) {
                    throw;
                }
                return future_factory_default::makeFuture();
            }

            // Requirement 8.2: a bounded consistency poll so the next
            // assess_quorum observes the instance as gone rather than racing
            // the control plane and re-reporting it live.
            const auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
            for (;;) {
                // Checked before sleeping, not after: a removal that took
                // effect immediately is the common case, and sleeping first
                // would put a poll interval's floor under every decommission
                // for nothing.
                bool still_present = false;
                try {
                    for (const auto& [id, lifecycle] : list_scaling_instances()) {
                        if (id == found->id) {
                            still_present = true;
                            break;
                        }
                    }
                } catch (const std::exception&) {
                    // A read failure is not evidence either way; the deadline
                    // is what ends this loop.
                    break;
                }
                if (!still_present) {
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(_cfg.poll_interval);
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("alibaba_ess_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// @brief Returns the configured topology (Requirement 4.3). No API call.
    [[nodiscard]] auto topology() const -> kythira::desired_topology<std::string> {
        return _cfg.topology;
    }

    /// @brief The `NodeId` this manager would assign next (Requirement 5.3).
    ///
    /// Max-of-parsed-tags plus one, scanned across **this cluster's** members
    /// only. Two clusters sharing one scaling group therefore number
    /// independently, which is the same blast-radius rule every other read
    /// here follows (design.md Property 4) and is safe because NodeIds only
    /// need to be unique within a cluster.
    ///
    /// **NodeIds are reused after a decommission.** The scan can only see
    /// instances the group still lists, so once the highest-numbered node is
    /// removed the next provision gets that number back. There is no ESS call
    /// that would show a removed instance's tags, so this is a property of the
    /// design rather than an oversight in it: a deployment that cannot
    /// tolerate a recycled identity has to keep the assignment somewhere
    /// outside the group.
    ///
    /// The TOCTOU race between two concurrent provisions is the one every
    /// sibling has, and its resolution stays deferred to `quorum_management.hpp`'s
    /// leader-side pending-provision tracking note.
    ///
    /// Exposed for tests and diagnostics; `provision_node` computes the same
    /// value from the listing it already holds, so the two cannot disagree.
    [[nodiscard]] auto next_node_id() const -> NodeId {
        return next_node_id_from(describe_members());
    }

private:
    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
        }
    }

    /// Requirement 6.2's whole liveness ladder, in one place so the callers
    /// read as classification rather than as policy.
    [[nodiscard]] static auto is_live(const alibaba_ess_detail::instance_view& inst) -> bool {
        return inst.lifecycle_state == "InService" && inst.status == "Running";
    }

    /// One ESS RPC, with `RegionId` folded in when the config carries one.
    [[nodiscard]] auto ess(std::string_view action, std::map<std::string, std::string> params) const
        -> boost::json::value {
        if (!_cfg.alibaba.region.empty()) {
            params.emplace("RegionId", _cfg.alibaba.region);
        }
        return _http.rpc("ess", action, alibaba_ess_detail::ess_version, params);
    }

    /// One ECS RPC, likewise.
    [[nodiscard]] auto ecs(std::string_view action, std::map<std::string, std::string> params) const
        -> boost::json::value {
        if (!_cfg.alibaba.region.empty()) {
            params.emplace("RegionId", _cfg.alibaba.region);
        }
        return _http.rpc("ecs", action, alibaba_ess_detail::ecs_version, params);
    }

    [[nodiscard]] auto describe_scaling_group() const -> boost::json::value {
        return ess("DescribeScalingGroups", {{"ScalingGroupId.1", _cfg.scaling_group_id}});
    }

    /// The group's DesiredCapacity, or a best available substitute.
    ///
    /// A scaling group that has never had a capacity set reports no
    /// `DesiredCapacity` at all; `TotalCapacity` (what the group actually
    /// holds) is the next best answer, and the caller's own member count is
    /// the last. Guessing zero here would shrink the group on the next
    /// `ModifyScalingGroup`, which is the one wrong answer available.
    [[nodiscard]] auto current_desired_capacity(std::size_t member_count) const -> std::int64_t {
        const auto described = describe_scaling_group();
        const auto* groups =
            alibaba_ess_detail::nested_array(described, "ScalingGroups", "ScalingGroup");
        if (groups == nullptr || groups->empty()) {
            throw std::runtime_error("scaling group " + _cfg.scaling_group_id +
                                     " disappeared between construction and provisioning");
        }
        const auto& group = groups->front();
        auto capacity = alibaba_ess_detail::json_int(group, "DesiredCapacity", -1);
        if (capacity < 0) {
            capacity = alibaba_ess_detail::json_int(group, "TotalCapacity", -1);
        }
        if (capacity < 0) {
            capacity = static_cast<std::int64_t>(member_count);
        }
        return capacity;
    }

    auto set_desired_capacity(std::int64_t capacity) const -> void {
        (void)ess("ModifyScalingGroup", {{"ScalingGroupId", _cfg.scaling_group_id},
                                         {"DesiredCapacity", std::to_string(capacity)}});
    }

    /// `DescribeScalingInstances`, paginated to exhaustion (Requirement 6.1).
    ///
    /// Returns `(instance id, lifecycle state)` pairs for **every** instance
    /// the group holds, this cluster's or not — filtering is the caller's job
    /// and happens after the ECS tags are known.
    [[nodiscard]] auto list_scaling_instances() const
        -> std::vector<std::pair<std::string, std::string>> {
        std::vector<std::pair<std::string, std::string>> out;
        for (int page = 1;; ++page) {
            const auto response =
                ess("DescribeScalingInstances",
                    {{"ScalingGroupId", _cfg.scaling_group_id},
                     {"PageNumber", std::to_string(page)},
                     {"PageSize", std::to_string(alibaba_ess_detail::scaling_instance_page_size)}});
            const auto* arr =
                alibaba_ess_detail::nested_array(response, "ScalingInstances", "ScalingInstance");
            if (arr == nullptr || arr->empty()) {
                break;
            }
            for (const auto& entry : *arr) {
                auto id = alibaba_ess_detail::json_string(entry, "InstanceId");
                if (id.empty()) {
                    continue;
                }
                out.emplace_back(std::move(id),
                                 alibaba_ess_detail::json_string(entry, "LifecycleState"));
            }
            // A short page is the last page. Trusting `TotalCount` instead
            // would loop forever against a control plane whose count and
            // pages disagree, which is exactly the state a scaling group is in
            // while it scales.
            if (arr->size() <
                static_cast<std::size_t>(alibaba_ess_detail::scaling_instance_page_size)) {
                break;
            }
        }
        return out;
    }

    /// ESS lifecycle joined to ECS status/zone/IP/tags for every group member.
    [[nodiscard]] auto describe_members() const -> std::vector<alibaba_ess_detail::instance_view> {
        const auto listed = list_scaling_instances();
        std::vector<alibaba_ess_detail::instance_view> out;
        out.reserve(listed.size());
        std::map<std::string, std::size_t> index_of;
        for (const auto& [id, lifecycle] : listed) {
            index_of.emplace(id, out.size());
            out.push_back(
                alibaba_ess_detail::instance_view{.id = id, .lifecycle_state = lifecycle});
        }

        for (std::size_t start = 0; start < listed.size();
             start += alibaba_ess_detail::ecs_describe_batch) {
            const auto stop =
                std::min(start + alibaba_ess_detail::ecs_describe_batch, listed.size());
            boost::json::array ids;
            for (std::size_t i = start; i < stop; ++i) {
                ids.push_back(boost::json::string(listed[i].first));
            }
            // `InstanceIds` is a JSON array *inside* a query parameter — the
            // one place this RPC-style API departs from flat `Key.N` lists.
            const auto response =
                ecs("DescribeInstances",
                    {{"InstanceIds", boost::json::serialize(ids)},
                     {"PageSize", std::to_string(alibaba_ess_detail::ecs_describe_batch)}});
            const auto* instances =
                alibaba_ess_detail::nested_array(response, "Instances", "Instance");
            if (instances == nullptr) {
                continue;
            }
            for (const auto& entry : *instances) {
                const auto id = alibaba_ess_detail::json_string(entry, "InstanceId");
                const auto slot = index_of.find(id);
                if (slot == index_of.end()) {
                    continue;
                }
                auto& view = out[slot->second];
                view.status = alibaba_ess_detail::json_string(entry, "Status");
                view.zone_id = alibaba_ess_detail::json_string(entry, "ZoneId");
                view.tags = alibaba_ess_detail::tags_of(entry);
                // VPC first, classic network second: a VPC instance reports
                // its address under `VpcAttributes`, a classic one under
                // `InnerIpAddress`, and neither reports both.
                const auto* obj = entry.if_object();
                if (obj != nullptr) {
                    if (const auto* vpc = obj->if_contains("VpcAttributes"); vpc != nullptr) {
                        view.private_ip = alibaba_ess_detail::first_ip(*vpc, "PrivateIpAddress");
                    }
                }
                if (view.private_ip.empty()) {
                    view.private_ip = alibaba_ess_detail::first_ip(entry, "InnerIpAddress");
                }
            }
        }
        return out;
    }

    /// Requirement 6.4 / design.md Property 4: everything that counts, tags or
    /// removes goes through this filter first, so the blast radius of every
    /// mutating call is bounded to this cluster's own instances. An operator
    /// sharing the group with a second cluster — or with plain untagged
    /// instances — is a supported configuration, not a hazard.
    [[nodiscard]] auto cluster_members() const -> std::vector<alibaba_ess_detail::instance_view> {
        std::vector<alibaba_ess_detail::instance_view> out;
        for (auto& inst : describe_members()) {
            const auto tag = inst.tags.find(alibaba_ess_detail::tag_cluster);
            if (tag != inst.tags.end() && tag->second == _cfg.cluster_name) {
                out.push_back(std::move(inst));
            }
        }
        return out;
    }

    [[nodiscard]] auto next_node_id_from(
        const std::vector<alibaba_ess_detail::instance_view>& instances) const -> NodeId {
        std::uint64_t highest = 0;
        for (const auto& inst : instances) {
            const auto cluster = inst.tags.find(alibaba_ess_detail::tag_cluster);
            if (cluster == inst.tags.end() || cluster->second != _cfg.cluster_name) {
                continue;
            }
            const auto tag = inst.tags.find(alibaba_ess_detail::tag_node_id);
            if (tag == inst.tags.end()) {
                continue;
            }
            try {
                highest = std::max(highest, static_cast<std::uint64_t>(std::stoull(tag->second)));
            } catch (const std::exception&) {
                // A tag nobody here wrote. Ignoring it is right: it cannot
                // collide with an id this manager assigns, because this
                // manager only ever assigns parseable ones.
            }
        }
        return static_cast<NodeId>(highest + 1);
    }

    [[nodiscard]] auto find_instance(const NodeId& node) const
        -> std::optional<alibaba_ess_detail::instance_view> {
        const auto wanted = node_id_str(node);
        for (auto& inst : cluster_members()) {
            const auto tag = inst.tags.find(alibaba_ess_detail::tag_node_id);
            if (tag != inst.tags.end() && tag->second == wanted) {
                return inst;
            }
        }
        return std::nullopt;
    }

    /// ECS `TagResources` — **additive, no read-merge-write**. See the file
    /// comment: this is the deliberate contrast with the OCI manager, and it
    /// is why there is no `merged_tags` here to go looking for.
    auto tag_instance(const std::string& instance_id,
                      const std::map<std::string, std::string>& tags) const -> void {
        std::map<std::string, std::string> params{
            {"ResourceType", "instance"},
            {"ResourceId.1", instance_id},
        };
        int n = 1;
        for (const auto& [key, value] : tags) {
            params.emplace("Tag." + std::to_string(n) + ".Key", key);
            params.emplace("Tag." + std::to_string(n) + ".Value", value);
            ++n;
        }
        (void)ecs("TagResources", std::move(params));
    }

    /// Remove an instance we grew the group for but could not finish adopting.
    /// Best-effort by definition: it runs on a path that is already failing,
    /// and the alternative is a running, billed instance that no future call
    /// will ever recognise as ours.
    auto best_effort_remove(const std::string& instance_id) const noexcept -> void {
        try {
            (void)ess("RemoveInstances", {{"ScalingGroupId", _cfg.scaling_group_id},
                                          {"InstanceId.1", instance_id},
                                          {"DecreaseDesiredCapacity", "true"}});
        } catch (const std::exception& ex) {
            std::cerr << "[alibaba_ess_quorum_manager::provision_node] could not clean up "
                      << instance_id << " after a failed adoption; it may still be running and "
                      << "billing: " << ex.what() << "\n";
        }
    }

    /// Identical to the AWS and OCI siblings' `build_health`, deliberately:
    /// the per-group breakdown is a property of the topology and the supplied
    /// cluster, not of the cloud (Requirement 6.3). Divergence here would be a
    /// difference in behaviour with no difference in cause.
    [[nodiscard]] auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                                    const std::map<std::string, bool>& live_map) const
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        std::vector<NodeId> unreachable;
        std::size_t live_count = 0;
        std::map<std::string, std::size_t> group_live;
        for (const auto& np : cluster) {
            const auto it = live_map.find(node_id_str(np.node_id));
            if (it != live_map.end() && it->second) {
                ++live_count;
                group_live[np.group_id]++;
            } else {
                unreachable.push_back(np.node_id);
            }
        }

        std::vector<placement_group_health<NodeId, std::string>> groups;
        for (const auto& gt : _cfg.topology.groups) {
            std::size_t group_live_count = 0;
            if (const auto it = group_live.find(gt.group_id); it != group_live.end()) {
                group_live_count = it->second;
            }
            std::vector<NodeId> group_unreachable;
            for (const auto& nid : unreachable) {
                for (const auto& np : cluster) {
                    if (np.node_id == nid && np.group_id == gt.group_id) {
                        group_unreachable.push_back(nid);
                    }
                }
            }
            groups.push_back({.group_id = gt.group_id,
                              .live_count = group_live_count,
                              .target_count = gt.target_count,
                              .unreachable_nodes = std::move(group_unreachable)});
        }

        return future_factory_default::makeFuture(quorum_health<NodeId, std::string>{
            .status = compute_quorum_status(live_count, cluster.size()),
            .live_node_count = live_count,
            .total_node_count = cluster.size(),
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

    /// The same four-level mapping every quorum manager in this tree uses.
    static auto compute_quorum_status(std::size_t live, std::size_t total) -> quorum_status {
        if (total == 0) {
            return quorum_status::healthy;
        }
        const std::size_t majority = total / 2 + 1;
        if (live < majority) {
            return quorum_status::lost;
        }
        if (live == majority) {
            return quorum_status::critical;
        }
        if (live < total) {
            return quorum_status::degraded;
        }
        return quorum_status::healthy;
    }

    alibaba_ess_quorum_manager_config _cfg;
    alibaba_http_client _http;
};

static_assert(quorum_manager<alibaba_ess_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "alibaba_ess_quorum_manager must satisfy quorum_manager");

}  // namespace kythira
