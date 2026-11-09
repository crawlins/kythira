// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// checkpoint_registry (.kiro/specs/file-install-snapshot/ Requirement 5):
// one checkpoint per (index, term) shared by every lease, kept while
// leased, deleted when unused and superseded or too old, and counted for
// the metric.

#define BOOST_TEST_MODULE checkpoint_registry_test
#include <boost/test/unit_test.hpp>

#include <raft/checkpoint_registry.hpp>
#include <raft/memory_kv_engine.hpp>

#include "format1_snapshot_transfer.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {

using kythira::checkpoint_registry;
using namespace std::chrono_literals;

const kythira::key_range k_range{.start = "a", .end = std::string("z")};

struct fixture {
    kythira::test::format1::temp_dir root;
    kythira::memory_kv_engine engine;
    int takes = 0;

    fixture() {
        kythira::write_batch batch;
        batch.put(kythira::column_family::data, "k", std::string(100, 'x'));
        engine.write(batch);
    }

    auto take() {
        return [this](const std::filesystem::path& dir) {
            ++takes;
            return engine.checkpoint(k_range, dir);
        };
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(leases_at_the_same_point_share_one_checkpoint, fixture) {
    checkpoint_registry reg(root.path(), 10min);
    auto a = reg.acquire(10, 2, take());
    auto b = reg.acquire(10, 2, take());
    BOOST_TEST(takes == 1);
    BOOST_TEST(a.directory() == b.directory());
    BOOST_TEST(std::filesystem::exists(a.directory()));
    BOOST_TEST(reg.count() == 1U);
    BOOST_TEST(reg.bytes() == a.manifest().total_bytes());
    BOOST_TEST(reg.bytes() > 0U);
    BOOST_TEST(a.directory().filename().string().starts_with(checkpoint_registry::k_prefix));
}

BOOST_FIXTURE_TEST_CASE(a_leased_checkpoint_is_never_collected, fixture) {
    checkpoint_registry reg(root.path(), 0ms);
    auto a = reg.acquire(10, 2, take());
    const auto dir = a.directory();
    auto copy = a;
    BOOST_TEST(reg.collect() == 0U);
    BOOST_TEST(reg.release_unused() == 0U);
    a = {};
    BOOST_TEST(reg.release_unused() == 0U);  // the copy still holds it
    copy = {};
    BOOST_TEST(reg.collect() == 1U);
    BOOST_TEST(!std::filesystem::exists(dir));
    BOOST_TEST(reg.count() == 0U);
}

BOOST_FIXTURE_TEST_CASE(an_unused_checkpoint_is_kept_for_its_age, fixture) {
    checkpoint_registry reg(root.path(), 10min);
    std::filesystem::path dir;
    {
        auto a = reg.acquire(10, 2, take());
        dir = a.directory();
    }
    BOOST_TEST(reg.collect() == 0U);
    // A failed transfer starting again gets the same files.
    auto again = reg.acquire(10, 2, take());
    BOOST_TEST(takes == 1);
    BOOST_TEST(again.directory() == dir);
    again = {};
    BOOST_TEST(reg.collect(checkpoint_registry::clock::now() + 11min) == 1U);
    BOOST_TEST(!std::filesystem::exists(dir));
}

BOOST_FIXTURE_TEST_CASE(a_newer_checkpoint_supersedes_unused_older_ones, fixture) {
    checkpoint_registry reg(root.path(), 10min);
    std::filesystem::path old_unused;
    {
        auto a = reg.acquire(10, 2, take());
        old_unused = a.directory();
    }
    auto old_leased = reg.acquire(11, 2, take());
    BOOST_TEST(!std::filesystem::exists(old_unused));  // superseded by 11
    auto newer = reg.acquire(12, 2, take());
    BOOST_TEST(std::filesystem::exists(old_leased.directory()));  // still leased
    BOOST_TEST(reg.count() == 2U);
    old_leased = {};
    BOOST_TEST(reg.release_unused() == 1U);
    BOOST_TEST(reg.count() == 1U);
    BOOST_TEST(std::filesystem::exists(newer.directory()));
}

BOOST_FIXTURE_TEST_CASE(a_failed_checkpoint_leaves_nothing, fixture) {
    checkpoint_registry reg(root.path(), 10min);
    std::filesystem::path seen;
    BOOST_CHECK_THROW(
        reg.acquire(10, 2,
                    [&](const std::filesystem::path& dir) -> kythira::snapshot_manifest {
                        seen = dir;
                        std::filesystem::create_directory(dir);
                        throw std::runtime_error("disk full");
                    }),
        std::runtime_error);
    BOOST_TEST(!std::filesystem::exists(seen));
    BOOST_TEST(reg.count() == 0U);
    auto ok = reg.acquire(10, 2, take());
    BOOST_TEST(std::filesystem::exists(ok.directory()));
}
