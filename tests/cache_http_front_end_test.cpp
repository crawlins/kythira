// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file cache_http_front_end_test.cpp
/// @brief The cache's HTTP front end (.kiro/specs/disk-backed-build-cache/
///        task 6; Requirements 3.4, 7.1-7.8, 8.1-8.5 and 10.2): three
///        in-process `multi_raft` hosts over `cache_kv_state_machine`, each
///        with its own chunk store and a front end on a loopback port, driven
///        by a hand-written HTTP client that sends exactly the bytes a case
///        names.

#define BOOST_TEST_MODULE cache_http_front_end_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/cache_http_auth.hpp>
#include <raft/cache_http_front_end.hpp>
#include <raft/cache_kv_state_machine.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("cache_http_front_end_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::cache_http_config;
using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::redis_acl;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = fabric_server;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::cache_kv_state_machine<log_index_type>;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;
using front_end_type = kythira::cache_http_front_end<host_type, kythira::console_logger>;

constexpr std::uint32_t k_kdf_iters = 1000;
/// `sccache/...` sorts above the "m" cut, so it lives in shard 2.
constexpr group_id_type k_sccache_group = 2;

auto acl_text() -> std::string {
    return "user farm " + redis_acl::hash_secret("farm-secret", k_kdf_iters) +
           " read_write sccache/ vcpkg/\n" + "user reader " +
           redis_acl::hash_secret("reader-secret", k_kdf_iters) + " read_only sccache/ vcpkg/\n" +
           "user ops " + redis_acl::hash_secret("ops-secret", k_kdf_iters) + " admin *\n";
}

auto basic(const std::string& user, const std::string& secret) -> std::string {
    const std::string raw = user + ":" + secret;
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        const auto n = (static_cast<unsigned char>(raw[i]) << 16U) |
                       (static_cast<unsigned char>(raw[i + 1]) << 8U) |
                       static_cast<unsigned char>(raw[i + 2]);
        out += alphabet[(n >> 18U) & 63U];
        out += alphabet[(n >> 12U) & 63U];
        out += alphabet[(n >> 6U) & 63U];
        out += alphabet[n & 63U];
    }
    if (i + 1 == raw.size()) {
        const auto n = static_cast<unsigned char>(raw[i]) << 16U;
        out += alphabet[(n >> 18U) & 63U];
        out += alphabet[(n >> 12U) & 63U];
        out += "==";
    } else if (i + 2 == raw.size()) {
        const auto n = (static_cast<unsigned char>(raw[i]) << 16U) |
                       (static_cast<unsigned char>(raw[i + 1]) << 8U);
        out += alphabet[(n >> 18U) & 63U];
        out += alphabet[(n >> 12U) & 63U];
        out += alphabet[(n >> 6U) & 63U];
        out += '=';
    }
    return "Basic " + out;
}

auto farm() -> std::string {
    return basic("farm", "farm-secret");
}

auto pattern(std::size_t size, std::uint32_t seed) -> std::string {
    std::string out(size, '\0');
    std::uint32_t x = (seed * 2654435761U) + 7;
    for (auto& b : out) {
        x ^= x << 13U;
        x ^= x >> 17U;
        x ^= x << 5U;
        b = static_cast<char>(x);
    }
    return out;
}

/// A blocking loopback socket that speaks just enough HTTP/1.1.
class http_client {
public:
    explicit http_client(std::uint16_t port) : _fd(::socket(AF_INET, SOCK_STREAM, 0)) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        timeval tv{30, 0};
        ::setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (::connect(_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("connect failed");
        }
    }
    ~http_client() {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }
    http_client(const http_client&) = delete;
    auto operator=(const http_client&) -> http_client& = delete;
    http_client(http_client&&) = delete;
    auto operator=(http_client&&) -> http_client& = delete;

