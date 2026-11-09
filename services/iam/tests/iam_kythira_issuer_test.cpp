// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The Kythira CA as an IAM identity provider, end to end
// (.kiro/specs/iam-compatible-auth/ task 20): a ca_cluster_node and a
// three-member iam_node cluster as processes.
//
//  - the CA issues a workload a client certificate and, over mutual TLS,
//    an OIDC token;
//  - iam_node, given --kythira-issuer and --kythira-issuer-local, reads the
//    issuer's keys straight from the CA member (no discovery document, no
//    public host) and exchanges the token through an unsigned
//    AssumeRoleWithWebIdentity for role credentials the role's trust policy
//    allows by the kythira:workload claim;
//  - the session signs IAM and STS calls;
//  - with --iam-credentials-exchange, POST /v1/iam/credentials exchanges the
//    verified client certificate itself, matched by kythira:cert/san-uri;
//  - another workload, and a caller with no certificate, are refused.

#define BOOST_TEST_MODULE iam_kythira_issuer_test
#include <boost/test/unit_test.hpp>

#include "ca_cluster_node_harness.hpp"
#include "iam_node_test_support.hpp"

#include <raft/certificate_authority.hpp>
#include <raft/certificate_provider.hpp>

#include <httplib.h>

using namespace iam_node_testing;  // NOLINT(google-build-using-namespace)
using namespace std::chrono_literals;

namespace {

constexpr const char* k_issuer = "https://issuer.kythira.test";
constexpr const char* k_provider = "issuer.kythira.test";
constexpr const char* k_audience = "sts.amazonaws.com";
constexpr const char* k_prefix = "spiffe://kythira.test/wl/";
constexpr const char* k_auth_token = "iam-kythira-issuer-test-token";

auto enc(std::string_view s) -> std::string {
    static constexpr std::string_view hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if ((std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4U]);
            out.push_back(hex[c & 0xFU]);
        }
    }
    return out;
}

/// The text of the first <name> element in an XML body.
auto element(const std::string& xml, const std::string& name) -> std::string {
    const auto open = "<" + name + ">";
    const auto at = xml.find(open);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + open.size();
    return xml.substr(start, xml.find("</" + name + ">", start) - start);
}

struct workload {
    std::string cert;
    std::string key;
    std::string token;
};

/// A lone ca_cluster_node with an HTTPS client listener and OIDC issuance.
class ca_member {
public:
    explicit ca_member(fs::path dir)
        : http_port(static_cast<int>(kythira::testing::free_port())), _dir(std::move(dir)) {
        fs::create_directories(_dir);
        raft::testing::certificate_authority listener_ca;
        listener_root = (_dir / "listener_root.pem").string();
        std::ofstream(listener_root) << listener_ca.root_certificate_pem();
        raft::testing::leaf_certificate_options o;
        o.subject.common_name = "ca-node-1";
        o.ip_addresses = {"127.0.0.1"};
        auto leaf = listener_ca.issue(o);
        std::ofstream(_dir / "cert.pem") << leaf.chain_pem;
        std::ofstream(_dir / "key.pem") << leaf.private_key_pem;
        std::ofstream(_dir / "unseal.key") << "iam-kythira-issuer-test-passphrase\n";

        const int rpc_port = static_cast<int>(kythira::testing::free_port());
        const std::string peers = "1:127.0.0.1:" + std::to_string(rpc_port) +
                                  "@https://127.0.0.1:" + std::to_string(http_port);
        _node = std::make_unique<ca_cluster_harness::cluster_node_process>(
            1, rpc_port, http_port, (_dir / "node1").string(), (_dir / "unseal.key").string(),
            k_auth_token, peers, /*bootstrap=*/true,
            std::vector<std::string>{"--rpc-address", "127.0.0.1", "--http-address", "127.0.0.1",
                                     "--tls-cert", (_dir / "cert.pem").string(), "--tls-key",
                                     (_dir / "key.pem").string(), "--peer-tls-ca", listener_root,
                                     "--oidc-issuer-url", k_issuer, "--oidc-audience", k_audience,
                                     "--oidc-workload-uri-prefix", k_prefix});
    }

