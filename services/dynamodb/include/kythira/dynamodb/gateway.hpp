// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file gateway.hpp
/// @brief What `dynamodb_gateway_node` runs: configuration from the
///        environment, and the front end and handlers served over the core
///        HTTPS server (`.kiro/specs/dynamodb-compatible-api/` task 7).
///
/// `gateway<Backend>` owns no storage: it takes a backend, builds the
/// static-key authorizer, the front end and the handlers on it, and serves
/// them through `raft/beast_https_server.hpp`, the Beast host the IAM and
/// ledger daemons use. `GET /` answers as AWS does,
/// `healthy: dynamodb.<region>.amazonaws.com `, which SDK and load-balancer
/// health checks expect.
///
/// The daemon starts with `local_backend` on `memory_kv_engine`: one node,
/// no Raft, and nothing kept across a restart, like DynamoDB Local's
/// `-inMemory`. The Raft-hosted backend on the RocksDB engine replaces it
/// behind the same `dynamo_backend` concept.
///
/// ## Environment
///
/// | Variable | Default | Meaning |
/// |---|---|---|
/// | `KYTHIRA_DYNAMODB_BIND` | `0.0.0.0` | address to listen on, or `*` |
/// | `KYTHIRA_DYNAMODB_PORT` | `8000` | port; 0 picks a free one |
/// | `KYTHIRA_DYNAMODB_TLS_CERT`, `KYTHIRA_DYNAMODB_TLS_KEY` | | PEM chain and key; required unless
/// plaintext | | `KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT` | `0` | `1` serves plain HTTP (tests only;
/// warns at start) | | `KYTHIRA_DYNAMODB_REGION` | `us-east-1` | region in ARNs and the SigV4 scope
/// | | `KYTHIRA_DYNAMODB_PARTITION` | `aws` | partition in ARNs | |
/// `KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT` | `localhost:<port>` | what `DescribeEndpoints` answers |
/// | `KYTHIRA_DYNAMODB_STATIC_KEYS_FILE` | | access keys, one per line (below) |
/// | `KYTHIRA_DYNAMODB_STATIC_KEYS` | | the same, `,`-separated, for tests |
/// | `KYTHIRA_DYNAMODB_DATA_DIR` | | directory the disk guard watches; unset means no guard |
/// | `KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES` | 1 GiB | free space below which writes are refused |
/// | `KYTHIRA_DYNAMODB_ENGINE` | `memory` | the storage engine; only `memory` so far |
///
/// A key is `access-key-id:secret:account:name:role`, with role `read-only`,
/// `read-write` or `admin`. Blank lines and lines starting with `#` are
/// skipped. At least one key is required: the gateway never serves
/// unauthenticated requests.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/batch_api.hpp>
#include <kythira/dynamodb/disk_guard.hpp>
#include <kythira/dynamodb/dynamodb_actions.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_api.hpp>
#include <kythira/dynamodb/query_api.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/beast_https_server.hpp>
#include <raft/static_key_authorizer.hpp>

#include <boost/beast/http.hpp>

#include <atomic>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kythira::dynamodb {

struct gateway_config {
    https::server_config http{};
    front_end_config front{};
    std::vector<aws_auth::static_key> keys;
    /// Empty: no disk guard.
    std::string data_dir;
    std::uint64_t disk_floor_bytes = disk_guard::k_default_floor_bytes;
    std::string engine = "memory";
};

/// Reads one environment variable; empty optional when unset.
using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

namespace gateway_detail {

[[nodiscard]] inline auto trim(std::string_view s) -> std::string_view {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

[[nodiscard]] inline auto split(std::string_view s, char sep) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    for (;;) {
        const auto at = s.find(sep);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) {
            return out;
        }
        s.remove_prefix(at + 1);
    }
}

template<typename T> [[nodiscard]] auto number(std::string_view s) -> std::optional<T> {
    T v{};
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

}  // namespace gateway_detail

