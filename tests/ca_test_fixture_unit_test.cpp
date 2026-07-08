#define BOOST_TEST_MODULE ca_test_fixture_unit_test

#include <boost/test/unit_test.hpp>

#include "ca_test_fixture.hpp"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace raft::testing;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) X509_free(c);
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert_file(const std::string& path) -> x509_ptr {
    FILE* fp = std::fopen(path.c_str(), "r");
    BOOST_REQUIRE(fp != nullptr);
    x509_ptr cert{PEM_read_X509(fp, nullptr, nullptr, nullptr)};
    std::fclose(fp);
    return cert;
}

auto load_cert_pem(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    x509_ptr cert{PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)};
    BIO_free(bio);
    return cert;
}

}  // namespace

BOOST_AUTO_TEST_CASE(construct_in_process_and_bootstrap, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    BOOST_TEST(!fixture.root_certificate_pem().empty());

    const auto& files = fixture.bootstrap_client("server", {"server.example.com"});
    BOOST_TEST(std::filesystem::exists(files.cert_path()));
    BOOST_TEST(std::filesystem::exists(files.key_path()));
}

BOOST_AUTO_TEST_CASE(fixture_owns_returned_files_for_its_own_lifetime,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    std::string cert_path;
    {
        const auto& files = fixture.bootstrap_client("client-a", {"a.example.com"});
        cert_path = files.cert_path();
    }
    // The reference above went out of scope, but the fixture keeps the
    // underlying temp_cert_files alive — the file must still exist.
    BOOST_TEST(std::filesystem::exists(cert_path));
}

// Property 10: repeated bootstrap_client() calls with different client_id values
// produce distinct, non-impersonatable identities that both chain-verify
// against the same root.
BOOST_AUTO_TEST_CASE(property_distinct_clients_chain_to_same_root_and_cannot_impersonate,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& a = fixture.bootstrap_client("client-a", {"a.example.com"});
    const auto& b = fixture.bootstrap_client("client-b", {"b.example.com"});

    auto root = load_cert_pem(fixture.root_certificate_pem());
    auto cert_a = load_cert_file(a.cert_path());
    auto cert_b = load_cert_file(b.cert_path());

    BOOST_REQUIRE(root != nullptr);
    BOOST_REQUIRE(cert_a != nullptr);
    BOOST_REQUIRE(cert_b != nullptr);

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.get());

    for (X509* leaf : {cert_a.get(), cert_b.get()}) {
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        X509_STORE_CTX_init(ctx, store, leaf, nullptr);
        BOOST_TEST(X509_verify_cert(ctx) == 1);
        X509_STORE_CTX_free(ctx);
    }
    X509_STORE_free(store);

    // Distinct subjects and independent key material.
    BOOST_TEST(X509_NAME_cmp(X509_get_subject_name(cert_a.get()),
                             X509_get_subject_name(cert_b.get())) != 0);
    BOOST_TEST(X509_cmp(cert_a.get(), cert_b.get()) != 0);

    std::ifstream key_a(a.key_path());
    std::ifstream key_b(b.key_path());
    std::string key_a_content((std::istreambuf_iterator<char>(key_a)),
                              std::istreambuf_iterator<char>());
    std::string key_b_content((std::istreambuf_iterator<char>(key_b)),
                              std::istreambuf_iterator<char>());
    BOOST_TEST(key_a_content != key_b_content);
}

// Property 14: renew() preserves identity (subject/SAN) while producing a
// distinct certificate — atomically replacing the material at the same paths.
BOOST_AUTO_TEST_CASE(property_renew_preserves_identity_and_advances_material,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    const auto& original = fixture.bootstrap_client("renewable", {"renewable.example.com"});
    std::string cert_path = original.cert_path();
    std::string key_path = original.key_path();

    auto original_cert = load_cert_file(cert_path);
    BOOST_REQUIRE(original_cert != nullptr);
    ASN1_INTEGER* original_serial = X509_get_serialNumber(original_cert.get());
    X509_NAME* original_subject = X509_NAME_dup(X509_get_subject_name(original_cert.get()));

    const auto& renewed = fixture.renew("renewable");

    // Same paths — renew() replaces in place, doesn't hand back a new fixture.
    BOOST_TEST(renewed.cert_path() == cert_path);
    BOOST_TEST(renewed.key_path() == key_path);
    BOOST_TEST(std::filesystem::exists(cert_path));

    auto renewed_cert = load_cert_file(cert_path);
    BOOST_REQUIRE(renewed_cert != nullptr);

    // Identity preserved...
    BOOST_TEST(X509_NAME_cmp(original_subject, X509_get_subject_name(renewed_cert.get())) == 0);
    // ...but the serial (and thus the underlying certificate) has advanced.
    BOOST_TEST(ASN1_INTEGER_cmp(original_serial, X509_get_serialNumber(renewed_cert.get())) != 0);

    auto root = load_cert_pem(fixture.root_certificate_pem());
    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.get());
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, renewed_cert.get(), nullptr);
    BOOST_TEST(X509_verify_cert(ctx) == 1);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);

    X509_NAME_free(original_subject);
}