    auto send(std::string_view bytes) -> bool {
        while (!bytes.empty()) {
            auto n = ::send(_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
        return true;
    }
    void close() {
        ::close(_fd);
        _fd = -1;
    }

    struct response {
        int _status = 0;
        std::map<std::string, std::string> _headers;  // lower-cased names
        std::string _body;
        bool _complete = false;
    };

    /// Read one response. `head` says the request was HEAD (no body).
    auto read_response(bool head = false, bool interim = false) -> response {
        response r;
        std::string header_text;
        while (true) {
            auto end = _pending.find("\r\n\r\n");
            if (end != std::string::npos) {
                header_text = _pending.substr(0, end);
                _pending.erase(0, end + 4);
                break;
            }
            if (!fill()) {
                return r;
            }
        }
        auto first = header_text.find("\r\n");
        auto status_line = header_text.substr(0, first);
        r._status = std::stoi(status_line.substr(9, 3));
        std::size_t pos = first == std::string::npos ? header_text.size() : first + 2;
        while (pos < header_text.size()) {
            auto eol = header_text.find("\r\n", pos);
            auto line =
                header_text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = eol == std::string::npos ? header_text.size() : eol + 2;
            auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            for (auto& ch : name) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            auto value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') {
                value.erase(0, 1);
            }
            r._headers[name] = value;
        }
        if (r._status == 100) {
            if (interim) {
                return r;
            }
            return read_response(head);
        }
        if (head || r._status == 204 || r._status == 304) {
            r._complete = true;
            return r;
        }
        if (auto it = r._headers.find("content-length"); it != r._headers.end()) {
            const auto len = std::stoull(it->second);
            while (_pending.size() < len) {
                if (!fill()) {
                    r._body = std::move(_pending);
                    _pending.clear();
                    return r;
                }
            }
            r._body = _pending.substr(0, len);
            _pending.erase(0, len);
            r._complete = true;
            return r;
        }
        // Read to close.
        while (fill()) {
        }
        r._body = std::move(_pending);
        _pending.clear();
        r._complete = true;
        return r;
    }

    auto call(const std::string& method, const std::string& target, const std::string& body = {},
              const std::string& auth = farm(), const std::string& extra = {}) -> response {
        std::string req = method + " " + target + " HTTP/1.1\r\nHost: test\r\n";
        if (!auth.empty()) {
            req += "Authorization: " + auth + "\r\n";
        }
        req += extra;
        if (!body.empty() || method == "PUT") {
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "\r\n";
        req += body;
        if (!send(req)) {
            return {};
        }
        return read_response(method == "HEAD");
    }

private:
    auto fill() -> bool {
        std::array<char, 65536> buf{};
        auto n = ::recv(_fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            return false;
        }
        _pending.append(buf.data(), static_cast<std::size_t>(n));
        return true;
    }

    int _fd = -1;
    std::string _pending;
};

/// One node's chunk store and the counts over it, kept alive together.
struct node_store {
    explicit node_store(const std::filesystem::path& dir) : _store(options(dir)), _refs(_store) {}
    static auto options(const std::filesystem::path& dir) -> kythira::cache_chunk_store_options {
        kythira::cache_chunk_store_options o;
        o._data_dir = dir;
        return o;
    }
    kythira::cache_chunk_store _store;
    kythira::chunk_refcounts _refs;
};

// ── a throwaway PKI for the HTTPS case ──────────────────────────────────────

using pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

auto make_cert(const std::string& cn, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool ca)
    -> x509_ptr {
    static std::atomic<long> serial{1};
    x509_ptr cert(X509_new(), &X509_free);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial++);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -3600);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 86400);
    X509_set_pubkey(cert.get(), key);
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
    X509_set_issuer_name(cert.get(), issuer != nullptr ? X509_get_subject_name(issuer) : name);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer != nullptr ? issuer : cert.get(), cert.get(), nullptr, nullptr, 0);
    auto add = [&](int nid, const char* value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
        X509_add_ext(cert.get(), ext, -1);
        X509_EXTENSION_free(ext);
    };
    if (ca) {
        add(NID_basic_constraints, "critical,CA:TRUE");
        add(NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add(NID_basic_constraints, "critical,CA:FALSE");
        add(NID_ext_key_usage, "serverAuth");
        add(NID_subject_alt_name, "IP:127.0.0.1");
    }
    X509_sign(cert.get(), issuer_key != nullptr ? issuer_key : key, EVP_sha256());
    return cert;
}

/// A CA and one server leaf for 127.0.0.1, as PEM files.
class test_pki {
public:
    test_pki() {
        static std::atomic<int> counter{0};
        _dir =
            std::filesystem::temp_directory_path() /
            ("kythira-cache-tls-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        pkey_ptr ca_key(EVP_EC_gen("P-256"), &EVP_PKEY_free);
        auto ca = make_cert("kythira cache test CA", ca_key.get(), nullptr, nullptr, true);
        write("ca", ca.get(), ca_key.get());
        pkey_ptr key(EVP_EC_gen("P-256"), &EVP_PKEY_free);
        auto leaf = make_cert("kythira-cache-node", key.get(), ca.get(), ca_key.get(), false);
        write("node", leaf.get(), key.get());
    }
    ~test_pki() {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }
    test_pki(const test_pki&) = delete;
    auto operator=(const test_pki&) -> test_pki& = delete;
    test_pki(test_pki&&) = delete;
    auto operator=(test_pki&&) -> test_pki& = delete;

    [[nodiscard]] auto cert(const std::string& name) const -> std::string {
        return (_dir / (name + ".crt")).string();
    }
    [[nodiscard]] auto key(const std::string& name) const -> std::string {
        return (_dir / (name + ".key")).string();
    }

private:
    void write(const std::string& name, X509* cert, EVP_PKEY* key) {
        BIO* c = BIO_new_file(this->cert(name).c_str(), "w");
        BIO* k = BIO_new_file(this->key(name).c_str(), "w");
        PEM_write_bio_X509(c, cert);
        PEM_write_bio_PrivateKey(k, key, nullptr, nullptr, 0, nullptr, nullptr);
        BIO_free(c);
        BIO_free(k);
    }
    std::filesystem::path _dir;
};

/// One HTTPS request on a fresh connection, verifying the server against
/// the test CA.
auto https_call(std::uint16_t port, const std::string& ca, boost::beast::http::verb method,
                const std::string& target, const std::string& body = {})
    -> boost::beast::http::response<boost::beast::http::string_body> {
    namespace ssl = boost::asio::ssl;
    namespace http = boost::beast::http;
    boost::asio::io_context ioc;
    ssl::context ctx(ssl::context::tls_client);
    ctx.load_verify_file(ca);
    ctx.set_verify_mode(ssl::verify_peer);
    ssl::stream<boost::asio::ip::tcp::socket> stream(ioc, ctx);
    stream.set_verify_callback(ssl::host_name_verification("127.0.0.1"));
    stream.next_layer().connect({boost::asio::ip::make_address("127.0.0.1"), port});
    stream.handshake(ssl::stream_base::client);
    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::authorization, farm());
    req.body() = body;
    req.prepare_payload();
    http::write(stream, req);
    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    http::read(stream, buf, res);
    return res;
}

