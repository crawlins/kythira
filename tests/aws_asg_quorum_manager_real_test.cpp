// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Real-AWS validation of aws_asg_quorum_manager
// (.kiro/specs/aws-asg-real-cloud-tests/).
//
// Nothing else in this repository executes aws_asg_quorum_manager against an
// Auto Scaling endpoint: the unit test stops at config validation and fault
// points, and the LocalStack suite can never run because autoscaling is
// Pro-only there. A hand-written mock is ruled out (Requirement 0): the
// Azure VMSS sibling was wrong in five independent ways on first real
// execution, four of them "the API does not behave the way the code assumes",
// which a mock written by the same hand encodes rather than catches.
//
// The fixture provisions everything it needs per case (design §2) — VPC, one
// subnet per Availability Zone, a no-ingress security group, an EC2 launch
// template and one HealthCheckType=EC2 Auto Scaling group per AZ at desired
// capacity 0 — and destroys it in the reverse dependency order design §3
// specifies. There are deliberately no operator fixtures and no repository
// variables to pass: a missing resource fails loudly with the API's own
// error, where a missing input skipped five Azure cases for months.
//
// Naming and tagging contract with scripts/ci-cloud-credentials/aws/policies/
// asg-quorum-manager.json, which scopes the CI role by both:
//   * every Auto Scaling group is named kythira-asgtest-*;
//   * every EC2 resource carries kythira:suite=aws-asg-quorum-manager.
// kythira:managed-by is NOT used for that purpose, because the manager
// overwrites it on each instance it provisions.

#define BOOST_TEST_MODULE aws_asg_quorum_manager_real_test
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_AWS_REAL_EC2_TESTS
#ifdef KYTHIRA_HAS_AWS_SDK

#include <raft/aws_asg_quorum_manager.hpp>

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/autoscaling/model/CreateAutoScalingGroupRequest.h>
#include <aws/autoscaling/model/DeleteAutoScalingGroupRequest.h>
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/autoscaling/model/Filter.h>
#include <aws/autoscaling/model/Instance.h>
#include <aws/autoscaling/model/LifecycleState.h>
#include <aws/autoscaling/model/LaunchTemplateSpecification.h>
#include <aws/autoscaling/model/PutLifecycleHookRequest.h>
#include <aws/autoscaling/model/Tag.h>
#include <aws/autoscaling/model/UpdateAutoScalingGroupRequest.h>
#include <aws/core/Aws.h>
#include <aws/core/utils/UUID.h>
#include <aws/ec2/EC2Client.h>
#include <aws/ec2/model/CreateLaunchTemplateRequest.h>
#include <aws/ec2/model/CreateSecurityGroupRequest.h>
#include <aws/ec2/model/CreateSubnetRequest.h>
#include <aws/ec2/model/CreateVpcRequest.h>
#include <aws/ec2/model/DeleteLaunchTemplateRequest.h>
#include <aws/ec2/model/DeleteSecurityGroupRequest.h>
#include <aws/ec2/model/DeleteSubnetRequest.h>
#include <aws/ec2/model/DeleteVpcRequest.h>
#include <aws/ec2/model/DescribeAvailabilityZonesRequest.h>
#include <aws/ec2/model/DescribeImagesRequest.h>
#include <aws/ec2/model/DescribeInstanceStatusRequest.h>
#include <aws/ec2/model/DescribeInstancesRequest.h>
#include <aws/ec2/model/DescribeLaunchTemplatesRequest.h>
#include <aws/ec2/model/DescribeSecurityGroupsRequest.h>
#include <aws/ec2/model/DescribeSubnetsRequest.h>
#include <aws/ec2/model/DescribeVpcsRequest.h>
#include <aws/ec2/model/Filter.h>
#include <aws/ec2/model/InstanceStateName.h>
#include <aws/ec2/model/InstanceType.h>
#include <aws/ec2/model/LaunchTemplateInstanceNetworkInterfaceSpecificationRequest.h>
#include <aws/ec2/model/LaunchTemplateTagSpecificationRequest.h>
#include <aws/ec2/model/RequestLaunchTemplateData.h>
#include <aws/ec2/model/ResourceType.h>
#include <aws/ec2/model/StopInstancesRequest.h>
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

using asg_manager = kythira::aws_asg_quorum_manager<>;
using asg_node_id = asg_manager::node_id_type;
using asg_cluster = std::vector<kythira::node_placement<asg_node_id, std::string>>;

constexpr const char* kSuiteTagKey = "kythira:suite";
constexpr const char* kSuiteTagValue = "aws-asg-quorum-manager";
// The CI role may only mutate Auto Scaling groups under this prefix.
constexpr const char* kAsgNamePrefix = "kythira-asgtest-";
constexpr int kAsgMaxSize = 3;
constexpr std::size_t kAzCount = 3;

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return (v != nullptr) ? std::string(v) : std::string{};
}

// Same contract as aws_quorum_manager_real_ec2_test.cpp's helper of this name:
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
// than "Failed" when no AWS environment is configured. See the identical
// fixture in aws_quorum_manager_real_ec2_test.cpp for why this must be a
// BOOST_GLOBAL_FIXTURE and not init_unit_test_suite.
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
        _init = std::make_unique<folly::Init>(&argc, &argv, false);
    }
    std::unique_ptr<folly::Init> _init;
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

