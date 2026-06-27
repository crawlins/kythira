#pragma once

#include <raft/aws_client_config.hpp>
#include <raft/aws_ec2_quorum_manager.hpp>
#include <raft/fault_injection.hpp>
#include <raft/quorum_management.hpp>

#ifdef KYTHIRA_HAS_AWS_SDK

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
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

struct aws_asg_quorum_manager_config {
    std::string cluster_name;
    std::map<std::string, std::string> asg_by_group;
    std::uint16_t node_port{7000};
    desired_topology<std::string> topology;
    std::chrono::seconds provision_timeout{120};
    std::chrono::seconds poll_interval{5};
    aws_client_config aws;
};

// ============================================================================
// aws_asg_quorum_manager
// ============================================================================

template<typename NodeId = std::uint64_t, typename Address = std::string>
requires node_id<NodeId>
class aws_asg_quorum_manager {
    using ec2_mgr_t = aws_ec2_quorum_manager<NodeId, Address>;

public:
    using node_id_type = NodeId;
    using address_type = Address;
    using placement_group_id_type = std::string;

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

    // ── assess_quorum ─────────────────────────────────────────────────────────
    // Liveness is determined via DescribeInstanceStatus, not DescribeInstances.
    // The node_id encodes the EC2 instance ID directly (ec2_id_to_node_id /
    // node_id_to_ec2_id from aws_ec2_quorum_manager), so no tag scan is needed.