    [[nodiscard]] auto client(const std::string& cert = {}, const std::string& key = {}) const
        -> std::unique_ptr<httplib::SSLClient> {
        auto c = cert.empty()
                     ? std::make_unique<httplib::SSLClient>("127.0.0.1", http_port)
                     : std::make_unique<httplib::SSLClient>("127.0.0.1", http_port, cert, key);
        c->set_ca_cert_path(listener_root);
        c->enable_server_certificate_verification(true);
        c->set_connection_timeout(5, 0);
        c->set_read_timeout(30, 0);
        return c;
    }

    [[nodiscard]] static auto bearer() -> httplib::Headers {
        return {{"Authorization", std::string("Bearer ") + k_auth_token}};
    }

    /// The cluster CA's root, once the CA exists and has published a JWKS.
    auto wait_ready(std::chrono::seconds timeout) -> std::string {
        std::string root;
        bool jwks = false;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline && (root.empty() || !jwks)) {
            if (root.empty()) {
                if (auto r = client()->Get("/v1/root-ca", bearer()); r && r->status == 200) {
                    root = r->body;
                }
            }
            if (!jwks) {
                auto r = client()->Get("/.well-known/jwks.json");
                jwks = r && r->status == 200;
            }
            if (root.empty() || !jwks) {
                std::this_thread::sleep_for(250ms);
            }
        }
        return jwks ? root : std::string{};
    }

    /// A client certificate for `uri` from the cluster CA and the OIDC
    /// token the CA issues it over mutual TLS.
    auto issue(const std::string& uri, const std::string& name) -> workload {
        raft::testing::leaf_certificate_options wl;
        wl.subject.common_name = name;
        auto csr = raft::testing::generate_key_and_csr(wl);
        const boost::json::object body{{"csr_pem", csr.csr_pem},
                                       {"uri_names", boost::json::array{boost::json::string(uri)}},
                                       {"client_auth", true},
                                       {"server_auth", false}};
        auto res = client()->Post("/v1/certificates", bearer(), boost::json::serialize(body),
                                  "application/json");
        BOOST_REQUIRE(res);
        BOOST_REQUIRE_MESSAGE(res->status == 200, "issue: " << res->status << " " << res->body);
        workload w;
        w.cert = (_dir / (name + "-cert.pem")).string();
        w.key = (_dir / (name + "-key.pem")).string();
        std::ofstream(w.cert) << std::string(
            boost::json::parse(res->body).at("chain_pem").as_string());
        std::ofstream(w.key) << csr.private_key_pem;
        auto token = client(w.cert, w.key)
                         ->Post("/v1/oidc/token", std::string("audience=") + k_audience,
                                "application/x-www-form-urlencoded");
        BOOST_REQUIRE(token);
        BOOST_REQUIRE_MESSAGE(token->status == 200,
                              "token: " << token->status << " " << token->body);
        w.token = std::string(boost::json::parse(token->body).at("token").as_string());
        return w;
    }

    void stop() { _node->stop(); }

    std::string listener_root;
    int http_port = 0;

private:
    fs::path _dir;
    std::unique_ptr<ca_cluster_harness::cluster_node_process> _node;
};

}  // namespace

