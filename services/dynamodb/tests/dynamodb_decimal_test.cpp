// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The decimal vector suite (`.kiro/specs/dynamodb-compatible-api/` task 3,
// Requirements 6.2, 6.3 and 21.2): AWS's documented precision, range,
// normalization and comparison cases, exact arithmetic including the
// cancellation and 39th-digit edges, and the key encoding's order. CI runs it
// on x86-64 and arm64; nothing in it may depend on the platform, so the
// randomized cases draw raw mt19937_64 output (whose sequence the standard
// fixes) instead of a distribution (whose mapping it does not).

#define BOOST_TEST_MODULE dynamodb_decimal_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/decimal.hpp>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace kythira::dynamodb {
// Boost.Test prints operands of a failed comparison through this.
inline auto boost_test_print_type(std::ostream& os, const dynamo_decimal& d) -> std::ostream& {
    return os << d.to_string();
}
}  // namespace kythira::dynamodb

namespace ddb = kythira::dynamodb;
using ddb::decimal_status;
using ddb::dynamo_decimal;

namespace {

auto dec(std::string_view s) -> dynamo_decimal {
    dynamo_decimal d;
    const auto st = dynamo_decimal::parse(s, d);
    BOOST_REQUIRE_MESSAGE(st == decimal_status::ok, "parse " << s);
    return d;
}

auto status_of(std::string_view s) -> decimal_status {
    dynamo_decimal d;
    return dynamo_decimal::parse(s, d);
}

auto sum(std::string_view a, std::string_view b, decimal_status want = decimal_status::ok)
    -> std::string {
    dynamo_decimal out;
    const auto st = dynamo_decimal::add(dec(a), dec(b), out);
    BOOST_TEST_INFO(a << " + " << b);
    BOOST_TEST((st == want));
    return st == decimal_status::ok ? out.to_string() : std::string();
}

auto difference(std::string_view a, std::string_view b) -> std::string {
    dynamo_decimal out;
    BOOST_REQUIRE(dynamo_decimal::subtract(dec(a), dec(b), out) == decimal_status::ok);
    return out.to_string();
}

const std::string k_max_positive = "9.9999999999999999999999999999999999999E+125";
const std::string k_38_nines = "99999999999999999999999999999999999999";

}  // namespace

BOOST_AUTO_TEST_CASE(literals_normalize_to_plain_notation) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"0", "0"},
        {"-0", "0"},
        {"+0.000", "0"},
        {"0e999999999999999", "0"},
        {"+5", "5"},
        {"007", "7"},
        {"1.50", "1.5"},
        {".5", "0.5"},
        {"5.", "5"},
        {"1e2", "100"},
        {"1E+2", "100"},
        {"100", "100"},
        {"1.23e-2", "0.0123"},
        {"-12.340", "-12.34"},
        {"12.5E1", "125"},
        {"0.00012300", "0.000123"},
        {"123456.789e-3", "123.456789"},
    };
    for (const auto& [in, want] : cases) {
        BOOST_TEST_INFO(in);
        BOOST_TEST(dec(in).to_string() == want);
    }
}

BOOST_AUTO_TEST_CASE(malformed_literals_are_refused) {
    for (const auto* s : {"",          "-",    "+",     ".",   "-.",    "e5",    "1e",  "1e+",
                          "1e-",       "abc",  " 1",    "1 ",  "1..2",  "1.2.3", "NaN", "Infinity",
                          "-Infinity", "0x10", "1e5.5", "1,5", "1_000", "١"}) {
        BOOST_TEST_INFO("'" << s << "'");
        BOOST_TEST((status_of(s) == decimal_status::not_a_number));
    }
}

BOOST_AUTO_TEST_CASE(thirty_eight_significant_digits_and_no_more) {
    BOOST_TEST(dec("12345678901234567890123456789012345678").digit_count() == 38);
    BOOST_TEST(
        (status_of("123456789012345678901234567890123456789") == decimal_status::too_many_digits));
    // Leading and trailing zeros are not significant.
    BOOST_TEST(dec("000012345678901234567890123456789012345678").digit_count() == 38);
    BOOST_TEST(dec("123456789012345678901234567890123456780").digit_count() == 38);
    BOOST_TEST(dec("1234567890123456789012345678901234567.8000").digit_count() == 38);
    BOOST_TEST((status_of("0.123456789012345678901234567890123456789") ==
                decimal_status::too_many_digits));
}

