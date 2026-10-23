// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE multi_raft_node_s3_store_test
#include <boost/test/unit_test.hpp>

// The S3 arm of multi_raft_node's switchable store, through the real
// aws_s3_client against tests/s3_stateful_mock.hpp
// (.kiro/specs/multi-raft-node-s3-persistence/ tasks 6.2 and 6.3,
// Requirements 4, 10.2, 11.1 and 11.2).
//
// The engine's own suite proves the engine. What can go wrong here is the
// wrapper: `switchable_persistence` answering the wrong durability class, a
// barrier not reaching the engine, or a host prefix two hosts share. Each of
// those loses data or batching with no error, so every case goes through
// `make_store()`, the function the host itself calls.

#include "config.hpp"
#include "s3_describe.hpp"
#include "s3_stateful_mock.hpp"
#include "switchable_persistence.hpp"

#include <aws/core/Aws.h>

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

using namespace kythira::bench;
using kythira::testing::s3_stateful_mock;

namespace {

struct aws_sdk_fixture {
    Aws::SDKOptions options;
    aws_sdk_fixture() {
        // Static keys and no profile, so the SDK chain answers at once and
        // nothing from the developer's machine leaks in.
        ::setenv("AWS_ACCESS_KEY_ID", "AKIDSTORETEST", 1);
        ::setenv("AWS_SECRET_ACCESS_KEY", "store-test-secret", 1);
        ::setenv("AWS_EC2_METADATA_DISABLED", "true", 1);
        ::unsetenv("AWS_PROFILE");
        ::unsetenv("AWS_ENDPOINT_URL");
        ::unsetenv("AWS_ENDPOINT_URL_S3");
        Aws::InitAPI(options);
    }
    ~aws_sdk_fixture() { Aws::ShutdownAPI(options); }
    aws_sdk_fixture(const aws_sdk_fixture&) = delete;
    auto operator=(const aws_sdk_fixture&) -> aws_sdk_fixture& = delete;
    aws_sdk_fixture(aws_sdk_fixture&&) = delete;
    auto operator=(aws_sdk_fixture&&) -> aws_sdk_fixture& = delete;
};

BOOST_TEST_GLOBAL_FIXTURE(aws_sdk_fixture);

using log_entry_t = switchable_persistence::log_entry_t;

auto entry(std::uint64_t term, std::uint64_t index) -> log_entry_t {
    const std::string text = "cmd-" + std::to_string(index);
    return log_entry_t{
        term, index,
        std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                               reinterpret_cast<const std::byte*>(text.data()) + text.size())};
}

/// A host's options as the parser would leave them: node `node` at
/// `raft_url`, `--persistence s3` against `mock`, then `extra`.
auto host_options(const s3_stateful_mock& mock, std::uint64_t node, const std::string& raft_url,
                  std::initializer_list<std::string> extra = {}) -> node_options {
    std::vector<std::string> args{"multi_raft_node",
                                  "--node-id",
                                  std::to_string(node),
                                  "--raft-port",
                                  "9001",
                                  "--data-port",
                                  "9002",
                                  "--control-port",
                                  "9003",
                                  "--peer",
                                  std::to_string(node) + "=" + raft_url,
                                  "--persistence",
                                  "s3",
                                  "--s3-bucket",
                                  "b",
                                  "--s3-prefix",
                                  "run",
                                  "--s3-endpoint",
                                  mock.endpoint(),
                                  "--s3-region",
                                  "us-east-1",
                                  "--s3-timeout",
                                  "5000"};
    args.insert(args.end(), extra);
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_node_options(static_cast<int>(argv.size()), argv.data());
}

auto log_prefix(const node_options& opt, std::uint64_t group) -> std::string {
    return kythira::group_key_prefix(s3_host_prefix(opt), group) + "/log/";
}

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(s3_arm)

