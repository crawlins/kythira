#pragma once

/// @file oci_real_test_support.hpp
/// @brief Shared real-OCI integration test infrastructure: cost estimation and
///        reporting, signal-driven cleanup, the out-of-host-capacity
///        classifier, and the preemptible-first launch ladder
///        (`.kiro/specs/oci-cloud-provider/`, Requirements 13.12-13.15,
///        14.4-14.5).
///
/// The OCI counterpart of `aws_real_ec2_test_support.hpp`, and a deliberate
/// port rather than a rewrite: the cost/reporting and signal machinery are the
/// same problem in both clouds, and two divergent implementations of "did this
/// run leak anything" is how one of them rots.
///
/// Header-only, included directly by each real-OCI test `.cpp`.
/// `g_cost_accumulator` and `g_active_oci_fixture` are `inline` (not `static`)
/// so each including *binary* gets exactly one definition — this is a
/// header-only multi-binary library, not one shared translation unit.
///
/// Three things here are OCI-specific and were settled by probing a live
/// tenancy rather than by reading documentation (`spike-notes.md` Findings 14
/// and 15). Each is marked where it appears.

#include <httplib.h>

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace kythira::testing::oci_real {

// ── Out-of-host-capacity classification (Requirement 13.14) ────────────────

/// @brief Whether an error is OCI refusing for want of capacity.
///
/// **Matched on the message, deliberately, and the code is ignored.**
/// Requirement 13.14 anticipated a `code` of `OutOfHostCapacity`. Provoked
/// against a live tenancy (`spike-notes.md` Finding 14 — `VM.Standard.A1.Flex`
/// in `WrmK:PHX-AD-2`, while the same request succeeded in AD-1 and AD-3), what
/// OCI actually returns is:
///
/// ```
/// status:  500
/// code:    InternalError
/// message: Out of host capacity.
/// ```
///
/// The code is the **generic** `InternalError`, shared with ordinary transient
/// failures. Classifying on it would treat every 500 — a genuine service blip,
/// a malformed request OCI mishandled — as a stockout, and the escalation
/// ladder would walk the whole list swallowing real defects. That is exactly
/// the outcome Requirement 13.14 forbids ("a **non**-capacity error SHALL abort
/// immediately ... so a real defect is not masked as a stockout").
///
/// So this matches the message substring, which is fragile in the way a string
/// match always is — and is why the requirement said this "cannot be written
/// correctly from inspection alone". It could not have been.
[[nodiscard]] inline auto is_out_of_host_capacity(const std::string& what) -> bool {
    // Substring, not equality: `oci_http_client` wraps OCI's message inside its
    // own "METHOD path failed with HTTP 500: code: message" envelope.
    return what.find("Out of host capacity") != std::string::npos;
}

[[nodiscard]] inline auto is_out_of_host_capacity(const std::exception& e) -> bool {
    return is_out_of_host_capacity(std::string(e.what()));
}

// ── Shape pricing (Requirement 13.12) ──────────────────────────────────────

/// @brief Published on-demand price per OCPU-hour, in USD.
///
/// **Queried at runtime**, which `spike-notes.md` Finding 15 established is
/// possible: `apexapps.oracle.com/pls/apex/cetools/api/v1/products/` is public
/// and unauthenticated, and returns per-currency `PAY_AS_YOU_GO` values. That
/// settles Requirement 13.12's open question in favour of computing the
/// cheapest-first ordering rather than maintaining a table by hand, as the
/// Azure harness has to.
///
/// Two caveats the same finding recorded, both of which bite here:
///
/// - **There is no preemptible SKU.** Zero of 648 catalog entries mention one;
///   OCI prices preemptible as a discount off on-demand. This function returns
///   the *on-demand* rate only. See @ref preemptible_first_launch_options for
///   what that means for ordering.
/// - **`A1` lists at USD 0**, presumably the Always Free allocation rather than
///   a true zero marginal rate. A naive cheapest-first sort therefore ranks
///   Ampere first — and Finding 14 found Ampere is also the shape most likely
///   to be out of capacity. Handled explicitly in the ladder below rather than
///   left to sort itself out.
///
/// Network failure returns `std::nullopt` rather than a guess: a cost report
/// that silently substituted a made-up rate would be worse than one that says
/// it does not know.
[[nodiscard]] inline auto ocpu_hourly_rate_usd(const std::string& shape_family)
    -> std::optional<double> {
    static std::mutex mtx;
    static std::map<std::string, std::optional<double>> cache;
    const std::lock_guard<std::mutex> lock(mtx);
    if (const auto it = cache.find(shape_family); it != cache.end()) {
        return it->second;
    }

    std::optional<double> rate;
    try {
        httplib::Client client("https://apexapps.oracle.com");
        client.set_connection_timeout(10, 0);
        client.set_read_timeout(20, 0);
        auto res = client.Get(
            "/pls/apex/cetools/api/v1/products/"
            "?serviceCategory=Compute%20-%20Virtual%20Machine");
        if (res && res->status == 200) {
            const auto parsed = boost::json::parse(res->body);
            if (const auto* obj = parsed.if_object(); obj != nullptr) {
                if (const auto* items = obj->if_contains("items");
                    items != nullptr && items->is_array()) {
                    for (const auto& item : items->get_array()) {
                        const auto* io = item.if_object();
                        if (io == nullptr) {
                            continue;
                        }
                        const auto* name = io->if_contains("displayName");
                        const auto* metric = io->if_contains("metricName");
                        if (name == nullptr || !name->is_string() || metric == nullptr ||
                            !metric->is_string()) {
                            continue;
                        }
                        const std::string display(name->get_string());
                        // `OCPU` appears in metricName, not displayName —
                        // requiring it in the latter matched nothing at all,
                        // which is why the first version of this silently
                        // priced everything at zero.
                        if (display.find(shape_family) == std::string::npos ||
                            std::string(metric->get_string()) != "OCPU Per Hour") {
                            continue;
                        }
                        // Skip the Always Free allocations. `E2` matches both
                        // `Compute - Standard - E2` (USD 0.03) and
                        // `... - E2 Micro - Free` (USD 0); taking whichever
                        // came first would price a billable instance at zero
                        // half the time. This is also why `A1` reads as 0 —
                        // see the note above.
                        if (display.find("Free") != std::string::npos) {
                            continue;
                        }
                        const auto* locs = io->if_contains("currencyCodeLocalizations");
                        if (locs == nullptr || !locs->is_array()) {
                            continue;
                        }
                        for (const auto& loc : locs->get_array()) {
                            const auto* lo = loc.if_object();
                            if (lo == nullptr) {
                                continue;
                            }
                            const auto* code = lo->if_contains("currencyCode");
                            if (code == nullptr || !code->is_string() ||
                                std::string(code->get_string()) != "USD") {
                                continue;
                            }
                            const auto* prices = lo->if_contains("prices");
                            if (prices != nullptr && prices->is_array() &&
                                !prices->get_array().empty()) {
                                const auto* p0 = prices->get_array()[0].if_object();
                                if (p0 != nullptr) {
                                    if (const auto* v = p0->if_contains("value");
                                        v != nullptr && (v->is_double() || v->is_int64())) {
                                        rate = v->is_double() ? v->get_double()
                                                              : static_cast<double>(v->get_int64());
                                    }
                                }
                            }
                        }
                        if (rate.has_value()) {
                            break;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[oci-cost] could not query the OCI price list for " << shape_family << ": "
                  << e.what() << " — this shape's cost will be reported as unknown\n";
    }
    cache.emplace(shape_family, rate);
    return rate;
}

/// @brief The price-list family token for a concrete shape name.
///
/// `VM.Standard.E2.1` -> `E2`, `VM.Standard.A1.Flex` -> `A1`. The price list
/// keys on the family, not the full shape name, so a cost report has to map
/// one to the other or it prices nothing.
[[nodiscard]] inline auto shape_price_family(const std::string& shape) -> std::string {
    // Third dot-separated token: VM . Standard . <family> . <size>
    std::vector<std::string> parts;
    std::string current;
    for (const char c : shape) {
        if (c == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    parts.push_back(current);
    return parts.size() >= 3 ? parts[2] : shape;
}

// ── Cost estimation and reporting (Requirements 14.4-14.5) ─────────────────

/// One billable thing, timed from construction.
///
/// Unlike the AWS equivalent, the rate is per **OCPU**-hour and the resource
/// carries its OCPU count: OCI's flexible shapes price by core, so a 4-OCPU
/// A1.Flex and a 1-OCPU A1.Flex are the same SKU at different quantities.
struct BilledResource {
    std::string label;
    double ocpu_hourly_rate{0.0};
    double ocpus{1.0};
    /// **Defaults to false, deliberately.** A resource nobody priced must not
    /// read as "known to cost nothing" — that is how this suite's first live
    /// run reported $0.000000 for a real instance, twice: once because no
    /// lookup happened, and once because this flag defaulted to true so the
    /// missing lookup was invisible.
    bool rate_known{false};
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    std::optional<std::chrono::steady_clock::time_point> stop;

    void finalize() {
        if (!stop) {
            stop = std::chrono::steady_clock::now();
        }
    }

    [[nodiscard]] auto hours() const -> double {
        const auto end = stop.value_or(std::chrono::steady_clock::now());
        return std::chrono::duration<double>(end - start).count() / 3600.0;
    }
    [[nodiscard]] auto minutes() const -> double { return hours() * 60.0; }
    [[nodiscard]] auto cost_usd() const -> double { return hours() * ocpu_hourly_rate * ocpus; }
};

struct TestCostReport {
    std::string test_name;
    std::vector<BilledResource> resources;

    [[nodiscard]] auto total_usd() const -> double {
        double total = 0.0;
        for (const auto& r : resources) {
            total += r.cost_usd();
        }
        return total;
    }

    [[nodiscard]] auto format() const -> std::string {
        std::ostringstream oss;
        oss << std::fixed;
        oss << "\n[oci-cost] " << test_name << "\n";
        for (const auto& r : resources) {
            oss << "[oci-cost]   " << std::left << std::setw(38) << r.label << std::right
                << std::setw(7) << std::setprecision(1) << r.minutes() << " min"
                << "   ";
            if (r.rate_known) {
                oss << "$" << std::setprecision(6) << r.cost_usd() << "\n";
            } else {
                oss << "(rate unknown)\n";
            }
        }
        oss << "[oci-cost]   " << std::left << std::setw(38) << "TOTAL" << std::right
            << std::setw(11) << " "
            << "$" << std::setprecision(6) << total_usd() << "\n";
        return oss.str();
    }
};

struct CostAccumulator {
    std::mutex mtx;
    std::vector<TestCostReport> reports;

    void add(TestCostReport report) {
        const std::lock_guard<std::mutex> lock{mtx};
        reports.push_back(std::move(report));
    }
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline CostAccumulator g_cost_accumulator;

struct CostSummaryFixture {
    ~CostSummaryFixture() {
        const std::lock_guard<std::mutex> lock{g_cost_accumulator.mtx};
        const auto& reports = g_cost_accumulator.reports;
        // Loud, not silent — the same branch the AWS and GCP versions carry,
        // and for the reason the GCP one records: a bare `return` makes "no
        // case registered a cost" indistinguishable from "this fixture never
        // ran", and in that suite the second was silently true for every run
        // ever made.
        if (reports.empty()) {
            std::cerr << "\n[oci-cost] WARNING: no cost reports were registered. Either no case "
                         "provisioned a billable resource, or a case failed to file its "
                         "TestCostReport. Real-cloud spend for this run is UNREPORTED.\n";
            return;
        }

        double grand = 0.0;
        for (const auto& r : reports) {
            grand += r.total_usd();
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6);
        oss << "\n================================================================\n";
        oss << " OCI Real-Cloud Test Cost Estimate Summary\n";
        oss << "================================================================\n";
        bool any_unknown = false;
        for (const auto& r : reports) {
            // The per-resource detail, not just the total. Without it a
            // resource whose rate could not be looked up contributes 0 to a
            // total that then reads as authoritative — which is how this
            // suite's first live run reported $0.000000 for a real instance.
            oss << r.format();
            for (const auto& res : r.resources) {
                any_unknown = any_unknown || !res.rate_known;
            }
        }
        oss << "----------------------------------------------------------------\n";
        for (const auto& r : reports) {
            oss << "  " << std::left << std::setw(52) << r.test_name << "  $" << r.total_usd()
                << "\n";
        }
        oss << "----------------------------------------------------------------\n";
        oss << "  " << std::left << std::setw(52) << "GRAND TOTAL"
            << "  $" << grand << "\n";
        oss << "================================================================\n";
        if (any_unknown) {
            oss << " WARNING: at least one resource had NO KNOWN RATE and contributed\n";
            oss << " $0 to the total above. That total is an UNDER-estimate, not a\n";
            oss << " measurement. Look for '(rate unknown)' in the detail above.\n";
            oss << "================================================================\n";
        }
        oss << " Pricing: on-demand OCPU-hour rates queried from Oracle's public\n";
        oss << " price list at test start. Preemptible instances bill at a\n";
        oss << " discount that is NOT published as a separate SKU, so any\n";
        oss << " preemptible line here is an OVER-estimate. Use OCI Cost\n";
        oss << " Analysis for authoritative billing data.\n";
        oss << "================================================================\n";
        // std::cerr, not BOOST_TEST_MESSAGE: Boost's default log level drops
        // `message`-level records, which is why no real-cloud suite's cost
        // summary has ever reached a CI log.
        std::cerr << oss.str() << std::flush;
    }
};

// ── Preemptible-first launch ladder (Requirements 13.12-13.15) ─────────────

/// One rung: a shape in a specific Availability Domain, at a market.
struct launch_option {
    std::string shape;
    std::string availability_domain;
    double ocpus{1.0};
    double memory_gbs{6.0};
    bool preemptible{false};
    std::optional<double> ocpu_hourly_rate;  ///< on-demand rate; nullopt = unknown

    [[nodiscard]] auto describe() const -> std::string {
        std::ostringstream oss;
        oss << shape << " x" << ocpus << " in " << availability_domain << " ("
            << (preemptible ? "preemptible" : "on-demand") << ")";
        return oss.str();
    }
};

/// @brief Build the escalation ladder, cheapest-first, preemptible first.
///
/// **Keyed on (shape, Availability Domain) pairs, not shape alone**
/// (Requirement 13.13), and that is not a theoretical refinement: Finding 14
/// observed `VM.Standard.A1.Flex` refused in `PHX-AD-2` while the identical
/// request succeeded in AD-1 and AD-3, at the same moment. A shape-only ladder
/// retries into the same shortage.
///
/// **On ordering and the missing preemptible price.** Finding 15 found no
/// preemptible SKU in Oracle's price list — it is a discount off on-demand, and
/// this pass never established the discount factor. Rather than invent one,
/// this orders the preemptible rungs by their *on-demand* rate. If the discount
/// is uniform across shapes, that ordering is identical to the true
/// preemptible ordering, since a constant factor cannot reorder a sorted list.
/// If it is not uniform, this ordering is approximate — and saying so is better
/// than encoding a number nobody verified.
///
/// Requirement 13.12 wants the preemptible run truncated at the cheapest
/// reliably-available on-demand fallback, which is appended last.
[[nodiscard]] inline auto preemptible_first_launch_options(
    const std::vector<std::string>& availability_domains, const std::string& fallback_shape,
    double ocpus, double memory_gbs) -> std::vector<launch_option> {
    // Candidate families, cheapest-looking first. Ampere is deliberately NOT
    // first despite listing at USD 0: that is the Always Free allocation rather
    // than a true zero rate, and Finding 14 found it is also the family most
    // likely to be out of capacity. Leading with it would make the common case
    // "walk the whole ladder", which is slow and noisy even though it works.
    static const std::vector<std::string> kPreemptibleFamilies{"E4", "E5", "A1"};

    std::vector<launch_option> ladder;
    const auto fallback_rate = ocpu_hourly_rate_usd(fallback_shape);

    for (const auto& family : kPreemptibleFamilies) {
        const auto rate = ocpu_hourly_rate_usd(family);
        // Requirement 13.12: stop once a preemptible candidate costs at least
        // as much as the on-demand fallback — there is no reason to keep
        // trying preemptible past that point.
        if (rate.has_value() && fallback_rate.has_value() && *rate >= *fallback_rate) {
            continue;
        }
        for (const auto& ad : availability_domains) {
            ladder.push_back(launch_option{.shape = "VM.Standard." + family + ".Flex",
                                           .availability_domain = ad,
                                           .ocpus = ocpus,
                                           .memory_gbs = memory_gbs,
                                           .preemptible = true,
                                           .ocpu_hourly_rate = rate});
        }
    }

    std::stable_sort(ladder.begin(), ladder.end(),
                     [](const launch_option& a, const launch_option& b) {
                         // Unknown rates sort last rather than as zero, so a
                         // price-list outage degrades to "try them in the
                         // listed order" instead of "try the unpriced one
                         // first because 0 is cheap".
                         const double ra = a.ocpu_hourly_rate.value_or(1e9);
                         const double rb = b.ocpu_hourly_rate.value_or(1e9);
                         return ra < rb;
                     });

    // The on-demand fallback, last (Requirement 13.12).
    for (const auto& ad : availability_domains) {
        ladder.push_back(launch_option{.shape = fallback_shape,
                                       .availability_domain = ad,
                                       .ocpus = ocpus,
                                       .memory_gbs = memory_gbs,
                                       .preemptible = false,
                                       .ocpu_hourly_rate = fallback_rate});
    }
    return ladder;
}

// ── Preflight (Requirement 13.9) ───────────────────────────────────────────

/// Read an env var, or empty.
[[nodiscard]] inline auto env(const char* name) -> std::string {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string{value};
}

/// @brief Every OCID the real-OCI fixtures need, read from the environment.
///
/// Requirement 13.11: **no auto-creation fallback at all**, unlike the AWS
/// fixture's VPC/subnet path. These components never create an Instance Pool,
/// Instance Configuration, VCN or Certificate Authority, so a fixture that
/// created them would be testing something the library cannot do.
struct real_test_config {
    std::string region;
    std::string tenancy_id;
    std::string user_id;
    std::string fingerprint;
    std::string private_key_pem;
    std::string compartment_id;
    std::string instance_pool_id;
    std::string certificate_authority_id;
    std::string subnet_id;

    [[nodiscard]] static auto from_environment() -> real_test_config {
        return real_test_config{
            .region = env("KYTHIRA_OCI_REGION"),
            .tenancy_id = env("KYTHIRA_OCI_TENANCY_ID"),
            .user_id = env("KYTHIRA_OCI_USER_ID"),
            .fingerprint = env("KYTHIRA_OCI_FINGERPRINT"),
            .private_key_pem = env("KYTHIRA_OCI_PRIVATE_KEY_PEM"),
            .compartment_id = env("KYTHIRA_OCI_COMPARTMENT_ID"),
            .instance_pool_id = env("KYTHIRA_OCI_INSTANCE_POOL_ID"),
            .certificate_authority_id = env("KYTHIRA_OCI_CERTIFICATE_AUTHORITY_ID"),
            .subnet_id = env("KYTHIRA_OCI_SUBNET_ID"),
        };
    }

    /// The credentials every suite needs, regardless of which resources it uses.
    [[nodiscard]] auto has_credentials() const -> bool {
        return !region.empty() && !tenancy_id.empty() && !user_id.empty() && !fingerprint.empty() &&
               !private_key_pem.empty();
    }
};

// ── Signal-driven cleanup (Requirement 14.5) ───────────────────────────────

/// Implemented by any fixture that allocates OCI resources, so a trappable
/// signal mid-run can still tear them down. Non-public destructor: this is
/// never used to `delete` through a base pointer, only to call `teardown()`.
struct signal_cleanup_target {
    virtual void teardown() noexcept = 0;

protected:
    ~signal_cleanup_target() = default;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
inline std::atomic<signal_cleanup_target*> g_active_oci_fixture{nullptr};

inline void oci_signal_cleanup_handler(int sig) {
    signal_cleanup_target* fixture =
        g_active_oci_fixture.exchange(nullptr, std::memory_order_acq_rel);
    if (fixture != nullptr) {
        fixture->teardown();
    }
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
    raise(sig);
}

/// SIGABRT/SIGSEGV/SIGBUS as well as the polite termination signals, matching
/// the AWS handler and for the reason recorded there: a crash leaks exactly the
/// same resources a `ctest` TIMEOUT kill does, and previously hit none of the
/// covered signals. Calling `teardown()` — HTTP calls, allocation,
/// mutex-protected containers — from a signal handler is not async-signal-safe,
/// and is riskiest for SIGSEGV/SIGBUS where memory may already be corrupt. That
/// is a deliberate trade for a test-cleanup path: best case the instances this
/// run owns are terminated before the process dies; worst case `teardown()`
/// itself faults, which is no worse than the unconditional leak it replaces.
/// `SA_RESETHAND` makes that worst case safe — a second fault of the same
/// signal during teardown falls through to the kernel's default action rather
/// than recursing.
inline void install_oci_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = oci_signal_cleanup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGABRT, SIGSEGV, SIGBUS}) {
        sigaction(sig, &sa, nullptr);
    }
}

struct OciSignalHandlerFixture {
    OciSignalHandlerFixture() { install_oci_signal_handlers(); }
};

}  // namespace kythira::testing::oci_real
