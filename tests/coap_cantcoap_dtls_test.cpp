// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_cantcoap_dtls_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>

// Set test timeout to prevent hanging tests
#define BOOST_TEST_TIMEOUT (240 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/json_serializer.hpp>
#include <raft/network.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/coap_transport_cantcoap_impl.hpp>
#include <folly/executors/CPUThreadPoolExecutor.h>

#include <openssl/evp.h>
#include <openssl/opensslv.h>
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
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// DTLS for the cantcoap backend (.kiro/specs/coap-transport-cantcoap/
// Requirement 6.1). cantcoap has no DTLS at all, so every handshake here is
// OpenSSL driven over the backend's own socket by coap_cantcoap_dtls.hpp.
//
// The negative controls carry as much weight as the round trips: a transport
// that silently fell back to plaintext would pass every round trip, so each
// mode also proves that the wrong credential is refused *and the handler never
// runs*.

namespace {
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;
using test_metrics = kythira::noop_metrics;

struct test_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = test_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = folly::Executor;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

using test_client = kythira::coap_cantcoap_client<test_types>;
using test_server = kythira::coap_cantcoap_server<test_types>;

#ifdef CANTCOAP_AVAILABLE
constexpr std::uint64_t peer_node_id = 5;
constexpr const char* loopback = "127.0.0.1";
constexpr std::uint16_t ephemeral_port = 0;

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    return std::string{"coaps://"} + loopback + ":" + std::to_string(port);
}

[[nodiscard]] auto fast_client_config() -> kythira::coap_client_config {
    kythira::coap_client_config config;
    config.use_confirmable_messages = true;
    config.ack_timeout = std::chrono::milliseconds{300};
    config.ack_random_factor_ms = std::chrono::milliseconds{50};
    config.max_retransmit = 3;
    return config;
}

[[nodiscard]] auto psk_key(std::uint8_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> key;
    for (int i = 0; i < 16; ++i) {
        key.push_back(static_cast<std::byte>(seed + i));
    }
    return key;
}

[[nodiscard]] auto psk_security(std::string identity, std::uint8_t seed = 1)
    -> kythira::coap_security_config {
    kythira::coap_security_config config;
    config.mode = kythira::coap_auth_mode::dtls_psk;
    config.credentials = kythira::psk_credentials{std::move(identity), psk_key(seed)};
    return config;
}

/// Fire raw bytes at a port. Used to prove the server survives garbage.
auto send_raw(std::uint16_t port, const std::vector<std::uint8_t>& bytes) -> void {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, loopback, &addr.sin_addr);
    ::sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::close(fd);
}

struct pkey_deleter {
    auto operator()(EVP_PKEY* key) const -> void { EVP_PKEY_free(key); }
};
struct x509_deleter {
    auto operator()(X509* cert) const -> void { X509_free(cert); }
};
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

[[nodiscard]] auto make_key() -> pkey_ptr {
    pkey_ptr key{EVP_EC_gen("P-256")};
    BOOST_REQUIRE(key);
    return key;
}

/// A certificate for `subject_key`, signed by `issuer_key` under
/// `issuer_name` (or self-signed when they are the subject's own).
[[nodiscard]] auto make_cert(EVP_PKEY* subject_key, const std::string& common_name,
                             EVP_PKEY* issuer_key, X509* issuer, bool is_ca, long serial)
    -> x509_ptr {
    x509_ptr cert{X509_new()};
    BOOST_REQUIRE(cert);
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L * 24L);
    X509_set_pubkey(cert.get(), subject_key);
    X509_NAME* name = X509_get_subject_name(cert.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1,
                               0);
    X509_set_issuer_name(cert.get(), issuer != nullptr ? X509_get_subject_name(issuer) : name);
    if (is_ca) {
        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, cert.get(), cert.get(), nullptr, nullptr, 0);
        X509_EXTENSION* extension =
            X509V3_EXT_conf_nid(nullptr, &context, NID_basic_constraints, "critical,CA:TRUE");
        X509_add_ext(cert.get(), extension, -1);
        X509_EXTENSION_free(extension);
    }
    BOOST_REQUIRE(X509_sign(cert.get(), issuer_key, EVP_sha256()) > 0);
    return cert;
}

