// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// dynamodb_gateway_node as a three-node cluster, in one process
// (`.kiro/specs/dynamodb-compatible-api/` task 7): each node is the
// daemon's own run_cluster() on the memory engine, with its Raft listener
// and gateway on free loopback ports, in plaintext. Requests go to every
// node over HTTP, signed as an SDK signs them, so a request that lands on a
// node that does not lead its group crosses the forward link to the one
// that does.

#define BOOST_TEST_MODULE dynamodb_gateway_cluster_test

#include <boost/test/unit_test.hpp>

#include "run_cluster.hpp"
#include "test_ports.hpp"

#include <raft/sigv4_verifier.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <unistd.h>

// The default (Folly) backend needs folly::Init before the first Raft
// timer asks for folly's Timekeeper singleton.
#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("dynamodb_gateway_cluster_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace ddb = kythira::dynamodb;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;
namespace http = boost::beast::http;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
constexpr int k_nodes = 3;

struct answer {
    unsigned status = 0;
    json::object body;

    [[nodiscard]] auto type() const -> std::string {
        const auto* t = body.if_contains("__type");
        if (t == nullptr) {
            return {};
        }
        const std::string full(t->as_string());
        return full.substr(full.find('#') + 1);
    }
    [[nodiscard]] auto message() const -> std::string {
        const auto* m = body.if_contains("message");
        if (m == nullptr) {
            m = body.if_contains("Message");
        }
        return m == nullptr ? std::string{} : std::string(m->as_string());
    }
};

/// Three run_cluster() nodes on threads, stopped when the fixture goes.
struct cluster {
    std::array<std::uint16_t, k_nodes> gateway_ports{};
    std::array<std::uint16_t, k_nodes> raft_ports{};
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    std::vector<std::thread> nodes;
    std::array<std::exception_ptr, k_nodes> failures{};
    /// Three regions (task 27): node `i` is a one-node cluster in
    /// `k_regions[i]`, and the three are voters of one strong host.
    bool regions = false;
    std::array<std::uint16_t, k_nodes> strong_ports{};
    std::array<std::vector<std::string>, k_nodes> logs{};

    static constexpr std::array<std::string_view, k_nodes> k_regions{"us-east-1", "us-east-2",
                                                                     "us-west-2"};

