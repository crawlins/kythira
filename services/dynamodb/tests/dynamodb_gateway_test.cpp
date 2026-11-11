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
    vars["KYTHIRA_DYNAMODB_ENGINE"] = "leveldb";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_ENGINE: unknown engine 'leveldb' (memory or rocksdb)");

    vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_LOG_LEVEL"] = "verbose";
    BOOST_TEST(config_error(vars) ==
               "KYTHIRA_DYNAMODB_LOG_LEVEL: unknown level 'verbose' "
               "(trace, debug, info, warning, error or critical)");

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

// ---------------------------------------------------------------------------
// A cluster node: its settings, and the forward endpoint peers send ops to.
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view k_forward_secret = "0123456789abcdef-forward";

auto cluster_env() -> std::map<std::string, std::string> {
    auto vars = minimal_env();
    vars["KYTHIRA_DYNAMODB_NODE_ID"] = "1";
    vars["KYTHIRA_DYNAMODB_PEERS"] =
        "1=http://10.0.0.1:7000, 2=http://10.0.0.2:7000,"
        "3=http://10.0.0.3:7000";
    vars["KYTHIRA_DYNAMODB_PEER_GATEWAYS"] = "2=10.0.0.2:8000,3=[fd00::3]:8443";
    vars["KYTHIRA_DYNAMODB_FORWARD_SECRET"] = std::string(k_forward_secret);
    return vars;
}

}  // namespace

BOOST_AUTO_TEST_CASE(cluster_settings_are_read) {
    auto vars = cluster_env();
    vars["KYTHIRA_DYNAMODB_RAFT_BIND"] = "10.0.0.1";
    vars["KYTHIRA_DYNAMODB_RAFT_PORT"] = "7100";
    vars["KYTHIRA_DYNAMODB_FORWARD_CA"] = "/etc/kythira/ca.pem";
    vars["KYTHIRA_DYNAMODB_ENGINE"] = "rocksdb";
    vars["KYTHIRA_DYNAMODB_DATA_DIR"] = "/var/lib/kythira";
    BOOST_REQUIRE_EQUAL(config_error(vars), "ok");
    vars["KYTHIRA_DYNAMODB_LOG_LEVEL"] = " warning ";
    BOOST_TEST(
        (std::get<ddb::gateway_config>(ddb::gateway_config_from_env(env_of(vars))).log_level ==
         kythira::log_level::warning));
    vars.erase("KYTHIRA_DYNAMODB_LOG_LEVEL");
    auto r = ddb::gateway_config_from_env(env_of(vars));
    const auto& c = std::get<ddb::gateway_config>(r);
    BOOST_TEST(c.engine == "rocksdb");
    BOOST_TEST((c.log_level == kythira::log_level::info));
    BOOST_TEST(c.cluster.enabled());
    BOOST_TEST(c.cluster.node_id == "1");
    BOOST_TEST(c.cluster.peers.size() == 3U);
    BOOST_TEST(c.cluster.peers.at("2") == "http://10.0.0.2:7000");
    BOOST_TEST(c.cluster.raft_bind == "10.0.0.1");
    BOOST_TEST(c.cluster.raft_port == 7100U);
    BOOST_TEST(c.cluster.forward_secret == k_forward_secret);
    BOOST_TEST(c.cluster.forward_ca_path == "/etc/kythira/ca.pem");
    BOOST_REQUIRE(c.cluster.peer_gateways.size() == 2U);
    BOOST_TEST(c.cluster.peer_gateways.at("2").host == "10.0.0.2");
    BOOST_TEST(c.cluster.peer_gateways.at("3").host == "fd00::3");
    BOOST_TEST(c.cluster.peer_gateways.at("3").port == 8443U);

    // Without PEERS there is no cluster; one PEERS entry is a cluster of
    // one, which needs no secret or gateways.
    BOOST_TEST(!std::get<ddb::gateway_config>(ddb::gateway_config_from_env(env_of(minimal_env())))
                    .cluster.enabled());
    auto one = minimal_env();
    one["KYTHIRA_DYNAMODB_NODE_ID"] = "a";
    one["KYTHIRA_DYNAMODB_PEERS"] = "a=http://127.0.0.1:7000";
    BOOST_TEST(config_error(one) == "ok");
}

