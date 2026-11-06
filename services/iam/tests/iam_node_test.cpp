// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// iam_node and kythira-iam as processes (.kiro/specs/iam-compatible-auth/
// task 9): a three-node cluster over mutual-TLS Raft RPC with an HTTPS
// listener, driven by the CLI with an admin certificate.
//
//  - create-account through a follower (the 307 to the leader is followed),
//    the root key file at mode 0600, list-accounts from another member;
//  - the admin routes refuse no certificate and a non-admin one;
//  - IAM and STS requests route by credential scope (NotImplemented until
//    tasks 14 and 17), unsigned ones are refused;
//  - rotate-sts-key retires the previous key;
//  - a read replica joins as a learner and applies the log;
//  - the leader is stopped, a new one serves writes, and the old one
//    catches up on restart;
//  - a member restarted with the wrong unseal passphrase exits.

#define BOOST_TEST_MODULE iam_node_test
#include <boost/test/results_collector.hpp>
#include <boost/test/unit_test.hpp>

#include "iam_test_pki.hpp"
#include "test_ports.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef IAM_NODE_PATH
#error "IAM_NODE_PATH must be defined"
#endif
#ifndef KYTHIRA_IAM_PATH
#error "KYTHIRA_IAM_PATH must be defined"
#endif

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using node_id = kythira::host_node_id;

auto make_id(int n) -> node_id {
    return *kythira::parse_host_peer_node_id(std::to_string(n));
}

auto slurp(const fs::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// Runs a program to completion, capturing stdout and stderr together.
auto run_capture(const std::vector<std::string>& args, std::chrono::seconds timeout = 60s)
    -> std::pair<int, std::string> {
    std::array<int, 2> fds{};
    BOOST_REQUIRE(::pipe(fds.data()) == 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::dup2(fds[1], 1);
        ::dup2(fds[1], 2);
        ::close(fds[0]);
        ::close(fds[1]);
        std::vector<char*> argv;
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    std::string out;
    std::array<char, 4096> buf{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool exited = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto n = ::read(fds[0], buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), static_cast<std::size_t>(n));
            continue;
        }
        if (::waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            while (true) {
                const auto m = ::read(fds[0], buf.data(), buf.size());
                if (m <= 0) {
                    break;
                }
                out.append(buf.data(), static_cast<std::size_t>(m));
            }
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    ::close(fds[0]);
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        return {-1, out + "\n[timed out]"};
    }
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), out};
}

struct http_result {
    unsigned status = 0;
    std::string body;
    std::string location;
};

