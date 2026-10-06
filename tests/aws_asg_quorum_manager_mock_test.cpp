// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file aws_asg_quorum_manager_mock_test.cpp
/// @brief `aws_asg_quorum_manager`'s timeout rollback and scale-in protection
///        against in-process Auto Scaling and EC2 doubles
///        (`.kiro/specs/group-scale-up-rollback/`, task 6).
///
/// The doubles subclass the SDK clients and override the operations the
/// manager calls, so no network or credentials are involved. The group they
/// model chooses its own scale-in victim the way the old timeout path relied
/// on: a desired-capacity decrease terminates the **oldest** member that is
/// not protected from scale-in. That is what makes a blind shrink cost a
/// voter here, and what the targeted rollback has to avoid.

#define BOOST_TEST_MODULE aws_asg_quorum_manager_mock_test
#include <boost/test/unit_test.hpp>

#include <raft/aws_asg_quorum_manager.hpp>

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/AutoScalingErrors.h>
#include <aws/autoscaling/model/AutoScalingGroup.h>
#include <aws/autoscaling/model/AutoScalingInstanceDetails.h>
#include <aws/autoscaling/model/CompleteLifecycleActionRequest.h>
#include <aws/autoscaling/model/CompleteLifecycleActionResult.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsResult.h>
#include <aws/autoscaling/model/DescribeAutoScalingInstancesResult.h>
#include <aws/autoscaling/model/DescribeLifecycleHooksRequest.h>
#include <aws/autoscaling/model/DescribeLifecycleHooksResult.h>
#include <aws/autoscaling/model/Instance.h>
#include <aws/autoscaling/model/LifecycleHook.h>
#include <aws/autoscaling/model/LifecycleState.h>
#include <aws/autoscaling/model/SetInstanceProtectionResult.h>
#include <aws/autoscaling/model/TerminateInstanceInAutoScalingGroupResult.h>
#include <aws/core/Aws.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/EC2Errors.h>
#include <aws/ec2/model/DescribeInstanceStatusResponse.h>
#include <aws/ec2/model/DescribeInstancesResponse.h>
#include <aws/ec2/model/Instance.h>
#include <aws/ec2/model/Reservation.h>
#include <aws/ec2/model/Tag.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace as = Aws::AutoScaling;
namespace asm_ = Aws::AutoScaling::Model;
namespace em = Aws::EC2::Model;

struct SdkFixture {
    SdkFixture() {
        // A default ClientConfiguration may otherwise ask the instance
        // metadata service for a region, which is a network call.
        ::setenv("AWS_EC2_METADATA_DISABLED", "true", 1);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
#endif
        Aws::InitAPI(_opts);
    }
    ~SdkFixture() { Aws::ShutdownAPI(_opts); }
    SdkFixture(const SdkFixture&) = delete;
    SdkFixture& operator=(const SdkFixture&) = delete;
    SdkFixture(SdkFixture&&) = delete;
    SdkFixture& operator=(SdkFixture&&) = delete;

private:
    Aws::SDKOptions _opts;
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    std::unique_ptr<folly::Init> _init;
#endif
};

BOOST_TEST_GLOBAL_FIXTURE(SdkFixture);

auto client_config() -> Aws::Client::ClientConfiguration {
    Aws::Client::ClientConfiguration cfg;
    cfg.region = "us-east-1";
    return cfg;
}

auto ec2_id(unsigned value) -> std::string {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "i-%017x", value);
    return buf;
}

const std::string group_name = "kythira-mock-asg";
const std::string cluster = "mock-cluster";

/// One member of the modelled group.
struct member {
    std::string id;
    std::string lifecycle;
    bool protected_from_scale_in = false;
};

/// The tags EC2 holds per instance, shared by both doubles.
using tag_store = std::map<std::string, std::map<std::string, std::string>>;

/// Auto Scaling, as far as `aws_asg_quorum_manager` uses it.
class FakeAutoScaling : public as::AutoScalingClient {
public:
    explicit FakeAutoScaling(std::shared_ptr<tag_store> tags)
        : as::AutoScalingClient(client_config()), _tags(std::move(tags)) {}

