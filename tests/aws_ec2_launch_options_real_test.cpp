// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Real-EC2 coverage for the parts of aws_ec2_quorum_manager::provision_node
// that only change the RunInstances request: placement groups, the spot
// market option, and the provision-timeout cleanup path
// (aws-quorum-manager Requirement 16.19 e-i, Requirement 18.10).
//
// aws_quorum_manager_real_ec2_test.cpp covers liveness and remediation, and
// every case there pays for a NAT gateway and a bastion, because those cases
// reach the instances. Nothing here does: each case asks EC2 how it placed
// or billed an instance, which DescribeInstances answers without a packet
// reaching the instance. So this fixture is the lean one the ASG suite
// uses: a VPC, one private subnet per zone, a security group with no
// ingress, and the placement groups a case asks for. No internet gateway,
// NAT gateway, key pair or bastion.
//
// It is its own binary so that it can run in its own CI job. The aws job's
// worst-case budget is already the full 360 minutes.
//
// Every resource carries kythira:suite=aws-ec2-launch-options and this
// run's kythira:cluster tag, and teardown finds everything it deletes by
// that tag at deletion time, so a signal that lands while a Create call is
// in flight still cleans up what that call made.

#define BOOST_TEST_MODULE aws_ec2_launch_options_real_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_ec2_quorum_manager.hpp>

#include <aws/core/Aws.h>
#include <aws/core/utils/UUID.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreatePlacementGroupRequest.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeletePlacementGroupRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeAvailabilityZonesRequest.h>
#include <aws/ec2/model/DescribeImagesRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DescribePlacementGroupsRequest.h>
#include <aws/ec2/model/DescribeSecurityGroupsRequest.h>
#include <aws/ec2/model/DescribeSubnetsRequest.h>
#include <aws/ec2/model/DescribeVpcsRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/InstanceLifecycleType.h>
#include <aws/ec2/model/InstanceStateName.h>
#include <aws/ec2/model/InstanceType.h>
#include <aws/ec2/model/PlacementStrategy.h>
#include <aws/ec2/model/ResourceType.h>
#include <aws/ec2/model/Tag.h>
#include <aws/ec2/model/TagSpecification.h>
#include <aws/ec2/model/TerminateInstancesRequest.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/GetCallerIdentityRequest.h>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include "aws_real_ec2_test_support.hpp"
#include "aws_refreshing_credentials_provider.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using kythira::testing::aws_real_ec2::AwsSignalHandlerFixture;
using kythira::testing::aws_real_ec2::BilledResource;
using kythira::testing::aws_real_ec2::CostSummaryFixture;
using kythira::testing::aws_real_ec2::ec2_hourly_rate;
using kythira::testing::aws_real_ec2::g_active_aws_fixture;
using kythira::testing::aws_real_ec2::g_cost_accumulator;
using kythira::testing::aws_real_ec2::signal_cleanup_target;
using kythira::testing::aws_real_ec2::TestCostReport;

using ec2_manager = kythira::aws_ec2_quorum_manager<>;
using node_id = ec2_manager::node_id_type;
using ec2_cluster = std::vector<kythira::node_placement<node_id, std::string>>;

constexpr const char* kSuiteTagKey = "kythira:suite";
constexpr const char* kSuiteTagValue = "aws-ec2-launch-options";
constexpr const char* kRunPrefix = "kythira-ec2lo-";
constexpr std::size_t kAzCount = 2;

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : std::string{};
}

// Same contract as the helper of this name in aws_quorum_manager_real_ec2_test.cpp:
// a provider that re-federates GitHub Actions OIDC before the one-hour session
// credentials expire, or nullptr off-CI so the SDK default chain applies.
auto refreshing_credentials_provider(const std::string& region)
    -> std::shared_ptr<Aws::Auth::AWSCredentialsProvider> {
    using kythira::testing::aws_real_ec2::github_oidc_refreshing_credentials_provider;

    if (!github_oidc_refreshing_credentials_provider::environment_available()) {
        return nullptr;
    }
    std::string role_arn = env("KYTHIRA_AWS_WEB_IDENTITY_ROLE_ARN");
    if (role_arn.empty()) {
        role_arn = env("AWS_ROLE_ARN");
    }
    if (role_arn.empty()) {
        return nullptr;
    }
    return std::make_shared<github_oidc_refreshing_credentials_provider>(
        Aws::String(role_arn.data(), role_arn.size()), Aws::String("kythira-ci-real-cloud-tests"),
        Aws::String(region.data(), region.size()));
}

