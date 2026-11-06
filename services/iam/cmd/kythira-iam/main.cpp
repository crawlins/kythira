// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// kythira-iam: the IAM admin CLI (.kiro/specs/iam-compatible-auth/
// Requirement 8.3, design Component 7). Kythira-only operations with no AWS
// API, for bootstrapping a cluster before anyone holds IAM credentials:
//
//   create-account   a new account and its root user's first access key,
//                    printed once and written to --root-key-file (0600)
//   list-accounts    every account, without secrets
//   rotate-sts-key   a new STS sealing key; the old one retires
//   replica-add      register a read replica (an iam_node --replica)
//
// Each talks HTTPS to iam_node's /v1/admin/* with a client certificate whose
// URI SAN is on the node's --admin-uri-san list. A follower's 307 to the
// leader is followed, and a 503 (no leader yet) is retried for --wait-secs.

#include <raft/private_file.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/json.hpp>

#include <openssl/crypto.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace ssl = boost::asio::ssl;

constexpr const char* k_usage =
    "Usage: kythira-iam [--endpoint <https://host:port>] [--cert <path>] [--key <path>]\n"
    "                   [--ca <path>] [--wait-secs <n>] <command> [options]\n"
    "\n"
    "Commands:\n"
    "  create-account --root-key-file <path> [--alias <alias>] [--account-id <12 digits>]\n"
    "  list-accounts [--json]\n"
    "  rotate-sts-key [--retire-after-secs <n>]\n"
    "  replica-add --node-id <id> --rpc-host <host> --rpc-port <port>\n"
    "  health         the node's /healthz as JSON (role, leader, term, applied)\n"
    "\n"
    "Environment: KYTHIRA_IAM_ENDPOINT, KYTHIRA_IAM_CERT, KYTHIRA_IAM_KEY and\n"
    "KYTHIRA_IAM_CA stand in for the connection flags.\n";

struct url {
    std::string host;
    std::string port;
    std::string path;  // with the query, "" for none
};

auto parse_url(const std::string& text) -> url {
    static constexpr std::string_view scheme = "https://";
    if (!text.starts_with(scheme)) {
        throw std::invalid_argument("the endpoint must be an https:// URL: " + text);
    }
    auto rest = text.substr(scheme.size());
    url u;
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        u.path = rest.substr(slash);
        rest = rest.substr(0, slash);
    }
    if (rest.starts_with('[')) {
        const auto close = rest.find(']');
        if (close == std::string::npos) {
            throw std::invalid_argument("bad IPv6 literal in " + text);
        }
        u.host = rest.substr(1, close - 1);
        u.port =
            close + 2 <= rest.size() && rest[close + 1] == ':' ? rest.substr(close + 2) : "443";
    } else if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        u.host = rest.substr(0, colon);
        u.port = rest.substr(colon + 1);
    } else {
        u.host = rest;
        u.port = "443";
    }
    if (u.host.empty() || u.port.empty()) {
        throw std::invalid_argument("bad endpoint " + text);
    }
    return u;
}

struct connection_options {
    std::string endpoint;
    std::string cert;
    std::string key;
    std::string ca;
    int wait_secs = 30;
};

struct result {
    unsigned status = 0;
    boost::json::object body;
};

/// One HTTPS request with the admin certificate, verifying the server
/// against --ca and its name.
auto send_once(const connection_options& o, const url& u, http::verb method,
               const std::string& body) -> http::response<http::string_body> {
    ssl::context ctx(ssl::context::tls_client);
    SSL_CTX_set_min_proto_version(ctx.native_handle(), TLS1_2_VERSION);
    ctx.load_verify_file(o.ca);
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.use_certificate_chain_file(o.cert);
    ctx.use_private_key_file(o.key, ssl::context::pem);
    asio::io_context ioc;
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
    if (SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str()) != 1) {
        throw std::runtime_error("cannot set the TLS server name");
    }
    stream.set_verify_callback(ssl::host_name_verification(u.host));
    asio::ip::tcp::resolver resolver(ioc);
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(stream).connect(resolver.resolve(u.host, u.port));
    stream.handshake(ssl::stream_base::client);
    http::request<http::string_body> req{method, u.path, 11};
    req.set(http::field::host, u.host);
    req.set(http::field::user_agent, "kythira-iam");
    if (!body.empty()) {
        req.set(http::field::content_type, "application/json");
        req.body() = body;
    }
    req.prepare_payload();
    http::write(stream, req);
    beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    beast::error_code ignored;
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(2));
    (void)stream.shutdown(ignored);  // the server may close first
    return res;
}