    auto assess_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/asg/describe_instance_status",
                      throw std::runtime_error("fault: raft/aws/asg/describe_instance_status"););

            if (cluster.empty()) {
                return build_health(cluster, {});
            }

            Aws::EC2::Model::DescribeInstanceStatusRequest req;
            for (const auto& np : cluster) {
                req.AddInstanceIds(ec2_mgr_t::node_id_to_ec2_id(np.node_id));
            }
            req.SetIncludeAllInstances(true);

            auto outcome = _ec2->DescribeInstanceStatus(req);
            if (!outcome.IsSuccess()) {
                throw std::runtime_error("ec2 DescribeInstanceStatus: " +
                                         std::string(outcome.GetError().GetMessage()));
            }

            std::map<std::string, bool> live_map;
            for (const auto& status : outcome.GetResult().GetInstanceStatuses()) {
                bool running = (status.GetInstanceState().GetName() ==
                                Aws::EC2::Model::InstanceStateName::running);
                auto nid = ec2_mgr_t::ec2_id_to_node_id(std::string(status.GetInstanceId()));
                live_map[node_id_str(nid)] = running;
            }

            return build_health(cluster, live_map);
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::runtime_error(std::string("aws_asg_quorum_manager::assess_quorum: ") +
                                   ex.what()));
        }
    }

    // ── maintain_quorum ───────────────────────────────────────────────────────

    auto maintain_quorum(const std::vector<node_placement<NodeId, std::string>>& cluster)
        -> kythira::Future<quorum_health<NodeId, std::string>> {
        try {
            fiu_do_on("raft/aws/asg/maintain_quorum",
                      throw std::runtime_error("fault: raft/aws/asg/maintain_quorum"););
        } catch (...) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::current_exception());
        }

        quorum_health<NodeId, std::string> pre_health;
        try {
            pre_health = assess_quorum(cluster).get();
        } catch (...) {
            return FutureFactory::makeExceptionalFuture<quorum_health<NodeId, std::string>>(
                std::current_exception());
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
                decommission_node(nid).get();
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
                    provision_node(gt.group_id, hint).get();
                } catch (const std::exception& ex) {
                    std::cerr << "[aws_asg_quorum_manager::maintain_quorum] provision in "
                              << gt.group_id << " failed: " << ex.what() << "\n";
                }
            }
        }

        return FutureFactory::makeFuture(std::move(pre_health));
    }

    // ── provision_node ────────────────────────────────────────────────────────
    // The NodeId is derived from the new EC2 instance ID returned by the ASG.
    // DescribeInstances is used here only for provisioning (getting private IP),
    // not for liveness determination.

    auto provision_node(std::string target_group, std::optional<NodeId> /*replacing*/)
        -> kythira::Future<peer_info<NodeId, Address>> {
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

            NodeId new_id = ec2_mgr_t::ec2_id_to_node_id(new_ec2_id);
            apply_tags(new_ec2_id, new_id, target_group);
            Address addr = static_cast<Address>(private_ip + ":" + std::to_string(_cfg.node_port));
            return FutureFactory::makeFuture(peer_info<NodeId, Address>{new_id, addr});
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<peer_info<NodeId, Address>>(
                std::runtime_error(std::string("aws_asg_quorum_manager::provision_node: ") +
                                   ex.what()));
        }
    }

    // ── decommission_node ─────────────────────────────────────────────────────
    // The EC2 instance ID is derived directly from node_id — no DescribeInstances
    // lookup is required.

    auto decommission_node(const NodeId& node_id) -> kythira::Future<void> {
        try {
            fiu_do_on("raft/aws/asg/terminate_instance",
                      throw std::runtime_error("fault: raft/aws/asg/terminate_instance"););

            std::string ec2_id = ec2_mgr_t::node_id_to_ec2_id(node_id);
            Aws::AutoScaling::Model::TerminateInstanceInAutoScalingGroupRequest req;
            req.SetInstanceId(ec2_id);
            req.SetShouldDecrementDesiredCapacity(true);
            auto outcome = _asg->TerminateInstanceInAutoScalingGroup(req);
            if (!outcome.IsSuccess()) {
                const auto& err = outcome.GetError();
                // Instance already terminated or not found → treat as success.
                if (std::string(err.GetMessage()).find("not found") != std::string::npos ||
                    std::string(err.GetExceptionName()).find("ValidationError") !=
                        std::string::npos) {
                    return FutureFactory::makeFuture();
                }
                throw std::runtime_error("TerminateInstanceInAutoScalingGroup: " +
                                         std::string(err.GetMessage()));
            }
            // Poll until the EC2 state confirms the transition away from running.
            {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
                while (std::chrono::steady_clock::now() < deadline) {
                    Aws::EC2::Model::DescribeInstanceStatusRequest poll;
                    poll.AddInstanceIds(ec2_id);
                    poll.SetIncludeAllInstances(true);
                    auto ps = _ec2->DescribeInstanceStatus(poll);
                    if (!ps.IsSuccess()) {
                        break;
                    }
                    const auto& sv = ps.GetResult().GetInstanceStatuses();
                    if (sv.empty()) {
                        break;
                    }
                    if (sv[0].GetInstanceState().GetName() !=
                        Aws::EC2::Model::InstanceStateName::running) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds{2});
                }
            }
            return FutureFactory::makeFuture();
        } catch (const std::exception& ex) {
            return FutureFactory::makeExceptionalFuture<void>(std::runtime_error(
                std::string("aws_asg_quorum_manager::decommission_node: ") + ex.what()));
        }
    }

    // ── topology ─────────────────────────────────────────────────────────────

    [[nodiscard]] auto topology() const -> desired_topology<std::string> { return _cfg.topology; }

private:
    aws_asg_quorum_manager_config _cfg;
    std::shared_ptr<Aws::AutoScaling::AutoScalingClient> _asg;
    std::shared_ptr<Aws::EC2::EC2Client> _ec2;

    static auto node_id_str(const NodeId& id) -> std::string {
        if constexpr (std::is_same_v<NodeId, std::string>) {
            return id;
        } else {
            return std::to_string(id);
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

    auto build_health(const std::vector<node_placement<NodeId, std::string>>& cluster,
                      const std::map<std::string, bool>& live_map) const
        -> kythira::Future<quorum_health<NodeId, std::string>> {
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
        return FutureFactory::makeFuture(quorum_health<NodeId, std::string>{
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
        req.AddTags(make_tag("Name", "kythira-" + _cfg.cluster_name + "-" + node_id_str(nid)));
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

}  // namespace kythira

#endif  // KYTHIRA_HAS_AWS_SDK