// Registered first so ctest reports "Not Run" (SKIP_RETURN_CODE 77) rather
// than "Failed" when no AWS environment is configured.
struct PreflightSkipFixture {
    PreflightSkipFixture() {
        if (env("AWS_DEFAULT_REGION").empty() && env("AWS_REGION").empty()) {
            std::cerr << "SKIP: AWS region not set (AWS_DEFAULT_REGION or AWS_REGION)\n";
            std::exit(77);
        }
    }
};

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
struct FollyInitFixture {
    FollyInitFixture() {
        int argc = boost::unit_test::framework::master_test_suite().argc;
        char** argv = boost::unit_test::framework::master_test_suite().argv;
        folly::init(&argc, &argv, false);
    }
};
#endif

struct AwsSdkFixture {
    AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::InitAPI(opts);
    }
    ~AwsSdkFixture() {
        Aws::SDKOptions opts;
        Aws::ShutdownAPI(opts);
    }
};

BOOST_GLOBAL_FIXTURE(PreflightSkipFixture);
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
BOOST_GLOBAL_FIXTURE(FollyInitFixture);
#endif
BOOST_GLOBAL_FIXTURE(AwsSdkFixture);
BOOST_GLOBAL_FIXTURE(CostSummaryFixture);
BOOST_GLOBAL_FIXTURE(AwsSignalHandlerFixture);

// Polls `done` every `interval` until it returns true or `budget` elapses, and
// says which in the log either way.
auto wait_until(const std::string& what, std::chrono::seconds budget, std::chrono::seconds interval,
                const std::function<bool()>& done) -> bool {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + budget;
    while (true) {
        if (done()) {
            const auto took = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start);
            std::cerr << "[ec2-launch] " << what << ": done after " << took.count() << "s\n";
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[ec2-launch] TIMED OUT after " << budget.count()
                      << "s waiting for: " << what << "\n";
            return false;
        }
        std::this_thread::sleep_for(interval);
    }
}

auto ec2_tag(const std::string& k, const std::string& v) -> Aws::EC2::Model::Tag {
    Aws::EC2::Model::Tag t;
    t.SetKey(k);
    t.SetValue(v);
    return t;
}

auto ec2_filter(const std::string& name, const std::string& value) -> Aws::EC2::Model::Filter {
    Aws::EC2::Model::Filter f;
    f.SetName(name);
    f.AddValues(value);
    return f;
}

auto tag_value(const Aws::Vector<Aws::EC2::Model::Tag>& tags, const std::string& key)
    -> std::string {
    for (const auto& t : tags) {
        if (std::string_view{t.GetKey()} == key) {
            return {t.GetValue().data(), t.GetValue().size()};
        }
    }
    return {};
}

auto is_gone(Aws::EC2::Model::InstanceStateName st) -> bool {
    return st == Aws::EC2::Model::InstanceStateName::shutting_down ||
           st == Aws::EC2::Model::InstanceStateName::terminated;
}

// ── Ec2LaunchFixture ─────────────────────────────────────────────────────────

struct Ec2LaunchFixture : signal_cleanup_target {
    bool torn_down_{false};
    std::atomic<bool> setup_complete_{false};

    std::string region;
    std::string ami_id;
    // Burstable type used by every case except the cluster placement group.
    std::string instance_type;
    // EC2 refuses burstable (T-family) types in a cluster placement group,
    // so that one case needs a fixed-performance type.
    std::string cluster_pg_instance_type;
    // Short random id; scopes every name and the kythira:cluster tag.
    std::string run_id;

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> creds_provider;
    std::shared_ptr<Aws::EC2::EC2Client> ec2;

    std::vector<std::string> azs;
    std::vector<std::string> subnet_ids;  // parallel to azs
    std::string vpc_id;
    std::string sg_id;
    std::vector<std::string> placement_groups;

    kythira::aws_ec2_quorum_manager_config mgr_cfg;
    TestCostReport cost_report;
    std::map<std::string, std::size_t> billed_line;

