// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// IAM as a module of kythira_node (.kiro/specs/service-colocation/ task 8,
// Requirements 5.1-5.3 and 7.2), as processes: three kythira_node members
// run IAM and Kinesis together over mutual TLS on the node's one Raft
// listener. IAM elects and serves its admin and Query APIs as iam_node
// does, keeps its state across a restart, and Kinesis authorizes through
// the IAM module's in-process evaluator: a user's request is refused
// until a policy allows it, with no IAM replica embedded in Kinesis.

#define BOOST_TEST_MODULE kythira_node_iam_test

#include <boost/test/unit_test.hpp>

#include "iam_node_test_support.hpp"

#ifndef KYTHIRA_NODE_PATH
#error "KYTHIRA_NODE_PATH must be defined"
#endif

#include <raft/private_file.hpp>

#include <boost/json.hpp>

#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace iam_node_testing;
namespace sigv4 = kythira::sigv4;

struct node_proc {
    int n = 0;
    std::uint16_t raft = 0;
    std::uint16_t metrics = 0;
    std::uint16_t iam = 0;
    std::uint16_t kinesis = 0;
    pid_t pid = -1;
};

class node_cluster {
public:
    node_cluster() {
        _dir = raft::make_private_temp_directory(fs::temp_directory_path(), "kythira-node-iam-");
        const auto base = kythira::testing::free_port_block(12);
        for (int i = 0; i < 3; ++i) {
            node_proc p;
            p.n = i + 1;
            p.raft = static_cast<std::uint16_t>(base + 4 * i);
            p.metrics = static_cast<std::uint16_t>(base + 4 * i + 1);
            p.iam = static_cast<std::uint16_t>(base + 4 * i + 2);
            p.kinesis = static_cast<std::uint16_t>(base + 4 * i + 3);
            nodes.push_back(p);
        }
        kythira::iam::testing::test_pki_options o;
        o.dir = _dir;
        for (const auto& p : nodes) {
            o.nodes.push_back(make_id(p.n));
        }
        pki = kythira::iam::testing::write_test_pki(o);
    }

    node_cluster(const node_cluster&) = delete;
    auto operator=(const node_cluster&) -> node_cluster& = delete;
    node_cluster(node_cluster&&) = delete;
    auto operator=(node_cluster&&) -> node_cluster& = delete;

    ~node_cluster() {
        for (auto& p : nodes) {
            stop(p);
        }
        if (std::getenv("IAM_NODE_TEST_KEEP") == nullptr) {
            std::error_code ec;
            fs::remove_all(_dir, ec);
        }
    }