/// Parses one key, `id:secret:account:name:role`. The secret may not hold a
/// `:`, which AWS-style secrets never do. Returns the error text on failure.
[[nodiscard]] inline auto parse_static_key(std::string_view line)
    -> std::variant<aws_auth::static_key, std::string> {
    const auto parts = gateway_detail::split(line, ':');
    if (parts.size() != 5) {
        return std::string(
            "a key needs five ':'-separated fields "
            "(access-key-id:secret:account:name:role)");
    }
    aws_auth::static_key k;
    k.access_key_id = std::string(parts[0]);
    k.secret = std::string(parts[1]);
    k.account = std::string(parts[2]);
    k.name = std::string(parts[3]);
    const auto role = parts[4];
    if (role == "read-only") {
        k.role = aws_auth::static_role::read_only;
    } else if (role == "read-write") {
        k.role = aws_auth::static_role::read_write;
    } else if (role == "admin") {
        k.role = aws_auth::static_role::admin;
    } else {
        return "unknown role '" + std::string(role) + "' (read-only, read-write or admin)";
    }
    if (k.access_key_id.empty() || k.secret.empty() || k.account.empty() || k.name.empty()) {
        return std::string("a key's id, secret, account and name must not be empty");
    }
    return k;
}

/// Parses a key list: one key per line, or per `,` when `sep` is `,`.
/// Blank entries and `#` comments are skipped. Error text names the entry
/// by its 1-based number and never echoes the secret.
[[nodiscard]] inline auto parse_static_keys(std::string_view text, char sep)
    -> std::variant<std::vector<aws_auth::static_key>, std::string> {
    std::vector<aws_auth::static_key> out;
    std::size_t n = 0;
    for (const auto raw : gateway_detail::split(text, sep)) {
        ++n;
        const auto entry = gateway_detail::trim(raw);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }
        auto k = parse_static_key(entry);
        if (auto* e = std::get_if<std::string>(&k)) {
            return "static key " + std::to_string(n) + ": " + *e;
        }
        out.push_back(std::move(std::get<aws_auth::static_key>(k)));
    }
    return out;
}

/// The environment variables `gateway_config_from_env` reads, for
/// `--help`.
inline constexpr std::string_view k_gateway_env_help =
    "  KYTHIRA_DYNAMODB_BIND                 address to listen on (0.0.0.0)\n"
    "  KYTHIRA_DYNAMODB_PORT                 port (8000; 0 picks a free one)\n"
    "  KYTHIRA_DYNAMODB_TLS_CERT             PEM certificate chain, leaf first\n"
    "  KYTHIRA_DYNAMODB_TLS_KEY              PEM private key\n"
    "  KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT   1 serves plain HTTP (tests only)\n"
    "  KYTHIRA_DYNAMODB_REGION               region (us-east-1)\n"
    "  KYTHIRA_DYNAMODB_PARTITION            ARN partition (aws)\n"
    "  KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT  host:port DescribeEndpoints answers\n"
    "  KYTHIRA_DYNAMODB_STATIC_KEYS_FILE     access keys, one per line:\n"
    "                                        id:secret:account:name:role\n"
    "  KYTHIRA_DYNAMODB_STATIC_KEYS          the same, ','-separated\n"
    "  KYTHIRA_DYNAMODB_DATA_DIR             directory the disk guard watches\n"
    "  KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES     free-space floor for writes (1 GiB)\n"
    "  KYTHIRA_DYNAMODB_ENGINE               storage engine (memory)\n";