    Ec2LaunchFixture() {
        region = env("AWS_DEFAULT_REGION");
        if (region.empty()) {
            region = env("AWS_REGION");
        }
        if (region.empty()) {
            throw std::runtime_error("skip: AWS region not set (AWS_DEFAULT_REGION or AWS_REGION)");
        }
        cost_report.test_name =
            std::string(boost::unit_test::framework::current_test_case().p_name);

        Aws::Client::ClientConfiguration cli_cfg;
        cli_cfg.region = region;
        creds_provider = refreshing_credentials_provider(region);
        ec2 = creds_provider ? std::make_shared<Aws::EC2::EC2Client>(creds_provider, cli_cfg)
                             : std::make_shared<Aws::EC2::EC2Client>(cli_cfg);

        auto sts = creds_provider ? Aws::STS::STSClient{creds_provider, cli_cfg}
                                  : Aws::STS::STSClient{cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error(
                "skip: AWS STS GetCallerIdentity failed — credentials not available: " +
                std::string(id_out.GetError().GetMessage()));
        }

        instance_type = env("KYTHIRA_TEST_INSTANCE_TYPE");
        cluster_pg_instance_type = env("KYTHIRA_TEST_CLUSTER_PG_INSTANCE_TYPE");
#if defined(__aarch64__) || defined(__arm64__)
        if (instance_type.empty()) {
            instance_type = "t4g.micro";
        }
        if (cluster_pg_instance_type.empty()) {
            cluster_pg_instance_type = "c6g.medium";
        }
#else
        if (instance_type.empty()) {
            instance_type = "t3.micro";
        }
        if (cluster_pg_instance_type.empty()) {
            cluster_pg_instance_type = "c5.large";
        }
#endif
        ami_id = env("KYTHIRA_TEST_AMI_ID");
        if (ami_id.empty()) {
            ami_id = latest_al2023_ami();
        }
        if (ami_id.empty()) {
            throw std::runtime_error("skip: could not determine AMI (set KYTHIRA_TEST_AMI_ID)");
        }

        const Aws::String uuid = Aws::Utils::UUID::RandomUUID();
        run_id = std::string(kRunPrefix) + std::string(uuid.data(), uuid.size()).substr(0, 8);
        std::cerr << "[ec2-launch] run " << run_id << " in " << region << ", " << instance_type
                  << " / " << cluster_pg_instance_type << " from " << ami_id << "\n";

        // Registered before the first resource exists, so a signal that lands
        // mid-setup still runs teardown().
        g_active_aws_fixture.store(this, std::memory_order_release);
        try {
            resolve_azs();
            create_vpc();
            create_subnets();
            create_security_group();
            build_mgr_cfg();
            setup_complete_.store(true, std::memory_order_release);
        } catch (...) {
            g_active_aws_fixture.store(nullptr, std::memory_order_release);
            teardown();
            throw;
        }
    }

    ~Ec2LaunchFixture() {
        g_active_aws_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    Ec2LaunchFixture(const Ec2LaunchFixture&) = delete;
    Ec2LaunchFixture& operator=(const Ec2LaunchFixture&) = delete;
    Ec2LaunchFixture(Ec2LaunchFixture&&) = delete;
    Ec2LaunchFixture& operator=(Ec2LaunchFixture&&) = delete;

    // ── Construction ─────────────────────────────────────────────────────────

    auto latest_al2023_ami() -> std::string {
        Aws::EC2::Model::DescribeImagesRequest req;
        req.AddOwners("amazon");
#if defined(__aarch64__) || defined(__arm64__)
        req.AddFilters(ec2_filter("name", "al2023-ami-*arm64*"));
        req.AddFilters(ec2_filter("architecture", "arm64"));
#else
        req.AddFilters(ec2_filter("name", "al2023-ami-*x86_64*"));
        req.AddFilters(ec2_filter("architecture", "x86_64"));
#endif
        req.AddFilters(ec2_filter("virtualization-type", "hvm"));
        auto out = ec2->DescribeImages(req);
        Aws::String latest_date;
        std::string latest_id;
        if (out.IsSuccess()) {
            for (const auto& img : out.GetResult().GetImages()) {
                if (img.GetCreationDate() > latest_date) {
                    latest_date = img.GetCreationDate();
                    latest_id = std::string(img.GetImageId());
                }
            }
        }
        return latest_id;
    }

    void resolve_azs() {
        Aws::EC2::Model::DescribeAvailabilityZonesRequest req;
        req.AddFilters(ec2_filter("state", "available"));
        req.AddFilters(ec2_filter("zone-type", "availability-zone"));
        auto out = ec2->DescribeAvailabilityZones(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "DescribeAvailabilityZones: " +
                                                   std::string(out.GetError().GetMessage()));
        for (const auto& z : out.GetResult().GetAvailabilityZones()) {
            azs.emplace_back(z.GetZoneName());
        }
        std::sort(azs.begin(), azs.end());
        BOOST_REQUIRE_MESSAGE(azs.size() >= kAzCount,
                              "region " + region + " has fewer than 2 available zones");
        azs.resize(kAzCount);
    }

    [[nodiscard]] auto tag_spec(Aws::EC2::Model::ResourceType type, const std::string& name) const
        -> Aws::EC2::Model::TagSpecification {
        Aws::EC2::Model::TagSpecification spec;
        spec.SetResourceType(type);
        spec.AddTags(ec2_tag(kSuiteTagKey, kSuiteTagValue));
        spec.AddTags(ec2_tag("kythira:cluster", run_id));
        spec.AddTags(ec2_tag("Name", name));
        return spec;
    }

    void create_vpc() {
        Aws::EC2::Model::CreateVpcRequest req;
        req.SetCidrBlock("10.79.0.0/16");
        req.AddTagSpecifications(tag_spec(Aws::EC2::Model::ResourceType::vpc, run_id + "-vpc"));
        auto out = ec2->CreateVpc(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "CreateVpc: " + std::string(out.GetError().GetMessage()));
        vpc_id = std::string(out.GetResult().GetVpc().GetVpcId());
    }

    // Private subnets with only the VPC's local route: nothing here talks to an
    // instance, and the ASG suite measured that an instance with no egress
    // still reaches `running` in seconds.
    void create_subnets() {
        for (std::size_t i = 0; i < azs.size(); ++i) {
            Aws::EC2::Model::CreateSubnetRequest req;
            req.SetVpcId(vpc_id);
            req.SetAvailabilityZone(azs[i]);
            req.SetCidrBlock("10.79." + std::to_string(i + 1) + ".0/24");
            req.AddTagSpecifications(
                tag_spec(Aws::EC2::Model::ResourceType::subnet, run_id + "-" + azs[i]));
            auto out = ec2->CreateSubnet(req);
            BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "CreateSubnet " + azs[i] + ": " +
                                                       std::string(out.GetError().GetMessage()));
            subnet_ids.emplace_back(out.GetResult().GetSubnet().GetSubnetId());
        }
    }