BOOST_AUTO_TEST_CASE(state_survives_a_fresh_store_under_both_fencing_modes) {
    for (const char* fencing : {"cas", "none"}) {
        BOOST_TEST_CONTEXT("fencing " << fencing) {
            s3_stateful_mock mock;
            const auto opt =
                host_options(mock, 1, "http://127.0.0.1:9001", {"--s3-fencing", fencing});
            {
                auto store = make_store(opt, 7);
                store.save_current_term(3);
                store.save_voted_for(kythira::host_node_id_from_number(2));
                for (std::uint64_t i = 1; i <= 4; ++i) {
                    store.append_log_entry(entry(3, i));
                }
            }
            auto reloaded = make_store(opt, 7);
            BOOST_TEST(reloaded.load_current_term() == 3U);
            BOOST_TEST((reloaded.load_voted_for() == kythira::host_node_id_from_number(2)));
            BOOST_TEST(reloaded.get_last_log_index() == 4U);
            BOOST_TEST(reloaded.get_log_entry(4)->term() == 3U);

            // Requirement 2.3: the host prefix carries the node id, and the
            // group prefix the group, so nothing lands outside them.
            const auto keys = mock.keys("b");
            BOOST_REQUIRE(!keys.empty());
            for (const auto& key : keys) {
                BOOST_TEST(key.starts_with("run/nodes/1/groups/7/"), key);
            }
            const bool has_owner = !mock.keys("b", "run/nodes/1/groups/7/owner").empty();
            BOOST_TEST(has_owner == (std::string(fencing) == "cas"));
        }
    }
}

BOOST_AUTO_TEST_CASE(two_hosts_sharing_bucket_and_prefix_never_share_a_key) {
    s3_stateful_mock mock;
    auto one = make_store(host_options(mock, 1, "http://127.0.0.1:9001"), 1);
    auto two = make_store(host_options(mock, 2, "http://127.0.0.1:9101"), 1);
    one.save_current_term(5);
    two.save_current_term(9);
    BOOST_TEST(make_store(host_options(mock, 1, "http://127.0.0.1:9001"), 1).load_current_term() ==
               5U);
    BOOST_TEST(make_store(host_options(mock, 2, "http://127.0.0.1:9101"), 1).load_current_term() ==
               9U);
}

// Requirement 4.4: a second process under a reused node id is refused at
// construction, and a restart of the same host is not.
BOOST_AUTO_TEST_CASE(cas_refuses_a_second_process_with_the_same_node_id) {
    s3_stateful_mock mock;
    const auto original = host_options(mock, 1, "http://127.0.0.1:9001");
    {
        auto store = make_store(original, 1);
        store.save_current_term(2);
    }
    // The same host again: a restart, which claims the next epoch.
    BOOST_CHECK_NO_THROW((void)make_store(original, 1));
    const auto owner = mock.object("b", "run/nodes/1/groups/1/owner");
    BOOST_REQUIRE(owner.has_value());
    BOOST_TEST(contains(owner->body, R"("epoch":2)"), owner->body);

    const auto before = mock.put_log().size();
    std::string message;
    try {
        (void)make_store(host_options(mock, 1, "http://127.0.0.1:9201"), 1);
    } catch (const std::exception& e) {
        message = e.what();
    }
    BOOST_TEST(contains(message, "group 1"), message);
    BOOST_TEST(contains(message, "run/nodes/1/groups/1/owner"), message);
    BOOST_TEST(contains(message, "held by another process"), message);
    // Refused at the owner claim, before any write of its own.
    BOOST_TEST(mock.put_log().size() == before);
}

BOOST_AUTO_TEST_CASE(cas_none_does_not_write_an_owner) {
    s3_stateful_mock mock;
    auto store =
        make_store(host_options(mock, 1, "http://127.0.0.1:9001", {"--s3-fencing", "none"}), 1);
    store.save_current_term(1);
    BOOST_TEST(mock.keys("b", "run/nodes/1/groups/1/owner").empty());
}