BOOST_AUTO_TEST_CASE(bad_cluster_settings_are_refused_with_the_variable_named) {
    const auto with = [](const std::string& name, const std::optional<std::string>& value) {
        auto vars = cluster_env();
        if (value) {
            vars[name] = *value;
        } else {
            vars.erase(name);
        }
        return config_error(vars);
    };
    BOOST_TEST(with("KYTHIRA_DYNAMODB_NODE_ID", std::nullopt) ==
               "KYTHIRA_DYNAMODB_NODE_ID is required with KYTHIRA_DYNAMODB_PEERS");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_NODE_ID", "9") ==
               "KYTHIRA_DYNAMODB_PEERS must include this node's own id '9'");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_PEERS", "1=http://a:1,1=http://b:1") ==
               "KYTHIRA_DYNAMODB_PEERS: id '1' appears twice");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_PEERS", "1") ==
               "KYTHIRA_DYNAMODB_PEERS: expected id=value, got '1'");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_RAFT_PORT", "70000") ==
               "KYTHIRA_DYNAMODB_RAFT_PORT: not a port: '70000'");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_FORWARD_SECRET", "short")
                   .find("KYTHIRA_DYNAMODB_FORWARD_SECRET is required") == 0U);
    BOOST_TEST(with("KYTHIRA_DYNAMODB_FORWARD_SECRET", std::nullopt)
                   .find("KYTHIRA_DYNAMODB_FORWARD_SECRET is required") == 0U);
    BOOST_TEST(with("KYTHIRA_DYNAMODB_PEER_GATEWAYS", "2=10.0.0.2:8000") ==
               "KYTHIRA_DYNAMODB_PEER_GATEWAYS has no gateway for peer '3'");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_PEER_GATEWAYS", "2=10.0.0.2,3=h:1") ==
               "KYTHIRA_DYNAMODB_PEER_GATEWAYS: '10.0.0.2' is not host:port");
    BOOST_TEST(with("KYTHIRA_DYNAMODB_ENGINE", "rocksdb") ==
               "KYTHIRA_DYNAMODB_ENGINE=rocksdb needs KYTHIRA_DYNAMODB_DATA_DIR");
    auto single = minimal_env();
    single["KYTHIRA_DYNAMODB_ENGINE"] = "rocksdb";
    BOOST_TEST(config_error(single) ==
               "KYTHIRA_DYNAMODB_ENGINE=rocksdb needs a cluster: set "
               "KYTHIRA_DYNAMODB_PEERS (it may list only this node)");
}

BOOST_AUTO_TEST_CASE(a_forwarded_op_needs_a_fresh_mac_over_its_body) {
    const std::uint64_t t = 1794300000123ULL;
    const auto mac = ddb::forward_mac(k_forward_secret, t, "op");
    BOOST_TEST(mac.size() == 64U);
    BOOST_TEST(ddb::forward_authentic(k_forward_secret, std::to_string(t), mac, "op", t));
    // Within the allowed skew either way, and not beyond it.
    BOOST_TEST(ddb::forward_authentic(k_forward_secret, std::to_string(t), mac, "op", t + 60000));
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, std::to_string(t), mac, "op", t + 60001));
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, std::to_string(t), mac, "op", t - 60001));
    // Another body, time, secret or MAC fails.
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, std::to_string(t), mac, "op2", t));
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, std::to_string(t + 1), mac, "op", t));
    BOOST_TEST(!ddb::forward_authentic("another-secret-16", std::to_string(t), mac, "op", t));
    BOOST_TEST(
        !ddb::forward_authentic(k_forward_secret, std::to_string(t), mac.substr(1), "op", t));
    // A malformed time, or no secret at all, fails.
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, "", mac, "op", t));
    BOOST_TEST(!ddb::forward_authentic(k_forward_secret, "12x", mac, "op", t));
    BOOST_TEST(
        !ddb::forward_authentic("", std::to_string(t), ddb::forward_mac("", t, "op"), "op", t));
}

