// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#include "test_timeout_scale.hpp"
// **Feature: coap-transport-security, Requirements 4.2, 7.2, 9.4**
// OSCORE layered over DTLS, end to end on real libcoap: a client and a server
// exchange a request over a loopback DTLS session that also carries OSCORE.
// Each layer is shown to be enforced on its own: the wrong DTLS key fails with
// the right OSCORE context, and the wrong OSCORE context fails over a good
// DTLS session. Both PSK and certificate DTLS are covered, since libcoap has a
// separate combined constructor for each.
//
// The configuration checks at the bottom need no libcoap and run in every
// build.
#define BOOST_TEST_MODULE coap_oscore_over_dtls_test
#include <boost/test/unit_test.hpp>

#define BOOST_TEST_TIMEOUT (30 * KYTHIRA_TEST_TIMEOUT_SCALE)

#include <raft/coap_security_impl.hpp>

#include <optional>
#include <string>
#include <vector>

using namespace kythira;

namespace {

auto make_oscore_credentials(std::vector<std::byte> sender_id, std::vector<std::byte> recipient_id,
                             std::vector<std::byte> master_secret) -> oscore_credentials {
    oscore_credentials creds;
    creds.sender_id = std::move(sender_id);
    creds.recipient_id = std::move(recipient_id);
    creds.master_secret = std::move(master_secret);
    creds.master_salt =
        std::vector<std::byte>{std::byte{0x9e}, std::byte{0x7c}, std::byte{0xa9}, std::byte{0x22},
                               std::byte{0x23}, std::byte{0x78}, std::byte{0x63}, std::byte{0x40}};
    return creds;
}

auto server_oscore(std::vector<std::byte> secret) -> oscore_credentials {
    return make_oscore_credentials({std::byte{0x01}}, {std::byte{0x00}}, std::move(secret));
}

auto client_oscore(std::vector<std::byte> secret) -> oscore_credentials {
    return make_oscore_credentials({std::byte{0x00}}, {std::byte{0x01}}, std::move(secret));
}

auto make_psk(std::byte fill) -> psk_credentials {
    return psk_credentials{"kythira-node", std::vector<std::byte>(16, fill)};
}

}  // namespace

#ifdef LIBCOAP_AVAILABLE

#include <coap3/coap.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <thread>

namespace {

constexpr const char* kResourcePath = "echo";

auto loopback(std::uint16_t port) -> coap_address_t {
    coap_address_t addr;
    coap_address_init(&addr);
    addr.addr.sin.sin_family = AF_INET;
    addr.addr.sin.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.addr.sin.sin_addr);
    addr.size = sizeof(struct sockaddr_in);
    return addr;
}

// The port an endpoint bound to port 0 was given. libcoap has no accessor for
// it; coap_endpoint_str() ("127.0.0.1:<port> DTLS") is the one place it shows,
// as the transport's own endpoint_port() reads it.
auto bound_port(coap_endpoint_t* ep) -> std::uint16_t {
    const char* text = coap_endpoint_str(ep);
    BOOST_REQUIRE(text != nullptr);
    const char* colon = std::strrchr(text, ':');
    BOOST_REQUIRE(colon != nullptr);
    return static_cast<std::uint16_t>(std::strtoul(colon + 1, nullptr, 10));
}

// The same block mode the transport sets on both sides
// (configure_libcoap_block_mode). Besides block-wise transfer it lets
// libcoap's client answer an OSCORE Echo challenge (RFC 8613 Appendix B.1.2)
// by itself.
auto set_block_mode(coap_context_t* ctx) -> void {
    coap_context_set_block_mode(ctx, COAP_BLOCK_USE_LIBCOAP | COAP_BLOCK_SINGLE_BODY);
}

// A server whose only resource echoes a POST's payload back in a 2.04, and
// counts how often it ran. The provider configures both layers on the context;
// the endpoint is DTLS only, so nothing reaches the handler without DTLS.
struct combined_server {
    std::atomic<int> handled{0};
    oscore_provider provider;
    coap_context_t* ctx{nullptr};
    std::uint16_t port{0};
    std::atomic<bool> running{true};
    std::thread io_thread;

    combined_server(oscore_credentials oscore, oscore_dtls_credentials dtls)
        : provider(std::move(oscore), coap_security_role::server, std::move(dtls)) {
        coap_startup();
        // Not a member initializer: coap_startup() must run first.
        // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
        ctx = coap_new_context(nullptr);
        BOOST_REQUIRE(ctx != nullptr);
        coap_set_app_data(ctx, this);
        set_block_mode(ctx);
        provider.configure_session(ctx);

        coap_str_const_t* uri = coap_new_str_const(reinterpret_cast<const uint8_t*>(kResourcePath),
                                                   std::strlen(kResourcePath));
        coap_resource_t* resource = coap_resource_init(uri, 0);
        coap_register_request_handler(
            resource, COAP_REQUEST_POST,
            [](coap_resource_t*, coap_session_t* session, const coap_pdu_t* request,
               const coap_string_t*, coap_pdu_t* response) {
                auto* self = static_cast<combined_server*>(
                    coap_get_app_data(coap_session_get_context(session)));
                self->handled.fetch_add(1);
                const uint8_t* data = nullptr;
                std::size_t len = 0;
                coap_get_data(request, &len, &data);
                coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
                if (len > 0) {
                    coap_add_data(response, len, data);
                }
            });
        coap_add_resource(ctx, resource);

        auto addr = loopback(0);
        coap_endpoint_t* ep = coap_new_endpoint(ctx, &addr, COAP_PROTO_DTLS);
        BOOST_REQUIRE(ep != nullptr);
        port = bound_port(ep);
        io_thread = std::thread([this] {
            while (running.load()) {
                coap_io_process(ctx, 50);
            }
        });
    }

    combined_server(const combined_server&) = delete;
    auto operator=(const combined_server&) -> combined_server& = delete;

    ~combined_server() {
        running.store(false);
        if (io_thread.joinable()) {
            io_thread.join();
        }
        if (ctx != nullptr) {
            coap_free_context(ctx);
        }
    }
};

struct exchange_result {
    std::optional<std::string> echoed;
    coap_proto_t session_proto{COAP_PROTO_NONE};
};

struct exchange_state {
    std::optional<std::string> echoed;
};

// One confirmable POST through a session the client provider makes. The
// provider is asked for UDP, as the transport's plain-endpoint path does: a
// layered provider must still come back with a DTLS session.
auto exchange(coap_security_provider& provider, std::uint16_t port, const std::string& payload,
              std::chrono::milliseconds timeout) -> exchange_result {
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    set_block_mode(ctx);
    provider.configure_session(ctx);

    exchange_state state;
    coap_register_response_handler(
        ctx,
        [](coap_session_t* session, const coap_pdu_t*, const coap_pdu_t* received,
           coap_mid_t) -> coap_response_t {
            auto* st = static_cast<exchange_state*>(coap_session_get_app_data(session));
            if (coap_pdu_get_code(received) == COAP_RESPONSE_CODE_CHANGED) {
                const uint8_t* data = nullptr;
                std::size_t len = 0;
                coap_get_data(received, &len, &data);
                st->echoed = std::string(reinterpret_cast<const char*>(data), len);
            }
            return COAP_RESPONSE_OK;
        });

    auto server_addr = loopback(port);
    coap_session_t* session =
        provider.create_client_session(ctx, nullptr, &server_addr, COAP_PROTO_UDP);
    BOOST_REQUIRE(session != nullptr);
    coap_session_set_app_data(session, &state);
    exchange_result result;
    result.session_proto = coap_session_get_proto(session);

    coap_pdu_t* pdu =
        coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_CODE_POST, coap_new_message_id(session),
                      coap_session_max_pdu_size(session));
    BOOST_REQUIRE(pdu != nullptr);
    coap_add_option(pdu, COAP_OPTION_URI_PATH, std::strlen(kResourcePath),
                    reinterpret_cast<const uint8_t*>(kResourcePath));
    coap_add_data(pdu, payload.size(), reinterpret_cast<const uint8_t*>(payload.data()));
    coap_send(session, pdu);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!state.echoed && std::chrono::steady_clock::now() < deadline) {
        coap_io_process(ctx, 50);
    }
    result.echoed = state.echoed;

    coap_session_release(session);
    coap_free_context(ctx);
    return result;
}

// ── certificate material ──────────────────────────────────────────────────

struct pkey_deleter {
    auto operator()(EVP_PKEY* key) const -> void { EVP_PKEY_free(key); }
};
struct x509_deleter {
    auto operator()(X509* cert) const -> void { X509_free(cert); }
};
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto make_key() -> pkey_ptr {
    pkey_ptr key{EVP_EC_gen("P-256")};
    BOOST_REQUIRE(key);
    return key;
}

// A certificate for `subject_key` signed by `issuer_key` (self-signed when
// `issuer` is null).
auto make_cert(EVP_PKEY* subject_key, const std::string& common_name, EVP_PKEY* issuer_key,
               X509* issuer, bool is_ca, long serial) -> x509_ptr {
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

// One CA and a server and a client leaf it signed, on disk.
struct pki_material {
    std::string dir;
    std::string ca_file;
    std::string server_cert;
    std::string server_key;
    std::string client_cert;
    std::string client_key;

    pki_material() {
        static std::atomic<int> counter{0};
        dir = (std::filesystem::temp_directory_path() /
               ("kythira_oscore_over_dtls_" + std::to_string(::getpid()) + "_" +
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

        ca_file = write_cert("ca.pem", ca.get());
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

    [[nodiscard]] auto credentials(const std::string& cert, const std::string& key) const
        -> pki_credentials {
        pki_credentials creds;
        creds.cert_file = cert;
        creds.key_file = key;
        creds.ca_file = ca_file;
        creds.verify_peer_cert = true;
        return creds;
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

const auto kSecret = std::vector<std::byte>(16, std::byte{0x77});
constexpr auto kExchangeTimeout = std::chrono::milliseconds(5000);
// Long enough for a handshake and a retransmission or two on loopback; a
// rejected exchange never completes, so this is how long a negative case
// waits.
constexpr auto kRejectTimeout = std::chrono::milliseconds(2000);

// The round trips need a libcoap with both OSCORE and DTLS, which every
// build of the transport links. One built without a TLS library has neither
// (OSCORE needs its crypto); there only the capability checks below apply.
auto oscore_and_dtls_linked(boost::unit_test::test_unit_id) -> boost::test_tools::assertion_result {
    coap_startup();
    return coap_oscore_is_supported() != 0 && coap_dtls_is_supported() != 0 &&
           coap_dtls_psk_is_supported() != 0 && coap_dtls_pki_is_supported() != 0;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_oscore_over_dtls_tests)

BOOST_AUTO_TEST_CASE(psk_round_trip_runs_oscore_inside_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25)) *
                         boost::unit_test::precondition(oscore_and_dtls_linked)) {
    combined_server server(server_oscore(kSecret), make_psk(std::byte{0x42}));

    oscore_provider client(client_oscore(kSecret), coap_security_role::client,
                           make_psk(std::byte{0x42}));
    BOOST_CHECK(client.layered_over_dtls());
    auto result = exchange(client, server.port, "hello-oscore-dtls", kExchangeTimeout);

    BOOST_REQUIRE(result.echoed.has_value());
    BOOST_CHECK_EQUAL(*result.echoed, "hello-oscore-dtls");
    // Asked for UDP, given DTLS: the outer layer cannot be dropped.
    BOOST_CHECK(result.session_proto == COAP_PROTO_DTLS);
    BOOST_CHECK_EQUAL(server.handled.load(), 1);
}

BOOST_AUTO_TEST_CASE(pki_round_trip_runs_oscore_inside_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25)) *
                         boost::unit_test::precondition(oscore_and_dtls_linked)) {
    pki_material pki;
    combined_server server(server_oscore(kSecret),
                           pki.credentials(pki.server_cert, pki.server_key));

    oscore_provider client(client_oscore(kSecret), coap_security_role::client,
                           pki.credentials(pki.client_cert, pki.client_key));
    auto result = exchange(client, server.port, "hello-oscore-dtls-pki", kExchangeTimeout);

    BOOST_REQUIRE(result.echoed.has_value());
    BOOST_CHECK_EQUAL(*result.echoed, "hello-oscore-dtls-pki");
    BOOST_CHECK(result.session_proto == COAP_PROTO_DTLS);
}

// The DTLS layer is enforced: right OSCORE context, wrong DTLS key.
BOOST_AUTO_TEST_CASE(wrong_dtls_key_is_rejected_even_with_valid_oscore,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25)) *
                         boost::unit_test::precondition(oscore_and_dtls_linked)) {
    combined_server server(server_oscore(kSecret), make_psk(std::byte{0x42}));

    oscore_provider client(client_oscore(kSecret), coap_security_role::client,
                           make_psk(std::byte{0x43}));
    auto result = exchange(client, server.port, "hello", kRejectTimeout);

    BOOST_CHECK(!result.echoed.has_value());
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

// The OSCORE layer is enforced: a good DTLS session, the wrong OSCORE secret.
BOOST_AUTO_TEST_CASE(wrong_oscore_secret_is_rejected_over_valid_dtls,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(25)) *
                         boost::unit_test::precondition(oscore_and_dtls_linked)) {
    combined_server server(server_oscore(kSecret), make_psk(std::byte{0x42}));

    oscore_provider client(client_oscore(std::vector<std::byte>(16, std::byte{0x22})),
                           coap_security_role::client, make_psk(std::byte{0x42}));
    auto result = exchange(client, server.port, "hello", kRejectTimeout);

    BOOST_CHECK(!result.echoed.has_value());
    BOOST_CHECK_EQUAL(server.handled.load(), 0);
}

BOOST_AUTO_TEST_CASE(layered_session_over_tcp_is_refused,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10)) *
                         boost::unit_test::precondition(oscore_and_dtls_linked)) {
    coap_startup();
    oscore_provider client(client_oscore(kSecret), coap_security_role::client,
                           make_psk(std::byte{0x42}));
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    auto addr = loopback(5684);
    BOOST_CHECK_THROW(client.create_client_session(ctx, nullptr, &addr, COAP_PROTO_TCP),
                      coap_security_config_error);
    coap_free_context(ctx);
}

// Requirement 7.2 against whatever libcoap this binary linked: a build with
// OSCORE and DTLS configures the layered provider; a build missing either
// refuses it, naming OSCORE as the mode, before the context is touched.
BOOST_AUTO_TEST_CASE(layered_provider_checks_linked_libcoap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_startup();
    oscore_provider server(server_oscore(kSecret), coap_security_role::server,
                           make_psk(std::byte{0x42}));
    coap_context_t* ctx = coap_new_context(nullptr);
    BOOST_REQUIRE(ctx != nullptr);
    if (coap_oscore_is_supported() != 0 && coap_dtls_is_supported() != 0 &&
        coap_dtls_psk_is_supported() != 0) {
        BOOST_CHECK_NO_THROW(server.configure_session(ctx));
    } else {
        try {
            server.configure_session(ctx);
            BOOST_FAIL("expected coap_unsupported_security_mode_error");
        } catch (const coap_unsupported_security_mode_error& e) {
            BOOST_CHECK(e.mode() == coap_auth_mode::oscore);
        }
    }
    coap_free_context(ctx);
}

// Requirement 7.2 for the DTLS modes themselves: with no DTLS in the linked
// libcoap, each provider refuses before configuring anything, where it used
// to configure the context and leave every handshake to fail.
BOOST_AUTO_TEST_CASE(dtls_providers_check_linked_libcoap,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_startup();
    if (coap_dtls_is_supported() != 0) {
        BOOST_TEST_MESSAGE("linked libcoap has DTLS; the refusal path needs one without it");
        return;
    }
    dtls_psk_provider psk(make_psk(std::byte{0x42}), coap_security_role::server);
    dtls_pki_provider pki(pki_credentials{"cert.pem", "key.pem", "", false, {}, {}, {}},
                          coap_security_role::server);
    dtls_rpk_provider rpk(rpk_credentials{}, coap_security_role::server);
    std::vector<coap_security_provider*> providers{&psk, &pki, &rpk};
    for (auto* provider : providers) {
        coap_context_t* ctx = coap_new_context(nullptr);
        BOOST_REQUIRE(ctx != nullptr);
        try {
            provider->configure_session(ctx);
            BOOST_ERROR("expected coap_unsupported_security_mode_error for "
                        << to_string(provider->mode()));
        } catch (const coap_unsupported_security_mode_error& e) {
            BOOST_CHECK(e.mode() == provider->mode());
            BOOST_CHECK(std::string(e.what()).find("DTLS not compiled") != std::string::npos);
        }
        coap_free_context(ctx);
    }
}

BOOST_AUTO_TEST_SUITE_END()

#else  // !LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_CASE(oscore_over_dtls_round_trip_requires_libcoap) {
    BOOST_TEST_MESSAGE(
        "coap_oscore_over_dtls_test built without LIBCOAP_AVAILABLE; skipping the "
        "real-libcoap OSCORE-over-DTLS round trips.");
}

#endif  // LIBCOAP_AVAILABLE

// ── configuration (no libcoap needed) ─────────────────────────────────────

BOOST_AUTO_TEST_SUITE(coap_oscore_over_dtls_config_tests)

BOOST_AUTO_TEST_CASE(oscore_dtls_without_oscore_mode_is_a_config_error,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_security_config config;
    config.mode = coap_auth_mode::dtls_psk;
    config.credentials = make_psk(std::byte{0x42});
    config.oscore_dtls = make_psk(std::byte{0x42});
    BOOST_CHECK_THROW(make_security_provider(config, coap_security_role::client),
                      coap_security_config_error);
}

BOOST_AUTO_TEST_CASE(factory_layers_oscore_over_dtls_when_asked,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    config.credentials = client_oscore(std::vector<std::byte>(16, std::byte{0x77}));
    auto plain = make_security_provider(config, coap_security_role::client);
    BOOST_CHECK(!dynamic_cast<oscore_provider&>(*plain).layered_over_dtls());

    config.oscore_dtls = make_psk(std::byte{0x42});
    auto layered = make_security_provider(config, coap_security_role::client);
    BOOST_CHECK(layered->mode() == coap_auth_mode::oscore);
    BOOST_CHECK(dynamic_cast<oscore_provider&>(*layered).layered_over_dtls());
}

// The DTLS layer's credentials are validated as DTLS-PSK's own are.
BOOST_AUTO_TEST_CASE(layered_psk_is_validated_like_plain_dtls_psk,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(10))) {
    coap_security_config config;
    config.mode = coap_auth_mode::oscore;
    config.credentials = client_oscore(std::vector<std::byte>(16, std::byte{0x77}));
    config.oscore_dtls = psk_credentials{"kythira-node", std::vector<std::byte>(2, std::byte{1})};
    BOOST_CHECK_THROW(make_security_provider(config, coap_security_role::client),
                      coap_security_error);
}

BOOST_AUTO_TEST_SUITE_END()