    explicit cluster(bool three_regions = false) : regions(three_regions) {
        for (int i = 0; i < k_nodes; ++i) {
            gateway_ports.at(i) = kythira::testing::free_port();
            raft_ports.at(i) = kythira::testing::free_port();
            strong_ports.at(i) = kythira::testing::free_port();
        }
        for (int i = 0; i < k_nodes; ++i) {
            nodes.emplace_back([this, i] { run(i); });
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    ~cluster() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        for (auto& t : nodes) {
            t.join();
        }
    }

    /// Node `i`'s settings: one of three voters of one cluster, or with
    /// `regions` a one-node cluster in its own region with a strong host.
    auto config_of(int i) -> ddb::gateway_config {
        if (regions) {
            return region_config_of(i);
        }
        std::string peers;
        std::string gateways;
        for (int j = 0; j < k_nodes; ++j) {
            const auto id = std::to_string(j + 1);
            peers +=
                (j == 0 ? "" : ",") + id + "=http://127.0.0.1:" + std::to_string(raft_ports.at(j));
            if (j != i) {
                gateways += (gateways.empty() ? "" : ",") + id +
                            "=127.0.0.1:" + std::to_string(gateway_ports.at(j));
            }
        }
        const std::map<std::string, std::string> vars{
            {"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_PORT", std::to_string(gateway_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STATIC_KEYS",
             "AKIDADMIN:" + std::string(k_secret) + ":111122223333:admin:admin"},
            {"KYTHIRA_DYNAMODB_NODE_ID", std::to_string(i + 1)},
            {"KYTHIRA_DYNAMODB_PEERS", peers},
            {"KYTHIRA_DYNAMODB_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_RAFT_PORT", std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_PEER_GATEWAYS", gateways},
            {"KYTHIRA_DYNAMODB_FORWARD_SECRET", "0123456789abcdef-cluster-test"},
        };
        return parse(vars);
    }

    auto region_config_of(int i) -> ddb::gateway_config {
        const auto id = std::to_string(i + 1);
        std::string strong_peers;
        std::string strong_gateways;
        for (int j = 0; j < k_nodes; ++j) {
            const auto sid = std::to_string(10 * (j + 1) + 1);
            strong_peers += (j == 0 ? "" : ",") + sid + "=" + std::string(k_regions.at(j)) +
                            "@http://127.0.0.1:" + std::to_string(strong_ports.at(j));
            if (j != i) {
                strong_gateways += (strong_gateways.empty() ? "" : ",") + sid +
                                   "=127.0.0.1:" + std::to_string(gateway_ports.at(j));
            }
        }
        const std::map<std::string, std::string> vars{
            {"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_PORT", std::to_string(gateway_ports.at(i))},
            {"KYTHIRA_DYNAMODB_REGION", std::string(k_regions.at(i))},
            {"KYTHIRA_DYNAMODB_STATIC_KEYS",
             "AKIDADMIN:" + std::string(k_secret) + ":111122223333:admin:admin"},
            {"KYTHIRA_DYNAMODB_NODE_ID", id},
            {"KYTHIRA_DYNAMODB_PEERS",
             id + "=http://127.0.0.1:" + std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_RAFT_PORT", std::to_string(raft_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STRONG_NODE_ID", std::to_string(10 * (i + 1) + 1)},
            {"KYTHIRA_DYNAMODB_STRONG_PEERS", strong_peers},
            {"KYTHIRA_DYNAMODB_STRONG_RAFT_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_STRONG_RAFT_PORT", std::to_string(strong_ports.at(i))},
            {"KYTHIRA_DYNAMODB_STRONG_GATEWAYS", strong_gateways},
            {"KYTHIRA_DYNAMODB_STRONG_SECRET", "0123456789abcdef-strong-test"},
            // Loopback is not a wide-area network.
            {"KYTHIRA_DYNAMODB_STRONG_HEARTBEAT_MS", "50"},
            {"KYTHIRA_DYNAMODB_STRONG_ELECTION_MIN_MS", "300"},
            {"KYTHIRA_DYNAMODB_STRONG_ELECTION_MAX_MS", "600"},
        };
        return parse(vars);
    }

    static auto parse(const std::map<std::string, std::string>& vars) -> ddb::gateway_config {
        auto r = ddb::gateway_config_from_env([&vars](std::string_view name) {
            const auto it = vars.find(std::string(name));
            return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
        if (const auto* e = std::get_if<std::string>(&r)) {
            throw std::runtime_error(*e);
        }
        return std::get<ddb::gateway_config>(r);
    }

    /// Waits until node `i` has logged a line holding `text`.
    auto wait_for_log(int i, std::string_view text, std::chrono::seconds limit) -> bool {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, limit, [&] {
            const auto& lines = logs.at(i);
            return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
                return l.find(text) != std::string::npos;
            });
        });
    }

    auto run(int i) -> void {
        using storage = dynamodb_gateway::memory_storage;
        try {
            dynamodb_gateway::run_cluster<storage>(
                config_of(i), std::make_shared<storage::engine_type>(),
                [](std::uint64_t) { return storage::persistence_engine_type{}; },
                [this, i](bool, std::string_view line) {
                    {
                        const std::lock_guard lock(mutex);
                        logs.at(i).emplace_back(line);
                    }
                    cv.notify_all();
                },
                [this] {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [this] { return stopping; });
                });
        } catch (...) {
            failures.at(i) = std::current_exception();
        }
    }

    /// `op` signed now and sent to node `i`'s gateway.
    auto call(int i, std::string_view op, const std::string& body) -> answer {
        const auto port = gateway_ports.at(i);
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto amz_date = sv4::detail::format_amz_date(now);
        const std::string host = "127.0.0.1:" + std::to_string(port);
        std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Type", "application/x-amz-json-1.0"},
            {"Host", host},
            {"X-Amz-Date", amz_date},
            {"X-Amz-Target", "DynamoDB_20120810." + std::string(op)}};
        const sv4::credential_scope scope{amz_date.substr(0, 8), "us-east-1", "dynamodb"};
        headers.emplace_back("Authorization",
                             "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" + scope.text() +
                                 ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                                 ", Signature=" +
                                 std::string(64, '0'));
        sv4::service_rules rules;
        rules.service = "dynamodb";
        rules.protocol = sv4::error_protocol::json;
        const auto parsed = sv4::parse(sv4::request{"POST", "/", "", headers, body}, rules, now);
        BOOST_REQUIRE(parsed.has_value());
        auto& auth = headers.back().second;
        auth.replace(auth.size() - 64, 64,
                     sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope)));

        http::request<http::string_body> req{http::verb::post, "/", 11};
        for (const auto& [n, v] : headers) {
            req.set(n, v);
        }
        req.body() = body;
        req.prepare_payload();
        boost::asio::io_context ioc;
        boost::beast::tcp_stream s(ioc);
        s.expires_after(std::chrono::seconds(30));
        s.connect({boost::asio::ip::make_address("127.0.0.1"), port});
        http::write(s, req);
        boost::beast::flat_buffer buf;
        http::response_parser<http::string_body> parser;
        http::read(s, buf, parser);
        const auto& res = parser.get();
        return answer{static_cast<unsigned>(res.result_int()), json::parse(res.body()).as_object()};
    }

    /// Calls until the answer is not a server error (the cluster electing
    /// its leaders), as an SDK retries; the last answer.
    auto call_until_served(int i, std::string_view op, const std::string& body) -> answer {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (true) {
            std::optional<answer> a;
            try {
                a = call(i, op, body);
            } catch (const std::exception&) {
                // The listener is not up yet.
            }
            if ((a && a->status < 500U) || std::chrono::steady_clock::now() > deadline) {
                return a.value_or(answer{});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
};

}  // namespace

// Task 24: the ids a voter gives a split's children. Voters' ranges are
// disjoint, and a node that keeps its data never hands out an id twice,
// even when it is killed between reserving and using a block.
BOOST_AUTO_TEST_CASE(split_children_get_ids_from_the_voters_own_range) {
    using dynamodb_gateway::reserved_group_ids;
    reserved_group_ids a(0, std::nullopt);
    reserved_group_ids b(1, std::nullopt);
    BOOST_TEST(a.first() > 2U);  // above the bootstrap groups
    BOOST_TEST(a.last() == b.first());
    const auto from_a = a.allocate(3);
    const auto from_b = b.allocate(3);
    BOOST_TEST_REQUIRE(from_a.size() == 3U);
    BOOST_TEST_REQUIRE(from_b.size() == 3U);
    for (const auto id : from_a) {
        BOOST_TEST((id >= a.first() && id < a.last()));
    }
    for (const auto id : from_b) {
        BOOST_TEST((id >= b.first() && id < b.last()));
    }
    BOOST_CHECK_THROW(reserved_group_ids(255, std::nullopt), std::invalid_argument);

    const auto dir = std::filesystem::temp_directory_path() /
                     ("kythira-ddb-group-ids-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::vector<std::uint64_t> seen;
    {
        reserved_group_ids first_run(2, dir / "group-ids");
        for (const auto id : first_run.allocate(reserved_group_ids::k_block + 5)) {
            seen.push_back(id);
        }
    }
    // A restart (or a SIGKILL) resumes after the last reserved block.
    reserved_group_ids second_run(2, dir / "group-ids");
    const auto more = second_run.allocate(4);
    BOOST_TEST_REQUIRE(more.size() == 4U);
    for (const auto id : more) {
        BOOST_TEST(id > seen.back());
        BOOST_TEST((id >= second_run.first() && id < second_run.last()));
    }
    // Another voter's file is refused rather than trusted.
    BOOST_CHECK_THROW(reserved_group_ids(3, dir / "group-ids"), std::runtime_error);
    std::filesystem::remove_all(dir);
}

// Task 24: the layout a RocksDB node restarts with. Binary range bounds,
// open ends, epochs and members come back as saved; no file means none;
// a damaged file is refused rather than read as the bootstrap layout.
BOOST_AUTO_TEST_CASE(a_saved_group_layout_loads_back) {
    using descriptor = kythira::shard_descriptor<std::uint64_t, std::string, std::uint64_t>;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("kythira-ddb-layout-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const dynamodb_gateway::group_layout_file<descriptor, std::uint64_t> file(
        dir / "layout.json", [](const std::uint64_t& n) { return std::to_string(n); },
        [](const std::string& t) { return static_cast<std::uint64_t>(std::stoull(t)); });
    BOOST_TEST(!file.load().has_value());

    descriptor left;
    left._group_id = 2;
    left._range._start = std::string("t/", 2);
    left._range._end = std::string("t/\0\0\0\0\0\0\0\x05/\xff\x01", 13);
    left._epoch = {._version = 3, ._conf_version = 1};
    left._voters = {1, 2, 3};
    descriptor right = left;
    right._group_id = 281474976710656ULL;
    right._range._start = left._range._end;
    right._range._end.reset();
    right._learners = {4};
    BOOST_TEST_REQUIRE(file.save({left, right}));
    const auto loaded = file.load();
    BOOST_TEST_REQUIRE(loaded.has_value());
    BOOST_TEST_REQUIRE(loaded->size() == 2U);
    BOOST_TEST(((*loaded)[0] == left));
    BOOST_TEST(((*loaded)[1] == right));
    BOOST_TEST(((*loaded)[1]._learners == std::vector<std::uint64_t>{4}));

    {
        std::ofstream(dir / "layout.json") << "{\"groups\": [";
    }
    BOOST_CHECK_THROW((void)file.load(), std::runtime_error);
    std::filesystem::remove_all(dir);
}

// Task 27.2: three one-node clusters in three regions run one strong
// host. Its catalog answers on every node once a leader that a quorum
// spanning two regions elected is known, which takes the strong forward
// link from the nodes that do not lead it.
BOOST_AUTO_TEST_CASE(three_regions_form_one_strong_host) {
    cluster c(true);
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST_INFO("node " << i);
        BOOST_TEST(c.wait_for_log(i, "strong host ready", std::chrono::seconds(60)));
    }
    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST(c.wait_for_log(i, "in regions us-east-1, us-east-2, us-west-2",
                                  std::chrono::seconds(1)));
    }
}

