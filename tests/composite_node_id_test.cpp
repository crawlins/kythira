// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 14.1 of the cloud-composite-node-ids spec: each composite id
// type's validation, canonical round trip, case folding, percent-encoding,
// ordering and hashing.

#define BOOST_TEST_MODULE composite_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/composite_node_id.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace kythira;

static_assert(node_id<aws_ec2_node_id>);
static_assert(node_id<std::string>);
static_assert(node_id<std::uint64_t>);
static_assert(textual_node_id<gcp_instance_node_id>);
static_assert(!textual_node_id<std::uint32_t>);
static_assert(!node_id<int>);

namespace {

template<typename T> void check_round_trip(const T& id) {
    auto text = id.to_string();
    auto back = T::parse(text);
    BOOST_REQUIRE_MESSAGE(back.has_value(), "parse failed for " << text);
    BOOST_CHECK(*back == id);
    BOOST_CHECK_EQUAL(back->to_string(), text);
    BOOST_CHECK_EQUAL(back->scope(), id.scope());
    BOOST_CHECK_EQUAL(back->native(), id.native());
}

}  // namespace

BOOST_AUTO_TEST_SUITE(aws_ec2)

BOOST_AUTO_TEST_CASE(accepts_17_and_8_digit_ids) {
    for (const char* native : {"i-0123456789abcdef0", "i-f0123456789abcdef", "i-1234abcd"}) {
        aws_ec2_node_id id{"us-east-1", native};
        BOOST_CHECK_EQUAL(id.native(), native);
        BOOST_CHECK_EQUAL(id.scope(), "us-east-1");
        BOOST_CHECK_EQUAL(id.to_string(), std::string("aws-ec2:us-east-1:") + native);
        check_round_trip(id);
    }
}

BOOST_AUTO_TEST_CASE(legacy_8_digit_id_is_not_padded) {
    aws_ec2_node_id id{"eu-west-2", "i-1234abcd"};
    BOOST_CHECK_EQUAL(id.to_string(), "aws-ec2:eu-west-2:i-1234abcd");
}

BOOST_AUTO_TEST_CASE(accepts_multi_part_regions) {
    check_round_trip(aws_ec2_node_id{"us-gov-west-1", "i-0123456789abcdef0"});
    check_round_trip(aws_ec2_node_id{"ap-southeast-2", "i-0123456789abcdef0"});
}

BOOST_AUTO_TEST_CASE(rejects_bad_native_ids) {
    for (const char* native :
         {"", "i-", "0123456789abcdef0", "i-0123456789ABCDEF0", "i-0123456789abcdef", "i-123",
          "i-0123456789abcdef01", "i-g123abcd", "x-1234abcd", "i-1234abcd ", "ami-1234abcd"}) {
        BOOST_CHECK_THROW((aws_ec2_node_id{"us-east-1", native}), std::invalid_argument);
        BOOST_CHECK(!aws_ec2_node_id::parse(std::string("aws-ec2:us-east-1:") + native));
    }
}

BOOST_AUTO_TEST_CASE(rejects_bad_regions) {
    for (const char* region : {"", "us", "us-east", "US-east-1", "useast1", "us-east-x", "u-east-1",
                               "us--east-1", "us-east-1-"}) {
        BOOST_CHECK_THROW((aws_ec2_node_id{region, "i-1234abcd"}), std::invalid_argument);
    }
}

