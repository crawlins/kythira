// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: grpc-tls-reload
//
// Unit tests for issuing_tls_material_source (.kiro/specs/grpc-tls-reload/,
// Requirement 6, Task 7): first issuance publishes generation 1, renewal at
// two thirds of a short validity publishes a fresh key, a failing provider
// keeps the old material while backing off and reporting, and expiry without
// renewal is reported once.

#define BOOST_TEST_MODULE IssuingTlsMaterialSourceUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>
#include <raft/issuing_tls_material_source.hpp>

#include "recording_metrics.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using kythira::testing::recording_metrics;

// local_certificate_provider with a switch that makes signing fail, standing
// in for an unreachable or refusing CA.
class switchable_provider {
public:
    explicit switchable_provider(raft::testing::certificate_authority& ca) : _local(ca) {}

    [[nodiscard]] auto root_certificate_pem() -> kythira::future_default<std::string> {
        return _local.root_certificate_pem();
    }

    [[nodiscard]] auto sign_csr(std::string csr_pem, raft::testing::csr_signing_options options)
        -> kythira::future_default<raft::testing::pem_material> {
        ++calls;
        if (failing.load()) {
            throw std::runtime_error("provider unavailable");
        }
        return _local.sign_csr(std::move(csr_pem), std::move(options));
    }

    std::atomic<bool> failing{false};
    std::atomic<int> calls{0};

private:
    raft::testing::local_certificate_provider _local;
};
static_assert(raft::testing::certificate_provider<switchable_provider>);

auto options_with_validity(std::chrono::seconds validity) -> kythira::issuing_tls_material_options {
    kythira::issuing_tls_material_options o;
    o.subject.common_name = "node-1";
    o.signing.dns_names = {"node-1"};
    o.signing.server_auth = true;
    o.signing.client_auth = true;
    o.signing.validity = validity;
    o.initial_backoff = 50ms;
    o.max_backoff = 200ms;
    return o;
}

template<typename Pred>
auto eventually(Pred pred, std::chrono::milliseconds deadline = 10s) -> bool {
    auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return pred();
}

}  // namespace

BOOST_AUTO_TEST_CASE(first_issuance_publishes_generation_one) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    kythira::issuing_tls_material_source<switchable_provider> source(
        provider, options_with_validity(std::chrono::hours(1)));

    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(source.self_refreshing());
    auto m = source.current();
    BOOST_REQUIRE(m);
    BOOST_CHECK_NO_THROW(kythira::validate_tls_material(*m, "issued"));
    BOOST_TEST(m->root_certificates_pem == ca.root_certificate_pem());

    // Renewal is scheduled two thirds of the way through the hour.
    auto due = source.renewal_due();
    BOOST_REQUIRE(due.has_value());
    auto until_due =
        std::chrono::duration_cast<std::chrono::seconds>(*due - std::chrono::system_clock::now());
    BOOST_TEST(until_due.count() > 2350);
    BOOST_TEST(until_due.count() <= 2400);
}

BOOST_AUTO_TEST_CASE(renewal_publishes_a_fresh_key) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    kythira::issuing_tls_material_source<switchable_provider> source(provider,
                                                                     options_with_validity(3s));
    auto first = source.current();

    std::atomic<std::uint64_t> notified{0};
    auto sub = source.subscribe([&](auto, std::uint64_t gen) { notified = gen; });

    // Two thirds of 3s; allow for whole-second certificate times.
    BOOST_REQUIRE(eventually([&] { return source.generation() >= 2U; }, 6s));
    auto second = source.current();
    BOOST_TEST(second->private_key_pem != first->private_key_pem);
    BOOST_TEST(second->certificate_chain_pem != first->certificate_chain_pem);
    BOOST_TEST(notified.load() >= 2U);
}

BOOST_AUTO_TEST_CASE(explicit_refresh_issues_now_with_a_fresh_key) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    kythira::issuing_tls_material_source<switchable_provider> source(
        provider, options_with_validity(std::chrono::hours(1)));
    auto first = source.current();
    source.refresh();
    BOOST_TEST(source.generation() == 2U);
    BOOST_TEST(source.current()->private_key_pem != first->private_key_pem);

    provider->failing = true;
    BOOST_CHECK_THROW(source.refresh(), std::runtime_error);
    BOOST_TEST(source.generation() == 2U);
}

// A provider that stops signing: the old material stays, failures are
// reported with backoff rather than a busy loop, expiry is reported once,
// and the source recovers when the provider does.
BOOST_AUTO_TEST_CASE(failing_renewal_keeps_material_backs_off_and_reports_expiry) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    recording_metrics metrics;
    std::atomic<int> reported{0};
    kythira::issuing_tls_material_source<switchable_provider, recording_metrics> source(
        provider, options_with_validity(2s), metrics, [&](std::string_view) { ++reported; });
    auto first = source.current();
    provider->failing = true;

    const auto& rec = *metrics.recorder();
    BOOST_REQUIRE(
        eventually([&] { return rec.count_named("tls_material_source.expired") == 1U; }, 6s));
    BOOST_TEST(source.generation() == 1U);
    BOOST_TEST(source.current() == first);
    BOOST_TEST(rec.count_named("tls_material_source.renewal.failed") >= 1U);
    BOOST_TEST(reported.load() >= 1);

    // Backoff is capped at 200ms, so over one second there are a handful of
    // attempts, not thousands.
    auto calls_before = provider->calls.load();
    std::this_thread::sleep_for(1s);
    auto attempts = provider->calls.load() - calls_before;
    BOOST_TEST(attempts >= 3);
    BOOST_TEST(attempts <= 12);
    BOOST_TEST(rec.count_named("tls_material_source.expired") == 1U);

    provider->failing = false;
    BOOST_REQUIRE(eventually([&] { return source.generation() == 2U; }, 3s));
    BOOST_TEST(source.current()->private_key_pem != first->private_key_pem);
}

// A provider that is down at startup: generation 0 and no material until it
// comes up, never empty material.
BOOST_AUTO_TEST_CASE(failed_first_issuance_stays_at_generation_zero_then_recovers) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    provider->failing = true;
    kythira::issuing_tls_material_source<switchable_provider> source(
        provider, options_with_validity(std::chrono::hours(1)));

    BOOST_TEST(source.generation() == 0U);
    BOOST_TEST(!source.current());
    BOOST_TEST(source.failure_count() >= 1U);

    provider->failing = false;
    BOOST_REQUIRE(eventually([&] { return source.generation() == 1U; }, 3s));
    BOOST_CHECK_NO_THROW(kythira::validate_tls_material(*source.current(), "issued"));
}

BOOST_AUTO_TEST_CASE(invalid_options_are_refused) {
    raft::testing::certificate_authority ca;
    auto provider = std::make_shared<switchable_provider>(ca);
    auto bad_fraction = options_with_validity(std::chrono::hours(1));
    bad_fraction.renew_at_fraction = 1.0;
    BOOST_CHECK_THROW(
        (kythira::issuing_tls_material_source<switchable_provider>(provider, bad_fraction)),
        std::invalid_argument);
    auto bad_backoff = options_with_validity(std::chrono::hours(1));
    bad_backoff.max_backoff = 10ms;
    BOOST_CHECK_THROW(
        (kythira::issuing_tls_material_source<switchable_provider>(provider, bad_backoff)),
        std::invalid_argument);
    BOOST_CHECK_THROW((kythira::issuing_tls_material_source<switchable_provider>(
                          nullptr, options_with_validity(std::chrono::hours(1)))),
                      std::invalid_argument);
}