BOOST_AUTO_TEST_CASE(requests_on_every_node_reach_one_table) {
    cluster c;
    // The first CreateTable may be retried across an election; a retry of
    // one that did commit meets the table.
    const auto created =
        c.call_until_served(0, "CreateTable",
                            R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST",)"
                            R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
                            R"({"AttributeName":"sk","AttributeType":"N"}],)"
                            R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                            R"({"AttributeName":"sk","KeyType":"RANGE"}]})");
    BOOST_REQUIRE_MESSAGE(created.status == 200U || created.type() == "ResourceInUseException",
                          json::serialize(created.body));

    // Every node comes to see it ACTIVE.
    for (int i = 0; i < k_nodes; ++i) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::string status;
        while (status != "ACTIVE" && std::chrono::steady_clock::now() < deadline) {
            const auto d = c.call_until_served(i, "DescribeTable", R"({"TableName":"orders"})");
            if (d.status == 200U) {
                status = std::string(d.body.at("Table").at("TableStatus").as_string());
            }
            if (status != "ACTIVE") {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        BOOST_REQUIRE_MESSAGE(status == "ACTIVE", "node " << i + 1 << ": " << status);
    }

    // Writes through each node; a consistent read through every other node
    // sees each of them.
    for (int i = 0; i < k_nodes; ++i) {
        const auto put = c.call_until_served(
            i, "PutItem",
            R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":")" + std::to_string(i) +
                R"("},"by":{"N":")" + std::to_string(i + 1) + R"("}}})");
        BOOST_REQUIRE_MESSAGE(put.status == 200U, json::serialize(put.body));
        for (int j = 0; j < k_nodes; ++j) {
            const auto got = c.call_until_served(
                j, "GetItem",
                R"({"TableName":"orders","ConsistentRead":true,"Key":{"pk":{"S":"p"},)"
                R"("sk":{"N":")" +
                    std::to_string(i) + R"("}}})");
            BOOST_REQUIRE_MESSAGE(got.status == 200U, json::serialize(got.body));
            BOOST_TEST(got.body.at("Item").at("by").at("N").as_string() == std::to_string(i + 1),
                       "write via node " << i + 1 << ", read via node " << j + 1);
        }
    }

    // A failed condition crosses the link as the client's error.
    const auto cond =
        c.call_until_served(2, "PutItem",
                            R"({"TableName":"orders","Item":{"pk":{"S":"p"},"sk":{"N":"0"}},)"
                            R"x("ConditionExpression":"attribute_not_exists(pk)"})x");
    BOOST_TEST(cond.type() == "ConditionalCheckFailedException");

    // Query and Scan from each node see all three items, in order.
    for (int i = 0; i < k_nodes; ++i) {
        const auto q = c.call_until_served(
            i, "Query",
            R"({"TableName":"orders","ConsistentRead":true,"KeyConditionExpression":"pk = :p",)"
            R"("ExpressionAttributeValues":{":p":{"S":"p"}}})");
        BOOST_REQUIRE_MESSAGE(q.status == 200U, json::serialize(q.body));
        const auto& items = q.body.at("Items").as_array();
        BOOST_REQUIRE(items.size() == 3U);
        BOOST_TEST(items[2].at("sk").at("N").as_string() == "2");
        const auto s = c.call_until_served(
            i, "Scan", R"({"TableName":"orders","ConsistentRead":true,"Select":"COUNT"})");
        BOOST_TEST(s.body.at("Count").to_number<int>() == 3);
    }

    // DeleteTable through the last node frees the name everywhere.
    const auto del = c.call_until_served(2, "DeleteTable", R"({"TableName":"orders"})");
    BOOST_REQUIRE_MESSAGE(del.status == 200U, json::serialize(del.body));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::string type;
    while (type != "ResourceNotFoundException" && std::chrono::steady_clock::now() < deadline) {
        type = c.call_until_served(0, "DescribeTable", R"({"TableName":"orders"})").type();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    BOOST_TEST(type == "ResourceNotFoundException");

    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST(!c.failures.at(i), "node " << i + 1 << " stopped early");
    }
}

