// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_asg_quorum_manager.hpp
/// @brief Quorum manager that provisions and monitors Raft nodes through AWS Auto Scaling Groups.

#include <raft/aws_client_config.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/group_scale_rollback.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/autoscaling/model/DescribeAutoScalingInstancesRequest.h>
#include <aws/autoscaling/model/LifecycleState.h>
#include <aws/autoscaling/model/SetInstanceProtectionRequest.h>
#include <aws/autoscaling/model/TerminateInstanceInAutoScalingGroupRequest.h>
#include <aws/autoscaling/model/UpdateAutoScalingGroupRequest.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateTagsRequest.h>
#include <aws/ec2/model/DescribeInstanceStatusRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>

#include <algorithm>
#include <atomic>
#include <string_view>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace kythira {

namespace aws_asg_detail {

/// `SetInstanceProtection` accepts at most 50 instance ids per call.
inline constexpr std::size_t protection_batch = 50;

/// @brief Map an Auto Scaling lifecycle name onto the rollback planner's
///        three classes (group-scale-up-rollback design, lifecycle table).
///
/// `InService` and `Standby` serve; every `Terminating*`, `Terminated` and
/// `Detach*` state is leaving; everything else, including `Pending*`,
/// `Quarantined`, `ReplacingRootVolume*` and `Warmed:*`, is on its way in.
[[nodiscard]] inline auto rollback_state(std::string_view lifecycle)
    -> group_rollback::member_state {
    if (lifecycle == "InService" || lifecycle == "Standby") {
        return group_rollback::member_state::live;
    }
    if (lifecycle.starts_with("Terminat") || lifecycle.starts_with("Detach") ||
        lifecycle.starts_with("Warmed:Terminat")) {
        return group_rollback::member_state::terminal;
    }
    return group_rollback::member_state::pending;
}

/// True for the exception names AWS uses when the caller's policy lacks an
/// action, as opposed to a transient or input error.
[[nodiscard]] inline auto is_permission_error(std::string_view exception_name) -> bool {
    return exception_name == "AccessDenied" || exception_name == "AccessDeniedException" ||
           exception_name == "UnauthorizedOperation" || exception_name == "UnauthorizedAccess";
}

/// True when Auto Scaling refused a call only because the group is busy, so
/// the same call can succeed once the current scaling activity ends.
[[nodiscard]] inline auto is_group_busy(std::string_view exception_name) -> bool {
    return exception_name.starts_with("ScalingActivityInProgress") ||
           exception_name.starts_with("ResourceContention");
}

}  // namespace aws_asg_detail

// ============================================================================
// aws_asg_quorum_manager_config
// ============================================================================

/// @brief Configuration for `aws_asg_quorum_manager`.
struct aws_asg_quorum_manager_config {
    /// Logical cluster name; used as a tag on provisioned instances.
    std::string cluster_name;
    /// Maps each topology `group_id` to the ASG name responsible for that group.
    std::map<std::string, std::string> asg_by_group;
    /// TCP port on which each Raft node listens; written into the returned address.
    std::uint16_t node_port{7000};
    /// Target node counts per placement group.
    desired_topology<std::string> topology;
    /// Maximum time to wait for a newly launched instance to become `InService`.
    std::chrono::seconds provision_timeout{120};
    /// Sleep interval between ASG poll iterations during provisioning.
    std::chrono::seconds poll_interval{5};
    /// AWS client settings (region, endpoint override, credentials, timeout).
    aws_client_config aws;
};

// ============================================================================
// aws_asg_quorum_manager
// ============================================================================