    void create_security_group() {
        Aws::EC2::Model::CreateSecurityGroupRequest req;
        req.SetVpcId(vpc_id);
        req.SetGroupName(run_id + "-sg");
        req.SetDescription("kythira ec2 launch-options real test, no ingress");
        req.AddTagSpecifications(
            tag_spec(Aws::EC2::Model::ResourceType::security_group, run_id + "-sg"));
        auto out = ec2->CreateSecurityGroup(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "CreateSecurityGroup: " + std::string(out.GetError().GetMessage()));
        sg_id = std::string(out.GetResult().GetGroupId());
    }

    // Groups are named AZ1, AZ2 like the sibling suite's, mapped to this
    // run's subnets. The suite tag rides along as an extra tag so every
    // instance the manager launches can be found by teardown and the audit.
    void build_mgr_cfg() {
        mgr_cfg.cluster_name = run_id;
        mgr_cfg.image_id = ami_id;
        mgr_cfg.instance_type = instance_type;
        mgr_cfg.node_port = 7000;
        for (std::size_t i = 0; i < azs.size(); ++i) {
            const std::string group = "AZ" + std::to_string(i + 1);
            mgr_cfg.subnet_by_group[group] = subnet_ids[i];
            mgr_cfg.topology.groups.push_back({.group_id = group, .target_count = 2});
        }
        mgr_cfg.security_group_ids.push_back(sg_id);
        mgr_cfg.extra_tags[kSuiteTagKey] = kSuiteTagValue;
        mgr_cfg.provision_timeout = std::chrono::seconds{180};
        mgr_cfg.poll_interval = std::chrono::seconds{5};
        mgr_cfg.aws.region = region;
        if (creds_provider) {
            mgr_cfg.aws.credentials_provider =
                std::make_shared<kythira::testing::aws_real_ec2::single_provider_chain>(
                    creds_provider);
        }
    }

    // ── Helpers for the cases ────────────────────────────────────────────────

    // Creates a placement group tagged with this run and records it for
    // teardown before returning.
    auto create_placement_group(Aws::EC2::Model::PlacementStrategy strategy, int partitions = 0)
        -> std::string {
        const std::string name =
            run_id + "-" +
            std::string(
                Aws::EC2::Model::PlacementStrategyMapper::GetNameForPlacementStrategy(strategy));
        Aws::EC2::Model::CreatePlacementGroupRequest req;
        req.SetGroupName(name);
        req.SetStrategy(strategy);
        if (partitions > 0) {
            req.SetPartitionCount(partitions);
        }
        req.AddTagSpecifications(tag_spec(Aws::EC2::Model::ResourceType::placement_group, name));
        auto out = ec2->CreatePlacementGroup(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "CreatePlacementGroup " + name + ": " +
                                                   std::string(out.GetError().GetMessage()));
        placement_groups.push_back(name);
        return name;
    }

