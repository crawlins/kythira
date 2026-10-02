// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_gateway_integration_test.cpp
/// @brief Three in-process `multi_raft` hosts, each with a `redis_gateway` on
///        a real TCP port, driven by a hand-rolled RESP client
///        (.kiro/specs/redis-compatible-kv/ tasks 5-9 and 11).
///
/// The TLS cases mint a throwaway CA and certificates per test with OpenSSL,
/// so they need no fixtures on disk and no network beyond loopback.
///
/// The client is deliberately not a Redis library: it sends exactly the bytes
/// the test names and compares exactly the bytes that come back, so a reply
/// that redis-rs would tolerate but the spec forbids still fails. The real
/// sccache acceptance lives under tests/docker_chaos/sccache_e2e/.

#define BOOST_TEST_MODULE redis_gateway_integration_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/redis_gateway_impl.hpp>
#include <raft/redis_kv_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("redis_gateway_integration_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::redis_acl;
using kythira::redis_gateway;
using kythira::redis_gateway_config;
using kythira::redis_read_consistency;
using kythira::resp_reply_length;
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
    using state_machine_type = kythira::redis_kv_state_machine<log_index_type>;

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
using gateway_type = redis_gateway<host_type, kythira::console_logger, kythira::noop_metrics>;

constexpr std::size_t k_node_count = 3;
constexpr std::size_t k_shard_count = 2;
constexpr std::uint32_t k_kdf_iters = 1000;
/// Every `sccache/...` key sorts above the "m" cut, so it lives in shard 2.
/// Tests that inspect a shard's node directly must look there, not in shard 1.
constexpr group_id_type k_sccache_group = 2;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