/// One HTTPS request to a member, optionally with a client certificate.
auto https_call(std::uint16_t port, const kythira::iam::testing::test_pki_paths& pki,
                http::verb method, const std::string& target, const std::string& cert = {},
                const std::string& key = {}, const std::string& authorization = {})
    -> std::optional<http_result> {
    try {
        ssl::context ctx(ssl::context::tls_client);
        ctx.load_verify_file(pki.ca.string());
        ctx.set_verify_mode(ssl::verify_peer);
        if (!cert.empty()) {
            ctx.use_certificate_chain_file(cert);
            ctx.use_private_key_file(key, ssl::context::pem);
        }
        asio::io_context ioc;
        beast::ssl_stream<beast::tcp_stream> s(ioc, ctx);
        SSL_set_tlsext_host_name(s.native_handle(), "localhost");
        beast::get_lowest_layer(s).expires_after(5s);
        beast::get_lowest_layer(s).connect(
            asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
        s.handshake(ssl::stream_base::client);
        http::request<http::string_body> req{method, target, 11};
        req.set(http::field::host, "localhost");
        if (!authorization.empty()) {
            req.set(http::field::authorization, authorization);
        }
        req.prepare_payload();
        http::write(s, req);
        beast::flat_buffer buf;
        // Through a parser, not http::read into a message: that overload
        // moves the message into a parser and back, which the static
        // analyzer reads as a use after move.
        http::response_parser<http::string_body> parser;
        http::read(s, buf, parser);
        auto res = parser.release();
        auto location = std::string(res[http::field::location]);
        const auto status = res.result_int();
        return http_result{status, std::move(res.body()), std::move(location)};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

auto json_of(const std::string& s) -> boost::json::object {
    boost::system::error_code ec;
    auto v = boost::json::parse(s, ec);
    return !ec && v.is_object() ? v.as_object() : boost::json::object{};
}

struct member {
    node_id id;
    std::uint16_t rpc_port = 0;
    std::uint16_t http_port = 0;
    fs::path dir;
    bool replica = false;
    pid_t pid = -1;
};

class cluster {
public:
    cluster() {
        _dir = raft::make_private_temp_directory(fs::temp_directory_path(), "iam-node-test-");
        const auto base = kythira::testing::free_port_block(8);
        for (int i = 0; i < 4; ++i) {
            member m;
            m.id = make_id(i + 1);
            m.rpc_port = static_cast<std::uint16_t>(base + 2 * i);
            m.http_port = static_cast<std::uint16_t>(base + 2 * i + 1);
            m.dir = _dir / ("node-" + std::to_string(i + 1));
            m.replica = i == 3;
            fs::create_directories(m.dir);
            members.push_back(m);
        }
        kythira::iam::testing::test_pki_options o;
        o.dir = _dir;
        for (const auto& m : members) {
            o.nodes.push_back(m.id);
        }
        pki = kythira::iam::testing::write_test_pki(o);
        raft::write_private_file(_dir / "wrong-unseal.txt", "not-the-passphrase\n");
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    ~cluster() {
        for (auto& m : members) {
            stop(m);
        }
        if (std::getenv("IAM_NODE_TEST_KEEP") == nullptr) {
            std::error_code ec;
            fs::remove_all(_dir, ec);
        }
    }

    auto voters() -> std::vector<member*> { return {&members[0], &members[1], &members[2]}; }

    [[nodiscard]] auto peers_for(const member& self) const -> std::string {
        std::string out;
        for (const auto& m : members) {
            if (m.replica || m.id == self.id) {
                continue;
            }
            if (!out.empty()) {
                out += ",";
            }
            out += kythira::host_node_id_text(m.id) + ":127.0.0.1:" + std::to_string(m.rpc_port) +
                   "@https://localhost:" + std::to_string(m.http_port);
        }
        return out;
    }

    void start(member& m, const fs::path& unseal = {}) {
        const auto idt = kythira::host_node_id_text(m.id);
        std::vector<std::string> args{IAM_NODE_PATH,
                                      "--node-id",
                                      idt,
                                      "--data-dir",
                                      m.dir.string(),
                                      "--unseal-key-file",
                                      (unseal.empty() ? pki.unseal : unseal).string(),
                                      "--peers",
                                      peers_for(m),
                                      "--rpc-address",
                                      "127.0.0.1",
                                      "--rpc-port",
                                      std::to_string(m.rpc_port),
                                      "--http-address",
                                      "127.0.0.1",
                                      "--http-port",
                                      std::to_string(m.http_port),
                                      "--tls-cert",
                                      pki.node_cert(m.id).string(),
                                      "--tls-key",
                                      pki.node_key(m.id).string(),
                                      "--rpc-tls-cert",
                                      pki.node_cert(m.id).string(),
                                      "--rpc-tls-key",
                                      pki.node_key(m.id).string(),
                                      "--rpc-tls-ca",
                                      pki.ca.string(),
                                      "--admin-uri-san",
                                      kythira::iam::testing::k_admin_uri,
                                      "--election-timeout-min-ms",
                                      "400",
                                      "--election-timeout-max-ms",
                                      "800",
                                      "--heartbeat-interval-ms",
                                      "80",
                                      "--rpc-timeout-ms",
                                      "2000"};
        if (m.replica) {
            args.emplace_back("--replica");
        }
        const auto log = (m.dir / "node.log").string();
        const pid_t pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            std::vector<char*> argv;
            for (const auto& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }
        m.pid = pid;
    }

    static void stop(member& m) {
        if (m.pid <= 0) {
            return;
        }
        ::kill(m.pid, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (::waitpid(m.pid, &status, WNOHANG) == m.pid) {
                m.pid = -1;
                return;
            }
            std::this_thread::sleep_for(20ms);
        }
        BOOST_ERROR("iam_node " << m.pid << " ignored SIGTERM; killing it");
        ::kill(m.pid, SIGKILL);
        ::waitpid(m.pid, &status, 0);
        m.pid = -1;
    }

    /// The member's /healthz, or nothing while it does not answer.
    auto health(const member& m) -> std::optional<boost::json::object> {
        auto r = https_call(m.http_port, pki, http::verb::get, "/healthz");
        if (!r || r->status != 200) {
            return std::nullopt;
        }
        return json_of(r->body);
    }

    /// Waits until one running voter reports itself leader.
    auto wait_leader(std::chrono::seconds timeout = 60s) -> member* {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto* m : voters()) {
                if (m->pid <= 0) {
                    continue;
                }
                auto h = health(*m);
                if (h && h->at("role").as_string() == "leader") {
                    // A leader names itself, as a follower names it.
                    BOOST_TEST(h->at("leader").as_string() == h->at("node_id").as_string());
                    return m;
                }
            }
            std::this_thread::sleep_for(200ms);
        }
        return nullptr;
    }

    /// Waits until `m` reports `accounts` accounts applied.
    auto wait_accounts(const member& m, std::int64_t accounts, std::chrono::seconds timeout = 60s)
        -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto h = health(m);
            if (h && h->at("accounts").to_number<std::int64_t>() == accounts) {
                return true;
            }
            std::this_thread::sleep_for(200ms);
        }
        return false;
    }

    auto cli(const member& endpoint, std::vector<std::string> args) -> std::pair<int, std::string> {
        std::vector<std::string> full{KYTHIRA_IAM_PATH,
                                      "--endpoint",
                                      "https://localhost:" + std::to_string(endpoint.http_port),
                                      "--cert",
                                      pki.admin_cert.string(),
                                      "--key",
                                      pki.admin_key.string(),
                                      "--ca",
                                      pki.ca.string(),
                                      "--wait-secs",
                                      "60"};
        full.insert(full.end(), args.begin(), args.end());
        return run_capture(full, 90s);
    }

    void dump_logs() const {
        for (const auto& m : members) {
            BOOST_TEST_MESSAGE("---- node " << kythira::host_node_id_text(m.id) << " log ----\n"
                                            << slurp(m.dir / "node.log"));
        }
    }

    [[nodiscard]] auto dir() const -> const fs::path& { return _dir; }

    std::vector<member> members;
    kythira::iam::testing::test_pki_paths pki;

private:
    fs::path _dir;
};

}  // namespace