/// Sends `body` to `path` on the endpoint, following a follower's redirect
/// to the leader and retrying while no leader is known.
auto call(const connection_options& o, http::verb method, const std::string& path,
          const boost::json::object& body = {}) -> result {
    const std::string text = body.empty() ? std::string() : boost::json::serialize(body);
    auto u = parse_url(o.endpoint);
    u.path = path;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(o.wait_secs);
    int redirects = 0;
    std::string last_error;
    while (true) {
        try {
            auto res = send_once(o, u, method, text);
            const unsigned status = res.result_int();
            if (status == 307 || status == 308) {
                const auto location = std::string(res[http::field::location]);
                if (location.empty() || ++redirects > 5) {
                    throw std::runtime_error("too many redirects");
                }
                u = parse_url(location);
                continue;
            }
            boost::json::object parsed;
            boost::system::error_code ec;
            auto v = boost::json::parse(res.body(), ec);
            if (!ec && v.is_object()) {
                parsed = std::move(v.as_object());
            }
            if (status == 503 && std::chrono::steady_clock::now() < deadline) {
                last_error = res.body();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                u = parse_url(o.endpoint);
                u.path = path;
                continue;
            }
            return {status, std::move(parsed)};
        } catch (const std::exception& e) {
            // A node that is down or restarting: try again until the
            // deadline, from the configured endpoint.
            last_error = e.what();
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("cannot reach " + o.endpoint + ": " + last_error);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            u = parse_url(o.endpoint);
            u.path = path;
        }
    }
}

auto fail(const result& r) -> int {
    const auto* e = r.body.if_contains("error");
    const auto* m = r.body.if_contains("message");
    std::cerr << "kythira-iam: HTTP " << r.status;
    if (e != nullptr && e->is_string()) {
        std::cerr << " " << e->as_string().c_str();
    }
    if (m != nullptr && m->is_string()) {
        std::cerr << ": " << m->as_string().c_str();
    }
    std::cerr << "\n";
    return 1;
}

auto str(const boost::json::object& o, std::string_view k) -> std::string {
    const auto* v = o.if_contains(k);
    return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string();
}

auto create_account(const connection_options& o, const std::vector<std::string>& args) -> int {
    std::string key_file;
    boost::json::object body;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("missing value for " + args[i]);
            }
            return args[++i];
        };
        if (args[i] == "--root-key-file") {
            key_file = value();
        } else if (args[i] == "--alias") {
            body["alias"] = value();
        } else if (args[i] == "--account-id") {
            body["account_id"] = value();
        } else {
            throw std::invalid_argument("create-account: unknown option " + args[i]);
        }
    }
    if (key_file.empty()) {
        throw std::invalid_argument("create-account needs --root-key-file");
    }
    // Checked before the call: the secret exists only in the response, so a
    // failure to write it afterwards would lose the only copy.
    if (std::filesystem::exists(key_file)) {
        std::cerr << "kythira-iam: " << key_file
                  << " already exists; refusing to overwrite a root key\n";
        return 1;
    }
    if (const auto dir = std::filesystem::path(key_file).parent_path();
        !dir.empty() && !std::filesystem::is_directory(dir)) {
        std::cerr << "kythira-iam: directory " << dir << " does not exist\n";
        return 1;
    }
    auto r = call(o, http::verb::post, "/v1/admin/accounts", body);
    if (r.status != 201) {
        return fail(r);
    }
    const auto account = str(r.body, "account_id");
    const auto key_id = str(r.body, "access_key_id");
    std::string secret = str(r.body, "secret_access_key");
    std::string file = "# Kythira IAM root access key for account " + account +
                       "\n[default]\naws_access_key_id = " + key_id +
                       "\naws_secret_access_key = " + secret + "\n";
    int rc = 0;
    try {
        raft::write_private_file(key_file, file);
    } catch (const std::exception& e) {
        std::cerr << "kythira-iam: writing " << key_file << " failed: " << e.what() << "\n";
        rc = 1;
    }
    std::cout << "account_id:            " << account << "\n";
    if (const auto alias = str(r.body, "alias"); !alias.empty()) {
        std::cout << "alias:                 " << alias << "\n";
    }
    std::cout << "aws_access_key_id:     " << key_id << "\n"
              << "aws_secret_access_key: " << secret << "\n"
              << "The secret is shown once" << (rc == 0 ? " and saved to " + key_file : "")
              << ".\n";
    OPENSSL_cleanse(secret.data(), secret.size());
    OPENSSL_cleanse(file.data(), file.size());
    return rc;
}