/// One CA and two leaves it signed, plus a second, unrelated CA, all on disk.
/// P-256 throughout, so a handshake flight with a certificate chain stays well
/// inside one datagram.
struct pki_material {
    std::string dir;
    std::string ca_file;
    std::string server_cert;
    std::string server_key;
    std::string client_cert;
    std::string client_key;
    std::string other_ca_file;

    pki_material() {
        static std::atomic<int> counter{0};
        dir = (std::filesystem::temp_directory_path() /
               ("kythira_cantcoap_dtls_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter++)))
                  .string();
        std::filesystem::create_directories(dir);

        auto ca_key = make_key();
        auto ca = make_cert(ca_key.get(), "kythira-test-ca", ca_key.get(), nullptr, true, 1);
        auto server_pkey = make_key();
        auto server =
            make_cert(server_pkey.get(), "kythira-server", ca_key.get(), ca.get(), false, 2);
        auto client_pkey = make_key();
        auto client =
            make_cert(client_pkey.get(), "kythira-client", ca_key.get(), ca.get(), false, 3);
        auto other_key = make_key();
        auto other = make_cert(other_key.get(), "unrelated-ca", other_key.get(), nullptr, true, 4);

        ca_file = write_cert("ca.pem", ca.get());
        other_ca_file = write_cert("other-ca.pem", other.get());
        server_cert = write_cert("server.pem", server.get());
        client_cert = write_cert("client.pem", client.get());
        server_key = write_key("server.key", server_pkey.get());
        client_key = write_key("client.key", client_pkey.get());
    }

    pki_material(const pki_material&) = delete;
    auto operator=(const pki_material&) -> pki_material& = delete;

    ~pki_material() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    [[nodiscard]] auto server_security() const -> kythira::coap_security_config {
        kythira::pki_credentials creds;
        creds.cert_file = server_cert;
        creds.key_file = server_key;
        creds.ca_file = ca_file;
        creds.verify_peer_cert = true;
        return {kythira::coap_auth_mode::dtls_pki, creds, std::nullopt};
    }

    [[nodiscard]] auto client_security() const -> kythira::coap_security_config {
        kythira::pki_credentials creds;
        creds.cert_file = client_cert;
        creds.key_file = client_key;
        creds.ca_file = ca_file;
        creds.verify_peer_cert = true;
        return {kythira::coap_auth_mode::dtls_pki, creds, std::nullopt};
    }

private:
    [[nodiscard]] auto write_cert(const std::string& name, X509* cert) const -> std::string {
        const auto path = dir + "/" + name;
        FILE* file = std::fopen(path.c_str(), "wb");
        BOOST_REQUIRE(file != nullptr);
        PEM_write_X509(file, cert);
        std::fclose(file);
        return path;
    }

    [[nodiscard]] auto write_key(const std::string& name, EVP_PKEY* key) const -> std::string {
        const auto path = dir + "/" + name;
        FILE* file = std::fopen(path.c_str(), "wb");
        BOOST_REQUIRE(file != nullptr);
        PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(file);
        return path;
    }
};

/// A server with a request_vote handler that records whether it ever ran.
struct recording_server {
    test_server server;
    std::shared_ptr<std::atomic<bool>> handler_ran = std::make_shared<std::atomic<bool>>(false);

    recording_server(kythira::coap_server_config config, std::uint16_t port = ephemeral_port)
        : server{loopback, port, std::move(config), test_metrics{}} {
        server.register_request_vote_handler([ran = handler_ran](
                                                 const kythira::request_vote_request<>& request) {
            ran->store(true);
            return kythira::request_vote_response<>{request.term(), request.candidate_id() == 11};
        });
        server.start();
    }
};

[[nodiscard]] auto vote(test_client& client, std::uint64_t term,
                        std::chrono::seconds timeout = std::chrono::seconds{20})
    -> kythira::request_vote_response<> {
    const kythira::request_vote_request<> request{term, 11, 0, 0};
    return client.send_request_vote(peer_node_id, request, timeout).get();
}

/// Run one RPC that must be refused, and say whether it was -- by any
/// transport error, since a refused handshake and an unanswered request are
/// both acceptable outcomes for a peer that must not be served.
[[nodiscard]] auto refused(test_client& client, std::chrono::seconds timeout) -> bool {
    try {
        (void)vote(client, 1, timeout);
    } catch (const kythira::coap_transport_error&) {
        return true;
    }
    return false;
}
#endif  // CANTCOAP_AVAILABLE
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_cantcoap_dtls_tests)

#ifdef CANTCOAP_AVAILABLE

// ── DTLS-PSK ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_dtls_psk_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = psk_security("kythira-node");
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    // Several RPCs over the one session: the handshake happens once, and the
    // session then carries everything that follows.
    for (std::uint64_t term = 1; term <= 4; ++term) {
        const auto response = vote(client, term);
        BOOST_TEST(response.term() == term);
        BOOST_TEST(response.vote_granted());
    }
    peer.server.stop();
}

