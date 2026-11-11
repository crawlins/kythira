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

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

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

    cluster() {
        for (int i = 0; i < k_nodes; ++i) {
            gateway_ports.at(i) = kythira::testing::free_port();
            raft_ports.at(i) = kythira::testing::free_port();
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

    auto config_of(int i) -> ddb::gateway_config {
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
        auto r = ddb::gateway_config_from_env([&vars](std::string_view name) {
            const auto it = vars.find(std::string(name));
            return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
        });
        if (const auto* e = std::get_if<std::string>(&r)) {
            throw std::runtime_error(*e);
        }
        return std::get<ddb::gateway_config>(r);
    }

    auto run(int i) -> void {
        using storage = dynamodb_gateway::memory_storage;
        try {
            dynamodb_gateway::run_cluster<storage>(
                config_of(i), std::make_shared<storage::engine_type>(),
                [](std::uint64_t) { return storage::persistence_engine_type{}; },
                [](bool, std::string_view) {},
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