/// Builds the configuration from the environment. Returns the error text
/// on a bad or missing setting.
[[nodiscard]] inline auto gateway_config_from_env(const env_lookup& env)
    -> std::variant<gateway_config, std::string> {
    using gateway_detail::number;
    gateway_config c;
    c.http.port = 8000;
    if (auto v = env("KYTHIRA_DYNAMODB_BIND")) {
        c.http.bind_address = *v;
    }
    if (auto v = env("KYTHIRA_DYNAMODB_PORT")) {
        const auto p = number<std::uint16_t>(*v);
        if (!p) {
            return "KYTHIRA_DYNAMODB_PORT: not a port: '" + *v + "'";
        }
        c.http.port = *p;
    }
    c.http.tls_cert_path = env("KYTHIRA_DYNAMODB_TLS_CERT").value_or("");
    c.http.tls_key_path = env("KYTHIRA_DYNAMODB_TLS_KEY").value_or("");
    if (auto v = env("KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT")) {
        if (*v != "0" && *v != "1") {
            return "KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT: expected 0 or 1, got '" + *v + "'";
        }
        c.http.insecure_plaintext = *v == "1";
    }
    if (!c.http.insecure_plaintext &&
        (c.http.tls_cert_path.empty() || c.http.tls_key_path.empty())) {
        return std::string(
            "KYTHIRA_DYNAMODB_TLS_CERT and KYTHIRA_DYNAMODB_TLS_KEY are required "
            "(or KYTHIRA_DYNAMODB_INSECURE_PLAINTEXT=1 for tests)");
    }
    // DynamoDB items are up to 400 KB and a request carries one; leave
    // room for the JSON around it and for BatchWriteItem's 25 items later.
    c.http.max_body_bytes = std::uint64_t{16} * 1024 * 1024;
    c.front.region = env("KYTHIRA_DYNAMODB_REGION").value_or("us-east-1");
    c.front.partition = env("KYTHIRA_DYNAMODB_PARTITION").value_or("aws");
    c.front.advertised_endpoint = env("KYTHIRA_DYNAMODB_ADVERTISED_ENDPOINT").value_or("");

    std::string keys_error;
    if (auto path = env("KYTHIRA_DYNAMODB_STATIC_KEYS_FILE")) {
        std::ifstream in(*path);
        if (!in) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE: cannot read '" + *path + "'";
        }
        std::ostringstream text;
        text << in.rdbuf();
        auto keys = parse_static_keys(text.str(), '\n');
        if (auto* e = std::get_if<std::string>(&keys)) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS_FILE: " + *e;
        }
        c.keys = std::move(std::get<0>(keys));
    }
    if (auto text = env("KYTHIRA_DYNAMODB_STATIC_KEYS")) {
        auto keys = parse_static_keys(*text, ',');
        if (auto* e = std::get_if<std::string>(&keys)) {
            return "KYTHIRA_DYNAMODB_STATIC_KEYS: " + *e;
        }
        for (auto& k : std::get<0>(keys)) {
            c.keys.push_back(std::move(k));
        }
    }
    if (c.keys.empty()) {
        return std::string(
            "no access keys: set KYTHIRA_DYNAMODB_STATIC_KEYS_FILE or "
            "KYTHIRA_DYNAMODB_STATIC_KEYS");
    }

    c.data_dir = env("KYTHIRA_DYNAMODB_DATA_DIR").value_or("");
    if (auto v = env("KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES")) {
        const auto f = number<std::uint64_t>(*v);
        if (!f) {
            return "KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES: not a byte count: '" + *v + "'";
        }
        c.disk_floor_bytes = *f;
    }
    c.engine = env("KYTHIRA_DYNAMODB_ENGINE").value_or("memory");
    if (c.engine != "memory") {
        return "KYTHIRA_DYNAMODB_ENGINE: unknown engine '" + c.engine + "' (only 'memory' so far)";
    }
    return c;
}

