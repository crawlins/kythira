// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/**
 * Raft RPCs over DTLS on the libcoap backend (coap-transport Requirements
 * 6.1, 10.3; coap-transport-security Requirements 1-3, task 13).
 *
 * Before this file, every libcoap DTLS test stopped at the handshake:
 * coap_dtls_handshake_test drives initiate/complete_dtls_handshake() and
 * nothing more, and coap_dtls_rpk_test checks the RPK trust-set comparison
 * without a peer. No test sent a RequestVote or AppendEntries through a DTLS
 * session, so a transport that handshook and then fell back to plaintext, or
 * never handshook at all on the RPC path, would have passed everything.
 *
 * Each mode here runs real RPCs between a coap_client and a coap_server on
 * localhost, and pairs every round trip with a negative control: the wrong
 * credential is refused *and the handler never runs*.
 *
 * DTLS-RPK needs a libcoap TLS backend with raw-public-key support. libcoap's
 * OpenSSL backend (the one the vcpkg port builds) has none --
 * coap_dtls_rpk_is_supported() returns 0 -- so there the live RPK cases are
 * skipped and the test instead checks that configuring RPK fails closed.
 * Against a GnuTLS-backed libcoap they run for real.
 */
#define BOOST_TEST_MODULE coap_dtls_raft_rpc_test
#include <boost/test/unit_test.hpp>
#include <raft/future_default.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test_timeout_scale.hpp"

using namespace kythira;

namespace {
using test_serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

// default_transport_types pins future_template to one response type, and this
// file sends more than one RPC kind.
struct test_transport_types {
    using serializer_type = test_serializer;
    using serializer_registry_type = kythira::single_serializer_registry<test_serializer>;
    using rpc_serializer_type = test_serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = kythira::console_logger;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;

    using future_type = kythira::future_default<kythira::request_vote_response<>>;
};
}  // namespace

using test_client = coap_client<test_transport_types>;
using test_server = coap_server<test_transport_types>;

namespace {
constexpr std::uint64_t peer_node_id = 2;
constexpr std::uint64_t candidate_id = 11;
constexpr const char* loopback = "127.0.0.1";

[[nodiscard]] auto endpoint_for(std::uint16_t port) -> std::string {
    return std::format("coaps://{}:{}", loopback, port);
}

[[nodiscard]] auto psk_key(std::uint8_t seed) -> std::vector<std::byte> {
    std::vector<std::byte> key;
    for (int i = 0; i < 16; ++i) {
        key.push_back(static_cast<std::byte>(seed + i));
    }
    return key;
}

[[nodiscard]] auto fast_client_config() -> coap_client_config {
    coap_client_config config;
    config.enable_dtls = true;
    config.max_retransmit = 2;
    return config;
}

[[nodiscard]] auto dtls_server_config() -> coap_server_config {
    coap_server_config config;
    config.enable_dtls = true;
    return config;
}

// ── Key and certificate material ───────────────────────────────────────────

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

/// One CA and two leaves it signed, plus an unrelated CA, all on disk.
struct pki_material {
    std::string dir;
    std::string ca_file;
    std::string server_cert;
    std::string server_key;
    std::string client_cert;
    std::string client_key;
    std::string other_ca_file;
    std::string stranger_cert;  // a client leaf the unrelated CA signed
    std::string stranger_key;

    pki_material() {
        static std::atomic<int> counter{0};
        dir = (std::filesystem::temp_directory_path() /
               std::format("kythira_coap_dtls_rpc_{}_{}", ::getpid(), counter++))
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
        auto stranger_pkey = make_key();
        auto stranger = make_cert(stranger_pkey.get(), "kythira-stranger", other_key.get(),
                                  other.get(), false, 5);

        ca_file = write_cert("ca.pem", ca.get());
        other_ca_file = write_cert("other-ca.pem", other.get());
        server_cert = write_cert("server.pem", server.get());
        client_cert = write_cert("client.pem", client.get());
        server_key = write_key("server.key", server_pkey.get());
        client_key = write_key("client.key", client_pkey.get());
        stranger_cert = write_cert("stranger.pem", stranger.get());
        stranger_key = write_key("stranger.key", stranger_pkey.get());
    }

    pki_material(const pki_material&) = delete;
    auto operator=(const pki_material&) -> pki_material& = delete;

