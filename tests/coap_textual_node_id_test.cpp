// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// **Feature: cloud-composite-node-ids, tasks 6.2 and 6.5 (CoAP, libcoap)**
// The libcoap CoAP transport with textual node ids: coap_client and
// coap_server instantiated for std::string and aws_ec2_node_id carry every
// RPC kind they implement, through each serializer, with the ids intact; an
// unknown textual target fails the call naming it; the multicast discovery
// adaptor wraps a std::string client; and coap_peer_identity_validator binds
// a DTLS-PKI peer to the node id its certificate names, in the SAN kind the
// id type uses.
// **Validates: Requirements 9.2, 9.6, 12.2**

#include "test_timeout_scale.hpp"
#define BOOST_TEST_MODULE coap_textual_node_id_test
#include <boost/test/unit_test.hpp>

#include <raft/future_default.hpp>

#include <raft/cbor_serializer.hpp>
#include <raft/coap_peer_identity.hpp>
#include <raft/coap_transport.hpp>
#include <raft/coap_transport_impl.hpp>
#include <raft/composite_node_id.hpp>
#include <raft/console_logger.hpp>
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
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace {

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
    using executor_type = kythira::console_logger;

    template<typename T> using future_template = kythira::future_default<T>;
    template<typename T> using promise_template = kythira::promise_default<T>;
};

// The ids a case names its two peers by. The std::string ids hold ':', '%',
// '/' and a space: none of it may reach a Uri-Path, and the body carries it
// as is.
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

[[nodiscard]] auto endpoint(const char* scheme, std::uint16_t port) -> std::string {
    return std::format("{}://{}:{}", scheme, loopback, port);
}

[[nodiscard]] auto deadline() -> std::chrono::milliseconds {
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

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_textual_node_id_tests)

#ifdef LIBCOAP_AVAILABLE

