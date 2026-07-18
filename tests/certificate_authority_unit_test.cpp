#define BOOST_TEST_MODULE certificate_authority_unit_test

#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

using namespace raft::testing;

namespace {

struct x509_deleter {
    void operator()(X509* c) const {
        if (c != nullptr) {
            X509_free(c);
        }
    }
};
using x509_ptr = std::unique_ptr<X509, x509_deleter>;

auto load_cert(const std::string& pem) -> x509_ptr {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return x509_ptr{cert};
}

auto extension_value(X509* cert, int nid) -> std::string {
    int idx = X509_get_ext_by_NID(cert, nid, -1);
    if (idx < 0) {
        return "";
    }
    X509_EXTENSION* ext = X509_get_ext(cert, idx);
    BIO* bio = BIO_new(BIO_s_mem());
    X509V3_EXT_print(bio, ext, 0, 0);
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    std::string out(mem->data, mem->length);
    BIO_free(bio);
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(construct_ca_for_every_key_algorithm, *boost::unit_test::timeout(30)) {
    for (auto algo : {key_algorithm::rsa_2048, key_algorithm::rsa_4096, key_algorithm::ecdsa_p256,
                      key_algorithm::ecdsa_p384}) {
        ca_options opts;
        opts.algorithm = algo;
        certificate_authority ca(opts);
        BOOST_TEST(!ca.root_certificate_pem().empty());

        auto root = load_cert(ca.root_certificate_pem());
        BOOST_REQUIRE(root != nullptr);
        BOOST_TEST(extension_value(root.get(), NID_basic_constraints).find("CA:TRUE") !=
                   std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(issue_leaf_key_usage_and_eku_match_options, *boost::unit_test::timeout(30)) {
    certificate_authority ca;

    struct case_t {
        bool server_auth;
        bool client_auth;
    };
    for (auto c : std::array<case_t, 3>{{{true, false}, {false, true}, {true, true}}}) {
        leaf_certificate_options opts;
        opts.subject.common_name = "leaf";
        opts.dns_names = {"leaf.example.com"};
        opts.server_auth = c.server_auth;
        opts.client_auth = c.client_auth;

        auto material = ca.issue(opts);
        auto leaf = load_cert(material.certificate_pem);
        BOOST_REQUIRE(leaf != nullptr);

        BOOST_TEST(extension_value(leaf.get(), NID_basic_constraints).find("CA:FALSE") !=
                   std::string::npos);

        auto eku = extension_value(leaf.get(), NID_ext_key_usage);
        BOOST_TEST((eku.find("TLS Web Server Authentication") != std::string::npos) ==
                   c.server_auth);
        BOOST_TEST((eku.find("TLS Web Client Authentication") != std::string::npos) ==
                   c.client_auth);

        auto san = extension_value(leaf.get(), NID_subject_alt_name);
        BOOST_TEST(san.find("leaf.example.com") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(issue_leaf_with_ip_san, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "leaf-ip";
    opts.ip_addresses = {"127.0.0.1"};
    auto material = ca.issue(opts);
    auto leaf = load_cert(material.certificate_pem);
    auto san = extension_value(leaf.get(), NID_subject_alt_name);
    BOOST_TEST(san.find("127.0.0.1") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(issue_throws_when_no_san_entries, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "no-san";
    BOOST_CHECK_THROW(ca.issue(opts), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(revoke_unknown_serial_throws, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    pem_material fake;
    fake.serial = 0xDEADBEEF;
    BOOST_CHECK_THROW(ca.revoke(fake), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(temp_cert_files_key_mode_and_cleanup, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "files";
    opts.dns_names = {"files.example.com"};
    auto material = ca.issue(opts);

    std::string cert_path;
    std::string key_path;
    std::string chain_path;
    {
        temp_cert_files files(material);
        cert_path = files.cert_path();
        key_path = files.key_path();
        chain_path = files.chain_path();

        BOOST_TEST(std::filesystem::exists(cert_path));
        BOOST_TEST(std::filesystem::exists(key_path));
        BOOST_TEST(!chain_path.empty());
        BOOST_TEST(std::filesystem::exists(chain_path));

        struct stat st{};
        BOOST_REQUIRE(::stat(key_path.c_str(), &st) == 0);
        BOOST_TEST((st.st_mode & 0777) == 0600);
    }
    BOOST_TEST(!std::filesystem::exists(cert_path));
    BOOST_TEST(!std::filesystem::exists(key_path));
}

BOOST_AUTO_TEST_CASE(temp_cert_files_no_path_collisions, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "collide";
    opts.dns_names = {"collide.example.com"};

    auto m1 = ca.issue(opts);
    auto m2 = ca.issue(opts);
    temp_cert_files f1(m1);
    temp_cert_files f2(m2);
    BOOST_TEST(f1.cert_path() != f2.cert_path());
}

BOOST_AUTO_TEST_CASE(temp_cert_files_survives_throwing_scope, *boost::unit_test::timeout(30)) {
    certificate_authority ca;
    leaf_certificate_options opts;
    opts.subject.common_name = "throwing";
    opts.dns_names = {"throwing.example.com"};
    auto material = ca.issue(opts);

    std::string cert_path;
    try {
        temp_cert_files files(material);
        cert_path = files.cert_path();
        BOOST_TEST(std::filesystem::exists(cert_path));
        throw std::runtime_error("boom");
    } catch (const std::runtime_error&) {
        // expected
    }
    BOOST_TEST(!std::filesystem::exists(cert_path));
}
