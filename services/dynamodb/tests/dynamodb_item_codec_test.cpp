// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The item codec (`.kiro/specs/dynamodb-compatible-api/` task 3,
// Requirements 6.1, 6.3 and 6.4): attribute values to and from the wire,
// AWS's validation rules and messages, the item-size formula and the 400 KB
// limit, key-attribute checks, the order-preserving key encoding, and the
// binary storage form, including refusing corrupt bytes.

#define BOOST_TEST_MODULE dynamodb_item_codec_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/item_codec.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace ddb = kythira::dynamodb;
namespace json = boost::json;
using ddb::attribute_type;
using ddb::attribute_value;

namespace {

// Deep enough for 33 levels of nested M or L, as the core codec's limit
// (128) allows.
auto parse(std::string_view text) -> json::object {
    json::parse_options opts;
    opts.max_depth = 128;
    return json::parse(text, {}, opts).as_object();
}

auto item_of(std::string_view text) -> ddb::item {
    auto r = ddb::item_from_json(parse(text));
    BOOST_REQUIRE_MESSAGE(r.has_value(),
                          text << ": " << (r ? std::string() : r.error().message.value_or("")));
    return *r;
}

auto error_of(std::string_view text) -> std::string {
    auto r = ddb::item_from_json(parse(text));
    BOOST_REQUIRE_MESSAGE(!r.has_value(), "accepted " << text);
    BOOST_TEST(r.error().type == "com.amazon.coral.validate#ValidationException");
    return r.error().message.value_or("");
}

auto nested(const std::string& kind, int levels) -> std::string {
    std::string open;
    std::string close;
    for (int i = 0; i < levels; ++i) {
        open += kind == "L" ? R"({"L":[)" : R"({"M":{"k":)";
        close += kind == "L" ? "]}" : "}}";
    }
    return R"({"a":)" + open + R"({"S":"x"})" + close + "}";
}

const std::string k_every_type = R"({
    "s": {"S": "héllo"}, "empty": {"S": ""}, "n": {"N": "-12.50"},
    "b": {"B": "AAEC/w=="}, "t": {"BOOL": true}, "f": {"BOOL": false},
    "z": {"NULL": true}, "ss": {"SS": ["b", "a"]}, "ns": {"NS": ["1", "1E+2"]},
    "bs": {"BS": ["AA==", "AQ=="]},
    "l": {"L": [{"N": "1"}, {"S": "x"}, {"L": []}, {"M": {}}]},
    "m": {"M": {"y": {"N": "2"}, "x": {"M": {"deep": {"BOOL": false}}}}}
})";

}  // namespace

BOOST_AUTO_TEST_CASE(every_type_round_trips_through_json) {
    const auto it = item_of(k_every_type);
    BOOST_TEST(it.size() == 12U);
    BOOST_TEST(std::is_sorted(it.begin(), it.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; }));
    BOOST_TEST(ddb::find_attribute(it, "n")->num.to_string() == "-12.5");
    BOOST_TEST(ddb::find_attribute(it, "b")->str == std::string("\x00\x01\x02\xFF", 4));
    BOOST_TEST(ddb::find_attribute(it, "nope") == nullptr);
    const auto* m = ddb::find_attribute(it, "m");
    BOOST_TEST((m->names == std::vector<std::string>{"x", "y"}));  // sorted

    const auto back = ddb::item_to_json(it);
    // Numbers come back normalized; everything else as sent.
    BOOST_TEST(back.at("n").at("N").as_string() == "-12.5");
    BOOST_TEST(back.at("ns").at("NS").as_array()[1].as_string() == "100");
    BOOST_TEST(back.at("b").at("B").as_string() == "AAEC/w==");
    BOOST_TEST(back.at("z").at("NULL").as_bool());
    BOOST_TEST((ddb::item_from_json(back).value() == it));
}

BOOST_AUTO_TEST_CASE(an_attribute_value_has_exactly_one_type) {
    BOOST_TEST(error_of(R"({"a":{}})") ==
               "Supplied AttributeValue is empty, must contain exactly one of the supported "
               "datatypes");
    BOOST_TEST(error_of(R"({"a":{"S":null}})") ==
               "Supplied AttributeValue is empty, must contain exactly one of the supported "
               "datatypes");
    BOOST_TEST(error_of(R"({"a":{"S":"x","N":"1"}})") ==
               "Supplied AttributeValue has more than one datatypes set, must contain exactly "
               "one of the supported datatypes");
    // A JSON null member is absent, so this has exactly one type.
    BOOST_TEST(item_of(R"({"a":{"S":"x","N":null}})").size() == 1U);
}

