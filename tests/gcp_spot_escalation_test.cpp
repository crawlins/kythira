/// @file gcp_spot_escalation_test.cpp
/// @brief Offline unit tests for the spot-first, zone-laddering launch
///        escalation used by `tests/gcp_quorum_manager_real_gce_test.cpp`.
///
/// Links neither google-cloud-cpp nor `network_simulator`, so it builds and
/// runs on every leg of CI with no project, no credentials, and no instances.
///
/// That is load-bearing twice over here, more so than for the Azure equivalent:
///
///  * The google-cloud-cpp `compute` component is absent from this repo's
///    default vcpkg install (only the dedicated `gcp-sdk-build` CI job has it),
///    so `gcp_quorum_manager_real_gce_test.cpp` does not even compile in most
///    environments. Without these cases the escalation logic would have no
///    local test at all.
///  * The behaviour under test only executes when a zone is genuinely out of
///    capacity — the condition that broke the Aug 3 2026 scheduled run and
///    which cannot be scheduled on demand. A green real-GCE run proves nothing
///    about it.
///
/// The error strings below are the real ones GCE returns, quoted from that
/// run's job log.

#define BOOST_TEST_MODULE gcp_spot_escalation_test
#include <boost/test/unit_test.hpp>

#include "gcp_real_gce_test_support.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace kythira::testing::gcp_real_gce;

namespace {

/// Three zones of a region, with a deliberately uneven machine-type offering:
/// `e2-small` is everywhere, `e2-medium` is missing from `-c`, and `n1-` only
/// appears in `-a`. `e2-micro` is present everywhere but below the memory
/// floor, and `a2-highgpu-1g` is present to be excluded by family.
auto sample_machine_types() -> std::vector<gcp_machine_type_info> {
    std::vector<gcp_machine_type_info> out;
    for (const auto* zone : {"us-central1-a", "us-central1-b", "us-central1-c"}) {
        out.push_back({.name = "e2-micro", .zone = zone, .guest_cpus = 2, .memory_mb = 1024});
        out.push_back({.name = "e2-small", .zone = zone, .guest_cpus = 2, .memory_mb = 2048});
        out.push_back(
            {.name = "a2-highgpu-1g", .zone = zone, .guest_cpus = 12, .memory_mb = 85000});
    }
    out.push_back(
        {.name = "e2-medium", .zone = "us-central1-a", .guest_cpus = 2, .memory_mb = 4096});
    out.push_back(
        {.name = "e2-medium", .zone = "us-central1-b", .guest_cpus = 2, .memory_mb = 4096});
    out.push_back(
        {.name = "n1-standard-1", .zone = "us-central1-a", .guest_cpus = 1, .memory_mb = 3840});
    return out;
}

auto zones_of(const std::vector<gcp_launch_option>& ladder, const std::string& machine_type,
              bool spot) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const auto& rung : ladder) {
        if (rung.machine_type == machine_type && rung.spot == spot) {
            out.push_back(rung.zone);
        }
    }
    return out;
}

const std::vector<gcp_launch_option> kLadder{
    {.machine_type = "e2-small", .zone = "us-central1-c", .spot = true, .price_per_hr = 0.0067},
    {.machine_type = "e2-small", .zone = "us-central1-a", .spot = true, .price_per_hr = 0.0067},
    {.machine_type = "e2-small", .zone = "us-central1-b", .spot = true, .price_per_hr = 0.0067},
    {.machine_type = "e2-medium", .zone = "us-central1-c", .spot = true, .price_per_hr = 0.0134},
    {.machine_type = "e2-small", .zone = "us-central1-c", .spot = false, .price_per_hr = 0.0168},
};

auto noop_escalate = [](const gcp_launch_option&, const gcp_launch_option&, const char*) {};

/// The exact text GCE returned in the Aug 3 2026 run, as the quorum manager
/// flattens it into a `std::runtime_error`.
constexpr const char* kZoneExhausted =
    "gcp_compute_quorum_manager::provision_node: gcp zone operation "
    "'operation-1785753842982-6582237779dde-8b2570fd-969bb8f1' failed: "
    "[ZONE_RESOURCE_POOL_EXHAUSTED] The zone "
    "'projects/prefab-sky-500619-s9/zones/us-central1-c' does not have enough resources "
    "available to fulfill the request.  Try a different zone, or try again later.;";

}  // namespace