auto list_accounts(const connection_options& o, const std::vector<std::string>& args) -> int {
    bool json = false;
    for (const auto& a : args) {
        if (a == "--json") {
            json = true;
        } else {
            throw std::invalid_argument("list-accounts: unknown option " + a);
        }
    }
    auto r = call(o, http::verb::get, "/v1/admin/accounts");
    if (r.status != 200) {
        return fail(r);
    }
    if (json) {
        std::cout << boost::json::serialize(r.body) << "\n";
        return 0;
    }
    std::cout << "ACCOUNT_ID    ALIAS                USERS  ROLES  ROOT_KEYS\n";
    for (const auto& v : r.body.at("accounts").as_array()) {
        const auto& a = v.as_object();
        auto alias = str(a, "alias");
        alias.resize(std::max<std::size_t>(alias.size(), 20), ' ');
        std::cout << str(a, "account_id") << "  " << alias << " " << a.at("users").as_int64()
                  << "      " << a.at("roles").as_int64() << "      "
                  << a.at("root_access_keys").as_int64() << "\n";
    }
    return 0;
}

auto rotate_sts_key(const connection_options& o, const std::vector<std::string>& args) -> int {
    boost::json::object body;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--retire-after-secs" && i + 1 < args.size()) {
            body["retire_after_seconds"] = std::stoll(args[++i]);
        } else {
            throw std::invalid_argument("rotate-sts-key: unknown option " + args[i]);
        }
    }
    auto r = call(o, http::verb::post, "/v1/admin/sts-keys/rotate", body);
    if (r.status != 200) {
        return fail(r);
    }
    std::cout << "new STS key: " << str(r.body, "kid") << "\n";
    for (const auto& v : r.body.at("retiring").as_array()) {
        std::cout << "retiring:    " << str(v.as_object(), "kid") << " at "
                  << v.as_object().at("retire_after").as_int64() << "\n";
    }
    return 0;
}

auto replica_add(const connection_options& o, const std::vector<std::string>& args) -> int {
    boost::json::object body;
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("missing value for " + args[i]);
            }
            return args[++i];
        };
        if (args[i] == "--node-id") {
            body["node_id"] = value();
        } else if (args[i] == "--rpc-host") {
            body["rpc_host"] = value();
        } else if (args[i] == "--rpc-port") {
            body["rpc_port"] = std::stoll(value());
        } else {
            throw std::invalid_argument("replica-add: unknown option " + args[i]);
        }
    }
    auto r = call(o, http::verb::post, "/v1/admin/replicas", body);
    if (r.status != 200) {
        return fail(r);
    }
    std::cout << "replica " << str(r.body, "node_id") << " registered and added as a learner\n";
    return 0;
}

auto health(const connection_options& o, const std::vector<std::string>& args) -> int {
    if (!args.empty()) {
        throw std::invalid_argument("health: unknown option " + args.front());
    }
    auto r = call(o, http::verb::get, "/healthz");
    if (r.status != 200) {
        return fail(r);
    }
    std::cout << boost::json::serialize(r.body) << "\n";
    return 0;
}

auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

}  // namespace

int main(int argc, char** argv) {
    connection_options o{env("KYTHIRA_IAM_ENDPOINT"), env("KYTHIRA_IAM_CERT"),
                         env("KYTHIRA_IAM_KEY"), env("KYTHIRA_IAM_CA")};
    std::string command;
    std::vector<std::string> rest;
    try {
        int i = 1;
        for (; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("missing value for " + arg);
                }
                return argv[++i];
            };
            if (arg == "--endpoint") {
                o.endpoint = value();
            } else if (arg == "--cert") {
                o.cert = value();
            } else if (arg == "--key") {
                o.key = value();
            } else if (arg == "--ca") {
                o.ca = value();
            } else if (arg == "--wait-secs") {
                o.wait_secs = std::stoi(value());
            } else if (arg == "-h" || arg == "--help") {
                std::cout << k_usage;
                return 0;
            } else if (arg.starts_with("-")) {
                throw std::invalid_argument("unknown option " + arg);
            } else {
                command = arg;
                ++i;
                break;
            }
        }
        for (; i < argc; ++i) {
            rest.emplace_back(argv[i]);
        }
        if (command.empty()) {
            throw std::invalid_argument("no command given");
        }
        if (o.endpoint.empty() || o.cert.empty() || o.key.empty() || o.ca.empty()) {
            throw std::invalid_argument(
                "--endpoint, --cert, --key and --ca (or KYTHIRA_IAM_*) are required");
        }
        if (command == "create-account") {
            return create_account(o, rest);
        }
        if (command == "list-accounts") {
            return list_accounts(o, rest);
        }
        if (command == "rotate-sts-key") {
            return rotate_sts_key(o, rest);
        }
        if (command == "replica-add") {
            return replica_add(o, rest);
        }
        if (command == "health") {
            return health(o, rest);
        }
        throw std::invalid_argument("unknown command " + command);
    } catch (const std::invalid_argument& e) {
        std::cerr << "kythira-iam: " << e.what() << "\n\n" << k_usage;
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "kythira-iam: " << e.what() << "\n";
        return 1;
    }
}
