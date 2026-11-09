// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The ledger node's core over multi_raft (.kiro/specs/verifiable-ledger/
// task 7c): four hosts on the in-process message fabric, each with its
// own engine. The catalog runs on all four; ledgers are placed on three
// and every replica creates its group from its catalog replica. Requests
// go through ledger_api on any member: a non-leader answers not_leader
// with the leader's URL, and the client follows it. Index builds and seals
// run in the background on the leader, the journals of every replica
// match, and a leader change keeps the ledger writable.

#define BOOST_TEST_MODULE ledger_host_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <kythira/ledger/ion_value.hpp>
#include <kythira/ledger/ledger_api.hpp>
#include <kythira/ledger/ledger_group_state_machine.hpp>
#include <kythira/ledger/ledger_host.hpp>

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/memory_kv_engine.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("ledger_host_test"), nullptr};
        char** argv = argv_data;
        init_ = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> init_;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using namespace kythira::ledger;
using kythira::memory_kv_engine;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using group_sm = basic_ledger_group_state_machine<memory_kv_engine>;

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = group_sm;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using raft_host = kythira::multi_raft<host_types, std::string, std::uint64_t>;
using raft_config = kythira::multi_raft_config<host_types, std::string, std::uint64_t>;
using ledger_host = basic_ledger_host<raft_host, memory_kv_engine>;
using api_t = ledger_api<ledger_host, group_sm::ledger_type>;

constexpr std::uint64_t k_members = 4;