    /// Add a member that existed before the test's provision.
    void seed(const std::string& id, const std::string& lifecycle = "InService",
              bool is_protected = false) {
        std::lock_guard lock(_mu);
        _members.push_back({id, lifecycle, is_protected});
        _desired = static_cast<int>(_members.size());
    }

    /// The lifecycle a new launch reports, or empty to launch nothing.
    void set_launch_lifecycle(std::string lifecycle) {
        std::lock_guard lock(_mu);
        _launch_lifecycle = std::move(lifecycle);
    }
    /// When set, every `Pending` member that existed before a grow turns
    /// `InService` at the grow: the case that used to be adopted by mistake.
    void promote_pending_on_grow() {
        std::lock_guard lock(_mu);
        _promote_pending = true;
    }
    /// Refuse the next @p n terminations with `ScalingActivityInProgress`.
    void set_busy_terminations(int n) {
        std::lock_guard lock(_mu);
        _busy_terminations = n;
    }
    /// Add a launch lifecycle hook. While a member is `Pending:Wait` its
    /// launch activity is open: terminating it is refused with
    /// `ScalingActivityInProgress`, as AWS does, and only a
    /// `CompleteLifecycleAction` on a hook releases it.
    void add_launch_hook(std::string name) {
        std::lock_guard lock(_mu);
        _launch_hooks.push_back(std::move(name));
    }
    [[nodiscard]] auto abandon_calls() const -> int {
        std::lock_guard lock(_mu);
        return _abandon_calls;
    }
    /// Fail every `SetInstanceProtection` with this exception name.
    void set_protection_error(std::string name) {
        std::lock_guard lock(_mu);
        _protection_error = std::move(name);
    }

    [[nodiscard]] auto ids() const -> std::vector<std::string> {
        std::lock_guard lock(_mu);
        std::vector<std::string> out;
        for (const auto& m : _members) {
            out.push_back(m.id);
        }
        return out;
    }
    [[nodiscard]] auto desired() const -> int {
        std::lock_guard lock(_mu);
        return _desired;
    }
    [[nodiscard]] auto is_protected(const std::string& id) const -> bool {
        std::lock_guard lock(_mu);
        const auto it = std::ranges::find(_members, id, &member::id);
        return it != _members.end() && it->protected_from_scale_in;
    }
    [[nodiscard]] auto update_calls() const -> int {
        std::lock_guard lock(_mu);
        return _update_calls;
    }
    [[nodiscard]] auto terminate_calls() const -> int {
        std::lock_guard lock(_mu);
        return _terminate_calls;
    }
    [[nodiscard]] auto protection_calls() const -> int {
        std::lock_guard lock(_mu);
        return _protection_calls;
    }
    [[nodiscard]] auto blind_scale_ins() const -> int {
        std::lock_guard lock(_mu);
        return _blind_scale_ins;
    }
    /// The id of the most recent launch, or empty.
    [[nodiscard]] auto last_launch() const -> std::string {
        std::lock_guard lock(_mu);
        return _last_launch;
    }

    auto DescribeAutoScalingGroups(const asm_::DescribeAutoScalingGroupsRequest& /*request*/) const
        -> asm_::DescribeAutoScalingGroupsOutcome override {
        std::lock_guard lock(_mu);
        asm_::AutoScalingGroup group;
        group.SetAutoScalingGroupName(group_name);
        group.SetDesiredCapacity(_desired);
        group.SetHealthCheckType("EC2");
        for (const auto& m : _members) {
            asm_::Instance inst;
            inst.SetInstanceId(m.id);
            inst.SetLifecycleState(
                asm_::LifecycleStateMapper::GetLifecycleStateForName(m.lifecycle));
            inst.SetProtectedFromScaleIn(m.protected_from_scale_in);
            group.AddInstances(std::move(inst));
        }
        asm_::DescribeAutoScalingGroupsResult result;
        result.AddAutoScalingGroups(std::move(group));
        return asm_::DescribeAutoScalingGroupsOutcome(std::move(result));
    }