/// Two shards split at "m", every one replicated on all three nodes.
auto static_shards() -> std::vector<descriptor_type> {
    const std::vector<node_id_t> voters{1, 2, 3};
    return {
        descriptor_type{._group_id = 1,
                        ._range = range_of(std::nullopt, key_type{"m"}),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
        descriptor_type{._group_id = 2,
                        ._range = range_of(key_type{"m"}, std::nullopt),
                        ._epoch = shard_epoch{},
                        ._voters = voters},
    };
}

auto acl_text() -> std::string {
    return "user farm " + redis_acl::hash_secret("farm-secret", k_kdf_iters) +
           " read_write sccache/\n" + "user reader " +
           redis_acl::hash_secret("reader-secret", k_kdf_iters) + " read_only sccache/\n" +
           "user ops " + redis_acl::hash_secret("ops-secret", k_kdf_iters) + " admin *\n" +
           "user kythira-internal " + redis_acl::hash_secret("internal-secret", k_kdf_iters) +
           " read_write *\n" +
           // Task 9: client certificates map to users by subject.
           "user certfarm " + redis_acl::hash_secret("certfarm-secret", k_kdf_iters) +
           " read_write sccache/ cert=CN=farm\n" +
           "user gone disabled read_write sccache/ cert=CN=gone\n";
}

// ── a throwaway PKI for the TLS cases ────────────────────────────────────────

using pkey_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using x509_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

auto make_key() -> pkey_ptr {
    pkey_ptr key(EVP_EC_gen("P-256"), &EVP_PKEY_free);
    if (!key) {
        throw std::runtime_error("EVP_EC_gen failed");
    }
    return key;
}

/// A certificate for `cn` signed by `issuer` (self-signed when null). A CA
/// gets the CA basic constraint; a leaf gets server and client key usage and
/// `san` (an OpenSSL subjectAltName value, e.g. "IP:127.0.0.1") if non-empty.
auto make_cert(const std::string& cn, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key, bool ca,
               const std::string& san) -> x509_ptr {
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
    auto add = [&](int nid, const std::string& value) {
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
        if (ext == nullptr) {
            throw std::runtime_error("bad certificate extension " + value);
        }
        X509_add_ext(cert.get(), ext, -1);
        X509_EXTENSION_free(ext);
    };
    if (ca) {
        add(NID_basic_constraints, "critical,CA:TRUE");
        add(NID_key_usage, "critical,keyCertSign,cRLSign");
    } else {
        add(NID_basic_constraints, "critical,CA:FALSE");
        add(NID_ext_key_usage, "serverAuth,clientAuth");
        if (!san.empty()) {
            add(NID_subject_alt_name, san);
        }
    }
    if (X509_sign(cert.get(), issuer_key != nullptr ? issuer_key : key, EVP_sha256()) == 0) {
        throw std::runtime_error("X509_sign failed");
    }
    return cert;
}

/// PEM files under a private temporary directory, removed on destruction:
/// `ca` and `rogue-ca`, and leaves signed by `ca` — `node` (SAN IP 127.0.0.1,
/// what every gateway presents), `wrong-host` (SAN DNS kv.invalid only), and
/// client certificates `farm`, `gone` and `stranger`.
class test_pki {
public:
    test_pki() {
        static std::atomic<int> counter{0};
        _dir =
            std::filesystem::temp_directory_path() /
            ("kythira-redis-tls-" + std::to_string(::getpid()) + "-" + std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        auto ca_key = make_key();
        auto ca = make_cert("kythira test CA", ca_key.get(), nullptr, nullptr, true, "");
        write("ca", ca.get(), ca_key.get());
        auto rogue_key = make_key();
        auto rogue = make_cert("rogue CA", rogue_key.get(), nullptr, nullptr, true, "");
        write("rogue-ca", rogue.get(), rogue_key.get());
        auto leaf = [&](const std::string& file, const std::string& cn, const std::string& san) {
            auto key = make_key();
            auto cert = make_cert(cn, key.get(), ca.get(), ca_key.get(), false, san);
            write(file, cert.get(), key.get());
        };
        leaf("node", "kythira-redis-node", "IP:127.0.0.1");
        leaf("wrong-host", "kythira-redis-node", "DNS:kv.invalid");
        leaf("farm", "farm", "");
        leaf("gone", "gone", "");
        leaf("stranger", "stranger", "");
    }
    ~test_pki() {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }
    test_pki(const test_pki&) = delete;
    auto operator=(const test_pki&) -> test_pki& = delete;

    [[nodiscard]] auto cert(const std::string& name) const -> std::string {
        return (_dir / (name + ".crt")).string();
    }
    [[nodiscard]] auto key(const std::string& name) const -> std::string {
        return (_dir / (name + ".key")).string();
    }

private:
    auto write(const std::string& name, X509* cert, EVP_PKEY* key) -> void {
        BIO* c = BIO_new_file(this->cert(name).c_str(), "w");
        BIO* k = BIO_new_file(this->key(name).c_str(), "w");
        bool ok = c != nullptr && k != nullptr && PEM_write_bio_X509(c, cert) == 1 &&
                  PEM_write_bio_PrivateKey(k, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
        BIO_free(c);
        BIO_free(k);
        if (!ok) {
            throw std::runtime_error("cannot write PEM files for " + name);
        }
    }

    std::filesystem::path _dir;
};

/// Gateways with an mTLS listener presenting `leaf`, forwarding over TLS.
auto tls_config(const test_pki& pki, const std::string& leaf = "node") -> redis_gateway_config {
    redis_gateway_config cfg;
    cfg._tls_listen = "127.0.0.1:0";
    cfg._tls_cert_path = pki.cert(leaf);
    cfg._tls_key_path = pki.key(leaf);
    cfg._tls_ca_path = pki.cert("ca");
    cfg._require_client_cert = true;
    cfg._forward_tls = true;
    return cfg;
}

/// A TLS client context trusting the test CA, presenting `leaf` if given.
auto client_tls(const test_pki& pki, const std::string& leaf = "")
    -> std::shared_ptr<boost::asio::ssl::context> {
    namespace ssl = boost::asio::ssl;
    auto ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
    ctx->load_verify_file(pki.cert("ca"));
    ctx->set_verify_mode(ssl::verify_peer);
    if (!leaf.empty()) {
        ctx->use_certificate_chain_file(pki.cert(leaf));
        ctx->use_private_key_file(pki.key(leaf), ssl::context::pem);
    }
    return ctx;
}

// ── a minimal RESP client ────────────────────────────────────────────────────

class resp_client {
public:
    /// Plaintext, or TLS when `tls` is given; a TLS client checks that the
    /// server certificate names 127.0.0.1.
    explicit resp_client(std::uint16_t port,
                         std::shared_ptr<boost::asio::ssl::context> tls = nullptr)
        : _tls_ctx(std::move(tls)) {
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::make_address("127.0.0.1"), port);
        if (_tls_ctx) {
            _tls.emplace(_io, *_tls_ctx);
            _tls->next_layer().connect(ep);
            _tls->next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(_tls->native_handle()), "127.0.0.1");
            _tls->handshake(boost::asio::ssl::stream_base::client);
        } else {
            _plain.emplace(_io);
            _plain->connect(ep);
            _plain->set_option(boost::asio::ip::tcp::no_delay(true));
        }
    }

    static auto encode(const std::vector<std::string>& argv) -> std::string {
        std::string out = "*" + std::to_string(argv.size()) + "\r\n";
        for (const auto& a : argv) {
            out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
        }
        return out;
    }

    auto send_raw(const std::string& bytes) -> void {
        if (_tls) {
            boost::asio::write(*_tls, boost::asio::buffer(bytes));
        } else {
            boost::asio::write(*_plain, boost::asio::buffer(bytes));
        }
    }

    /// Send one command and return its raw reply.
    auto call(const std::vector<std::string>& argv) -> std::string {
        send_raw(encode(argv));
        return read_reply();
    }

    auto read_reply() -> std::string {
        std::size_t len = 0;
        while ((len = resp_reply_length(_buffer)) == 0) {
            std::array<char, 65536> chunk{};
            boost::system::error_code ec;
            auto n = read_some(boost::asio::buffer(chunk), ec);
            if (ec) {
                throw std::runtime_error("connection closed: " + ec.message());
            }
            _buffer.append(chunk.data(), n);
        }
        auto reply = _buffer.substr(0, len);
        _buffer.erase(0, len);
        return reply;
    }

    /// True once the peer has closed the connection (EOF on read).
    auto wait_closed() -> bool {
        std::array<char, 16> chunk{};
        boost::system::error_code ec;
        while (true) {
            auto n = read_some(boost::asio::buffer(chunk), ec);
            if (ec == boost::asio::error::eof) {
                return true;
            }
            if (ec) {
                return true;
            }
            _buffer.append(chunk.data(), n);
        }
    }

    auto auth(const std::string& user, const std::string& secret) -> std::string {
        return call({"AUTH", user, secret});
    }

private:
    auto read_some(boost::asio::mutable_buffer into, boost::system::error_code& ec) -> std::size_t {
        return _tls ? _tls->read_some(into, ec) : _plain->read_some(into, ec);
    }

    boost::asio::io_context _io;
    std::shared_ptr<boost::asio::ssl::context> _tls_ctx;
    std::optional<boost::asio::ip::tcp::socket> _plain;
    std::optional<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _tls;
    std::string _buffer;
};

/// Every reply the gateway gives when a shard's leader cannot be reached.
auto is_retry_error(const std::string& reply) -> bool {
    return reply.rfind("-ERR ", 0) == 0 && reply.find("retry") != std::string::npos;
}

auto bulk(const std::string& s) -> std::string {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}

// ── a three-node cluster with a gateway per node ─────────────────────────────

class cluster {
public:
    /// `plaintext_forwarding` opts the gateways into forwarding over
    /// plaintext loopback, which every case not about forwarding security
    /// relies on; it has no effect once `base._forward_tls` is set.
    explicit cluster(redis_gateway_config base = {}, bool plaintext_forwarding = true) {
        _acl.reload(acl_text());
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            for (const auto& shard : static_shards()) {
                _hosts.back()->create_group(shard);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            auto cfg = base;
            cfg._listen = "127.0.0.1:0";
            cfg._internal_secret = "internal-secret";
            cfg._io_threads = 1;
            cfg._worker_threads = 4;
            cfg._command_timeout = std::chrono::milliseconds{3000};
            cfg._allow_plaintext_forwarding =
                base._allow_plaintext_forwarding || plaintext_forwarding;
            const node_id_t from = static_cast<node_id_t>(i + 1);
            auto resolver = [this, from](const node_id_t& to) -> std::optional<std::string> {
                if (auto endpoint = endpoint_override(from, to)) {
                    return endpoint;
                }
                auto target = route(from, to);
                if (target < 1 || target > _gateways.size() || !_gateways[target - 1]) {
                    return std::nullopt;
                }
                const auto& gw = *_gateways[target - 1];
                return "127.0.0.1:" +
                       std::to_string(gw.config()._forward_tls ? gw.tls_port() : gw.port());
            };
            _gateways.push_back(
                std::make_unique<gateway_type>(*_hosts[i], _acl, _logger, _metrics, cfg, resolver));
        }
        for (auto& g : _gateways) {
            g->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    ~cluster() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (auto& g : _gateways) {
            g->stop();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    cluster(const cluster&) = delete;
    auto operator=(const cluster&) -> cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto gateway(node_id_t id) -> gateway_type& { return *_gateways.at(id - 1); }
    [[nodiscard]] auto port(node_id_t id) -> std::uint16_t { return _gateways.at(id - 1)->port(); }
    [[nodiscard]] auto tls_port(node_id_t id) -> std::uint16_t {
        return _gateways.at(id - 1)->tls_port();
    }

    /// Make node `from`'s gateway resolve node `to` to node `target`'s
    /// gateway: a stale or poisoned routing map.
    auto poison_route(node_id_t from, node_id_t to, node_id_t target) -> void {
        std::lock_guard<std::mutex> lock(_routes_mutex);
        _routes[{from, to}] = target;
    }
    /// Make node `from`'s gateway resolve node `to` to an arbitrary endpoint.
    auto override_endpoint(node_id_t from, node_id_t to, std::string endpoint) -> void {
        std::lock_guard<std::mutex> lock(_routes_mutex);
        _endpoint_overrides[{from, to}] = std::move(endpoint);
    }
    auto heal_routes() -> void {
        std::lock_guard<std::mutex> lock(_routes_mutex);
        _routes.clear();
        _endpoint_overrides.clear();
    }

    /// Crash node `id`: its Raft traffic is dropped, its host stops ticking
    /// and its gateway closes its listeners and connections.
    auto kill(node_id_t id) -> void {
        _alive[id - 1] = false;
        _fabric.kill(id);
        if (_drivers[id - 1].joinable()) {
            _drivers[id - 1].join();
        }
        _gateways[id - 1]->stop();
    }

    [[nodiscard]] auto leader_of(group_id_type group) -> node_id_t {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (!_alive[id - 1]) {
                continue;  // a crashed node still believes it leads
            }
            auto* n = _hosts[id - 1]->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return 0;
    }

    [[nodiscard]] auto a_follower_of(group_id_type group) -> node_id_t {
        auto leader = leader_of(group);
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (id != leader && _alive[id - 1]) {
                return id;
            }
        }
        return 0;
    }

    auto await_all_leaders(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            bool all = true;
            for (group_id_type g = 1; g <= k_shard_count; ++g) {
                if (leader_of(g) == 0) {
                    all = false;
                    break;
                }
            }
            if (all) {
                // Let the leaders' no-op entries commit so the first write does
                // not race the term's first heartbeat.
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// Run the gateway's maintenance on the leader of `group` right now.
    auto maintain(group_id_type group) -> std::size_t {
        auto leader = leader_of(group);
        if (leader == 0) {
            return 0;
        }
        return _gateways[leader - 1]->run_maintenance();
    }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
        return cfg;
    }

    auto endpoint_override(node_id_t from, node_id_t to) -> std::optional<std::string> {
        std::lock_guard<std::mutex> lock(_routes_mutex);
        auto it = _endpoint_overrides.find({from, to});
        if (it == _endpoint_overrides.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto route(node_id_t from, node_id_t to) -> node_id_t {
        std::lock_guard<std::mutex> lock(_routes_mutex);
        auto it = _routes.find({from, to});
        return it == _routes.end() ? to : it->second;
    }

    auto drive(std::size_t index) -> void {
        while (_running.load() && _alive[index].load()) {
            _hosts[index]->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    message_fabric _fabric{8};
    redis_acl _acl;
    kythira::console_logger _logger{kythira::log_level::warning};
    kythira::noop_metrics _metrics;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::unique_ptr<gateway_type>> _gateways;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::array<std::atomic<bool>, k_node_count> _alive{true, true, true};
    std::mutex _routes_mutex;
    std::map<std::pair<node_id_t, node_id_t>, node_id_t> _routes;
    std::map<std::pair<node_id_t, node_id_t>, std::string> _endpoint_overrides;
};

/// Accepts connections on loopback and never answers: a peer gateway that
/// hung after accept().
class silent_peer {
public:
    silent_peer() : _acceptor(_io, {boost::asio::ip::make_address("127.0.0.1"), 0}) {
        _thread = std::thread([this] {
            while (!_stopping.load()) {
                boost::system::error_code ec;
                auto sock = _acceptor.accept(ec);
                if (ec) {
                    return;
                }
                ++_accepted;
                _held.push_back(std::move(sock));
            }
        });
    }
    ~silent_peer() {
        _stopping = true;
        boost::system::error_code ec;
        _acceptor.cancel(ec);
        // accept() may not wake on cancel; a connection does wake it.
        boost::asio::ip::tcp::socket wake(_io);
        wake.connect(_acceptor.local_endpoint(), ec);
        _acceptor.close(ec);
        _thread.join();
    }
    silent_peer(const silent_peer&) = delete;
    auto operator=(const silent_peer&) -> silent_peer& = delete;

    [[nodiscard]] auto endpoint() const -> std::string {
        return "127.0.0.1:" + std::to_string(_acceptor.local_endpoint().port());
    }
    [[nodiscard]] auto accepted() const -> int { return _accepted.load(); }

private:
    boost::asio::io_context _io;
    boost::asio::ip::tcp::acceptor _acceptor;
    std::vector<boost::asio::ip::tcp::socket> _held;
    std::atomic<bool> _stopping{false};
    std::atomic<int> _accepted{0};
    std::thread _thread;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(redis_gateway)

BOOST_AUTO_TEST_CASE(sccache_handshake_pipeline_and_round_trip, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto leader = c.leader_of(2);
    resp_client client(c.port(leader));

    // redis-rs opens with this exact pipeline; the replies must come back in
    // order as one stream (Requirement 1.2, 1.5).
    client.send_raw(resp_client::encode({"AUTH", "farm", "farm-secret"}) +
                    resp_client::encode({"SELECT", "0"}) +
                    resp_client::encode({"CLIENT", "SETINFO", "LIB-NAME", "redis-rs"}) +
                    resp_client::encode({"CLIENT", "SETINFO", "LIB-VER", "0.25.0"}));
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/missing"}), "$-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/obj1", "object-bytes"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/obj1"}), bulk("object-bytes"));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/obj1"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"STRLEN", "sccache/obj1"}), ":12\r\n");
    BOOST_CHECK_EQUAL(client.call({"GETRANGE", "sccache/obj1", "0", "5"}), bulk("object"));
    BOOST_CHECK_EQUAL(client.call({"GETRANGE", "sccache/obj1", "-5", "-1"}), bulk("bytes"));
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/obj1"}), ":-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/obj1"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/obj1"}), ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/obj1"}), "$-1\r\n");

    // Binary-safe: values may contain CRLF and NULs (Requirement 1.3).
    std::string binary("a\r\n\0b\xff", 6);
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/bin", binary}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/bin"}), bulk(binary));

    BOOST_CHECK_EQUAL(client.call({"SELECT", "1"}), "-ERR DB index is out of range\r\n");
    BOOST_CHECK_EQUAL(client.call({"ECHO", "hi"}), bulk("hi"));
    BOOST_CHECK_EQUAL(client.call({"GET"}), "-ERR wrong number of arguments for 'get' command\r\n");
    auto unknown = client.call({"FLUSHALL"});
    BOOST_CHECK(unknown.rfind("-ERR unknown command 'FLUSHALL'", 0) == 0);
    // ...and the connection is still usable afterwards (Requirement 1.8).
    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"QUIT"}), "+OK\r\n");
    BOOST_CHECK(client.wait_closed());
}