BOOST_AUTO_TEST_CASE(status_registry_sums_flushes_and_reports_fenced_groups) {
    s3_stateful_mock mock;
    const auto opt = host_options(mock, 1, "http://127.0.0.1:9001",
                                  {"--s3-write-behind", "1", "--s3-log", "segmented"});
    auto store = make_store(opt, 3);
    const auto before = s3_store_registry::instance().summarise();
    const auto seq = store.append_log_entry_sequenced(entry(1, 1));
    store.barrier_through(seq);
    const auto after = s3_store_registry::instance().summarise();
    BOOST_TEST(after.flushes == before.flushes + 1);
    BOOST_TEST(after.fenced_groups.empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Requirement 11.1 and 11.2: group commit through the wrapper ───────────────

BOOST_AUTO_TEST_SUITE(write_behind_through_the_wrapper)

BOOST_AUTO_TEST_CASE(durability_follows_write_behind_in_every_combination) {
    s3_stateful_mock mock;
    for (const char* wb : {"0", "1"}) {
        for (const char* fencing : {"none", "cas"}) {
            for (const char* log : {"per-entry", "segmented"}) {
                BOOST_TEST_CONTEXT("write-behind " << wb << " fencing " << fencing << " log "
                                                   << log) {
                    const auto opt = host_options(
                        mock, 1, "http://127.0.0.1:9001",
                        {"--s3-write-behind", wb, "--s3-fencing", fencing, "--s3-log", log,
                         "--s3-prefix", std::string("dur-") + wb + fencing + log});
                    auto store = make_store(opt, 1);
                    const auto expected = std::string(wb) == "1"
                                              ? kythira::durability_class::barrier
                                              : kythira::durability_class::synchronous;
                    BOOST_TEST((store.durability() == expected));
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(segmented_stages_then_flushes_once_at_the_barrier) {
    s3_stateful_mock mock;
    const auto opt = host_options(mock, 1, "http://127.0.0.1:9001",
                                  {"--s3-write-behind", "1", "--s3-log", "segmented"});
    auto store = make_store(opt, 1);
    const auto logs = log_prefix(opt, 1);

    kythira::write_sequence last{};
    for (std::uint64_t i = 1; i <= 10; ++i) {
        last = store.append_log_entry_sequenced(entry(1, i));
    }
    BOOST_TEST(mock.put_count(logs) == 0U);
    BOOST_TEST(store.durable_through() < last);

    store.barrier_through(last);
    BOOST_TEST(mock.put_count(logs) == 1U);
    BOOST_TEST(store.durable_through() == last);

    auto fresh = make_store(opt, 1);
    BOOST_TEST(fresh.get_last_log_index() == 10U);
    BOOST_TEST(fresh.get_log_entries(1, 10).size() == 10U);
}

BOOST_AUTO_TEST_CASE(per_entry_flush_is_bounded_by_append_concurrency) {
    s3_stateful_mock mock;
    const auto opt = host_options(
        mock, 1, "http://127.0.0.1:9001",
        {"--s3-write-behind", "1", "--s3-log", "per-entry", "--s3-append-concurrency", "4"});
    auto store = make_store(opt, 1);
    const auto logs = log_prefix(opt, 1);

    kythira::write_sequence last{};
    for (std::uint64_t i = 1; i <= 10; ++i) {
        last = store.append_log_entry_sequenced(entry(1, i));
    }
    BOOST_TEST(mock.put_count(logs) == 0U);

    // A delay long enough that PUTs which can overlap do, so the high-water
    // mark measures the engine's concurrency rather than scheduling luck.
    mock.reset_counters();
    mock.set_put_delay(std::chrono::milliseconds{100});
    store.barrier_through(last);
    mock.set_put_delay(std::chrono::milliseconds{0});

    const auto puts = mock.put_count(logs);
    const auto high_water = mock.max_concurrent_puts();
    BOOST_TEST(puts == 10U);
    BOOST_TEST(high_water <= 4U);
    // ceil(10 / 4) = 3 rounds at most.
    const auto rounds = (puts + high_water - 1) / std::max<std::size_t>(high_water, 1);
    BOOST_TEST(rounds <= 3U, "PUTs " << puts << ", high-water " << high_water);
    BOOST_TEST(store.durable_through() == last);
    BOOST_TEST(make_store(opt, 1).get_last_log_index() == 10U);
}

BOOST_AUTO_TEST_CASE(write_behind_off_is_synchronous_through_the_wrapper) {
    s3_stateful_mock mock;
    const auto opt = host_options(mock, 1, "http://127.0.0.1:9001");
    auto store = make_store(opt, 1);
    const auto seq = store.append_log_entry_sequenced(entry(1, 1));
    BOOST_TEST(mock.put_count(log_prefix(opt, 1)) == 1U);
    BOOST_TEST(store.durable_through() >= seq);
}

BOOST_AUTO_TEST_SUITE_END()
