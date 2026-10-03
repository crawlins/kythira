// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Feature: oci-ca-chain-leaf
//
// Unit tests for include/raft/pem_chain.hpp (.kiro/specs/oci-ca-chain-leaf/,
// Task 1): splitting a PEM bundle, comparing certificates by DER, and
// assembling a leaf-first chain without duplicating the leaf.

#define BOOST_TEST_MODULE PemChainUnitTest
#include <boost/test/unit_test.hpp>

#include <raft/certificate_authority.hpp>
#include <raft/pem_chain.hpp>

#include <stdexcept>
#include <string>

namespace {

using kythira::pem_chain::leaf_first;
using kythira::pem_chain::same_certificate;
using kythira::pem_chain::split_certificates;

struct issued {
    raft::testing::certificate_authority ca;
    raft::testing::pem_material leaf;
    raft::testing::pem_material other;

    issued() {
        raft::testing::leaf_certificate_options o;
        o.subject.common_name = "node-1";
        o.dns_names = {"node-1"};
        leaf = ca.issue(o);
        o.subject.common_name = "node-2";
        o.dns_names = {"node-2"};
        other = ca.issue(o);
    }

    [[nodiscard]] auto root() const -> const std::string& { return ca.root_certificate_pem(); }
};

auto without_trailing_newline(std::string pem) -> std::string {
    while (!pem.empty() && (pem.back() == '\n' || pem.back() == '\r')) {
        pem.pop_back();
    }
    return pem;
}

auto to_crlf(const std::string& pem) -> std::string {
    std::string out;
    for (char c : pem) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// Re-wraps the base64 body at a different width; the DER is unchanged.
auto rewrap(const std::string& pem, std::size_t width) -> std::string {
    const std::string begin = "-----BEGIN CERTIFICATE-----";
    const std::string end = "-----END CERTIFICATE-----";
    const auto body_start = pem.find(begin) + begin.size();
    const auto body_end = pem.find(end);
    std::string base64;
    for (auto i = body_start; i < body_end; ++i) {
        if (pem[i] != '\n' && pem[i] != '\r') base64 += pem[i];
    }
    std::string out = begin + "\n";
    for (std::size_t i = 0; i < base64.size(); i += width) {
        out += base64.substr(i, width) + "\n";
    }
    return out + end + "\n";
}

}  // namespace

BOOST_AUTO_TEST_CASE(split_one_and_several_blocks) {
    issued f;
    auto one = split_certificates(f.leaf.certificate_pem);
    BOOST_REQUIRE_EQUAL(one.size(), 1U);
    BOOST_TEST(one.front().back() == '\n');

    auto three = split_certificates(f.leaf.certificate_pem + f.other.certificate_pem + f.root());
    BOOST_REQUIRE_EQUAL(three.size(), 3U);
    BOOST_TEST(same_certificate(three[0], f.leaf.certificate_pem));
    BOOST_TEST(same_certificate(three[1], f.other.certificate_pem));
    BOOST_TEST(same_certificate(three[2], f.root()));

    BOOST_TEST(split_certificates("").empty());
    BOOST_TEST(split_certificates("no pem here").empty());
}

BOOST_AUTO_TEST_CASE(split_tolerates_missing_newlines_crlf_and_other_blocks) {
    issued f;
    // Two blocks butted together with no newline between them.
    auto joined = split_certificates(without_trailing_newline(f.leaf.certificate_pem) +
                                     without_trailing_newline(f.root()));
    BOOST_REQUIRE_EQUAL(joined.size(), 2U);
    for (const auto& cert : joined) {
        BOOST_TEST(cert.back() == '\n');
    }
    BOOST_TEST(same_certificate(joined[1], f.root()));

    auto crlf = split_certificates(to_crlf(f.leaf.certificate_pem + f.root()));
    BOOST_REQUIRE_EQUAL(crlf.size(), 2U);
    BOOST_TEST(same_certificate(crlf[0], f.leaf.certificate_pem));

    // A private key block in the bundle is ignored.
    auto mixed = split_certificates(f.leaf.certificate_pem + f.leaf.private_key_pem + f.root());
    BOOST_REQUIRE_EQUAL(mixed.size(), 2U);
    BOOST_TEST(same_certificate(mixed[1], f.root()));
}

BOOST_AUTO_TEST_CASE(same_certificate_compares_der_not_text) {
    issued f;
    BOOST_TEST(same_certificate(f.leaf.certificate_pem, f.leaf.certificate_pem));
    const auto rewrapped = rewrap(f.leaf.certificate_pem, 76);
    BOOST_TEST(rewrapped != f.leaf.certificate_pem);
    BOOST_TEST(same_certificate(rewrapped, f.leaf.certificate_pem));
    BOOST_TEST(!same_certificate(f.leaf.certificate_pem, f.other.certificate_pem));
    BOOST_TEST(!same_certificate(f.leaf.certificate_pem, f.root()));
}

BOOST_AUTO_TEST_CASE(leaf_first_prepends_the_leaf_once) {
    issued f;
    // Issuer chain only: the OCI shape.
    auto chain = split_certificates(leaf_first(f.leaf.certificate_pem, f.root()));
    BOOST_REQUIRE_EQUAL(chain.size(), 2U);
    BOOST_TEST(same_certificate(chain[0], f.leaf.certificate_pem));
    BOOST_TEST(same_certificate(chain[1], f.root()));

    // Already leaf-first, even in a different wrapping: no duplicate.
    auto already = split_certificates(
        leaf_first(f.leaf.certificate_pem, rewrap(f.leaf.certificate_pem, 76) + f.root()));
    BOOST_REQUIRE_EQUAL(already.size(), 2U);
    BOOST_TEST(same_certificate(already[0], f.leaf.certificate_pem));

    // No issuers: the leaf alone.
    BOOST_TEST(split_certificates(leaf_first(f.leaf.certificate_pem, "")).size() == 1U);
    BOOST_TEST(split_certificates(leaf_first(f.leaf.certificate_pem, " \n")).size() == 1U);

    // Missing trailing newlines never merge two blocks.
    const auto out = leaf_first(without_trailing_newline(f.leaf.certificate_pem),
                                without_trailing_newline(f.root()));
    BOOST_TEST(out.find("-----END CERTIFICATE----------BEGIN") == std::string::npos);
    BOOST_TEST(out.back() == '\n');
}

BOOST_AUTO_TEST_CASE(malformed_input_throws_invalid_argument) {
    issued f;
    const std::string garbage_block =
        "-----BEGIN CERTIFICATE-----\nnot-base64-der\n-----END CERTIFICATE-----\n";
    BOOST_CHECK_THROW(split_certificates(garbage_block), std::invalid_argument);
    BOOST_CHECK_THROW(split_certificates("-----BEGIN CERTIFICATE-----\nMIIB\n"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(same_certificate(garbage_block, f.root()), std::invalid_argument);
    BOOST_CHECK_THROW(leaf_first(f.leaf.certificate_pem, "not a certificate chain"),
                      std::invalid_argument);
    BOOST_CHECK_THROW(leaf_first("", f.root()), std::invalid_argument);
    BOOST_CHECK_THROW(leaf_first(f.leaf.certificate_pem, garbage_block), std::invalid_argument);
}