BOOST_AUTO_TEST_CASE(the_range_is_1e_minus_130_to_9_99e125) {
    BOOST_TEST(dec(k_max_positive).adjusted_exponent() == 125);
    BOOST_TEST(dec("-" + k_max_positive).is_negative());
    BOOST_TEST(dec("1E-130").adjusted_exponent() == -130);
    BOOST_TEST(dec("0.1E-129").adjusted_exponent() == -130);
    BOOST_TEST(dec("1.5E-130").to_string() == dec("15E-131").to_string());
    BOOST_TEST((status_of("1E+126") == decimal_status::overflow));
    BOOST_TEST((status_of("-1E+126") == decimal_status::overflow));
    BOOST_TEST((status_of("10E+125") == decimal_status::overflow));
    BOOST_TEST((status_of("1E-131") == decimal_status::underflow));
    BOOST_TEST((status_of("-0.1E-130") == decimal_status::underflow));
    BOOST_TEST((status_of("1E+999999999999999999999") == decimal_status::overflow));
    BOOST_TEST((status_of("1E-999999999999999999999") == decimal_status::underflow));
    // Too many digits is reported before range.
    BOOST_TEST((status_of("123456789012345678901234567890123456789E+200") ==
                decimal_status::too_many_digits));

    const auto tiny = dec("1E-130").to_string();
    BOOST_TEST(tiny == "0." + std::string(129, '0') + "1");
    const auto huge = dec("1E+125").to_string();
    BOOST_TEST(huge == "1" + std::string(125, '0'));
    BOOST_TEST(dec(tiny) == dec("1E-130"));
    BOOST_TEST(dec(huge) == dec("1E+125"));
}

BOOST_AUTO_TEST_CASE(comparison_is_numeric) {
    BOOST_TEST((dec("1") == dec("1.0")));
    BOOST_TEST((dec("1") == dec("10E-1")));
    BOOST_TEST((dec("0") == dec("-0")));
    const std::vector<std::string> ascending = {
        "-" + k_max_positive,
        "-1E+125",
        "-100",
        "-12.01",
        "-12",
        "-1.5",
        "-1",
        "-0.5",
        "-1E-130",
        "0",
        "1E-130",
        "0.000123",
        "0.5",
        "1",
        "1.5",
        "12",
        "12.01",
        "100",
        k_38_nines,
        "1E+38",
        "1E+125",
        k_max_positive,
    };
    for (std::size_t i = 0; i < ascending.size(); ++i) {
        for (std::size_t j = 0; j < ascending.size(); ++j) {
            const auto a = dec(ascending[i]);
            const auto b = dec(ascending[j]);
            BOOST_TEST_INFO(ascending[i] << " vs " << ascending[j]);
            BOOST_TEST(((a <=> b) == (i <=> j)));
            BOOST_TEST(((a.encode_key() <=> b.encode_key()) == (i <=> j)));
        }
    }
}

BOOST_AUTO_TEST_CASE(addition_and_subtraction_are_exact) {
    BOOST_TEST(sum("1", "2") == "3");
    BOOST_TEST(sum("0.1", "0.2") == "0.3");
    BOOST_TEST(sum("-1", "1") == "0");
    BOOST_TEST(sum("0", "-7.25") == "-7.25");
    BOOST_TEST(sum("1.5", "-0.25") == "1.25");
    BOOST_TEST(sum("-1.5", "0.25") == "-1.25");
    BOOST_TEST(sum("999", "1") == "1000");
    BOOST_TEST(sum(k_38_nines, "1") == "1" + std::string(38, '0'));
    // A 38-digit operand shifted past 38 digits on the grid, cancelled back.
    BOOST_TEST(sum("1E+40", "-" + k_38_nines + "E+2") == "100");
    BOOST_TEST(sum("2E-130", "-1E-130") == dec("1E-130").to_string());
    BOOST_TEST(sum("1E+125", "-1E+125") == "0");

    BOOST_TEST(difference("-5", "3") == "-8");
    BOOST_TEST(difference("5", "8") == "-3");
    BOOST_TEST(difference("1.5", "1.5") == "0");
    BOOST_TEST(difference("0", "0.001") == "-0.001");

    // Refused rather than rounded.
    sum("12345678901234567890123456789012345678", "0.1", decimal_status::too_many_digits);
    sum(k_38_nines, "0.5", decimal_status::too_many_digits);
    sum("1E+20", "1E-20", decimal_status::too_many_digits);
    sum(k_max_positive, "1E+88", decimal_status::overflow);
    sum("-" + k_max_positive, "-1E+88", decimal_status::overflow);
    sum("1.5E-130", "-1.4E-130", decimal_status::underflow);
}

BOOST_AUTO_TEST_CASE(sizes_follow_the_item_size_formula) {
    BOOST_TEST(dec("0").size_bytes() == 2U);
    BOOST_TEST(dec("1").size_bytes() == 2U);
    BOOST_TEST(dec("12").size_bytes() == 2U);
    BOOST_TEST(dec("123").size_bytes() == 3U);
    BOOST_TEST(dec("1.23E+50").size_bytes() == 3U);
    BOOST_TEST(dec("12345678901234567890123456789012345678").size_bytes() == 20U);
}

