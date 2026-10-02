// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file oci_launch_ladder_unit_test.cpp
/// @brief The real-OCI harness's preemptible-first launch ladder, offline
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 13.12-13.15).
///
/// The ladder only matters on the day OCI is out of capacity, which no test
/// can arrange. The real suite forces one stockout per run; this pins the
/// rest of the contract deterministically, on every CI build, with a
/// scripted launcher and injected prices:
///
/// - a capacity error advances to the next rung (13.14);
/// - any other error aborts at once, so a defect is not walked past (13.14);
/// - running out of rungs reports the last error (13.14);
/// - preemptible rungs come first, cheapest first, keyed on (shape, AD), and
///   stop at the on-demand fallback's price (13.12, 13.13);
/// - the cost line names the chosen shape, AD, market and rate (13.15).

#define BOOST_TEST_MODULE oci_launch_ladder_unit_test
#include <boost/test/unit_test.hpp>

#include "oci_real_test_support.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using kythira::testing::oci_real::billed_resource_for;
using kythira::testing::oci_real::launch_option;
using kythira::testing::oci_real::preemptible_first_launch_options;
using kythira::testing::oci_real::TestCostReport;
using kythira::testing::oci_real::walk_launch_ladder;

/// Finding 14's verbatim live-OCI stockout, as `oci_http_client` wraps it.
constexpr const char* kStockout =
    "oci_http_client: POST /20160918/instances failed with HTTP "
    "500: InternalError: Out of host capacity.";

auto three_rungs() -> std::vector<launch_option> {
    return {
        {.shape = "VM.Standard.E4.Flex", .availability_domain = "AD-1", .preemptible = true},
        {.shape = "VM.Standard.E4.Flex", .availability_domain = "AD-2", .preemptible = true},
        {.shape = "VM.Standard.E2.1", .availability_domain = "AD-1", .preemptible = false},
    };
}

/// Prices by family, recording every family asked for.
struct fake_prices {
    std::map<std::string, std::optional<double>> table;
    std::vector<std::string> asked;

    auto lookup() {
        return [this](const std::string& family) -> std::optional<double> {
            asked.push_back(family);
            const auto it = table.find(family);
            return it == table.end() ? std::nullopt : it->second;
        };
    }
};

}  // namespace

BOOST_AUTO_TEST_SUITE(walk)