// Polls `done` every `interval` until it returns true or `budget` elapses.
// Returns whether the condition was met; on expiry it logs which condition
// and how long, so a caller that carries on (teardown) still leaves a trail
// the job's audit can confirm or contradict. Requirement 7: a wait confirms
// a condition, it never merely elapses.
auto wait_until(const std::string& what, std::chrono::seconds budget, std::chrono::seconds interval,
                const std::function<bool()>& done) -> bool {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + budget;
    while (true) {
        if (done()) {
            const auto took = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start);
            std::cerr << "[asg-real] " << what << ": done after " << took.count() << "s\n";
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "[asg-real] TIMED OUT after " << budget.count()
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

// The cases look a node's instance up through the manager: in numeric mode
// that reads the kythira:node-id tag it wrote when it adopted the instance.
auto ec2_id_of(const asg_manager& mgr, asg_node_id nid) -> std::string {
    auto id = mgr.instance_id_of(nid);
    BOOST_REQUIRE_MESSAGE(id.has_value(), "no instance carries node id " << nid);
    return *id;
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

// Runs one decommission and returns the failure text, empty on success.
// BOOST_CHECK_NO_THROW reports only that something was thrown; the first
// real run of decommission_is_idempotent failed that way twice and the AWS
// error behind it was lost, so every decommission a case asserts on goes
// through here and the message is printed with the failure.
auto decommission_error(asg_manager& mgr, asg_node_id nid) -> std::string {
    try {
        std::move(mgr.decommission_node(nid)).get();
        return {};
    } catch (const std::exception& ex) {
        return ex.what()[0] != '\0' ? std::string{ex.what()} : std::string{"(empty what())"};
    } catch (...) {
        return "non-std exception";
    }
}

// ── AsgRealFixture ───────────────────────────────────────────────────────────

struct AsgRealFixture : signal_cleanup_target {
    bool torn_down_{false};
    // False until every fixture resource exists. A teardown that starts
    // before then may be racing a Create call still in flight (see
    // teardown_steps), so it first gives that call time to land.
    std::atomic<bool> setup_complete_{false};
    // Last error each network delete returned, keyed by resource id, so a
    // LEAK line can say why (an AccessDenied reads very differently from a
    // DependencyViolation) without logging every retry.
    std::map<std::string, std::string> last_delete_error_;

    std::string region;
    std::string ami_id;
    std::string instance_type;
    // Short random id; scopes every name and the kythira:cluster tag. Random,
    // not derived from the test case: concurrent runs of the same case (the
    // x64 and arm64 matrix legs) must never collide on a resource name.
    std::string run_id;

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> creds_provider;
    std::shared_ptr<Aws::EC2::EC2Client> ec2;
    std::shared_ptr<Aws::AutoScaling::AutoScalingClient> asg;

    std::vector<std::string> azs;
    std::string vpc_id;
    std::vector<std::string> subnet_ids;  // parallel to azs
    std::string sg_id;
    std::string launch_template_id;
    std::vector<std::string> asg_names;  // parallel to azs; only groups that exist

    kythira::aws_asg_quorum_manager_config mgr_cfg;
    TestCostReport cost_report;
    // Instance id -> its line in cost_report, so a case that terminates an
    // instance can stop its clock instead of billing it until teardown.
    std::map<std::string, std::size_t> billed_line;

    AsgRealFixture() {
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
        if (creds_provider) {
            ec2 = std::make_shared<Aws::EC2::EC2Client>(creds_provider, cli_cfg);
            asg = std::make_shared<Aws::AutoScaling::AutoScalingClient>(creds_provider, cli_cfg);
        } else {
            ec2 = std::make_shared<Aws::EC2::EC2Client>(cli_cfg);
            asg = std::make_shared<Aws::AutoScaling::AutoScalingClient>(cli_cfg);
        }

        auto sts = creds_provider ? Aws::STS::STSClient{creds_provider, cli_cfg}
                                  : Aws::STS::STSClient{cli_cfg};
        auto id_out = sts.GetCallerIdentity(Aws::STS::Model::GetCallerIdentityRequest{});
        if (!id_out.IsSuccess()) {
            throw std::runtime_error(
                "skip: AWS STS GetCallerIdentity failed — credentials not available: " +
                std::string(id_out.GetError().GetMessage()));
        }

        // Nothing here runs a workload (design §2.3), so the smallest
        // burstable type is enough. Architecture matches the build host only
        // because the AMI lookup does; it has no other significance.
        instance_type = env("KYTHIRA_TEST_INSTANCE_TYPE");
        if (instance_type.empty()) {
#if defined(__aarch64__) || defined(__arm64__)
            instance_type = "t4g.micro";
#else
            instance_type = "t3.micro";
#endif
        }
        ami_id = env("KYTHIRA_TEST_AMI_ID");
        if (ami_id.empty()) {
            ami_id = latest_al2023_ami();
        }
        if (ami_id.empty()) {
            throw std::runtime_error("skip: could not determine AMI (set KYTHIRA_TEST_AMI_ID)");
        }

        const Aws::String uuid = Aws::Utils::UUID::RandomUUID();
        run_id = std::string(kAsgNamePrefix) + std::string(uuid.data(), uuid.size()).substr(0, 8);
        std::cerr << "[asg-real] run " << run_id << " in " << region << ", " << instance_type
                  << " from " << ami_id << "\n";

        // Registered before the first resource exists, so a signal that
        // lands mid-setup still runs teardown().
        g_active_aws_fixture.store(this, std::memory_order_release);

        // A throw out of a constructor means the destructor never runs, so
        // everything created so far would leak. Tear down here and rethrow.
        try {
            resolve_azs();
            create_vpc();
            create_subnets();
            create_security_group();
            create_launch_template();
            create_asgs();
            build_mgr_cfg();
            setup_complete_.store(true, std::memory_order_release);
        } catch (...) {
            g_active_aws_fixture.store(nullptr, std::memory_order_release);
            teardown();
            throw;
        }
    }

    ~AsgRealFixture() {
        g_active_aws_fixture.store(nullptr, std::memory_order_release);
        teardown();
    }

    AsgRealFixture(const AsgRealFixture&) = delete;
    AsgRealFixture& operator=(const AsgRealFixture&) = delete;
    AsgRealFixture(AsgRealFixture&&) = delete;
    AsgRealFixture& operator=(AsgRealFixture&&) = delete;

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
                const auto& d = img.GetCreationDate();
                if (d > latest_date) {
                    latest_date = d;
                    latest_id = std::string(img.GetImageId());
                }
            }
        }
        return latest_id;
    }

    // The first kAzCount available zones, by name. Zones are not assumed to be
    // region + "a/b/c": some accounts lack a given letter.
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
                              "region " + region + " has fewer than 3 available zones");
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
        req.SetCidrBlock("10.78.0.0/16");
        req.AddTagSpecifications(tag_spec(Aws::EC2::Model::ResourceType::vpc, run_id + "-vpc"));
        auto out = ec2->CreateVpc(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "CreateVpc: " + std::string(out.GetError().GetMessage()));
        vpc_id = std::string(out.GetResult().GetVpc().GetVpcId());
    }

    // One private subnet per zone. No internet gateway and no route beyond
    // the VPC's local one: the manager reads instance state, never connects
    // to an instance, and the suite runs no workload (design §2.3).
    //
    // Measured 2026-10-02 (spec task 1c, us-east-1, t3.micro, AL2023, no IGW,
    // no public IP): an EC2-health group's instance was `running` 14s after
    // CreateAutoScalingGroup, InService/HEALTHY at 16s, and both status checks
    // `ok` about 2.5 minutes later. Egress is not needed for any of that, so
    // an internet gateway would be pure cost and teardown surface. The same
    // figures put the manager's 120s provision_timeout default well clear of
    // the observed launch time.
    void create_subnets() {
        for (std::size_t i = 0; i < azs.size(); ++i) {
            Aws::EC2::Model::CreateSubnetRequest req;
            req.SetVpcId(vpc_id);
            req.SetAvailabilityZone(azs[i]);
            req.SetCidrBlock("10.78." + std::to_string(i + 1) + ".0/24");
            req.AddTagSpecifications(
                tag_spec(Aws::EC2::Model::ResourceType::subnet, run_id + "-" + azs[i]));
            auto out = ec2->CreateSubnet(req);
            BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "CreateSubnet " + azs[i] + ": " +
                                                       std::string(out.GetError().GetMessage()));
            subnet_ids.emplace_back(out.GetResult().GetSubnet().GetSubnetId());
        }
    }

    // No ingress rules (design §2.3): nothing ever connects to these
    // instances. The default allow-all egress rule is left as AWS creates it.
    void create_security_group() {
        Aws::EC2::Model::CreateSecurityGroupRequest req;
        req.SetVpcId(vpc_id);
        req.SetGroupName(run_id + "-sg");
        req.SetDescription("kythira asg real test, no ingress");
        req.AddTagSpecifications(
            tag_spec(Aws::EC2::Model::ResourceType::security_group, run_id + "-sg"));
        auto out = ec2->CreateSecurityGroup(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "CreateSecurityGroup: " + std::string(out.GetError().GetMessage()));
        sg_id = std::string(out.GetResult().GetGroupId());
    }

    // A launch template, not a launch configuration (Requirement 2.3). Its
    // tag specifications stamp the suite tag on every instance and volume at
    // launch, which is what lets the CI role stop and terminate them and
    // lets teardown's stray-instance sweep find them.
    void create_launch_template() {
        Aws::EC2::Model::LaunchTemplateInstanceNetworkInterfaceSpecificationRequest nic;
        nic.SetDeviceIndex(0);
        nic.SetAssociatePublicIpAddress(false);
        nic.AddGroups(sg_id);

        Aws::EC2::Model::RequestLaunchTemplateData data;
        data.SetImageId(ami_id);
        data.SetInstanceType(
            Aws::EC2::Model::InstanceTypeMapper::GetInstanceTypeForName(instance_type));
        data.AddNetworkInterfaces(nic);
        for (auto type :
             {Aws::EC2::Model::ResourceType::instance, Aws::EC2::Model::ResourceType::volume}) {
            Aws::EC2::Model::LaunchTemplateTagSpecificationRequest spec;
            spec.SetResourceType(type);
            spec.AddTags(ec2_tag(kSuiteTagKey, kSuiteTagValue));
            spec.AddTags(ec2_tag("kythira:cluster", run_id));
            data.AddTagSpecifications(spec);
        }

        Aws::EC2::Model::CreateLaunchTemplateRequest req;
        req.SetLaunchTemplateName(run_id + "-lt");
        req.SetLaunchTemplateData(data);
        req.AddTagSpecifications(
            tag_spec(Aws::EC2::Model::ResourceType::launch_template, run_id + "-lt"));
        auto out = ec2->CreateLaunchTemplate(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                              "CreateLaunchTemplate: " + std::string(out.GetError().GetMessage()));
        launch_template_id = std::string(out.GetResult().GetLaunchTemplate().GetLaunchTemplateId());
    }

    // Requirement 2.4: HealthCheckType=EC2 (the manager refuses anything
    // else), MinSize 0 and desired 0, so an idle group costs nothing.
    void create_asgs() {
        for (std::size_t i = 0; i < azs.size(); ++i) {
            create_group(run_id + "-" + azs[i], subnet_ids[i], "EC2");
        }
    }

    // Creates one group at min 0 / desired 0 and records it for teardown
    // before returning, so a case that adds a group of its own (3.8) gets the
    // same cleanup as the fixture's.
    void create_group(const std::string& name, const std::string& subnet_id,
                      const std::string& health_check_type) {
        Aws::AutoScaling::Model::LaunchTemplateSpecification lt;
        lt.SetLaunchTemplateId(launch_template_id);
        lt.SetVersion("$Latest");
        Aws::AutoScaling::Model::CreateAutoScalingGroupRequest req;
        req.SetAutoScalingGroupName(name);
        req.SetLaunchTemplate(lt);
        req.SetMinSize(0);
        req.SetMaxSize(kAsgMaxSize);
        req.SetDesiredCapacity(0);
        req.SetVPCZoneIdentifier(subnet_id);
        req.SetHealthCheckType(health_check_type);
        for (const auto& [k, v] :
             {std::pair<std::string, std::string>{kSuiteTagKey, kSuiteTagValue},
              {"kythira:cluster", run_id}}) {
            Aws::AutoScaling::Model::Tag t;
            t.SetKey(k);
            t.SetValue(v);
            t.SetPropagateAtLaunch(true);
            req.AddTags(t);
        }
        auto out = asg->CreateAutoScalingGroup(req);
        BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "CreateAutoScalingGroup " + name + ": " +
                                                   std::string(out.GetError().GetMessage()));
        asg_names.push_back(name);
    }

    // One topology group per zone, named by the zone itself so a failure
    // message names something an operator can look up.
    void build_mgr_cfg() {
        mgr_cfg.cluster_name = run_id;
        mgr_cfg.node_port = 7000;
        for (std::size_t i = 0; i < azs.size(); ++i) {
            mgr_cfg.asg_by_group[azs[i]] = asg_names[i];
            mgr_cfg.topology.groups.push_back({.group_id = azs[i], .target_count = 1});
        }
        mgr_cfg.aws.region = region;
        if (creds_provider) {
            mgr_cfg.aws.credentials_provider =
                std::make_shared<kythira::testing::aws_real_ec2::single_provider_chain>(
                    creds_provider);
        }
    }

    // ── Observation helpers for the cases ────────────────────────────────────

    auto describe_group(const std::string& name)
        -> std::optional<Aws::AutoScaling::Model::AutoScalingGroup> {
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest req;
        req.AddAutoScalingGroupNames(name);
        auto out = asg->DescribeAutoScalingGroups(req);
        if (!out.IsSuccess() || out.GetResult().GetAutoScalingGroups().empty()) {
            return std::nullopt;
        }
        return out.GetResult().GetAutoScalingGroups().front();
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

    // The group's membership entry for one instance, if it is still a member.
    auto group_member(const std::string& group_name, const std::string& id)
        -> std::optional<Aws::AutoScaling::Model::Instance> {
        auto g = describe_group(group_name);
        if (!g) {
            return std::nullopt;
        }
        for (const auto& i : g->GetInstances()) {
            if (std::string_view{i.GetInstanceId()} == id) {
                return i;
            }
        }
        return std::nullopt;
    }

    // Provisions through the manager, then reads the instance back so its cost
    // line names the type EC2 actually launched rather than the one this file
    // asked for (spec task 5: the Azure suite priced one VM size while running
    // another for months). The clock starts when the capacity is requested,
    // not at InService: EC2 bills from launch, and a case that terminates
    // right after provisioning otherwise reports 0.0 minutes for an instance
    // that ran for most of one. Over-reporting by the API latency is the safe
    // side of that error.
    auto provision(asg_manager& mgr, const std::string& group)
        -> kythira::peer_info<asg_node_id, std::string> {
        const auto requested = std::chrono::steady_clock::now();
        auto p = std::move(mgr.provision_node(group, std::nullopt)).get();
        const auto id = ec2_id_of(mgr, p.node_id);
        auto inst = describe_instance(id);
        BOOST_REQUIRE_MESSAGE(inst.has_value(),
                              "provisioned instance " + id + " not returned by DescribeInstances");
        const auto launched_name =
            Aws::EC2::Model::InstanceTypeMapper::GetNameForInstanceType(inst->GetInstanceType());
        const std::string launched(launched_name.data(), launched_name.size());
        BOOST_CHECK_EQUAL(launched, instance_type);
        BilledResource line;
        line.start = requested;
        line.label = "ec2 " + launched + " " + id;
        line.hourly_rate = ec2_hourly_rate(launched);
        cost_report.resources.push_back(std::move(line));
        billed_line[id] = cost_report.resources.size() - 1;
        std::cerr << "[asg-real] provisioned " << id << " (" << launched << ") in " << group
                  << " at " << p.address << "\n";
        return p;
    }

    void stop_billing(const std::string& id) {
        if (auto it = billed_line.find(id); it != billed_line.end()) {
            cost_report.resources[it->second].finalize();
        }
    }

    // Requirement 7: each of these confirms a condition within a budget and
    // returns false on expiry; the caller's REQUIRE message names which side
    // failed.
    auto wait_not_running(const std::string& id, std::chrono::seconds budget) -> bool {
        return wait_until(id + " to leave `running`", budget, std::chrono::seconds{5}, [&] {
            Aws::EC2::Model::DescribeInstanceStatusRequest req;
            req.AddInstanceIds(id);
            req.SetIncludeAllInstances(true);
            auto out = ec2->DescribeInstanceStatus(req);
            if (!out.IsSuccess()) {
                return false;
            }
            const auto& sv = out.GetResult().GetInstanceStatuses();
            return !sv.empty() && sv.front().GetInstanceState().GetName() !=
                                      Aws::EC2::Model::InstanceStateName::running;
        });
    }

    auto wait_left_group(const std::string& group_name, const std::string& id,
                         std::chrono::seconds budget) -> bool {
        return wait_until(id + " to leave " + group_name, budget, std::chrono::seconds{5}, [&] {
            return describe_group(group_name).has_value() && !group_member(group_name, id);
        });
    }

    // A manager confined to the first group, for cases whose refill must be
    // attributable to one group (maintain_quorum provisions every deficit).
    [[nodiscard]] auto single_group_cfg() const -> kythira::aws_asg_quorum_manager_config {
        auto cfg = mgr_cfg;
        cfg.asg_by_group = {{azs.front(), asg_names.front()}};
        cfg.topology.groups = {{.group_id = azs.front(), .target_count = 1}};
        return cfg;
    }

    // ── Teardown (design §3) ─────────────────────────────────────────────────
    //
    // Reverse dependency order. Every wait is bounded, and an expiry is logged
    // and then IGNORED, so one stuck resource never strands everything after
    // it; the last step reports whatever survived, and the job's audit is the
    // independent check on that report.
    //
    // Every resource is found by this run's tag (or group-name list) at the
    // moment it is deleted, never only by the id setup remembered. The
    // signal handler runs teardown on the thread a Create call was blocked
    // in; when that request has already reached AWS, the resource exists but
    // its id never comes back. Spec task 3's interrupted run leaked a
    // security group, and with it the VPC, exactly that way.
    //
    // Worst-case duration, which every case's own Boost timeout must cover on
    // top of its body: 15s (in-flight creates settling, mid-setup only) +
    // 600s (group deletion, which terminates any instances) + 300s (stray
    // instances) + 540s (security groups, subnets and VPC, retried together
    // while released network interfaces drain) = 1455s. The common case,
    // groups already at zero, is under two minutes.

    void teardown() noexcept override {
        if (torn_down_) {
            return;
        }
        torn_down_ = true;
        try {
            teardown_steps();
        } catch (const std::exception& ex) {
            std::cerr << "[asg-real] TEARDOWN THREW (continuing to exit): " << ex.what() << "\n";
        } catch (...) {
            std::cerr << "[asg-real] TEARDOWN THREW a non-std exception\n";
        }
        for (auto& r : cost_report.resources) {
            r.finalize();
        }
        std::cerr << cost_report.format();
        g_cost_accumulator.add(std::move(cost_report));
    }

    void teardown_steps() {
        // 0. Interrupted mid-setup: let a Create call already sent to AWS
        //    complete, so the discovery below can see what it made.
        if (!setup_complete_.load(std::memory_order_acquire)) {
            std::cerr << "[asg-real] teardown before setup finished: waiting 15s for "
                         "in-flight creates to land\n";
            std::this_thread::sleep_for(std::chrono::seconds{15});
        }
        std::set<std::string> groups(asg_names.begin(), asg_names.end());
        for (auto& g : tagged_group_names()) {
            groups.insert(std::move(g));
        }

        // 1. Zero every group so nothing new launches while deletes land.
        for (const auto& name : groups) {
            Aws::AutoScaling::Model::UpdateAutoScalingGroupRequest req;
            req.SetAutoScalingGroupName(name);
            req.SetMinSize(0);
            req.SetDesiredCapacity(0);
            auto out = asg->UpdateAutoScalingGroup(req);
            if (!out.IsSuccess()) {
                std::cerr << "[asg-real] teardown: zeroing " << name
                          << " failed: " << out.GetError().GetMessage() << "\n";
            }
        }

        // 2. Force-delete every group (terminating its instances) and wait
        //    until none is returned. Without ForceDelete a group that still
        //    has instances refuses deletion and both would be left running.
        for (const auto& name : groups) {
            Aws::AutoScaling::Model::DeleteAutoScalingGroupRequest req;
            req.SetAutoScalingGroupName(name);
            req.SetForceDelete(true);
            auto out = asg->DeleteAutoScalingGroup(req);
            if (!out.IsSuccess()) {
                std::cerr << "[asg-real] teardown: DeleteAutoScalingGroup " << name
                          << " failed: " << out.GetError().GetMessage() << "\n";
            }
        }
        if (!groups.empty()) {
            wait_until("Auto Scaling groups " + run_id + "-* deleted", std::chrono::seconds{600},
                       std::chrono::seconds{10}, [&] {
                           Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest req;
                           for (const auto& name : groups) {
                               req.AddAutoScalingGroupNames(name);
                           }
                           auto out = asg->DescribeAutoScalingGroups(req);
                           return out.IsSuccess() && out.GetResult().GetAutoScalingGroups().empty();
                       });
        }

        // 3. Launch templates.
        auto found_templates = tagged_launch_templates();
        std::set<std::string> templates(found_templates.begin(), found_templates.end());
        if (!launch_template_id.empty()) {
            templates.insert(launch_template_id);
        }
        for (const auto& id : templates) {
            Aws::EC2::Model::DeleteLaunchTemplateRequest req;
            req.SetLaunchTemplateId(id);
            auto out = ec2->DeleteLaunchTemplate(req);
            if (!out.IsSuccess()) {
                std::cerr << "[asg-real] teardown: DeleteLaunchTemplate " << id
                          << " failed: " << out.GetError().GetMessage() << "\n";
            }
        }

        // 4. Backstop: any instance carrying this run's tag that is not yet
        //    terminated — one a group launched but had not registered when
        //    its delete landed, or one a case stopped out of band.
        auto strays = live_tagged_instances();
        if (!strays.empty()) {
            Aws::EC2::Model::TerminateInstancesRequest req;
            for (const auto& id : strays) {
                req.AddInstanceIds(id);
                std::cerr << "[asg-real] teardown: terminating stray instance " << id << "\n";
            }
            auto out = ec2->TerminateInstances(req);
            if (!out.IsSuccess()) {
                std::cerr << "[asg-real] teardown: TerminateInstances failed: "
                          << out.GetError().GetMessage() << "\n";
            }
            wait_until("stray instances of " + run_id + " terminated", std::chrono::seconds{300},
                       std::chrono::seconds{10}, [&] { return live_tagged_instances().empty(); });
        }

        // 5. Network, innermost first, rediscovered on every attempt so a
        //    resource whose Create was still in flight is found once it
        //    lands. Retried because a terminated instance's network
        //    interface releases asynchronously and blocks its security group
        //    and subnet until it does.
        wait_until("network of " + run_id + " deleted", std::chrono::seconds{540},
                   std::chrono::seconds{10}, [&] { return delete_network_pass(); });

        report_survivors();
    }

    // One attempt at the whole network: every security group, subnet and VPC
    // carrying this run's tag, plus any non-default security group inside
    // one of those VPCs. Returns true once none of them is left.
    auto delete_network_pass() -> bool {
        const auto vpcs = tagged_vpcs();
        auto sgs = tagged_security_groups();
        for (const auto& v : vpcs) {
            for (auto& g : security_groups_in(v)) {
                sgs.push_back(std::move(g));
            }
        }
        const auto subnets = tagged_subnets();
        if (vpcs.empty() && sgs.empty() && subnets.empty()) {
            return true;
        }
        for (const auto& id : std::set<std::string>(sgs.begin(), sgs.end())) {
            Aws::EC2::Model::DeleteSecurityGroupRequest req;
            req.SetGroupId(id);
            auto out = ec2->DeleteSecurityGroup(req);
            if (out.IsSuccess()) {
                std::cerr << "[asg-real] security group " << id << " deleted\n";
            } else {
                last_delete_error_[id] = std::string(out.GetError().GetExceptionName()) + ": " +
                                         std::string(out.GetError().GetMessage());
            }
        }
        for (const auto& id : subnets) {
            Aws::EC2::Model::DeleteSubnetRequest req;
            req.SetSubnetId(id);
            auto out = ec2->DeleteSubnet(req);
            if (out.IsSuccess()) {
                std::cerr << "[asg-real] subnet " << id << " deleted\n";
            } else {
                last_delete_error_[id] = std::string(out.GetError().GetExceptionName()) + ": " +
                                         std::string(out.GetError().GetMessage());
            }
        }
        for (const auto& id : vpcs) {
            Aws::EC2::Model::DeleteVpcRequest req;
            req.SetVpcId(id);
            auto out = ec2->DeleteVpc(req);
            if (out.IsSuccess()) {
                std::cerr << "[asg-real] vpc " << id << " deleted\n";
            } else {
                last_delete_error_[id] = std::string(out.GetError().GetExceptionName()) + ": " +
                                         std::string(out.GetError().GetMessage());
            }
        }
        return false;
    }

    // ── Discovery by tag ─────────────────────────────────────────────────────
    //
    // A failed Describe is logged and reads as "nothing found", so the
    // survivor report, which uses the same calls, is the place a persistent
    // failure shows.

    auto tagged_group_names() -> std::vector<std::string> {
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest req;
        Aws::AutoScaling::Model::Filter f;
        f.SetName("tag:kythira:cluster");
        f.AddValues(run_id);
        req.AddFilters(f);
        std::vector<std::string> names;
        auto out = asg->DescribeAutoScalingGroups(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeAutoScalingGroups by tag failed: "
                      << out.GetError().GetMessage() << "\n";
            return names;
        }
        for (const auto& g : out.GetResult().GetAutoScalingGroups()) {
            names.emplace_back(g.GetAutoScalingGroupName());
        }
        return names;
    }

    auto tagged_launch_templates() -> std::vector<std::string> {
        Aws::EC2::Model::DescribeLaunchTemplatesRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        std::vector<std::string> ids;
        auto out = ec2->DescribeLaunchTemplates(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeLaunchTemplates by tag failed: "
                      << out.GetError().GetMessage() << "\n";
            return ids;
        }
        for (const auto& lt : out.GetResult().GetLaunchTemplates()) {
            ids.emplace_back(lt.GetLaunchTemplateId());
        }
        return ids;
    }

    auto tagged_vpcs() -> std::vector<std::string> {
        Aws::EC2::Model::DescribeVpcsRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        std::vector<std::string> ids;
        auto out = ec2->DescribeVpcs(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeVpcs by tag failed: " << out.GetError().GetMessage()
                      << "\n";
            return ids;
        }
        for (const auto& v : out.GetResult().GetVpcs()) {
            ids.emplace_back(v.GetVpcId());
        }
        return ids;
    }

    auto tagged_subnets() -> std::vector<std::string> {
        Aws::EC2::Model::DescribeSubnetsRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        std::vector<std::string> ids;
        auto out = ec2->DescribeSubnets(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeSubnets by tag failed: " << out.GetError().GetMessage()
                      << "\n";
            return ids;
        }
        for (const auto& sn : out.GetResult().GetSubnets()) {
            ids.emplace_back(sn.GetSubnetId());
        }
        return ids;
    }

    auto describe_security_groups(const Aws::EC2::Model::Filter& f) -> std::vector<std::string> {
        Aws::EC2::Model::DescribeSecurityGroupsRequest req;
        req.AddFilters(f);
        std::vector<std::string> ids;
        auto out = ec2->DescribeSecurityGroups(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeSecurityGroups failed: " << out.GetError().GetMessage()
                      << "\n";
            return ids;
        }
        for (const auto& g : out.GetResult().GetSecurityGroups()) {
            // Every VPC's own default group goes with the VPC and cannot be
            // deleted on its own.
            if (g.GetGroupName() != "default") {
                ids.emplace_back(g.GetGroupId());
            }
        }
        return ids;
    }

    auto tagged_security_groups() -> std::vector<std::string> {
        return describe_security_groups(ec2_filter("tag:kythira:cluster", run_id));
    }

    auto security_groups_in(const std::string& vpc) -> std::vector<std::string> {
        return describe_security_groups(ec2_filter("vpc-id", vpc));
    }

    auto live_tagged_instances() -> std::vector<std::string> {
        Aws::EC2::Model::DescribeInstancesRequest req;
        req.AddFilters(ec2_filter("tag:kythira:cluster", run_id));
        Aws::EC2::Model::Filter states;
        states.SetName("instance-state-name");
        for (const char* s : {"pending", "running", "stopping", "stopped", "shutting-down"}) {
            states.AddValues(s);
        }
        req.AddFilters(states);
        std::vector<std::string> ids;
        auto out = ec2->DescribeInstances(req);
        if (!out.IsSuccess()) {
            std::cerr << "[asg-real] DescribeInstances (stray check) failed: "
                      << out.GetError().GetMessage() << "\n";
            return ids;
        }
        for (const auto& r : out.GetResult().GetReservations()) {
            for (const auto& i : r.GetInstances()) {
                ids.emplace_back(i.GetInstanceId());
            }
        }
        return ids;
    }

    // Re-reads, by tag rather than by remembered id, everything this run
    // could have created, and prints a LEAK line per survivor. Silence here
    // is the claim that teardown worked; spec task 3 verifies that claim by
    // hand before any case is written.
    void report_survivors() {
        bool clean = true;
        auto leak = [&](const std::string& what, const std::string& id) {
            clean = false;
            std::cerr << "[asg-real] LEAK: " << what << " " << id;
            if (auto it = last_delete_error_.find(id); it != last_delete_error_.end()) {
                std::cerr << " (last delete error: " << it->second << ")";
            }
            std::cerr << "\n";
        };
        for (const auto& name : tagged_group_names()) {
            leak("Auto Scaling group", name);
        }
        for (const auto& id : live_tagged_instances()) {
            leak("instance", id);
        }
        for (const auto& id : tagged_launch_templates()) {
            leak("launch template", id);
        }
        for (const auto& id : tagged_security_groups()) {
            leak("security group", id);
        }
        for (const auto& id : tagged_subnets()) {
            leak("subnet", id);
        }
        for (const auto& id : tagged_vpcs()) {
            leak("vpc", id);
        }
        std::cerr << "[asg-real] teardown of " << run_id
                  << (clean ? ": nothing tagged with this run survives\n"
                            : ": RESOURCES SURVIVED, see LEAK lines above\n");
    }
};