/// @brief `quorum_manager` implementation that provisions and monitors Raft nodes through AWS ASGs.
///
/// Liveness is determined via EC2 `DescribeInstanceStatus` (instance state == `running`),
/// consistent with how the ASG's own EC2 health-check type works.  The constructor validates
/// that every configured ASG uses EC2 health checks; it throws if any ASG uses ELB checks.
///
/// Node identity follows `aws_ec2_quorum_manager` and shares its id mapping: with
/// `aws_ec2_node_id` (or `std::string` holding its canonical text) the node id is the
/// instance the ASG launched; with an unsigned integer it is allocated when the new
/// instance is adopted, written to `kythira:node-id`, and looked up by that tag.
///
/// @tparam NodeId  Node identifier type; defaults to `uint64_t`. An unsigned integer,
///                 `std::string` or `aws_ec2_node_id`.
/// @tparam Address Network address type; defaults to `std::string`.
template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class aws_asg_quorum_manager {
    using ec2_mgr_t = aws_ec2_quorum_manager<NodeId, Address>;
    static constexpr bool instance_is_node_id = ec2_mgr_t::instance_is_node_id;

public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

    /// @brief Constructs the manager and validates the configuration.
    ///
    /// Verifies that `cluster_name` is non-empty, every topology group has a
    /// corresponding ASG entry, and every configured ASG uses EC2 health checks.
    /// Then protects from scale-in every adopted member of this cluster that
    /// is not protected yet (group-scale-up-rollback Requirement 4.3).
    ///
    /// @throws std::invalid_argument if any validation fails, including when
    ///         the caller lacks `autoscaling:SetInstanceProtection`.
    explicit aws_asg_quorum_manager(aws_asg_quorum_manager_config cfg) : _cfg(std::move(cfg)) {
        validate_config();
        Aws::Client::ClientConfiguration client_cfg;
        if (!_cfg.aws.region.empty()) {
            client_cfg.region = _cfg.aws.region;
        }
        if (!_cfg.aws.endpoint_override.empty()) {
            client_cfg.endpointOverride = _cfg.aws.endpoint_override;
        }
        auto ms = static_cast<long>(_cfg.aws.api_timeout.count() * 1000);
        client_cfg.requestTimeoutMs = ms;
        client_cfg.connectTimeoutMs = ms;
        if (_cfg.aws.credentials_provider) {
            _asg = std::make_shared<Aws::AutoScaling::AutoScalingClient>(
                _cfg.aws.credentials_provider, client_cfg);
            _ec2 = std::make_shared<Aws::EC2::EC2Client>(_cfg.aws.credentials_provider, client_cfg);
        } else {
            _asg = std::make_shared<Aws::AutoScaling::AutoScalingClient>(client_cfg);
            _ec2 = std::make_shared<Aws::EC2::EC2Client>(client_cfg);
        }
        validate_groups();
    }

    /// @brief Constructs the manager over caller-supplied clients instead of
    ///        ones built from `cfg.aws`.
    ///
    /// The seam the mock tests use: client subclasses that override the
    /// virtual operations stand in for Auto Scaling and EC2. Validates exactly
    /// as the other constructor does.
    aws_asg_quorum_manager(aws_asg_quorum_manager_config cfg,
                           std::shared_ptr<Aws::AutoScaling::AutoScalingClient> asg,
                           std::shared_ptr<Aws::EC2::EC2Client> ec2)
        : _cfg(std::move(cfg)), _asg(std::move(asg)), _ec2(std::move(ec2)) {
        validate_config();
        if (!_asg || !_ec2) {
            throw std::invalid_argument("aws_asg_quorum_manager: clients must be non-null");
        }
        validate_groups();
    }

    /// @brief Assesses cluster health from EC2 instance state.
    ///
    /// A node is live iff its instance state is `running`. A node with no instance,
    /// or whose id names another region, is unreachable.
    ///
    /// @param cluster Full cluster membership with placement-group annotations.
    /// @return Future containing the health report, or an exceptional Future on API error.
    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/asg/describe_instance_status",
                      throw std::runtime_error("fault: raft/aws/asg/describe_instance_status"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            std::vector<NodeId> ids;
            ids.reserve(cluster.size());
            for (const auto& np : cluster) {
                ids.push_back(np.node_id);
                raise_id_floor(np.node_id);
            }
            return build_health(
                cluster, ec2_mgr_t::live_by_node(*_ec2, _cfg.aws.region, _cfg.cluster_name, ids));
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<
                quorum_health<NodeId, std::string>>(std::make_exception_ptr(std::runtime_error(
                std::string("aws_asg_quorum_manager::assess_quorum: ") + ex.what())));
        }
    }

    /// @brief Assesses quorum, decommissions unreachable nodes via the ASG, and
    ///        provisions replacements to meet the desired topology.
    ///
    /// Decommission and provision errors are logged to stderr but do not abort the
    /// operation.  The returned health reflects the pre-maintenance cluster state.
    ///
    /// @param cluster Full cluster membership with placement-group annotations.
    /// @return Future containing the pre-maintenance health report.
    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/asg/maintain_quorum",
                      throw std::runtime_error("fault: raft/aws/asg/maintain_quorum"););
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
            std::string grp;
            for (const auto& np : cluster) {
                if (np.node_id == nid) {
                    grp = np.group_id;
                    break;
                }
            }
            try {
                std::move(decommission_node(nid)).get();
                last_replaced[grp] = nid;
            } catch (const std::exception& ex) {
                std::cerr << "[aws_asg_quorum_manager::maintain_quorum] decommission of "
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
            auto deficit =
                static_cast<std::ptrdiff_t>(gt.target_count) - static_cast<std::ptrdiff_t>(live);
            for (std::ptrdiff_t i = 0; i < deficit; ++i) {
                std::optional<NodeId> hint;
                if (auto it = last_replaced.find(gt.group_id); it != last_replaced.end()) {
                    hint = it->second;
                }
                try {
                    std::move(provision_node(gt.group_id, hint)).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[aws_asg_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return future_factory_default::makeFuture(std::move(pre_health));
    }

    /// @brief Provisions a new Raft node by incrementing the ASG desired capacity.
    ///
    /// Waits up to `provision_timeout` for an instance that was not in the
    /// group before the increment, in any lifecycle state, to reach `InService`
    /// and obtain a private IP address. The adopted instance is tagged and
    /// protected from scale-in, so the group's own scale-in never picks a
    /// voter.
    ///
    /// On timeout the increment is undone without letting Auto Scaling choose
    /// a victim (group-scale-up-rollback Requirements 2-3): every instance the
    /// increment launched is terminated by id with
    /// `ShouldDecrementDesiredCapacity`, and the desired capacity is restored
    /// by a write only when none was launched. The error ends with what the
    /// rollback did, e.g. `rollback: removed i-0abc (fresh, Pending)`.
    ///
    /// The `replacing` hint is accepted but unused; the new instance always gets a
    /// fresh EC2 ID and therefore a new `NodeId` (in numeric mode, one allocated
    /// from the cluster's `kythira:node-id` tags once the instance is found).
    ///
    /// @param target_group Placement-group key in `asg_by_group`.
    /// @param replacing    Ignored; present for interface compatibility.
    /// @return Future with the new node's identity and address on success.
    auto provision_node(std::string target_group, [[maybe_unused]] std::optional<NodeId> replacing)
        -> kythira::future_default<peer_info<NodeId, Address>> {
        try {
            fiu_do_on("raft/aws/asg/update_asg",
                      throw std::runtime_error("fault: raft/aws/asg/update_asg"););

            auto ait = _cfg.asg_by_group.find(target_group);
            if (ait == _cfg.asg_by_group.end()) {
                throw std::invalid_argument("aws_asg_quorum_manager: no ASG for group: " +
                                            target_group);
            }
            const std::string& asg_name = ait->second;

            Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest desc_req;
            desc_req.AddAutoScalingGroupNames(asg_name);
            auto desc = _asg->DescribeAutoScalingGroups(desc_req);
            if (!desc.IsSuccess()) {
                throw std::runtime_error("DescribeAutoScalingGroups: " +
                                         std::string(desc.GetError().GetMessage()));
            }
            const auto& asg_groups = desc.GetResult().GetAutoScalingGroups();
            if (asg_groups.empty()) {
                throw std::runtime_error("ASG not found: " + asg_name);
            }
            int orig_cap = asg_groups[0].GetDesiredCapacity();

            // Every member, whatever its lifecycle: a pre-existing `Pending`
            // instance that reaches `InService` during the wait is not the
            // one this increment launched, and must be neither adopted nor
            // terminated by the rollback.
            std::vector<std::string> pre_growth;
            for (const auto& inst : asg_groups[0].GetInstances()) {
                pre_growth.emplace_back(inst.GetInstanceId());
            }

            Aws::AutoScaling::Model::UpdateAutoScalingGroupRequest upd_req;
            upd_req.SetAutoScalingGroupName(asg_name);
            upd_req.SetDesiredCapacity(orig_cap + 1);
            auto upd = _asg->UpdateAutoScalingGroup(upd_req);
            if (!upd.IsSuccess()) {
                throw std::runtime_error("UpdateAutoScalingGroup: " +
                                         std::string(upd.GetError().GetMessage()));
            }

            std::string new_ec2_id;
            std::string private_ip;
            auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;

            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest poll_req;
                poll_req.AddAutoScalingGroupNames(asg_name);
                auto poll = _asg->DescribeAutoScalingGroups(poll_req);
                if (!poll.IsSuccess()) {
                    continue;
                }
                const auto& pgroups = poll.GetResult().GetAutoScalingGroups();
                if (pgroups.empty()) {
                    continue;
                }
                for (const auto& inst : pgroups[0].GetInstances()) {
                    if (inst.GetLifecycleState() !=
                        Aws::AutoScaling::Model::LifecycleState::InService) {
                        continue;
                    }
                    std::string iid(inst.GetInstanceId());
                    if (std::ranges::find(pre_growth, iid) != pre_growth.end()) {
                        continue;
                    }
                    // Use DescribeInstances to get the private IP for provisioning
                    // (this is not a liveness determination call).
                    Aws::EC2::Model::DescribeInstancesRequest ec2_req;
                    ec2_req.AddInstanceIds(iid);
                    auto ec2 = _ec2->DescribeInstances(ec2_req);
                    if (!ec2.IsSuccess()) {
                        continue;
                    }
                    const auto& res = ec2.GetResult().GetReservations();
                    if (res.empty() || res[0].GetInstances().empty()) {
                        continue;
                    }
                    const std::string& pip =
                        std::string(res[0].GetInstances()[0].GetPrivateIpAddress());
                    if (pip.empty()) {
                        continue;
                    }
                    new_ec2_id = iid;
                    private_ip = pip;
                    break;
                }
                if (!new_ec2_id.empty()) {
                    break;
                }
            }

            if (new_ec2_id.empty()) {
                std::vector<group_rollback::listed_member> final_listing;
                try {
                    final_listing = rollback_listing(describe_group(asg_name));
                } catch (const std::exception&) {
                    // An empty listing plans a restore: with nothing known to
                    // be fresh there is nothing to terminate by id.
                }
                const auto rollback =
                    undo_scale_up(asg_name, pre_growth, std::move(final_listing), orig_cap);
                throw std::runtime_error("asg provision timeout for group: " + target_group + "; " +
                                         group_rollback::describe(rollback, orig_cap + 1));
            }

            NodeId new_id{};
            if constexpr (instance_is_node_id) {
                auto from_instance = ec2_mgr_t::node_id_for_instance(_cfg.aws.region, new_ec2_id);
                if (!from_instance) {
                    throw std::runtime_error(
                        "ASG launched an instance whose id is not a valid "
                        "EC2 id in " +
                        _cfg.aws.region + ": '" + new_ec2_id + "'");
                }
                new_id = std::move(*from_instance);
            } else {
                new_id = ec2_mgr_t::allocate_numeric_node_id(*_ec2, _cfg.cluster_name,
                                                             _id_floor->load());
                raise_id_floor(new_id);
            }
            apply_tags(new_ec2_id, new_id, target_group);
            // Not fatal: the node is up and tagged, and the constructor's
            // reconcile protects it on the next start. Failing the provision
            // here would orphan a running, tagged instance.
            if (const auto [name, message] = set_scale_in_protection(asg_name, {new_ec2_id});
                !name.empty()) {
                std::cerr << "[aws_asg_quorum_manager::provision_node] SetInstanceProtection for "
                          << new_ec2_id << " failed: " << name << ": " << message << "\n";
            }
            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return future_factory_default::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("aws_asg_quorum_manager::provision_node: ") + ex.what())));
        }
    }

    /// @brief True when Auto Scaling no longer counts `ec2_id` as a live member
    ///        of any group: it is in none, or its lifecycle is already
    ///        `Terminating`/`Terminated`.  False when it is still a member or the
    ///        answer could not be read, so the caller reports its own error.
    [[nodiscard]] auto already_out_of_group(const std::string& ec2_id) const -> bool {
        Aws::AutoScaling::Model::DescribeAutoScalingInstancesRequest req;
        req.AddInstanceIds(ec2_id);
        auto out = _asg->DescribeAutoScalingInstances(req);
        if (!out.IsSuccess()) {
            return false;
        }
        const auto& members = out.GetResult().GetAutoScalingInstances();
        return std::ranges::all_of(members, [](const auto& m) {
            return std::string_view{m.GetLifecycleState()}.starts_with("Terminat");
        });
    }

    /// @brief Terminates a Raft node via `TerminateInstanceInAutoScalingGroup`.
    ///
    /// Sets `ShouldDecrementDesiredCapacity = true` so the ASG does not launch a
    /// replacement automatically.  A failed call is idempotent success when
    /// `DescribeAutoScalingInstances` shows the instance in no group, or already
    /// `Terminating`/`Terminated`; any other failure is returned.  Polls until the EC2 state leaves
    /// `running` (up to 30 s); success means that was observed, or EC2 no longer knows the
    /// instance.
    ///
    /// Scale-in protection does not block this call, so a protected node is
    /// terminated without clearing its protection first.
    ///
    /// @param node_id Identifier of the node to terminate.
    /// @return void Future on success, exceptional Future on API error or when the
    ///         instance is still `running` (or unobservable) when the wait expires.
    auto decommission_node(const NodeId& node_id) -> kythira::future_default<void> {
        try {
            fiu_do_on("raft/aws/asg/terminate_instance",
                      throw std::runtime_error("fault: raft/aws/asg/terminate_instance"););

            auto found = instance_id_of(node_id);
            if (!found) {
                if constexpr (instance_is_node_id) {
                    throw std::invalid_argument("node " + node_id_str(node_id) +
                                                " is not an EC2 instance in " + _cfg.aws.region);
                } else {
                    // No instance carries this id any more: already gone.
                    return future_factory_default::makeFuture();
                }
            }
            const std::string ec2_id = *found;
            Aws::AutoScaling::Model::TerminateInstanceInAutoScalingGroupRequest req;
            req.SetInstanceId(ec2_id);
            req.SetShouldDecrementDesiredCapacity(true);
            auto outcome = _asg->TerminateInstanceInAutoScalingGroup(req);
            if (!outcome.IsSuccess()) {
                // Whether the call failed because there is nothing left to
                // terminate is decided by asking Auto Scaling, not by reading
                // the error: an instance outside every group is reported as a
                // ValidationError, an AccessDenied (a role scoped to group
                // ARNs cannot be authorised against a group that does not
                // exist) or another text depending on the caller's policy,
                // while a ValidationError is also what a real refusal such as
                // dropping below MinSize looks like.
                const auto& err = outcome.GetError();
                if (already_out_of_group(ec2_id)) {
                    return future_factory_default::makeFuture();
                }
                std::string msg = "TerminateInstanceInAutoScalingGroup: ";
                msg += err.GetExceptionName();
                msg += ": ";
                msg += err.GetMessage();
                throw std::runtime_error(msg);
            }
            // Poll until the EC2 state confirms the transition away from running.
            // Success is returned only once that is observed: an expired wait, or
            // one that never got a readable answer, is a failure the caller must
            // see rather than a removal it assumes (Requirement 7.1 of the
            // aws-asg-real-cloud-tests spec).
            {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
                std::string last_seen = "no DescribeInstanceStatus answer";
                while (true) {
                    Aws::EC2::Model::DescribeInstanceStatusRequest poll;
                    poll.AddInstanceIds(ec2_id);
                    poll.SetIncludeAllInstances(true);
                    auto ps = _ec2->DescribeInstanceStatus(poll);
                    if (ps.IsSuccess()) {
                        const auto& sv = ps.GetResult().GetInstanceStatuses();
                        if (sv.empty() || sv[0].GetInstanceState().GetName() !=
                                              Aws::EC2::Model::InstanceStateName::running) {
                            break;
                        }
                        last_seen = "state running";
                    } else if (std::string(ps.GetError().GetExceptionName()) ==
                               "InvalidInstanceID.NotFound") {
                        break;
                    } else {
                        last_seen =
                            "DescribeInstanceStatus: " + std::string(ps.GetError().GetMessage());
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        std::string msg = ec2_id;
                        msg += " not confirmed out of running 30s after termination (last: ";
                        msg += last_seen;
                        msg += ")";
                        throw std::runtime_error(msg);
                    }
                    std::this_thread::sleep_for(std::chrono::seconds{2});
                }
            }
            return future_factory_default::makeFuture();
        } catch (const std::exception& ex) {
            return future_factory_default::makeExceptionalFuture<void>(
                std::make_exception_ptr(std::runtime_error(
                    std::string("aws_asg_quorum_manager::decommission_node: ") + ex.what())));
        }
    }

    /// @brief Returns the desired topology from the configuration.
    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

    /// @brief The instance a node runs on, or nullopt when there is none; see
    ///        `aws_ec2_quorum_manager::instance_id_of`.
    [[nodiscard]] auto instance_id_of(const NodeId& nid) const -> std::optional<std::string> {
        if constexpr (instance_is_node_id) {
            return ec2_mgr_t::instance_id_for_node(_cfg.aws.region, nid);
        } else {
            auto found = ec2_mgr_t::find_numeric_instances(*_ec2, _cfg.cluster_name, {nid});
            auto it = found.find(node_id_str(nid));
            if (it == found.end() || it->second.empty()) {
                return std::nullopt;
            }
            return ec2_mgr_t::preferred_instance(it->second).instance_id;
        }
    }

    /// @brief The node id of an instance, or nullopt when it is not one of this
    ///        cluster's nodes; see `aws_ec2_quorum_manager::node_id_of_instance`.
    [[nodiscard]] auto node_id_of_instance(const std::string& ec2_id) const
        -> std::optional<NodeId> {
        if constexpr (instance_is_node_id) {
            return ec2_mgr_t::node_id_for_instance(_cfg.aws.region, ec2_id);
        } else {
            return ec2_mgr_t::numeric_node_id_of_instance(*_ec2, _cfg.cluster_name, ec2_id);
        }
    }