    auto describe_instance(const std::string& id) -> std::optional<Aws::EC2::Model::Instance> {
        Aws::EC2::Model::DescribeInstancesRequest req;
        req.AddInstanceIds(id);
        auto out = ec2->DescribeInstances(req);
        if (!out.IsSuccess()) {
            return std::nullopt;
        }
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Every instance carrying this run's cluster tag, in any state.
    auto run_instances() -> std::vector<Aws::EC2::Model::Instance> {
        Aws::EC2::Model::DescribeInstancesRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        std::vector<Aws::EC2::Model::Instance> found;
        auto out = ec2->DescribeInstances(req);
        if (!out.IsSuccess()) {
            std::cerr << "[ec2-launch] DescribeInstances by tag failed: "
                      << out.GetError().GetMessage() << "\n";
            return found;
        }
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                found.push_back(i);
            }
        }
        return found;
    }

    auto live_run_instance_ids() -> std::vector<std::string> {
        std::vector<std::string> ids;
        for (const auto& i : run_instances()) {
            if (i.GetState().GetName() != Aws::EC2::Model::InstanceStateName::terminated) {
                ids.emplace_back(i.GetInstanceId());
            }
        }
        return ids;
    }

    // Provisions through `mgr`, starts a cost line at the request, and reads the
    // instance back so a case can assert on what EC2 actually launched.
    auto provision(ec2_manager& mgr, const std::string& group, const std::string& market)
        -> Aws::EC2::Model::Instance {
        const auto requested = std::chrono::steady_clock::now();
        auto p = std::move(mgr.provision_node(group, std::nullopt)).get();
        const auto id = ec2_manager::node_id_to_ec2_id(p.node_id);
        auto inst = describe_instance(id);
        BOOST_REQUIRE_MESSAGE(inst.has_value(),
                              "provisioned instance " + id + " not returned by DescribeInstances");
        const auto type_name =
            Aws::EC2::Model::InstanceTypeMapper::GetNameForInstanceType(inst->GetInstanceType());
        const std::string launched(type_name.data(), type_name.size());
        BilledResource line;
        line.start = requested;
        line.label = "ec2 " + launched + " " + market + " " + id;
        line.hourly_rate = ec2_hourly_rate(launched);
        cost_report.resources.push_back(std::move(line));
        billed_line[id] = cost_report.resources.size() - 1;
        std::cerr << "[ec2-launch] provisioned " << id << " (" << launched << ", " << market
                  << ") in " << group << "\n";
        return *inst;
    }

    void stop_billing(const std::string& id) {
        if (auto it = billed_line.find(id); it != billed_line.end()) {
            cost_report.resources[it->second].finalize();
        }
    }

    // Requirement 16.19i: decommission is only done once EC2 says so.
    auto wait_terminated(const std::string& id, std::chrono::seconds budget) -> bool {
        return wait_until(id + " to reach `terminated`", budget, std::chrono::seconds{5}, [&] {
            auto inst = describe_instance(id);
            return inst.has_value() &&
                   inst->GetState().GetName() == Aws::EC2::Model::InstanceStateName::terminated;
        });
    }

    // ── Teardown ─────────────────────────────────────────────────────────────
    //
    // Order: instances, then placement groups (EC2 refuses to delete one that
    // still holds a non-terminated instance), then the network. Every step is
    // bounded and an expiry is logged and passed over, so one stuck resource
    // never strands the rest. Worst case: 15s + 300s + 120s + 300s = 735s.

    void teardown() noexcept override {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        try {
            teardown_steps();
        } catch (const std::exception& ex) {
            std::cerr << "[ec2-launch] TEARDOWN THREW (continuing to exit): " << ex.what() << "\n";
        } catch (...) {
            std::cerr << "[ec2-launch] TEARDOWN THREW a non-std exception\n";
        }
        for (auto& r : cost_report.resources) {
            r.finalize();
        }
        std::cerr << cost_report.format();
        g_cost_accumulator.add(std::move(cost_report));
    }

    void teardown_steps() {
        if (!setup_complete_.load(std::memory_order_acquire)) {
            std::cerr << "[ec2-launch] teardown before setup finished: waiting 15s for "
                         "in-flight creates to land\n";
            std::this_thread::sleep_for(std::chrono::seconds{15});
        }

        auto live = live_run_instance_ids();
        if (!live.empty()) {
            Aws::EC2::Model::TerminateInstancesRequest req;
            for (const auto& id : live) {
                req.AddInstanceIds(id);
            }
            auto out = ec2->TerminateInstances(req);
            if (!out.IsSuccess()) {
                std::cerr << "[ec2-launch] teardown: TerminateInstances failed: "
                          << out.GetError().GetMessage() << "\n";
            }
            wait_until("instances of " + run_id + " terminated", std::chrono::seconds{300},
                       std::chrono::seconds{10}, [&] { return live_run_instance_ids().empty(); });
        }

        wait_until("placement groups of " + run_id + " deleted", std::chrono::seconds{120},
                   std::chrono::seconds{10}, [&] { return delete_placement_groups_pass(); });

        wait_until("network of " + run_id + " deleted", std::chrono::seconds{300},
                   std::chrono::seconds{10}, [&] { return delete_network_pass(); });

        report_survivors();
    }

    auto tagged_placement_groups() -> std::vector<std::string> {
        Aws::EC2::Model::DescribePlacementGroupsRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        std::vector<std::string> names;
        auto out = ec2->DescribePlacementGroups(req);
        if (out.IsSuccess()) {
            for (const auto& g : out.GetResult().GetPlacementGroups()) {
                names.emplace_back(g.GetGroupName());
            }
        }
        return names;
    }

    auto delete_placement_groups_pass() -> bool {
        std::set<std::string> groups(placement_groups.begin(), placement_groups.end());
        for (auto& g : tagged_placement_groups()) {
            groups.insert(std::move(g));
        }
        bool all_gone = true;
        for (const auto& name : groups) {
            Aws::EC2::Model::DeletePlacementGroupRequest req;
            req.SetGroupName(name);
            auto out = ec2->DeletePlacementGroup(req);
            const std::string err = out.IsSuccess() ? "" : std::string(out.GetError().GetMessage());
            if (out.IsSuccess() || err.find("does not exist") != std::string::npos ||
                err.find("Unknown") != std::string::npos) {
                std::erase(placement_groups, name);
                continue;
            }
            all_gone = false;
        }
        return all_gone && tagged_placement_groups().empty();
    }

    auto tagged_ids(const char* what) -> std::vector<std::string> {
        std::vector<std::string> ids;
        const std::string w = what;
        if (w == "vpc") {
            Aws::EC2::Model::DescribeVpcsRequest req;
            req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
            auto out = ec2->DescribeVpcs(req);
            if (out.IsSuccess()) {
                for (const auto& v : out.GetResult().GetVpcs()) {
                    ids.emplace_back(v.GetVpcId());
                }
            }
        } else if (w == "subnet") {
            Aws::EC2::Model::DescribeSubnetsRequest req;
            req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
            auto out = ec2->DescribeSubnets(req);
            if (out.IsSuccess()) {
                for (const auto& s : out.GetResult().GetSubnets()) {
                    ids.emplace_back(s.GetSubnetId());
                }
            }
        } else if (w == "sg") {
            Aws::EC2::Model::DescribeSecurityGroupsRequest req;
            req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
            auto out = ec2->DescribeSecurityGroups(req);
            if (out.IsSuccess()) {
                for (const auto& g : out.GetResult().GetSecurityGroups()) {
                    ids.emplace_back(g.GetGroupId());
                }
            }
        }
        return ids;
    }

    // One attempt at the whole network, rediscovered by tag each time.
    // Retried because a terminated instance's network interface releases
    // asynchronously and blocks its security group and subnet until it does.
    auto delete_network_pass() -> bool {
        const auto sgs = tagged_ids("sg");
        const auto subnets = tagged_ids("subnet");
        const auto vpcs = tagged_ids("vpc");
        if (sgs.empty() && subnets.empty() && vpcs.empty()) {
            return true;
        }
        for (const auto& id : sgs) {
            Aws::EC2::Model::DeleteSecurityGroupRequest req;
            req.SetGroupId(id);
            (void)ec2->DeleteSecurityGroup(req);
        }
        for (const auto& id : subnets) {
            Aws::EC2::Model::DeleteSubnetRequest req;
            req.SetSubnetId(id);
            (void)ec2->DeleteSubnet(req);
        }
        for (const auto& id : vpcs) {
            Aws::EC2::Model::DeleteVpcRequest req;
            req.SetVpcId(id);
            (void)ec2->DeleteVpc(req);
        }
        return false;
    }

    void report_survivors() {
        std::vector<std::string> left = live_run_instance_ids();
        for (auto& g : tagged_placement_groups()) {
            left.push_back("placement-group " + g);
        }
        for (const char* kind : {"sg", "subnet", "vpc"}) {
            for (auto& id : tagged_ids(kind)) {
                left.push_back(id);
            }
        }
        if (left.empty()) {
            std::cerr << "[ec2-launch] teardown of " << run_id << " left nothing\n";
            return;
        }
        for (const auto& id : left) {
            std::cerr << "[ec2-launch] LEAK: " << id << " (run " << run_id
                      << ") survived teardown — delete it manually\n";
        }
    }
};

}  // namespace