BOOST_AUTO_TEST_CASE(parse_rejects_malformed_text) {
    for (const char* text :
         {"", "aws-ec2", "aws-ec2:us-east-1", "aws-ec2:us-east-1:i-1234abcd:extra",
          "gcp:us-east-1:i-1234abcd", "AWS-EC2:us-east-1:i-1234abcd", ":us-east-1:i-1234abcd",
          "aws-ec2::i-1234abcd", "aws-ec2:us-east-1:", "aws-ec2:us%2Deast-1:i-1234abcd",
          "aws-ec2:us-east-1:i-1234abcd\n", "12345"}) {
        BOOST_CHECK_MESSAGE(!aws_ec2_node_id::parse(text), "accepted '" << text << "'");
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(other_providers)

BOOST_AUTO_TEST_CASE(alibaba_ecs) {
    alibaba_ecs_node_id id{"cn-hangzhou", "i-bp1abcdefghij0123456"};
    BOOST_CHECK_EQUAL(id.to_string(), "alibaba-ecs:cn-hangzhou:i-bp1abcdefghij0123456");
    check_round_trip(id);
    BOOST_CHECK_THROW((alibaba_ecs_node_id{"cn-hangzhou", "i-BP1"}), std::invalid_argument);
    BOOST_CHECK_THROW((alibaba_ecs_node_id{"hangzhou", "i-bp1"}), std::invalid_argument);
    BOOST_CHECK_THROW((alibaba_ecs_node_id{"cn-hangzhou", "i-"}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(azure_vm_folds_case) {
    azure_vm_node_id upper{"My-RG", "Kythira-VM-1"};
    azure_vm_node_id lower{"my-rg", "kythira-vm-1"};
    BOOST_CHECK(upper == lower);
    BOOST_CHECK_EQUAL(upper.to_string(), "azure-vm:my-rg:kythira-vm-1");
    BOOST_CHECK_EQUAL(std::hash<azure_vm_node_id>{}(upper), std::hash<azure_vm_node_id>{}(lower));
    check_round_trip(upper);
    // Only the canonical (lower-case) spelling parses.
    BOOST_CHECK(!azure_vm_node_id::parse("azure-vm:My-RG:kythira-vm-1"));
    check_round_trip(azure_vm_node_id{"rg(prod).1_x", "vm.name_1-a"});
    BOOST_CHECK_THROW((azure_vm_node_id{"rg", std::string(65, 'a')}), std::invalid_argument);
    BOOST_CHECK_THROW((azure_vm_node_id{std::string(91, 'a'), "vm"}), std::invalid_argument);
    BOOST_CHECK_THROW((azure_vm_node_id{"rg", "vm/1"}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(gcp_domain_scoped_project_is_percent_encoded) {
    gcp_instance_node_id id{"example.com:proj/us-central1-a", "kythira-node-1"};
    BOOST_CHECK_EQUAL(id.to_string(), "gcp:example.com%3Aproj/us-central1-a:kythira-node-1");
    BOOST_CHECK_EQUAL(id.scope(), "example.com:proj/us-central1-a");
    check_round_trip(id);
    // A raw ':' inside the scope is an extra segment, not a project.
    BOOST_CHECK(!gcp_instance_node_id::parse("gcp:example.com:proj/us-central1-a:kythira-node-1"));
    // Lower-case escapes and unknown escapes are not canonical.
    BOOST_CHECK(!gcp_instance_node_id::parse("gcp:example.com%3aproj/us-central1-a:n1"));
    BOOST_CHECK(!gcp_instance_node_id::parse("gcp:example.com%41proj/us-central1-a:n1"));
}

BOOST_AUTO_TEST_CASE(gcp_validation) {
    check_round_trip(gcp_instance_node_id{"my-project/europe-west1-b", "n1"});
    BOOST_CHECK_THROW((gcp_instance_node_id{"my-project", "n1"}), std::invalid_argument);
    BOOST_CHECK_THROW((gcp_instance_node_id{"p/z/x", "n1"}), std::invalid_argument);
    BOOST_CHECK_THROW((gcp_instance_node_id{"p/z", "1node"}), std::invalid_argument);
    BOOST_CHECK_THROW((gcp_instance_node_id{"p/z", "node-"}), std::invalid_argument);
    BOOST_CHECK_THROW((gcp_instance_node_id{"p/z", std::string(64, 'a')}), std::invalid_argument);
    BOOST_CHECK_NO_THROW((gcp_instance_node_id{"p/z", std::string(63, 'a')}));
}

BOOST_AUTO_TEST_CASE(oci_scope_is_the_ocid_region) {
    const std::string ocid = "ocid1.instance.oc1.phx.anyhqljtexampleuniqueid";
    oci_instance_node_id id{"phx", ocid};
    BOOST_CHECK_EQUAL(id.to_string(), "oci:phx:" + ocid);
    check_round_trip(id);
    BOOST_CHECK_THROW((oci_instance_node_id{"iad", ocid}), std::invalid_argument);
    BOOST_CHECK_THROW((oci_instance_node_id{"phx", "ocid1.volume.oc1.phx.abc"}),
                      std::invalid_argument);
    BOOST_CHECK_THROW((oci_instance_node_id{"phx", "ocid1.instance.oc1.phx"}),
                      std::invalid_argument);
    check_round_trip(oci_instance_node_id{"us-ashburn-1", "ocid1.instance.oc1.us-ashburn-1..abc"});
}

BOOST_AUTO_TEST_CASE(docker) {
    docker_container_node_id id{"kythira_test", "Node-1.a"};
    BOOST_CHECK_EQUAL(id.to_string(), "docker:kythira_test:Node-1.a");
    check_round_trip(id);
    BOOST_CHECK_THROW((docker_container_node_id{"Proj", "n"}), std::invalid_argument);
    BOOST_CHECK_THROW((docker_container_node_id{"p", "-n"}), std::invalid_argument);
    BOOST_CHECK_THROW((docker_container_node_id{"p", ""}), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ordering_and_hashing)

BOOST_AUTO_TEST_CASE(order_is_canonical_text_order) {
    std::vector<aws_ec2_node_id> ids{
        {"us-west-2", "i-0000000000000000a"},
        {"eu-west-1", "i-ffffffffffffffff0"},
        {"us-east-1", "i-1234abcd"},
        {"us-east-1", "i-0123456789abcdef0"},
    };
    std::ranges::sort(ids);
    for (std::size_t i = 1; i < ids.size(); ++i) {
        BOOST_CHECK(ids[i - 1].to_string() < ids[i].to_string());
        BOOST_CHECK(ids[i - 1] < ids[i]);
        BOOST_CHECK(ids[i - 1] != ids[i]);
    }
    std::set<aws_ec2_node_id> as_set(ids.begin(), ids.end());
    std::unordered_set<aws_ec2_node_id> as_hash(ids.begin(), ids.end());
    BOOST_CHECK_EQUAL(as_set.size(), ids.size());
    BOOST_CHECK_EQUAL(as_hash.size(), ids.size());
    BOOST_CHECK(as_hash.contains(aws_ec2_node_id{"us-east-1", "i-1234abcd"}));
}

BOOST_AUTO_TEST_CASE(default_constructed_is_empty_and_distinct) {
    aws_ec2_node_id empty;
    BOOST_CHECK(empty.to_string().empty());
    BOOST_CHECK(empty != (aws_ec2_node_id{"us-east-1", "i-1234abcd"}));
    BOOST_CHECK(empty == aws_ec2_node_id{});
}

// Property: every generated valid id round-trips, and parse agrees with the
// comparison operators on canonical text.
BOOST_AUTO_TEST_CASE(random_round_trip_and_order_property) {
    std::mt19937_64 rng(0xC0FFEE);
    const char* hex = "0123456789abcdef";
    const char* regions[] = {"us-east-1", "us-gov-west-1", "eu-central-2", "ap-south-1"};
    std::vector<aws_ec2_node_id> ids;
    for (int i = 0; i < 500; ++i) {
        std::string native = "i-";
        std::size_t len = (rng() % 4 == 0) ? 8 : 17;
        for (std::size_t k = 0; k < len; ++k) {
            native += hex[rng() % 16];
        }
        aws_ec2_node_id id{regions[rng() % 4], native};
        check_round_trip(id);
        ids.push_back(id);
    }
    for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
        const auto& a = ids[i];
        const auto& b = ids[i + 1];
        BOOST_CHECK_EQUAL(a < b, a.to_string() < b.to_string());
        BOOST_CHECK_EQUAL(a == b, a.to_string() == b.to_string());
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(numeric_floor)

BOOST_AUTO_TEST_CASE(allocates_above_both_the_listing_and_every_id_raised) {
    kythira::numeric_node_id_floor floor;
    BOOST_CHECK_EQUAL(floor.next_above(0), 1U);
    BOOST_CHECK_EQUAL(floor.next_above(4), 5U);

    floor.raise(std::uint64_t{7});
    BOOST_CHECK_EQUAL(floor.next_above(4), 8U);
    BOOST_CHECK_EQUAL(floor.next_above(9), 10U);
    // next_above does not raise the floor; only the caller's raise does.
    BOOST_CHECK_EQUAL(floor.value(), 7U);

    // Never lowered.
    floor.raise(std::uint64_t{3});
    BOOST_CHECK_EQUAL(floor.value(), 7U);
}

BOOST_AUTO_TEST_CASE(textual_ids_raise_only_when_strictly_decimal) {
    kythira::numeric_node_id_floor floor;
    floor.raise(std::string{"12"});
    BOOST_CHECK_EQUAL(floor.value(), 12U);
    floor.raise(std::string{"99x"});
    floor.raise(std::string{"-1"});
    floor.raise(std::string{});
    floor.raise(aws_ec2_node_id{"us-east-1", "i-0123456789abcdef0"});
    BOOST_CHECK_EQUAL(floor.value(), 12U);
}

BOOST_AUTO_TEST_CASE(refuses_to_pass_the_ceiling) {
    kythira::numeric_node_id_floor floor;
    floor.raise(std::uint64_t{std::numeric_limits<std::uint32_t>::max()});
    BOOST_CHECK_THROW(
        static_cast<void>(floor.next_above(0, std::numeric_limits<std::uint32_t>::max())),
        std::overflow_error);
    BOOST_CHECK_EQUAL(floor.next_above(0),
                      std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1);
}

BOOST_AUTO_TEST_CASE(copies_share_and_moved_from_floors_stay_usable) {
    kythira::numeric_node_id_floor floor;
    auto copy = floor;
    copy.raise(std::uint64_t{5});
    BOOST_CHECK_EQUAL(floor.value(), 5U);

    // A move copies the shared handle; the test checks the source stays usable.
    // NOLINTNEXTLINE(performance-move-const-arg)
    auto moved = std::move(floor);
    moved.raise(std::uint64_t{6});
    // NOLINTNEXTLINE(bugprone-use-after-move): the point of the check.
    BOOST_CHECK_EQUAL(floor.value(), 6U);
    floor.raise(std::uint64_t{8});
    BOOST_CHECK_EQUAL(moved.value(), 8U);
}

BOOST_AUTO_TEST_SUITE_END()