// ── Cases ────────────────────────────────────────────────────────────────────
//
// Per-case Boost timeouts include fixture setup and teardown, which run
// inside the case. Teardown's worst case is 1455s (see its comment), so no
// case may carry less than that plus its own body.

BOOST_FIXTURE_TEST_SUITE(real_asg, AsgRealFixture)

// Spec task 3's teardown proof: the fixture alone, before any case drives
// the manager. Asserts the groups exist in the shape the manager requires and
// that the manager's constructor accepts them, which is the first time its
// DescribeAutoScalingGroups health-check validation runs against a real
// group. Everything it creates is then torn down by the fixture.
BOOST_AUTO_TEST_CASE(fixture_provisions_idle_ec2_health_groups, *boost::unit_test::timeout(1800)) {
    BOOST_REQUIRE_EQUAL(asg_names.size(), kAzCount);
    for (std::size_t i = 0; i < asg_names.size(); ++i) {
        auto g = describe_group(asg_names[i]);
        BOOST_REQUIRE_MESSAGE(g.has_value(), "group " + asg_names[i] + " not returned");
        BOOST_CHECK_EQUAL(std::string(g->GetHealthCheckType()), "EC2");
        BOOST_CHECK_EQUAL(g->GetMinSize(), 0);
        BOOST_CHECK_EQUAL(g->GetDesiredCapacity(), 0);
        BOOST_CHECK_EQUAL(g->GetInstances().size(), 0U);
        BOOST_CHECK_EQUAL(std::string(g->GetVPCZoneIdentifier()), subnet_ids[i]);
    }
    BOOST_CHECK_NO_THROW(kythira::aws_asg_quorum_manager<>{mgr_cfg});
}