BOOST_AUTO_TEST_CASE(hello_3_switches_the_reply_encoding, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(1)));

    BOOST_CHECK_EQUAL(client.call({"HELLO", "4"}), "-NOPROTO unsupported protocol version\r\n");
    auto hello = client.call({"HELLO", "3", "AUTH", "farm", "farm-secret", "SETNAME", "t"});
    BOOST_CHECK(hello.rfind("%", 0) == 0);
    BOOST_CHECK(hello.find("$6\r\nserver\r\n$7\r\nkythira\r\n") != std::string::npos);
    BOOST_CHECK(hello.find("$5\r\nproto\r\n:3\r\n") != std::string::npos);
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "_\r\n");
    BOOST_CHECK_EQUAL(client.call({"CLIENT", "GETNAME"}), bulk("t"));

    BOOST_CHECK_EQUAL(client.call({"RESET"}), "+RESET\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "-NOAUTH Authentication required.\r\n");
    auto hello2 = client.call({"HELLO", "2", "AUTH", "farm", "farm-secret"});
    BOOST_CHECK(hello2.rfind("*", 0) == 0);
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/none"}), "$-1\r\n");
}

BOOST_AUTO_TEST_CASE(authentication_and_authorization, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._auth_failure_limit = 3;
    // Short enough to wait out below, long enough that the four failures and
    // the reconnect check comfortably land inside one window.
    cfg._auth_failure_window = std::chrono::seconds{3};
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto port = c.port(c.leader_of(1));

    {
        resp_client client(port);
        BOOST_CHECK_EQUAL(client.call({"GET", "sccache/x"}),
                          "-NOAUTH Authentication required.\r\n");
        BOOST_CHECK_EQUAL(client.call({"PING"}), "-NOAUTH Authentication required.\r\n");
        const std::string wrongpass =
            "-WRONGPASS invalid username-password pair or user is disabled.\r\n";
        BOOST_CHECK_EQUAL(client.auth("farm", "nope"), wrongpass);
        BOOST_CHECK_EQUAL(client.auth("ghost", "nope"), wrongpass);
        BOOST_CHECK_EQUAL(client.call({"AUTH", "nope"}), wrongpass);  // "default" user
        // Fourth failure from this source inside the window is refused
        // without running the KDF.
        auto limited = client.auth("farm", "farm-secret");
        BOOST_CHECK(limited.rfind("-ERR too many authentication failures", 0) == 0);
        BOOST_CHECK_GE(c.gateway(c.leader_of(1)).stats()._auth_failures.load(), 4u);
    }
    {
        // The limit is per source ADDRESS, not per connection: reconnecting
        // (a fresh source port) must not reset it, or a password guesser just
        // opens one connection per attempt.
        resp_client reconnect(port);
        auto limited = reconnect.auth("farm", "farm-secret");
        BOOST_CHECK(limited.rfind("-ERR too many authentication failures", 0) == 0);
    }
    // Once the window lapses the address may authenticate again.
    std::this_thread::sleep_for(cfg._auth_failure_window + std::chrono::milliseconds{500});
    {
        resp_client reader(port);
        BOOST_CHECK_EQUAL(reader.auth("reader", "reader-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(reader.call({"GET", "sccache/x"}), "$-1\r\n");
        BOOST_CHECK_EQUAL(reader.call({"SET", "sccache/x", "v"}),
                          "-NOPERM User reader has no permissions to run the 'SET' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"DEL", "sccache/x"}),
                          "-NOPERM User reader has no permissions to run the 'DEL' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"INFO"}),
                          "-NOPERM User reader has no permissions to run the 'INFO' command\r\n");
        BOOST_CHECK_EQUAL(reader.call({"GET", "other/x"}),
                          "-NOPERM No permissions to access a key\r\n");

        resp_client farm(port);
        BOOST_CHECK_EQUAL(farm.auth("farm", "farm-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(farm.call({"SET", "other/x", "v"}),
                          "-NOPERM No permissions to access a key\r\n");
        BOOST_CHECK_EQUAL(farm.call({"SET", "sccache/x", "v"}), "+OK\r\n");
        BOOST_CHECK_EQUAL(farm.call({"INFO"}),
                          "-NOPERM User farm has no permissions to run the 'INFO' command\r\n");

        // DBSIZE counts this node's replicas, so ask the node that led the
        // SET rather than one that may not have applied it yet.
        resp_client ops(c.port(c.leader_of(k_sccache_group)));
        BOOST_CHECK_EQUAL(ops.auth("ops", "ops-secret"), "+OK\r\n");
        auto info = ops.call({"INFO"});
        BOOST_CHECK(info.find("kythira_gateway:1") != std::string::npos);
        BOOST_CHECK(info.find("kythira_authz_denials:") != std::string::npos);
        BOOST_CHECK_EQUAL(ops.call({"DBSIZE"}), ":1\r\n");
        BOOST_CHECK_EQUAL(ops.call({"COMMAND", "COUNT"}), ":17\r\n");
    }
}

BOOST_AUTO_TEST_CASE(any_node_answers_any_key_by_forwarding, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));

    // Talk only to a follower of shard 2 and write a shard-2 key: the write
    // must be forwarded to the leader's gateway and the read served the same
    // way (Requirements 9.1-9.3).
    auto follower = c.a_follower_of(2);
    BOOST_REQUIRE_NE(follower, 0u);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/zzz", "far away"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/zzz"}), bulk("far away"));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/zzz"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/zzz"}), ":-1\r\n");
    BOOST_CHECK_GE(c.gateway(follower).stats()._forwards.load(), 4u);
    BOOST_CHECK_EQUAL(c.gateway(follower).stats()._forward_failures.load(), 0u);

    // The leader's own view agrees.
    resp_client direct(c.port(c.leader_of(2)));
    BOOST_REQUIRE_EQUAL(direct.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(direct.call({"GET", "sccache/zzz"}), bulk("far away"));
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/zzz"}), ":1\r\n");
    BOOST_CHECK_EQUAL(direct.call({"GET", "sccache/zzz"}), "$-1\r\n");

    // A RESP3 client forwarding through a RESP2 internal hop still gets `_`.
    resp_client v3(c.port(follower));
    auto hello = v3.call({"HELLO", "3", "AUTH", "farm", "farm-secret"});
    BOOST_REQUIRE(hello.rfind("%", 0) == 0);
    BOOST_CHECK_EQUAL(v3.call({"GET", "sccache/zzz"}), "_\r\n");
}