    void start(node_proc& p) {
        std::string peers;
        std::string iam_urls;
        std::string gateways;
        for (const auto& q : nodes) {
            const auto id = std::to_string(q.n);
            peers += (peers.empty() ? "" : ",") + id + "=127.0.0.1:" + std::to_string(q.raft);
            iam_urls +=
                (iam_urls.empty() ? "" : ",") + id + "=https://localhost:" + std::to_string(q.iam);
            gateways +=
                (gateways.empty() ? "" : ",") + id + "=localhost:" + std::to_string(q.kinesis);
        }
        const auto dir = _dir / ("node-" + std::to_string(p.n));
        fs::create_directories(dir);
        const std::vector<std::string> env{
            "KYTHIRA_SERVICES=iam,kinesis",
            "KYTHIRA_NODE_ID=" + std::to_string(p.n),
            "KYTHIRA_NODE_DATA_DIR=" + dir.string(),
            "KYTHIRA_NODE_PEERS=" + peers,
            "KYTHIRA_NODE_RAFT_BIND=127.0.0.1",
            "KYTHIRA_NODE_RAFT_PORT=" + std::to_string(p.raft),
            "KYTHIRA_NODE_TLS_CERT=" + pki.node_cert(make_id(p.n)).string(),
            "KYTHIRA_NODE_TLS_KEY=" + pki.node_key(make_id(p.n)).string(),
            "KYTHIRA_NODE_TLS_CA=" + pki.ca.string(),
            "KYTHIRA_NODE_METRICS_BIND=127.0.0.1",
            "KYTHIRA_NODE_METRICS_PORT=" + std::to_string(p.metrics),
            "KYTHIRA_IAM_UNSEAL_KEY_FILE=" + pki.unseal.string(),
            "KYTHIRA_IAM_HTTP_BIND=127.0.0.1",
            "KYTHIRA_IAM_HTTP_PORT=" + std::to_string(p.iam),
            "KYTHIRA_IAM_PEER_URLS=" + iam_urls,
            std::string("KYTHIRA_IAM_ADMIN_URI_SANS=") + kythira::iam::testing::k_admin_uri,
            "KINESIS_LISTEN=127.0.0.1:" + std::to_string(p.kinesis),
            "KINESIS_AUTH=iam",
            "KINESIS_REGION=us-east-1",
            "KINESIS_FORWARD_SECRET=kythira-node-iam-test-forward-secret",
            "KINESIS_PEER_GATEWAYS=" + gateways,
            "PATH=/usr/bin:/bin",
        };
        const auto log = (dir / "node.log").string();
        const pid_t pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            std::vector<char*> envp;
            for (const auto& e : env) {
                envp.push_back(const_cast<char*>(e.c_str()));
            }
            envp.push_back(nullptr);
            char* argv[] = {const_cast<char*>(KYTHIRA_NODE_PATH), nullptr};
            ::execve(argv[0], argv, envp.data());
            ::_exit(127);
        }
        p.pid = pid;
    }

    static void stop(node_proc& p) {
        if (p.pid <= 0) {
            return;
        }
        ::kill(p.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(p.pid, &status, WNOHANG) == p.pid) {
                p.pid = -1;
                BOOST_TEST(WIFEXITED(status));
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        BOOST_ERROR("kythira_node " << p.pid << " ignored SIGTERM; killing it");
        ::kill(p.pid, SIGKILL);
        ::waitpid(p.pid, &status, 0);
        p.pid = -1;
    }

    auto health(const node_proc& p) -> std::optional<boost::json::object> {
        auto r = https_call(p.iam, pki, http::verb::get, "/healthz");
        if (!r || r->status != 200) {
            return std::nullopt;
        }
        return json_of(r->body);
    }

    /// The member IAM's group elected, once one reports itself leader.
    auto wait_leader(std::chrono::seconds timeout = 90s) -> node_proc* {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& p : nodes) {
                auto h = health(p);
                if (h && h->at("role").as_string() == "leader") {
                    return &p;
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        return nullptr;
    }

    [[nodiscard]] auto dir() const -> const fs::path& { return _dir; }

    std::vector<node_proc> nodes;
    kythira::iam::testing::test_pki_paths pki;

private:
    fs::path _dir;
};

/// A SigV4-signed Kinesis JSON call (`Kinesis_20131202.<action>`).
auto kinesis_call(std::uint16_t port, const kythira::iam::testing::test_pki_paths& pki,
                  const std::string& key_id, const std::string& secret, const std::string& action,
                  const std::string& body) -> std::optional<http_result> {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto amz = sigv4::detail::format_amz_date(now);
    const std::string content_type = "application/x-amz-json-1.1";
    const std::string target = "Kinesis_20131202." + action;
    const std::string signed_headers = "content-type;host;x-amz-date;x-amz-target";
    const auto auth = [&](std::string_view sig) {
        return "AWS4-HMAC-SHA256 Credential=" + key_id + "/" + amz.substr(0, 8) +
               "/us-east-1/kinesis/aws4_request, SignedHeaders=" + signed_headers +
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
    const auto parsed = sigv4::parse(r, {.service = "kinesis"}, now);
    if (!parsed) {
        return std::nullopt;
    }
    const auto sig =
        sigv4::compute_signature(*parsed, sigv4::derive_signing_key(secret, parsed->scope));
    return https_call(
        port, pki, http::verb::post, "/", {}, {}, auth(sig),
        {{"Content-Type", content_type}, {"X-Amz-Date", amz}, {"X-Amz-Target", target}}, body);
}

/// Repeats `call` until it answers `status` with `marker` in the body, for
/// state that reaches a member's applied log a moment after the leader
/// acknowledged it.
template<typename Call>
auto until_status(unsigned status, Call call, std::string_view marker = {},
                  std::chrono::seconds timeout = 20s) -> std::optional<http_result> {
    std::optional<http_result> r;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        r = call();
        if (r && r->status == status && r->body.find(marker) != std::string::npos) {
            return r;
        }
        std::this_thread::sleep_for(200ms);
    }
    return r;
}

auto form_value(const std::string& xml, const std::string& tag) -> std::string {
    const auto open = "<" + tag + ">";
    const auto at = xml.find(open);
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + open.size();
    return xml.substr(start, xml.find("</" + tag + ">", start) - start);
}

}  // namespace