// An identity the server was not configured with must fail the handshake --
// promptly, as a security error, and without the handler ever running.
BOOST_AUTO_TEST_CASE(test_dtls_psk_unknown_identity_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = psk_security("somebody-else");
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    const auto started = std::chrono::steady_clock::now();
    bool security_error = false;
    try {
        (void)vote(client, 1, std::chrono::seconds{20});
    } catch (const kythira::coap_security_error&) {
        security_error = true;
    } catch (const kythira::coap_transport_error&) {
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    BOOST_TEST(security_error, "a refused handshake must surface as a security error");
    BOOST_TEST((elapsed < std::chrono::seconds{10}),
               "the refusal must arrive with the alert, not at the request's timeout");
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// Right identity, wrong key: the Finished MACs cannot agree.
BOOST_AUTO_TEST_CASE(test_dtls_psk_wrong_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node", 1);
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = psk_security("kythira-node", 99);
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    BOOST_TEST(refused(client, std::chrono::seconds{10}));
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// A DTLS server must not answer plaintext CoAP at all, or it would be a
// downgrade waiting to happen.
BOOST_AUTO_TEST_CASE(test_dtls_server_ignores_plaintext_clients,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    recording_server peer{server_config};

    test_client client{{{peer_node_id, endpoint_for(peer.server.bound_port())}},
                       fast_client_config(),
                       test_metrics{}};
    BOOST_TEST(refused(client, std::chrono::seconds{6}));
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// Block-wise rides on top of DTLS unchanged: a 12 KiB snapshot in 256-byte
// blocks is ~48 request/response pairs, each its own record.
BOOST_AUTO_TEST_CASE(test_large_snapshot_over_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    server_config.enable_block_transfer = true;
    server_config.max_block_size = 256;
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    // Atomic: written on the server's loop thread, read here.
    std::atomic<std::size_t> received{0};
    server.register_install_snapshot_handler(
        [&received](const kythira::install_snapshot_request<>& request) {
            received = request.data().size();
            return kythira::install_snapshot_response<>{request.last_included_index()};
        });
    server.start();

    auto client_config = fast_client_config();
    client_config.security = psk_security("kythira-node");
    client_config.enable_block_transfer = true;
    client_config.max_block_size = 256;
    test_client client{
        {{peer_node_id, endpoint_for(server.bound_port())}}, client_config, test_metrics{}};

    constexpr std::size_t snapshot_size = 12 * 1024;
    kythira::install_snapshot_request<> request{};
    request._term = 1;
    request._leader_id = 1;
    request._last_included_index = 900;
    request._data = std::vector<std::byte>(snapshot_size);
    for (std::size_t i = 0; i < snapshot_size; ++i) {
        request._data[i] = static_cast<std::byte>(i * 7U);
    }
    request._done = true;
    const auto response =
        client.send_install_snapshot(peer_node_id, request, std::chrono::seconds{90}).get();
    BOOST_TEST(response.term() == 900U);
    BOOST_TEST(received.load() == snapshot_size);
    server.stop();
}

// A server that restarts loses its half of every session. Its close_notify
// tells the client, so the next request handshakes again rather than sending
// records nobody can read.
BOOST_AUTO_TEST_CASE(test_client_handshakes_again_after_a_server_restart,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    auto client_config = fast_client_config();
    client_config.security = psk_security("kythira-node");

    std::uint16_t port = 0;
    std::unique_ptr<test_client> client;
    {
        recording_server first{server_config};
        port = first.server.bound_port();
        client = std::make_unique<test_client>(
            std::unordered_map<std::uint64_t, std::string>{{peer_node_id, endpoint_for(port)}},
            client_config, test_metrics{});
        BOOST_TEST(vote(*client, 1).term() == 1U);
        first.server.stop();
    }

    recording_server second{server_config, port};
    BOOST_TEST(vote(*client, 2).term() == 2U);
    client.reset();
    second.server.stop();
}

// Garbage at a DTLS server -- not a ClientHello, a truncated record, a record
// for a session that does not exist -- must be dropped, and the server must
// still complete a real handshake afterwards.
BOOST_AUTO_TEST_CASE(test_garbage_keeps_the_dtls_server_alive,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    kythira::coap_server_config server_config;
    server_config.security = psk_security("kythira-node");
    recording_server peer{server_config};
    const auto port = peer.server.bound_port();

    send_raw(port, {});
    send_raw(port, {0x16});                                                  // one byte of a header
    send_raw(port, {0x16, 0xfe, 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0x01});  // truncated hello
    send_raw(port, {0x17, 0xfe, 0xfd, 0, 1, 0, 0, 0, 0, 0, 1, 0, 4, 1, 2, 3, 4});  // stray app data
    send_raw(port, {0x40, 0x02, 0x00, 0x01});                                      // plaintext CoAP
    std::vector<std::uint8_t> big(1400, 0x16);
    send_raw(port, big);

    auto client_config = fast_client_config();
    client_config.security = psk_security("kythira-node");
    test_client client{{{peer_node_id, endpoint_for(port)}}, client_config, test_metrics{}};
    BOOST_TEST(vote(client, 7).term() == 7U);
    peer.server.stop();
}

// ── DTLS-PKI ───────────────────────────────────────────────────────────────

// Mutual authentication: each side presents a certificate from the shared CA
// and verifies the other's.
BOOST_AUTO_TEST_CASE(test_dtls_pki_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    kythira::coap_server_config server_config;
    server_config.security = pki.server_security();
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = pki.client_security();
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    const auto response = vote(client, 21);
    BOOST_TEST(response.term() == 21U);
    BOOST_TEST(response.vote_granted());
    peer.server.stop();
}

// A client that trusts a different CA must refuse the server's certificate.
BOOST_AUTO_TEST_CASE(test_dtls_pki_untrusted_server_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    kythira::coap_server_config server_config;
    server_config.security = pki.server_security();
    recording_server peer{server_config};

    auto security = pki.client_security();
    std::get<kythira::pki_credentials>(security.credentials).ca_file = pki.other_ca_file;
    auto client_config = fast_client_config();
    client_config.security = security;
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    BOOST_TEST(refused(client, std::chrono::seconds{10}));
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// A server that verifies peers must insist on a client certificate.
BOOST_AUTO_TEST_CASE(test_dtls_pki_client_without_a_certificate_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    kythira::coap_server_config server_config;
    server_config.security = pki.server_security();
    recording_server peer{server_config};

    auto security = pki.client_security();
    auto& creds = std::get<kythira::pki_credentials>(security.credentials);
    creds.cert_file.clear();
    creds.key_file.clear();
    auto client_config = fast_client_config();
    client_config.security = security;
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    BOOST_TEST(refused(client, std::chrono::seconds{10}));
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// cn_validator runs on the peer's certificate after the chain has verified,
// and a false answer ends the session before any request crosses it.
BOOST_AUTO_TEST_CASE(test_dtls_pki_cn_validator_is_honoured,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    kythira::coap_server_config server_config;
    server_config.security = pki.server_security();
    recording_server peer{server_config};

    std::atomic<int> consulted{0};
    auto security = pki.client_security();
    std::get<kythira::pki_credentials>(security.credentials).cn_validator =
        [&consulted](const std::string& pem) {
            ++consulted;
            BOOST_TEST(pem.find("BEGIN CERTIFICATE") != std::string::npos);
            return false;
        };
    auto client_config = fast_client_config();
    client_config.security = security;
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    BOOST_TEST(refused(client, std::chrono::seconds{10}));
    BOOST_TEST(consulted.load() >= 1);
    BOOST_TEST(!peer.handler_ran->load());
    peer.server.stop();
}

// cipher_suites takes IANA names (OpenSSL's own names work too) and is
// enforced: overlapping lists handshake, disjoint ones do not. The IANA
// spelling used to go to OpenSSL untranslated and fail the whole setup.
BOOST_AUTO_TEST_CASE(test_dtls_pki_cipher_suites_are_enforced,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    auto server_security = pki.server_security();
    std::get<kythira::pki_credentials>(server_security.credentials).cipher_suites = {
        "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"};
    kythira::coap_server_config server_config;
    server_config.security = server_security;
    recording_server peer{server_config};
    const auto endpoint = endpoint_for(peer.server.bound_port());

    {
        auto security = pki.client_security();
        std::get<kythira::pki_credentials>(security.credentials).cipher_suites = {
            "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", "ECDHE-ECDSA-AES128-GCM-SHA256"};
        auto client_config = fast_client_config();
        client_config.security = security;
        test_client client{{{peer_node_id, endpoint}}, client_config, test_metrics{}};
        BOOST_TEST(vote(client, 31).term() == 31U);
    }
    {
        auto security = pki.client_security();
        std::get<kythira::pki_credentials>(security.credentials).cipher_suites = {
            "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"};
        auto client_config = fast_client_config();
        client_config.security = security;
        test_client client{{{peer_node_id, endpoint}}, client_config, test_metrics{}};
        BOOST_TEST(refused(client, std::chrono::seconds{10}));
    }
    peer.server.stop();
}

// Bad key material fails before any socket exists: in start() for a server,
// at construction for a client.
BOOST_AUTO_TEST_CASE(test_pki_with_missing_files_fails_early,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::pki_credentials creds;
    creds.cert_file = "/nonexistent/kythira-cantcoap.pem";
    creds.key_file = "/nonexistent/kythira-cantcoap.key";

    kythira::coap_server_config server_config;
    server_config.security = {kythira::coap_auth_mode::dtls_pki, creds, std::nullopt};
    test_server server{loopback, ephemeral_port, server_config, test_metrics{}};
    BOOST_CHECK_THROW(server.start(), kythira::coap_security_error);
    BOOST_TEST(!server.is_running());

    kythira::coap_client_config client_config;
    client_config.security = {kythira::coap_auth_mode::dtls_pki, creds, std::nullopt};
    BOOST_CHECK_THROW((test_client{{}, client_config, test_metrics{}}),
                      kythira::coap_security_error);
}

// A mode given credentials of the wrong kind is a configuration error, not a
// read of the wrong variant member.
BOOST_AUTO_TEST_CASE(test_psk_mode_with_pki_credentials_is_a_config_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_server_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_psk;
    config.security.credentials = kythira::pki_credentials{};
    test_server server{loopback, ephemeral_port, config, test_metrics{}};
    BOOST_CHECK_THROW(server.start(), kythira::coap_security_config_error);
    BOOST_TEST(!server.is_running());
}

// ── DTLS-RPK (RFC 7250) ────────────────────────────────────────────────────

#if OPENSSL_VERSION_NUMBER >= 0x30200000L

[[nodiscard]] auto pem_private(EVP_PKEY* key) -> std::vector<std::byte> {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::vector<std::byte> out(reinterpret_cast<std::byte*>(data),
                               reinterpret_cast<std::byte*>(data) + length);
    BIO_free(bio);
    return out;
}

[[nodiscard]] auto pem_public(EVP_PKEY* key) -> std::vector<std::byte> {
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, key);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::vector<std::byte> out(reinterpret_cast<std::byte*>(data),
                               reinterpret_cast<std::byte*>(data) + length);
    BIO_free(bio);
    return out;
}

[[nodiscard]] auto rpk_security(EVP_PKEY* own, EVP_PKEY* trusted_peer)
    -> kythira::coap_security_config {
    kythira::rpk_credentials creds;
    creds.public_key = pem_public(own);
    creds.private_key = pem_private(own);
    creds.trusted_peer_keys.push_back(pem_public(trusted_peer));
    return {kythira::coap_auth_mode::dtls_rpk, creds, std::nullopt};
}

BOOST_AUTO_TEST_CASE(test_dtls_rpk_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto server_key = make_key();
    auto client_key = make_key();
    kythira::coap_server_config server_config;
    server_config.security = rpk_security(server_key.get(), client_key.get());
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = rpk_security(client_key.get(), server_key.get());
    test_client client{
        {{peer_node_id, endpoint_for(peer.server.bound_port())}}, client_config, test_metrics{}};

    const auto response = vote(client, 31);
    BOOST_TEST(response.term() == 31U);
    BOOST_TEST(response.vote_granted());
    peer.server.stop();
}

// A raw key is trusted by being pinned, so an unpinned one -- on either side --
// must end the session before a request crosses it.
BOOST_AUTO_TEST_CASE(test_dtls_rpk_unpinned_keys_are_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto server_key = make_key();
    auto client_key = make_key();
    auto stranger = make_key();

    {
        // The client pins a key the server does not hold.
        kythira::coap_server_config server_config;
        server_config.security = rpk_security(server_key.get(), client_key.get());
        recording_server peer{server_config};
        auto client_config = fast_client_config();
        client_config.security = rpk_security(client_key.get(), stranger.get());
        test_client client{{{peer_node_id, endpoint_for(peer.server.bound_port())}},
                           client_config,
                           test_metrics{}};
        BOOST_TEST(refused(client, std::chrono::seconds{10}));
        BOOST_TEST(!peer.handler_ran->load());
        peer.server.stop();
    }
    {
        // The server pins a key the client does not hold.
        kythira::coap_server_config server_config;
        server_config.security = rpk_security(server_key.get(), stranger.get());
        recording_server peer{server_config};
        auto client_config = fast_client_config();
        client_config.security = rpk_security(client_key.get(), server_key.get());
        test_client client{{{peer_node_id, endpoint_for(peer.server.bound_port())}},
                           client_config,
                           test_metrics{}};
        BOOST_TEST(refused(client, std::chrono::seconds{10}));
        BOOST_TEST(!peer.handler_ran->load());
        peer.server.stop();
    }
}

#else  // OpenSSL < 3.2

BOOST_AUTO_TEST_CASE(test_dtls_rpk_is_refused_naming_the_openssl_version,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    kythira::coap_client_config config;
    config.security.mode = kythira::coap_auth_mode::dtls_rpk;
    config.security.credentials = kythira::rpk_credentials{};
    try {
        test_client client{{}, config, test_metrics{}};
        BOOST_FAIL("RPK must be refused on an OpenSSL without RFC 7250 support");
    } catch (const kythira::coap_unsupported_security_mode_error& e) {
        const std::string message = e.what();
        BOOST_TEST(message.find("3.2") != std::string::npos, message);
    }
}

#endif  // OPENSSL_VERSION_NUMBER

#else  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(test_cantcoap_dtls_tests_skipped_without_cantcoap) {
    BOOST_TEST_MESSAGE(
        "cantcoap not available — the cantcoap DTLS tests are skipped. Rebuild with the vcpkg "
        "'coap-cantcoap' feature to run them.");
    BOOST_TEST(!test_client::backend_available());
}

#endif  // CANTCOAP_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