BOOST_AUTO_TEST_CASE(forwarding_off_answers_with_a_retry_error, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._forwarding = false;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/aaa", "v"}),
                      "-ERR shard has no reachable leader, retry\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/aaa"}),
                      "-ERR shard has no reachable leader, retry\r\n");
}

BOOST_AUTO_TEST_CASE(any_replica_reads_serve_locally, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._read_consistency = redis_read_consistency::any_replica;
    cfg._forwarding = false;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client leader(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(leader.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(leader.call({"SET", "sccache/aaa", "replicated"}), "+OK\r\n");

    resp_client follower(c.port(c.a_follower_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(follower.auth("farm", "farm-secret"), "+OK\r\n");
    // Replication is asynchronous from the client's point of view; poll.
    std::string got;
    for (int i = 0; i < 100 && got != bulk("replicated"); ++i) {
        got = follower.call({"GET", "sccache/aaa"});
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_EQUAL(got, bulk("replicated"));
    BOOST_CHECK_EQUAL(c.gateway(c.a_follower_of(k_sccache_group)).stats()._forwards.load(), 0u);
}

BOOST_AUTO_TEST_CASE(linearizable_reads_go_through_the_log, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._read_consistency = redis_read_consistency::linearizable;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/aaa", "v1"}), "+OK\r\n");
    auto before =
        c.host(c.leader_of(k_sccache_group)).group_node(k_sccache_group)->last_applied_index();
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/aaa"}), bulk("v1"));
    BOOST_CHECK_GT(
        c.host(c.leader_of(k_sccache_group)).group_node(k_sccache_group)->last_applied_index(),
        before);
}

BOOST_AUTO_TEST_CASE(setex_expires_and_the_sweep_removes_it, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "0", "v"}),
                      "-ERR invalid expire time in 'setex' command\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "-5", "v"}),
                      "-ERR invalid expire time in 'setex' command\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "abc", "v"}),
                      "-ERR value is not an integer or out of range\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "1", "v"}), "+OK\r\n");
    auto ttl = client.call({"TTL", "sccache/ttl"});
    BOOST_CHECK(ttl == ":1\r\n" || ttl == ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/ttl"}), bulk("v"));
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/keep", "1000", "k"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/forever", "f"}), "+OK\r\n");

    std::this_thread::sleep_for(std::chrono::milliseconds{1200});
    // Expired for readers immediately, before any sweep (Requirement 6.2).
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/ttl"}), "$-1\r\n");
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/ttl"}), ":0\r\n");
    BOOST_CHECK_EQUAL(client.call({"TTL", "sccache/ttl"}), ":-2\r\n");
    // Rewriting an expired key is not a conflict.
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/ttl", "v2"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"DEL", "sccache/ttl"}), ":1\r\n");
    BOOST_CHECK_EQUAL(client.call({"SETEX", "sccache/ttl", "1", "v3"}), "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds{1200});

    // The sweep removes it from the store on every replica.
    auto proposals = c.maintain(k_sccache_group);
    BOOST_CHECK_GE(proposals, 1u);
    auto count_on = [&](node_id_t id) {
        return c.host(id).group_node(k_sccache_group)->with_state_machine([](auto& sm) {
            return sm.approximate_key_count();
        });
    };
    for (int i = 0; i < 100 && (count_on(1) != 2 || count_on(2) != 2 || count_on(3) != 2); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_EQUAL(count_on(1), 2u);
    BOOST_CHECK_EQUAL(count_on(2), 2u);
    BOOST_CHECK_EQUAL(count_on(3), 2u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._expirations.load(), 1u);
    // Nothing to sweep now: the maintenance pass is a no-op.
    BOOST_CHECK_EQUAL(c.maintain(k_sccache_group), 0u);
}

BOOST_AUTO_TEST_CASE(immutable_values_and_size_limit, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_value_bytes = 64;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "one"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "one"}), "+OK\r\n");  // identical: no-op
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/k", "two"}),
                      "-ERR value conflict for an existing key\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/k"}), bulk("one"));
    BOOST_CHECK_EQUAL(c.gateway(c.leader_of(k_sccache_group)).stats()._value_conflicts.load(), 1u);

    std::string big(65, 'x');
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/big", big}),
                      "-ERR value exceeds the configured maximum of 64 bytes\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/big"}), "$-1\r\n");
    std::string fits(64, 'x');
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/big", fits}), "+OK\r\n");
}