BOOST_AUTO_TEST_CASE(messages_are_aws_text) {
    BOOST_TEST(dynamo_decimal::message(decimal_status::too_many_digits) ==
               "Attempting to store more than 38 significant digits in a Number");
    BOOST_TEST(dynamo_decimal::message(decimal_status::overflow) ==
               "Number overflow. Attempting to store a number with magnitude larger than "
               "supported range");
    BOOST_TEST(dynamo_decimal::message(decimal_status::underflow) ==
               "Number underflow. Attempting to store a number with magnitude smaller than "
               "supported range");
    BOOST_TEST(dynamo_decimal::message(decimal_status::not_a_number, "abc") ==
               "The parameter cannot be converted to a numeric value: abc");
}

BOOST_AUTO_TEST_CASE(key_decoding_refuses_non_canonical_bytes) {
    for (const std::string& bad : {std::string(), std::string("\x01", 1), std::string("\x81", 1),
                                   std::string("\x81\x82", 2),              // no terminator
                                   std::string("\x81\x82\x00", 3),          // no digits
                                   std::string("\x81\x82\x01\x00", 4),      // leading zero pair
                                   std::string("\x81\x82\x02\x01\x00", 5),  // trailing 00 pair
                                   std::string("\x81\x82\x66\x00", 4),      // pair byte 102
                                   std::string("\x81\xFF\x02\x00", 4)}) {   // exponent 125+
        std::string_view in = bad;
        BOOST_TEST_INFO("bytes " << bad.size());
        BOOST_TEST(!dynamo_decimal::decode_key(in).has_value());
    }
    // Decoding consumes exactly one encoding.
    const auto a = dec("-12.5").encode_key();
    const auto b = dec("7").encode_key();
    const std::string both = a + b;
    std::string_view in = both;
    BOOST_TEST((dynamo_decimal::decode_key(in) == dec("-12.5")));
    BOOST_TEST((dynamo_decimal::decode_key(in) == dec("7")));
    BOOST_TEST(in.empty());
}

namespace {

// A random in-range value: 1-38 digits, adjusted exponent within the range,
// either sign, or zero now and then.
auto random_decimal(std::mt19937_64& rng) -> dynamo_decimal {
    const auto r = rng();
    if (r % 50 == 0) {
        return {};
    }
    const int digits = 1 + static_cast<int>((r >> 8) % 38);
    std::string s = (r >> 20) % 2 == 0 ? "" : "-";
    for (int i = 0; i < digits; ++i) {
        s.push_back(static_cast<char>('0' + ((i == 0 ? 1 : 0) + rng() % (i == 0 ? 9 : 10))));
    }
    // Keep some pairs close together so arithmetic sees overlap.
    const int adjusted = (r >> 32) % 4 == 0 ? static_cast<int>((r >> 40) % 256) - 130
                                            : static_cast<int>((r >> 40) % 41) - 20;
    s += "E" + std::to_string(adjusted - digits + 1);
    return *dynamo_decimal::from_string(s);
}

}  // namespace

BOOST_AUTO_TEST_CASE(random_values_round_trip_and_order_consistently) {
    std::mt19937_64 rng(0x6b797468697261ULL);
    std::vector<dynamo_decimal> values;
    for (int i = 0; i < 4000; ++i) {
        values.push_back(random_decimal(rng));
    }
    for (const auto& v : values) {
        BOOST_TEST_INFO(v.to_string());
        BOOST_TEST((dynamo_decimal::from_string(v.to_string()) == v));
        const auto key = v.encode_key();
        std::string_view in = key;
        BOOST_TEST((dynamo_decimal::decode_key(in) == v));
        BOOST_TEST(in.empty());
    }
    auto by_value = values;
    std::sort(by_value.begin(), by_value.end());
    auto by_key = values;
    std::sort(by_key.begin(), by_key.end(),
              [](const auto& a, const auto& b) { return a.encode_key() < b.encode_key(); });
    BOOST_TEST((by_value == by_key));
}

BOOST_AUTO_TEST_CASE(random_arithmetic_is_consistent) {
    std::mt19937_64 rng(0x64796e616d6fULL);
    int checked = 0;
    for (int i = 0; i < 4000; ++i) {
        const auto a = random_decimal(rng);
        const auto b = random_decimal(rng);
        dynamo_decimal ab;
        dynamo_decimal ba;
        const auto s1 = dynamo_decimal::add(a, b, ab);
        const auto s2 = dynamo_decimal::add(b, a, ba);
        BOOST_TEST_INFO(a.to_string() << " + " << b.to_string());
        BOOST_TEST((s1 == s2));
        if (s1 != decimal_status::ok) {
            continue;
        }
        BOOST_TEST((ab == ba));
        // (a + b) - b == a whenever every step is representable.
        dynamo_decimal back;
        if (dynamo_decimal::subtract(ab, b, back) == decimal_status::ok) {
            BOOST_TEST((back == a));
            ++checked;
        }
        // Adding a positive value never decreases the result.
        if (!b.is_negative()) {
            BOOST_TEST((ab >= a));
        }
    }
    BOOST_TEST(checked > 500);
}