// Requirement 3.8 / spec task 11: the constructor refuses a group whose
// HealthCheckType is not EC2. Measured 2026-10-02 (spec task 1a):
// CreateAutoScalingGroup accepts HealthCheckType=ELB with no load balancer
// or target group attached, and stores ELB, so this case needs no load
// balancer and costs nothing. The stored type is asserted first: if AWS ever
// silently rewrote it to EC2, the throw below would be checking nothing.
BOOST_AUTO_TEST_CASE(rejects_non_ec2_health_check, *boost::unit_test::timeout(1800)) {
    const std::string name = run_id + "-elb";
    create_group(name, subnet_ids.front(), "ELB");
    auto g = describe_group(name);
    BOOST_REQUIRE_MESSAGE(g.has_value(), "group " + name + " not returned");
    BOOST_REQUIRE_EQUAL(std::string(g->GetHealthCheckType()), "ELB");

    kythira::aws_asg_quorum_manager_config cfg = mgr_cfg;
    cfg.asg_by_group = {{"elb", name}};
    cfg.topology.groups = {{.group_id = "elb", .target_count = 1}};
    BOOST_CHECK_THROW(kythira::aws_asg_quorum_manager<>{cfg}, std::invalid_argument);
}

// Requirement 3.1 / spec task 5.
BOOST_AUTO_TEST_CASE(provision_node_increases_desired_capacity, *boost::unit_test::timeout(2100)) {
    asg_manager mgr{mgr_cfg};
    const auto& group = azs.front();
    const auto& group_name = asg_names.front();
    auto before = describe_group(group_name);
    BOOST_REQUIRE(before.has_value());
    const int capacity = before->GetDesiredCapacity();

    auto p = provision(mgr, group);
    const auto id = ec2_id_of(mgr, p.node_id);

    auto after = describe_group(group_name);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK_EQUAL(after->GetDesiredCapacity(), capacity + 1);
    auto member = group_member(group_name, id);
    BOOST_REQUIRE_MESSAGE(member.has_value(), id + " is not a member of " + group_name);
    BOOST_CHECK(member->GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService);

    // The manager tags after InService, and tag reads are eventually
    // consistent, so wait for the tag rather than reading it once.
    std::optional<Aws::EC2::Model::Instance> inst;
    BOOST_CHECK_MESSAGE(
        wait_until(id + " to carry kythira:node-id", std::chrono::seconds{60},
                   std::chrono::seconds{5},
                   [&] {
                       inst = describe_instance(id);
                       return inst && !tag_value(inst->GetTags(), "kythira:node-id").empty();
                   }),
        "the manager's CreateTags never landed on " + id);
    BOOST_REQUIRE(inst.has_value());
    BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:node-id"), std::to_string(p.node_id));
    BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:group"), group);
    BOOST_CHECK_EQUAL(tag_value(inst->GetTags(), "kythira:cluster"), run_id);
    BOOST_CHECK_EQUAL(p.address, std::string(inst->GetPrivateIpAddress()) + ":" +
                                     std::to_string(mgr_cfg.node_port));
}