BOOST_AUTO_TEST_CASE(eviction_when_a_shard_is_over_budget, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_shard_bytes = 400;
    cfg._sweep_batch = 2;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");

    // Ten 50-byte values: well past 400 bytes. Writes are never refused.
    for (int i = 0; i < 10; ++i) {
        BOOST_CHECK_EQUAL(
            client.call({"SET", "sccache/e" + std::to_string(i), std::string(50, 'v')}), "+OK\r\n");
    }
    // Reads keep e9 hot so it survives; the LRU tail goes first.
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/e0"}), bulk(std::string(50, 'v')));

    auto bytes_on_leader = [&] {
        return c.host(c.leader_of(k_sccache_group))
            .group_node(k_sccache_group)
            ->with_state_machine([](auto& sm) { return sm.approximate_size_bytes(); });
    };
    std::size_t passes = 0;
    while (bytes_on_leader() > 400 && passes < 20) {
        BOOST_CHECK_GE(c.maintain(k_sccache_group), 1u);
        ++passes;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    BOOST_CHECK_LE(bytes_on_leader(), 400u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._evictions.load(), 1u);
    BOOST_CHECK_GE(c.gateway(c.leader_of(k_sccache_group)).stats()._over_budget_ticks.load(), 1u);
    // The most recently read key survived.
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/e0"}), bulk(std::string(50, 'v')));
    BOOST_CHECK_EQUAL(client.call({"EXISTS", "sccache/e1"}), ":0\r\n");
}

BOOST_AUTO_TEST_CASE(protocol_errors_close_and_limits_hold, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_clients = 2;
    cfg._parser_limits._max_bulk_len = 1024;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto port = c.port(c.leader_of(1));

    {
        resp_client bad(port);
        bad.send_raw("*1\r\n$2000\r\n");
        auto reply = bad.read_reply();
        BOOST_CHECK(reply.rfind("-ERR Protocol error:", 0) == 0);
        BOOST_CHECK(bad.wait_closed());
    }
    {
        resp_client bad(port);
        bad.send_raw("*1\r\n$abc\r\n");
        BOOST_CHECK(bad.read_reply().rfind("-ERR Protocol error:", 0) == 0);
        BOOST_CHECK(bad.wait_closed());
    }
    {
        resp_client first(port);
        resp_client second(port);
        BOOST_CHECK_EQUAL(first.auth("farm", "farm-secret"), "+OK\r\n");
        BOOST_CHECK_EQUAL(second.auth("farm", "farm-secret"), "+OK\r\n");
        resp_client third(port);
        BOOST_CHECK_EQUAL(third.read_reply(), "-ERR max number of clients reached\r\n");
        BOOST_CHECK(third.wait_closed());
        BOOST_CHECK_GE(c.gateway(c.leader_of(1)).stats()._connections_rejected.load(), 1u);
    }
    // Slots are released on close.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    resp_client again(port);
    BOOST_CHECK_EQUAL(again.auth("farm", "farm-secret"), "+OK\r\n");

    // Inline commands work too (redis-cli style).
    again.send_raw("PING\r\n");
    BOOST_CHECK_EQUAL(again.read_reply(), "+PONG\r\n");
}

// Vulnerability audit 2026-10-02 H3: before AUTH the parser runs under
// Redis's unauthenticated limits, one command at a time, so a stranger cannot
// park a 32 MiB argument per connection, yet a client that pipelines AUTH and
// a large SET in one write still gets both executed.
BOOST_AUTO_TEST_CASE(pre_auth_limits_and_pipelined_auth, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto port = c.port(c.leader_of(k_sccache_group));
    {
        resp_client bad(port);
        bad.send_raw("*2\r\n$4\r\nAUTH\r\n$20000\r\n");
        BOOST_CHECK_EQUAL(bad.read_reply(), "-ERR Protocol error: invalid bulk length\r\n");
        BOOST_CHECK(bad.wait_closed());
    }
    {
        resp_client bad(port);
        bad.send_raw("*11\r\n");
        BOOST_CHECK_EQUAL(bad.read_reply(), "-ERR Protocol error: invalid multibulk length\r\n");
        BOOST_CHECK(bad.wait_closed());
    }
    {
        const std::string value(100000, 'p');
        resp_client client(port);
        client.send_raw(resp_client::encode({"AUTH", "farm", "farm-secret"}) +
                        resp_client::encode({"SET", "sccache/pipelined", value}) +
                        resp_client::encode({"GET", "sccache/pipelined"}));
        BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
        BOOST_CHECK(client.read_reply() == bulk(value));
    }
}

// Audit M8: a client that stops reading its replies stalls its own commands
// once its output buffer is full, instead of growing it without bound, and
// gets every reply, in order, once it reads again.
BOOST_AUTO_TEST_CASE(slow_reader_stalls_instead_of_buffering, *boost::unit_test::timeout(120)) {
    redis_gateway_config cfg;
    cfg._max_output_buffer_bytes = 1024u * 1024u;
    cluster c(cfg);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    auto leader = c.leader_of(k_sccache_group);
    resp_client client(c.port(leader));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    const std::string value(512u * 1024u, 'r');
    BOOST_REQUIRE_EQUAL(client.call({"SET", "sccache/slow", value}), "+OK\r\n");

    // ~50 MiB of replies, far beyond the socket buffers and the 1 MiB cap.
    constexpr int k_gets = 100;
    std::string batch;
    for (int i = 0; i < k_gets; ++i) {
        batch += resp_client::encode({"GET", "sccache/slow"});
    }
    client.send_raw(batch);
    auto& stats = c.gateway(leader).stats();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (stats._output_stalls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    BOOST_CHECK_GE(stats._output_stalls.load(), 1u);
    const auto expected = bulk(value);
    for (int i = 0; i < k_gets; ++i) {
        BOOST_REQUIRE(client.read_reply() == expected);
    }
    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
}

BOOST_AUTO_TEST_CASE(pipelined_writes_keep_order, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    resp_client client(c.port(c.leader_of(k_sccache_group)));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    std::string batch;
    constexpr int n = 50;
    for (int i = 0; i < n; ++i) {
        batch += resp_client::encode({"SET", "sccache/p" + std::to_string(i), std::to_string(i)});
        batch += resp_client::encode({"GET", "sccache/p" + std::to_string(i)});
    }
    client.send_raw(batch);
    for (int i = 0; i < n; ++i) {
        BOOST_CHECK_EQUAL(client.read_reply(), "+OK\r\n");
        BOOST_CHECK_EQUAL(client.read_reply(), bulk(std::to_string(i)));
    }
}

BOOST_AUTO_TEST_CASE(empty_acl_refuses_to_start_unless_anonymous, *boost::unit_test::timeout(60)) {
    // No cluster needed: this is a start()-time check. A dummy host is
    // enough because start() never touches it.
    message_fabric fabric{2};
    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
    host_type host(std::move(cfg));
    redis_acl acl;
    kythira::console_logger logger{kythira::log_level::error};
    kythira::noop_metrics metrics;
    redis_gateway_config gcfg;
    gcfg._listen = "127.0.0.1:0";
    {
        gateway_type gw(host, acl, logger, metrics, gcfg, nullptr);
        BOOST_CHECK_THROW(gw.start(), std::runtime_error);
        BOOST_CHECK(!gw.is_running());
    }
    gcfg._allow_anonymous = true;
    gateway_type gw(host, acl, logger, metrics, gcfg, nullptr);
    BOOST_CHECK_NO_THROW(gw.start());
    BOOST_CHECK(gw.is_running());
    BOOST_CHECK_NE(gw.port(), 0);
    resp_client client(gw.port());
    // Anonymous connections are admin; the host is not running, so a key
    // command reports LOADING rather than NOAUTH.
    BOOST_CHECK_EQUAL(client.call({"PING"}), "+PONG\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "x"}),
                      "-LOADING Kythira is loading the dataset in memory\r\n");
    gw.stop();
    BOOST_CHECK(!gw.is_running());
}

// ── task 8: forwarding failure modes ─────────────────────────────────────────

BOOST_AUTO_TEST_CASE(killing_a_shard_leader_gives_a_retry_error_then_recovers,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto old_leader = c.leader_of(k_sccache_group);
    const auto survivor = c.a_follower_of(k_sccache_group);
    BOOST_REQUIRE_NE(survivor, 0u);
    resp_client client(c.port(survivor));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_REQUIRE_EQUAL(client.call({"SET", "sccache/survives", "v"}), "+OK\r\n");

    c.kill(old_leader);

    // The survivor still names the dead node as leader (or no leader at all)
    // until an election completes; either way the client is told to retry,
    // promptly, rather than left hanging on a gateway that is gone.
    const auto started = std::chrono::steady_clock::now();
    const auto first = client.call({"GET", "sccache/survives"});
    BOOST_CHECK_MESSAGE(is_retry_error(first), "first reply after the kill: " << first);
    BOOST_CHECK_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds{5});

    // Then the shard elects a new leader among the survivors and the same
    // single endpoint answers again, with the committed value intact.
    bool recovered = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (!recovered && std::chrono::steady_clock::now() < deadline) {
        auto reply = client.call({"GET", "sccache/survives"});
        if (reply == bulk("v")) {
            recovered = true;
            break;
        }
        BOOST_REQUIRE_MESSAGE(is_retry_error(reply), "unexpected reply during failover: " << reply);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    BOOST_REQUIRE(recovered);
    const auto new_leader = c.leader_of(k_sccache_group);
    BOOST_CHECK_NE(new_leader, 0u);
    BOOST_CHECK_NE(new_leader, old_leader);
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/after", "w"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/after"}), bulk("w"));
}