namespace {

/// local_backend that also runs forwarded ops, as raft_backend does: here
/// it echoes them.
struct op_backend : ddb::local_backend<> {
    using ddb::local_backend<>::local_backend;
    auto serve(std::string_view op) -> std::string { return "served:" + std::string(op); }
};

/// A gateway over op_backend with a forward secret, on a free port.
struct forwarding {
    kythira::memory_kv_engine engine;
    op_backend backend{engine};
    std::optional<ddb::gateway<op_backend>> gw;

    explicit forwarding(std::string_view secret) {
        auto vars = minimal_env();
        vars["KYTHIRA_DYNAMODB_BIND"] = "127.0.0.1";
        vars["KYTHIRA_DYNAMODB_PORT"] = "0";
        auto c = ddb::gateway_config_from_env(env_of(vars));
        BOOST_REQUIRE(std::holds_alternative<ddb::gateway_config>(c));
        auto cfg = std::get<ddb::gateway_config>(c);
        cfg.cluster.forward_secret = std::string(secret);
        gw.emplace(cfg, backend);
        gw->start();
    }

    auto post(const std::string& body, std::optional<std::uint64_t> time, const std::string& mac)
        -> http::response<http::string_body> {
        boost::asio::io_context ioc;
        boost::beast::tcp_stream s(ioc);
        s.expires_after(std::chrono::seconds(10));
        s.connect({boost::asio::ip::make_address("127.0.0.1"), gw->port()});
        http::request<http::string_body> req{http::verb::post, ddb::k_forward_path, 11};
        req.set(http::field::host, "127.0.0.1");
        if (time) {
            req.set(ddb::k_forward_time_header, std::to_string(*time));
        }
        req.set(ddb::k_forward_mac_header, mac);
        req.body() = body;
        req.prepare_payload();
        http::write(s, req);
        boost::beast::flat_buffer buf;
        http::response<http::string_body> res;
        http::read(s, buf, res);
        return res;
    }
};

auto now_ms() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_forward_endpoint_serves_authentic_ops_only) {
    forwarding f(k_forward_secret);
    const auto t = now_ms();
    auto r = f.post("abc", t, ddb::forward_mac(k_forward_secret, t, "abc"));
    BOOST_TEST(r.result_int() == 200U);
    BOOST_TEST(r.body() == "served:abc");

    // A wrong MAC, a stale time, or no time: refused, and the op not run.
    BOOST_TEST(f.post("abc", t, ddb::forward_mac(k_forward_secret, t, "abd")).result_int() == 403U);
    const auto old = t - 120000;
    BOOST_TEST(f.post("abc", old, ddb::forward_mac(k_forward_secret, old, "abc")).result_int() ==
               403U);
    BOOST_TEST(
        f.post("abc", std::nullopt, ddb::forward_mac(k_forward_secret, t, "abc")).result_int() ==
        403U);

    // The forward client reaches it and gets the answer back.
    const ddb::forward_client client(
        ddb::forward_client_config{.secret = std::string(k_forward_secret), .tls = false},
        {{"7", ddb::forward_peer{"127.0.0.1", f.gw->port()}}});
    BOOST_TEST(client.send("7", "xyz").value_or("none") == "served:xyz");
    BOOST_TEST(!client.send("8", "xyz").has_value());  // no such peer
    const ddb::forward_client wrong(
        ddb::forward_client_config{.secret = "a-different-secret", .tls = false},
        {{"7", ddb::forward_peer{"127.0.0.1", f.gw->port()}}});
    BOOST_TEST(!wrong.send("7", "xyz").has_value());  // refused: 403
}

BOOST_AUTO_TEST_CASE(without_a_secret_the_forward_path_is_not_served) {
    // A single node has no secret: the path is just an unsigned request to
    // the front end, which refuses it.
    forwarding f("");
    const auto t = now_ms();
    const auto r = f.post("abc", t, ddb::forward_mac("", t, "abc"));
    BOOST_TEST(r.result_int() == 400U);
    BOOST_TEST(r.body().find("served:") == std::string::npos);
}