    auto UpdateAutoScalingGroup(const asm_::UpdateAutoScalingGroupRequest& request) const
        -> asm_::UpdateAutoScalingGroupOutcome override {
        std::lock_guard lock(_mu);
        ++_update_calls;
        const int target = request.GetDesiredCapacity();
        if (target > _desired && _promote_pending) {
            for (auto& m : _members) {
                if (m.lifecycle == "Pending") {
                    m.lifecycle = "InService";
                }
            }
        }
        while (static_cast<int>(_members.size()) < target && !_launch_lifecycle.empty()) {
            _last_launch = ec2_id(++_next);
            _members.push_back({_last_launch, _launch_lifecycle, false});
        }
        // The group picks its own victims: oldest first, skipping protected
        // members and launches a hook still holds.
        while (static_cast<int>(_members.size()) > target) {
            const auto victim = std::ranges::find_if(_members, [](const member& m) {
                return !m.protected_from_scale_in && m.lifecycle != "Pending:Wait";
            });
            if (victim == _members.end()) {
                break;
            }
            _members.erase(victim);
            ++_blind_scale_ins;
        }
        _desired = target;
        return asm_::UpdateAutoScalingGroupOutcome(Aws::NoResult());
    }

    auto TerminateInstanceInAutoScalingGroup(
        const asm_::TerminateInstanceInAutoScalingGroupRequest& request) const
        -> asm_::TerminateInstanceInAutoScalingGroupOutcome override {
        std::lock_guard lock(_mu);
        ++_terminate_calls;
        if (_busy_terminations > 0) {
            --_busy_terminations;
            return busy_error();
        }
        const auto it = std::ranges::find(_members, request.GetInstanceId(), &member::id);
        if (it != _members.end() && it->lifecycle == "Pending:Wait") {
            return busy_error();
        }
        if (it == _members.end()) {
            return as::AutoScalingError(Aws::Client::AWSError<as::AutoScalingErrors>(
                as::AutoScalingErrors::VALIDATION, "ValidationError",
                "Instance Id not found - No managed instance found", false));
        }
        // Protection does not block a terminate (AWS documents this), so the
        // double ignores it too.
        _members.erase(it);
        if (request.GetShouldDecrementDesiredCapacity()) {
            --_desired;
        }
        return asm_::TerminateInstanceInAutoScalingGroupResult{};
    }

    auto SetInstanceProtection(const asm_::SetInstanceProtectionRequest& request) const
        -> asm_::SetInstanceProtectionOutcome override {
        std::lock_guard lock(_mu);
        ++_protection_calls;
        if (!_protection_error.empty()) {
            return as::AutoScalingError(Aws::Client::AWSError<as::AutoScalingErrors>(
                as::AutoScalingErrors::ACCESS_DENIED, _protection_error, "refused by the double",
                false));
        }
        for (const auto& id : request.GetInstanceIds()) {
            const auto it = std::ranges::find(_members, std::string(id), &member::id);
            if (it != _members.end()) {
                it->protected_from_scale_in = request.GetProtectedFromScaleIn();
            }
        }
        return asm_::SetInstanceProtectionResult{};
    }

    auto DescribeLifecycleHooks(const asm_::DescribeLifecycleHooksRequest& /*request*/) const
        -> asm_::DescribeLifecycleHooksOutcome override {
        std::lock_guard lock(_mu);
        asm_::DescribeLifecycleHooksResult result;
        for (const auto& name : _launch_hooks) {
            asm_::LifecycleHook hook;
            hook.SetLifecycleHookName(name);
            hook.SetAutoScalingGroupName(group_name);
            hook.SetLifecycleTransition("autoscaling:EC2_INSTANCE_LAUNCHING");
            result.AddLifecycleHooks(std::move(hook));
        }
        return asm_::DescribeLifecycleHooksOutcome(std::move(result));
    }

    /// `ABANDON` terminates the held launch. The group then replaces it
    /// whenever the desired capacity still counts it, as AWS does.
    auto CompleteLifecycleAction(const asm_::CompleteLifecycleActionRequest& request) const
        -> asm_::CompleteLifecycleActionOutcome override {
        std::lock_guard lock(_mu);
        const auto it = std::ranges::find(_members, request.GetInstanceId(), &member::id);
        if (it == _members.end() || it->lifecycle != "Pending:Wait" ||
            std::ranges::find(_launch_hooks, request.GetLifecycleHookName()) ==
                _launch_hooks.end()) {
            return as::AutoScalingError(Aws::Client::AWSError<as::AutoScalingErrors>(
                as::AutoScalingErrors::VALIDATION, "ValidationError",
                "No active Lifecycle Action found with instance ID " + request.GetInstanceId(),
                false));
        }
        ++_abandon_calls;
        if (request.GetLifecycleActionResult() == "ABANDON") {
            _members.erase(it);
            while (static_cast<int>(_members.size()) < _desired && !_launch_lifecycle.empty()) {
                _last_launch = ec2_id(++_next);
                _members.push_back({_last_launch, _launch_lifecycle, false});
            }
        } else {
            it->lifecycle = "InService";
        }
        return asm_::CompleteLifecycleActionResult{};
    }