BOOST_AUTO_TEST_CASE(a_poisoned_routing_map_does_not_loop, *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    node_id_t f = 0;
    node_id_t g = 0;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        if (id != leader) {
            (f == 0 ? f : g) = id;
        }
    }
    BOOST_REQUIRE(f != 0 && g != 0);
    // Both followers believe the other one is the leader's gateway, the
    // shape that would bounce a command between them forever without the
    // one-hop rule (Requirement 13.3).
    c.poison_route(f, leader, g);
    c.poison_route(g, leader, f);

    resp_client client(c.port(f));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    const auto f_before = c.gateway(f).stats()._forwards.load();
    const auto g_before = c.gateway(g).stats()._forwards.load();
    BOOST_CHECK(is_retry_error(client.call({"SET", "sccache/poison", "v"})));
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/poison"})));
    // Exactly one hop per command: f forwarded each once, and g, which got
    // them on an internal connection, forwarded nothing.
    BOOST_CHECK_EQUAL(c.gateway(f).stats()._forwards.load() - f_before, 2u);
    BOOST_CHECK_EQUAL(c.gateway(g).stats()._forwards.load() - g_before, 0u);

    // A map that points a gateway at itself is the degenerate loop.
    c.heal_routes();
    c.poison_route(f, leader, f);
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/poison"})));
    BOOST_CHECK_EQUAL(c.gateway(f).stats()._forwards.load() - f_before, 3u);

    c.heal_routes();
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/poison", "v"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/poison"}), bulk("v"));
}