// Requirement 3.2 / spec task 5.
BOOST_AUTO_TEST_CASE(assess_quorum_reports_live_nodes, *boost::unit_test::timeout(2100)) {
    asg_manager mgr{mgr_cfg};
    const auto& group = azs.front();
    auto p = provision(mgr, group);

    const asg_cluster cluster{{.node_id = p.node_id, .group_id = group}};
    auto h = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(h.live_node_count, 1U);
    BOOST_CHECK_EQUAL(h.total_node_count, 1U);
    BOOST_CHECK(h.unreachable_nodes.empty());
    auto gh = std::ranges::find_if(h.groups, [&](const auto& g) { return g.group_id == group; });
    BOOST_REQUIRE(gh != h.groups.end());
    BOOST_CHECK_EQUAL(gh->live_count, 1U);
    BOOST_CHECK(gh->unreachable_nodes.empty());
}

// Requirement 3.3 / spec task 6: liveness comes from EC2's instance state, not
// from ASG membership. The stop is made directly, outside the manager, and
// the case waits for EC2 to report it before assessing: the Azure analogue
// failed its first real run by asserting straight after an asynchronous
// deallocate. The ASG will notice the stopped instance and replace it;
// teardown removes the replacement with everything else.
BOOST_AUTO_TEST_CASE(assess_detects_stopped_instance, *boost::unit_test::timeout(2400)) {
    asg_manager mgr{mgr_cfg};
    const auto& group = azs.front();
    auto p = provision(mgr, group);
    const auto id = ec2_id_of(mgr, p.node_id);

    Aws::EC2::Model::StopInstancesRequest stop;
    stop.AddInstanceIds(id);
    auto out = ec2->StopInstances(stop);
    BOOST_REQUIRE_MESSAGE(out.IsSuccess(),
                          "StopInstances " + id + ": " + std::string(out.GetError().GetMessage()));
    BOOST_REQUIRE_MESSAGE(wait_not_running(id, std::chrono::seconds{300}),
                          "the STOP failed, not assess_quorum: " + id +
                              " was still `running` 300s after StopInstances");

    const asg_cluster cluster{{.node_id = p.node_id, .group_id = group}};
    auto h = std::move(mgr.assess_quorum(cluster)).get();
    BOOST_CHECK_EQUAL(h.live_node_count, 0U);
    BOOST_REQUIRE_EQUAL(h.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(h.unreachable_nodes.front(), p.node_id);
}

// Requirement 3.4 / spec task 7. The manager's own wait only confirms the
// instance left `running`; this case additionally confirms it left the group
// and that the group shrank back, which is what callers rely on.
BOOST_AUTO_TEST_CASE(decommission_removes_instance, *boost::unit_test::timeout(2400)) {
    asg_manager mgr{mgr_cfg};
    const auto& group_name = asg_names.front();
    auto before = describe_group(group_name);
    BOOST_REQUIRE(before.has_value());
    const int capacity = before->GetDesiredCapacity();

    auto p = provision(mgr, azs.front());
    const auto id = ec2_id_of(mgr, p.node_id);
    {
        const auto err = decommission_error(mgr, p.node_id);
        BOOST_REQUIRE_MESSAGE(err.empty(), "decommission_node: " + err);
    }

    auto inst = describe_instance(id);
    BOOST_REQUIRE(inst.has_value());
    BOOST_CHECK(inst->GetState().GetName() != Aws::EC2::Model::InstanceStateName::running);
    BOOST_REQUIRE_MESSAGE(
        wait_left_group(group_name, id, std::chrono::seconds{300}),
        id + " was still a member of " + group_name + " 300s after decommission_node returned");
    stop_billing(id);
    auto after = describe_group(group_name);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK_EQUAL(after->GetDesiredCapacity(), capacity);
}

// Requirement 3.5 / spec task 7. Two nodes share one group so the case can
// see a repeated decommission doing harm: had either repeat decremented the
// capacity again, the group would shrink to zero and take the bystander
// with it. The first repeat is immediate (the retry a caller makes while the
// instance is still Terminating); the second comes after it has left.
BOOST_AUTO_TEST_CASE(decommission_is_idempotent, *boost::unit_test::timeout(2700)) {
    asg_manager mgr{mgr_cfg};
    const auto& group = azs.front();
    const auto& group_name = asg_names.front();

    // A node id no instance carries.
    {
        const auto err = decommission_error(mgr, asg_node_id{1});
        BOOST_CHECK_MESSAGE(err.empty(), "decommission of an unknown instance: " + err);
    }

    auto gone = provision(mgr, group);
    auto bystander = provision(mgr, group);
    const auto gone_id = ec2_id_of(mgr, gone.node_id);
    const auto bystander_id = ec2_id_of(mgr, bystander.node_id);
    auto before = describe_group(group_name);
    BOOST_REQUIRE(before.has_value());
    const int capacity = before->GetDesiredCapacity();

    {
        const auto first = decommission_error(mgr, gone.node_id);
        BOOST_REQUIRE_MESSAGE(first.empty(), "first decommission: " + first);
        const auto repeat = decommission_error(mgr, gone.node_id);
        BOOST_CHECK_MESSAGE(repeat.empty(), "repeat while Terminating: " + repeat);
    }
    BOOST_REQUIRE_MESSAGE(wait_left_group(group_name, gone_id, std::chrono::seconds{300}),
                          gone_id + " was still a member of " + group_name +
                              " 300s after decommission_node returned");
    stop_billing(gone_id);
    {
        const auto err = decommission_error(mgr, gone.node_id);
        BOOST_CHECK_MESSAGE(err.empty(), "repeat after leaving the group: " + err);
    }

    auto after = describe_group(group_name);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK_EQUAL(after->GetDesiredCapacity(), capacity - 1);
    auto member = group_member(group_name, bystander_id);
    BOOST_REQUIRE_MESSAGE(member.has_value(), "bystander " + bystander_id + " left " + group_name);
    BOOST_CHECK(member->GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService);
}

// Requirement 3.6 / spec task 8. The instance is terminated directly, outside
// the manager, and maintain_quorum runs as soon as EC2 reports it gone from
// `running`. maintain_quorum logs a failed step rather than throwing, so the
// outcome is read back from Auto Scaling, not inferred from its return. The
// group must end at its target of one: a decommission that failed to
// decrement, followed by the manager's own refill, would leave two.
BOOST_AUTO_TEST_CASE(maintain_quorum_restores_full_cluster, *boost::unit_test::timeout(2700)) {
    asg_manager mgr{single_group_cfg()};
    const auto& group = azs.front();
    const auto& group_name = asg_names.front();
    auto p = provision(mgr, group);
    const auto id = ec2_id_of(mgr, p.node_id);

    Aws::EC2::Model::TerminateInstancesRequest term;
    term.AddInstanceIds(id);
    auto out = ec2->TerminateInstances(term);
    BOOST_REQUIRE_MESSAGE(out.IsSuccess(), "TerminateInstances " + id + ": " +
                                               std::string(out.GetError().GetMessage()));
    BOOST_REQUIRE_MESSAGE(wait_not_running(id, std::chrono::seconds{300}),
                          "the TERMINATE failed, not maintain_quorum: " + id +
                              " was still `running` 300s after TerminateInstances");
    stop_billing(id);

    const asg_cluster cluster{{.node_id = p.node_id, .group_id = group}};
    auto pre = std::move(mgr.maintain_quorum(cluster)).get();
    BOOST_REQUIRE_EQUAL(pre.unreachable_nodes.size(), 1U);
    BOOST_CHECK_EQUAL(pre.unreachable_nodes.front(), p.node_id);

    auto after = describe_group(group_name);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK_EQUAL(after->GetDesiredCapacity(), 1);
    std::vector<std::string> replacements;
    for (const auto& i : after->GetInstances()) {
        if (std::string_view{i.GetInstanceId()} != id &&
            i.GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService) {
            replacements.emplace_back(i.GetInstanceId().data(), i.GetInstanceId().size());
        }
    }
    BOOST_CHECK_EQUAL(replacements.size(), 1U);
}

// Requirement 3.7 / spec task 8: one node per zone, each landing in the zone
// and subnet its group names.
BOOST_AUTO_TEST_CASE(multi_az_topology, *boost::unit_test::timeout(2400)) {
    asg_manager mgr{mgr_cfg};
    for (std::size_t i = 0; i < azs.size(); ++i) {
        auto p = provision(mgr, azs[i]);
        const auto id = ec2_id_of(mgr, p.node_id);
        auto inst = describe_instance(id);
        BOOST_REQUIRE(inst.has_value());
        BOOST_CHECK_EQUAL(std::string(inst->GetPlacement().GetAvailabilityZone()), azs[i]);
        BOOST_CHECK_EQUAL(std::string(inst->GetSubnetId()), subnet_ids[i]);
        BOOST_CHECK_MESSAGE(group_member(asg_names[i], id).has_value(),
                            id + " is not a member of " + asg_names[i]);
    }
}

// group-scale-up-rollback Requirement 8.2 / spec task 9.1. A voter is
// provisioned and adopted first, so it is InService and protected. A launch
// lifecycle hook then holds every new instance in `Pending:Wait`, which never
// meets the adoption bar, and a manager with a short provision_timeout grows
// the group. The rollback must terminate that fresh instance by id, leave
// the voter alone, and bring the desired capacity back to one.
//
// The hook's DefaultResult is ABANDON, so a rollback that fails outright
// still has the launch terminated when the heartbeat expires, and teardown's
// ForceDelete removes the group and its hook either way.
BOOST_AUTO_TEST_CASE(provision_timeout_removes_only_the_fresh_instance,
                     *boost::unit_test::timeout(2700)) {
    const auto& group = azs.front();
    const auto& group_name = asg_names.front();
    asg_manager voter_mgr{single_group_cfg()};
    auto voter = provision(voter_mgr, group);
    const auto voter_id = ec2_id_of(voter_mgr, voter.node_id);
    {
        auto member = group_member(group_name, voter_id);
        BOOST_REQUIRE(member.has_value());
        BOOST_REQUIRE_MESSAGE(member->GetProtectedFromScaleIn(),
                              "adoption did not protect " + voter_id + " from scale-in");
    }

    Aws::AutoScaling::Model::PutLifecycleHookRequest hook;
    hook.SetAutoScalingGroupName(group_name);
    hook.SetLifecycleHookName(run_id + "-hold-launch");
    hook.SetLifecycleTransition("autoscaling:EC2_INSTANCE_LAUNCHING");
    hook.SetHeartbeatTimeout(900);
    hook.SetDefaultResult("ABANDON");
    {
        auto out = asg->PutLifecycleHook(hook);
        BOOST_REQUIRE_MESSAGE(
            out.IsSuccess(), "PutLifecycleHook: " + std::string(out.GetError().GetExceptionName()) +
                                 ": " + std::string(out.GetError().GetMessage()));
    }

    auto cfg = single_group_cfg();
    cfg.provision_timeout = std::chrono::seconds{90};
    asg_manager mgr{cfg};
    const auto requested = std::chrono::steady_clock::now();
    std::string error;
    try {
        (void)std::move(mgr.provision_node(group, std::nullopt)).get();
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    std::cerr << "[asg-real] timed-out provision: " << error << "\n";
    BOOST_REQUIRE_MESSAGE(!error.empty(),
                          "provision_node adopted an instance held in Pending:Wait");
    BOOST_CHECK_MESSAGE(error.find("asg provision timeout") != std::string::npos, error);
    BOOST_CHECK_MESSAGE(error.find("rollback: removed i-") != std::string::npos,
                        "the rollback did not remove the fresh instance by id: " + error);
    BOOST_CHECK_MESSAGE(error.find(voter_id) == std::string::npos,
                        "the rollback names the voter " + voter_id + ": " + error);

    // Anything in the group other than the voter is the fresh launch, still
    // leaving; bill it from the request until it is gone.
    std::vector<std::string> fresh;
    if (auto g = describe_group(group_name)) {
        for (const auto& i : g->GetInstances()) {
            if (std::string_view{i.GetInstanceId()} != voter_id) {
                fresh.emplace_back(i.GetInstanceId().data(), i.GetInstanceId().size());
            }
        }
    }
    for (const auto& id : fresh) {
        BilledResource line;
        line.start = requested;
        line.label = "ec2 " + instance_type + " " + id + " (rolled back)";
        line.hourly_rate = ec2_hourly_rate(instance_type);
        cost_report.resources.push_back(std::move(line));
        billed_line[id] = cost_report.resources.size() - 1;
        BOOST_CHECK_MESSAGE(error.find(id) != std::string::npos,
                            "fresh instance " + id + " is not named in: " + error);
        BOOST_CHECK_MESSAGE(
            wait_left_group(group_name, id, std::chrono::seconds{300}),
            id + " was still a member of " + group_name + " 300s after the rollback");
        stop_billing(id);
    }

    auto after = describe_group(group_name);
    BOOST_REQUIRE(after.has_value());
    BOOST_CHECK_EQUAL(after->GetDesiredCapacity(), 1);
    auto member = group_member(group_name, voter_id);
    BOOST_REQUIRE_MESSAGE(member.has_value(), "the rollback removed voter " + voter_id);
    BOOST_CHECK(member->GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService);
    BOOST_CHECK(member->GetProtectedFromScaleIn());
}

BOOST_AUTO_TEST_SUITE_END()

}  // namespace

#endif  // KYTHIRA_HAS_AWS_SDK
#endif  // KYTHIRA_AWS_REAL_EC2_TESTS