BOOST_AUTO_TEST_CASE(cluster_lifecycle, *boost::unit_test::timeout(600)) {
    cluster c;
    for (auto* m : c.voters()) {
        c.start(*m);
    }
    auto* leader = c.wait_leader();
    if (leader == nullptr) {
        c.dump_logs();
    }
    BOOST_REQUIRE(leader != nullptr);
    member* follower = nullptr;
    for (auto* m : c.voters()) {
        if (m != leader) {
            follower = m;
            break;
        }
    }
    if (follower == nullptr) {
        BOOST_FAIL("no follower among the voters");
        return;
    }

    // A follower redirects an admin request to the leader, once it has
    // heard from it (503 until then).
    std::optional<http_result> redirect;
    for (int i = 0; i < 100; ++i) {
        redirect = https_call(follower->http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                              c.pki.admin_cert.string(), c.pki.admin_key.string());
        if (redirect && redirect->status != 503) {
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    BOOST_REQUIRE(redirect.has_value());
    BOOST_TEST(redirect->status == 307U);
    BOOST_TEST(redirect->location ==
               "https://localhost:" + std::to_string(leader->http_port) + "/v1/admin/accounts");

    // create-account through the follower: the CLI follows the redirect.
    const auto key_file = c.dir() / "root-key.ini";
    auto [rc, out] = c.cli(*follower, {"create-account", "--alias", "first-account",
                                       "--root-key-file", key_file.string()});
    BOOST_TEST_INFO("kythira-iam output: " << out);
    BOOST_REQUIRE(rc == 0);
    BOOST_TEST(out.find("aws_access_key_id:     AKIA") != std::string::npos);
    struct stat st{};
    BOOST_REQUIRE(::stat(key_file.c_str(), &st) == 0);
    BOOST_TEST((st.st_mode & 0777) == 0600U);
    const auto ini = slurp(key_file);
    BOOST_TEST(ini.find("aws_secret_access_key = ") != std::string::npos);
    // The key file is never overwritten.
    auto [rc_again, out_again] =
        c.cli(*leader, {"create-account", "--root-key-file", key_file.string()});
    BOOST_TEST(rc_again == 1);
    BOOST_TEST(out_again.find("refusing to overwrite") != std::string::npos);

    // list-accounts from another member, once it has applied the entry.
    for (auto* m : c.voters()) {
        BOOST_TEST(c.wait_accounts(*m, 1));
    }
    auto [rc_list, listed] = c.cli(*follower, {"list-accounts", "--json"});
    BOOST_REQUIRE(rc_list == 0);
    const auto accounts = json_of(listed).at("accounts").as_array();
    BOOST_REQUIRE(accounts.size() == 1U);
    BOOST_TEST(std::string(accounts[0].as_object().at("alias").as_string()) == "first-account");
    BOOST_TEST(listed.find("secret") == std::string::npos);

    // Admin routes need an admin certificate.
    auto none = https_call(leader->http_port, c.pki, http::verb::get, "/v1/admin/accounts");
    BOOST_REQUIRE(none.has_value());
    BOOST_TEST(none->status == 403U);
    auto outsider = https_call(leader->http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                               c.pki.outsider_cert.string(), c.pki.outsider_key.string());
    BOOST_REQUIRE(outsider.has_value());
    BOOST_TEST(outsider->status == 403U);

    // AWS routing by credential scope.
    const std::string iam_auth =
        "AWS4-HMAC-SHA256 Credential=AKIAEXAMPLEKEYID0000/20261106/us-east-1/iam/aws4_request, "
        "SignedHeaders=host;x-amz-date, Signature=00";
    auto iam_call = https_call(follower->http_port, c.pki, http::verb::post, "/", {}, {}, iam_auth);
    BOOST_REQUIRE(iam_call.has_value());
    BOOST_TEST(iam_call->status == 501U);
    BOOST_TEST(iam_call->body.find("<Code>NotImplemented</Code>") != std::string::npos);
    BOOST_TEST(iam_call->body.find("https://iam.amazonaws.com/doc/2010-05-08/") !=
               std::string::npos);
    auto sts_call = https_call(follower->http_port, c.pki, http::verb::get,
                               "/?Action=GetCallerIdentity&X-Amz-Credential=AKIAEXAMPLEKEYID0000%"
                               "2F20261106%2Fus-east-1%2Fsts%2Faws4_request");
    BOOST_REQUIRE(sts_call.has_value());
    BOOST_TEST(sts_call->status == 501U);
    BOOST_TEST(sts_call->body.find("https://sts.amazonaws.com/doc/2011-06-15/") !=
               std::string::npos);
    auto unsigned_call =
        https_call(follower->http_port, c.pki, http::verb::get, "/?Action=ListUsers");
    BOOST_REQUIRE(unsigned_call.has_value());
    BOOST_TEST(unsigned_call->status == 403U);
    BOOST_TEST(unsigned_call->body.find("MissingAuthenticationToken") != std::string::npos);

    // STS key rotation.
    auto [rc_rot1, rot1] = c.cli(*leader, {"rotate-sts-key"});
    BOOST_TEST_INFO("rotate: " << rot1);
    BOOST_TEST(rc_rot1 == 0);
    auto [rc_rot2, rot2] = c.cli(*leader, {"rotate-sts-key", "--retire-after-secs", "60"});
    BOOST_TEST(rc_rot2 == 0);
    BOOST_TEST(rot2.find("retiring:") != std::string::npos);

    // A read replica joins as a learner and applies the log.
    auto& replica = c.members[3];
    c.start(replica);
    auto [rc_rep, rep] =
        c.cli(*follower, {"replica-add", "--node-id", "4", "--rpc-host", "127.0.0.1", "--rpc-port",
                          std::to_string(replica.rpc_port)});
    BOOST_TEST_INFO("replica-add: " << rep);
    BOOST_TEST(rc_rep == 0);
    BOOST_TEST(c.wait_accounts(replica, 1));
    auto rh = c.health(replica);
    BOOST_REQUIRE(rh.has_value());
    BOOST_TEST(std::string(rh->at("role").as_string()) == "replica");
    // Admin writes on a replica go to the leader too.
    auto rep_redirect = https_call(replica.http_port, c.pki, http::verb::get, "/v1/admin/accounts",
                                   c.pki.admin_cert.string(), c.pki.admin_key.string());
    BOOST_REQUIRE(rep_redirect.has_value());
    BOOST_TEST(rep_redirect->status == 307U);

    // Stop the leader: another voter takes over and serves writes, and the
    // replica keeps applying through the new leader.
    cluster::stop(*leader);
    auto* next = c.wait_leader();
    BOOST_REQUIRE(next != nullptr);
    BOOST_TEST(next != leader);
    auto [rc2, out2] = c.cli(*next, {"create-account", "--account-id", "222222222222",
                                     "--root-key-file", (c.dir() / "second.ini").string()});
    BOOST_TEST_INFO("second create-account: " << out2);
    BOOST_TEST(rc2 == 0);
    BOOST_TEST(c.wait_accounts(replica, 2));
    c.start(*leader);
    BOOST_TEST(c.wait_accounts(*leader, 2));

    // A member restarted with the wrong passphrase refuses to run once it
    // sees the sealed secrets.
    member* victim = nullptr;
    for (auto* m : c.voters()) {
        if (m != next) {
            victim = m;
            break;
        }
    }
    cluster::stop(*victim);
    c.start(*victim, c.dir() / "wrong-unseal.txt");
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(victim->pid, &status, WNOHANG) == victim->pid) {
            exited = true;
            victim->pid = -1;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    BOOST_TEST(exited);
    BOOST_TEST((exited && WIFEXITED(status) && WEXITSTATUS(status) == 1));
    BOOST_TEST(slurp(victim->dir / "node.log").find("wrong --unseal-key-file") !=
               std::string::npos);
    if (!boost::unit_test::results_collector
             .results(boost::unit_test::framework::current_test_case().p_id)
             .passed()) {
        c.dump_logs();
    }
}

BOOST_AUTO_TEST_CASE(refuses_unsafe_configurations) {
    auto dir = raft::make_private_temp_directory(fs::temp_directory_path(), "iam-node-flags-");
    raft::write_private_file(dir / "unseal.txt", "x\n");
    const std::string unseal = (dir / "unseal.txt").string();
    const std::vector<std::string> base{
        IAM_NODE_PATH, "--node-id",  "1",       "--unseal-key-file",        unseal,
        "--data-dir",  dir.string(), "--peers", "2:127.0.0.1:1@https://h:1"};
    auto with = [&](std::vector<std::string> extra) {
        auto args = base;
        args.insert(args.end(), extra.begin(), extra.end());
        return run_capture(args, 20s);
    };
    // No TLS and no --insecure-plaintext.
    auto [rc1, out1] = with({"--rpc-address", "127.0.0.1"});
    BOOST_TEST(rc1 == 1);
    BOOST_TEST(out1.find("--tls-cert and --tls-key are required") != std::string::npos);
    // Plaintext Raft RPC off loopback without the flag.
    auto [rc2, out2] = with({"--insecure-plaintext", "--rpc-address", "0.0.0.0"});
    BOOST_TEST(rc2 == 1);
    BOOST_TEST(out2.find("Raft RPC needs") != std::string::npos);
    // Admin SANs without TLS.
    auto [rc3, out3] = with({"--insecure-plaintext", "--rpc-address", "127.0.0.1",
                             "--admin-uri-san", "spiffe://x/admin"});
    BOOST_TEST(rc3 == 1);
    BOOST_TEST(out3.find("--admin-uri-san needs TLS") != std::string::npos);
    // An RPC certificate without this node's reserved SAN.
    kythira::iam::testing::test_pki_options o;
    o.dir = dir;
    o.nodes = {make_id(7)};
    auto pki = kythira::iam::testing::write_test_pki(o);
    auto [rc4, out4] = with({"--tls-cert", pki.node_cert(make_id(7)).string(), "--tls-key",
                             pki.node_key(make_id(7)).string(), "--rpc-tls-cert",
                             pki.node_cert(make_id(7)).string(), "--rpc-tls-key",
                             pki.node_key(make_id(7)).string(), "--rpc-tls-ca", pki.ca.string()});
    BOOST_TEST(rc4 == 1);
    BOOST_TEST(out4.find("reserved peer SAN") != std::string::npos);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