BOOST_AUTO_TEST_CASE(scalar_rules) {
    BOOST_TEST(error_of(R"({"a":{"NULL":false}})") ==
               "One or more parameter values were invalid: Null attribute value types must have "
               "the value of true");
    BOOST_TEST(error_of(R"({"a":{"N":"abc"}})") ==
               "The parameter cannot be converted to a numeric value: abc");
    BOOST_TEST(error_of(R"({"a":{"N":"1E+126"}})") ==
               "Number overflow. Attempting to store a number with magnitude larger than "
               "supported range");
    BOOST_TEST(error_of(R"({"a":{"NS":["1","123456789012345678901234567890123456789"]}})") ==
               "Attempting to store more than 38 significant digits in a Number");
    // Empty strings and binaries are fine outside keys.
    BOOST_TEST(item_of(R"({"a":{"S":""},"b":{"B":""}})").size() == 2U);
}

BOOST_AUTO_TEST_CASE(sets_are_non_empty_and_duplicate_free) {
    BOOST_TEST(error_of(R"({"a":{"SS":[]}})") ==
               "One or more parameter values were invalid: An string set  may not be empty");
    BOOST_TEST(error_of(R"({"a":{"NS":[]}})") ==
               "One or more parameter values were invalid: An number set  may not be empty");
    BOOST_TEST(error_of(R"({"a":{"BS":[]}})") ==
               "One or more parameter values were invalid: An binary set  may not be empty");
    // DynamoDB words a duplicate differently for each set type (as
    // DynamoDB Local 3.3.2 answers PutItem).
    BOOST_TEST(error_of(R"({"a":{"SS":["a","b","a"]}})") ==
               "One or more parameter values were invalid: Input collection [a, b, a] contains "
               "duplicates");
    // Numerically equal elements are duplicates.
    BOOST_TEST(error_of(R"({"a":{"NS":["1","1.0"]}})") == "Input collection contains duplicates");
    BOOST_TEST(error_of(R"({"a":{"BS":["AA==","AA=="]}})") ==
               "One or more parameter values were invalid: Input collection of type BS contains "
               "duplicates.");
}

BOOST_AUTO_TEST_CASE(nesting_stops_at_32_levels) {
    BOOST_TEST(item_of(nested("L", 32)).size() == 1U);
    BOOST_TEST(item_of(nested("M", 32)).size() == 1U);
    BOOST_TEST(error_of(nested("L", 33)) == "Nesting Levels have exceeded supported limits");
    BOOST_TEST(error_of(nested("M", 33)) == "Nesting Levels have exceeded supported limits");
}

BOOST_AUTO_TEST_CASE(sizes_follow_aws_formula) {
    // name + value: "pk" (2) + "a" (1).
    BOOST_TEST(ddb::item_size(item_of(R"({"pk":{"S":"a"}})")) == 3U);
    // UTF-8 bytes, not characters.
    BOOST_TEST(ddb::item_size(item_of(R"({"s":{"S":"é"}})")) == 3U);
    // N: one byte per two significant digits, plus one.
    BOOST_TEST(ddb::item_size(item_of(R"({"n":{"N":"12345"}})")) == 1U + 4U);
    BOOST_TEST(ddb::item_size(item_of(R"({"b":{"BOOL":true},"z":{"NULL":true}})")) == 4U);
    BOOST_TEST(ddb::item_size(item_of(R"({"ss":{"SS":["ab","c"]}})")) == 2U + 3U);
    BOOST_TEST(ddb::item_size(item_of(R"({"ns":{"NS":["1","123"]}})")) == 2U + 2U + 3U);
    BOOST_TEST(ddb::item_size(item_of(R"({"bs":{"BS":["AAE="]}})")) == 2U + 2U);
    // L and M: 3 bytes, plus per element 1 byte and the element (M: its name).
    BOOST_TEST(ddb::item_size(item_of(R"({"l":{"L":[]}})")) == 1U + 3U);
    BOOST_TEST(ddb::item_size(item_of(R"({"l":{"L":[{"S":"ab"},{"N":"1"}]}})")) ==
               1U + 3U + (1U + 2U) + (1U + 2U));
    BOOST_TEST(ddb::item_size(item_of(R"({"m":{"M":{"a":{"N":"1"}}}})")) == 1U + 3U + 1U + 1U + 2U);
}