// ── Cases ────────────────────────────────────────────────────────────────────
//
// Per-case budget: setup under a minute, two launches at up to 180s each, and
// the 735s worst-case teardown above. 1500s leaves room for neither to run
// long without being a target.

BOOST_FIXTURE_TEST_SUITE(ec2_launch_options, Ec2LaunchFixture)

// Requirement 16.19e: both nodes land in the cluster placement group the
// manager was configured with, and the instances are tagged with the strategy.
BOOST_AUTO_TEST_CASE(placement_group_cluster_strategy, *boost::unit_test::timeout(1500)) {
    auto pg = create_placement_group(Aws::EC2::Model::PlacementStrategy::cluster);
    auto cfg = mgr_cfg;
    cfg.instance_type = cluster_pg_instance_type;
    cfg.placement_by_group["AZ1"] = {.name = pg,
                                     .strategy = kythira::ec2_placement_group_strategy::cluster};
    ec2_manager mgr{cfg};

    ec2_cluster cluster;
    for (int i = 0; i < 2; ++i) {
        auto inst = provision(mgr, "AZ1", "on-demand");
        BOOST_CHECK_EQUAL(std::string(inst.GetPlacement().GetGroupName()), pg);
        BOOST_CHECK_EQUAL(std::string(inst.GetPlacement().GetAvailabilityZone()), azs[0]);
        BOOST_CHECK_EQUAL(tag_value(inst.GetTags(), "kythira:placement-strategy"), "cluster");
        cluster.push_back(
            {.node_id = ec2_manager::ec2_id_to_node_id(inst.GetInstanceId()), .group_id = "AZ1"});
    }
    auto health = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(health.live_node_count, 2u);
}