    auto DescribeAutoScalingInstances(const asm_::DescribeAutoScalingInstancesRequest& request)
        const -> asm_::DescribeAutoScalingInstancesOutcome override {
        std::lock_guard lock(_mu);
        asm_::DescribeAutoScalingInstancesResult result;
        for (const auto& id : request.GetInstanceIds()) {
            const auto it = std::ranges::find(_members, std::string(id), &member::id);
            if (it != _members.end()) {
                asm_::AutoScalingInstanceDetails details;
                details.SetInstanceId(it->id);
                details.SetAutoScalingGroupName(group_name);
                details.SetLifecycleState(it->lifecycle);
                result.AddAutoScalingInstances(std::move(details));
            }
        }
        return asm_::DescribeAutoScalingInstancesOutcome(std::move(result));
    }

private:
    [[nodiscard]] static auto busy_error() -> as::AutoScalingError {
        return as::AutoScalingError(Aws::Client::AWSError<as::AutoScalingErrors>(
            as::AutoScalingErrors::SCALING_ACTIVITY_IN_PROGRESS_FAULT, "ScalingActivityInProgress",
            "Scaling activity is in progress", false));
    }

    mutable std::mutex _mu;
    std::shared_ptr<tag_store> _tags;
    mutable std::vector<member> _members;
    mutable int _desired = 0;
    mutable unsigned _next = 100;
    mutable std::string _last_launch;
    std::string _launch_lifecycle = "InService";
    bool _promote_pending = false;
    mutable int _busy_terminations = 0;
    std::string _protection_error;
    mutable int _update_calls = 0;
    mutable int _terminate_calls = 0;
    mutable int _protection_calls = 0;
    mutable int _blind_scale_ins = 0;
    std::vector<std::string> _launch_hooks;
    mutable int _abandon_calls = 0;
};

/// EC2, as far as `aws_asg_quorum_manager` uses it: every instance the group
/// lists has a private IP, and tags land in the shared store.
class FakeEc2 : public Aws::EC2::EC2Client {
public:
    FakeEc2(std::shared_ptr<tag_store> tags, const FakeAutoScaling& asg)
        : Aws::EC2::EC2Client(Aws::EC2::EC2ClientConfiguration(client_config())),
          _tags(std::move(tags)),
          _asg(asg) {}

    /// By instance id, or, with no ids, every tagged instance matching the
    /// request's `tag:` filters: the managers look nodes up by their
    /// kythira:cluster and kythira:node-id tags. A member the group no longer
    /// lists is reported terminated.
    auto DescribeInstances(const em::DescribeInstancesRequest& request) const
        -> em::DescribeInstancesOutcome override {
        std::lock_guard lock(_mu);
        em::DescribeInstancesResponse response;
        unsigned octet = 10;
        std::vector<Aws::String> ids(request.GetInstanceIds().begin(),
                                     request.GetInstanceIds().end());
        if (ids.empty()) {
            for (const auto& [id, tags] : *_tags) {
                if (matches_tag_filters(tags, request.GetFilters())) {
                    ids.emplace_back(id);
                }
            }
        }
        const auto live = _asg.ids();
        for (const auto& id : ids) {
            em::Instance inst;
            inst.SetInstanceId(id);
            inst.SetPrivateIpAddress("10.0.0." + std::to_string(++octet));
            em::InstanceState state;
            state.SetName(std::ranges::find(live, std::string(id)) != live.end()
                              ? em::InstanceStateName::running
                              : em::InstanceStateName::terminated);
            inst.SetState(std::move(state));
            if (const auto it = _tags->find(std::string(id)); it != _tags->end()) {
                for (const auto& [key, value] : it->second) {
                    em::Tag tag;
                    tag.SetKey(key);
                    tag.SetValue(value);
                    inst.AddTags(std::move(tag));
                }
            }
            em::Reservation reservation;
            reservation.AddInstances(std::move(inst));
            response.AddReservations(std::move(reservation));
        }
        return em::DescribeInstancesOutcome(std::move(response));
    }

