// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// What dynamodb_gateway_node runs (`.kiro/specs/dynamodb-compatible-api/`
// task 7): configuration from the environment, and the gateway serving
// signed requests over a real socket. The server listens in plaintext on a
// free port; the TLS path is the core HTTPS server's, which
// beast_https_server_test covers.

#define BOOST_TEST_MODULE dynamodb_gateway_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/gateway.hpp>

#include <raft/memory_kv_engine.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace ddb = kythira::dynamodb;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;
namespace http = boost::beast::http;

namespace {

constexpr std::string_view k_secret = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";

auto env_of(std::map<std::string, std::string> vars) -> ddb::env_lookup {
    return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
        const auto it = vars.find(std::string(name));
        return it == vars.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

auto minimal_env() -> std::map<std::string, std::string> {
    return {{"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_STATIC_KEYS",
             "AKIDADMIN:" + std::string(k_secret) + ":111122223333:admin:admin"}};
}

auto config_error(std::map<std::string, std::string> vars) -> std::string {
    auto r = ddb::gateway_config_from_env(env_of(std::move(vars)));
    const auto* e = std::get_if<std::string>(&r);
    return e == nullptr ? std::string("ok") : *e;
}

}  // namespace

BOOST_AUTO_TEST_CASE(defaults_from_a_minimal_environment) {
    auto r = ddb::gateway_config_from_env(env_of(minimal_env()));
    BOOST_REQUIRE(std::holds_alternative<ddb::gateway_config>(r));
    const auto& c = std::get<ddb::gateway_config>(r);
    BOOST_TEST(c.http.bind_address == "0.0.0.0");
    BOOST_TEST(c.http.port == 8000U);
    BOOST_TEST(c.http.insecure_plaintext);
    BOOST_TEST(c.front.region == "us-east-1");
    BOOST_TEST(c.front.partition == "aws");
    BOOST_TEST(c.data_dir.empty());
    BOOST_TEST(c.engine == "memory");
    BOOST_REQUIRE(c.keys.size() == 1U);
    BOOST_TEST(c.keys[0].access_key_id == "AKIDADMIN");
    BOOST_TEST(c.keys[0].account == "111122223333");
    BOOST_TEST((c.keys[0].role == kythira::aws_auth::static_role::admin));
}

BOOST_AUTO_TEST_CASE(every_setting_is_read) {
    const auto dir = std::filesystem::temp_directory_path() / "dynamodb_gateway_test_keys";
    std::filesystem::create_directories(dir);
    const auto file = dir / "keys";
    {
        std::ofstream out(file);
        out << "# operators\n\nAKIDRO:s3cret:111122223333:reader:read-only\n"
               "AKIDRW:s3cret2:111122223333:writer:read-write\r\n";
    }
    auto vars = minimal_env();
    vars.erase("KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT");
    vars["KYTHIRA_DYNAMODB_TLS_CERT"] = "/etc/kythira/tls.crt";
    vars["KYTHIRA_DYNAMODB_TLS_KEY"] = "/etc/kythira/tls.key";
    vars["KYTHIRA_DYNAMODB_BIND"] = "*";
    vars["KYTHIRA_DYNAMODB_PORT"] = "0";
    vars["KYTHIRA_DYNAMODB_REGION"] = "eu-west-1";
    vars["KYTHIRA_DYNAMODB_PARTITION"] = "aws-cn";
    vars["KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT"] = "ddb.example:8443";
    vars["KYTHIRA_DYNAMODB_STATIC_KEYS_FILE"] = file.string();
    vars["KYTHIRA_DYNAMODB_DATA_DIR"] = "/var/lib/kythira";
    vars["KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES"] = "4096";
    auto r = ddb::gateway_config_from_env(env_of(vars));
    std::filesystem::remove_all(dir);
    BOOST_REQUIRE(std::holds_alternative<ddb::gateway_config>(r));
    const auto& c = std::get<ddb::gateway_config>(r);
    BOOST_TEST(!c.http.insecure_plaintext);
    BOOST_TEST(c.http.tls_cert_path == "/etc/kythira/tls.crt");
    BOOST_TEST(c.http.bind_address == "*");
    BOOST_TEST(c.http.port == 0U);
    BOOST_TEST(c.front.region == "eu-west-1");
    BOOST_TEST(c.front.partition == "aws-cn");
    BOOST_TEST(c.front.advertised_endpoint == "ddb.example:8443");
    BOOST_TEST(c.data_dir == "/var/lib/kythira");
    BOOST_TEST(c.disk_floor_bytes == 4096U);
    // The file's keys first, then the variable's.
    BOOST_REQUIRE(c.keys.size() == 3U);
    BOOST_TEST(c.keys[0].name == "reader");
    BOOST_TEST((c.keys[0].role == kythira::aws_auth::static_role::read_only));
    BOOST_TEST(c.keys[1].secret == "s3cret2");
    BOOST_TEST((c.keys[1].role == kythira::aws_auth::static_role::read_write));
    BOOST_TEST(c.keys[2].access_key_id == "AKIDADMIN");
}

BOOST_AUTO_TEST_CASE(bad_settings_are_refused_with_the_variable_named) {
    auto vars = minimal_env();
    vars.erase("KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT");
    BOOST_TEST(config_error(vars).find("KYTHIRA_DYNAMODB_TLS_CERT and KYTHIRA_DYNAMODB_TLS_KEY "
                                       "are required") == 0U);

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT"] = "yes";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT: expected 0 or 1, got 'yes'");

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_PORT"] = "70000";
    BOOST_TEST(config_error(vars) == "KYTHIRA_DYNAMODB_PORT: not a port: '70000'");

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES"] = "1G";
    BOOST_TEST(config_error(vars) == "KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES: not a byte count: '1G'");

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_ENGINE"] = "rocksdb";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_ENGINE: unknown engine 'rocksdb' (only 'memory' so far)");

    vars = minimal_env();
    vars.erase("KYTHIRA_DYNAMODB_STATIC_KEYS");
    BOOST_TEST(config_error(vars) ==
               "no access keys: set KYTHIRA_DYNAMODB_STATIC_KEYS_FILE or "
               "KYTHIRA_DYNAMODB_STATIC_KEYS");

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_STATIC_KEYS_FILE"] = "/nonexistent/kythira/keys";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE: cannot read '/nonexistent/kythira/keys'");
}