    ~pki_material() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
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

/// An EC key pair as rpk_credentials wants it: PEM for our own key, DER
/// SubjectPublicKeyInfo for what a peer's trust set holds.
struct rpk_keypair {
    std::vector<std::byte> public_pem;
    std::vector<std::byte> private_pem;
    std::vector<std::byte> public_der;
};

[[nodiscard]] auto bio_bytes(BIO* bio) -> std::vector<std::byte> {
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    std::memcpy(bytes.data(), data, bytes.size());
    return bytes;
}

[[nodiscard]] auto make_rpk_keypair() -> rpk_keypair {
    auto key = make_key();
    rpk_keypair result;

    BIO* pub = BIO_new(BIO_s_mem());
    BOOST_REQUIRE(PEM_write_bio_PUBKEY(pub, key.get()) == 1);
    result.public_pem = bio_bytes(pub);
    BIO_free(pub);

    BIO* priv = BIO_new(BIO_s_mem());
    BOOST_REQUIRE(
        PEM_write_bio_PrivateKey(priv, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    result.private_pem = bio_bytes(priv);
    BIO_free(priv);

    const int der_length = i2d_PUBKEY(key.get(), nullptr);
    BOOST_REQUIRE(der_length > 0);
    result.public_der.resize(static_cast<std::size_t>(der_length));
    auto* out = reinterpret_cast<unsigned char*>(result.public_der.data());
    i2d_PUBKEY(key.get(), &out);
    return result;
}

[[nodiscard]] auto rpk_security(const rpk_keypair& self,
                                std::vector<std::vector<std::byte>> trusted)
    -> coap_security_config {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_rpk;
    config.credentials = rpk_credentials{self.public_pem, self.private_pem, std::move(trusted)};
    return config;
}

// ── Server and RPC helpers ─────────────────────────────────────────────────

/// A started server whose handlers record whether they ever ran.
struct recording_server {
    test_transport_types::metrics_type metrics;
    test_server server;
    std::shared_ptr<std::atomic<int>> handled = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<std::size_t>> entries_received =
        std::make_shared<std::atomic<std::size_t>>(0);

    explicit recording_server(coap_server_config config)
        : server{loopback, 0, std::move(config), metrics} {
        server.register_request_vote_handler([count = handled](const request_vote_request<>& req) {
            count->fetch_add(1);
            return request_vote_response<>{req.term(), req.candidate_id() == candidate_id};
        });
        server.register_append_entries_handler(
            [count = handled, entries = entries_received](const append_entries_request<>& req) {
                count->fetch_add(1);
                entries->fetch_add(req.entries().size());
                return append_entries_response<>{req.term(), true, std::nullopt, std::nullopt};
            });
        server.start();
    }

    recording_server(const recording_server&) = delete;
    auto operator=(const recording_server&) -> recording_server& = delete;
    ~recording_server() { server.stop(); }

    [[nodiscard]] auto endpoint() const -> std::string { return endpoint_for(server.bound_port()); }
};

/// A client whose only peer is `server`.
struct peer_client {
    test_transport_types::metrics_type metrics;
    test_client client;

    peer_client(const recording_server& server, coap_client_config config)
        : client{{{peer_node_id, server.endpoint()}}, std::move(config), metrics} {}
};

[[nodiscard]] auto vote(test_client& client, std::uint64_t term,
                        std::chrono::milliseconds timeout = std::chrono::seconds{10})
    -> request_vote_response<> {
    const request_vote_request<> request{term, candidate_id, 0, 0};
    return client.send_request_vote(peer_node_id, request, timeout).get();
}

/// One RPC that must not be served. Either outcome counts: a transport error,
/// or no answer at all within `patience`. The libcoap client does not enforce
/// the per-RPC timeout (only libcoap's own NACKs complete a request), and a
/// peer that silently drops our Finished leaves the handshake retransmitting
/// for 30-60 s, so waiting for the error would make this test that slow.
/// Callers pair this with the server's handler count, which tells "refused"
/// apart from "served late".
[[nodiscard]] auto refused(test_client& client,
                           std::chrono::milliseconds patience = std::chrono::seconds{8}) -> bool {
    const request_vote_request<> request{1, candidate_id, 0, 0};
    auto future = client.send_request_vote(peer_node_id, request, patience);
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (!future.isReady() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    if (!future.isReady()) {
        return true;
    }
    try {
        (void)std::move(future).get();
    } catch (const coap_transport_error&) {
        return true;
    }
    return false;
}

[[nodiscard]] auto rpk_supported() -> bool {
    coap_startup();
    return coap_dtls_rpk_is_supported() != 0;
}
}  // namespace

BOOST_AUTO_TEST_SUITE(coap_dtls_raft_rpc_tests)

// ── DTLS-PSK ───────────────────────────────────────────────────────────────

// The legacy psk_identity/psk_key fields. Several RPCs, both kinds, over the
// one session the first one handshook.
BOOST_AUTO_TEST_CASE(psk_request_vote_and_append_entries_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto server_config = dtls_server_config();
    server_config.psk_identity = "raft-node";
    server_config.psk_key = psk_key(1);
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.psk_identity = "raft-node";
    client_config.psk_key = psk_key(1);
    peer_client node{peer, client_config};

    for (std::uint64_t term = 1; term <= 3; ++term) {
        const auto response = vote(node.client, term);
        BOOST_TEST(response.term() == term);
        BOOST_TEST(response.vote_granted());
    }

    append_entries_request<> request;
    request._term = 3;
    request._leader_id = 1;
    request._prev_log_index = 5;
    request._prev_log_term = 2;
    request._leader_commit = 5;
    request._entries = {log_entry<>{3, 6, {std::byte{0x01}, std::byte{0x02}}},
                        log_entry<>{3, 7, {std::byte{0x03}}}};
    const auto appended =
        node.client.send_append_entries(peer_node_id, request, std::chrono::seconds{10}).get();
    BOOST_TEST(appended.term() == 3U);
    BOOST_TEST(appended.success());
    BOOST_TEST(peer.entries_received->load() == 2U);
    BOOST_TEST(peer.handled->load() == 4);
}

// The explicit security.mode == dtls_psk form must work the same way.
BOOST_AUTO_TEST_CASE(psk_security_mode_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    coap_security_config security;
    security.mode = coap_auth_mode::dtls_psk;
    security.credentials = psk_credentials{"raft-node", psk_key(1)};

    auto server_config = dtls_server_config();
    server_config.security = security;
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = security;
    peer_client node{peer, client_config};

    const auto response = vote(node.client, 4);
    BOOST_TEST(response.term() == 4U);
    BOOST_TEST(response.vote_granted());
}

// Right identity, wrong key: the Finished MACs cannot agree. The server drops
// the Finished it cannot decrypt without an alert, so on OpenSSL the client
// hears nothing until its handshake retransmissions run out.
BOOST_AUTO_TEST_CASE(psk_wrong_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto server_config = dtls_server_config();
    server_config.psk_identity = "raft-node";
    server_config.psk_key = psk_key(1);
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.psk_identity = "raft-node";
    client_config.psk_key = psk_key(99);
    peer_client node{peer, client_config};

    BOOST_TEST(refused(node.client));
    BOOST_TEST(peer.handled->load() == 0);
}

// A DTLS server must not answer plaintext CoAP, or it is a downgrade.
BOOST_AUTO_TEST_CASE(dtls_server_ignores_plaintext_client,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    auto server_config = dtls_server_config();
    server_config.psk_identity = "raft-node";
    server_config.psk_key = psk_key(1);
    recording_server peer{server_config};

    coap_client_config client_config;
    client_config.max_retransmit = 1;
    test_transport_types::metrics_type metrics;
    const auto plaintext_endpoint = std::format("coap://{}:{}", loopback, peer.server.bound_port());
    test_client client{{{peer_node_id, plaintext_endpoint}}, client_config, metrics};

    BOOST_TEST(refused(client));
    BOOST_TEST(peer.handled->load() == 0);
}

// ── DTLS-PKI ───────────────────────────────────────────────────────────────

// Mutual authentication through the legacy cert_file/key_file/ca_file fields.
BOOST_AUTO_TEST_CASE(pki_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    auto server_config = dtls_server_config();
    server_config.cert_file = pki.server_cert;
    server_config.key_file = pki.server_key;
    server_config.ca_file = pki.ca_file;
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.cert_file = pki.client_cert;
    client_config.key_file = pki.client_key;
    client_config.ca_file = pki.ca_file;
    peer_client node{peer, client_config};

    const auto response = vote(node.client, 21);
    BOOST_TEST(response.term() == 21U);
    BOOST_TEST(response.vote_granted());
    BOOST_TEST(peer.handled->load() == 1);
}

// A client that trusts a different CA must refuse the server's certificate.
BOOST_AUTO_TEST_CASE(pki_untrusted_server_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    auto server_config = dtls_server_config();
    server_config.cert_file = pki.server_cert;
    server_config.key_file = pki.server_key;
    server_config.ca_file = pki.ca_file;
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.cert_file = pki.client_cert;
    client_config.key_file = pki.client_key;
    client_config.ca_file = pki.other_ca_file;
    peer_client node{peer, client_config};

    BOOST_TEST(refused(node.client));
    BOOST_TEST(peer.handled->load() == 0);
}

// A server that verifies peers must refuse a client certificate from a CA it
// does not trust, before any handler runs.
BOOST_AUTO_TEST_CASE(pki_untrusted_client_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    const pki_material pki;
    auto server_config = dtls_server_config();
    server_config.cert_file = pki.server_cert;
    server_config.key_file = pki.server_key;
    server_config.ca_file = pki.ca_file;
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.cert_file = pki.stranger_cert;
    client_config.key_file = pki.stranger_key;
    client_config.ca_file = pki.ca_file;
    peer_client node{peer, client_config};

    BOOST_TEST(refused(node.client));
    BOOST_TEST(peer.handled->load() == 0);
}

// ── DTLS-RPK ───────────────────────────────────────────────────────────────

// Each side trusts exactly the other's raw public key.
BOOST_AUTO_TEST_CASE(rpk_round_trip,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (!rpk_supported()) {
        BOOST_TEST_MESSAGE("libcoap TLS backend has no RPK support; live RPK case skipped");
        return;
    }
    const auto server_key = make_rpk_keypair();
    const auto client_key = make_rpk_keypair();

    auto server_config = dtls_server_config();
    server_config.security = rpk_security(server_key, {client_key.public_der});
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = rpk_security(client_key, {server_key.public_der});
    peer_client node{peer, client_config};

    for (std::uint64_t term = 1; term <= 2; ++term) {
        const auto response = vote(node.client, term);
        BOOST_TEST(response.term() == term);
        BOOST_TEST(response.vote_granted());
    }
    BOOST_TEST(peer.handled->load() == 2);
}

// A client that does not trust the server's key must refuse it.
BOOST_AUTO_TEST_CASE(rpk_untrusted_server_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (!rpk_supported()) {
        BOOST_TEST_MESSAGE("libcoap TLS backend has no RPK support; live RPK case skipped");
        return;
    }
    const auto server_key = make_rpk_keypair();
    const auto client_key = make_rpk_keypair();
    const auto stranger = make_rpk_keypair();

    auto server_config = dtls_server_config();
    server_config.security = rpk_security(server_key, {client_key.public_der});
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = rpk_security(client_key, {stranger.public_der});
    peer_client node{peer, client_config};

    BOOST_TEST(refused(node.client));
    BOOST_TEST(peer.handled->load() == 0);
}

// A server that does not trust the client's key must refuse it.
BOOST_AUTO_TEST_CASE(rpk_untrusted_client_key_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {
    if (!rpk_supported()) {
        BOOST_TEST_MESSAGE("libcoap TLS backend has no RPK support; live RPK case skipped");
        return;
    }
    const auto server_key = make_rpk_keypair();
    const auto client_key = make_rpk_keypair();
    const auto stranger = make_rpk_keypair();

    auto server_config = dtls_server_config();
    server_config.security = rpk_security(server_key, {stranger.public_der});
    recording_server peer{server_config};

    auto client_config = fast_client_config();
    client_config.security = rpk_security(client_key, {server_key.public_der});
    peer_client node{peer, client_config};

    BOOST_TEST(refused(node.client));
    BOOST_TEST(peer.handled->load() == 0);
}

// Without backend RPK support there is no RPK handshake to run, so asking for
// one must fail when the transport is built, not quietly at the first RPC.
BOOST_AUTO_TEST_CASE(rpk_without_backend_support_fails_closed,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    if (rpk_supported()) {
        BOOST_TEST_MESSAGE("libcoap TLS backend supports RPK; fail-closed case not applicable");
        return;
    }
    const auto key = make_rpk_keypair();
    auto server_config = dtls_server_config();
    server_config.security = rpk_security(key, {key.public_der});
    test_transport_types::metrics_type metrics;
    BOOST_CHECK_THROW((test_server{loopback, 0, server_config, metrics}),
                      coap_unsupported_security_mode_error);

    auto client_config = fast_client_config();
    client_config.security = rpk_security(key, {key.public_der});
    BOOST_CHECK_THROW((test_client{{{peer_node_id, endpoint_for(5684)}}, client_config, metrics}),
                      coap_unsupported_security_mode_error);
}

BOOST_AUTO_TEST_SUITE_END()