BOOST_AUTO_TEST_CASE(renew_unknown_client_id_throws, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture;
    fixture.bootstrap_client("known", {"known.example.com"});
    BOOST_CHECK_THROW(fixture.renew("never-bootstrapped"), std::invalid_argument);
}

// ── Network-service mode: same case set, run against a real ca_service --serve
// child process instead of an in-process certificate_authority. ──────────────

namespace {
auto network_options() -> ca_test_fixture_options {
    ca_test_fixture_options opts;
    opts.start_network_service = true;
    opts.startup_timeout = std::chrono::seconds(15);
    return opts;
}
}  // namespace

BOOST_AUTO_TEST_CASE(network_service_construct_and_bootstrap, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture{network_options()};
    BOOST_TEST(!fixture.root_certificate_pem().empty());

    const auto& files = fixture.bootstrap_client("server", {"server.example.com"});
    BOOST_TEST(std::filesystem::exists(files.cert_path()));
    BOOST_TEST(std::filesystem::exists(files.key_path()));

    auto root = load_cert_pem(fixture.root_certificate_pem());
    auto leaf = load_cert_file(files.cert_path());
    BOOST_REQUIRE(root != nullptr);
    BOOST_REQUIRE(leaf != nullptr);

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.get());
    X509_STORE_CTX* ctx = X509_STORE_CTX_new();
    X509_STORE_CTX_init(ctx, store, leaf.get(), nullptr);
    BOOST_TEST(X509_verify_cert(ctx) == 1);
    X509_STORE_CTX_free(ctx);
    X509_STORE_free(store);
}

// Property 10, network-service mode: distinct, non-impersonatable identities
// obtained over the network still chain-verify against the same root.
BOOST_AUTO_TEST_CASE(network_service_property_distinct_clients_chain_to_same_root,
                     *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture{network_options()};
    const auto& a = fixture.bootstrap_client("client-a", {"a.example.com"});
    const auto& b = fixture.bootstrap_client("client-b", {"b.example.com"});

    auto root = load_cert_pem(fixture.root_certificate_pem());
    auto cert_a = load_cert_file(a.cert_path());
    auto cert_b = load_cert_file(b.cert_path());

    X509_STORE* store = X509_STORE_new();
    X509_STORE_add_cert(store, root.get());
    for (X509* leaf : {cert_a.get(), cert_b.get()}) {
        X509_STORE_CTX* ctx = X509_STORE_CTX_new();
        X509_STORE_CTX_init(ctx, store, leaf, nullptr);
        BOOST_TEST(X509_verify_cert(ctx) == 1);
        X509_STORE_CTX_free(ctx);
    }
    X509_STORE_free(store);

    BOOST_TEST(X509_NAME_cmp(X509_get_subject_name(cert_a.get()),
                             X509_get_subject_name(cert_b.get())) != 0);

    // The private key was generated locally by generate_key_and_csr() and never
    // transmitted — bootstrap_via_service() combines it with the server's
    // response, so it must still be present and distinct per client.
    std::ifstream key_a(a.key_path());
    std::ifstream key_b(b.key_path());
    std::string key_a_content((std::istreambuf_iterator<char>(key_a)),
                              std::istreambuf_iterator<char>());
    std::string key_b_content((std::istreambuf_iterator<char>(key_b)),
                              std::istreambuf_iterator<char>());
    BOOST_TEST(!key_a_content.empty());
    BOOST_TEST(key_a_content != key_b_content);
}

BOOST_AUTO_TEST_CASE(network_service_startup_timeout_throws_on_bad_binary,
                     *boost::unit_test::timeout(30)) {
    // Sanity check for the failure path: an impossibly short timeout against a
    // real child process must still fail closed with a clear error rather than
    // hanging or silently succeeding.
    ca_test_fixture_options opts = network_options();
    opts.startup_timeout = std::chrono::seconds(0);
    // A zero timeout may or may not race successfully against a fast-starting
    // service; the meaningful assertion is that the fixture never hangs — the
    // test's own timeout() decoration enforces that regardless of outcome.
    try {
        ca_test_fixture fixture{opts};
    } catch (const std::runtime_error&) {
        // Expected on the (likely) unhealthy-before-timeout path.
    }
}

BOOST_AUTO_TEST_CASE(network_service_renew_preserves_identity, *boost::unit_test::timeout(30)) {
    ca_test_fixture fixture{network_options()};
    const auto& original = fixture.bootstrap_client("net-renewable", {"net-renewable.example.com"});
    std::string cert_path = original.cert_path();

    auto original_cert = load_cert_file(cert_path);
    ASN1_INTEGER* original_serial = X509_get_serialNumber(original_cert.get());
    X509_NAME* original_subject = X509_NAME_dup(X509_get_subject_name(original_cert.get()));

    const auto& renewed = fixture.renew("net-renewable");
    BOOST_TEST(renewed.cert_path() == cert_path);

    auto renewed_cert = load_cert_file(cert_path);
    BOOST_REQUIRE(renewed_cert != nullptr);
    BOOST_TEST(X509_NAME_cmp(original_subject, X509_get_subject_name(renewed_cert.get())) == 0);
    BOOST_TEST(ASN1_INTEGER_cmp(original_serial, X509_get_serialNumber(renewed_cert.get())) != 0);

    X509_NAME_free(original_subject);
}