BOOST_AUTO_TEST_CASE(bad_keys_are_named_by_position_without_the_secret) {
    auto vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_STATIC_KEYS"] = "AKID1:topsecret:111122223333:a:admin,AKID2:topsecret";
    const auto e = config_error(vars);
    BOOST_TEST(e ==
               "KYTHIRA_DYNAMODB_STATIC_KEYS: static key 2: a key needs five ':'-separated "
               "fields (access-key-id:secret:account:name:role)");
    BOOST_TEST(e.find("topsecret") == std::string::npos);

    vars["KYTHIRA_DYNAMODB_STATIC_KEYS"] = "AKID1:topsecret:111122223333:a:root";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_STATIC_KEYS: static key 1: unknown role 'root' (read-only, "
               "read-write or admin)");

    vars["KYTHIRA_DYNAMODB_STATIC_KEYS"] = "AKID1::111122223333:a:admin";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_STATIC_KEYS: static key 1: a key's id, secret, account and "
               "name must not be empty");
}

namespace {

/// The gateway on local_backend, listening in plaintext on a free port.
struct served {
    kythira::memory_kv_engine engine;
    ddb::local_backend<> backend{engine};
    std::optional<ddb::gateway<ddb::local_backend<>>> gw;

    served() {
        auto vars = minimal_env();
        vars["KYTHIRA_DYNAMODB_BIND"] = "127.0.0.1";
        vars["KYTHIRA_DYNAMODB_PORT"] = "0";
        vars["KYTHIRA_DYNAMODB_STATIC_KEYS"] +=
            ",AKIDREAD:" + std::string(k_secret) + ":111122223333:reader:read-only";
        auto c = ddb::gateway_config_from_env(env_of(vars));
        BOOST_REQUIRE(std::holds_alternative<ddb::gateway_config>(c));
        gw.emplace(std::get<ddb::gateway_config>(c), backend);
        gw->start();
    }

