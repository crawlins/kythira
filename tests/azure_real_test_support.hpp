#pragma once

/// @file azure_real_test_support.hpp
/// @brief Shared real-Azure integration test infrastructure: cost
/// estimation/reporting and signal-driven cleanup. Ported from
/// `aws_real_ec2_test_support.hpp` (aws-quorum-manager spec Requirements
/// 20/21) — the cost-accumulator and signal-handling shapes are
/// provider-agnostic, only the pricing table and fixture names differ.
///
/// Header-only, included directly by each real-Azure test .cpp. `g_cost_accumulator`
/// and `g_active_azure_fixture` are declared `inline` so each including
/// *binary* gets exactly one definition, even though more than one real-Azure
/// test binary includes this same header.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <utility>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#ifdef KYTHIRA_HAS_AZURE_SDK
#include <azure/core/context.hpp>
#include <azure/core/http/http.hpp>
#include <azure/core/http/policies/policy.hpp>
#include <azure/core/internal/client_options.hpp>
#include <azure/core/internal/http/pipeline.hpp>
#include <azure/core/url.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>
#endif

namespace kythira::testing::azure_real {

// ── Cost estimation (design.md's "CI wiring" section) ──────────────────────
//
// Published on-demand eastus Linux prices ($/hr, approximate).
// Source: https://azure.microsoft.com/en-us/pricing/details/virtual-machines/linux/
// (as of this writing — reverify before relying on this for real budgeting).
inline auto azure_vm_hourly_rate(const std::string& size) -> double {
    static const std::map<std::string, double> kRates{
        {"Standard_B1s", 0.0104},   {"Standard_B2s", 0.0416},     {"Standard_D2s_v5", 0.096},
        {"Standard_D4s_v5", 0.192}, {"Standard_D2als_v6", 0.086}, {"Standard_F2s_v2", 0.085},
    };
    auto it = kRates.find(size);
    return (it != kRates.end()) ? it->second : 0.096;
}

struct BilledResource {
    std::string label;
    double hourly_rate{0.0};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::optional<std::chrono::steady_clock::time_point> stop;

    void finalize() {
        if (!stop) {
            stop = std::chrono::steady_clock::now();
        }
    }

    [[nodiscard]] auto hours() const -> double {
        auto e = stop.value_or(std::chrono::steady_clock::now());
        return std::chrono::duration<double>(e - start).count() / 3600.0;
    }
    [[nodiscard]] auto minutes() const -> double { return hours() * 60.0; }
    [[nodiscard]] auto cost_usd() const -> double { return hours() * hourly_rate; }
};

struct TestCostReport {
    std::string test_name;
    std::vector<BilledResource> resources;

    [[nodiscard]] auto total_usd() const -> double {
        double t = 0.0;
        for (const auto& r : resources) {
            t += r.cost_usd();
        }
        return t;
    }

    [[nodiscard]] auto format() const -> std::string {
        std::ostringstream oss;
        oss << std::fixed;
        oss << "\n[azure-cost] " << test_name << "\n";
        for (const auto& r : resources) {
            oss << "[azure-cost]   " << std::left << std::setw(38) << r.label << std::right
                << std::setw(7) << std::setprecision(1) << r.minutes() << " min"
                << "   $" << std::setprecision(6) << r.cost_usd() << "\n";
        }
        oss << "[azure-cost]   " << std::left << std::setw(38) << "TOTAL" << std::right
            << std::setw(11) << " "
            << "$" << std::setprecision(6) << total_usd() << "\n";
        return oss.str();
    }
};

struct CostAccumulator {
    std::mutex mtx;
    std::vector<TestCostReport> reports;

