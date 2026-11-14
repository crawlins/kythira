// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Cross-account DynamoDB access under Kythira IAM, end to end
// (.kiro/specs/dynamodb-compatible-api/ task 28): three iam_node members
// and one dynamodb_gateway_node, all as processes, the gateway with
// KYTHIRA_DYNAMODB_AUTH=iam and KYTHIRA_DYNAMODB_NAMESPACES=on and its
// embedded IAM replica registered with `kythira-iam replica-add`.
//
//  - two accounts, each with a user whose identity policy allows
//    dynamodb:*, have tables of the same bare name that never meet;
//  - account B's user reaching account A's table by ARN is refused while
//    the table has no resource policy;
//  - once A's root puts a resource policy naming B's user, that user may
//    read and write A's table by ARN, and only with the actions the
//    policy names;
//  - A's own user sees B's write by the bare name.
//
// The test lives in the IAM module because it needs the IAM cluster
// fixture (iam_node_test_support.hpp); it meets the DynamoDB module only
// as a program on the far side of a socket.

#define BOOST_TEST_MODULE iam_dynamodb_cross_account_test
#include <boost/test/unit_test.hpp>

#include "iam_node_test_support.hpp"

#ifndef DYNAMODB_GATEWAY_NODE_PATH
#error "DYNAMODB_GATEWAY_NODE_PATH must be defined"
#endif

using namespace iam_node_testing;  // NOLINT(google-build-using-namespace)
using namespace std::chrono_literals;