// Time to live on the cluster (task 13): the catalog leader's gateway sweeps,
// and its conditional deletes reach the item group's leader wherever it is.
BOOST_AUTO_TEST_CASE(expired_items_are_swept_on_the_cluster) {
    cluster c;
    const auto created = c.call_until_served(
        1, "CreateTable",
        R"({"TableName":"sessions","BillingMode":"PAY_PER_REQUEST",)"
        R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
        R"("Tags":[{"Key":"kythira:compat:ttl-delay","Value":"prompt"}]})");
    BOOST_REQUIRE_MESSAGE(created.status == 200U || created.type() == "ResourceInUseException",
                          json::serialize(created.body));
    // Every node comes to see it ACTIVE before writing through it: a node
    // whose catalog is behind answers ResourceNotFoundException.
    for (int i = 0; i < k_nodes; ++i) {
        const auto active_by = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        std::string status;
        while (status != "ACTIVE" && std::chrono::steady_clock::now() < active_by) {
            const auto d = c.call_until_served(i, "DescribeTable", R"({"TableName":"sessions"})");
            if (d.status == 200U) {
                status = std::string(d.body.at("Table").at("TableStatus").as_string());
            }
            if (status != "ACTIVE") {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        BOOST_REQUIRE_MESSAGE(status == "ACTIVE", "node " << i + 1 << ": " << status);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    answer ttl;
    while (ttl.status != 200U && std::chrono::steady_clock::now() < deadline) {
        ttl = c.call_until_served(
            1, "UpdateTimeToLive",
            R"({"TableName":"sessions","TimeToLiveSpecification":{"Enabled":true,)"
            R"("AttributeName":"exp"}})");
        if (ttl.status != 200U) {
            // CREATING, or a retry of a change that did commit.
            if (ttl.message() == "TimeToLive is already enabled") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    const auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    for (int i = 0; i < k_nodes; ++i) {
        const auto put = c.call_until_served(i, "PutItem",
                                             R"({"TableName":"sessions","Item":{"pk":{"S":"s)" +
                                                 std::to_string(i) + R"("},"exp":{"N":")" +
                                                 std::to_string(now_s - 60) + R"("}}})");
        BOOST_REQUIRE_MESSAGE(put.status == 200U, json::serialize(put.body));
    }
    const auto keep =
        c.call_until_served(0, "PutItem",
                            R"({"TableName":"sessions","Item":{"pk":{"S":"keep"},"exp":{"N":")" +
                                std::to_string(now_s + 3600) + R"("}}})");
    BOOST_REQUIRE_MESSAGE(keep.status == 200U, json::serialize(keep.body));

    int count = -1;
    const auto swept = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (count != 1 && std::chrono::steady_clock::now() < swept) {
        const auto s = c.call_until_served(
            2, "Scan", R"({"TableName":"sessions","ConsistentRead":true,"Select":"COUNT"})");
        if (s.status == 200U) {
            count = s.body.at("Count").to_number<int>();
        }
        if (count != 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    BOOST_TEST(count == 1);
    const auto got = c.call_until_served(
        2, "GetItem",
        R"({"TableName":"sessions","ConsistentRead":true,"Key":{"pk":{"S":"keep"}}})");
    BOOST_TEST(got.body.contains("Item"));

    for (int i = 0; i < k_nodes; ++i) {
        BOOST_TEST(!c.failures.at(i), "node " << i + 1 << " stopped early");
    }
}