    void add(TestCostReport r) {
        std::lock_guard<std::mutex> lk{mtx};
        reports.push_back(std::move(r));
    }
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline CostAccumulator g_cost_accumulator;

struct CostSummaryFixture {
    ~CostSummaryFixture() {
        std::lock_guard<std::mutex> lk{g_cost_accumulator.mtx};
        const auto& reps = g_cost_accumulator.reports;
        // Loud, not silent — see the same branch in gcp_real_gce_test_support.hpp.
        // A bare `return` makes "no case registered a cost" indistinguishable
        // from "this fixture never ran", and in the GCP suite the first of
        // those was silently true for every run ever made.
        if (reps.empty()) {
            std::cerr << "\n[azure-cost] WARNING: no cost reports were registered. Either no case "
                         "provisioned a billable resource, or a case failed to file its "
                         "TestCostReport. Real-cloud spend for this run is UNREPORTED.\n";
            return;
        }

        double grand = 0.0;
        for (const auto& r : reps) {
            grand += r.total_usd();
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "\n================================================================\n";
        oss << " Azure Real-Cloud Test Cost Estimate Summary\n";
        oss << "================================================================\n";
        for (const auto& r : reps) {
            oss << "  " << std::left << std::setw(52) << r.test_name << "  $" << r.total_usd()
                << "\n";
        }
        oss << "----------------------------------------------------------------\n";
        oss << "  " << std::left << std::setw(52) << "GRAND TOTAL"
            << "  $" << grand << "\n";
        oss << "================================================================\n";
        oss << " Pricing: on-demand Linux rates (approximate). Actual costs vary\n";
        oss << " by region and time. Use Azure Cost Management for authoritative\n";
        oss << " billing data.\n";
        oss << "================================================================\n";
        // std::cerr, not BOOST_TEST_MESSAGE: Boost's default log level drops
        // `message`-level records, which is why no real-cloud suite's cost
        // summary has ever appeared in a CI log.
        std::cerr << oss.str() << std::flush;
    }
};

// ── Spot-first SKU escalation ──────────────────────────────────────────────
//
// Mirrors `spot_first_launch_options()`/`is_insufficient_capacity()` in
// `tests/aws_quorum_manager_real_ec2_test.cpp`. As on the AWS side, this lives
// in the *test* support layer only and deliberately never leaks into
// `azure_vm_quorum_manager`: choosing the cheapest currently-launchable SKU is
// a property of how we want to pay for CI, not of the quorum manager's
// contract.
//
// Two live data sources have to be joined, and joining them is not optional:
//
//   1. The Azure Retail Prices API (public, unauthenticated) gives per-SKU
//      spot and on-demand $/hr but says nothing about whether *this*
//      subscription may actually launch a SKU.
//   2. ARM's `Microsoft.Compute/skus` gives restrictions, zones, and shape
//      capabilities but no prices.
//
// Ranking on prices alone puts `Standard_M*_v4` at the top of the ladder at
// ~$0.002/hr — SKUs this subscription cannot purchase at all and whose vCPU
// count would blow the regional quota even if it could.

/// One rung of the spot-first launch ladder.
struct azure_vm_option {
    std::string vm_size;
    bool spot{true};
    double price_per_hr{0.0};