class cluster {
public:
    explicit cluster(std::size_t nodes = 3, const cache_http_config& base = {},
                     std::size_t snapshot_threshold = 0)
        : _nodes(nodes) {
        static std::atomic<int> counter{0};
        _root = std::filesystem::temp_directory_path() /
                ("kythira-cache-http-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::remove_all(_root);
        _acl.reload(acl_text());
        std::vector<node_id_t> voters;
        for (node_id_t id = 1; id <= _nodes; ++id) {
            voters.push_back(id);
        }
        for (node_id_t id = 1; id <= _nodes; ++id) {
            auto owner = std::make_shared<node_store>(_root / ("node-" + std::to_string(id)));
            _stores.emplace_back(owner, &owner->_refs);
            _hosts.push_back(std::make_unique<host_type>(make_config(id, snapshot_threshold)));
            for (const auto& shard : shards(voters)) {
                _hosts.back()->create_group(shard);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        for (std::size_t i = 0; i < _nodes; ++i) {
            auto cfg = base;
            if (base._tls_listen.empty()) {
                cfg._listen = "127.0.0.1:0";
                cfg._allow_plaintext = true;
                cfg._allow_plaintext_forwarding = true;
            }
            cfg._internal_secret = "internal-secret";
            cfg._command_timeout = std::chrono::milliseconds{5000};
            if (base._min_free_bytes == cache_http_config{}._min_free_bytes) {
                cfg._min_free_bytes = 0;  // the sandbox's disk is not the subject here
                cfg._min_free_inodes = 0;
            }
            auto resolver = [this](const node_id_t& to) -> std::optional<std::string> {
                if (to < 1 || to > _front_ends.size() || !_front_ends[to - 1]) {
                    return std::nullopt;
                }
                const auto& fe = *_front_ends[to - 1];
                return "127.0.0.1:" +
                       std::to_string(fe.config()._forward_tls ? fe.tls_port() : fe.port());
            };
            _front_ends.push_back(std::make_unique<front_end_type>(*_hosts[i], _acl, _logger,
                                                                   _stores[i], cfg, resolver));
        }
        for (auto& f : _front_ends) {
            f->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _nodes; ++i) {
            _drivers.emplace_back([this, i] {
                while (_running.load()) {
                    _hosts[i]->tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            });
        }
    }

    ~cluster() {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        for (auto& f : _front_ends) {
            f->stop();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
        _front_ends.clear();
        _hosts.clear();
        _stores.clear();
        std::error_code ec;
        std::filesystem::remove_all(_root, ec);
    }
    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;
    cluster(cluster&&) = delete;
    auto operator=(cluster&&) -> cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto front_end(node_id_t id) -> front_end_type& {
        return *_front_ends.at(id - 1);
    }
    [[nodiscard]] auto port(node_id_t id) -> std::uint16_t { return front_end(id).port(); }
    [[nodiscard]] auto acl() -> redis_acl& { return _acl; }

    [[nodiscard]] auto leader_of(group_id_type group) -> node_id_t {
        for (node_id_t id = 1; id <= _nodes; ++id) {
            auto* n = _hosts[id - 1]->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return 0;
    }
    [[nodiscard]] auto a_follower_of(group_id_type group) -> node_id_t {
        auto leader = leader_of(group);
        for (node_id_t id = 1; id <= _nodes; ++id) {
            if (id != leader) {
                return id;
            }
        }
        return 0;
    }

    auto await_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (leader_of(1) != 0 && leader_of(2) != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// Wait until `id` holds `key` (replication is asynchronous).
    auto await_key(node_id_t id, group_id_type group, const std::string& key) -> bool {
        for (int i = 0; i < 200; ++i) {
            auto m = host(id).group_node(group)->with_state_machine(
                [&key](auto& sm) { return sm.lookup(key); });
            if (m && m->has_all()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

private:
    static auto shards(const std::vector<node_id_t>& voters) -> std::vector<descriptor_type> {
        return {
            descriptor_type{
                ._group_id = 1,
                ._range = shard_range<key_type>{._start = std::nullopt, ._end = key_type{"m"}},
                ._epoch = shard_epoch{},
                ._voters = voters},
            descriptor_type{
                ._group_id = 2,
                ._range = shard_range<key_type>{._start = key_type{"m"}, ._end = std::nullopt},
                ._epoch = shard_epoch{},
                ._voters = voters},
        };
    }

    auto make_config(node_id_t id, std::size_t snapshot_threshold) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
            .on_group_destroyed = {},
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        if (snapshot_threshold != 0) {
            cfg.config._snapshot_threshold_bytes = snapshot_threshold;
        }
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
        auto refs = _stores[id - 1];
        cfg.state_machine_factory = [refs](const group_id_type&) {
            return host_types::state_machine_type(refs);
        };
        return cfg;
    }

    std::size_t _nodes;
    std::filesystem::path _root;
    message_fabric _fabric{8};
    redis_acl _acl;
    kythira::console_logger _logger{kythira::log_level::error};
    std::vector<std::shared_ptr<kythira::chunk_refcounts>> _stores;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::unique_ptr<front_end_type>> _front_ends;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

auto small_chunks() -> cache_http_config {
    cache_http_config cfg;
    cfg._chunk_bytes = 64 * 1024;
    return cfg;
}

auto peak_rss_bytes() -> std::uint64_t {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.starts_with("VmHWM:")) {
            return std::stoull(line.substr(6)) * 1024;
        }
    }
    return 0;
}

void reset_peak_rss() {
    std::ofstream clear("/proc/self/clear_refs");
    clear << "5";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(cache_http_front_end_tests)

BOOST_AUTO_TEST_CASE(parse_authorization_and_ranges, *boost::unit_test::timeout(10)) {
    using kythira::cache_http_credentials;
    using kythira::parse_cache_http_authorization;
    auto b = parse_cache_http_authorization(farm());
    BOOST_REQUIRE(b);
    BOOST_CHECK(b->_scheme == cache_http_credentials::scheme::basic);
    BOOST_CHECK_EQUAL(b->_user, "farm");
    BOOST_CHECK_EQUAL(b->_secret, "farm-secret");
    auto t = parse_cache_http_authorization("bearer   tok:en");
    BOOST_REQUIRE(t);
    BOOST_CHECK(t->_scheme == cache_http_credentials::scheme::bearer);
    BOOST_CHECK_EQUAL(t->_secret, "tok:en");
    BOOST_CHECK(!parse_cache_http_authorization(""));
    BOOST_CHECK(!parse_cache_http_authorization("Basic"));
    BOOST_CHECK(!parse_cache_http_authorization("Basic !!!!"));
    BOOST_CHECK(!parse_cache_http_authorization("Digest abc"));

    using kythira::cache_http_detail::byte_range;
    using kythira::cache_http_detail::parse_range;
    auto r = parse_range("bytes=10-19", 100);
    BOOST_CHECK(r._kind == byte_range::kind::partial && r._first == 10 && r._last == 19);
    r = parse_range("bytes=90-", 100);
    BOOST_CHECK(r._kind == byte_range::kind::partial && r._first == 90 && r._last == 99);
    r = parse_range("bytes=-5", 100);
    BOOST_CHECK(r._kind == byte_range::kind::partial && r._first == 95 && r._last == 99);
    r = parse_range("bytes=50-500", 100);
    BOOST_CHECK(r._kind == byte_range::kind::partial && r._first == 50 && r._last == 99);
    BOOST_CHECK(parse_range("bytes=100-", 100)._kind == byte_range::kind::unsatisfiable);
    BOOST_CHECK(parse_range("bytes=0-1,5-6", 100)._kind == byte_range::kind::whole);
    BOOST_CHECK(parse_range("lines=1-2", 100)._kind == byte_range::kind::whole);
    BOOST_CHECK(parse_range("bytes=9-3", 100)._kind == byte_range::kind::whole);

    auto p = kythira::cache_http_detail::decode_path("/sccache/a%2Fb%20c?x=1");
    BOOST_REQUIRE(p);
    BOOST_CHECK_EQUAL(*p, "/sccache/a/b c");
    BOOST_CHECK(!kythira::cache_http_detail::decode_path("/bad%2"));
    BOOST_CHECK(!kythira::cache_http_detail::decode_path("http://x/y"));
}

// Requirement 8.4: a success is cached under a keyed hash, a reload empties
// the cache, failures are never cached, and the bound holds.
BOOST_AUTO_TEST_CASE(auth_cache_hits_flushes_and_bounds, *boost::unit_test::timeout(30)) {
    redis_acl acl;
    acl.reload(acl_text());
    kythira::cache_http_auth_cache cache(std::chrono::seconds{300}, 2);
    int kdf_runs = 0;
    auto check = [&](const std::string& user, const std::string& secret) {
        auto creds = kythira::parse_cache_http_authorization(basic(user, secret));
        return cache.authenticate(acl, *creds, [&] {
            ++kdf_runs;
            return acl.authenticate(user, secret);
        });
    };
    BOOST_CHECK(check("farm", "farm-secret"));
    BOOST_CHECK(check("farm", "farm-secret"));
    BOOST_CHECK_EQUAL(kdf_runs, 1);
    BOOST_CHECK_EQUAL(cache.hits(), 1U);
    BOOST_CHECK(!check("farm", "wrong"));
    BOOST_CHECK(!check("farm", "wrong"));
    BOOST_CHECK_EQUAL(kdf_runs, 3);  // failures are not cached
    BOOST_CHECK(check("reader", "reader-secret"));
    BOOST_CHECK(check("ops", "ops-secret"));
    BOOST_CHECK_LE(cache.size(), 2U);

    // A reload that revokes `farm` takes effect at once, not after the TTL.
    acl.reload("user reader " + redis_acl::hash_secret("reader-secret", k_kdf_iters) +
               " read_only sccache/\n");
    BOOST_CHECK(!check("farm", "farm-secret"));
    BOOST_CHECK_EQUAL(cache.flushes(), 1U);

    // Expiry.
    kythira::cache_http_auth_cache brief(std::chrono::seconds{1}, 8);
    auto creds = kythira::parse_cache_http_authorization(basic("reader", "reader-secret"));
    const auto t0 = kythira::cache_http_auth_cache::clock::now();
    int runs = 0;
    auto verify = [&] {
        ++runs;
        return acl.authenticate("reader", "reader-secret");
    };
    BOOST_CHECK(brief.authenticate(acl, *creds, verify, t0));
    BOOST_CHECK(brief.authenticate(acl, *creds, verify, t0 + std::chrono::milliseconds{500}));
    BOOST_CHECK(brief.authenticate(acl, *creds, verify, t0 + std::chrono::seconds{2}));
    BOOST_CHECK_EQUAL(runs, 2);

    // Bearer: the token is matched against each user's record.
    auto id = kythira::cache_http_auth_cache::verify_bearer(acl, "reader-secret");
    BOOST_REQUIRE(id);
    BOOST_CHECK_EQUAL(id->_user, "reader");
    BOOST_CHECK(!kythira::cache_http_auth_cache::verify_bearer(acl, "nope"));
}

BOOST_AUTO_TEST_CASE(put_get_head_range_and_miss, *boost::unit_test::timeout(120)) {
    cluster c(3, small_chunks());
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    http_client client(c.port(leader));

    const auto value = pattern(300 * 1024 + 17, 1);  // five chunks, the last short
    auto put = client.call("PUT", "/sccache/8/5/6/abc", value);
    BOOST_CHECK_EQUAL(put._status, 201);

    auto get = client.call("GET", "/sccache/8/5/6/abc");
    BOOST_CHECK_EQUAL(get._status, 200);
    BOOST_CHECK(get._body == value);
    const auto etag = get._headers["etag"];
    BOOST_CHECK_EQUAL(etag.size(), 66U);

    auto head = client.call("HEAD", "/sccache/8/5/6/abc");
    BOOST_CHECK_EQUAL(head._status, 200);
    BOOST_CHECK_EQUAL(head._headers["content-length"], std::to_string(value.size()));
    BOOST_CHECK_EQUAL(head._headers["etag"], etag);

    // A range across a chunk boundary.
    auto part =
        client.call("GET", "/sccache/8/5/6/abc", {}, farm(), "Range: bytes=65530-131080\r\n");
    BOOST_CHECK_EQUAL(part._status, 206);
    BOOST_CHECK(part._body == value.substr(65530, 131080 - 65530 + 1));
    BOOST_CHECK_EQUAL(part._headers["content-range"],
                      "bytes 65530-131080/" + std::to_string(value.size()));
    auto tail = client.call("GET", "/sccache/8/5/6/abc", {}, farm(), "Range: bytes=-10\r\n");
    BOOST_CHECK_EQUAL(tail._status, 206);
    BOOST_CHECK(tail._body == value.substr(value.size() - 10));
    auto beyond =
        client.call("GET", "/sccache/8/5/6/abc", {}, farm(), "Range: bytes=999999999-\r\n");
    BOOST_CHECK_EQUAL(beyond._status, 416);

    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/8/5/6/nope")._status, 404);
    BOOST_CHECK_EQUAL(client.call("HEAD", "/sccache/8/5/6/nope")._status, 404);

    // An empty value, and a replacement.
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/empty", "")._status, 201);
    auto empty = client.call("GET", "/sccache/empty");
    BOOST_CHECK_EQUAL(empty._status, 200);
    BOOST_CHECK(empty._body.empty());
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/8/5/6/abc", "v2")._status, 201);
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/8/5/6/abc")._body, "v2");

    // DELETE.
    BOOST_CHECK_EQUAL(client.call("DELETE", "/sccache/8/5/6/abc")._status, 204);
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/8/5/6/abc")._status, 404);
    BOOST_CHECK_EQUAL(client.call("DELETE", "/sccache/8/5/6/abc")._status, 404);
}

BOOST_AUTO_TEST_CASE(chunked_transfer_encoding_and_expect, *boost::unit_test::timeout(120)) {
    cluster c(3, small_chunks());
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    http_client client(c.port(c.leader_of(k_sccache_group)));
    const auto value = pattern(150 * 1024 + 3, 2);
    std::string req = "PUT /vcpkg/zlib/1.3.1/abc HTTP/1.1\r\nHost: t\r\nAuthorization: " + farm() +
                      "\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n";
    BOOST_REQUIRE(client.send(req));
    // curl waits for the interim answer before it sends a large body.
    BOOST_CHECK_EQUAL(client.read_response(false, true)._status, 100);
    for (std::size_t off = 0; off < value.size(); off += 10000) {
        const auto piece = value.substr(off, 10000);
        std::ostringstream hex;
        hex << std::hex << piece.size();
        BOOST_REQUIRE(client.send(hex.str() + "\r\n" + piece + "\r\n"));
    }
    BOOST_REQUIRE(client.send("0\r\n\r\n"));
    BOOST_CHECK_EQUAL(client.read_response()._status, 201);
    auto got = client.call("GET", "/vcpkg/zlib/1.3.1/abc");
    BOOST_CHECK_EQUAL(got._status, 200);
    BOOST_CHECK(got._body == value);
}

// Requirement 8: no credentials, bad credentials, a read-only user's write
// (403, and nothing reaches the log), and a key outside the user's prefixes.
BOOST_AUTO_TEST_CASE(authentication_and_authorization, *boost::unit_test::timeout(120)) {
    cluster c(3);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    http_client client(c.port(leader));
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/k", "v")._status, 201);

    http_client anon(c.port(leader));
    auto r = anon.call("GET", "/sccache/k", {}, "");
    BOOST_CHECK_EQUAL(r._status, 401);
    BOOST_CHECK(r._headers.contains("www-authenticate"));
    http_client wrong(c.port(leader));
    BOOST_CHECK_EQUAL(wrong.call("GET", "/sccache/k", {}, basic("farm", "nope"))._status, 401);

    const auto reader = basic("reader", "reader-secret");
    http_client ro(c.port(leader));
    BOOST_CHECK_EQUAL(ro.call("GET", "/sccache/k", {}, reader)._body, "v");
    auto* node = c.host(leader).group_node(k_sccache_group);
    const auto log_before = node->last_log_index();
    BOOST_CHECK_EQUAL(ro.call("PUT", "/sccache/k", "poison", reader)._status, 403);
    BOOST_CHECK_EQUAL(ro.call("DELETE", "/sccache/k", {}, reader)._status, 403);
    BOOST_CHECK_EQUAL(ro.call("MKCOL", "/sccache/d/", {}, reader)._status, 403);
    BOOST_CHECK_EQUAL(ro.call("PROPFIND", "/sccache/d/", {}, reader, "Depth: 0\r\n")._status, 403);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_CHECK_EQUAL(node->last_log_index(), log_before);
    BOOST_CHECK_EQUAL(ro.call("GET", "/sccache/k", {}, reader)._body, "v");

    // Outside the user's prefixes.
    BOOST_CHECK_EQUAL(client.call("PUT", "/other/k", "v")._status, 403);
    BOOST_CHECK_EQUAL(client.call("GET", "/other/k")._status, 403);
    // The admin route needs admin.
    BOOST_CHECK_EQUAL(client.call("GET", "/_metrics")._status, 403);
    auto metrics = client.call("GET", "/_metrics", {}, basic("ops", "ops-secret"));
    BOOST_CHECK_EQUAL(metrics._status, 200);
    BOOST_CHECK(metrics._body.find("kythira_cache_http_puts 1") != std::string::npos);
    // The chunk route takes only the internal secret.
    BOOST_CHECK_EQUAL(client.call("GET", "/_chunk/" + std::string(64, '0'))._status, 403);
    BOOST_CHECK_EQUAL(
        client.call("GET", "/_chunk/" + std::string(64, '0'), {}, "Bearer internal-secret")._status,
        404);
    // ...and the internal secret opens nothing else.
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/k", {}, "Bearer internal-secret")._status, 403);
    // Bearer with a user's secret works.
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/k", {}, "Bearer reader-secret")._body, "v");
    // Repeated requests hit the auth cache rather than the KDF.
    BOOST_CHECK_GE(c.front_end(leader).auth_cache().hits(), 3U);
}

BOOST_AUTO_TEST_CASE(over_the_maximum_is_413, *boost::unit_test::timeout(120)) {
    auto cfg = small_chunks();
    cfg._max_value_bytes = 100000;
    cluster c(3, cfg);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    {
        // Refused on the declared length, before any body is read.
        http_client client(c.port(leader));
        std::string req = "PUT /sccache/big HTTP/1.1\r\nHost: t\r\nAuthorization: " + farm() +
                          "\r\nContent-Length: 100001\r\n\r\n";
        BOOST_REQUIRE(client.send(req));
        BOOST_CHECK_EQUAL(client.read_response()._status, 413);
    }
    {
        // A chunked body that grows past the limit.
        http_client client(c.port(leader));
        std::string req = "PUT /sccache/big HTTP/1.1\r\nHost: t\r\nAuthorization: " + farm() +
                          "\r\nTransfer-Encoding: chunked\r\n\r\n";
        BOOST_REQUIRE(client.send(req));
        const auto piece = pattern(50000, 3);
        for (int i = 0; i < 3; ++i) {
            std::ostringstream hex;
            hex << std::hex << piece.size();
            (void)client.send(hex.str() + "\r\n" + piece + "\r\n");
        }
        (void)client.send("0\r\n\r\n");
        BOOST_CHECK_EQUAL(client.read_response()._status, 413);
    }
    http_client client(c.port(leader));
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/big")._status, 404);
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/big", pattern(100000, 4))._status, 201);
    BOOST_CHECK_GE(c.front_end(leader).stats()._too_large.load(), 2U);
}

// Requirement 7.5: a client that disconnects mid-body leaves the old value.
BOOST_AUTO_TEST_CASE(a_disconnect_mid_put_keeps_the_old_value, *boost::unit_test::timeout(120)) {
    cluster c(3, small_chunks());
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    http_client client(c.port(leader));
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/k", "old")._status, 201);
    {
        http_client quitter(c.port(leader));
        std::string req = "PUT /sccache/k HTTP/1.1\r\nHost: t\r\nAuthorization: " + farm() +
                          "\r\nContent-Length: 1000000\r\n\r\n";
        BOOST_REQUIRE(quitter.send(req + pattern(300000, 5)));
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        quitter.close();
    }
    for (int i = 0; i < 100 && c.front_end(leader).stats()._aborted_puts.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_EQUAL(c.front_end(leader).stats()._aborted_puts.load(), 1U);
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/k")._body, "old");
}

// Requirement 7.8: writes on a follower are relayed to the leader; reads are
// answered locally once the chunks are here.
BOOST_AUTO_TEST_CASE(a_follower_relays_writes_and_serves_reads, *boost::unit_test::timeout(120)) {
    cluster c(3, small_chunks());
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto follower = c.a_follower_of(k_sccache_group);
    http_client client(c.port(follower));
    const auto value = pattern(200 * 1024, 6);
    auto put = client.call("PUT", "/sccache/far", value);
    BOOST_CHECK_EQUAL(put._status, 201);
    BOOST_CHECK_GE(c.front_end(follower).stats()._forwards.load(), 1U);
    BOOST_REQUIRE(c.await_key(follower, k_sccache_group, "sccache/far"));
    const auto forwards = c.front_end(follower).stats()._forwards.load();
    http_client again(c.port(follower));
    auto get = again.call("GET", "/sccache/far");
    BOOST_CHECK_EQUAL(get._status, 200);
    BOOST_CHECK(get._body == value);
    BOOST_CHECK_EQUAL(c.front_end(follower).stats()._forwards.load(), forwards);
    http_client del(c.port(follower));
    BOOST_CHECK_EQUAL(del.call("DELETE", "/sccache/far")._status, 204);
}

// sccache's write path (tasks.md task 1): PROPFIND on the parent collection
// must be a full multistatus, MKCOL is a no-op.
BOOST_AUTO_TEST_CASE(webdav_propfind_and_mkcol, *boost::unit_test::timeout(120)) {
    cluster c(3);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    http_client client(c.port(c.leader_of(k_sccache_group)));
    const std::string body = R"(<D:propfind xmlns:D="DAV:"><D:allprop/></D:propfind>)";
    auto dir = client.call("PROPFIND", "/sccache/8/5/6/", body, farm(), "Depth: 0\r\n");
    BOOST_CHECK_EQUAL(dir._status, 207);
    BOOST_CHECK(dir._body.find("<D:collection/>") != std::string::npos);
    BOOST_CHECK(dir._body.find("<D:href>/sccache/8/5/6/</D:href>") != std::string::npos);
    BOOST_CHECK(dir._body.find("<D:getlastmodified>") != std::string::npos);
    BOOST_CHECK(dir._body.find("<D:displayname>6</D:displayname>") != std::string::npos);
    BOOST_CHECK_EQUAL(client.call("MKCOL", "/sccache/8/5/6/")._status, 201);
    BOOST_CHECK_EQUAL(
        client.call("PROPFIND", "/sccache/8/5/6/f", body, farm(), "Depth: 0\r\n")._status, 404);
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/8/5/6/f", "12345")._status, 201);
    auto file = client.call("PROPFIND", "/sccache/8/5/6/f", body, farm(), "Depth: 0\r\n");
    BOOST_CHECK_EQUAL(file._status, 207);
    BOOST_CHECK(file._body.find("<D:getcontentlength>5</D:getcontentlength>") != std::string::npos);
    BOOST_CHECK(file._body.find("<D:getetag>") != std::string::npos);
    BOOST_CHECK(file._body.find("<D:resourcetype/>") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_chunk_route_serves_peers, *boost::unit_test::timeout(120)) {
    cluster c(3);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    http_client client(c.port(leader));
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/k", "chunk bytes")._status, 201);
    std::vector<std::byte> bytes;
    for (char ch : std::string("chunk bytes")) {
        bytes.push_back(static_cast<std::byte>(ch));
    }
    const auto hex = kythira::to_hex(kythira::sha256_of(bytes));
    auto r = client.call("GET", "/_chunk/" + hex, {}, "Bearer internal-secret");
    BOOST_CHECK_EQUAL(r._status, 200);
    BOOST_CHECK_EQUAL(r._body, "chunk bytes");
    BOOST_CHECK_EQUAL(client.call("GET", "/_chunk/xyz", {}, "Bearer internal-secret")._status, 400);
}

// Requirement 8.5.
BOOST_AUTO_TEST_CASE(plaintext_needs_the_opt_in, *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        .on_group_destroyed = {},
    };
    cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
    auto dir = std::filesystem::temp_directory_path() /
               ("kythira-cache-http-optin-" + std::to_string(::getpid()));
    auto owner = std::make_shared<node_store>(dir);
    std::shared_ptr<kythira::chunk_refcounts> refs(owner, &owner->_refs);
    cfg.state_machine_factory = [refs](const group_id_type&) {
        return host_types::state_machine_type(refs);
    };
    host_type host(std::move(cfg));
    redis_acl acl;
    acl.reload(acl_text());
    kythira::console_logger logger{kythira::log_level::error};
    cache_http_config hc;
    hc._listen = "127.0.0.1:0";
    {
        front_end_type fe(host, acl, logger, refs, hc, nullptr);
        BOOST_CHECK_THROW(fe.start(), std::runtime_error);
    }
    hc._allow_plaintext = true;
    {
        front_end_type fe(host, acl, logger, refs, hc, nullptr);
        BOOST_CHECK_NO_THROW(fe.start());
        BOOST_CHECK_NE(fe.port(), 0);
        fe.stop();
    }
    owner.reset();
    refs.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// HTTPS on the listener and TLS on the relay between front ends.
BOOST_AUTO_TEST_CASE(https_and_tls_forwarding, *boost::unit_test::timeout(120)) {
    namespace http = boost::beast::http;
    test_pki pki;
    cache_http_config cfg;
    cfg._tls_listen = "127.0.0.1:0";
    cfg._tls_cert_path = pki.cert("node");
    cfg._tls_key_path = pki.key("node");
    cfg._forward_tls = true;
    cfg._forward_tls_ca_path = pki.cert("ca");
    cluster c(3, cfg);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto follower = c.a_follower_of(k_sccache_group);
    BOOST_CHECK_EQUAL(c.port(follower), 0);  // no plaintext listener
    const auto tls_port = c.front_end(follower).tls_port();
    auto put = https_call(tls_port, pki.cert("ca"), http::verb::put, "/sccache/tls", "secret");
    BOOST_CHECK_EQUAL(put.result_int(), 201);
    BOOST_CHECK_GE(c.front_end(follower).stats()._forwards.load(), 1U);
    BOOST_CHECK_EQUAL(c.front_end(follower).stats()._forward_failures.load(), 0U);
    const auto leader = c.leader_of(k_sccache_group);
    auto get =
        https_call(c.front_end(leader).tls_port(), pki.cert("ca"), http::verb::get, "/sccache/tls");
    BOOST_CHECK_EQUAL(get.result_int(), 200);
    BOOST_CHECK_EQUAL(get.body(), "secret");
}

BOOST_AUTO_TEST_CASE(a_full_disk_refuses_puts_with_507, *boost::unit_test::timeout(120)) {
    cache_http_config cfg;
    cfg._min_free_bytes = std::numeric_limits<std::uint64_t>::max();
    cluster c(3, cfg);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    http_client client(c.port(leader));
    BOOST_CHECK_EQUAL(client.call("PUT", "/sccache/k", "v")._status, 507);
    BOOST_CHECK_EQUAL(client.call("GET", "/sccache/k")._status, 404);  // reads continue
}

// Requirement 7.4: a 1 GiB PUT is streamed, never buffered. One node so the
// process holds one Raft log, compacted every 8 MB.
BOOST_AUTO_TEST_CASE(a_1_gib_put_stays_under_64_mib_of_peak_rss, *boost::unit_test::timeout(600)) {
    cache_http_config cfg;
    cfg._max_value_bytes = std::uint64_t{2} * 1024 * 1024 * 1024;
    cluster c(1, cfg, 8 * 1000 * 1000);
    BOOST_REQUIRE(c.await_leaders(std::chrono::seconds{20}));
    http_client client(c.port(1));
    constexpr std::uint64_t total = std::uint64_t{1} << 30U;
    const auto block = pattern(1 << 20, 7);
    // Warm up allocator arenas with a small PUT first.
    BOOST_CHECK_EQUAL(client.call("PUT", "/vcpkg/warm", pattern(8 << 20, 8))._status, 201);
    reset_peak_rss();
    const auto baseline = peak_rss_bytes();
    std::string req = "PUT /vcpkg/huge HTTP/1.1\r\nHost: t\r\nAuthorization: " + farm() +
                      "\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n";
    BOOST_REQUIRE(client.send(req));
    for (std::uint64_t sent = 0; sent < total; sent += block.size()) {
        BOOST_REQUIRE(client.send(block));
    }
    auto res = client.read_response();
    BOOST_CHECK_EQUAL(res._status, 201);
    const auto peak = peak_rss_bytes();
    BOOST_TEST_MESSAGE("peak RSS above baseline: " << (peak - baseline) / (1024 * 1024) << " MiB");
    BOOST_CHECK_LT(peak - baseline, std::uint64_t{64} * 1024 * 1024);
    auto head = client.call("HEAD", "/vcpkg/huge");
    BOOST_CHECK_EQUAL(head._status, 200);
    BOOST_CHECK_EQUAL(head._headers["content-length"], std::to_string(total));
}

BOOST_AUTO_TEST_SUITE_END()