    auto CreateTags(const em::CreateTagsRequest& request) const -> em::CreateTagsOutcome override {
        std::lock_guard lock(_mu);
        for (const auto& resource : request.GetResources()) {
            for (const auto& tag : request.GetTags()) {
                (*_tags)[std::string(resource)][std::string(tag.GetKey())] =
                    std::string(tag.GetValue());
            }
        }
        return em::CreateTagsOutcome(Aws::NoResult());
    }

    /// A member the group no longer lists is reported as gone.
    auto DescribeInstanceStatus(const em::DescribeInstanceStatusRequest& request) const
        -> em::DescribeInstanceStatusOutcome override {
        em::DescribeInstanceStatusResponse response;
        const auto live = _asg.ids();
        for (const auto& id : request.GetInstanceIds()) {
            if (std::ranges::find(live, std::string(id)) == live.end()) {
                continue;
            }
            em::InstanceStatus status;
            status.SetInstanceId(id);
            em::InstanceState state;
            state.SetName(em::InstanceStateName::running);
            status.SetInstanceState(std::move(state));
            response.AddInstanceStatuses(std::move(status));
        }
        return em::DescribeInstanceStatusOutcome(std::move(response));
    }

private:
    /// True when @p tags satisfies every `tag:<key>` filter; other filters
    /// are not used by the managers and are ignored.
    static auto matches_tag_filters(const std::map<std::string, std::string>& tags,
                                    const Aws::Vector<em::Filter>& filters) -> bool {
        for (const auto& filter : filters) {
            const std::string name(filter.GetName());
            if (!name.starts_with("tag:")) {
                continue;
            }
            const auto it = tags.find(name.substr(4));
            if (it == tags.end() ||
                std::ranges::none_of(filter.GetValues(),
                                     [&](const Aws::String& v) { return it->second == v; })) {
                return false;
            }
        }
        return true;
    }

    mutable std::mutex _mu;
    std::shared_ptr<tag_store> _tags;
    const FakeAutoScaling& _asg;
};

using manager = kythira::aws_asg_quorum_manager<std::uint64_t, std::string>;

/// Three tagged voters in one group, and a manager over the doubles.
struct Cloud {
    std::shared_ptr<tag_store> tags = std::make_shared<tag_store>();
    std::shared_ptr<FakeAutoScaling> asg = std::make_shared<FakeAutoScaling>(tags);
    std::shared_ptr<FakeEc2> ec2 = std::make_shared<FakeEc2>(tags, *asg);
    std::vector<std::string> voters{ec2_id(1), ec2_id(2), ec2_id(3)};

    explicit Cloud(bool protect_voters = true) {
        for (std::size_t i = 0; i < voters.size(); ++i) {
            adopt(voters[i], std::to_string(i + 1), protect_voters);
        }
    }

    /// Seed @p id as a member this cluster adopted earlier.
    void adopt(const std::string& id, const std::string& node, bool is_protected) {
        asg->seed(id, "InService", is_protected);
        (*tags)[id] = {{"kythira:cluster", cluster}, {"kythira:node-id", node}};
    }

    [[nodiscard]] static auto config() -> kythira::aws_asg_quorum_manager_config {
        kythira::aws_asg_quorum_manager_config cfg;
        cfg.cluster_name = cluster;
        cfg.asg_by_group["AZ1"] = group_name;
        cfg.topology.groups.push_back({.group_id = "AZ1", .target_count = 3});
        cfg.provision_timeout = std::chrono::seconds{1};
        cfg.poll_interval = std::chrono::seconds{0};
        cfg.aws.region = "us-east-1";
        return cfg;
    }

    [[nodiscard]] auto make() const -> manager { return manager{config(), asg, ec2}; }
};