// Every RPC kind the libcoap pair implements, addressed by and carrying
// textual ids. The handlers see the sender's id exactly, and FetchLogEntries
// brings the responder's id back.
BOOST_TEST_DECORATOR(*boost::unit_test::timeout(kythira::testing::scaled_timeout(60)))
BOOST_AUTO_TEST_CASE_TEMPLATE(every_rpc_round_trips_with_textual_ids, Case, rpc_cases) {
    using node_id = typename Case::node_id;
    using types = test_types<typename Case::serializer>;
    const node_id server_id = ids<node_id>::server();
    const node_id client_id = ids<node_id>::client();
    BOOST_TEST_MESSAGE("serializer: " << typename Case::serializer{}.name() << ", server id: "
                                      << kythira::node_id_traits<node_id>::to_text(server_id));

    kythira::coap_server<types, node_id> server(loopback, 0, kythira::coap_server_config{},
                                                kythira::noop_metrics{});
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

    kythira::coap_client<types, node_id> client(
        {{server_id, endpoint("coap", server.bound_port())}}, kythira::coap_client_config{},
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
BOOST_AUTO_TEST_CASE(unknown_textual_target_fails_naming_it,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(30))) {
    using types = test_types<json_ser>;
    using node_id = kythira::aws_ec2_node_id;
    kythira::coap_client<types, node_id> client({{ids<node_id>::server(), endpoint("coap", 9)}},
                                                kythira::coap_client_config{},
                                                kythira::noop_metrics{});
    const auto message = failure_message([&] {
        return client.send_request_vote(ids<node_id>::stranger(),
                                        kythira::request_vote_request<node_id>{
                                            ._term = 1, ._candidate_id = ids<node_id>::client()},
                                        deadline());
    });
    BOOST_TEST(message.find(ids<node_id>::stranger().to_string()) != std::string::npos,
               "message: " << message);
}

#endif  // LIBCOAP_AVAILABLE

// Requirement 9.6: the discovery adaptor, whose ids are std::string, wraps a
// client instantiated for std::string as well as a numeric one.
BOOST_AUTO_TEST_CASE(multicast_discovery_wraps_a_textual_client) {
    using types = test_types<json_ser>;
    using text_discovery = kythira::coap_multicast_peer_discovery<types, std::string>;
    using numeric_discovery = kythira::coap_multicast_peer_discovery<types>;
    static_assert(
        std::is_constructible_v<text_discovery, kythira::coap_client<types, std::string>&>);
    static_assert(
        std::is_constructible_v<numeric_discovery, kythira::coap_client<types, std::uint64_t>&>);
    static_assert(std::same_as<typename text_discovery::node_id_type, std::string>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_SUITE_END()

// ── DTLS-PKI peers bound to their node ids ───────────────────────────────────

namespace {

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

auto add_extension(X509* cert, X509* issuer, int nid, const std::string& value) -> void {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    BOOST_REQUIRE(extension != nullptr);
    X509_add_ext(cert, extension, -1);
    X509_EXTENSION_free(extension);
}

// `san` is an OpenSSL subjectAltName value ("URI:...", "DNS:..."), or empty
// for a CA certificate.
[[nodiscard]] auto make_cert(EVP_PKEY* subject_key, const std::string& common_name,
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

[[nodiscard]] auto pem_of(X509* cert) -> std::string {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
    PEM_write_bio_X509(bio.get(), cert);
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    return {data, static_cast<std::size_t>(length)};
}

// One CA, and leaves it signed for whichever SANs a case asks for, on disk.
struct pki_dir {
    std::string dir;
    pkey_ptr ca_key{make_key()};
    x509_ptr ca{make_cert(ca_key.get(), "kythira-test-ca", ca_key.get(), nullptr, "", 1)};
    std::string ca_file;
    long serial{2};

    pki_dir() {
        static std::atomic<int> counter{0};
        dir = (std::filesystem::temp_directory_path() /
               std::format("kythira_coap_textual_id_{}_{}", ::getpid(), counter++))
                  .string();
        std::filesystem::create_directories(dir);
        ca_file = write("ca.pem", pem_of(ca.get()));
    }
    pki_dir(const pki_dir&) = delete;
    auto operator=(const pki_dir&) -> pki_dir& = delete;
    ~pki_dir() {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }

    struct leaf {
        std::string cert_pem;
        std::string cert_file;
        std::string key_file;
    };

    auto issue(const std::string& san) -> leaf {
        auto key = make_key();
        auto cert = make_cert(key.get(), "kythira-peer", ca_key.get(), ca.get(), san, serial);
        const auto stem = std::to_string(serial++);
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), &BIO_free);
        PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio.get(), &data);
        leaf out{pem_of(cert.get()), write(stem + ".pem", pem_of(cert.get())),
                 write(stem + ".key", std::string(data, static_cast<std::size_t>(length)))};
        return out;
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

}  // namespace

BOOST_AUTO_TEST_SUITE(coap_peer_identity_tests)

// A textual validator accepts the URI SAN naming one of its peers, and nothing
// else: another id, the same text as a DNS name, or garbage.
BOOST_AUTO_TEST_CASE(textual_validator_matches_uri_sans_only) {
    using node_id = kythira::aws_ec2_node_id;
    pki_dir pki;
    const auto validator = kythira::coap_peer_identity_validator<node_id>(
        {ids<node_id>::server(), ids<node_id>::client()});

    BOOST_TEST(validator(pki.issue(uri_san(ids<node_id>::client())).cert_pem));
    BOOST_TEST(validator(pki.issue(uri_san(ids<node_id>::server())).cert_pem));
    BOOST_TEST(!validator(pki.issue(uri_san(ids<node_id>::stranger())).cert_pem));
    // The right name in the wrong SAN kind is not a peer: a DNS SAN cannot
    // even hold canonical text, so take the numeric form of the same peer.
    BOOST_TEST(!validator(pki.issue("DNS:" + raft::testing::peer_identity_dns_name(7)).cert_pem));
    BOOST_TEST(!validator("not a certificate"));
    BOOST_TEST(!kythira::coap_peer_identity_validator<node_id>({})(
        pki.issue(uri_san(ids<node_id>::client())).cert_pem));

    // std::string ids percent-encode '%', '/' and ' ' into the URI, so the
    // validator must use the same encoding as the certificate.
    const auto text_validator =
        kythira::coap_peer_identity_validator<std::string>({ids<std::string>::client()});
    BOOST_TEST(text_validator(pki.issue(uri_san(ids<std::string>::client())).cert_pem));
    BOOST_TEST(!text_validator(pki.issue(uri_san(ids<std::string>::server())).cert_pem));
}

// A numeric validator keeps the DNS SAN form and refuses the URI form.
BOOST_AUTO_TEST_CASE(numeric_validator_matches_dns_sans_only) {
    pki_dir pki;
    const auto validator = kythira::coap_peer_identity_validator<std::uint64_t>({7});
    BOOST_TEST(validator(pki.issue("DNS:" + raft::testing::peer_identity_dns_name(7)).cert_pem));
    BOOST_TEST(!validator(pki.issue("DNS:" + raft::testing::peer_identity_dns_name(8)).cert_pem));
    BOOST_TEST(!validator(pki.issue("URI:" + raft::testing::peer_identity_uri("7")).cert_pem));
}

#ifdef LIBCOAP_AVAILABLE

// Over a live libcoap DTLS-PKI session, each side binds the other to its node
// id. A client whose certificate names a different id never reaches a handler.
BOOST_AUTO_TEST_CASE(dtls_pki_binds_peers_to_composite_ids,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    using node_id = kythira::aws_ec2_node_id;
    using types = test_types<json_ser>;
    pki_dir pki;
    const auto server_leaf = pki.issue(uri_san(ids<node_id>::server()));
    const auto client_leaf = pki.issue(uri_san(ids<node_id>::client()));
    const auto stranger_leaf = pki.issue(uri_san(ids<node_id>::stranger()));

    auto security = [&](const pki_dir::leaf& self, const node_id& expected_peer) {
        kythira::coap_security_config config;
        config.mode = kythira::coap_auth_mode::dtls_pki;
        kythira::pki_credentials creds;
        creds.cert_file = self.cert_file;
        creds.key_file = self.key_file;
        creds.ca_file = pki.ca_file;
        creds.verify_peer_cert = true;
        creds.cn_validator = kythira::coap_peer_identity_validator<node_id>({expected_peer});
        config.credentials = std::move(creds);
        return config;
    };

    kythira::coap_server_config server_config;
    server_config.enable_dtls = true;
    server_config.security = security(server_leaf, ids<node_id>::client());
    kythira::coap_server<types, node_id> server(loopback, 0, server_config,
                                                kythira::noop_metrics{});
    auto handled = std::make_shared<std::atomic<int>>(0);
    server.register_request_vote_handler(
        [handled](const kythira::request_vote_request<node_id>& r) {
            handled->fetch_add(1);
            return kythira::request_vote_response<>{
                ._term = r.term(), ._vote_granted = r.candidate_id() == ids<node_id>::client()};
        });
    server.start();
    const auto coaps = endpoint("coaps", server.bound_port());

    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.max_retransmit = 2;
        config.security = security(client_leaf, ids<node_id>::server());
        kythira::coap_client<types, node_id> client({{ids<node_id>::server(), coaps}}, config,
                                                    kythira::noop_metrics{});
        const auto vote = wait_get(
            client.send_request_vote(ids<node_id>::server(),
                                     kythira::request_vote_request<node_id>{
                                         ._term = 2, ._candidate_id = ids<node_id>::client()},
                                     kythira::testing::scaled_deadline(10000)));
        BOOST_TEST(vote.vote_granted());
        BOOST_TEST(handled->load() == 1);
    }

    {
        kythira::coap_client_config config;
        config.enable_dtls = true;
        config.max_retransmit = 1;
        config.security = security(stranger_leaf, ids<node_id>::server());
        kythira::coap_client<types, node_id> client({{ids<node_id>::server(), coaps}}, config,
                                                    kythira::noop_metrics{});
        auto future =
            client.send_request_vote(ids<node_id>::server(),
                                     kythira::request_vote_request<node_id>{
                                         ._term = 3, ._candidate_id = ids<node_id>::client()},
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

#endif  // LIBCOAP_AVAILABLE

BOOST_AUTO_TEST_SUITE_END()