BOOST_AUTO_TEST_CASE(items_stop_at_400_kb) {
    // "p" + "x" * n == 409600 exactly is allowed; one more byte is not.
    ddb::item it{{"p", attribute_value::string(std::string(ddb::k_max_item_bytes - 1, 'x'))}};
    BOOST_TEST(ddb::item_size(it) == ddb::k_max_item_bytes);
    BOOST_TEST(!ddb::check_item_size(it).has_value());
    it[0].second.str.push_back('x');
    const auto e = ddb::check_item_size(it);
    BOOST_REQUIRE(e.has_value());
    BOOST_TEST(*e->message == "Item size has exceeded the maximum allowed size");
}

BOOST_AUTO_TEST_CASE(key_attributes_follow_the_schema) {
    ddb::key_schema schema;
    schema.partition_name = "pk";
    schema.partition_type = attribute_type::S;
    schema.sort_name = "sk";
    schema.sort_type = attribute_type::N;
    const auto msg = [&](std::string_view text) -> std::string {
        const auto e = ddb::validate_key_attributes(item_of(text), schema);
        return e ? e->message.value_or("") : std::string("ok");
    };
    BOOST_TEST(msg(R"({"pk":{"S":"a"},"sk":{"N":"1"},"other":{"S":""}})") == "ok");
    BOOST_TEST(msg(R"({"sk":{"N":"1"}})") ==
               "One or more parameter values were invalid: Missing the key pk in the item");
    BOOST_TEST(msg(R"({"pk":{"N":"1"},"sk":{"N":"1"}})") ==
               "One or more parameter values were invalid: Type mismatch for key pk expected: S "
               "actual: N");
    BOOST_TEST(msg(R"({"pk":{"S":""},"sk":{"N":"1"}})") ==
               "One or more parameter values are not valid. The AttributeValue for a key "
               "attribute cannot contain an empty string value. Key: pk");

    const std::string max_pk(ddb::k_max_partition_key_bytes, 'p');
    BOOST_TEST(msg(R"({"pk":{"S":")" + max_pk + R"("},"sk":{"N":"1"}})") == "ok");
    BOOST_TEST(msg(R"({"pk":{"S":")" + max_pk + R"(p"},"sk":{"N":"1"}})") ==
               "One or more parameter values were invalid: Size of hashkey has exceeded the "
               "maximum size limit of2048 bytes");

    schema.sort_type = attribute_type::B;
    ddb::item it{{"pk", attribute_value::string("a")},
                 {"sk", attribute_value::binary(std::string(ddb::k_max_sort_key_bytes, 'b'))}};
    BOOST_TEST(!ddb::validate_key_attributes(it, schema).has_value());
    it[1].second.str.push_back('b');
    BOOST_TEST(*ddb::validate_key_attributes(it, schema)->message ==
               "One or more parameter values were invalid: Aggregated size of all range keys has "
               "exceeded the size limit of 1024 bytes");
    it[1].second.str.clear();
    BOOST_TEST(ddb::validate_key_attributes(it, schema)
                   ->message->ends_with("cannot contain an empty binary value. Key: sk"));
}

BOOST_AUTO_TEST_CASE(a_key_parameter_is_exactly_the_schema) {
    ddb::key_schema schema;
    schema.partition_name = "pk";
    const auto msg = [&](std::string_view text) -> std::string {
        const auto e = ddb::validate_key(item_of(text), schema);
        return e ? e->message.value_or("") : std::string("ok");
    };
    const std::string mismatch =
        "One or more parameter values were invalid: The provided key element does not match the "
        "schema";
    BOOST_TEST(msg(R"({"pk":{"S":"a"}})") == "ok");
    BOOST_TEST(msg(R"({"pk":{"S":"a"},"x":{"S":"b"}})") == mismatch);
    BOOST_TEST(msg(R"({"x":{"S":"b"}})") == mismatch);
    BOOST_TEST(msg(R"({"pk":{"N":"1"}})") == mismatch);
}