    [[nodiscard]] auto label() const -> std::string {
        return vm_size + (spot ? " (spot)" : " (on-demand)");
    }
};

/// Availability and shape facts for one VM SKU in one region, as reported by
/// `Microsoft.Compute/skus`.
struct azure_sku_facts {
    unsigned vcpus{0};
    /// `CpuArchitectureType` capability — "x64" or "Arm64".
    std::string architecture{"x64"};
    /// `HyperVGenerations` capability — "V1", "V2", or "V1,V2".
    std::string hyperv_generations{"V1,V2"};
    /// True when the SKU is offered in at least one of availability zones 1-3.
    bool zonal{false};
    /// True when the SKU advertises a `ConfidentialComputingType` capability
    /// (the DC-series families). Such SKUs reject any VM create that does not
    /// also carry a `securityProfile.securityType`, which
    /// `azure_vm_quorum_manager` does not set — and they reject it as a plain
    /// `BadRequest`, which escalation treats as fatal, so a single one of these
    /// reachable on the ladder aborts the whole walk.
    bool confidential{false};
    /// True when any `restrictions` entry applies (e.g.
    /// `NotAvailableForSubscription`), which makes the SKU unlaunchable here.
    bool restricted{false};
};

/// What a candidate SKU must satisfy to be a launchable rung of the ladder.
///
/// The defaults match what `AzureIntegrationFixture` actually launches, and
/// every one of them is load-bearing:
///
/// * `architecture` — the fixture's marketplace image is x86-64 Ubuntu. Left
///   unfiltered, the cheapest SKUs in eastus are Ampere (`Standard_B2pts_v2`
///   at $0.0084/hr on-demand, cheaper than *any* x64 spot price) and would
///   head the ladder with a SKU the image cannot boot.
/// * `hyperv_generation` — every cheap modern SKU (the v6/v7 families that
///   dominate the low end) is Gen2-only, so the image must be the `-gen2`
///   variant and candidates must advertise V2. Pairing a Gen2-only size with
///   a Gen1 image is a hard `BadRequest`, not a capacity error, so escalation
///   would abort rather than advance.
/// * `max_vcpus` — this subscription's regional quota is 10 regular vCPUs / 3
///   spot vCPUs, and the largest case holds 9 VMs at once. Anything above 2
///   vCPUs cannot fit that case whatever the price.
struct azure_sku_requirements {
    unsigned max_vcpus{2};
    std::string architecture{"x64"};
    std::string hyperv_generation{"V2"};
};

inline auto to_lower_copy(std::string s) -> std::string {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// True when an ARM error means "this SKU, right now, in this
/// subscription/region" rather than "this request is wrong".
///
/// Escalation advances the ladder on these and aborts on anything else — a
/// misconfigured image or subnet must fail the test loudly, not silently burn
/// through 40 SKUs first.
///
/// Both capacity *and* quota rejections advance. That is deliberate rather
/// than sloppy: with a 3-vCPU spot quota, the 4th concurrent spot VM in a case
/// is rejected for quota, and the correct response is exactly the same as for
/// a capacity shortage — try the next rung, ending at on-demand. Family quotas
/// are per-family, so advancing across families genuinely helps.
///
/// `azure_vm_quorum_manager` surfaces these as
/// `std::runtime_error("VM creation failed: …")` — a flattened string with no
/// structured error code — so, as on the AWS side, the classifier is a
/// case-insensitive substring match.
inline auto is_capacity_or_quota_error(std::string_view message) -> bool {
    const std::string haystack = to_lower_copy(std::string(message));
    // Covers, in order: AllocationFailed/ZonalAllocationFailed ("Allocation
    // failed. We do not have sufficient capacity…"), SkuNotAvailable ("The
    // requested size … is currently not available in location…"),
    // OperationNotAllowed/QuotaExceeded ("…exceeding approved
    // standardFSv7Family Cores quota"), OverconstrainedAllocationRequest, and
    // subscription-level SKU restrictions.
    static const std::vector<std::string> kNeedles{
        "allocation failed",
        "allocationfailed",
        "capacity",
        "quota",
        "skunotavailable",
        "not available in location",
        "not available for subscription",
        "overconstrained",
    };
    return std::any_of(kNeedles.begin(), kNeedles.end(), [&](const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    });
}

/// Folds one Retail Prices API page into `spot`/`on_demand` (`armSkuName` →
/// cheapest observed $/hr) and returns the page's `NextPageLink`, or an empty
/// string when this was the last page.
///
/// The feed has four traps this filters, all confirmed against live eastus
/// data:
///  * it is paginated (~10 pages for one region's Virtual Machines);
///  * Windows SKUs share `armSkuName` with their Linux counterparts at a much
///    higher price, so they are dropped by `productName`;
///  * the retired "Low Priority" tier looks like spot but is not purchasable;
///  * the same `armSkuName` appears on several rows (per-meter/per-tier
///    variants), so the minimum is taken rather than the first seen.
///
/// Spot rows are distinguished by "Spot" appearing in `skuName`.
inline auto absorb_retail_price_page(const boost::json::value& page,
                                     std::map<std::string, double>& spot,
                                     std::map<std::string, double>& on_demand) -> std::string {
    if (!page.is_object()) {
        return {};
    }
    const auto& obj = page.as_object();
    if (const auto* items = obj.if_contains("Items"); items != nullptr && items->is_array()) {
        for (const auto& row : items->as_array()) {
            if (!row.is_object()) {
                continue;
            }
            const auto& r = row.as_object();
            const auto str = [&](const char* key) -> std::string {
                const auto* v = r.if_contains(key);
                return (v != nullptr && v->is_string()) ? std::string(v->as_string())
                                                        : std::string{};
            };
            const std::string arm_sku = str("armSkuName");
            const std::string sku_name = str("skuName");
            const std::string product = str("productName");
            const auto* price_v = r.if_contains("retailPrice");
            const double price = (price_v != nullptr && price_v->is_double())
                                     ? price_v->as_double()
                                     : (price_v != nullptr && price_v->is_int64()
                                            ? static_cast<double>(price_v->as_int64())
                                            : 0.0);
            if (arm_sku.empty() || price <= 0.0) {
                continue;
            }
            if (product.find("Windows") != std::string::npos) {
                continue;
            }
            if (sku_name.find("Low Priority") != std::string::npos) {
                continue;
            }
            auto& target = (sku_name.find("Spot") != std::string::npos) ? spot : on_demand;
            auto it = target.find(arm_sku);
            if (it == target.end() || price < it->second) {
                target[arm_sku] = price;
            }
        }
    }
    if (const auto* next = obj.if_contains("NextPageLink"); next != nullptr && next->is_string()) {
        return std::string(next->as_string());
    }
    return {};
}

/// Parses a `Microsoft.Compute/skus` response into per-SKU availability facts,
/// keeping only `resourceType == "virtualMachines"` entries.
inline auto parse_compute_skus(const boost::json::value& body)
    -> std::map<std::string, azure_sku_facts> {
    std::map<std::string, azure_sku_facts> out;
    if (!body.is_object()) {
        return out;
    }
    const auto* value = body.as_object().if_contains("value");
    if (value == nullptr || !value->is_array()) {
        return out;
    }
    for (const auto& entry : value->as_array()) {
        if (!entry.is_object()) {
            continue;
        }
        const auto& e = entry.as_object();
        const auto* rt = e.if_contains("resourceType");
        if (rt == nullptr || !rt->is_string() || rt->as_string() != "virtualMachines") {
            continue;
        }
        const auto* name = e.if_contains("name");
        if (name == nullptr || !name->is_string()) {
            continue;
        }

        azure_sku_facts facts;
        // Any restriction at all disqualifies the SKU. The common one is
        // NotAvailableForSubscription, but the shape is the same for every
        // reason code and none of them leave the SKU launchable.
        if (const auto* r = e.if_contains("restrictions");
            r != nullptr && r->is_array() && !r->as_array().empty()) {
            facts.restricted = true;
        }
        if (const auto* li = e.if_contains("locationInfo"); li != nullptr && li->is_array()) {
            for (const auto& loc : li->as_array()) {
                if (!loc.is_object()) {
                    continue;
                }
                const auto* zones = loc.as_object().if_contains("zones");
                if (zones == nullptr || !zones->is_array()) {
                    continue;
                }
                for (const auto& z : zones->as_array()) {
                    if (z.is_string()) {
                        const auto zs = std::string(z.as_string());
                        if (zs == "1" || zs == "2" || zs == "3") {
                            facts.zonal = true;
                        }
                    }
                }
            }
        }
        if (const auto* caps = e.if_contains("capabilities"); caps != nullptr && caps->is_array()) {
            for (const auto& cap : caps->as_array()) {
                if (!cap.is_object()) {
                    continue;
                }
                const auto* cn = cap.as_object().if_contains("name");
                const auto* cv = cap.as_object().if_contains("value");
                if (cn == nullptr || cv == nullptr || !cn->is_string() || !cv->is_string()) {
                    continue;
                }
                const auto key = std::string(cn->as_string());
                const auto val = std::string(cv->as_string());
                if (key == "vCPUs") {
                    facts.vcpus = static_cast<unsigned>(std::strtoul(val.c_str(), nullptr, 10));
                } else if (key == "CpuArchitectureType") {
                    facts.architecture = val;
                } else if (key == "HyperVGenerations") {
                    facts.hyperv_generations = val;
                } else if (key == "ConfidentialComputingType") {
                    facts.confidential = true;
                }
            }
        }
        out[std::string(name->as_string())] = std::move(facts);
    }
    return out;
}

/// True when `facts` describes a SKU this suite could actually launch.
inline auto sku_is_launchable(const azure_sku_facts& facts, const azure_sku_requirements& req)
    -> bool {
    return !facts.restricted && !facts.confidential && facts.zonal && facts.vcpus > 0 &&
           facts.vcpus <= req.max_vcpus && facts.architecture == req.architecture &&
           facts.hyperv_generations.find(req.hyperv_generation) != std::string::npos;
}

/// Builds the ordered launch ladder: every launchable spot SKU in ascending
/// price order that still undercuts simply paying on-demand for the cheapest
/// launchable on-demand SKU, followed by that on-demand SKU as the terminal
/// fallback.
///
/// Truncating at the cheapest available on-demand price is the same rule the
/// AWS side uses: once the next spot rung costs at least as much as just
/// buying on-demand, there is no reason left to keep trying spot.
///
/// Returns empty when no launchable on-demand SKU exists at all, which the
/// caller must treat as "fall back to the configured default" rather than as
/// "launch nothing".
inline auto rank_spot_first(const std::map<std::string, double>& spot,
                            const std::map<std::string, double>& on_demand,
                            const std::map<std::string, azure_sku_facts>& facts,
                            const azure_sku_requirements& req) -> std::vector<azure_vm_option> {
    const auto launchable = [&](const std::string& sku) {
        auto it = facts.find(sku);
        return it != facts.end() && sku_is_launchable(it->second, req);
    };

    std::optional<azure_vm_option> cheapest_on_demand;
    for (const auto& [sku, price] : on_demand) {
        if (!launchable(sku)) {
            continue;
        }
        if (!cheapest_on_demand || price < cheapest_on_demand->price_per_hr) {
            cheapest_on_demand =
                azure_vm_option{.vm_size = sku, .spot = false, .price_per_hr = price};
        }
    }
    if (!cheapest_on_demand) {
        return {};
    }

    std::vector<azure_vm_option> ladder;
    for (const auto& [sku, price] : spot) {
        if (price < cheapest_on_demand->price_per_hr && launchable(sku)) {
            ladder.push_back({.vm_size = sku, .spot = true, .price_per_hr = price});
        }
    }
    std::sort(ladder.begin(), ladder.end(), [](const auto& a, const auto& b) {
        // Tie-break by name so the ladder is deterministic across runs, which
        // matters for reproducing a failure from a CI log.
        return (a.price_per_hr != b.price_per_hr) ? a.price_per_hr < b.price_per_hr
                                                  : a.vm_size < b.vm_size;
    });
    ladder.push_back(*cheapest_on_demand);
    return ladder;
}

/// True when a refusal is specifically the *subscription-wide spot* quota
/// (`LowPriorityCores`) rather than a per-SKU or per-family limit.
///
/// This distinction is what stops the walk being quadratic in practice. A
/// capacity shortage or a per-family core quota is specific to the SKU being
/// asked for, so the next rung is a genuinely different question. The spot
/// quota is not: it is one region-wide counter shared by every spot SKU, so
/// once it is exhausted *every* remaining spot rung is guaranteed to fail for
/// the identical reason. Observed directly — a nine-VM case burned 40
/// consecutive spot rungs, each rejected with "exceeding approved
/// LowPriorityCores quota ... Current Limit: 3, Current Usage: 3", before it
/// could reach the on-demand fallback that was always going to be the answer.
inline auto is_spot_quota_error(std::string_view message) -> bool {
    const std::string haystack = to_lower_copy(std::string(message));
    return haystack.find("lowprioritycores") != std::string::npos;
}

/// Walks `ladder` forward from `rung`, invoking `attempt(ladder[rung])` until
/// one rung launches.
///
/// Advances (incrementing `escalations`) when a rung is rejected for capacity
/// or quota; rethrows immediately on any other error, and rethrows the last
/// rejection once the ladder is exhausted. `rung` is carried in and out by
/// reference so a caller provisioning several nodes resumes where the previous
/// one settled rather than re-failing on rungs already known bad.
///
/// `on_escalate(from, to, message)` is invoked once per advance, for logging.
///
/// This is separated from the manager it drives purely so the walk can be
/// exercised offline against a stub `attempt` — a real-Azure-only escalation
/// path is otherwise only ever observed on runs that happen to hit a capacity
/// shortage, which is exactly the run you cannot schedule.
template<typename Attempt, typename OnEscalate>
auto escalate_launch(const std::vector<azure_vm_option>& ladder, std::size_t& rung,
                     std::size_t& escalations, Attempt&& attempt, OnEscalate&& on_escalate)
    -> decltype(attempt(ladder.front())) {
    for (;;) {
        try {
            return attempt(ladder.at(rung));
        } catch (const std::exception& ex) {
            if (!is_capacity_or_quota_error(ex.what()) || rung + 1 >= ladder.size()) {
                throw;
            }
            // The region-wide spot quota dooms every remaining spot rung
            // identically, so jump straight to the first non-spot rung rather
            // than proving that once per rung. If there is no such rung (a
            // spot-only ladder), fall through to the ordinary single step so
            // the walk still terminates by exhausting the ladder.
            std::size_t next = rung + 1;
            if (is_spot_quota_error(ex.what())) {
                for (std::size_t i = next; i < ladder.size(); ++i) {
                    if (!ladder[i].spot) {
                        next = i;
                        break;
                    }
                }
            }
            on_escalate(ladder[rung], ladder[next], ex.what());
            rung = next;
            ++escalations;
        }
    }
}

#ifdef KYTHIRA_HAS_AZURE_SDK

/// Unauthenticated pipeline for the public Retail Prices API.
inline auto make_public_pipeline() -> Azure::Core::Http::_internal::HttpPipeline {
    Azure::Core::_internal::ClientOptions client_options;
    return Azure::Core::Http::_internal::HttpPipeline(
        client_options, "kythira-azure-real-test", "1.0.0",
        std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{},
        std::vector<std::unique_ptr<Azure::Core::Http::Policies::HttpPolicy>>{});
}

inline auto http_get_body(Azure::Core::Http::_internal::HttpPipeline& pipeline,
                          const std::string& url) -> boost::json::value {
    Azure::Core::Url parsed(url);
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, parsed);
    Azure::Core::Context context;
    auto response = pipeline.Send(request, context);
    const auto code = static_cast<int>(response->GetStatusCode());
    if (code < 200 || code >= 300) {
        throw std::runtime_error("GET " + url + " returned HTTP " + std::to_string(code));
    }
    const auto& body = response->GetBody();
    return boost::json::parse(std::string(body.begin(), body.end()));
}

/// Walks every page of the Retail Prices API for `location`'s Virtual
/// Machines, returning (spot, on-demand) `armSkuName` → cheapest $/hr maps.
inline auto fetch_retail_prices(const std::string& location)
    -> std::pair<std::map<std::string, double>, std::map<std::string, double>> {
    auto pipeline = make_public_pipeline();
    std::map<std::string, double> spot;
    std::map<std::string, double> on_demand;
    // The $filter value contains spaces and quotes, so it must arrive
    // percent-encoded; Azure::Core::Url::AppendQueryParameter does that for us
    // rather than us hand-rolling it (the same bug 870cfc0 fixed in
    // next_node_id()'s ARM $filter).
    Azure::Core::Url url("https://prices.azure.com/api/retail/prices");
    url.AppendQueryParameter("$filter",
                             Azure::Core::Url::Encode("armRegionName eq '" + location +
                                                      "' and serviceName eq 'Virtual Machines' "
                                                      "and priceType eq 'Consumption'"));
    std::string next = url.GetAbsoluteUrl();
    // NextPageLink is server-supplied and finite (~10 pages/region), but a
    // bounded loop keeps a malformed feed from hanging a CI job forever.
    for (int page = 0; page < 64 && !next.empty(); ++page) {
        next = absorb_retail_price_page(http_get_body(pipeline, next), spot, on_demand);
    }
    return {std::move(spot), std::move(on_demand)};
}

/// Fetches per-SKU availability facts for `location` from ARM.  `arm_pipeline`
/// must already carry a bearer-token policy for management.azure.com.
inline auto fetch_compute_skus(Azure::Core::Http::_internal::HttpPipeline& arm_pipeline,
                               const std::string& arm_endpoint, const std::string& subscription_id,
                               const std::string& location)
    -> std::map<std::string, azure_sku_facts> {
    Azure::Core::Url url(arm_endpoint + "/subscriptions/" + subscription_id +
                         "/providers/Microsoft.Compute/skus");
    url.AppendQueryParameter("api-version", "2021-07-01");
    url.AppendQueryParameter("$filter", Azure::Core::Url::Encode("location eq '" + location + "'"));
    return parse_compute_skus(http_get_body(arm_pipeline, url.GetAbsoluteUrl()));
}

/// Discovers the full spot-first ladder for `location`, joining live retail
/// prices against live SKU availability. Returns empty (and explains itself on
/// stderr) if either call fails, leaving the caller to fall back to a
/// statically-configured size.
inline auto discover_spot_first_options(Azure::Core::Http::_internal::HttpPipeline& arm_pipeline,
                                        const std::string& arm_endpoint,
                                        const std::string& subscription_id,
                                        const std::string& location,
                                        const azure_sku_requirements& req)
    -> std::vector<azure_vm_option> {
    try {
        auto [spot, on_demand] = fetch_retail_prices(location);
        auto facts = fetch_compute_skus(arm_pipeline, arm_endpoint, subscription_id, location);
        auto ladder = rank_spot_first(spot, on_demand, facts, req);
        std::cerr << "[azure-spot] " << location << ": " << spot.size() << " spot / "
                  << on_demand.size() << " on-demand priced SKUs, " << facts.size()
                  << " SKUs in region -> ladder of " << ladder.size() << "\n";
        return ladder;
    } catch (const std::exception& ex) {
        std::cerr << "[azure-spot] SKU/price discovery failed (" << ex.what()
                  << "); falling back to the configured VM size\n";
        return {};
    }
}

#endif  // KYTHIRA_HAS_AZURE_SDK

// ── Signal-driven cleanup (mirrors aws_real_ec2_test_support.hpp exactly) ──

struct signal_cleanup_target {
    virtual void teardown() noexcept = 0;

protected:
    ~signal_cleanup_target() = default;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::atomic<signal_cleanup_target*> g_active_azure_fixture{nullptr};

inline void azure_signal_cleanup_handler(int sig) {
    signal_cleanup_target* f = g_active_azure_fixture.exchange(nullptr, std::memory_order_acq_rel);
    if (f != nullptr) {
        f->teardown();
    }

    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

inline void install_azure_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = azure_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGABRT, SIGSEGV, SIGBUS}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct AzureSignalHandlerFixture {
    AzureSignalHandlerFixture() { install_azure_signal_handlers(); }
};

}  // namespace kythira::testing::azure_real