BOOST_AUTO_TEST_SUITE(gcp_error_classification)

BOOST_AUTO_TEST_CASE(advances_on_the_real_zone_exhaustion_message) {
    BOOST_TEST(is_gcp_capacity_or_quota_error(kZoneExhausted));
}

BOOST_AUTO_TEST_CASE(advances_on_capacity_and_quota_variants) {
    const std::vector<std::string> retryable{
        "[ZONE_RESOURCE_POOL_EXHAUSTED_WITH_DETAILS] The zone does not have enough resources",
        "[RESOURCE_POOL_EXHAUSTED] The resource pool is exhausted",
        "Quota 'CPUS' exceeded. Limit: 8.0 in region us-central1.",
        "[QUOTA_EXCEEDED] quota exceeded for resource CPUS",
        "[RATE_LIMIT_EXCEEDED] Operation rate exceeded",
    };
    for (const auto& message : retryable) {
        BOOST_TEST(is_gcp_capacity_or_quota_error(message), "should advance: " + message);
    }
}

BOOST_AUTO_TEST_CASE(aborts_on_configuration_and_permission_errors) {
    // A wrong image, subnetwork, or service account must fail on the first rung
    // rather than walk the whole ladder proving the same thing repeatedly.
    const std::vector<std::string> fatal{
        "[RESOURCE_NOT_FOUND] The resource 'projects/x/global/images/nope' was not found",
        "[PERMISSION_DENIED] Required 'compute.instances.create' permission",
        "[INVALID_FIELD_VALUE] Invalid value for field 'resource.machineType'",
        "[CONDITION_NOT_MET] Constraint constraints/compute.vmExternalIpAccess violated",
        "gcp instances.insert: UNAUTHENTICATED: request had invalid authentication credentials",
        "provision timeout waiting for instance to reach RUNNING",
    };
    for (const auto& message : fatal) {
        BOOST_TEST(!is_gcp_capacity_or_quota_error(message), "should abort: " + message);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(gcp_machine_type_eligibility)

BOOST_AUTO_TEST_CASE(honours_cpu_memory_and_family) {
    const gcp_machine_type_requirements req;  // 2 vCPU, 2048 MiB, general-purpose families

    BOOST_TEST(gcp_machine_type_is_eligible(
        {.name = "e2-small", .zone = "z", .guest_cpus = 2, .memory_mb = 2048}, req));

    // Below the memory floor: cheaper than e2-small, but the ladder may only
    // substitute something at least as capable as what the suite launched
    // before escalation existed.
    BOOST_TEST(!gcp_machine_type_is_eligible(
        {.name = "e2-micro", .zone = "z", .guest_cpus = 2, .memory_mb = 1024}, req));

    // Outside the allowlisted families: accelerator/metal types need extra
    // required fields the quorum manager does not set, so they would fail with
    // a hard error the classifier deliberately does not retry.
    BOOST_TEST(!gcp_machine_type_is_eligible(
        {.name = "a2-highgpu-1g", .zone = "z", .guest_cpus = 2, .memory_mb = 8192}, req));

    // Too many vCPUs.
    BOOST_TEST(!gcp_machine_type_is_eligible(
        {.name = "e2-standard-4", .zone = "z", .guest_cpus = 4, .memory_mb = 16384}, req));

    // Missing capability data is treated as ineligible rather than assumed.
    BOOST_TEST(!gcp_machine_type_is_eligible(
        {.name = "e2-small", .zone = "z", .guest_cpus = 0, .memory_mb = 2048}, req));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(gcp_ladder_ranking)

BOOST_AUTO_TEST_CASE(tries_every_zone_before_growing_the_machine_type) {
    // The central ordering property. Changing zone is free; changing machine
    // type costs money. So all of e2-small's zones must be exhausted before
    // e2-medium is reached at all.
    auto ladder = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                          "us-central1-c", {});
    BOOST_REQUIRE(!ladder.empty());

    std::size_t first_medium = ladder.size();
    std::size_t last_small = 0;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (!ladder[i].spot) {
            break;  // confine this check to the spot half
        }
        if (ladder[i].machine_type == "e2-small") {
            last_small = i;
        }
        if (ladder[i].machine_type == "e2-medium" && first_medium == ladder.size()) {
            first_medium = i;
        }
    }
    // Guard against a vacuous pass: if e2-medium never made it into the spot
    // half at all, `first_medium` would still be ladder.size() and the ordering
    // check below would hold for the wrong reason.
    BOOST_REQUIRE_MESSAGE(first_medium < ladder.size(),
                          "e2-medium is absent from the spot half — the ordering assertion "
                          "below would pass vacuously");
    BOOST_TEST(last_small < first_medium);
    BOOST_TEST(zones_of(ladder, "e2-small", true).size() == 3u);
    // e2-medium is offered in only two of the three zones in the fixture, so
    // this also pins that the ladder emits rungs per *available* zone rather
    // than assuming every type exists everywhere.
    BOOST_TEST(zones_of(ladder, "e2-medium", true).size() == 2u);
}

BOOST_AUTO_TEST_CASE(puts_the_preferred_zone_first) {
    // An ordinary run must land exactly where the caller asked; the ladder is
    // only allowed to matter once something is actually wrong.
    auto ladder = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                          "us-central1-c", {});
    BOOST_REQUIRE(!ladder.empty());
    BOOST_TEST(ladder.front().zone == "us-central1-c");
    BOOST_TEST(ladder.front().machine_type == "e2-small");
    BOOST_TEST(ladder.front().spot);

    auto other = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                         "us-central1-b", {});
    BOOST_REQUIRE(!other.empty());
    BOOST_TEST(other.front().zone == "us-central1-b");
}

