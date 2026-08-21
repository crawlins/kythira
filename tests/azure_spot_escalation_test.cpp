// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file azure_spot_escalation_test.cpp
/// @brief Offline unit tests for the spot-first VM launch escalation used by
///        `tests/azure_quorum_manager_real_test.cpp`.
///
/// This deliberately links neither the Azure SDK nor `network_simulator`, so
/// `KYTHIRA_HAS_AZURE_SDK` is undefined and only the *pure* half of
/// `azure_real_test_support.hpp` (price/SKU parsing, ranking, the error
/// classifier, and the escalation walk) is compiled in. Everything under test
/// here is a total function of its inputs, so it can be exercised
/// deterministically with no subscription, no credentials, and no VMs.
///
/// That matters more than usual for this feature. The escalation path only
/// executes on a real run that *happens* to hit a capacity shortage or a quota
/// refusal — a run you cannot schedule and, on a healthy day, will never
/// observe. A green real-Azure run therefore proves nothing about escalation;
/// these cases are where "it advances on capacity, advances on quota, and
/// aborts on anything else" is actually established. The real suite's
/// `AZURE_TEST_FORCE_FIRST_VM_SIZE` knob covers the same ground end-to-end
/// against live ARM.
///
/// The JSON fixtures are trimmed from genuine eastus responses (Retail Prices
/// API and `Microsoft.Compute/skus`, captured August 2026), including the
/// specific shapes that made naive versions of this code rank unlaunchable
/// SKUs first.

#define BOOST_TEST_MODULE azure_spot_escalation_test
#include <boost/test/unit_test.hpp>

#include "azure_real_test_support.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace kythira::testing::azure_real;

namespace {

/// One Retail Prices page carrying every trap the real feed contains:
/// a Windows row sharing an `armSkuName` with its Linux counterpart, a retired
/// "Low Priority" row, two rows for the same `armSkuName` at different prices,
/// a zero-price row, and a `NextPageLink`.
constexpr const char* kPricePage = R"({
  "Items": [
    {"armSkuName":"Standard_F1als_v7","skuName":"F1als v7 Spot","productName":"Virtual Machines FALSv7 Series","retailPrice":0.01118},
    {"armSkuName":"Standard_F1als_v7","skuName":"F1als v7","productName":"Virtual Machines FALSv7 Series","retailPrice":0.0605},
    {"armSkuName":"Standard_F1als_v7","skuName":"F1als v7 Spot","productName":"Virtual Machines FALSv7 Series Windows","retailPrice":0.0912},
    {"armSkuName":"Standard_D2als_v7","skuName":"D2als v7 Spot","productName":"Virtual Machines DALSv7 Series","retailPrice":0.01507},
    {"armSkuName":"Standard_D2als_v7","skuName":"D2als v7 Spot","productName":"Virtual Machines DALSv7 Series","retailPrice":0.02200},
    {"armSkuName":"Standard_D2als_v7","skuName":"D2als v7","productName":"Virtual Machines DALSv7 Series","retailPrice":0.0860},
    {"armSkuName":"Standard_D2s_v5","skuName":"D2s v5 Low Priority","productName":"Virtual Machines Dsv5 Series","retailPrice":0.00192},
    {"armSkuName":"Standard_M8_v4","skuName":"M8 v4 Spot","productName":"Virtual Machines Mv4 Series","retailPrice":0.00200},
    {"armSkuName":"Standard_M8_v4","skuName":"M8 v4","productName":"Virtual Machines Mv4 Series","retailPrice":2.4000},
    {"armSkuName":"Standard_B2pts_v2","skuName":"B2pts v2 Spot","productName":"Virtual Machines BPSv2 Series","retailPrice":0.00756},
    {"armSkuName":"Standard_B2pts_v2","skuName":"B2pts v2","productName":"Virtual Machines BPSv2 Series","retailPrice":0.00840},
    {"armSkuName":"Standard_DC2as_v5","skuName":"DC2as v5 Spot","productName":"Virtual Machines DCASv5 Series","retailPrice":0.01816},
    {"armSkuName":"Standard_DC2as_v5","skuName":"DC2as v5","productName":"Virtual Machines DCASv5 Series","retailPrice":0.0860},
    {"armSkuName":"","skuName":"Nameless Spot","productName":"Virtual Machines","retailPrice":0.00001},
    {"armSkuName":"Standard_Free_X","skuName":"Free X","productName":"Virtual Machines","retailPrice":0}
  ],
  "NextPageLink": "https://prices.azure.com/api/retail/prices?page=2"
})";

