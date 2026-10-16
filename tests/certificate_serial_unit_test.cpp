// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE certificate_serial_unit_test
#include <boost/test/unit_test.hpp>

// `certificate_serial` and its OpenSSL conversions
// (.kiro/specs/cloud-composite-node-ids/ Requirement 12.5).

#include <raft/certificate_provider.hpp>
#include <raft/certificate_serial.hpp>
#include <raft/certificate_serial_x509.hpp>

#include "wide_serial_test_support.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <limits>
#include <set>
#include <string>
#include <unordered_set>

using raft::testing::certificate_serial;

// Optionals compared in BOOST_TEST have no operator<<.
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<raft::testing::certificate_serial>)
BOOST_TEST_DONT_PRINT_LOG_VALUE(std::optional<std::uint64_t>)

namespace {

constexpr std::string_view k_twenty = "8081828384858687888990919293949596979814";

}  // namespace

BOOST_AUTO_TEST_CASE(from_u64_round_trips_through_hex) {
    BOOST_TEST(certificate_serial::from_u64(0).to_hex() == "00");
    BOOST_TEST(certificate_serial::from_u64(0).is_zero());
    BOOST_TEST(certificate_serial::from_u64(1).to_hex() == "01");
    BOOST_TEST(certificate_serial::from_u64(0x0a1b2c).to_hex() == "0a1b2c");
    BOOST_TEST(certificate_serial::from_u64(0x0a1b2c).size() == 3U);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    BOOST_TEST(certificate_serial::from_u64(max).to_hex() == "ffffffffffffffff");
    for (std::uint64_t v : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{255},
                            std::uint64_t{256}, std::uint64_t{1} << 63U, max}) {
        auto s = certificate_serial::from_u64(v);
        BOOST_TEST(s.to_u64() == v);
        BOOST_TEST(certificate_serial::from_hex(s.to_hex()) == s);
    }
}

BOOST_AUTO_TEST_CASE(from_hex_accepts_twenty_octets_in_either_case) {
    auto s = certificate_serial::from_hex(k_twenty);
    BOOST_REQUIRE(s.has_value());
    BOOST_TEST(s->size() == 20U);
    BOOST_TEST(s->to_hex() == k_twenty);
    BOOST_TEST(!s->to_u64().has_value());
    BOOST_TEST(certificate_serial::from_hex("8081828384858687888990919293949596979814") ==
               certificate_serial::from_hex("8081828384858687888990919293949596979814"));
    BOOST_TEST(certificate_serial::from_hex("ABcd") == certificate_serial::from_u64(0xabcd));
    // Leading zero octets are not significant.
    BOOST_TEST(certificate_serial::from_hex("0005") == certificate_serial::from_u64(5));
    BOOST_TEST(certificate_serial::from_hex("0005")->to_hex() == "05");
    BOOST_TEST(certificate_serial::from_hex("00")->is_zero());
}

BOOST_AUTO_TEST_CASE(from_hex_rejects_bad_input) {
    // Odd length.
    BOOST_TEST(!certificate_serial::from_hex("abc").has_value());
    BOOST_TEST(!certificate_serial::from_hex("0").has_value());
    // More than 20 octets, even when the extra octet is zero.
    BOOST_TEST(!certificate_serial::from_hex(std::string(k_twenty) + "01").has_value());
    BOOST_TEST(!certificate_serial::from_hex("00" + std::string(k_twenty)).has_value());
    // Not hex, or not only hex.
    for (const char* bad :
         {"", "zz", "0g", "0x01", "-1", "+1", " 01", "01 ", "0a:1b", "0a 1b", "\x01\x02"}) {
        BOOST_TEST(!certificate_serial::from_hex(bad).has_value(), "accepted: " << bad);
    }
}

BOOST_AUTO_TEST_CASE(from_bytes_strips_the_sign_octet_but_caps_the_width) {
    std::array<std::uint8_t, 21> with_sign{};
    with_sign[1] = 0x80;
    with_sign[20] = 0x01;
    auto s = certificate_serial::from_bytes(with_sign);
    BOOST_REQUIRE(s.has_value());
    BOOST_TEST(s->size() == 20U);

    std::array<std::uint8_t, 21> too_wide{};
    too_wide[0] = 0x01;
    BOOST_TEST(!certificate_serial::from_bytes(too_wide).has_value());
    BOOST_TEST(certificate_serial::from_bytes({})->is_zero());
}

