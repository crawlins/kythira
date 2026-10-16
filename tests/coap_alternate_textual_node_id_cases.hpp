// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The cases cloud-composite-node-ids task 6.2 runs against each alternate
// CoAP backend (cantcoap, libnyoci), mirroring coap_textual_node_id_test's
// libcoap cases. Written once, so both backends are held to the same ones;
// each suite includes this, only when its backend is built, after defining in
// namespace textual_ids:
//
//   backend_client<Types, NodeId>   the backend's client template
//   backend_server<Types, NodeId>   the backend's server template
//   endpoint_for(std::uint16_t)     the endpoint string a client dials
//   client_config()                 a coap_client_config with the backend's
//                                   preferred retransmit timing
//
// Every exchange is real and on loopback. The DTLS-PKI case needs
// coap_peer_identity_validator, so it compiles only with
// CPPHTTPLIB_OPENSSL_SUPPORT, like that header.

#include <raft/cbor_serializer.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/json_serializer.hpp>
#include <raft/serializer_registry.hpp>

#include "test_timeout_scale.hpp"

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#include <raft/coap_peer_identity.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <format>
#endif

namespace textual_ids {

using data_type = std::vector<std::byte>;
constexpr const char* loopback = "127.0.0.1";

template<typename Serializer> struct test_types {
    using serializer_type = Serializer;
    using serializer_registry_type = kythira::single_serializer_registry<Serializer>;
    using rpc_serializer_type = Serializer;
    using metrics_type = kythira::noop_metrics;
    using logger_type = kythira::console_logger;
    using address_type = std::string;
    using port_type = std::uint16_t;
    using executor_type = int;  // named, never invoked; see coap_conformance_types.hpp

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

// The ids a case names its peers by. The std::string ids hold ':', '%', '/'
// and a space: none of it may reach a Uri-Path, and the body carries it as is.
template<typename NodeId> struct ids;
template<> struct ids<std::string> {
    static auto server() -> std::string { return "docker:kythira:2"; }
    static auto client() -> std::string { return "node 1/50%:x"; }
    static auto stranger() -> std::string { return "docker:kythira:9"; }
};
template<> struct ids<kythira::aws_ec2_node_id> {
    static auto server() -> kythira::aws_ec2_node_id {
        return kythira::aws_ec2_node_id::parse("aws-ec2:us-east-1:i-0123456789abcdef0").value();
    }
    static auto client() -> kythira::aws_ec2_node_id {
        return kythira::aws_ec2_node_id::parse("aws-ec2:eu-west-1:i-0fedcba9876543210").value();
    }
    static auto stranger() -> kythira::aws_ec2_node_id {
        return kythira::aws_ec2_node_id::parse("aws-ec2:us-east-1:i-1234abcd").value();
    }
};

template<typename Serializer, typename NodeId> struct rpc_case {
    using serializer = Serializer;
    using node_id = NodeId;
};

using json_ser = kythira::json_rpc_serializer<data_type>;
using cbor_ser = kythira::cbor_rpc_serializer<data_type>;
using rpc_cases =
    std::tuple<rpc_case<json_ser, std::string>, rpc_case<json_ser, kythira::aws_ec2_node_id>,
               rpc_case<cbor_ser, std::string>, rpc_case<cbor_ser, kythira::aws_ec2_node_id>>;
using textual_node_ids = std::tuple<std::string, kythira::aws_ec2_node_id>;

[[nodiscard]] inline auto deadline() -> std::chrono::milliseconds {
    return kythira::testing::scaled_deadline(5000);
}

template<typename Future> auto wait_get(Future future) {
    BOOST_REQUIRE(future.wait(kythira::testing::scaled_deadline(10000)));
    return std::move(future).get();
}

// What a call failed with: its synchronous throw or its future's exception.
// Empty when it succeeded or never settled.
template<typename Call> auto failure_message(Call call) -> std::string {
    try {
        auto future = call();
        if (!future.wait(kythira::testing::scaled_deadline(10000))) {
            return {};
        }
        std::ignore = std::move(future).get();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

}  // namespace textual_ids

BOOST_AUTO_TEST_SUITE(alternate_coap_textual_node_ids)

// Every RPC kind the backend implements, addressed by and carrying textual
// ids. The handlers see the sender's id exactly, and FetchLogEntries brings
// the responder's id back.
BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(60)))
BOOST_AUTO_TEST_CASE_TEMPLATE(every_rpc_round_trips_with_textual_ids, Case,
                              textual_ids::rpc_cases) {
    using node_id = typename Case::node_id;
    using types = textual_ids::test_types<typename Case::serializer>;
    using textual_ids::deadline;
    using textual_ids::wait_get;
    const node_id server_id = textual_ids::ids<node_id>::server();
    const node_id client_id = textual_ids::ids<node_id>::client();
    BOOST_TEST_MESSAGE("serializer: " << typename Case::serializer{}.name() << ", server id: "
                                      << kythira::node_id_traits<node_id>::to_text(server_id));

    textual_ids::backend_server<types, node_id> server(
        textual_ids::loopback, 0, kythira::coap_server_config{}, kythira::noop_metrics{});
    std::mutex seen_mutex;
    std::vector<node_id> seen;
    auto record = [&](const node_id& id) {
        const std::lock_guard lock(seen_mutex);
        seen.push_back(id);
    };
    server.register_request_vote_handler([&](const kythira::request_vote_request<node_id>& r) {
        record(r.candidate_id());
        return kythira::request_vote_response<>{._term = r.term(),
                                                ._vote_granted = r.candidate_id() == client_id};
    });
    server.register_request_pre_vote_handler(
        [&](const kythira::request_pre_vote_request<node_id>& r) {
            record(r.candidate_id());
            return kythira::request_pre_vote_response<>{
                ._term = r.term(), ._vote_granted = r.candidate_id() == client_id};
        });
    server.register_append_entries_handler([&](const kythira::append_entries_request<node_id>& r) {
        record(r.leader_id());
        return kythira::append_entries_response<>{._term = r.term(),
                                                  ._success = r.entries().size() == 2};
    });
    server.register_install_snapshot_handler(
        [&](const kythira::install_snapshot_request<node_id>& r) {
            record(r.leader_id());
            return kythira::install_snapshot_response<>{._term = r.term()};
        });
    server.register_timeout_now_handler([&](const kythira::timeout_now_request<node_id>& r) {
        record(r.leader_id());
        return kythira::timeout_now_response<>{._term = r.term(), ._success = true};
    });
    server.register_fetch_log_entries_handler(
        [&](const kythira::fetch_log_entries_request<node_id>& r) {
            record(r.requester_id());
            kythira::fetch_log_entries_response<node_id> resp;
            resp._responder_id = server_id;
            resp._available = true;
            resp._prev_log_term = 1;
            resp._entries = {kythira::log_entry<>{2, r.from_index(), {std::byte{0x2a}}}};
            return resp;
        });
    server.start();

    textual_ids::backend_client<types, node_id> client(
        {{server_id, textual_ids::endpoint_for(server.bound_port())}}, textual_ids::client_config(),
        kythira::noop_metrics{});

    const auto vote = wait_get(client.send_request_vote(
        server_id, kythira::request_vote_request<node_id>{._term = 3, ._candidate_id = client_id},
        deadline()));
    BOOST_TEST(vote.vote_granted());

    const auto pre_vote = wait_get(client.send_request_pre_vote(
        server_id,
        kythira::request_pre_vote_request<node_id>{._term = 4, ._candidate_id = client_id},
        deadline()));
    BOOST_TEST(pre_vote.vote_granted());

    kythira::append_entries_request<node_id> append{._term = 4, ._leader_id = client_id};
    append._entries = {kythira::log_entry<>{4, 1, {std::byte{0x01}}},
                       kythira::log_entry<>{4, 2, {std::byte{0x02}}}};
    BOOST_TEST(wait_get(client.send_append_entries(server_id, append, deadline())).success());

    kythira::install_snapshot_request<node_id> snapshot{._term = 5, ._leader_id = client_id};
    snapshot._data = {std::byte{0x07}};
    snapshot._done = true;
    BOOST_TEST(wait_get(client.send_install_snapshot(server_id, snapshot, deadline())).term() ==
               5U);

    BOOST_TEST(wait_get(client.send_timeout_now(server_id,
                                                kythira::timeout_now_request<node_id>{
                                                    ._term = 6, ._leader_id = client_id},
                                                deadline()))
                   .success());

    const auto fetched = wait_get(client.send_fetch_log_entries(
        server_id,
        kythira::fetch_log_entries_request<node_id>{
            ._requester_id = client_id, ._from_index = 9, ._to_index = 9},
        deadline()));
    BOOST_TEST((fetched.responder_id() == server_id));
    BOOST_TEST(fetched.available());
    BOOST_REQUIRE(fetched.entries().size() == 1U);
    BOOST_TEST(fetched.entries().front().index() == 9U);

    server.stop();
    const std::lock_guard lock(seen_mutex);
    BOOST_TEST(seen.size() == 6U);
    for (const auto& id : seen) {
        BOOST_TEST((id == client_id));
    }
}

// A target the client has no endpoint for fails the call, and the error names
// it by its text rather than some number.
BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(30)))
BOOST_AUTO_TEST_CASE_TEMPLATE(unknown_textual_target_fails_naming_it, NodeId,
                              textual_ids::textual_node_ids) {
    using types = textual_ids::test_types<textual_ids::json_ser>;
    using ids = textual_ids::ids<NodeId>;
    textual_ids::backend_client<types, NodeId> client(
        {{ids::server(), textual_ids::endpoint_for(9)}}, textual_ids::client_config(),
        kythira::noop_metrics{});
    const auto message = textual_ids::failure_message([&] {
        return client.send_request_vote(
            ids::stranger(),
            kythira::request_vote_request<NodeId>{._term = 1, ._candidate_id = ids::client()},
            textual_ids::deadline());
    });
    BOOST_TEST(message.find(kythira::node_id_traits<NodeId>::to_text(ids::stranger())) !=
                   std::string::npos,
               "message: " << message);
}