/// The message a failed provision carries.
auto provision_error(manager& mgr) -> std::string {
    try {
        std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    BOOST_FAIL("provision_node was expected to time out");
    return {};
}

}  // namespace

// ── Timeout rollback (Requirements 2-3) ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(asg_timeout_rollback)

// The launch never reaches InService. The old path lowered the desired
// capacity and the group terminated its oldest unprotected member, which
// with unprotected voters was a voter. Now the launch itself goes, by id.
BOOST_AUTO_TEST_CASE(a_pending_launch_is_terminated_by_id_and_every_voter_kept) {
    // Unprotected, as a cluster adopted before protection existed would be,
    // and kept so by a reconcile that fails transiently: only the targeted
    // removal keeps them safe.
    Cloud cloud(/*protect_voters=*/false);
    cloud.asg->set_protection_error("Throttling");
    cloud.asg->set_launch_lifecycle("Pending");
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    const auto launched = cloud.asg->last_launch();
    BOOST_TEST(error.find("rollback: removed " + launched + " (fresh, Pending)") !=
               std::string::npos);
    BOOST_TEST(cloud.asg->ids() == cloud.voters);
    BOOST_TEST(cloud.asg->desired() == 3);
    BOOST_TEST(cloud.asg->blind_scale_ins() == 0);
    BOOST_TEST(cloud.asg->update_calls() == 1);  // The grow; no blind restore.
}

// A member that was Pending before the grow and comes InService during the
// wait is not this provision's launch: the old InService-only snapshot
// adopted it. It must be neither adopted nor removed.
BOOST_AUTO_TEST_CASE(a_pre_existing_pending_member_is_neither_adopted_nor_removed) {
    Cloud cloud;
    const auto early = ec2_id(50);
    cloud.asg->seed(early, "Pending");
    cloud.asg->set_launch_lifecycle("Pending");
    cloud.asg->promote_pending_on_grow();
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: removed " + cloud.asg->last_launch()) != std::string::npos);
    const auto ids = cloud.asg->ids();
    BOOST_TEST((std::ranges::find(ids, early) != ids.end()));
    BOOST_TEST(!cloud.tags->contains(early));
}

// ASG refuses a terminate while the scale-out it just started is in progress.
BOOST_AUTO_TEST_CASE(a_busy_group_is_waited_out_before_the_terminate) {
    Cloud cloud;
    cloud.asg->set_launch_lifecycle("Pending");
    cloud.asg->set_busy_terminations(2);
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: removed ") != std::string::npos);
    BOOST_TEST(cloud.asg->terminate_calls() == 3);
    BOOST_TEST(cloud.asg->ids() == cloud.voters);
}

// A launch lifecycle hook holds the launch in Pending:Wait, which keeps its
// scaling activity open, so the group refuses the terminate for as long as
// the hook's heartbeat. Real run 37475165001 retried that until the timeout
// and left the desired size grown. The held launch is abandoned instead,
// after the capacity is lowered so the group does not replace it.
BOOST_AUTO_TEST_CASE(a_launch_held_by_a_lifecycle_hook_is_abandoned) {
    Cloud cloud;
    cloud.asg->add_launch_hook("hold-launch");
    cloud.asg->set_launch_lifecycle("Pending:Wait");
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    const auto launched = cloud.asg->last_launch();
    BOOST_TEST(error.find("rollback: removed " + launched + " (fresh, Pending:Wait)") !=
               std::string::npos);
    BOOST_TEST(cloud.asg->ids() == cloud.voters);
    BOOST_TEST(cloud.asg->desired() == 3);
    BOOST_TEST(cloud.asg->abandon_calls() == 1);
    BOOST_TEST(cloud.asg->blind_scale_ins() == 0);
}

