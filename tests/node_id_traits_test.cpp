// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Requirement 14.2 of the cloud-composite-node-ids spec: node_id_traits for
// numeric, string and composite ids, and next_numeric_node_id.

#define BOOST_TEST_MODULE node_id_traits_test
#include <boost/test/unit_test.hpp>

#include <raft/composite_node_id.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace kythira;

static_assert(!node_id_traits<std::uint64_t>::is_textual);
static_assert(node_id_traits<std::string>::is_textual);
static_assert(node_id_traits<aws_ec2_node_id>::is_textual);

BOOST_AUTO_TEST_CASE(numeric_round_trip) {
    using t = node_id_traits<std::uint64_t>;
    BOOST_CHECK_EQUAL(t::to_text(0), "0");
    BOOST_CHECK_EQUAL(t::to_text(42), "42");
    BOOST_CHECK_EQUAL(t::from_text("42").value(), 42u);
    BOOST_CHECK_EQUAL(t::from_text("0").value(), 0u);
    BOOST_CHECK_EQUAL(t::from_text("007").value(), 7u);
    BOOST_CHECK_EQUAL(t::from_text("18446744073709551615").value(),
                      std::numeric_limits<std::uint64_t>::max());
}

BOOST_AUTO_TEST_CASE(numeric_rejects_non_canonical_text) {
    using t = node_id_traits<std::uint64_t>;
    for (const char* text : {"", "-1", "+1", " 1", "1 ", "0x10", "7x", "1.0", "1e3",
                             "18446744073709551616", "99999999999999999999999", "\t3"}) {
        BOOST_CHECK_MESSAGE(!t::from_text(text), "accepted '" << text << "'");
    }
}

BOOST_AUTO_TEST_CASE(narrower_numeric_type_range_checks) {
    using t = node_id_traits<std::uint16_t>;
    BOOST_CHECK_EQUAL(t::from_text("65535").value(), 65535u);
    BOOST_CHECK(!t::from_text("65536"));
    BOOST_CHECK(!t::from_text("4294967296"));
}

BOOST_AUTO_TEST_CASE(string_ids) {
    using t = node_id_traits<std::string>;
    BOOST_CHECK_EQUAL(t::to_text("node-a"), "node-a");
    BOOST_CHECK_EQUAL(t::from_text("aws-ec2:us-east-1:i-1234abcd").value(),
                      "aws-ec2:us-east-1:i-1234abcd");
    BOOST_CHECK(!t::from_text(""));
}

BOOST_AUTO_TEST_CASE(composite_ids) {
    using t = node_id_traits<aws_ec2_node_id>;
    aws_ec2_node_id id{"us-east-1", "i-f0123456789abcdef"};
    BOOST_CHECK_EQUAL(t::to_text(id), "aws-ec2:us-east-1:i-f0123456789abcdef");
    BOOST_CHECK(t::from_text(t::to_text(id)).value() == id);
    BOOST_CHECK(!t::from_text("17361641481138401520"));
}

BOOST_AUTO_TEST_CASE(next_numeric_increments) {
    BOOST_CHECK_EQUAL(next_numeric_node_id<std::uint64_t>(0), 1u);
    BOOST_CHECK_EQUAL(next_numeric_node_id<std::uint64_t>(41), 42u);
    BOOST_CHECK_EQUAL(next_numeric_node_id<std::uint8_t>(254), 255u);
}

BOOST_AUTO_TEST_CASE(next_numeric_refuses_to_wrap) {
    BOOST_CHECK_THROW(next_numeric_node_id(std::numeric_limits<std::uint64_t>::max()),
                      std::overflow_error);
    BOOST_CHECK_THROW(next_numeric_node_id(std::numeric_limits<std::uint8_t>::max()),
                      std::overflow_error);
    BOOST_CHECK_THROW(next_numeric_node_id(std::numeric_limits<std::uint32_t>::max()),
                      std::overflow_error);
}

// Requirement 7 (numeric hardening): the quorum managers' shared helpers.

static_assert(numeric_node_id_ceiling<std::uint16_t>() == 65535u);
static_assert(numeric_node_id_ceiling<std::string>() == std::numeric_limits<std::uint64_t>::max());

BOOST_AUTO_TEST_CASE(parse_numeric_node_id_is_strict_and_range_checked) {
    BOOST_CHECK_EQUAL(parse_numeric_node_id<std::uint64_t>("7").value(), 7u);
    BOOST_CHECK_EQUAL(parse_numeric_node_id<std::string>("18446744073709551615").value(),
                      std::numeric_limits<std::uint64_t>::max());
    BOOST_CHECK_EQUAL(parse_numeric_node_id<std::uint16_t>("65535").value(), 65535u);
    for (const char* text : {"", "-1", "7x", " 7", "0x7", "node-a"}) {
        BOOST_CHECK_MESSAGE(!parse_numeric_node_id<std::uint64_t>(text),
                            "accepted '" << text << "'");
    }
    // Past the NodeId type's range: a value a manager of that type could
    // never have written, so it is not one to allocate above.
    BOOST_CHECK(!parse_numeric_node_id<std::uint16_t>("65536"));
}

BOOST_AUTO_TEST_CASE(numeric_node_id_as_refuses_narrowing) {
    BOOST_CHECK_EQUAL(numeric_node_id_as<std::uint64_t>(9), 9u);
    BOOST_CHECK_EQUAL(numeric_node_id_as<std::string>(9), "9");
    BOOST_CHECK_EQUAL(numeric_node_id_as<std::uint16_t>(65535), 65535u);
    BOOST_CHECK_THROW(numeric_node_id_as<std::uint16_t>(65536), std::overflow_error);
    BOOST_CHECK_THROW(numeric_node_id_as<std::uint32_t>(0x1'0000'0001ULL), std::overflow_error);
}

BOOST_AUTO_TEST_CASE(random_numeric_node_id_stays_inside_the_type) {
    std::mt19937_64 gen(42);
    for (int i = 0; i < 10000; ++i) {
        const auto narrow = random_numeric_node_id<std::uint8_t>(gen);
        BOOST_REQUIRE_GE(narrow, 1u);  // never the reserved 0
        const auto wide = random_numeric_node_id<std::uint64_t>(gen);
        BOOST_REQUIRE_GE(wide, 1u);
        BOOST_REQUIRE_LE(wide, std::numeric_limits<std::uint64_t>::max() >> 1);
        const auto text = random_numeric_node_id<std::string>(gen);
        BOOST_REQUIRE(parse_numeric_node_id<std::uint64_t>(text).value() <=
                      (std::numeric_limits<std::uint64_t>::max() >> 1));
    }
}