BOOST_AUTO_TEST_SUITE_END()

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT

// ── DTLS-PKI peers bound to their node ids ───────────────────────────────────

namespace textual_ids {

struct pkey_deleter {
    auto operator()(EVP_PKEY* key) const -> void { EVP_PKEY_free(key); }
};
struct x509_deleter {
    auto operator()(X509* cert) const -> void { X509_free(cert); }
};
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

[[nodiscard]] inline auto make_key() -> pkey_ptr {
    pkey_ptr key{EVP_EC_gen("P-256")};
    BOOST_REQUIRE(key);
    return key;
}

inline auto add_extension(X509* cert, X509* issuer, int nid, const std::string& value) -> void {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    BOOST_REQUIRE(extension != nullptr);
    X509_add_ext(cert, extension, -1);
    X509_EXTENSION_free(extension);
}

// `san` is an OpenSSL subjectAltName value ("URI:..."), or empty for the CA.
[[nodiscard]] inline auto make_cert(EVP_PKEY* subject_key, const std::string& common_name,
                                    EVP_PKEY* issuer_key, X509* issuer, const std::string& san,
                                    long serial) -> x509_ptr {
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
    if (san.empty()) {
        add_extension(cert.get(), cert.get(), NID_basic_constraints, "critical,CA:TRUE");
    } else {
        add_extension(cert.get(), issuer, NID_subject_alt_name, san);
    }
    BOOST_REQUIRE(X509_sign(cert.get(), issuer_key, EVP_sha256()) > 0);
    return cert;
}

template<typename Write> auto pem_of(Write write) -> std::string {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    write(bio.get());
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    return {data, static_cast<std::size_t>(length)};
}

// One CA, and leaves signed for whichever SANs a case asks for, on disk.
struct pki_dir {
    std::string dir;
    pkey_ptr ca_key{make_key()};
    x509_ptr ca{make_cert(ca_key.get(), "kythira-test-ca", ca_key.get(), nullptr, "", 1)};
    std::string ca_file;
    long serial{2};