BOOST_AUTO_TEST_CASE(key_encoding_preserves_byte_order) {
    // S and B compare as unsigned bytes, a prefix first.
    std::vector<std::string> raw = {"",
                                    std::string("\0", 1),
                                    std::string("\0\0", 2),
                                    "a",
                                    std::string("a\0", 2),
                                    std::string("a\0b", 3),
                                    "ab",
                                    "b",
                                    "\x7F",
                                    "\x80",
                                    "\xFF",
                                    "\xFF\xFF"};
    std::sort(raw.begin(), raw.end());  // char_traits<char> compares as unsigned char
    for (std::size_t i = 0; i < raw.size(); ++i) {
        for (std::size_t j = 0; j < raw.size(); ++j) {
            const auto a = ddb::encode_key_component(attribute_value::binary(raw[i]));
            const auto b = ddb::encode_key_component(attribute_value::binary(raw[j]));
            BOOST_TEST_INFO(i << " vs " << j);
            BOOST_TEST(((a <=> b) == (i <=> j)));
        }
        const auto enc = ddb::encode_key_component(attribute_value::string(raw[i]));
        std::string_view in = enc;
        BOOST_TEST(ddb::decode_key_bytes(in).value() == raw[i]);
        BOOST_TEST(in.empty());
    }

    // Composite (partition, sort) keys sort by partition, then sort key,
    // whatever the components' lengths.
    struct row {
        std::string pk;
        std::string sk;
    };
    std::vector<row> rows = {{"a", "10"}, {"a", "9"},   {"a", "-1"},  {"ab", "0"},
                             {"", "5"},   {"a", "0.5"}, {"b", "-100"}};
    const auto composite = [](const row& r) {
        return ddb::encode_key_component(attribute_value::string(r.pk)) +
               ddb::encode_key_component(
                   attribute_value::number(*ddb::dynamo_decimal::from_string(r.sk)));
    };
    auto by_value = rows;
    std::sort(by_value.begin(), by_value.end(), [](const row& x, const row& y) {
        if (x.pk != y.pk) {
            return x.pk < y.pk;
        }
        return *ddb::dynamo_decimal::from_string(x.sk) < *ddb::dynamo_decimal::from_string(y.sk);
    });
    auto by_key = rows;
    std::sort(by_key.begin(), by_key.end(),
              [&](const row& x, const row& y) { return composite(x) < composite(y); });
    for (std::size_t i = 0; i < rows.size(); ++i) {
        BOOST_TEST(by_value[i].pk == by_key[i].pk);
        BOOST_TEST(by_value[i].sk == by_key[i].sk);
    }

    for (const std::string& bad :
         {std::string("abc"), std::string("a\0", 2), std::string("a\0\x02", 3)}) {
        std::string_view in = bad;
        BOOST_TEST(!ddb::decode_key_bytes(in).has_value());
    }
}

BOOST_AUTO_TEST_CASE(the_storage_form_round_trips) {
    const auto it = item_of(k_every_type);
    const auto bytes = ddb::encode_item(it);
    BOOST_TEST((ddb::decode_item(bytes).value() == it));
    BOOST_TEST(ddb::decode_item(ddb::encode_item({})).value().empty());
    // Deterministic: the same item encodes to the same bytes.
    BOOST_TEST(ddb::encode_item(item_of(k_every_type)) == bytes);
    // Deep nesting round trips up to the limit.
    const auto deep = item_of(nested("M", 32));
    BOOST_TEST((ddb::decode_item(ddb::encode_item(deep)).value() == deep));
}

BOOST_AUTO_TEST_CASE(the_storage_form_refuses_corrupt_bytes) {
    const auto bytes = ddb::encode_item(item_of(k_every_type));
    // Every truncation fails.
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        BOOST_TEST_INFO("prefix " << n);
        BOOST_TEST(!ddb::decode_item(std::string_view(bytes).substr(0, n)).has_value());
    }
    BOOST_TEST(!ddb::decode_item(bytes + "x").has_value());  // trailing byte
    auto wrong_version = bytes;
    wrong_version[0] = '\x02';
    BOOST_TEST(!ddb::decode_item(wrong_version).has_value());
    // One attribute named "a" with an unknown tag.
    BOOST_TEST(!ddb::decode_item(std::string("\x01\x01\x01"
                                             "a\x0C",
                                             5))
                    .has_value());
    // A count far larger than the bytes behind it.
    BOOST_TEST(!ddb::decode_item(std::string("\x01\xFF\xFF\xFF\xFF\x0F", 6)).has_value());
    // Nesting deeper than 32.
    std::string deep(
        "\x01\x01\x01"
        "a",
        4);
    for (int i = 0; i < 33; ++i) {
        deep += std::string("\x08\x01", 2);  // L with one element
    }
    deep += std::string("\x06", 1);  // NULL
    BOOST_TEST(!ddb::decode_item(deep).has_value());

    // Random byte flips never crash; whatever decodes re-encodes to itself.
    std::mt19937_64 rng(42);
    for (int i = 0; i < 2000; ++i) {
        auto mutated = bytes;
        mutated[rng() % mutated.size()] = static_cast<char>(rng() & 0xFFU);
        if (const auto d = ddb::decode_item(mutated)) {
            BOOST_TEST((ddb::decode_item(ddb::encode_item(*d)).value() == *d));
        }
    }
}
