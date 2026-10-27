// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE alibaba_oss_persistence_real_test
#include <boost/test/unit_test.hpp>

#include "alibaba_real_test_support.hpp"

#include <raft/alibaba_oss_persistence.hpp>

#include "object_persistence_real_cases.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Real-Alibaba persistence suite (spec Requirement 16.8-16.10). Compiled
// whenever the Alibaba gates are on, NEVER registered with CTest — run by
// name, by the CI job, against a real OSS bucket.
//
// What this tier proves that the mock tier cannot: that the OSS V4 signature
// is accepted by the real service over https with virtual-host addressing,
// and that the durability contract (Requirement 15.2) holds against real
// object semantics rather than a local in-memory store. The content-type
// signing bug this provider shipped with was invisible to every local tier
// and surfaced on the first live PUT — which is the argument for this suite
// existing at all.
//
// Each case namespaces its objects under a unique prefix and deletes them in
// teardown, so a shared CI bucket never accumulates state.

using namespace kythira;

namespace {

auto unique_prefix() -> std::string {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return "kythira-real-test/" +
           std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct RealOssFixture {
    alibaba_real::real_test_config cfg{alibaba_real::real_test_config::from_environment()};
    std::string prefix{unique_prefix()};

    RealOssFixture() {
        alibaba_real::skip_unless_configured(cfg, {{"KYTHIRA_ALIBABA_OSS_BUCKET", cfg.oss_bucket}});
        alibaba_real::skip_unless_reachable(cfg);

        // Registered before anything is written, so a signal mid-test still
        // cleans up.
        const auto bucket = cfg.oss_bucket;
        const auto client_cfg = cfg.client_config();
        const auto owned_prefix = prefix;
        alibaba_real::teardown_registry().emplace_back([client_cfg, bucket, owned_prefix] {
            const alibaba_oss_client client{client_cfg};
            for (const auto& key : client.list_keys(bucket, owned_prefix)) {
                client.delete_object(bucket, key);
            }
        });
    }

    [[nodiscard]] auto engine() const -> alibaba_oss_persistence_engine<> {
        return alibaba_oss_persistence_engine<>{cfg.client_config(), cfg.oss_bucket, prefix};
    }
};

}  // namespace

// BOOST_GLOBAL_FIXTURE pastes its argument into an identifier, so a
// namespace-qualified type does not compile. Alias first — the same shape
// the OCI real suites use.
using AlibabaSignalHandlerFixture = alibaba_real::signal_handler_fixture;
BOOST_GLOBAL_FIXTURE(AlibabaSignalHandlerFixture);

BOOST_AUTO_TEST_SUITE(alibaba_oss_persistence_real)

// The durability claim, against the real service: a returned write is
// readable by a FRESH engine that shares no memory with the writer. This is
// the property Requirement 15.2 argues for and the one a local mirror could
// otherwise fake.
BOOST_FIXTURE_TEST_CASE(term_and_vote_survive_a_fresh_engine, RealOssFixture,
                        *boost::unit_test::timeout(300)) {
    {
        auto writer = engine();
        writer.save_current_term(7);
        writer.save_voted_for(42);
    }
    auto reader = engine();
    BOOST_TEST(reader.load_current_term() == 7U);
    BOOST_REQUIRE(reader.load_voted_for().has_value());
    BOOST_TEST(reader.load_voted_for().value() == 42U);
}

BOOST_FIXTURE_TEST_CASE(log_entries_round_trip_in_index_order, RealOssFixture,
                        *boost::unit_test::timeout(300)) {
    {
        auto writer = engine();
        for (std::uint64_t i = 1; i <= 12; ++i) {
            log_entry<> e;
            e._term = 1;
            e._index = i;
            const std::string payload = "command-" + std::to_string(i);
            e._command.assign(reinterpret_cast<const std::byte*>(payload.data()),
                              reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
            writer.append_log_entry(e);
        }
    }
    // A fresh engine reconstructs its mirror from a real paginated List —
    // the path where 20-digit zero-padding earns its keep (design Property 5).
    auto reader = engine();
    BOOST_TEST(reader.get_last_log_index() == 12U);
    const auto entries = reader.get_log_entries(1, 12);
    BOOST_REQUIRE(entries.size() == 12U);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        BOOST_TEST(entries[i].index() == i + 1);
    }
}

BOOST_FIXTURE_TEST_CASE(truncate_removes_objects_for_real, RealOssFixture,
                        *boost::unit_test::timeout(300)) {
    {
        auto writer = engine();
        for (std::uint64_t i = 1; i <= 5; ++i) {
            log_entry<> e;
            e._term = 1;
            e._index = i;
            writer.append_log_entry(e);
        }
        writer.truncate_log(3);  // removes indices >= 3
    }
    auto reader = engine();
    BOOST_TEST(reader.get_last_log_index() == 2U);
    BOOST_TEST(!reader.get_log_entry(3).has_value());
}

// Binary-safe commands over the real wire: base64 in the record, arbitrary
// bytes out. Split literal because hex escapes are greedy.
BOOST_FIXTURE_TEST_CASE(binary_command_survives_the_real_service, RealOssFixture,
                        *boost::unit_test::timeout(300)) {
    const std::string payload(
        "\x00\x01\xff"
        "binary\x00\x00",
        11);
    {
        auto writer = engine();
        log_entry<> e;
        e._term = 3;
        e._index = 1;
        e._command.assign(reinterpret_cast<const std::byte*>(payload.data()),
                          reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
        writer.append_log_entry(e);
    }
    auto reader = engine();
    const auto got = reader.get_log_entry(1);
    BOOST_REQUIRE(got.has_value());
    BOOST_TEST(got->command().size() == payload.size());
}

// ── The shared task-16 cases ─────────────────────────────────────────────────
//
// The same checks every other provider's real tier runs
// (object_persistence_real_cases.hpp), so "Alibaba passes" means the same thing
// as "S3 passes" rather than something written separately and hoped to be
// equivalent.
//
// **The fencing case is absent, and its absence is the finding.** Alibaba OSS
// is the one provider of the five that cannot express an overwrite
// compare-and-swap — a conditional PUT against a *current* ETag is refused with
// `400 NotImplemented` (spike-notes Finding 1), which is why
// `alibaba_oss_persistence.hpp` carries a static_assert that
// `alibaba_oss_client` does NOT satisfy `conditional_key_object_store`. The
// case is therefore not skipped here, it is *uninstantiable*: calling it would
// not compile. Requirement 9.8 fires for exactly this provider.

BOOST_FIXTURE_TEST_CASE(shared_measured_latency, RealOssFixture, *boost::unit_test::timeout(900)) {
    kythira::object_real::case_measured_latency(alibaba_oss_client{cfg.client_config()},
                                                cfg.oss_bucket, prefix + "/latency", "oss");
}

BOOST_FIXTURE_TEST_CASE(shared_backup_verify_restore_read_back, RealOssFixture,
                        *boost::unit_test::timeout(900)) {
    kythira::object_real::case_backup_verify_restore_read_back(
        alibaba_oss_client{cfg.client_config()}, cfg.oss_bucket, prefix + "/src",
        prefix + "/backups", prefix + "/restored");
}

BOOST_FIXTURE_TEST_CASE(shared_list_after_write, RealOssFixture, *boost::unit_test::timeout(900)) {
    kythira::object_real::case_list_after_write(alibaba_oss_client{cfg.client_config()},
                                                cfg.oss_bucket, prefix + "/law", "oss");
}

// object-store-batched-layout task 10. The fenced segment case and the retired
// vote slot's negative control are absent for the reason the fencing case is.

BOOST_FIXTURE_TEST_CASE(shared_layout_migration_read_back, RealOssFixture,
                        *boost::unit_test::timeout(900)) {
    kythira::object_real::case_layout_migration_read_back(alibaba_oss_client{cfg.client_config()},
                                                          cfg.oss_bucket, prefix + "/migrate");
}

BOOST_FIXTURE_TEST_CASE(shared_segment_truncate_compact, RealOssFixture,
                        *boost::unit_test::timeout(900)) {
    kythira::object_real::case_segment_truncate_compact<
        kythira::object_real::engine_for<alibaba_oss_client>>(
        alibaba_oss_client{cfg.client_config()}, cfg.oss_bucket, prefix + "/segment",
        kythira::object_real::batched_options());
}

BOOST_FIXTURE_TEST_CASE(shared_batched_latency, RealOssFixture, *boost::unit_test::timeout(900)) {
    kythira::object_real::case_batched_latency(alibaba_oss_client{cfg.client_config()},
                                               cfg.oss_bucket, prefix + "/batched-latency", "oss");
}

BOOST_FIXTURE_TEST_CASE(shared_batched_backup_verify_restore, RealOssFixture,
                        *boost::unit_test::timeout(900)) {
    kythira::object_real::case_batched_backup_verify_restore(
        alibaba_oss_client{cfg.client_config()}, cfg.oss_bucket, prefix + "/batched-src",
        prefix + "/batched-backups", prefix + "/batched-restored");
}

BOOST_FIXTURE_TEST_CASE(shared_write_behind_commit_rate, RealOssFixture,
                        *boost::unit_test::timeout(900)) {
    kythira::object_real::case_write_behind_commit_rate(
        alibaba_oss_client{cfg.client_config()}, cfg.oss_bucket, prefix + "/commit-rate", "oss");
}

// The opt-in segment-size sweep (object-store-segment-size-sweep task 2).
// Each case passes at once unless KYTHIRA_SEGMENT_SWEEP=true. The truncation
// rewrite runs on the unfenced engine, for the reason the fencing case is
// absent (Requirement 4.1).
BOOST_FIXTURE_TEST_CASE(shared_segment_sweep_put, RealOssFixture,
                        *boost::unit_test::timeout(1800)) {
    kythira::object_real::case_segment_sweep_put(alibaba_oss_client{cfg.client_config()},
                                                 cfg.oss_bucket, prefix + "/sweep", "oss");
}

BOOST_FIXTURE_TEST_CASE(shared_segment_sweep_follower_run, RealOssFixture,
                        *boost::unit_test::timeout(1800)) {
    kythira::object_real::case_segment_sweep_follower_run(alibaba_oss_client{cfg.client_config()},
                                                          cfg.oss_bucket, prefix + "/sweep", "oss");
}

BOOST_FIXTURE_TEST_CASE(shared_segment_sweep_truncate, RealOssFixture,
                        *boost::unit_test::timeout(1800)) {
    kythira::object_real::case_segment_sweep_truncate<
        kythira::object_real::engine_for<alibaba_oss_client>>(
        alibaba_oss_client{cfg.client_config()}, cfg.oss_bucket, prefix + "/sweep", "oss", "");
}

// 4200 s, not the 1800 s of the other slices. Run 40516345843 spent the whole
// 1800 s here without finishing: every one of the 35 timed constructions
// reads the 16 MiB log back sequentially from a GitHub runner in the US to
// OSS in Asia, plus seven 16 MiB writes. Run 40553670229 measured it: about
// two minutes of writes, then about 7.7 minutes per sampling round, so some
// 41 minutes in all. The other providers finish in a few minutes, so only
// this case gets the longer limit.
//
// Not more than about 4294 s: Boost.Test holds the timeout in microseconds in
// 32 bits, so 5400 s wrapped to 5400e6 - 2^32 us and fired SIGALRM after
// 1105 s on run 40553670229.
BOOST_FIXTURE_TEST_CASE(shared_segment_sweep_recovery, RealOssFixture,
                        *boost::unit_test::timeout(4200)) {
    kythira::object_real::case_segment_sweep_recovery(alibaba_oss_client{cfg.client_config()},
                                                      cfg.oss_bucket, prefix + "/sweep", "oss");
}

BOOST_FIXTURE_TEST_CASE(shared_segment_sweep_write_behind, RealOssFixture,
                        *boost::unit_test::timeout(1800)) {
    kythira::object_real::case_segment_sweep_write_behind(alibaba_oss_client{cfg.client_config()},
                                                          cfg.oss_bucket, prefix + "/sweep", "oss");
}

BOOST_AUTO_TEST_SUITE_END()