    pki_dir() {
        static std::atomic<int> counter{0};
        dir = (std::filesystem::temp_directory_path() /
               std::format("kythira_alt_coap_textual_id_{}_{}", ::getpid(), counter++))
                  .string();
        std::filesystem::create_directories(dir);
        ca_file = write("ca.pem", pem_of([&](BIO* bio) { PEM_write_bio_X509(bio, ca.get()); }));
    }
    pki_dir(const pki_dir&) = delete;
    auto operator=(const pki_dir&) -> pki_dir& = delete;
    pki_dir(pki_dir&&) = delete;
    auto operator=(pki_dir&&) -> pki_dir& = delete;
    ~pki_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    struct leaf {
        std::string cert_file;
        std::string key_file;
    };

    auto issue(const std::string& san) -> leaf {
        auto key = make_key();
        auto cert = make_cert(key.get(), "kythira-peer", ca_key.get(), ca.get(), san, serial);
        const auto stem = std::to_string(serial++);
        return {
            write(stem + ".pem", pem_of([&](BIO* bio) { PEM_write_bio_X509(bio, cert.get()); })),
            write(stem + ".key", pem_of([&](BIO* bio) {
                      PEM_write_bio_PrivateKey(bio, key.get(), nullptr, nullptr, 0, nullptr,
                                               nullptr);
                  }))};
    }

private:
    [[nodiscard]] auto write(const std::string& name, const std::string& contents) const
        -> std::string {
        const auto path = dir + "/" + name;
        FILE* file = std::fopen(path.c_str(), "wb");
        BOOST_REQUIRE(file != nullptr);
        std::fwrite(contents.data(), 1, contents.size(), file);
        std::fclose(file);
        return path;
    }
};

template<typename NodeId> auto uri_san(const NodeId& id) -> std::string {
    return "URI:" + raft::testing::peer_identity_uri(kythira::node_id_traits<NodeId>::to_text(id));
}

}  // namespace textual_ids