// Requirement 13.4. The old SO_RCVTIMEO deadline never fired, because Asio
// answers the EAGAIN it causes by polling with no timeout, so this case hung
// a worker forever.
BOOST_AUTO_TEST_CASE(a_peer_that_never_answers_costs_one_command_timeout,
                     *boost::unit_test::timeout(120)) {
    cluster c;
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto leader = c.leader_of(k_sccache_group);
    const auto follower = c.a_follower_of(k_sccache_group);
    silent_peer peer;
    c.override_endpoint(follower, leader, peer.endpoint());

    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    const auto failures_before = c.gateway(follower).stats()._forward_failures.load();
    const auto started = std::chrono::steady_clock::now();
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/hung"})));
    const auto took = std::chrono::steady_clock::now() - started;
    BOOST_CHECK_GE(peer.accepted(), 1);
    BOOST_CHECK_EQUAL(c.gateway(follower).stats()._forward_failures.load() - failures_before, 1u);
    // The fixture's command timeout is 3 s.
    BOOST_CHECK_GE(took, std::chrono::milliseconds{2500});
    BOOST_CHECK_LT(took, std::chrono::seconds{10});

    c.heal_routes();
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/hung"}), "$-1\r\n");
}

// ── Requirement 12.5 / task 9: forwarding and listener security ─────────────