/// Adapts the HTTPS server's request to the front end's.
[[nodiscard]] inline auto to_front_end_request(const https::request& req,
                                               const https::request_context& ctx)
    -> front_end_request {
    front_end_request r;
    r.method = std::string(req.method_string());
    const std::string_view target = req.target();
    const auto q = target.find('?');
    r.path = std::string(target.substr(0, q));
    if (q != std::string_view::npos) {
        r.query = std::string(target.substr(q + 1));
    }
    for (const auto& f : req) {
        r.headers.emplace_back(std::string(f.name_string()), std::string(f.value()));
    }
    r.body = req.body();
    r.source_ip = ctx.peer_address;
    r.secure = ctx.tls;
    return r;
}

/// Adapts the front end's response to the HTTPS server's.
[[nodiscard]] inline auto to_https_response(const aws_json::http_response& in, unsigned version)
    -> https::response {
    https::response res{static_cast<boost::beast::http::status>(in.status), version};
    for (const auto& [name, value] : in.headers) {
        res.set(name, value);
    }
    res.body() = in.body;
    res.prepare_payload();
    return res;
}

/// The front end and handlers over one backend, served over HTTPS.
template<dynamo_backend Backend> class gateway {
public:
    using authorizer = aws_auth::static_key_authorizer;

    /// Throws `std::invalid_argument` on a bad key table.
    gateway(gateway_config config, Backend& backend,
            std::function<void(bool, std::string_view)> log = {})
        : _config(std::move(config)),
          _auth(_config.keys, _config.front.partition),
          _guard(_config.data_dir.empty()
                     ? nullptr
                     : std::make_unique<disk_guard>(_config.data_dir, _config.disk_floor_bytes)),
          _tables(backend),
          _items(backend, _guard.get()),
          _queries(backend),
          _batches(backend, _guard.get()),
          _log(std::move(log)) {
        _auth.register_service(dynamodb_static_roles());
    }

    gateway(const gateway&) = delete;
    auto operator=(const gateway&) -> gateway& = delete;
    gateway(gateway&&) = delete;
    auto operator=(gateway&&) -> gateway& = delete;
    ~gateway() { stop(); }

    /// Binds and starts serving. Throws when the listener or TLS setup
    /// fails.
    void start() {
        _server = std::make_unique<https::server>(
            _config.http,
            [this](const https::request& req, const https::request_context& ctx) {
                return handle(req, ctx);
            },
            _log);
        _server->start();
        auto front = _config.front;
        if (front.advertised_endpoint.empty()) {
            front.advertised_endpoint = "localhost:" + std::to_string(_server->port());
        }
        _front = std::make_unique<front_end<authorizer>>(std::move(front), _auth);
        _tables.register_with(*_front);
        _items.register_with(*_front);
        _queries.register_with(*_front);
        _batches.register_with(*_front);
        _ready.store(true);
    }

    void stop() {
        if (_server) {
            _server->stop();
        }
        _ready.store(false);
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _server ? _server->port() : 0; }

    /// One request; public so tests can call it without a socket.
    auto handle(const https::request& req, const https::request_context& ctx) -> https::response {
        namespace http = boost::beast::http;
        if (req.method() == http::verb::get && req.target() == "/") {
            https::response res{http::status::ok, req.version()};
            res.set(http::field::content_type, "text/plain");
            res.body() = "healthy: dynamodb." + _config.front.region + ".amazonaws.com ";
            res.prepare_payload();
            return res;
        }
        if (!_ready.load()) {
            https::response res{http::status::service_unavailable, req.version()};
            res.prepare_payload();
            return res;
        }
        return to_https_response(_front->handle(to_front_end_request(req, ctx)), req.version());
    }

private:
    gateway_config _config;
    authorizer _auth;
    std::unique_ptr<disk_guard> _guard;
    table_api<Backend> _tables;
    item_api<Backend> _items;
    query_api<Backend> _queries;
    batch_api<Backend> _batches;
    std::function<void(bool, std::string_view)> _log;
    std::unique_ptr<front_end<authorizer>> _front;
    std::unique_ptr<https::server> _server;
    std::atomic<bool> _ready{false};
};

}  // namespace kythira::dynamodb