// Requirement 16.19f: one spread placement group shared by two zones; each
// node lands in it, in the zone its group maps to.
BOOST_AUTO_TEST_CASE(placement_group_spread_strategy, *boost::unit_test::timeout(1500)) {
    auto pg = create_placement_group(Aws::EC2::Model::PlacementStrategy::spread);
    auto cfg = mgr_cfg;
    for (const auto* group : {"AZ1", "AZ2"}) {
        cfg.placement_by_group[group] = {.name = pg,
                                         .strategy = kythira::ec2_placement_group_strategy::spread};
    }
    ec2_manager mgr{cfg};

    for (std::size_t i = 0; i < kAzCount; ++i) {
        const std::string group = "AZ" + std::to_string(i + 1);
        auto inst = provision(mgr, group, "on-demand");
        BOOST_CHECK_EQUAL(std::string(inst.GetPlacement().GetGroupName()), pg);
        BOOST_CHECK_EQUAL(std::string(inst.GetPlacement().GetAvailabilityZone()), azs[i]);
        BOOST_CHECK_EQUAL(tag_value(inst.GetTags(), "kythira:placement-strategy"), "spread");
    }
}

// Requirement 16.19g: with partition_number = 1 both nodes land in partition 1
// of a two-partition group, which is the one thing the manager's
// SetPartitionNumber call decides. EC2 would otherwise spread them.
BOOST_AUTO_TEST_CASE(placement_group_partition_strategy, *boost::unit_test::timeout(1500)) {
    auto pg = create_placement_group(Aws::EC2::Model::PlacementStrategy::partition, 2);
    auto cfg = mgr_cfg;
    cfg.placement_by_group["AZ1"] = {.name = pg,
                                     .strategy = kythira::ec2_placement_group_strategy::partition,
                                     .partition_number = 1};
    ec2_manager mgr{cfg};

    for (int i = 0; i < 2; ++i) {
        auto inst = provision(mgr, "AZ1", "on-demand");
        BOOST_CHECK_EQUAL(std::string(inst.GetPlacement().GetGroupName()), pg);
        BOOST_CHECK_EQUAL(inst.GetPlacement().GetPartitionNumber(), 1);
        BOOST_CHECK_EQUAL(tag_value(inst.GetTags(), "kythira:placement-strategy"), "partition");
    }
}