// Nothing launched at all: the capacity write is the only undo, and the
// protected voters survive it.
BOOST_AUTO_TEST_CASE(nothing_launched_restores_the_desired_capacity_by_write) {
    Cloud cloud;
    cloud.asg->set_launch_lifecycle("");
    auto mgr = cloud.make();

    const auto error = provision_error(mgr);
    BOOST_TEST(error.find("rollback: desired size restored to 3") != std::string::npos);
    BOOST_TEST(error.find("lost member") == std::string::npos);
    BOOST_TEST(cloud.asg->update_calls() == 2);
    BOOST_TEST(cloud.asg->desired() == 3);
    BOOST_TEST(cloud.asg->ids() == cloud.voters);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Scale-in protection (Requirement 4) ──────────────────────────────────────

BOOST_AUTO_TEST_SUITE(asg_scale_in_protection)

BOOST_AUTO_TEST_CASE(an_adopted_instance_is_protected) {
    Cloud cloud;
    auto mgr = cloud.make();
    const auto peer = std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    const auto launched = cloud.asg->last_launch();
    BOOST_TEST(peer.address == "10.0.0.11:7000");
    BOOST_TEST(cloud.asg->is_protected(launched));
    BOOST_TEST(cloud.tags->at(launched).at("kythira:cluster") == cluster);
}

BOOST_AUTO_TEST_CASE(a_protection_failure_does_not_fail_the_provision) {
    Cloud cloud;
    auto mgr = cloud.make();
    cloud.asg->set_protection_error("Throttling");
    BOOST_CHECK_NO_THROW(std::move(mgr.provision_node("AZ1", std::nullopt)).get());
    BOOST_TEST(!cloud.asg->is_protected(cloud.asg->last_launch()));
}

// Only this cluster's adopted members: an untagged member and another
// cluster's node are left alone.
BOOST_AUTO_TEST_CASE(construction_protects_this_clusters_unprotected_members) {
    Cloud cloud;
    const auto stray = ec2_id(60);
    const auto foreign = ec2_id(61);
    const auto unprotected_voter = ec2_id(62);
    cloud.asg->seed(stray);
    cloud.asg->seed(foreign);
    (*cloud.tags)[foreign] = {{"kythira:cluster", "other"}, {"kythira:node-id", "9"}};
    cloud.adopt(unprotected_voter, "4", /*is_protected=*/false);

    auto mgr = cloud.make();
    BOOST_TEST(cloud.asg->protection_calls() == 1);
    BOOST_TEST(cloud.asg->is_protected(unprotected_voter));
    BOOST_TEST(!cloud.asg->is_protected(stray));
    BOOST_TEST(!cloud.asg->is_protected(foreign));
}

BOOST_AUTO_TEST_CASE(construction_without_the_protection_permission_throws) {
    Cloud cloud;
    cloud.adopt(ec2_id(62), "4", /*is_protected=*/false);
    cloud.asg->set_protection_error("AccessDenied");
    BOOST_CHECK_THROW((void)cloud.make(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(construction_with_every_member_protected_makes_no_call) {
    Cloud cloud;
    auto mgr = cloud.make();
    BOOST_TEST(cloud.asg->protection_calls() == 0);
}

// TerminateInstanceInAutoScalingGroup is not blocked by protection, so a
// protected node is decommissioned as it is.
BOOST_AUTO_TEST_CASE(a_protected_node_is_decommissioned_without_clearing_it) {
    Cloud cloud;
    auto mgr = cloud.make();
    BOOST_CHECK_NO_THROW(std::move(mgr.decommission_node(std::uint64_t{1})).get());
    const auto ids = cloud.asg->ids();
    BOOST_TEST((std::ranges::find(ids, cloud.voters[0]) == ids.end()));
    BOOST_TEST(cloud.asg->desired() == 2);
    BOOST_TEST(cloud.asg->protection_calls() == 0);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Lifecycle mapping ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(lifecycle_names_map_onto_the_planners_classes) {
    using kythira::aws_asg_detail::rollback_state;
    using kythira::group_rollback::member_state;
    BOOST_TEST((rollback_state("InService") == member_state::live));
    BOOST_TEST((rollback_state("Standby") == member_state::live));
    BOOST_TEST((rollback_state("Pending:Wait") == member_state::pending));
    BOOST_TEST((rollback_state("Quarantined") == member_state::pending));
    BOOST_TEST((rollback_state("Warmed:Pending") == member_state::pending));
    BOOST_TEST((rollback_state("Terminating:Wait") == member_state::terminal));
    BOOST_TEST((rollback_state("Terminated") == member_state::terminal));
    BOOST_TEST((rollback_state("Detaching") == member_state::terminal));
    BOOST_TEST((rollback_state("Warmed:Terminated") == member_state::terminal));
}
