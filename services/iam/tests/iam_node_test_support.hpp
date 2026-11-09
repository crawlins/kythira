// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// iam_node and kythira-iam as processes: the helpers iam_node_test and
// iam_kythira_issuer_test share (a four-member cluster on the test PKI,
// HTTPS calls with or without a client certificate, SigV4-signed IAM
// calls, and the admin CLI).

#include "iam_test_pki.hpp"
#include "test_ports.hpp"

#include <raft/sigv4_verifier.hpp>

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
#include <iostream>
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

namespace iam_node_testing {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using node_id = kythira::host_node_id;

inline auto make_id(int n) -> node_id {
    return *kythira::parse_host_peer_node_id(std::to_string(n));
}

inline auto slurp(const fs::path& p) -> std::string {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/// Runs a program to completion, capturing stdout and stderr together.
inline auto run_capture(const std::vector<std::string>& args, std::chrono::seconds timeout = 60s)
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
inline auto https_call(std::uint16_t port, const kythira::iam::testing::test_pki_paths& pki,
                       http::verb method, const std::string& target, const std::string& cert = {},
                       const std::string& key = {}, const std::string& authorization = {},
                       const std::vector<std::pair<std::string, std::string>>& headers = {},
                       const std::string& body = {}) -> std::optional<http_result> {
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
        for (const auto& [name, value] : headers) {
            req.set(name, value);
        }
        req.body() = body;
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

inline auto json_of(const std::string& s) -> boost::json::object {
    boost::system::error_code ec;
    auto v = boost::json::parse(s, ec);
    return !ec && v.is_object() ? v.as_object() : boost::json::object{};
}

/// The value after `name = ` in an INI file's text.
inline auto ini_value(const std::string& ini, const std::string& name) -> std::string {
    const auto at = ini.find(name + " = ");
    if (at == std::string::npos) {
        return {};
    }
    const auto start = at + name.size() + 3;
    return ini.substr(start, ini.find('\n', start) - start);
}

/// A SigV4-signed Query POST of `body` to a member on `port`, for
/// `service` ("iam" or "sts"), with a session token when one is given.
inline auto signed_call(std::uint16_t port, const kythira::iam::testing::test_pki_paths& pki,
                        const std::string& key_id, const std::string& secret,
                        const std::string& body, const std::string& service,
                        const std::string& session_token = {}) -> std::optional<http_result> {
    namespace sigv4 = kythira::sigv4;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto amz = sigv4::detail::format_amz_date(now);
    const std::string content_type = "application/x-www-form-urlencoded; charset=utf-8";
    const std::string signed_headers = session_token.empty()
                                           ? "content-type;host;x-amz-date"
                                           : "content-type;host;x-amz-date;x-amz-security-token";
    const auto auth = [&](std::string_view sig) {
        return "AWS4-HMAC-SHA256 Credential=" + key_id + "/" + amz.substr(0, 8) + "/us-east-1/" +
               service + "/aws4_request, SignedHeaders=" + signed_headers +
               ", Signature=" + std::string(sig);
    };
    sigv4::request r;
    r.method = "POST";
    r.raw_path = "/";
    r.body = std::string_view(body);
    r.headers = {{"Host", "localhost"},
                 {"Content-Type", content_type},
                 {"X-Amz-Date", amz},
                 {"Authorization", auth(std::string(64, '0'))}};
    std::vector<std::pair<std::string, std::string>> headers{{"Content-Type", content_type},
                                                             {"X-Amz-Date", amz}};
    if (!session_token.empty()) {
        r.headers.emplace_back("X-Amz-Security-Token", session_token);
        headers.emplace_back("X-Amz-Security-Token", session_token);
    }
    const auto parsed = sigv4::parse(r, {.service = service}, now);
    if (!parsed) {
        return std::nullopt;
    }
    const auto sig =
        sigv4::compute_signature(*parsed, sigv4::derive_signing_key(secret, parsed->scope));
    return https_call(port, pki, http::verb::post, "/", {}, {}, auth(sig), headers, body);
}

/// A SigV4-signed IAM Query POST of `body` to a member on `port`.
inline auto signed_iam_call(std::uint16_t port, const kythira::iam::testing::test_pki_paths& pki,
                            const std::string& key_id, const std::string& secret,
                            const std::string& body) -> std::optional<http_result> {
    return signed_call(port, pki, key_id, secret, body, "iam");
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
    /// `extra_pki_nodes`: more node ids to give certificates to, for a
    /// replica the test runs in process.
    explicit cluster(std::string ca_common_name = "Kythira Test Root CA",
                     const std::vector<int>& extra_pki_nodes = {}) {
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
        o.ca_common_name = std::move(ca_common_name);
        for (const auto& m : members) {
            o.nodes.push_back(m.id);
        }
        for (const int n : extra_pki_nodes) {
            o.nodes.push_back(make_id(n));
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
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        if (m.replica) {
            args.emplace_back("--replica");
            // Short, so the frozen-voters check below is quick.
            args.emplace_back("--iam-max-staleness-secs");
            args.emplace_back("3");
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
            // stderr, not BOOST_TEST_MESSAGE: the latter is dropped at the
            // default log level, which is what ctest runs with, so a CI
            // failure showed no node logs at all.
            std::cerr << "---- node " << kythira::host_node_id_text(m.id) << " log ----\n"
                      << slurp(m.dir / "node.log") << '\n';
            // The Raft log as the store holds it, one entry per line with the
            // command left out: a node that restarts and cannot apply has
            // been seen to come back with an index missing, and this is what
            // tells a lost write apart from a line the loader skipped.
            std::cerr << "---- node " << kythira::host_node_id_text(m.id) << " raft log ----\n";
            std::ifstream raft_log(m.dir / "log");
            std::string line;
            while (std::getline(raft_log, line)) {
                boost::system::error_code ec;
                auto v = boost::json::parse(line, ec);
                if (ec || !v.is_object()) {
                    std::cerr << "unparseable (" << line.size() << " bytes): " << line.substr(0, 80)
                              << '\n';
                    continue;
                }
                const auto& o = v.as_object();
                std::cerr << "index="
                          << boost::json::serialize(o.contains("index") ? o.at("index") : nullptr)
                          << " term="
                          << boost::json::serialize(o.contains("term") ? o.at("term") : nullptr)
                          << " type="
                          << boost::json::serialize(o.contains("type") ? o.at("type") : nullptr)
                          << '\n';
            }
        }
    }

    [[nodiscard]] auto dir() const -> const fs::path& { return _dir; }

    std::vector<member> members;
    kythira::iam::testing::test_pki_paths pki;
    /// Flags appended to every member's command line.
    std::vector<std::string> extra_args;

private:
    fs::path _dir;
};

}  // namespace iam_node_testing
