// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aws_asg_quorum_manager.hpp
/// @brief Quorum manager that provisions and monitors Raft nodes through AWS Auto Scaling Groups.

#include <raft/aws_client_config.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/fault_injection.hpp>
#include <raft/future_default.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/autoscaling/model/DescribeAutoScalingInstancesRequest.h>
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
    ///
    /// @throws std::invalid_argument if any validation fails.
    /// @throws std::runtime_error if `DescribeAutoScalingGroups` fails.
    explicit aws_asg_quorum_manager(aws_asg_quorum_manager_config cfg) : _cfg(std::move(cfg)) {
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
        // kythira determines liveness via DescribeInstanceStatus (instance state ==
        // running). The ASG must use the same signal — EC2 health checks — so that
        // the ASG and kythira agree on which instances are healthy and the ASG does
        // not replace instances that kythira still considers live.
        bool validate_hc = true;
        fiu_do_on("raft/aws/asg/skip_health_check_validation", validate_hc = false;);
        if (validate_hc) {
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
                    throw std::invalid_argument("aws_asg_quorum_manager: ASG '" +
                                                std::string(asg.GetAutoScalingGroupName()) +
                                                "' uses health check type '" +
                                                std::string(asg.GetHealthCheckType()) +
                                                "'; kythira requires EC2 health checks");
                }
            }
        }
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
    /// Waits up to `provision_timeout` for the new instance to reach `InService`
    /// state and obtain a private IP address.  On timeout, the ASG capacity is
    /// restored to its original value before returning an exceptional Future.
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

            std::vector<std::string> existing_ids;
            for (const auto& inst : asg_groups[0].GetInstances()) {
                if (inst.GetLifecycleState() ==
                    Aws::AutoScaling::Model::LifecycleState::InService) {
                    existing_ids.emplace_back(inst.GetInstanceId());
                }
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
                    if (std::ranges::find(existing_ids, iid) != existing_ids.end()) {
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
                Aws::AutoScaling::Model::UpdateAutoScalingGroupRequest restore;
                restore.SetAutoScalingGroupName(asg_name);
                restore.SetDesiredCapacity(orig_cap);
                _asg->UpdateAutoScalingGroup(restore);
                throw std::runtime_error("asg provision timeout for group: " + target_group);
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