namespace {

auto enc(std::string_view s) -> std::string {
    return kythira::sigv4::detail::uri_encode(s, false);
}

auto element(const std::string& xml, const std::string& name) -> std::string {
    const auto open = "<" + name + ">";
    const auto at = xml.find(open);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + open.size();
    return xml.substr(start, xml.find("</" + name + ">", start) - start);
}

struct credentials {
    std::string key_id;
    std::string secret;
};

/// One plaintext HTTP POST to the gateway.
auto http_post(std::uint16_t port, const std::vector<std::pair<std::string, std::string>>& headers,
               const std::string& body) -> std::optional<http_result> {
    try {
        asio::io_context ioc;
        beast::tcp_stream s(ioc);
        s.expires_after(10s);
        s.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        http::request<http::string_body> req{http::verb::post, "/", 11};
        for (const auto& [name, value] : headers) {
            req.set(name, value);
        }
        req.body() = body;
        req.prepare_payload();
        http::write(s, req);
        beast::flat_buffer buf;
        http::response_parser<http::string_body> parser;
        http::read(s, buf, parser);
        auto res = parser.release();
        const auto status = res.result_int();
        return http_result{status, std::move(res.body()), {}};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// A SigV4-signed DynamoDB JSON 1.0 call: `X-Amz-Target:
/// DynamoDB_20120810.<operation>`.
auto dynamodb_call(std::uint16_t port, const credentials& who, const std::string& operation,
                   const std::string& body) -> std::optional<http_result> {
    namespace sigv4 = kythira::sigv4;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto amz = sigv4::detail::format_amz_date(now);
    const std::string content_type = "application/x-amz-json-1.0";
    const std::string target = "DynamoDB_20120810." + operation;
    const std::string signed_headers = "content-type;host;x-amz-date;x-amz-target";
    const auto auth = [&](std::string_view sig) {
        return "AWS4-HMAC-SHA256 Credential=" + who.key_id + "/" + amz.substr(0, 8) +
               "/us-east-1/dynamodb/aws4_request, SignedHeaders=" + signed_headers +
               ", Signature=" + std::string(sig);
    };
    sigv4::request r;
    r.method = "POST";
    r.raw_path = "/";
    r.body = std::string_view(body);
    r.headers = {{"Host", "localhost"},
                 {"Content-Type", content_type},
                 {"X-Amz-Date", amz},
                 {"X-Amz-Target", target},
                 {"Authorization", auth(std::string(64, '0'))}};
    const auto parsed = sigv4::parse(r, {.service = "dynamodb"}, now);
    if (!parsed) {
        return std::nullopt;
    }
    const auto sig =
        sigv4::compute_signature(*parsed, sigv4::derive_signing_key(who.secret, parsed->scope));
    return http_post(port,
                     {{"Host", "localhost"},
                      {"Content-Type", content_type},
                      {"X-Amz-Date", amz},
                      {"X-Amz-Target", target},
                      {"Authorization", auth(sig)}},
                     body);
}

/// The error type of a DynamoDB error body, after its `#`.
auto error_type(const http_result& r) -> std::string {
    const auto o = json_of(r.body);
    const auto* t = o.if_contains("__type");
    if (t == nullptr || !t->is_string()) {
        return {};
    }
    const std::string full(t->as_string());
    return full.substr(full.find('#') + 1);
}

/// dynamodb_gateway_node as a child process with `env` added to its
/// environment, logging to `log`.
class gateway_process {
public:
    gateway_process(const std::vector<std::pair<std::string, std::string>>& env, fs::path log)
        : _log(std::move(log)) {
        const pid_t pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            const int fd = ::open(_log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            for (const auto& [name, value] : env) {
                ::setenv(name.c_str(), value.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
            }
            std::array<char*, 2> argv{const_cast<char*>(DYNAMODB_GATEWAY_NODE_PATH), nullptr};
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        _pid = pid;
    }

    gateway_process(const gateway_process&) = delete;
    auto operator=(const gateway_process&) -> gateway_process& = delete;
    gateway_process(gateway_process&&) = delete;
    auto operator=(gateway_process&&) -> gateway_process& = delete;

    ~gateway_process() {
        if (_pid <= 0) {
            return;
        }
        ::kill(_pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(_pid, &status, WNOHANG) == _pid) {
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        ::kill(_pid, SIGKILL);
        ::waitpid(_pid, &status, 0);
    }

    [[nodiscard]] auto log() const -> std::string { return slurp(_log); }

    /// True once the process has exited, which it does on a bad setting.
    [[nodiscard]] auto exited() -> bool {
        int status = 0;
        if (_pid > 0 && ::waitpid(_pid, &status, WNOHANG) == _pid) {
            _pid = -1;
        }
        return _pid <= 0;
    }

private:
    fs::path _log;
    pid_t _pid = -1;
};

auto eventually(const std::function<bool()>& ok, std::chrono::seconds within) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ok()) {
            return true;
        }
        std::this_thread::sleep_for(250ms);
    }
    return ok();
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_resource_policy_opens_one_accounts_table_to_another,
                     *boost::unit_test::timeout(500)) {
    constexpr int k_replica = 5;
    cluster c("Kythira Test Root CA", {k_replica});
    for (auto* m : c.voters()) {
        c.start(*m);
    }
    auto* leader = c.wait_leader();
    if (leader == nullptr) {
        c.dump_logs();
    }
    BOOST_REQUIRE(leader != nullptr);
    const auto lead = [&]() -> member& {
        auto* l = c.wait_leader();
        BOOST_REQUIRE(l != nullptr);
        return *l;
    };

    // Two accounts, each with its root key and a user allowed dynamodb:*.
    struct account_keys {
        std::string id;
        credentials root;
        credentials user;
        std::string user_arn;
    };
    const auto make_account = [&](const std::string& tag) {
        account_keys a;
        const auto key_file = c.dir() / ("root-key-" + tag + ".ini");
        auto [rc, out] = c.cli(lead(), {"create-account", "--root-key-file", key_file.string()});
        BOOST_TEST_INFO("kythira-iam output: " << out);
        BOOST_REQUIRE(rc == 0);
        const auto ini = slurp(key_file);
        a.root = {ini_value(ini, "aws_access_key_id"), ini_value(ini, "aws_secret_access_key")};
        const auto root_call = [&](const std::string& body, const std::string& service = "iam") {
            auto r =
                signed_call(lead().http_port, c.pki, a.root.key_id, a.root.secret, body, service);
            BOOST_REQUIRE(r.has_value());
            BOOST_TEST_INFO("account " << tag << " root call " << body << ": " << r->body);
            BOOST_TEST_REQUIRE(r->status == 200U);
            return r->body;
        };
        a.id = element(root_call("Action=GetCallerIdentity&Version=2011-06-15", "sts"), "Account");
        BOOST_REQUIRE_EQUAL(a.id.size(), 12U);
        (void)root_call("Action=CreateUser&Version=2010-05-08&UserName=app");
        const auto key_xml = root_call("Action=CreateAccessKey&Version=2010-05-08&UserName=app");
        a.user = {element(key_xml, "AccessKeyId"), element(key_xml, "SecretAccessKey")};
        BOOST_REQUIRE(!a.user.key_id.empty() && !a.user.secret.empty());
        a.user_arn = "arn:aws:iam::" + a.id + ":user/app";
        const std::string policy =
            R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"dynamodb:*",)"
            R"("Resource":"*"}]})";
        (void)root_call(
            "Action=PutUserPolicy&Version=2010-05-08&UserName=app&PolicyName=dynamodb"
            "&PolicyDocument=" +
            enc(policy));
        return a;
    };
    const auto a = make_account("a");
    const auto b = make_account("b");
    BOOST_REQUIRE(a.id != b.id);

    // The gateway, its IAM replica registered with the group.
    const auto gateway_port = kythira::testing::free_port();
    const auto replica_port = kythira::testing::free_port();
    std::string voters;
    for (auto* v : c.voters()) {
        voters += (voters.empty() ? "" : ",") + kythira::host_node_id_text(v->id) +
                  ":127.0.0.1:" + std::to_string(v->rpc_port);
    }
    const auto replica_dir = c.dir() / "gateway-iam-replica";
    fs::create_directories(replica_dir);
    gateway_process gateway(
        {
            {"KYTHIRA_DYNAMODB_BIND", "127.0.0.1"},
            {"KYTHIRA_DYNAMODB_PORT", std::to_string(gateway_port)},
            {"KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT", "1"},
            {"KYTHIRA_DYNAMODB_AUTH", "iam"},
            {"KYTHIRA_DYNAMODB_NAMESPACES", "on"},
            // Required with IAM: the owner of tables made before
            // namespaces were on, of which this gateway has none.
            {"KYTHIRA_DYNAMODB_ACCOUNT_ID", a.id},
            {"KYTHIRA_IAM_VOTERS", voters},
            {"KYTHIRA_IAM_REPLICA_NODE_ID", std::to_string(k_replica)},
            {"KYTHIRA_IAM_REPLICA_RPC_BIND", "127.0.0.1"},
            {"KYTHIRA_IAM_REPLICA_RPC_PORT", std::to_string(replica_port)},
            {"KYTHIRA_IAM_DATA_DIR", replica_dir.string()},
            {"KYTHIRA_IAM_UNSEAL_KEY_FILE", c.pki.unseal.string()},
            {"KYTHIRA_IAM_RPC_TLS_CERT", c.pki.node_cert(make_id(k_replica)).string()},
            {"KYTHIRA_IAM_RPC_TLS_KEY", c.pki.node_key(make_id(k_replica)).string()},
            {"KYTHIRA_IAM_RPC_TLS_CA", c.pki.ca.string()},
        },
        c.dir() / "gateway.log");
    auto [rc_add, added] =
        c.cli(lead(), {"replica-add", "--node-id", std::to_string(k_replica), "--rpc-host",
                       "127.0.0.1", "--rpc-port", std::to_string(replica_port)});
    BOOST_TEST_INFO("replica-add: " << added);
    BOOST_REQUIRE(rc_add == 0);

    const auto call = [&](const credentials& who, const std::string& op, const std::string& body) {
        auto r = dynamodb_call(gateway_port, who, op, body);
        return r.value_or(http_result{0, "no answer", {}});
    };
    // Both users' keys and policies reach the replica.
    std::string last;
    const bool ready = eventually(
        [&] {
            if (gateway.exited()) {
                last = "dynamodb_gateway_node exited";
                return true;
            }
            const auto ra = call(a.user, "ListTables", "{}");
            const auto rb = call(b.user, "ListTables", "{}");
            last = ra.body + " / " + rb.body;
            return ra.status == 200 && rb.status == 200;
        },
        90s);
    if (!ready || gateway.exited()) {
        c.dump_logs();
        std::cerr << "---- gateway log ----\n" << gateway.log() << '\n';
    }
    BOOST_TEST_REQUIRE((ready && !gateway.exited()), "last answers: " << last);

    const auto report = [&](const std::string& what, const http_result& r) {
        BOOST_TEST_INFO(what << ": " << r.status << " " << r.body);
    };
    const std::string create_orders =
        R"({"TableName":"orders","BillingMode":"PAY_PER_REQUEST",)"
        R"("AttributeDefinitions":[{"AttributeName":"id","AttributeType":"S"}],)"
        R"("KeySchema":[{"AttributeName":"id","KeyType":"HASH"}]})";

    // Each account has its own "orders": the bare name is per account.
    {
        const auto r = call(a.user, "CreateTable", create_orders);
        report("A CreateTable", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
    }
    {
        const auto r = call(b.user, "CreateTable", create_orders);
        report("B CreateTable", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        const auto arn = json_of(r.body).at("TableDescription").at("TableArn").as_string();
        BOOST_TEST(arn == "arn:aws:dynamodb:us-east-1:" + b.id + ":table/orders");
    }
    const std::string a_orders = "arn:aws:dynamodb:us-east-1:" + a.id + ":table/orders";
    const auto put_item = [&](const std::string& table, const std::string& id) {
        return R"({"TableName":")" + table + R"(","Item":{"id":{"S":")" + id + R"("}}})";
    };
    const auto get_item = [&](const std::string& table, const std::string& id) {
        return R"({"TableName":")" + table + R"(","Key":{"id":{"S":")" + id +
               R"("}},"ConsistentRead":true})";
    };

    // Without a resource policy, B's user is refused A's table by ARN,
    // although its own identity policy allows dynamodb:*.
    {
        const auto r = call(b.user, "GetItem", get_item(a_orders, "1"));
        report("B GetItem on A's table, no resource policy", r);
        BOOST_TEST(r.status == 400U);
        BOOST_TEST(error_type(r) == "AccessDeniedException");
    }

    // A's root opens GetItem, PutItem and DescribeTable to B's user.
    const std::string resource_policy =
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow",)"
        R"("Principal":{"AWS":")" +
        b.user_arn +
        R"("},"Action":["dynamodb:GetItem","dynamodb:PutItem","dynamodb:DescribeTable"],)"
        R"("Resource":")" +
        a_orders + R"("}]})";
    {
        boost::json::object req;
        req["ResourceArn"] = a_orders;
        req["Policy"] = resource_policy;
        const auto r = call(a.root, "PutResourcePolicy", boost::json::serialize(req));
        report("A PutResourcePolicy", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
    }

    // Now B's user may write and read A's table by ARN...
    {
        const auto r = call(b.user, "PutItem", put_item(a_orders, "from-b"));
        report("B PutItem on A's table", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
    }
    {
        const auto r = call(b.user, "GetItem", get_item(a_orders, "from-b"));
        report("B GetItem on A's table", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        BOOST_TEST(json_of(r.body).contains("Item"));
    }
    {
        const auto r = call(b.user, "DescribeTable", R"({"TableName":")" + a_orders + R"("})");
        report("B DescribeTable on A's table", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        BOOST_TEST(json_of(r.body).at("Table").at("TableArn").as_string() == a_orders);
    }
    // ...but only with the actions the policy names.
    {
        const auto r = call(b.user, "DeleteTable", R"({"TableName":")" + a_orders + R"("})");
        report("B DeleteTable on A's table", r);
        BOOST_TEST(r.status == 400U);
        BOOST_TEST(error_type(r) == "AccessDeniedException");
    }

    // A's own user sees B's item by the bare name; B's "orders" by its
    // bare name does not have it.
    {
        const auto r = call(a.user, "GetItem", get_item("orders", "from-b"));
        report("A GetItem by bare name", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        BOOST_TEST(json_of(r.body).contains("Item"));
    }
    {
        const auto r = call(b.user, "GetItem", get_item("orders", "from-b"));
        report("B GetItem on its own table", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        BOOST_TEST(!json_of(r.body).contains("Item"));
    }
    // Each account lists only its own tables.
    for (const auto* who : {&a, &b}) {
        const auto r = call(who->user, "ListTables", "{}");
        report("ListTables", r);
        BOOST_TEST_REQUIRE(r.status == 200U);
        BOOST_TEST(json_of(r.body).at("TableNames").as_array().size() == 1U);
    }
}
