// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The handler-test fixture (`.kiro/specs/dynamodb-compatible-api/` tasks 7
// and 8): every handler registered with the front end over local_backend on
// memory_kv_engine, a fixed clock, an injectable disk guard, and requests
// signed with SigV4 as an SDK signs them. Include it once per test binary,
// after <boost/test/unit_test.hpp>.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/compat_profile.hpp>
#include <kythira/dynamodb/batch_api.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_api.hpp>
#include <kythira/dynamodb/partiql.hpp>
#include <kythira/dynamodb/query_api.hpp>
#include <kythira/dynamodb/table_api.hpp>
#include <kythira/dynamodb/ttl.hpp>
#include <kythira/dynamodb/txn_api.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/static_key_authorizer.hpp>

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dynamodb_test {

namespace ddb = kythira::dynamodb;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

inline constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
inline constexpr std::string_view k_account = "111122223333";
inline const auto k_now = sv4::detail::parse_amz_date("20261110T120000Z").value();

inline auto signed_request(std::string_view op, const std::string& body) -> ddb::front_end_request {
    ddb::front_end_request r;
    r.method = "POST";
    r.path = "/";
    r.body = body;
    r.source_ip = "192.0.2.10";
    r.headers = {{"Host", "localhost:8000"},
                 {"Content-Type", "application/x-amz-json-1.0"},
                 {"X-Amz-Date", "20261110T120000Z"},
                 {"X-Amz-Target", "DynamoDB_20120810." + std::string(op)}};
    sv4::service_rules rules;
    rules.service = "dynamodb";
    rules.protocol = sv4::error_protocol::json;
    const sv4::credential_scope scope{"20261110", "us-east-1", "dynamodb"};
    r.headers.emplace_back("Authorization",
                           "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" + scope.text() +
                               ", SignedHeaders=content-type;host;x-amz-date;x-amz-target"
                               ", Signature=" +
                               std::string(64, '0'));
    const auto parsed =
        sv4::parse(sv4::request{r.method, r.path, r.query, r.headers, r.body}, rules, k_now);
    BOOST_REQUIRE(parsed.has_value());
    const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
    r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
    return r;
}

struct reply {
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
        for (const char* k : {"message", "Message"}) {
            if (const auto* m = body.if_contains(k)) {
                return std::string(m->as_string());
            }
        }
        return {};
    }
};

struct fixture {
    kythira::memory_kv_engine engine;
    std::uint64_t clock_ms = 1794300000123ULL;
    std::optional<std::uintmax_t> free_bytes = std::uintmax_t{1} << 40U;
    ddb::local_backend<> backend{engine, [this] { return clock_ms; }};
    ddb::disk_guard guard{"/data", 1024,
                          [this](const std::filesystem::path&) { return free_bytes; }};
    az::static_key_authorizer auth{
        std::vector<az::static_key>{{"AKIDADMIN", std::string(k_secret), std::string(k_account),
                                     "admin", az::static_role::admin}},
        "aws", [] { return std::chrono::system_clock::time_point(k_now); }};
    std::unique_ptr<ddb::front_end<az::static_key_authorizer>> fe;
    ddb::table_api<ddb::local_backend<>> tables{backend};
    ddb::item_api<ddb::local_backend<>> items{backend, &guard};
    ddb::query_api<ddb::local_backend<>> queries{backend};
    ddb::batch_api<ddb::local_backend<>> batches{backend, &guard};
    ddb::txn_api<ddb::local_backend<>> transactions{backend, &guard};
    ddb::ttl_api<ddb::local_backend<>> ttl{backend};
    ddb::partiql_api<ddb::local_backend<>> partiql{backend, items, queries, transactions};
    /// Not run on its own: tests call `sweeper.pass()`.
    ddb::ttl_sweeper<ddb::local_backend<>> sweeper{backend};

    fixture() {
        auth.register_service(ddb::dynamodb_static_roles());
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        fe = std::make_unique<ddb::front_end<az::static_key_authorizer>>(cfg, auth);
        tables.register_with(*fe);
        items.register_with(*fe);
        queries.register_with(*fe);
        batches.register_with(*fe);
        transactions.register_with(*fe);
        ttl.register_with(*fe);
        partiql.register_with(*fe);
    }

    /// Turns capacity enforcement off for the whole fixture (Requirement
    /// 3.5's cluster switch), for tests about something else that move more
    /// data at one instant of the fixed clock than any bucket admits.
    void without_throttling() {
        const auto off = ddb::cluster_profile({{"throttling", "off"}}).value();
        items.set_compat_profile(off);
        queries.set_compat_profile(off);
        batches.set_compat_profile(off);
        transactions.set_compat_profile(off);
    }

    /// When set, requests go here instead of the front end: a test can run
    /// its cases against another implementation (DynamoDB Local) to check
    /// its own expectations.
    std::function<reply(const ddb::front_end_request&)> remote;

    auto call(std::string_view op, const std::string& body) -> reply {
        if (remote) {
            return remote(signed_request(op, body));
        }
        const auto r = fe->handle(signed_request(op, body));
        return reply{r.status, json::parse(r.body).as_object()};
    }

    auto ok(std::string_view op, const std::string& body) -> json::object {
        auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U,
                              std::string(op) + " failed: " + r.type() + ": " + r.message());
        return r.body;
    }

    auto create(const std::string& name, const std::string& extra = "") -> json::object {
        return ok("CreateTable",
                  R"({"TableName":")" + name +
                      R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
                      R"({"AttributeName":"sk","AttributeType":"N"}],)"
                      R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
                      R"({"AttributeName":"sk","KeyType":"RANGE"}],)"
                      R"("ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5})" +
                      extra + "}");
    }
};

}  // namespace dynamodb_test