BOOST_AUTO_TEST_SUITE(alternate_coap_peer_identity)

// Over a live DTLS-PKI session, each side binds the other to its node id
// through coap_peer_identity_validator. A client whose certificate names a
// different id never reaches a handler.
BOOST_AUTO_TEST_CASE(dtls_pki_binds_peers_to_composite_ids,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    using node_id = kythira::aws_ec2_node_id;
    using types = textual_ids::test_types<textual_ids::json_ser>;
    using ids = textual_ids::ids<node_id>;
    textual_ids::pki_dir pki;
    const auto server_leaf = pki.issue(textual_ids::uri_san(ids::server()));
    const auto client_leaf = pki.issue(textual_ids::uri_san(ids::client()));
    const auto stranger_leaf = pki.issue(textual_ids::uri_san(ids::stranger()));

    auto security = [&](const textual_ids::pki_dir::leaf& self, const node_id& expected_peer) {
        kythira::pki_credentials creds;
        creds.cert_file = self.cert_file;
        creds.key_file = self.key_file;
        creds.ca_file = pki.ca_file;
        creds.verify_peer_cert = true;
        creds.cn_validator = kythira::coap_peer_identity_validator<node_id>({expected_peer});
        return kythira::coap_security_config{kythira::coap_auth_mode::dtls_pki, creds,
                                             std::nullopt};
    };

    kythira::coap_server_config server_config;
    server_config.security = security(server_leaf, ids::client());
    textual_ids::backend_server<types, node_id> server(textual_ids::loopback, 0, server_config,
                                                       kythira::noop_metrics{});
    auto handled = std::make_shared<std::atomic<int>>(0);
    server.register_request_vote_handler(
        [handled](const kythira::request_vote_request<node_id>& r) {
            handled->fetch_add(1);
            return kythira::request_vote_response<>{
                ._term = r.term(), ._vote_granted = r.candidate_id() == ids::client()};
        });
    server.start();
    const auto endpoint = textual_ids::endpoint_for(server.bound_port());

    {
        auto config = textual_ids::client_config();
        config.security = security(client_leaf, ids::server());
        textual_ids::backend_client<types, node_id> client({{ids::server(), endpoint}}, config,
                                                           kythira::noop_metrics{});
        const auto vote = textual_ids::wait_get(client.send_request_vote(
            ids::server(),
            kythira::request_vote_request<node_id>{._term = 2, ._candidate_id = ids::client()},
            kythira::testing::scaled_deadline(10000)));
        BOOST_TEST(vote.vote_granted());
        BOOST_TEST(handled->load() == 1);
    }

    {
        auto config = textual_ids::client_config();
        config.security = security(stranger_leaf, ids::server());
        textual_ids::backend_client<types, node_id> client({{ids::server(), endpoint}}, config,
                                                           kythira::noop_metrics{});
        auto future = client.send_request_vote(
            ids::server(),
            kythira::request_vote_request<node_id>{._term = 3, ._candidate_id = ids::client()},
            kythira::testing::scaled_deadline(3000));
        // Refused either way: an error, or no answer before the deadline.
        if (future.wait(kythira::testing::scaled_deadline(15000))) {
            // Outside BOOST_CHECK_THROW: its do/while wrapper reads as a loop
            // that would reuse the moved-from future.
            bool refused = false;
            try {
                std::ignore = std::move(future).get();
            } catch (const std::exception&) {
                refused = true;
            }
            BOOST_TEST(refused);
        }
        BOOST_TEST(handled->load() == 1);
    }
    server.stop();
}

BOOST_AUTO_TEST_SUITE_END()

#endif  // CPPHTTPLIB_OPENSSL_SUPPORT