    auto send(http::request<http::string_body> req) -> http::response<http::string_body> {
        boost::asio::io_context ioc;
        boost::beast::tcp_stream s(ioc);
        s.expires_after(std::chrono::seconds(10));
        s.connect({boost::asio::ip::make_address("127.0.0.1"), gw->port()});
        req.set(http::field::host, "127.0.0.1:" + std::to_string(gw->port()));
        req.prepare_payload();
        http::write(s, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(s, buf, res);
        return res;
    }

    /// A DynamoDB call signed now with `key`'s secret.
    auto call(std::string_view op, const std::string& body, std::string_view key = "AKIDADMIN")
        -> http::response<http::string_body> {
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto amz_date = sv4::detail::format_amz_date(now);
        const std::string date = amz_date.substr(0, 8);
        const std::string host = "127.0.0.1:" + std::to_string(gw->port());
        std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Type", "application/x-amz-json-1.0"},
            {"Host", host},
            {"X-Amz-Date", amz_date},
            {"X-Amz-Target", "DynamoDB_20120810." + std::string(op)}};
        const sv4::credential_scope scope{date, "us-east-1", "dynamodb"};
        headers.emplace_back("Authorization",
                             "AWS4-HMAC-SHA256 Credential=" + std::string(key) + "/" +
                                 scope.text() +
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
        return send(std::move(req));
    }
};

auto body_of(const http::response<http::string_body>& r) -> json::object {
    return json::parse(r.body()).as_object();
}

auto type_of(const http::response<http::string_body>& r) -> std::string {
    const std::string t(body_of(r).at("__type").as_string());
    return t.substr(t.find('#') + 1);
}

}  // namespace

BOOST_AUTO_TEST_CASE(get_slash_is_the_health_check) {
    served s;
    http::request<http::string_body> req{http::verb::get, "/", 11};
    const auto res = s.send(std::move(req));
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res.body() == "healthy: dynamodb.us-east-1.amazonaws.com ");
}

BOOST_AUTO_TEST_CASE(signed_requests_round_trip_over_the_socket) {
    served s;
    auto r = s.call("CreateTable",
                    R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST",)"
                    R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}]})");
    BOOST_REQUIRE_MESSAGE(r.result_int() == 200U, r.body());
    BOOST_TEST(r[http::field::content_type] == "application/x-amz-json-1.0");
    BOOST_TEST(!r["x-amzn-RequestId"].empty());
    BOOST_TEST(body_of(r).at("TableDescription").at("TableStatus").as_string() == "CREATING");

    r = s.call("DescribeTable", R"({"TableName":"orders"})");
    BOOST_TEST(body_of(r).at("Table").at("TableStatus").as_string() == "ACTIVE");

    r = s.call("PutItem", R"({"TableName":"orders","Item":{"pk":{"S":"a"},"n":{"N":"7"}}})");
    BOOST_TEST(r.result_int() == 200U);
    r = s.call("GetItem", R"({"TableName":"orders","Key":{"pk":{"S":"a"}}})", "AKIDREAD");
    BOOST_REQUIRE_MESSAGE(r.result_int() == 200U, r.body());
    BOOST_TEST(json::serialize(body_of(r).at("Item")) == R"({"n":{"N":"7"},"pk":{"S":"a"}})");

    // DescribeEndpoints answers the port the server picked.
    r = s.call("DescribeEndpoints", "{}");
    BOOST_TEST(body_of(r).at("Endpoints").at(0).at("Address").as_string() ==
               "localhost:" + std::to_string(s.gw->port()));
}

BOOST_AUTO_TEST_CASE(roles_and_signatures_are_enforced) {
    served s;
    // The read-only key may not write.
    auto r = s.call("CreateTable",
                    R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST",)"
                    R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                    R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}]})",
                    "AKIDREAD");
    BOOST_TEST(r.result_int() == 400U);
    BOOST_TEST(type_of(r) == "AccessDeniedException");

    // An unsigned request learns nothing. (The core verifier's error type
    // for this case is shared with IAM and Kinesis; the message is AWS's.)
    http::request<http::string_body> req{http::verb::post, "/", 11};
    req.set(http::field::content_type, "application/x-amz-json-1.0");
    req.set("X-Amz-Target", "DynamoDB_20120810.ListTables");
    req.body() = "{}";
    const auto res = s.send(std::move(req));
    BOOST_TEST(res.result_int() == 400U);
    BOOST_TEST(std::string(body_of(res).at("message").as_string()) ==
               "Request is missing Authentication Token");
}