BOOST_AUTO_TEST_CASE(iam_module_authorizes_kinesis_in_process, *boost::unit_test::timeout(600)) {
    node_cluster c;
    for (auto& p : c.nodes) {
        c.start(p);
    }
    auto* leader = c.wait_leader();
    BOOST_REQUIRE_MESSAGE(leader != nullptr, "IAM elected no leader on the shared listener");

    // The admin route, with the admin certificate, on the leader.
    auto created =
        https_call(leader->iam, c.pki, http::verb::post, "/v1/admin/accounts",
                   c.pki.admin_cert.string(), c.pki.admin_key.string(), {},
                   {{"Content-Type", "application/json"}}, R"({"account_id":"111122223333"})");
    BOOST_REQUIRE(created.has_value());
    BOOST_REQUIRE_EQUAL(created->status, 201U);
    const auto account = json_of(created->body);
    const std::string root_id(account.at("access_key_id").as_string());
    const std::string root_secret(account.at("secret_access_key").as_string());

    // A user with a key and no policy, through IAM's Query API.
    auto user = signed_iam_call(leader->iam, c.pki, root_id, root_secret,
                                "Action=CreateUser&UserName=bob&Version=2010-05-08");
    BOOST_REQUIRE(user.has_value());
    BOOST_REQUIRE_EQUAL(user->status, 200U);
    auto key = signed_iam_call(leader->iam, c.pki, root_id, root_secret,
                               "Action=CreateAccessKey&UserName=bob&Version=2010-05-08");
    BOOST_REQUIRE(key.has_value());
    BOOST_REQUIRE_EQUAL(key->status, 200U);
    const auto bob_id = form_value(key->body, "AccessKeyId");
    const auto bob_secret = form_value(key->body, "SecretAccessKey");
    BOOST_REQUIRE(!bob_id.empty());

    // Every member's Kinesis authorizes against its own applied IAM state.
    for (const auto& p : c.nodes) {
        // An unknown key is not IAM's.
        auto unknown =
            kinesis_call(p.kinesis, c.pki, "AKIAUNKNOWNUNKNOWN00", "x", "ListStreams", "{}");
        BOOST_REQUIRE(unknown.has_value());
        BOOST_TEST(unknown->status == 400U);
        BOOST_TEST(unknown->body.find("UnrecognizedClientException") != std::string::npos);

        // bob is known everywhere once his key has applied, and denied.
        auto denied = until_status(
            400U,
            [&] { return kinesis_call(p.kinesis, c.pki, bob_id, bob_secret, "ListStreams", "{}"); },
            "AccessDeniedException");
        BOOST_REQUIRE(denied.has_value());
        BOOST_TEST(denied->status == 400U);
        BOOST_TEST(denied->body.find("AccessDeniedException") != std::string::npos, denied->body);
    }

    auto policy = signed_iam_call(
        leader->iam, c.pki, root_id, root_secret,
        "Action=PutUserPolicy&UserName=bob&PolicyName=list&Version=2010-05-08&PolicyDocument=" +
            std::string("%7B%22Version%22%3A%222012-10-17%22%2C%22Statement%22%3A%5B%7B%22Effect%"
                        "22%3A%22Allow%22%2C%22Action%22%3A%22kinesis%3AListStreams%22%2C%"
                        "22Resource%22%3A%22%2A%22%7D%5D%7D"));
    BOOST_REQUIRE(policy.has_value());
    BOOST_REQUIRE_EQUAL(policy->status, 200U);
    for (const auto& p : c.nodes) {
        auto allowed = until_status(200U, [&] {
            return kinesis_call(p.kinesis, c.pki, bob_id, bob_secret, "ListStreams", "{}");
        });
        BOOST_REQUIRE(allowed.has_value());
        BOOST_TEST(allowed->status == 200U, allowed->body);
    }

    // IAM's state is in its group's log: a whole-cluster restart keeps it.
    for (auto& p : c.nodes) {
        node_cluster::stop(p);
    }
    for (auto& p : c.nodes) {
        c.start(p);
    }
    BOOST_REQUIRE(c.wait_leader() != nullptr);
    for (const auto& p : c.nodes) {
        auto allowed = until_status(200U, [&] {
            return kinesis_call(p.kinesis, c.pki, bob_id, bob_secret, "ListStreams", "{}");
        });
        BOOST_REQUIRE(allowed.has_value());
        BOOST_TEST(allowed->status == 200U, allowed->body);
    }
}