BOOST_AUTO_TEST_CASE(forwarding_without_tls_or_the_plaintext_opt_in_sends_nothing,
                     *boost::unit_test::timeout(120)) {
    cluster c({}, false);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK(is_retry_error(client.call({"SET", "sccache/clear", "v"})));
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/clear"})));
    // No forward was even attempted, so the internal secret never left.
    BOOST_CHECK_EQUAL(c.gateway(follower).stats()._forwards.load(), 0u);
}

BOOST_AUTO_TEST_CASE(forwarding_crosses_mutual_tls, *boost::unit_test::timeout(120)) {
    test_pki pki;
    cluster c(tls_config(pki), false);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    BOOST_REQUIRE_NE(c.tls_port(1), 0);
    // The client speaks plaintext to a follower; the follower forwards to
    // the leader's mTLS listener, the only endpoint the resolver hands out.
    // That listener refuses a connection without a client certificate, so a
    // reply proves the hop was TLS with the node certificate presented.
    const auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"SET", "sccache/tls", "sealed"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(client.call({"GET", "sccache/tls"}), bulk("sealed"));
    BOOST_CHECK_GE(c.gateway(follower).stats()._forwards.load(), 2u);
    BOOST_CHECK_EQUAL(c.gateway(follower).stats()._forward_failures.load(), 0u);
}

BOOST_AUTO_TEST_CASE(forwarding_refuses_a_peer_the_ca_does_not_vouch_for,
                     *boost::unit_test::timeout(120)) {
    test_pki pki;
    auto cfg = tls_config(pki);
    cfg._forward_tls_ca_path = pki.cert("rogue-ca");
    cluster c(cfg, false);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/tls"})));
    BOOST_CHECK_GE(c.gateway(follower).stats()._forward_failures.load(), 1u);
}

BOOST_AUTO_TEST_CASE(forwarding_refuses_a_certificate_for_another_host,
                     *boost::unit_test::timeout(120)) {
    test_pki pki;
    // Every gateway presents a certificate the CA signed, but for
    // kv.invalid, while the resolver dials 127.0.0.1.
    cluster c(tls_config(pki, "wrong-host"), false);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto follower = c.a_follower_of(k_sccache_group);
    resp_client client(c.port(follower));
    BOOST_REQUIRE_EQUAL(client.auth("farm", "farm-secret"), "+OK\r\n");
    BOOST_CHECK(is_retry_error(client.call({"GET", "sccache/tls"})));
    BOOST_CHECK_GE(c.gateway(follower).stats()._forward_failures.load(), 1u);
}

BOOST_AUTO_TEST_CASE(mtls_listener_maps_client_certificates_to_users,
                     *boost::unit_test::timeout(120)) {
    test_pki pki;
    cluster c(tls_config(pki), false);
    BOOST_REQUIRE(c.await_all_leaders(std::chrono::seconds{20}));
    const auto port = c.tls_port(c.leader_of(k_sccache_group));

    // No certificate: the handshake, or the first exchange after it under
    // TLS 1.3, fails.
    bool refused = false;
    try {
        resp_client anonymous(port, client_tls(pki));
        anonymous.call({"PING"});
    } catch (const std::exception&) {
        refused = true;
    }
    BOOST_CHECK(refused);

    // A certificate whose subject maps to a user authenticates without
    // AUTH, and is held to that user's key prefixes.
    resp_client farm(port, client_tls(pki, "farm"));
    BOOST_CHECK_EQUAL(farm.call({"SET", "sccache/mtls", "v"}), "+OK\r\n");
    BOOST_CHECK_EQUAL(farm.call({"GET", "sccache/mtls"}), bulk("v"));
    BOOST_CHECK(farm.call({"SET", "other/mtls", "v"}).rfind("-NOPERM", 0) == 0);

    // A certificate for a disabled user establishes nobody.
    resp_client gone(port, client_tls(pki, "gone"));
    BOOST_CHECK_EQUAL(gone.call({"GET", "sccache/mtls"}), "-NOAUTH Authentication required.\r\n");

    // A valid certificate no user claims gets the same, and AUTH still works.
    resp_client stranger(port, client_tls(pki, "stranger"));
    BOOST_CHECK_EQUAL(stranger.call({"GET", "sccache/mtls"}),
                      "-NOAUTH Authentication required.\r\n");
    BOOST_REQUIRE_EQUAL(stranger.auth("reader", "reader-secret"), "+OK\r\n");
    BOOST_CHECK_EQUAL(stranger.call({"GET", "sccache/mtls"}), bulk("v"));
}

BOOST_AUTO_TEST_CASE(tls_forwarding_without_a_ca_refuses_to_start, *boost::unit_test::timeout(60)) {
    message_fabric fabric{2};
    config_type cfg{
        .node_id = 1,
        .network_client = fabric_client{fabric, 1},
        .network_server = fabric_server{fabric, 1},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.partitioner = kythira::make_partitioner<key_type>(kythira::redis_kv_partitioner{});
    host_type host(std::move(cfg));
    redis_acl acl;
    acl.reload(acl_text());
    kythira::console_logger logger{kythira::log_level::error};
    kythira::noop_metrics metrics;
    redis_gateway_config gcfg;
    gcfg._listen = "127.0.0.1:0";
    gcfg._internal_secret = "internal-secret";
    gcfg._forward_tls = true;
    gateway_type gw(host, acl, logger, metrics, gcfg, nullptr);
    BOOST_CHECK_THROW(gw.start(), std::runtime_error);
    BOOST_CHECK(!gw.is_running());
}

BOOST_AUTO_TEST_SUITE_END()