class temp_dir {
public:
    temp_dir() {
        static std::atomic<unsigned> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("kythira-ledger-host-" + std::to_string(rd()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(path_);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
    temp_dir(temp_dir&&) = delete;
    auto operator=(temp_dir&&) -> temp_dir& = delete;
    [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

private:
    std::filesystem::path path_;
};

auto text(const ion_value& v, std::string_view field) -> std::string {
    const ion_value* f = v.find(field);
    return f != nullptr ? f->as_text().value_or("") : "";
}

auto url_of(std::uint64_t id) -> std::string {
    return "https://ledger-" + std::to_string(id) + ":8443";
}

/// One member: its engine, raft host, ledger host and API.
struct member {
    std::uint64_t id;
    std::shared_ptr<group_sm::context> ctx;
    std::unique_ptr<raft_host> raft;
    std::unique_ptr<ledger_host> host;
    std::unique_ptr<api_t> api;
};

class cluster {
public:
    explicit cluster(std::uint64_t segment_bytes = std::uint64_t{64} << 20U) {
        std::vector<std::uint64_t> ids;
        std::map<std::uint64_t, std::string> urls;
        for (std::uint64_t id = 1; id <= k_members; ++id) {
            ids.push_back(id);
            urls[id] = url_of(id);
        }
        for (std::uint64_t id = 1; id <= k_members; ++id) {
            auto m = std::make_unique<member>();
            m->id = id;
            const auto seg_root = dir_.path() / ("node-" + std::to_string(id));
            m->ctx = ledger_host::make_context([seg_root, segment_bytes](const std::string& lid) {
                ledger_options o;
                o.segment_directory = seg_root / lid;
                o.segment_bytes = segment_bytes;
                return o;
            });
            m->raft = std::make_unique<raft_host>(make_config(id, m->ctx));
            ledger_host::options opts;
            opts.members = ids;
            opts.api_urls = urls;
            opts.maintenance_interval = std::chrono::milliseconds{50};
            opts.index_build_step = 2;
            m->host = std::make_unique<ledger_host>(*m->raft, m->ctx, opts);
            m->api = std::make_unique<api_t>(
                *m->host, ledger_roles{}.grant("CN=admin", "*", ledger_role::admin));
            members_.push_back(std::move(m));
        }
        for (auto& m : members_) {
            m->host->bootstrap();
            m->raft->start();
        }
        running_ = true;
        for (auto& m : members_) {
            drivers_.emplace_back([this, raft = m->raft.get()] {
                while (running_.load()) {
                    raft->tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            });
        }
        for (auto& m : members_) {
            m->host->start();
        }
    }

    ~cluster() {
        for (auto& m : members_) {
            m->host->stop();
        }
        running_ = false;
        for (auto& t : drivers_) {
            t.join();
        }
        for (auto& m : members_) {
            m->raft->stop();
        }
        fabric_.shutdown();
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    [[nodiscard]] auto at(std::uint64_t id) -> member& { return *members_.at(id - 1); }
    [[nodiscard]] auto fabric() -> message_fabric& { return fabric_; }

    /// Send a request to member `id`, following not_leader redirects (and
    /// retrying while there is no leader) as the client does.
    auto call(std::uint64_t id, std::string method, std::string path, std::string body = "",
              std::map<std::string, std::string> query = {},
              std::chrono::milliseconds budget = std::chrono::seconds{15}) -> api_response {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        api_response r;
        for (;;) {
            api_request req;
            req.method = method;
            req.path = path;
            req.body = body;
            req.principal = "CN=admin";
            req.query = query;
            r = at(id).api->handle(req);
            if (r.status != 503 || std::chrono::steady_clock::now() > deadline) {
                return r;
            }
            const auto v = ion_value::parse(std::string_view(r.body));
            if (text(v, "code") != "not_leader") {
                return r;  // commit_unconfirmed: resending could apply twice
            }
            if (const ion_value* l = v.find("leader"); l != nullptr) {
                const auto u = l->as_text().value_or("");
                for (std::uint64_t m = 1; m <= k_members; ++m) {
                    if (u == url_of(m)) {
                        id = m;
                    }
                }
            } else {
                id = id % k_members + 1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    auto await(const std::function<bool()>& predicate,
               std::chrono::milliseconds budget = std::chrono::seconds{15}) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return predicate();
    }

    /// A member leading `group`, other than `except` (a member cut off
    /// from the rest still thinks it leads).
    [[nodiscard]] auto leader_of(std::uint64_t group, std::uint64_t except = 0)
        -> std::optional<std::uint64_t> {
        for (auto& m : members_) {
            if (m->id == except) {
                continue;
            }
            auto* n = m->raft->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return m->id;
            }
        }
        return std::nullopt;
    }

private:
    auto make_config(std::uint64_t id, std::shared_ptr<group_sm::context> ctx) -> raft_config {
        raft_config cfg{
            .node_id = id,
            .network_client = fabric_client{fabric_, id},
            .network_server = fabric_server{fabric_, id},
            .store_factory =
                [](const std::uint64_t&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = kythira::hibernation_mode::off;
        cfg.logger = kythira::console_logger(kythira::log_level::error);
        cfg.logger_factory = [](const std::uint64_t&) {
            return kythira::console_logger(kythira::log_level::error);
        };
        cfg.state_engine = std::make_shared<memory_kv_engine>();
        cfg.state_machine_factory = [ctx](const std::uint64_t&) { return group_sm(ctx); };
        cfg.automatic_split_merge_enabled = false;
        cfg.executor_stripes = 2;
        return cfg;
    }

    temp_dir dir_;
    message_fabric fabric_{8};
    std::vector<std::unique_ptr<member>> members_;
    std::vector<std::thread> drivers_;
    std::atomic<bool> running_{false};
};

auto parse(const api_response& r) -> ion_value {
    return ion_value::parse(std::string_view(r.body));
}

}  // namespace

BOOST_AUTO_TEST_CASE(ledgers_are_placed_on_three_members_and_served_from_any) {
    cluster c;
    BOOST_REQUIRE(c.await([&] { return c.leader_of(catalog_group).has_value(); }));

    auto r = c.call(1, "POST", "/v1/ledgers", R"({"name": "bank", "deletionProtection": false})");
    BOOST_REQUIRE_MESSAGE(r.status == 201, r.body);
    const auto rec = parse(r);
    const auto group = static_cast<std::uint64_t>(*rec.find("group")->as_int64());
    BOOST_TEST(group == first_ledger_group);
    std::vector<std::uint64_t> voters;
    for (const auto& v : rec.find("voters")->items()) {
        voters.push_back(std::stoull(v.as_text().value_or("0")));
    }
    BOOST_TEST(voters.size() == 3U);

    // Every voter, and only the voters, create the group.
    BOOST_REQUIRE(c.await([&] {
        for (std::uint64_t id = 1; id <= k_members; ++id) {
            const bool voter = std::ranges::find(voters, id) != voters.end();
            if (c.at(id).raft->has_group(group) != voter) {
                return false;
            }
        }
        return c.leader_of(group).has_value();
    }));

    // Write through every member, including the one that is not a replica.
    r = c.call(1, "POST", "/v1/ledgers/bank/execute",
               R"({"statements": [{"op": "create_table", "table": "acct"}]})");
    BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);
    for (std::uint64_t id = 1; id <= k_members; ++id) {
        const auto body = std::string(R"({"statements": [{"op": "insert", "table": "acct", )") +
                          R"("id": ")" + new_id() + R"(", "value": {"n": )" + std::to_string(id) +
                          "}}]}";
        r = c.call(id, "POST", "/v1/ledgers/bank/execute", body);
        BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);
    }

    // Every replica holds the same journal.
    const auto ledger_id = text(rec, "ledgerId");
    std::optional<std::string> digest;
    BOOST_REQUIRE(c.await([&] {
        std::optional<std::string> seen;
        for (const auto id : voters) {
            auto sm = c.at(id).host->ledger(ledger_id);
            if (!sm || sm->block_count() != 5U) {
                return false;
            }
            const auto d = to_hex(sm->get_digest()->digest);
            if (seen && *seen != d) {
                return false;
            }
            seen = d;
        }
        digest = seen;
        return true;
    }));
    BOOST_TEST(c.at(1).host->stats().groups_created + c.at(2).host->stats().groups_created +
                   c.at(3).host->stats().groups_created + c.at(4).host->stats().groups_created ==
               3U);
}

BOOST_AUTO_TEST_CASE(indexes_build_and_segments_seal_in_the_background) {
    cluster c(/*segment_bytes=*/2048);
    BOOST_REQUIRE(c.await([&] { return c.leader_of(catalog_group).has_value(); }));
    auto r = c.call(2, "POST", "/v1/ledgers", R"({"name": "books", "deletionProtection": false})");
    BOOST_REQUIRE_MESSAGE(r.status == 201, r.body);
    const auto rec = parse(r);
    const auto ledger_id = text(rec, "ledgerId");
    r = c.call(2, "POST", "/v1/ledgers/books/execute",
               R"({"statements": [{"op": "create_table", "table": "acct"}]})");
    BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);
    for (int i = 0; i < 12; ++i) {
        const auto body = std::string(R"({"statements": [{"op": "insert", "table": "acct", )") +
                          R"("id": ")" + new_id() + R"(", "value": {"owner": "o)" +
                          std::to_string(i % 3) + R"(", "pad": ")" + std::string(200, 'x') +
                          "\"}}]}";
        r = c.call(2, "POST", "/v1/ledgers/books/execute", body);
        BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);
    }
    // An index on a non-empty table builds in steps of two documents.
    r = c.call(2, "POST", "/v1/ledgers/books/execute",
               R"({"statements": [{"op": "create_index", "table": "acct", "index": "by_owner",
                                   "path": ["owner"]}]})");
    BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);

    BOOST_REQUIRE(c.await([&] {
        r = c.call(2, "GET", "/v1/ledgers/books/tables/acct/indexes");
        return r.status == 200 && r.body.find("true") != std::string::npos;
    }));
    std::uint64_t steps = 0;
    for (std::uint64_t id = 1; id <= k_members; ++id) {
        steps += c.at(id).host->stats().index_steps;
    }
    BOOST_TEST(steps >= 6U);  // 12 documents, two a step

    // The journal passed 2 KiB long ago: every replica seals the same
    // segment files.
    const auto voters = parse(c.call(2, "GET", "/v1/ledgers/books")).find("voters")->items();
    BOOST_REQUIRE(c.await([&] {
        std::optional<std::vector<segment_info>> first;
        for (const auto& v : voters) {
            auto sm = c.at(std::stoull(v.as_text().value_or("0"))).host->ledger(ledger_id);
            if (!sm || sm->segments().empty()) {
                return false;
            }
            const auto segs = sm->segments();
            if (first &&
                (first->size() != segs.size() || first->front().sha256 != segs.front().sha256)) {
                return false;
            }
            first = segs;
        }
        return true;
    }));
}