// Requirement 16.19h: a provision that times out returns an exceptional
// future AND terminates the instance it launched.
//
// Zero, not the spec's 1s: provision_node sleeps poll_interval before its
// first DescribeInstances, so any non-zero timeout races the launch, and an
// instance that is already `running` at that first poll succeeds and throws
// nothing. A zero budget skips the poll loop and takes the timeout branch
// every time, which is the branch this case is about. The GCP suite's case of
// the same name made the same change for the same reason.
BOOST_AUTO_TEST_CASE(provision_timeout_cleanup, *boost::unit_test::timeout(1500)) {
    auto cfg = mgr_cfg;
    cfg.provision_timeout = std::chrono::seconds{0};
    ec2_manager mgr{cfg};

    const auto requested = std::chrono::steady_clock::now();
    std::string what;
    try {
        std::move(mgr.provision_node("AZ1", std::nullopt)).get();
    } catch (const std::exception& ex) {
        what = ex.what();
    }
    BOOST_REQUIRE_MESSAGE(!what.empty(), "provision_node with a zero timeout did not fail");
    BOOST_CHECK_MESSAGE(what.find("timeout") != std::string::npos,
                        "unexpected provision failure: " << what);

    // The launched instance is found by this run's tag, not by anything the
    // failed call returned, so a manager that forgot to terminate it cannot
    // hide it.
    auto instances = run_instances();
    BOOST_REQUIRE_EQUAL(instances.size(), 1u);
    const std::string id(instances.front().GetInstanceId());
    BilledResource line;
    line.start = requested;
    line.label = "ec2 " + instance_type + " on-demand " + id + " (timed out)";
    line.hourly_rate = ec2_hourly_rate(instance_type);
    cost_report.resources.push_back(std::move(line));
    billed_line[id] = cost_report.resources.size() - 1;

    BOOST_CHECK_MESSAGE(is_gone(instances.front().GetState().GetName()),
                        id << " was not terminated by the timeout path");
    BOOST_CHECK(wait_terminated(id, std::chrono::seconds{300}));
    stop_billing(id);
}

// Requirement 16.19i: with no spot options the instance is on-demand, both in
// what EC2 reports and in the market tag, and decommission terminates it.
BOOST_AUTO_TEST_CASE(on_demand_provision_and_decommission, *boost::unit_test::timeout(1500)) {
    auto cfg = mgr_cfg;
    cfg.spot_options = std::nullopt;
    ec2_manager mgr{cfg};

    auto inst = provision(mgr, "AZ1", "on-demand");
    const std::string id(inst.GetInstanceId());
    BOOST_CHECK(inst.GetInstanceLifecycle() != Aws::EC2::Model::InstanceLifecycleType::spot);
    BOOST_CHECK_EQUAL(tag_value(inst.GetTags(), "kythira:market"), "on-demand");

    std::string err;
    try {
        std::move(mgr.decommission_node(ec2_manager::ec2_id_to_node_id(id))).get();
    } catch (const std::exception& ex) {
        err = ex.what();
    }
    BOOST_CHECK_MESSAGE(err.empty(), "decommission_node threw: " << err);
    BOOST_CHECK(wait_terminated(id, std::chrono::seconds{300}));
    stop_billing(id);
}

// Requirement 18.10: with spot options the instance really is a Spot instance
// (InstanceLifecycle, which only EC2 can set), not merely tagged as one.
BOOST_AUTO_TEST_CASE(spot_provision_reports_spot_lifecycle, *boost::unit_test::timeout(1500)) {
    auto cfg = mgr_cfg;
    cfg.spot_options = kythira::ec2_spot_options{};
    ec2_manager mgr{cfg};

    auto inst = provision(mgr, "AZ1", "spot");
    const std::string id(inst.GetInstanceId());
    BOOST_CHECK(inst.GetInstanceLifecycle() == Aws::EC2::Model::InstanceLifecycleType::spot);
    BOOST_CHECK_EQUAL(tag_value(inst.GetTags(), "kythira:market"), "spot");

    std::string err;
    try {
        std::move(mgr.decommission_node(ec2_manager::ec2_id_to_node_id(id))).get();
    } catch (const std::exception& ex) {
        err = ex.what();
    }
    BOOST_CHECK_MESSAGE(err.empty(), "decommission_node threw: " << err);
    BOOST_CHECK(wait_terminated(id, std::chrono::seconds{300}));
    stop_billing(id);
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !KYTHIRA_HAS_AWS_SDK

BOOST_AUTO_TEST_CASE(aws_sdk_not_available) {
    BOOST_TEST_MESSAGE("aws_ec2_launch_options_real_test: built without the AWS SDK");
}

#endif  // KYTHIRA_HAS_AWS_SDK
#else   // !KYTHIRA_AWS_REAL_EC2_TESTS

BOOST_AUTO_TEST_CASE(real_ec2_tests_disabled) {
    BOOST_TEST_MESSAGE("aws_ec2_launch_options_real_test: KYTHIRA_AWS_REAL_EC2_TESTS not defined");
}

#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