constexpr const char* kLastPricePage = R"({
  "Items": [
    {"armSkuName":"Standard_F1as_v7","skuName":"F1as v7 Spot","productName":"Virtual Machines FASv7 Series","retailPrice":0.01262},
    {"armSkuName":"Standard_F1as_v7","skuName":"F1as v7","productName":"Virtual Machines FASv7 Series","retailPrice":0.0700}
  ]
})";

/// `Microsoft.Compute/skus` fixture matching the SKUs above:
///  * `Standard_M8_v4` is restricted (`NotAvailableForSubscription`) — this is
///    the entry that, unjoined, would head the ladder at $0.002/hr.
///  * `Standard_B2pts_v2` is Arm64 — cheaper than every x64 spot price, and
///    unbootable by the suite's x86-64 image.
///  * `Standard_DC2as_v5` is Gen1+Gen2 but 2 vCPU; `Standard_D2s_v5` is absent
///    from the region listing entirely.
///  * `Standard_F1alsx_v7` is zone-less (regional only).
constexpr const char* kSkus = R"({
  "value": [
    {"resourceType":"virtualMachines","name":"Standard_F1als_v7",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"1"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_F1as_v7",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"1"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_D2als_v7",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"2"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_M8_v4",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "restrictions":[{"type":"Location","reasonCode":"NotAvailableForSubscription"}],
     "capabilities":[{"name":"vCPUs","value":"8"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V1,V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_B2pts_v2",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"2"},{"name":"CpuArchitectureType","value":"Arm64"},{"name":"HyperVGenerations","value":"V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_DC2as_v5",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"2"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V1,V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_F1alsx_v7",
     "locationInfo":[{"location":"eastus","zones":[]}],
     "capabilities":[{"name":"vCPUs","value":"1"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V2"}]},
    {"resourceType":"virtualMachines","name":"Standard_DC2ads_v5",
     "locationInfo":[{"location":"eastus","zones":["1","2","3"]}],
     "capabilities":[{"name":"vCPUs","value":"2"},{"name":"CpuArchitectureType","value":"x64"},{"name":"HyperVGenerations","value":"V1,V2"},{"name":"ConfidentialComputingType","value":"SNP"}]},
    {"resourceType":"disks","name":"Premium_LRS","locationInfo":[{"location":"eastus","zones":["1"]}]}
  ]
})";

auto parse(const char* json) -> boost::json::value {
    return boost::json::parse(json);
}

struct prices {
    std::map<std::string, double> spot;
    std::map<std::string, double> on_demand;
};

auto load_prices() -> prices {
    prices p;
    auto next = absorb_retail_price_page(parse(kPricePage), p.spot, p.on_demand);
    BOOST_TEST(next == "https://prices.azure.com/api/retail/prices?page=2");
    next = absorb_retail_price_page(parse(kLastPricePage), p.spot, p.on_demand);
    BOOST_TEST(next.empty());
    return p;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(retail_price_parsing)

BOOST_AUTO_TEST_CASE(splits_spot_from_on_demand) {
    auto p = load_prices();
    BOOST_TEST(p.spot.at("Standard_F1als_v7") == 0.01118, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(p.on_demand.at("Standard_F1als_v7") == 0.0605, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(drops_windows_rows) {
    auto p = load_prices();
    // The Windows spot row for Standard_F1als_v7 is $0.0912 — far above the
    // Linux one. Taking the minimum would hide the bug, so assert the Linux
    // price survived *and* that nothing recorded the Windows price.
    BOOST_TEST(p.spot.at("Standard_F1als_v7") == 0.01118, boost::test_tools::tolerance(1e-9));
    for (const auto& [sku, price] : p.spot) {
        BOOST_TEST(price != 0.0912, "Windows row leaked into spot prices for " + sku);
    }
}

BOOST_AUTO_TEST_CASE(drops_low_priority_and_unpriced_and_nameless_rows) {
    auto p = load_prices();
    // "Low Priority" is a retired, unpurchasable tier that otherwise looks
    // like an extremely cheap spot row.
    BOOST_TEST(!p.spot.contains("Standard_D2s_v5"));
    BOOST_TEST(!p.on_demand.contains("Standard_D2s_v5"));
    BOOST_TEST(!p.on_demand.contains("Standard_Free_X"));
    BOOST_TEST(!p.spot.contains(""));
}

BOOST_AUTO_TEST_CASE(takes_the_minimum_of_duplicate_rows) {
    auto p = load_prices();
    // Standard_D2als_v7 appears twice as spot, at 0.01507 and 0.02200.
    BOOST_TEST(p.spot.at("Standard_D2als_v7") == 0.01507, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(accumulates_across_pages) {
    auto p = load_prices();
    BOOST_TEST(p.spot.at("Standard_F1as_v7") == 0.01262, boost::test_tools::tolerance(1e-9));
    BOOST_TEST(p.on_demand.at("Standard_F1as_v7") == 0.0700, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(compute_sku_parsing)

BOOST_AUTO_TEST_CASE(reads_capabilities_zones_and_restrictions) {
    auto facts = parse_compute_skus(parse(kSkus));

    // Non-virtualMachines resource types are excluded entirely.
    BOOST_TEST(!facts.contains("Premium_LRS"));

    const auto& f1als = facts.at("Standard_F1als_v7");
    BOOST_TEST(f1als.vcpus == 1u);
    BOOST_TEST(f1als.architecture == "x64");
    BOOST_TEST(f1als.hyperv_generations == "V2");
    BOOST_TEST(f1als.zonal);
    BOOST_TEST(!f1als.restricted);

    BOOST_TEST(facts.at("Standard_M8_v4").restricted);
    BOOST_TEST(facts.at("Standard_B2pts_v2").architecture == "Arm64");
    BOOST_TEST(!facts.at("Standard_F1alsx_v7").zonal);
}

BOOST_AUTO_TEST_CASE(launchability_honours_every_requirement) {
    auto facts = parse_compute_skus(parse(kSkus));
    const azure_sku_requirements req;  // 2 vCPU, x64, V2

    BOOST_TEST(sku_is_launchable(facts.at("Standard_F1als_v7"), req));
    BOOST_TEST(sku_is_launchable(facts.at("Standard_D2als_v7"), req));
    BOOST_TEST(!sku_is_launchable(facts.at("Standard_M8_v4"), req));      // restricted
    BOOST_TEST(!sku_is_launchable(facts.at("Standard_B2pts_v2"), req));   // Arm64
    BOOST_TEST(!sku_is_launchable(facts.at("Standard_F1alsx_v7"), req));  // no zones

    // Gen1-only requirement excludes the Gen2-only families and keeps the
    // V1,V2 one — the pairing that makes a `22_04-lts` image work.
    azure_sku_requirements gen1{.max_vcpus = 2, .architecture = "x64", .hyperv_generation = "V1"};
    BOOST_TEST(!sku_is_launchable(facts.at("Standard_F1als_v7"), gen1));
    BOOST_TEST(sku_is_launchable(facts.at("Standard_DC2as_v5"), gen1));

    // A 1-vCPU ceiling drops the 2-vCPU SKUs.
    azure_sku_requirements tiny{.max_vcpus = 1, .architecture = "x64", .hyperv_generation = "V2"};
    BOOST_TEST(sku_is_launchable(facts.at("Standard_F1als_v7"), tiny));
    BOOST_TEST(!sku_is_launchable(facts.at("Standard_D2als_v7"), tiny));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ladder_ranking)

BOOST_AUTO_TEST_CASE(ranks_spot_ascending_then_appends_on_demand_fallback) {
    auto p = load_prices();
    auto facts = parse_compute_skus(parse(kSkus));
    auto ladder = rank_spot_first(p.spot, p.on_demand, facts, azure_sku_requirements{});

    BOOST_REQUIRE(!ladder.empty());
    // Cheapest launchable on-demand is Standard_F1als_v7 at $0.0605; every
    // launchable spot SKU below that, ascending, then that on-demand rung.
    const std::vector<std::string> expected{"Standard_F1als_v7", "Standard_F1as_v7",
                                            "Standard_D2als_v7", "Standard_DC2as_v5",
                                            "Standard_F1als_v7"};
    BOOST_REQUIRE_EQUAL(ladder.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BOOST_TEST(ladder[i].vm_size == expected[i], "rung " << i);
    }
    for (std::size_t i = 0; i + 1 < ladder.size(); ++i) {
        BOOST_TEST(ladder[i].spot, "rung " << i << " should be spot");
        BOOST_TEST(ladder[i].price_per_hr <= ladder[i + 1].price_per_hr, "rung " << i);
    }
    BOOST_TEST(!ladder.back().spot);
    BOOST_TEST(ladder.back().price_per_hr == 0.0605, boost::test_tools::tolerance(1e-9));
}

BOOST_AUTO_TEST_CASE(excludes_restricted_and_wrong_architecture_skus) {
    auto p = load_prices();
    auto facts = parse_compute_skus(parse(kSkus));
    auto ladder = rank_spot_first(p.spot, p.on_demand, facts, azure_sku_requirements{});

    for (const auto& rung : ladder) {
        // Standard_M8_v4 at $0.002 is the cheapest spot price in the feed by a
        // wide margin and would head an unjoined ranking; it is
        // NotAvailableForSubscription. Standard_B2pts_v2 at $0.00756 is
        // cheaper than every x64 spot rung and is Arm64.
        BOOST_TEST(rung.vm_size != "Standard_M8_v4");
        BOOST_TEST(rung.vm_size != "Standard_B2pts_v2");
    }
}

BOOST_AUTO_TEST_CASE(truncates_spot_rungs_at_the_cheapest_on_demand_price) {
    auto p = load_prices();
    auto facts = parse_compute_skus(parse(kSkus));
    // Drop every on-demand row but the expensive DC2as one, so the truncation
    // point moves and the ladder must grow to include rungs it previously cut.
    std::map<std::string, double> on_demand{{"Standard_DC2as_v5", 0.0180}};
    auto ladder = rank_spot_first(p.spot, on_demand, facts, azure_sku_requirements{});

    BOOST_REQUIRE_EQUAL(ladder.size(), 4u);
    BOOST_TEST(ladder[0].vm_size == "Standard_F1als_v7");  // $0.01118 spot
    BOOST_TEST(ladder[1].vm_size == "Standard_F1as_v7");   // $0.01262 spot
    BOOST_TEST(ladder[2].vm_size == "Standard_D2als_v7");  // $0.01507 spot
    BOOST_TEST(ladder[3].vm_size == "Standard_DC2as_v5");  // $0.0180 on-demand
    BOOST_TEST(!ladder[3].spot);
    // The truncation this case exists to pin down: Standard_DC2as_v5 *spot*
    // costs $0.01816, above the $0.0180 on-demand cut, so it is dropped even
    // though it is a spot rung and passes every launchability check. With the
    // default on-demand set (cut at $0.0605) it survives — see the ranking
    // case above, where it is the last spot rung.
    for (std::size_t i = 0; i + 1 < ladder.size(); ++i) {
        BOOST_TEST(ladder[i].spot);
        BOOST_TEST(ladder[i].price_per_hr < 0.0180);
    }
}

BOOST_AUTO_TEST_CASE(returns_empty_when_no_on_demand_sku_is_launchable) {
    auto p = load_prices();
    auto facts = parse_compute_skus(parse(kSkus));
    // No launchable on-demand SKU means there is no terminal fallback and so
    // no honest ladder; the caller must fall back to its configured default
    // rather than launch something unvetted.
    BOOST_TEST(rank_spot_first(p.spot, {}, facts, azure_sku_requirements{}).empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(error_classification)

BOOST_AUTO_TEST_CASE(advances_on_real_capacity_and_quota_messages) {
    // Every string here is the `what()` an azure_vm_quorum_manager launch
    // failure actually produces, i.e. the ARM message flattened into
    // "VM creation failed: …".
    const std::vector<std::string> retryable{
        "VM creation failed: ARM request failed (409) AllocationFailed: Allocation failed. We "
        "do not have sufficient capacity for the requested VM size in this region.",
        "VM creation failed: ARM request failed (409) ZonalAllocationFailed: Allocation failed. "
        "We do not have sufficient capacity for the requested VM size in this zone.",
        "VM creation failed: ARM request failed (409) OperationNotAllowed: Operation could not "
        "be completed as it results in exceeding approved standardFSv7Family Cores quota.",
        "VM creation failed: ARM request failed (409) QuotaExceeded: subscription quota "
        "exceeded",
        "VM creation failed: ARM request failed (400) SkuNotAvailable: The requested size for "
        "resource is currently not available in location 'eastus' zones '1'.",
        "VM creation failed: ARM request failed (409) OverconstrainedAllocationRequest",
        "VM creation failed: SkuNotAvailableForSubscription: this SKU is not available for "
        "subscription in this region",
    };
    for (const auto& message : retryable) {
        BOOST_TEST(is_capacity_or_quota_error(message), "should advance: " + message);
    }
}

BOOST_AUTO_TEST_CASE(aborts_on_configuration_and_auth_errors) {
    // A misconfigured image, subnet, or credential must fail the test loudly
    // on the first rung rather than silently burning through 40 SKUs first.
    const std::vector<std::string> fatal{
        "VM creation failed: ARM request failed (400) BadRequest: The selected VM size "
        "'Standard_F1als_v7' cannot boot Hypervisor Generation '1'.",
        "VM creation failed: ARM request failed (404) NotFound: subnet not found",
        "VM creation failed: ARM request failed (403) AuthorizationFailed: does not have "
        "authorization to perform action 'Microsoft.Compute/virtualMachines/write'",
        "VM creation failed: ARM request failed (400) InvalidParameter: osProfile.adminUsername "
        "is not allowed",
        "NIC creation failed: ARM request failed (400) InvalidResourceReference",
        "provision timeout waiting for kythira-x to run",
    };
    for (const auto& message : fatal) {
        BOOST_TEST(!is_capacity_or_quota_error(message), "should abort: " + message);
    }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(escalation_walk)

namespace {

const std::vector<azure_vm_option> kLadder{
    {.vm_size = "spot-a", .spot = true, .price_per_hr = 0.01},
    {.vm_size = "spot-b", .spot = true, .price_per_hr = 0.02},
    {.vm_size = "spot-c", .spot = true, .price_per_hr = 0.03},
    {.vm_size = "on-demand", .spot = false, .price_per_hr = 0.06},
};

auto noop_escalate = [](const azure_vm_option&, const azure_vm_option&, const char*) {};

}  // namespace

BOOST_AUTO_TEST_CASE(uses_the_first_rung_when_it_launches) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    auto chosen = escalate_launch(
        kLadder, rung, escalations,
        [&](const azure_vm_option& option) {
            attempted.push_back(option.vm_size);
            return option.vm_size;
        },
        noop_escalate);

    BOOST_TEST(chosen == "spot-a");
    BOOST_TEST(rung == 0u);
    BOOST_TEST(escalations == 0u);
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
}

BOOST_AUTO_TEST_CASE(advances_past_capacity_then_quota_refusals) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;
    std::vector<std::string> escalated_to;

    auto chosen = escalate_launch(
        kLadder, rung, escalations,
        [&](const azure_vm_option& option) -> std::string {
            attempted.push_back(option.vm_size);
            if (option.vm_size == "spot-a") {
                throw std::runtime_error(
                    "VM creation failed: AllocationFailed: Allocation "
                    "failed. We do not have sufficient capacity.");
            }
            if (option.vm_size == "spot-b") {
                throw std::runtime_error(
                    "VM creation failed: OperationNotAllowed: exceeding "
                    "approved standardFSv7Family Cores quota");
            }
            return option.vm_size;
        },
        [&](const azure_vm_option&, const azure_vm_option& to, const char*) {
            escalated_to.push_back(to.vm_size);
        });

    BOOST_TEST(chosen == "spot-c");
    BOOST_TEST(rung == 2u);
    BOOST_TEST(escalations == 2u);
    const std::vector<std::string> expected_attempts{"spot-a", "spot-b", "spot-c"};
    BOOST_TEST(attempted == expected_attempts, boost::test_tools::per_element());
    const std::vector<std::string> expected_escalations{"spot-b", "spot-c"};
    BOOST_TEST(escalated_to == expected_escalations, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(falls_all_the_way_through_to_on_demand) {
    std::size_t rung = 0;
    std::size_t escalations = 0;

    auto chosen = escalate_launch(
        kLadder, rung, escalations,
        [&](const azure_vm_option& option) -> std::string {
            if (option.spot) {
                throw std::runtime_error(
                    "VM creation failed: AllocationFailed: insufficient "
                    "capacity");
            }
            return option.vm_size;
        },
        noop_escalate);

    BOOST_TEST(chosen == "on-demand");
    BOOST_TEST(escalations == 3u);
    BOOST_TEST(!kLadder[rung].spot);
}

BOOST_AUTO_TEST_CASE(spot_quota_refusal_jumps_straight_to_on_demand) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    // LowPriorityCores is one region-wide counter shared by every spot SKU, so
    // proving it exhausted once proves it for all of them. Walking the
    // remaining spot rungs individually is what a live nine-VM case actually
    // did — 40 consecutive identical refusals before reaching the fallback.
    auto chosen = escalate_launch(
        kLadder, rung, escalations,
        [&](const azure_vm_option& option) -> std::string {
            attempted.push_back(option.vm_size);
            if (option.spot) {
                throw std::runtime_error(
                    "VM creation failed: OperationNotAllowed: Operation could not be completed "
                    "as it results in exceeding approved LowPriorityCores quota. Current Limit: "
                    "3, Current Usage: 3");
            }
            return option.vm_size;
        },
        noop_escalate);

    BOOST_TEST(chosen == "on-demand");
    // The whole point: two attempts (first spot rung, then on-demand), not one
    // per spot rung.
    BOOST_REQUIRE_EQUAL(attempted.size(), 2u);
    BOOST_TEST(attempted[0] == "spot-a");
    BOOST_TEST(attempted[1] == "on-demand");
    BOOST_TEST(escalations == 1u);
}

BOOST_AUTO_TEST_CASE(spot_quota_refusal_still_terminates_on_a_spot_only_ladder) {
    // With no on-demand rung to jump to, the short-circuit must degrade to an
    // ordinary single step rather than looping or overrunning the ladder.
    const std::vector<azure_vm_option> spot_only{
        {.vm_size = "spot-a", .spot = true, .price_per_hr = 0.01},
        {.vm_size = "spot-b", .spot = true, .price_per_hr = 0.02},
    };
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    BOOST_CHECK_THROW(escalate_launch(
                          spot_only, rung, escalations,
                          [&](const azure_vm_option& option) -> std::string {
                              attempted.push_back(option.vm_size);
                              throw std::runtime_error(
                                  "VM creation failed: exceeding approved "
                                  "LowPriorityCores quota");
                          },
                          noop_escalate),
                      std::runtime_error);
    BOOST_REQUIRE_EQUAL(attempted.size(), spot_only.size());
    BOOST_TEST(escalations == 1u);
}

BOOST_AUTO_TEST_CASE(family_quota_refusal_advances_one_rung_at_a_time) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    // The control distinguishing the two quota kinds: a *family* core quota is
    // per-family, so the next spot rung is a genuinely different question and
    // must still be tried. Only LowPriorityCores short-circuits.
    auto chosen = escalate_launch(
        kLadder, rung, escalations,
        [&](const azure_vm_option& option) -> std::string {
            attempted.push_back(option.vm_size);
            if (option.vm_size == "spot-a") {
                throw std::runtime_error(
                    "VM creation failed: OperationNotAllowed: exceeding "
                    "approved standardFSv7Family Cores quota");
            }
            return option.vm_size;
        },
        noop_escalate);

    BOOST_TEST(chosen == "spot-b");
    BOOST_REQUIRE_EQUAL(attempted.size(), 2u);
    BOOST_TEST(escalations == 1u);
}

BOOST_AUTO_TEST_CASE(rethrows_immediately_on_a_non_capacity_error) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    BOOST_CHECK_THROW(escalate_launch(
                          kLadder, rung, escalations,
                          [&](const azure_vm_option& option) -> std::string {
                              attempted.push_back(option.vm_size);
                              throw std::runtime_error("VM creation failed: AuthorizationFailed");
                          },
                          noop_escalate),
                      std::runtime_error);

    // The control that makes this case meaningful: exactly one attempt, on the
    // first rung. A classifier that matched too broadly would show four.
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
    BOOST_TEST(attempted.front() == "spot-a");
    BOOST_TEST(rung == 0u);
    BOOST_TEST(escalations == 0u);
}

BOOST_AUTO_TEST_CASE(rethrows_the_last_refusal_once_the_ladder_is_exhausted) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    BOOST_CHECK_THROW(escalate_launch(
                          kLadder, rung, escalations,
                          [&](const azure_vm_option& option) -> std::string {
                              attempted.push_back(option.vm_size);
                              throw std::runtime_error(
                                  "VM creation failed: AllocationFailed: "
                                  "insufficient capacity");
                          },
                          noop_escalate),
                      std::runtime_error);

    BOOST_REQUIRE_EQUAL(attempted.size(), kLadder.size());
    BOOST_TEST(rung == kLadder.size() - 1);
    BOOST_TEST(escalations == kLadder.size() - 1);
}

BOOST_AUTO_TEST_CASE(resumes_from_the_rung_the_previous_call_settled_on) {
    std::size_t rung = 0;
    std::size_t escalations = 0;
    std::vector<std::string> attempted;

    auto attempt = [&](const azure_vm_option& option) -> std::string {
        attempted.push_back(option.vm_size);
        if (option.vm_size == "spot-a") {
            throw std::runtime_error("VM creation failed: AllocationFailed: no capacity");
        }
        return option.vm_size;
    };

    BOOST_TEST(escalate_launch(kLadder, rung, escalations, attempt, noop_escalate) == "spot-b");
    attempted.clear();

    // The second node must not retry spot-a: within one test case a rung that
    // has already been refused stays refused, which is what makes the 9-VM
    // case converge instead of paying the failure cost per node.
    BOOST_TEST(escalate_launch(kLadder, rung, escalations, attempt, noop_escalate) == "spot-b");
    BOOST_REQUIRE_EQUAL(attempted.size(), 1u);
    BOOST_TEST(attempted.front() == "spot-b");
    BOOST_TEST(escalations == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