BOOST_AUTO_TEST_CASE(a_new_leader_keeps_the_ledger_writable_and_splits_stay_vetoed) {
    cluster c;
    BOOST_REQUIRE(c.await([&] { return c.leader_of(catalog_group).has_value(); }));
    auto r = c.call(3, "POST", "/v1/ledgers", R"({"name": "audit", "deletionProtection": false})");
    BOOST_REQUIRE_MESSAGE(r.status == 201, r.body);
    const auto group = static_cast<std::uint64_t>(*parse(r).find("group")->as_int64());
    r = c.call(3, "POST", "/v1/ledgers/audit/execute",
               R"({"statements": [{"op": "create_table", "table": "t"}]})");
    BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);

    BOOST_REQUIRE(c.await([&] { return c.leader_of(group).has_value(); }));
    const auto old_leader = *c.leader_of(group);
    // Cut the leader off both ways (kill() drops only what it receives).
    c.fabric().kill(old_leader);
    for (std::uint64_t id = 1; id <= k_members; ++id) {
        c.fabric().partition(old_leader, id);
    }
    BOOST_REQUIRE(c.await([&] { return c.leader_of(group, old_leader).has_value(); }));
    const auto new_leader = *c.leader_of(group, old_leader);
    const auto doc_id = new_id();
    const auto body = std::string(R"({"statements": [{"op": "insert", "table": "t", "id": ")") +
                      doc_id + R"(", "value": {"a": 1}}]})";
    // What a careful client does: on commit_unconfirmed (leadership moved
    // while the insert was in flight) it checks whether the insert landed
    // before sending it again, since the entry may or may not commit.
    bool written = false;
    int unconfirmed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{40};
    while (!written && std::chrono::steady_clock::now() < deadline) {
        r = c.call(new_leader, "POST", "/v1/ledgers/audit/execute", body, {},
                   std::chrono::seconds{20});
        if (r.status == 200 || text(parse(r), "code") == "document_exists") {
            written = true;  // landed now, or an earlier unconfirmed attempt did
            break;
        }
        BOOST_REQUIRE_MESSAGE(r.status == 503, r.body);
        BOOST_TEST(text(parse(r), "code") == "commit_unconfirmed");
        BOOST_TEST(parse(r).find("retryable")->as_bool().value_or(true) == false);
        ++unconfirmed;
        // Let the group settle, then look before resending.
        written = c.await(
            [&] {
                return c.call(new_leader, "GET", "/v1/ledgers/audit/tables/t/documents/" + doc_id)
                           .status == 200;
            },
            std::chrono::seconds{3});
    }
    BOOST_REQUIRE_MESSAGE(
        written, "insert never confirmed after " << unconfirmed << " unconfirmed attempts");
    c.fabric().revive(old_leader);
    c.fabric().heal_all();

    // A split of a ledger group is refused before it is proposed.
    auto* host = &*c.at(new_leader).raft;
    BOOST_CHECK_THROW(
        {
            auto f = host->split_shard(group, {ledger_range("x").start},
                                       std::chrono::milliseconds{2000});
            std::ignore = std::move(f).get();
        },
        std::exception);
}