private:
    aws_asg_quorum_manager_config _cfg;
    std::shared_ptr<Aws::AutoScaling::AutoScalingClient> _asg;
    std::shared_ptr<Aws::EC2::EC2Client> _ec2;
    /// Numeric mode: the highest id allocated or assessed; see
    /// `aws_ec2_quorum_manager::allocate_numeric_node_id`.
    std::shared_ptr<std::atomic<std::uint64_t>> _id_floor =
        std::make_shared<std::atomic<std::uint64_t>>(0);

    void raise_id_floor(const NodeId& nid) {
        if constexpr (!instance_is_node_id) {
            auto v = static_cast<std::uint64_t>(nid);
            auto cur = _id_floor->load();
            while (cur < v && !_id_floor->compare_exchange_weak(cur, v)) {
            }
        }
    }

    static auto node_id_str(const NodeId& id) -> std::string {
        return node_id_traits<NodeId>::to_text(id);
    }

    /// The checks that need no AWS call.
    void validate_config() const {
        if (_cfg.cluster_name.empty()) {
            throw std::invalid_argument("aws_asg_quorum_manager: cluster_name must be non-empty");
        }
        if (_cfg.asg_by_group.empty()) {
            throw std::invalid_argument("aws_asg_quorum_manager: asg_by_group must be non-empty");
        }
        if (_cfg.node_port == 0) {
            throw std::invalid_argument("aws_asg_quorum_manager: node_port must be non-zero");
        }
        for (const auto& gt : _cfg.topology.groups) {
            if (_cfg.asg_by_group.find(gt.group_id) == _cfg.asg_by_group.end()) {
                throw std::invalid_argument(
                    "aws_asg_quorum_manager: no ASG configured for group: " + gt.group_id);
            }
        }
        if constexpr (instance_is_node_id) {
            if (!aws_ec2_rules::valid_region(_cfg.aws.region)) {
                throw std::invalid_argument(
                    "aws_asg_quorum_manager: aws.region must name a region when the node id is "
                    "the instance (got '" +
                    _cfg.aws.region + "')");
            }
        }
    }

    /// One `DescribeAutoScalingGroups` read of every configured group: the
    /// health-check type check, then the scale-in protection reconcile.
    void validate_groups() const {
        // kythira determines liveness via DescribeInstanceStatus (instance state ==
        // running). The ASG must use the same signal — EC2 health checks — so that
        // the ASG and kythira agree on which instances are healthy and the ASG does
        // not replace instances that kythira still considers live.
        bool validate_hc = true;
        fiu_do_on("raft/aws/asg/skip_health_check_validation", validate_hc = false;);
        if (!validate_hc) {
            return;
        }
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest chk;
        for (const auto& kv : _cfg.asg_by_group) {
            chk.AddAutoScalingGroupNames(kv.second);
        }
        auto out = _asg->DescribeAutoScalingGroups(chk);
        if (!out.IsSuccess()) {
            throw std::invalid_argument(
                "aws_asg_quorum_manager: DescribeAutoScalingGroups failed: " +
                std::string(out.GetError().GetMessage()));
        }
        for (const auto& asg : out.GetResult().GetAutoScalingGroups()) {
            if (std::string(asg.GetHealthCheckType()) != "EC2") {
                throw std::invalid_argument(
                    "aws_asg_quorum_manager: ASG '" + std::string(asg.GetAutoScalingGroupName()) +
                    "' uses health check type '" + std::string(asg.GetHealthCheckType()) +
                    "'; kythira requires EC2 health checks");
            }
        }
        for (const auto& asg : out.GetResult().GetAutoScalingGroups()) {
            reconcile_scale_in_protection(asg);
        }
    }

    /// @brief Protect every adopted member of this cluster in @p asg that is
    ///        not yet protected (group-scale-up-rollback Requirement 4.3).
    ///
    /// Covers clusters adopted before protection existed, and repairs an
    /// adoption whose protection call failed. Adopted means the EC2 instance
    /// carries this cluster's `kythira:cluster` and a `kythira:node-id` tag;
    /// Auto Scaling's own listing carries no EC2 tags, hence the extra read,
    /// made only when some member is unprotected. A permission error throws
    /// `std::invalid_argument`, like the other configuration checks; anything
    /// else is logged, because a transient read failure at startup is not a
    /// reason to refuse to run.
    void reconcile_scale_in_protection(const Aws::AutoScaling::Model::AutoScalingGroup& asg) const {
        const std::string asg_name(asg.GetAutoScalingGroupName());
        std::vector<std::string> unprotected;
        for (const auto& inst : asg.GetInstances()) {
            if (!inst.GetProtectedFromScaleIn() &&
                inst.GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService) {
                unprotected.emplace_back(inst.GetInstanceId());
            }
        }
        if (unprotected.empty()) {
            return;
        }
        Aws::EC2::Model::DescribeInstancesRequest req;
        for (const auto& id : unprotected) {
            req.AddInstanceIds(id);
        }
        auto ec2 = _ec2->DescribeInstances(req);
        if (!ec2.IsSuccess()) {
            std::cerr << "[aws_asg_quorum_manager] scale-in protection reconcile for " << asg_name
                      << " skipped: DescribeInstances: " << ec2.GetError().GetMessage() << "\n";
            return;
        }
        std::vector<std::string> adopted;
        for (const auto& reservation : ec2.GetResult().GetReservations()) {
            for (const auto& inst : reservation.GetInstances()) {
                if (find_tag(inst.GetTags(), "kythira:cluster") == _cfg.cluster_name &&
                    find_tag(inst.GetTags(), "kythira:node-id").has_value()) {
                    adopted.emplace_back(inst.GetInstanceId());
                }
            }
        }
        const auto [name, message] = set_scale_in_protection(asg_name, adopted);
        if (name.empty()) {
            return;
        }
        if (aws_asg_detail::is_permission_error(name)) {
            throw std::invalid_argument("aws_asg_quorum_manager: SetInstanceProtection on ASG '" +
                                        asg_name + "' was refused (" + name + ": " + message +
                                        "); the role needs autoscaling:SetInstanceProtection");
        }
        std::cerr << "[aws_asg_quorum_manager] scale-in protection reconcile for " << asg_name
                  << " failed: " << name << ": " << message << "\n";
    }

    /// @brief `SetInstanceProtection(ProtectedFromScaleIn=true)`, in batches
    ///        of the API's 50-id limit.
    ///
    /// @return The first failure's `(exception name, message)`, or two empty
    ///         strings when every batch succeeded.
    [[nodiscard]] auto set_scale_in_protection(const std::string& asg_name,
                                               const std::vector<std::string>& ids) const
        -> std::pair<std::string, std::string> {
        for (std::size_t start = 0; start < ids.size(); start += aws_asg_detail::protection_batch) {
            Aws::AutoScaling::Model::SetInstanceProtectionRequest req;
            req.SetAutoScalingGroupName(asg_name);
            req.SetProtectedFromScaleIn(true);
            const auto stop = std::min(start + aws_asg_detail::protection_batch, ids.size());
            for (std::size_t i = start; i < stop; ++i) {
                req.AddInstanceIds(ids[i]);
            }
            auto out = _asg->SetInstanceProtection(req);
            if (!out.IsSuccess()) {
                return {std::string(out.GetError().GetExceptionName()),
                        std::string(out.GetError().GetMessage())};
            }
        }
        return {};
    }

    /// The members of @p asg_name. Throws when the read fails or the group
    /// is missing, so a caller never mistakes a failed read for an empty group.
    [[nodiscard]] auto describe_group(const std::string& asg_name) const
        -> Aws::Vector<Aws::AutoScaling::Model::Instance> {
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest req;
        req.AddAutoScalingGroupNames(asg_name);
        auto out = _asg->DescribeAutoScalingGroups(req);
        if (!out.IsSuccess()) {
            throw std::runtime_error("DescribeAutoScalingGroups: " +
                                     std::string(out.GetError().GetMessage()));
        }
        const auto& groups = out.GetResult().GetAutoScalingGroups();
        if (groups.empty()) {
            throw std::runtime_error("ASG not found: " + asg_name);
        }
        return groups[0].GetInstances();
    }

    [[nodiscard]] static auto rollback_listing(
        const Aws::Vector<Aws::AutoScaling::Model::Instance>& instances)
        -> std::vector<group_rollback::listed_member> {
        std::vector<group_rollback::listed_member> out;
        out.reserve(instances.size());
        for (const auto& inst : instances) {
            std::string lifecycle(
                Aws::AutoScaling::Model::LifecycleStateMapper::GetNameForLifecycleState(
                    inst.GetLifecycleState()));
            auto state = aws_asg_detail::rollback_state(lifecycle);
            out.push_back({.id = std::string(inst.GetInstanceId()),
                           .state = state,
                           .lifecycle = std::move(lifecycle),
                           .node = {}});
        }
        return out;
    }

    /// @brief Terminate one instance by id and decrement the desired capacity.
    ///
    /// Retries while Auto Scaling refuses the call because a scaling activity
    /// is in progress, which a timed-out launch usually still is, for up to
    /// `provision_timeout`. An instance no longer in any group counts as
    /// removed.
    ///
    /// @return The error text, or empty on success.
    [[nodiscard]] auto try_terminate(const std::string& ec2_id) const -> std::string {
        const auto deadline = std::chrono::steady_clock::now() + _cfg.provision_timeout;
        for (;;) {
            Aws::AutoScaling::Model::TerminateInstanceInAutoScalingGroupRequest req;
            req.SetInstanceId(ec2_id);
            req.SetShouldDecrementDesiredCapacity(true);
            auto out = _asg->TerminateInstanceInAutoScalingGroup(req);
            if (out.IsSuccess()) {
                return {};
            }
            const std::string name(out.GetError().GetExceptionName());
            if (aws_asg_detail::is_group_busy(name) &&
                std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(_cfg.poll_interval);
                continue;
            }
            if (already_out_of_group(ec2_id)) {
                return {};
            }
            return name + ": " + std::string(out.GetError().GetMessage());
        }
    }

    /// Undo a timed-out increment of @p asg_name; see `provision_node`.
    [[nodiscard]] auto undo_scale_up(const std::string& asg_name,
                                     const std::vector<std::string>& pre_growth,
                                     std::vector<group_rollback::listed_member> final_listing,
                                     int orig_cap) const noexcept
        -> group_rollback::rollback_outcome {
        try {
            const std::chrono::milliseconds poll = _cfg.poll_interval;
            return group_rollback::execute_rollback(
                pre_growth, std::move(final_listing), orig_cap,
                [this](const std::string& id) { return try_terminate(id); },
                [this, &asg_name](std::int64_t size) {
                    Aws::AutoScaling::Model::UpdateAutoScalingGroupRequest req;
                    req.SetAutoScalingGroupName(asg_name);
                    req.SetDesiredCapacity(static_cast<int>(size));
                    auto out = _asg->UpdateAutoScalingGroup(req);
                    if (!out.IsSuccess()) {
                        throw std::runtime_error(std::string(out.GetError().GetExceptionName()) +
                                                 ": " + std::string(out.GetError().GetMessage()));
                    }
                },
                [this, &asg_name] { return rollback_listing(describe_group(asg_name)); },
                group_rollback::settle_window(_cfg.provision_timeout, poll), poll);
        } catch (const std::exception& ex) {
            group_rollback::rollback_outcome outcome;
            outcome.restore_error = std::string("rollback aborted: ") + ex.what();
            return outcome;
        }
    }

    static auto find_tag(const Aws::Vector<Aws::EC2::Model::Tag>& tags, const std::string& key)
        -> std::optional<std::string> {
        for (const auto& tag : tags) {
            if (std::string(tag.GetKey()) == key) {
                return std::string(tag.GetValue());
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                                    const std::map<std::string, bool>& live_map) const
        -> kythira::future_default<quorum_health<NodeId, std::string>> {
        std::vector<NodeId> unreachable;
        std::size_t live_count = 0;
        std::map<std::string, std::size_t> group_live;
        for (const auto& np : cluster) {
            auto key = node_id_str(np.node_id);
            auto it = live_map.find(key);
            bool is_live = (it != live_map.end() && it->second);
            if (is_live) {
                ++live_count;
                group_live[np.group_id]++;
            } else {
                unreachable.push_back(np.node_id);
            }
        }

        std::vector<placement_group_health<NodeId, std::string>> groups;
        for (const auto& gt : _cfg.topology.groups) {
            std::size_t gl = 0;
            if (auto it = group_live.find(gt.group_id); it != group_live.end()) {
                gl = it->second;
            }
            std::vector<NodeId> g_unreach;
            for (const auto& nid : unreachable) {
                for (const auto& np : cluster) {
                    if (np.node_id == nid && np.group_id == gt.group_id) {
                        g_unreach.push_back(nid);
                    }
                }
            }
            groups.push_back({.group_id = gt.group_id,
                              .live_count = gl,
                              .target_count = gt.target_count,
                              .unreachable_nodes = std::move(g_unreach)});
        }

        std::size_t total = cluster.size();
        return future_factory_default::makeFuture(quorum_health<NodeId, std::string>{
            .status = compute_status(live_count, total),
            .live_node_count = live_count,
            .total_node_count = total,
            .unreachable_nodes = std::move(unreachable),
            .groups = std::move(groups),
        });
    }

    static auto compute_status(std::size_t live, std::size_t total) -> quorum_status {
        if (total == 0) {
            return quorum_status::healthy;
        }
        std::size_t majority = total / 2 + 1;
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

    void apply_tags(const std::string& ec2_id, const NodeId& nid, const std::string& group) {
        auto make_tag = [](const std::string& k, const std::string& v) {
            Aws::EC2::Model::Tag t;
            t.SetKey(k);
            t.SetValue(v);
            return t;
        };
        Aws::EC2::Model::CreateTagsRequest req;
        req.AddResources(ec2_id);
        std::string name_suffix = node_id_str(nid);
        if constexpr (instance_is_node_id) {
            name_suffix = ec2_id;  // canonical text holds ':' and repeats the instance id
        }
        req.AddTags(make_tag("Name", "kythira-" + _cfg.cluster_name + "-" + name_suffix));
        req.AddTags(make_tag("kythira:cluster", _cfg.cluster_name));
        req.AddTags(make_tag("kythira:node-id", node_id_str(nid)));
        req.AddTags(make_tag("kythira:group", group));
        req.AddTags(make_tag("kythira:managed-by", "asg_quorum_manager"));
        _ec2->CreateTags(req);
    }
};

static_assert(quorum_manager<aws_asg_quorum_manager<std::uint64_t, std::string>, std::uint64_t,
                             std::string, std::string>,
              "aws_asg_quorum_manager must satisfy quorum_manager");
static_assert(quorum_manager<aws_asg_quorum_manager<aws_ec2_node_id, std::string>, aws_ec2_node_id,
                             std::string, std::string>,
              "aws_asg_quorum_manager<aws_ec2_node_id> must satisfy quorum_manager");

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK
