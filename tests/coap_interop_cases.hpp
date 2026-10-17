// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Cross-backend CoAP interop: a client on the includer's backend against a
// coap_interop_peer server on each backend that was built, libcoap, cantcoap
// and libnyoci alike (the client's own backend is the control). Two
// processes, because no two backends can share a translation unit; see
// coap_interop_peer.cpp. Each pair runs three times: in the clear, over
// DTLS-PSK, and over DTLS-PKI with both ends verifying the other's
// certificate against one CA.
//
// The includer defines, in namespace coap_interop:
//
//   backend_client<Types>   the backend's client template
//   client_config()         a coap_client_config for that backend
//
// and CMake passes the peers it built as KYTHIRA_INTEROP_PEER_PATH_LIBCOAP,
// _CANTCOAP and _LIBNYOCI (absolute paths). A peer that was not built has no
// case, so the case list names exactly the pairs that ran.

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/serializer_registry.hpp>
#include <raft/types.hpp>

#include "coap_interop_security.hpp"
#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace coap_interop {

using serializer = kythira::json_rpc_serializer<std::vector<std::byte>>;

struct interop_types {
    using serializer_type = serializer;
    using serializer_registry_type = kythira::single_serializer_registry<serializer>;
    using rpc_serializer_type = serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

constexpr std::uint64_t peer_id = 2;

/// A coap_interop_peer child process. Construction starts it and reads the
/// port it bound; destruction closes its stdin, which is its signal to stop,
/// and reaps it, killing it if it has not exited within a few seconds.
class peer_process {
public:
    peer_process(const std::string& path, const std::vector<std::string>& args) {
        int to_child[2];
        int from_child[2];
        if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
            throw std::runtime_error("peer_process: pipe() failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, to_child[1]);
        posix_spawn_file_actions_addclose(&actions, from_child[0]);
        std::vector<char*> argv{const_cast<char*>(path.c_str())};
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        const int rc = ::posix_spawn(&_pid, path.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(to_child[0]);
        ::close(from_child[1]);
        _stdin = to_child[1];
        if (rc != 0) {
            ::close(from_child[0]);
            ::close(_stdin);
            throw std::runtime_error("peer_process: cannot start " + path);
        }

        // One line, `PORT <n>`; anything else means the peer failed to start
        // (its reason is on stderr, which the test log inherits).
        FILE* out = ::fdopen(from_child[0], "r");
        unsigned port = 0;
        const int matched = out != nullptr ? std::fscanf(out, "PORT %u", &port) : 0;
        if (out != nullptr) {
            std::fclose(out);
        }
        if (matched != 1 || port == 0 || port > 65535) {
            stop();
            throw std::runtime_error("peer_process: " + path + " did not report a port");
        }
        _port = static_cast<std::uint16_t>(port);
    }

    ~peer_process() { stop(); }
    peer_process(const peer_process&) = delete;
    auto operator=(const peer_process&) -> peer_process& = delete;

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

    /// Stops the peer and returns its exit status (-1 if it had to be killed).
    auto stop() -> int {
        if (_stdin >= 0) {
            ::close(_stdin);
            _stdin = -1;
        }
        if (_pid <= 0) {
            return _status;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        int status = 0;
        while (::waitpid(_pid, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ::kill(_pid, SIGKILL);
                ::waitpid(_pid, &status, 0);
                _pid = 0;
                return _status = -1;
            }
            ::usleep(10'000);
        }
        _pid = 0;
        return _status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

private:
    pid_t _pid{0};
    int _stdin{-1};
    std::uint16_t _port{0};
    int _status{-1};
};

/// A CA and the two leaves it signed, on disk for both processes. P-256
/// throughout: libnyoci reads a datagram into about 1 KiB, and an RSA chain
/// in one handshake flight would not fit (coap_libnyoci_dtls_test.cpp). The
/// server's leaf carries 127.0.0.1 as an IP SAN, since that is the host the
/// client dials.
class pki_material {
public:
    pki_material() {
        static int counter = 0;
        _dir = std::filesystem::temp_directory_path() /
               ("kythira_coap_interop_" + std::to_string(::getpid()) + "_" +
                std::to_string(counter++));
        std::filesystem::create_directories(_dir);
        const auto ca_key = make_key();
        const auto ca = make_cert(ca_key.get(), "kythira-interop-ca", ca_key.get(), nullptr, 1, "");
        const auto server_key = make_key();
        const auto server = make_cert(server_key.get(), "kythira-interop-server", ca_key.get(),
                                      ca.get(), 2, "IP:127.0.0.1");
        const auto client_key = make_key();
        const auto client =
            make_cert(client_key.get(), "kythira-interop-client", ca_key.get(), ca.get(), 3, "");
        write_cert("ca.pem", ca.get());
        write_cert("server.pem", server.get());
        write_key("server.key", server_key.get());
        write_cert("client.pem", client.get());
        write_key("client.key", client_key.get());
    }

    ~pki_material() {
        std::error_code ignored;
        std::filesystem::remove_all(_dir, ignored);
    }
    pki_material(const pki_material&) = delete;
    auto operator=(const pki_material&) -> pki_material& = delete;

    [[nodiscard]] auto peer_args() const -> std::vector<std::string> {
        return {"pki", file("server.pem"), file("server.key"), file("ca.pem")};
    }
    [[nodiscard]] auto client_security() const -> kythira::coap_security_config {
        return pki_security(file("client.pem"), file("client.key"), file("ca.pem"));
    }

private:
    struct pkey_deleter {
        auto operator()(EVP_PKEY* key) const -> void { EVP_PKEY_free(key); }
    };
    struct x509_deleter {
        auto operator()(X509* cert) const -> void { X509_free(cert); }
    };
    using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
    using x509_ptr = std::unique_ptr<X509, x509_deleter>;

    static auto make_key() -> pkey_ptr {
        pkey_ptr key{EVP_EC_gen("P-256")};
        if (!key) {
            throw std::runtime_error("pki_material: EVP_EC_gen failed");
        }
        return key;
    }

    /// Signed by `issuer_key`; self-signed, and a CA, when `issuer` is null.
    static auto make_cert(EVP_PKEY* subject_key, const std::string& common_name,
                          EVP_PKEY* issuer_key, X509* issuer, long serial,
                          const std::string& subject_alt_name) -> x509_ptr {
        x509_ptr cert{X509_new()};
        X509_set_version(cert.get(), 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial);
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L * 24L);
        X509_set_pubkey(cert.get(), subject_key);
        X509_NAME* name = X509_get_subject_name(cert.get());
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(common_name.c_str()), -1,
                                   -1, 0);
        X509_set_issuer_name(cert.get(), issuer != nullptr ? X509_get_subject_name(issuer) : name);
        X509V3_CTX context;
        X509V3_set_ctx_nodb(&context);
        X509V3_set_ctx(&context, issuer != nullptr ? issuer : cert.get(), cert.get(), nullptr,
                       nullptr, 0);
        const auto add = [&](int nid, const std::string& value) {
            X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
            if (extension == nullptr) {
                throw std::runtime_error("pki_material: bad extension " + value);
            }
            X509_add_ext(cert.get(), extension, -1);
            X509_EXTENSION_free(extension);
        };
        add(NID_basic_constraints, issuer == nullptr ? "critical,CA:TRUE" : "CA:FALSE");
        if (!subject_alt_name.empty()) {
            add(NID_subject_alt_name, subject_alt_name);
        }
        if (X509_sign(cert.get(), issuer_key, EVP_sha256()) <= 0) {
            throw std::runtime_error("pki_material: X509_sign failed");
        }
        return cert;
    }

    [[nodiscard]] auto file(const std::string& name) const -> std::string {
        return (_dir / name).string();
    }

    auto write_cert(const std::string& name, X509* cert) const -> void {
        FILE* out = std::fopen(file(name).c_str(), "wb");
        if (out == nullptr || PEM_write_X509(out, cert) != 1) {
            throw std::runtime_error("pki_material: cannot write " + name);
        }
        std::fclose(out);
    }

    auto write_key(const std::string& name, EVP_PKEY* key) const -> void {
        FILE* out = std::fopen(file(name).c_str(), "wb");
        if (out == nullptr ||
            PEM_write_PrivateKey(out, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
            throw std::runtime_error("pki_material: cannot write " + name);
        }
        std::fclose(out);
    }

    std::filesystem::path _dir;
};

enum class transport_security {
    plaintext,
    psk,
    pki
};

/// Every RPC kind the peer serves, each checked against the value the peer
/// computes from the request (coap_interop_peer.cpp), so a match proves the
/// request body crossed backends intact and the response came back decoded.
inline auto run_rpc_kinds_against(const std::string& peer_path, transport_security security)
    -> void {
    std::optional<pki_material> pki;
    std::vector<std::string> peer_args;
    auto config = client_config();
    if (security == transport_security::psk) {
        peer_args = {"psk"};
        config.security = psk_security();
    } else if (security == transport_security::pki) {
        pki.emplace();
        peer_args = pki->peer_args();
        config.security = pki->client_security();
    }
    peer_process peer{peer_path, peer_args};
    const std::string scheme = security == transport_security::plaintext ? "coap" : "coaps";
    backend_client<interop_types> client{
        {{peer_id, scheme + "://127.0.0.1:" + std::to_string(peer.port())}},
        std::move(config),
        kythira::noop_metrics{}};
    const auto timeout = kythira::testing::scaled_deadline(10000);

    kythira::request_vote_request<> vote{};
    vote._term = 9;
    vote._candidate_id = 3;
    vote._last_log_index = 3;
    vote._last_log_term = 5;
    vote._group_id = 41;
    const auto voted = client.send_request_vote(peer_id, vote, timeout).get();
    BOOST_TEST(voted.term() == 14U);
    BOOST_TEST(voted.vote_granted());
    BOOST_TEST(voted.group_id() == 41U);

    kythira::append_entries_request<> append{};
    append._term = 4;
    append._leader_id = 1;
    append._prev_log_index = 10;
    append._prev_log_term = 1;
    append._leader_commit = 10;
    append._group_id = 42;
    append._entries.push_back(
        kythira::log_entry<>{4, 11, std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}},
                             kythira::entry_type::normal});
    append._entries.push_back(kythira::log_entry<>{4, 12, std::vector<std::byte>{std::byte{0x03}},
                                                   kythira::entry_type::normal});
    const auto appended = client.send_append_entries(peer_id, append, timeout).get();
    BOOST_TEST(appended.term() == 4U);
    BOOST_TEST(appended.success());
    BOOST_REQUIRE(appended.conflict_index().has_value());
    BOOST_TEST(*appended.conflict_index() == 12U);
    BOOST_TEST(appended.group_id() == 42U);

    kythira::install_snapshot_request<> snapshot{};
    snapshot._term = 12;
    snapshot._leader_id = 1;
    snapshot._last_included_index = 100;
    snapshot._last_included_term = 11;
    snapshot._offset = 0;
    snapshot._data = std::vector<std::byte>(300, std::byte{0xAB});
    snapshot._done = true;
    snapshot._group_id = 43;
    const auto installed = client.send_install_snapshot(peer_id, snapshot, timeout).get();
    BOOST_TEST(installed.term() == 400U);
    BOOST_TEST(installed.group_id() == 43U);

    kythira::timeout_now_request<> transfer{};
    transfer._term = 7;
    transfer._leader_id = 1;
    transfer._last_log_index = 35;
    transfer._group_id = 44;
    const auto transferred = client.send_timeout_now(peer_id, transfer, timeout).get();
    BOOST_TEST(transferred.term() == 42U);
    BOOST_TEST(transferred.success());
    BOOST_TEST(transferred.group_id() == 44U);

    BOOST_TEST(peer.stop() == 0);
}

/// A peer serving DTLS-PSK must not answer a plaintext client, or the DTLS
/// cases could pass in the clear: a libcoap server given only
/// security.mode = dtls_psk once bound a plaintext endpoint, and every
/// libcoap pair still passed.
inline auto expect_plaintext_refused_by(const std::string& peer_path) -> void {
    peer_process peer{peer_path, {"psk"}};
    auto config = client_config();
    config.ack_timeout = std::chrono::milliseconds{200};
    config.ack_random_factor_ms = std::chrono::milliseconds{50};
    config.max_retransmit = 1;
    backend_client<interop_types> client{
        {{peer_id, "coap://127.0.0.1:" + std::to_string(peer.port())}},
        std::move(config),
        kythira::noop_metrics{}};
    kythira::request_vote_request<> vote{};
    vote._term = 1;
    vote._candidate_id = 1;
    BOOST_CHECK_THROW(
        client.send_request_vote(peer_id, vote, std::chrono::milliseconds{3000}).get(),
        std::exception);
    BOOST_TEST(peer.stop() == 0);
}

}  // namespace coap_interop

// One case per peer CMake built and per transport security, so the case
// list names exactly the combinations that ran.
#define KYTHIRA_INTEROP_CASE(server, security_name, PATH)                                     \
    BOOST_AUTO_TEST_CASE(every_rpc_kind_against_a_##server##_server_over_##security_name,     \
                         *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) {  \
        coap_interop::run_rpc_kinds_against(PATH,                                             \
                                            coap_interop::transport_security::security_name); \
    }
#define KYTHIRA_INTEROP_CASES(server, PATH)                                                  \
    KYTHIRA_INTEROP_CASE(server, plaintext, PATH)                                            \
    KYTHIRA_INTEROP_CASE(server, psk, PATH)                                                  \
    KYTHIRA_INTEROP_CASE(server, pki, PATH)                                                  \
    BOOST_AUTO_TEST_CASE(a_##server##_dtls_server_refuses_a_plaintext_client,                \
                         *boost::unit_test::timeout(kythira::testing::scaled_timeout(60))) { \
        coap_interop::expect_plaintext_refused_by(PATH);                                     \
    }

BOOST_AUTO_TEST_SUITE(coap_cross_backend_interop)

#ifdef KYTHIRA_INTEROP_PEER_PATH_LIBCOAP
KYTHIRA_INTEROP_CASES(libcoap, KYTHIRA_INTEROP_PEER_PATH_LIBCOAP)
#endif

#ifdef KYTHIRA_INTEROP_PEER_PATH_CANTCOAP
KYTHIRA_INTEROP_CASES(cantcoap, KYTHIRA_INTEROP_PEER_PATH_CANTCOAP)
#endif

#ifdef KYTHIRA_INTEROP_PEER_PATH_LIBNYOCI
KYTHIRA_INTEROP_CASES(libnyoci, KYTHIRA_INTEROP_PEER_PATH_LIBNYOCI)
#endif

BOOST_AUTO_TEST_SUITE_END()

#undef KYTHIRA_INTEROP_CASES
#undef KYTHIRA_INTEROP_CASE