BOOST_AUTO_TEST_CASE(parse_accepts_separated_octets) {
    const auto expected = certificate_serial::from_u64(0x0a1b2c);
    BOOST_TEST(certificate_serial::parse("0a1b2c") == expected);
    BOOST_TEST(certificate_serial::parse("0A:1B:2C") == expected);
    BOOST_TEST(certificate_serial::parse("0a 1b 2c") == expected);
    BOOST_TEST(certificate_serial::parse("2c") == certificate_serial::from_u64(0x2c));
    auto wide = certificate_serial::from_hex(k_twenty).value();
    BOOST_TEST(certificate_serial::parse(wide.to_hex(':')) == wide);
    BOOST_TEST(certificate_serial::parse(wide.to_hex(' ')) == wide);
    BOOST_TEST(expected.to_hex(':') == "0a:1b:2c");

    for (const char* bad : {"", ":", "0a:", ":0a", "0a::1b", "0a:1b 2c", "a:1b", "0a:1b2c",
                            "0a  1b", "0g:1b", "0a-1b"}) {
        BOOST_TEST(!certificate_serial::parse(bad).has_value(), "accepted: " << bad);
    }
    // 21 separated octets is still too wide.
    BOOST_TEST(!certificate_serial::parse("01:" + wide.to_hex(':')).has_value());
}

BOOST_AUTO_TEST_CASE(order_is_numeric_and_hash_follows_equality) {
    auto a = certificate_serial::from_u64(0xff);
    auto b = certificate_serial::from_u64(0x100);
    auto wide = certificate_serial::from_hex(k_twenty).value();
    auto wide_plus =
        certificate_serial::from_hex("8081828384858687888990919293949596979815").value();
    BOOST_TEST((a < b));
    BOOST_TEST((b < wide));
    BOOST_TEST((wide < wide_plus));
    BOOST_TEST((certificate_serial{} < a));

    std::set<certificate_serial> ordered{wide_plus, a, wide, b};
    BOOST_TEST((*ordered.begin() == a));
    BOOST_TEST((*ordered.rbegin() == wide_plus));

    std::hash<certificate_serial> h;
    BOOST_TEST(h(*certificate_serial::from_hex("0005")) == h(certificate_serial::from_u64(5)));
    std::unordered_set<certificate_serial> set{a, b, wide, *certificate_serial::from_hex("00ff")};
    BOOST_TEST(set.size() == 3U);
    BOOST_TEST(set.contains(wide));
}

BOOST_AUTO_TEST_CASE(asn1_round_trip_at_full_width) {
    for (const auto& s : {certificate_serial{}, certificate_serial::from_u64(1),
                          certificate_serial::from_u64(std::numeric_limits<std::uint64_t>::max()),
                          certificate_serial::from_hex(k_twenty).value()}) {
        auto asn1 = raft::testing::serial_to_asn1(s);
        BOOST_TEST(raft::testing::serial_from_asn1(asn1.get()) == s);
    }
    // Agrees with OpenSSL's own 64-bit encoding.
    raft::testing::asn1_integer_ptr native{ASN1_INTEGER_new()};
    ASN1_INTEGER_set_uint64(native.get(), 0x8000000000000001ULL);
    BOOST_TEST(raft::testing::serial_from_asn1(native.get()) ==
               certificate_serial::from_u64(0x8000000000000001ULL));
    BOOST_TEST(
        ASN1_INTEGER_cmp(native.get(), raft::testing::serial_to_asn1(
                                           certificate_serial::from_u64(0x8000000000000001ULL))
                                           .get()) == 0);
    // Negative serials are not valid X.509 serials.
    raft::testing::asn1_integer_ptr negative{ASN1_INTEGER_new()};
    ASN1_INTEGER_set_int64(negative.get(), -5);
    BOOST_TEST(!raft::testing::serial_from_asn1(negative.get()).has_value());
}

BOOST_AUTO_TEST_CASE(serial_is_read_from_an_issued_certificate) {
    raft::testing::certificate_authority ca;
    raft::testing::leaf_certificate_options leaf;
    leaf.subject.common_name = "serial";
    leaf.dns_names = {"serial.example.com"};
    auto csr = raft::testing::generate_key_and_csr(leaf);
    raft::testing::csr_signing_options signing;
    signing.dns_names = leaf.dns_names;

    const auto wide = kythira::testing::twenty_octet_serial();
    auto material = kythira::testing::sign_csr_with_serial(ca, csr.csr_pem, signing, wide);
    BOOST_TEST(raft::testing::serial_from_certificate_pem(material.certificate_pem) == wide);
    // The chain's first certificate is the one read.
    BOOST_TEST(raft::testing::serial_from_certificate_pem(material.chain_pem) == wide);

    auto in_house = ca.sign_csr(csr.csr_pem, signing);
    BOOST_TEST(raft::testing::serial_from_certificate_pem(in_house.certificate_pem) ==
               in_house.serial);
    BOOST_TEST(in_house.serial.to_u64().has_value());

    BOOST_TEST(!raft::testing::serial_from_certificate_pem("").has_value());
    BOOST_TEST(!raft::testing::serial_from_certificate_pem("not a pem").has_value());
}