BOOST_AUTO_TEST_CASE(kythira_ca_tokens_and_certificates_become_role_credentials,
                     *boost::unit_test::timeout(400)) {
    // The test PKI's root is named apart from the cluster CA's: iam_node
    // trusts both for client certificates.
    cluster c("Kythira IAM Test PKI Root");
    ca_member ca(c.dir() / "ca");
    const auto cluster_root = ca.wait_ready(90s);
    BOOST_REQUIRE_MESSAGE(!cluster_root.empty(), "the CA never came up with a JWKS");
    const auto billing = ca.issue(std::string(k_prefix) + "billing/api", "billing");
    const auto other = ca.issue(std::string(k_prefix) + "reports/api", "reports");

    // Client certificates from either CA: the test PKI's for the admin CLI,
    // the cluster CA's for the workloads.
    const auto client_ca = c.dir() / "client-ca.pem";
    std::ofstream(client_ca) << slurp(c.pki.ca) << cluster_root;
    c.extra_args = {"--kythira-issuer",
                    k_issuer,
                    "--kythira-issuer-local",
                    "https://127.0.0.1:" + std::to_string(ca.http_port) + "/.well-known/jwks.json",
                    "--oidc-ca-file",
                    ca.listener_root,
                    "--client-ca",
                    client_ca.string(),
                    "--iam-credentials-exchange"};
    for (auto* m : c.voters()) {
        c.start(*m);
    }
    auto* leader = c.wait_leader();
    if (leader == nullptr) {
        c.dump_logs();
    }
    BOOST_REQUIRE(leader != nullptr);

    const auto key_file = c.dir() / "root-key.ini";
    auto [rc, out] = c.cli(*leader, {"create-account", "--root-key-file", key_file.string()});
    BOOST_TEST_INFO("kythira-iam output: " << out);
    BOOST_REQUIRE(rc == 0);
    // Session tokens are sealed with the cluster's STS key.
    auto [rc_rot, rot] = c.cli(*leader, {"rotate-sts-key"});
    BOOST_TEST_INFO("rotate-sts-key: " << rot);
    BOOST_REQUIRE(rc_rot == 0);
    const auto ini = slurp(key_file);
    const auto root_id = ini_value(ini, "aws_access_key_id");
    const auto root_secret = ini_value(ini, "aws_secret_access_key");

    // Leadership can move under a starved runner; every call re-resolves it.
    const auto lead = [&]() -> member& {
        auto* l = c.wait_leader();
        BOOST_REQUIRE(l != nullptr);
        return *l;
    };
    const auto root_call = [&](const std::string& body, const std::string& service = "iam") {
        auto r = signed_call(lead().http_port, c.pki, root_id, root_secret, body, service);
        BOOST_REQUIRE(r.has_value());
        BOOST_TEST_INFO("root call " << body << ": " << r->body);
        BOOST_TEST_REQUIRE(r->status == 200U);
        return r->body;
    };
    const auto account =
        element(root_call("Action=GetCallerIdentity&Version=2011-06-15", "sts"), "Account");
    BOOST_REQUIRE_EQUAL(account.size(), 12U);
    const std::string provider_arn = "arn:aws:iam::" + account + ":oidc-provider/" + k_provider;
    const std::string role_arn = "arn:aws:iam::" + account + ":role/billing";

    (void)root_call("Action=CreateOpenIDConnectProvider&Version=2010-05-08&Url=" + enc(k_issuer) +
                    "&ClientIDList.member.1=" + enc(k_audience) +
                    "&ThumbprintList.member.1=" + std::string(40, 'a'));
    const std::string workload_uri = std::string(k_prefix) + "billing/api";
    const std::string trust =
        R"({"Version":"2012-10-17","Statement":[)"
        R"({"Effect":"Allow","Principal":{"Federated":")" +
        provider_arn +
        R"("},"Action":"sts:AssumeRoleWithWebIdentity","Condition":{"StringEquals":{")" +
        k_provider + R"(:kythira:workload":")" + workload_uri + R"(",")" + k_provider +
        R"(:aud":")" + k_audience +
        R"("}}},)"
        R"({"Effect":"Allow","Principal":{"Federated":")" +
        provider_arn +
        R"("},"Action":"sts:AssumeRoleWithWebIdentity","Condition":{"StringEquals":{)"
        R"("kythira:cert/san-uri":")" +
        workload_uri + R"("}}}]})";
    (void)root_call(
        "Action=CreateRole&Version=2010-05-08&RoleName=billing"
        "&AssumeRolePolicyDocument=" +
        enc(trust));
    (void)root_call(
        "Action=PutRolePolicy&Version=2010-05-08&RoleName=billing&PolicyName=list-users"
        "&PolicyDocument=" +
        enc(R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Action":"iam:ListUsers","Resource":"*"}]})"));

    // The token, through an unsigned AssumeRoleWithWebIdentity. The CA
    // member's JWKS is read directly, so a fresh key can take a moment to
    // be seen; anything but IDPCommunicationError is final.
    const auto web_assume = [&](const std::string& token, const std::string& session) {
        const std::string body =
            "Action=AssumeRoleWithWebIdentity&Version=2011-06-15&RoleArn=" + enc(role_arn) +
            "&RoleSessionName=" + session + "&WebIdentityToken=" + enc(token);
        auto r = https_call(lead().http_port, c.pki, http::verb::post, "/", {}, {}, {},
                            {{"Content-Type", "application/x-www-form-urlencoded"}}, body);
        BOOST_REQUIRE(r.has_value());
        return *r;
    };
    auto assumed = web_assume(billing.token, "web");
    BOOST_TEST_INFO("AssumeRoleWithWebIdentity: " << assumed.body);
    if (assumed.status != 200U) {
        c.dump_logs();
    }
    BOOST_TEST_REQUIRE(assumed.status == 200U);
    BOOST_TEST(element(assumed.body, "Arn") ==
               "arn:aws:sts::" + account + ":assumed-role/billing/web");
    BOOST_TEST(element(assumed.body, "SubjectFromWebIdentityToken") == "workload:" + workload_uri);
    BOOST_TEST(element(assumed.body, "Provider") == k_provider);
    const auto web_id = element(assumed.body, "AccessKeyId");
    const auto web_secret = element(assumed.body, "SecretAccessKey");
    const auto web_token = element(assumed.body, "SessionToken");
    BOOST_TEST(web_id.starts_with("ASIA"));

    // The session signs calls: the role's policy allows ListUsers, STS
    // names the assumed role, and anything else is denied.
    auto listed = signed_call(lead().http_port, c.pki, web_id, web_secret,
                              "Action=ListUsers&Version=2010-05-08", "iam", web_token);
    BOOST_REQUIRE(listed.has_value());
    BOOST_TEST_INFO("ListUsers as the session: " << listed->body);
    BOOST_TEST(listed->status == 200U);
    auto me = signed_call(lead().http_port, c.pki, web_id, web_secret,
                          "Action=GetCallerIdentity&Version=2011-06-15", "sts", web_token);
    BOOST_REQUIRE(me.has_value());
    BOOST_TEST(element(me->body, "Arn") == "arn:aws:sts::" + account + ":assumed-role/billing/web");
    auto denied =
        signed_call(lead().http_port, c.pki, web_id, web_secret,
                    "Action=CreateUser&UserName=nope&Version=2010-05-08", "iam", web_token);
    BOOST_REQUIRE(denied.has_value());
    BOOST_TEST(denied->status == 403U);

    // Another workload's token is a valid Kythira token the trust policy
    // does not allow.
    auto refused = web_assume(other.token, "intruder");
    BOOST_TEST(refused.status == 403U);
    BOOST_TEST(refused.body.find("AccessDenied") != std::string::npos, refused.body);

    // The certificate itself, exchanged with no JWT step.
    const auto exchange = [&](const std::string& cert, const std::string& key,
                              const std::string& session) {
        auto r = https_call(lead().http_port, c.pki, http::verb::post, "/v1/iam/credentials", cert,
                            key, {}, {{"Content-Type", "application/x-www-form-urlencoded"}},
                            "RoleArn=" + enc(role_arn) + "&RoleSessionName=" + session);
        BOOST_REQUIRE(r.has_value());
        return *r;
    };
    const auto direct = exchange(billing.cert, billing.key, "direct");
    BOOST_TEST_INFO("credentials exchange: " << direct.body);
    BOOST_TEST_REQUIRE(direct.status == 200U);
    const auto doc = json_of(direct.body);
    BOOST_TEST(std::string(doc.at("assumedRoleUser").at("arn").as_string()) ==
               "arn:aws:sts::" + account + ":assumed-role/billing/direct");
    const auto& creds = doc.at("credentials").as_object();
    auto direct_me =
        signed_call(lead().http_port, c.pki, std::string(creds.at("accessKeyId").as_string()),
                    std::string(creds.at("secretAccessKey").as_string()),
                    "Action=GetCallerIdentity&Version=2011-06-15", "sts",
                    std::string(creds.at("sessionToken").as_string()));
    BOOST_REQUIRE(direct_me.has_value());
    BOOST_TEST(element(direct_me->body, "Arn") ==
               "arn:aws:sts::" + account + ":assumed-role/billing/direct");

    const auto other_cert = exchange(other.cert, other.key, "intruder");
    BOOST_TEST(other_cert.status == 403U);
    BOOST_TEST(other_cert.body.find("AccessDenied") != std::string::npos, other_cert.body);
    const auto no_cert = exchange({}, {}, "anonymous");
    BOOST_TEST(no_cert.status == 403U);

    ca.stop();
}