BOOST_AUTO_TEST_CASE(spot_rungs_all_precede_on_demand_rungs) {
    auto ladder = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                          "us-central1-a", {});
    bool seen_on_demand = false;
    for (const auto& rung : ladder) {
        if (!rung.spot) {
            seen_on_demand = true;
        } else {
            BOOST_TEST(!seen_on_demand,
                       "spot rung " << rung.label() << " follows an on-demand one");
        }
    }
    BOOST_TEST(seen_on_demand, "ladder must end with on-demand rungs as the fallback");
}

BOOST_AUTO_TEST_CASE(excluded_zones_never_appear) {
    // GroupId is the zone in this manager, so a multi-zone case needs N
    // *distinct* zones. A substitute that reused an occupied zone would
    // silently collapse two groups into one and weaken what the case tests.
    auto ladder = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                          "us-central1-c", {"us-central1-a", "us-central1-b"});
    BOOST_REQUIRE(!ladder.empty());
    for (const auto& rung : ladder) {
        BOOST_TEST(rung.zone == "us-central1-c");
    }
    // e2-medium is not offered in -c in this fixture, so excluding the other
    // two zones must drop it entirely rather than emit an unlaunchable rung.
    BOOST_TEST(zones_of(ladder, "e2-medium", true).empty());
}

BOOST_AUTO_TEST_CASE(machine_types_are_ordered_by_price) {
    auto ladder = rank_gcp_launch_options(sample_machine_types(), gcp_machine_type_requirements{},
                                          "us-central1-a", {});
    double previous = -1.0;
    for (const auto& rung : ladder) {
        if (!rung.spot) {
            break;
        }
        BOOST_TEST(rung.price_per_hr >= previous, "rung " << rung.label() << " is out of order");
        previous = rung.price_per_hr;
    }
}