BOOST_AUTO_TEST_CASE(a_forced_stockout_on_the_first_rung_advances_to_the_second) {
    const auto ladder = three_rungs();
    std::vector<std::string> attempted;
    const auto outcome = walk_launch_ladder(ladder, [&](const launch_option& rung) -> std::string {
        attempted.push_back(rung.availability_domain);
        if (attempted.size() == 1) {
            throw std::runtime_error(kStockout);
        }
        return "ocid1.instance." + rung.availability_domain;
    });
    BOOST_CHECK_EQUAL(outcome.rung_index, 1U);
    BOOST_CHECK_EQUAL(outcome.launched, "ocid1.instance.AD-2");
    BOOST_CHECK_EQUAL(outcome.chosen.availability_domain, "AD-2");
    BOOST_CHECK_EQUAL(attempted.size(), 2U);
    BOOST_REQUIRE_EQUAL(outcome.stockouts.size(), 1U);
    BOOST_CHECK(outcome.stockouts.front().starts_with(ladder.front().describe()));
    BOOST_CHECK(outcome.stockouts.front().find("Out of host capacity") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(every_preemptible_rung_out_of_capacity_degrades_to_on_demand) {
    const auto outcome = walk_launch_ladder(three_rungs(), [](const launch_option& rung) -> int {
        if (rung.preemptible) {
            throw std::runtime_error(kStockout);
        }
        return 42;
    });
    BOOST_CHECK_EQUAL(outcome.rung_index, 2U);
    BOOST_CHECK(!outcome.chosen.preemptible);
    BOOST_CHECK_EQUAL(outcome.stockouts.size(), 2U);
}

BOOST_AUTO_TEST_CASE(a_non_capacity_error_aborts_without_walking_further) {
    std::size_t calls = 0;
    try {
        (void)walk_launch_ladder(three_rungs(), [&](const launch_option&) -> int {
            ++calls;
            if (calls == 1) {
                throw std::runtime_error(kStockout);
            }
            // The generic code a stockout shares, with a different message:
            // matching on the code would walk past this.
            throw std::runtime_error(
                "oci_http_client: POST /20160918/instances failed with HTTP "
                "500: InternalError: image is not compatible with shape");
        });
        BOOST_FAIL("a non-capacity error did not abort the walk");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_CHECK(what.find("non-capacity") != std::string::npos);
        BOOST_CHECK(what.find("image is not compatible") != std::string::npos);
    }
    BOOST_CHECK_EQUAL(calls, 2U);
}

BOOST_AUTO_TEST_CASE(an_exhausted_ladder_reports_the_last_error) {
    std::size_t calls = 0;
    try {
        (void)walk_launch_ladder(three_rungs(), [&](const launch_option& rung) -> int {
            ++calls;
            throw std::runtime_error(std::string(kStockout) + " " + rung.shape);
        });
        BOOST_FAIL("an exhausted ladder did not throw");
    } catch (const std::runtime_error& e) {
        const std::string what = e.what();
        BOOST_CHECK(what.find("exhausted all 3") != std::string::npos);
        BOOST_CHECK(what.find("VM.Standard.E2.1") != std::string::npos);
    }
    BOOST_CHECK_EQUAL(calls, 3U);
}

BOOST_AUTO_TEST_CASE(an_empty_ladder_is_rejected) {
    BOOST_CHECK_THROW((void)walk_launch_ladder(std::vector<launch_option>{},
                                               [](const launch_option&) { return 0; }),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(build)

BOOST_AUTO_TEST_CASE(preemptible_first_cheapest_first_keyed_on_shape_and_ad) {
    fake_prices prices{.table = {{"E4", 0.025}, {"E5", 0.03}, {"A1", 0.0}, {"E2", 0.03}}};
    const auto ladder = preemptible_first_launch_options({"AD-1", "AD-2"}, "VM.Standard.E2.1", 1.0,
                                                         6.0, {"E4", "E5", "A1"}, prices.lookup());

    // E5 costs as much as the fallback, so 13.12 drops it; A1's zero list
    // price is the free allocation and orders behind every priced family.
    const std::vector<std::pair<std::string, std::string>> expected{
        {"VM.Standard.E4.Flex", "AD-1"}, {"VM.Standard.E4.Flex", "AD-2"},
        {"VM.Standard.A1.Flex", "AD-1"}, {"VM.Standard.A1.Flex", "AD-2"},
        {"VM.Standard.E2.1", "AD-1"},    {"VM.Standard.E2.1", "AD-2"},
    };
    BOOST_REQUIRE_EQUAL(ladder.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        BOOST_CHECK_EQUAL(ladder[i].shape, expected[i].first);
        BOOST_CHECK_EQUAL(ladder[i].availability_domain, expected[i].second);
        BOOST_CHECK_EQUAL(ladder[i].preemptible, i < 4);
    }
    // The fallback is priced by its family, not its full shape name.
    BOOST_CHECK_EQUAL(prices.asked.front(), "E2");
    BOOST_REQUIRE(ladder.back().ocpu_hourly_rate.has_value());
    BOOST_CHECK_CLOSE(*ladder.back().ocpu_hourly_rate, 0.03, 1e-9);
}

BOOST_AUTO_TEST_CASE(unknown_prices_keep_every_family_in_listed_order) {
    fake_prices prices;  // a price-list outage: nothing is known
    const auto ladder = preemptible_first_launch_options({"AD-1"}, "VM.Standard.E2.1", 1.0, 6.0,
                                                         {"E4", "E5"}, prices.lookup());
    BOOST_REQUIRE_EQUAL(ladder.size(), 3U);
    BOOST_CHECK_EQUAL(ladder[0].shape, "VM.Standard.E4.Flex");
    BOOST_CHECK_EQUAL(ladder[1].shape, "VM.Standard.E5.Flex");
    BOOST_CHECK_EQUAL(ladder[2].shape, "VM.Standard.E2.1");
    BOOST_CHECK(!ladder[2].preemptible);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(cost_report)

BOOST_AUTO_TEST_CASE(the_cost_line_names_shape_ad_market_and_rate) {
    const launch_option chosen{.shape = "VM.Standard.E4.Flex",
                               .availability_domain = "WrmK:PHX-AD-2",
                               .ocpus = 2.0,
                               .preemptible = true,
                               .ocpu_hourly_rate = 0.025};
    auto billed = billed_resource_for(chosen);
    BOOST_CHECK(billed.rate_known);
    BOOST_CHECK_CLOSE(billed.ocpus, 2.0, 1e-9);
    billed.finalize();
    const auto text = TestCostReport{.test_name = "t", .resources = {billed}}.format();
    for (const char* needle :
         {"VM.Standard.E4.Flex", "WrmK:PHX-AD-2", "preemptible", "$0.0250/OCPU-h"}) {
        BOOST_CHECK_MESSAGE(text.find(needle) != std::string::npos,
                            "cost report lacks '" << needle << "':" << text);
    }
}

BOOST_AUTO_TEST_CASE(an_unpriced_on_demand_rung_reads_as_unknown_not_free) {
    const launch_option chosen{.shape = "VM.Standard.E2.1", .availability_domain = "AD-1"};
    auto billed = billed_resource_for(chosen);
    BOOST_CHECK(!billed.rate_known);
    billed.finalize();
    const auto text = TestCostReport{.test_name = "t", .resources = {billed}}.format();
    BOOST_CHECK(text.find("on-demand") != std::string::npos);
    BOOST_CHECK(text.find("(rate unknown)") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