BOOST_AUTO_TEST_CASE(empty_when_nothing_is_eligible) {
    // No launchable machine type must yield no ladder, so the caller falls back
    // to its configured default rather than launching something unvetted.
    std::vector<gcp_machine_type_info> only_gpus{
        {.name = "a2-highgpu-1g", .zone = "us-central1-a", .guest_cpus = 2, .memory_mb = 8192}};
    BOOST_TEST(
        rank_gcp_launch_options(only_gpus, gcp_machine_type_requirements{}, "us-central1-a", {})
            .empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(gcp_escalation_walk)

BOOST_AUTO_TEST_CASE(uses_the_first_rung_when_it_launches) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    auto chosen = escalate_gcp_launch(
        kLadder, rung, escalations,
        [&](const gcp_launch_option& option) {
            attempted.push_back(option.label());
            return option.zone;
        },
        noop_escalate);

    BOOST_TEST(chosen == "us-central1-c");
    BOOST_TEST(escalations == 0u);
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
}

BOOST_AUTO_TEST_CASE(advances_to_another_zone_on_the_real_stockout) {
    // The Aug 3 failure, replayed: us-central1-c is out of e2-small, and the
    // fix is to land the node in another zone rather than fail the case.
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    auto chosen = escalate_gcp_launch(
        kLadder, rung, escalations,
        [&](const gcp_launch_option& option) -> std::string {
            attempted.push_back(option.zone);
            if (option.zone == "us-central1-c") {
                throw std::runtime_error(kZoneExhausted);
            }
            return option.zone;
        },
        noop_escalate);

    BOOST_TEST(chosen == "us-central1-a");
    BOOST_TEST(escalations == 1u);
    // Same machine type, different zone — it must not have jumped to a costlier
    // instance when a free move was available.
    BOOST_TEST(kLadder[rung].machine_type == "e2-small");
    const std::vector<std::string> expected{"us-central1-c", "us-central1-a"};
    BOOST_TEST(attempted == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(grows_the_machine_type_once_every_zone_is_exhausted) {
    std::size_t rung = 0;
    std::size_t escalations = 0;

    auto chosen = escalate_gcp_launch(
        kLadder, rung, escalations,
        [&](const gcp_launch_option& option) -> std::string {
            if (option.machine_type == "e2-small" && option.spot) {
                throw std::runtime_error(kZoneExhausted);
            }
            return option.machine_type;
        },
        noop_escalate);

    BOOST_TEST(chosen == "e2-medium");
    BOOST_TEST(escalations == 3u);
}

BOOST_AUTO_TEST_CASE(falls_through_to_on_demand) {
    std::size_t rung = 0;
    std::size_t escalations = 0;

    auto chosen = escalate_gcp_launch(
        kLadder, rung, escalations,
        [&](const gcp_launch_option& option) -> std::string {
            if (option.spot) {
                throw std::runtime_error("[ZONE_RESOURCE_POOL_EXHAUSTED] no capacity");
            }
            return option.label();
        },
        noop_escalate);

    BOOST_TEST(!kLadder[rung].spot);
    BOOST_TEST(chosen == kLadder.back().label());
    BOOST_TEST(escalations == 4u);
}

BOOST_AUTO_TEST_CASE(rethrows_immediately_on_a_non_capacity_error) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    BOOST_CHECK_THROW(escalate_gcp_launch(
                          kLadder, rung, escalations,
                          [&](const gcp_launch_option& option) -> std::string {
                              attempted.push_back(option.zone);
                              throw std::runtime_error(
                                  "[PERMISSION_DENIED] Required "
                                  "'compute.instances.create' permission");
                          },
                          noop_escalate),
                      std::runtime_error);

    // The control that gives this case teeth: exactly one attempt. A classifier
    // that matched too broadly would show five.
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
    BOOST_TEST(rung == 0u);
    BOOST_TEST(escalations == 0u);
}

BOOST_AUTO_TEST_CASE(rethrows_the_last_refusal_once_the_ladder_is_exhausted) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    BOOST_CHECK_THROW(escalate_gcp_launch(
                          kLadder, rung, escalations,
                          [&](const gcp_launch_option& option) -> std::string {
                              attempted.push_back(option.zone);
                              throw std::runtime_error(kZoneExhausted);
                          },
                          noop_escalate),
                      std::runtime_error);
    BOOST_REQUIRE_EQUAL(attempted.size(), kLadder.size());
    BOOST_TEST(escalations == kLadder.size() - 1);
}

BOOST_AUTO_TEST_CASE(resumes_from_the_rung_the_previous_call_settled_on) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    auto attempt = [&](const gcp_launch_option& option) -> std::string {
        attempted.push_back(option.zone);
        if (option.zone == "us-central1-c") {
            throw std::runtime_error(kZoneExhausted);
        }
        return option.zone;
    };

    BOOST_TEST(escalate_gcp_launch(kLadder, rung, escalations, attempt, noop_escalate) ==
               "us-central1-a");
    attempted.clear();

    // A zone that is out of capacity stays out for the rest of the case; the
    // second node must not re-pay the failure to rediscover that.
    BOOST_TEST(escalate_gcp_launch(kLadder, rung, escalations, attempt, noop_escalate) ==
               "us-central1-a");
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
    BOOST_TEST(escalations == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
