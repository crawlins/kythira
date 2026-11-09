// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The AWS JSON protocol codec (`.kiro/specs/dynamodb-compatible-api/` task 2,
// Requirement 4; shared with kinesis-compatible-streams): the generated
// model tables, validation in AWS's two passes, framing, the standard
// response headers and dispatch. The model is a small fixture
// (aws_json_protocol/fixture-service-2.json) that uses every shape kind and
// constraint the codec understands; the real services' models get their own
// tests in their modules.

#define BOOST_TEST_MODULE aws_json_protocol_test

#include <boost/test/unit_test.hpp>

#include <raft/aws_json_protocol.hpp>

#include "aws_json_protocol/fixture_model.hpp"

#include <cstddef>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aj = kythira::aws_json;
namespace fx = kythira::aws_json_fixture;

namespace {

auto make_codec(aj::error_style style = aj::error_style::coral("com.example.fixture.v20261109"),
                aj::unknown_member_policy unknown = aj::unknown_member_policy::reject)
    -> aj::codec {
    aj::codec_options o;
    o.style = std::move(style);
    o.unknown_members = unknown;
    return aj::codec(fx::k_model, std::move(o));
}

auto request(std::string_view target, std::string_view body) -> aj::http_request_view {
    return aj::http_request_view{"POST", "/", "application/x-amz-json-1.0", target, body};
}

auto put(std::string_view body) -> aj::http_request_view {
    return request("Fixture_20261109.PutThing", body);
}

auto parse_error(const aj::codec& c, const aj::http_request_view& r) -> aj::error {
    auto p = c.parse(r);
    BOOST_REQUIRE(!p.has_value());
    return p.error();
}

auto bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const char ch : s) {
        out.push_back(static_cast<std::byte>(ch));
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(model_tables)

BOOST_AUTO_TEST_CASE(shapes_and_operations_are_sorted_and_found) {
    const auto& m = fx::k_model;
    BOOST_TEST(m.target_prefix == "Fixture_20261109");
    BOOST_TEST(m.operations.size() == 3U);
    for (std::size_t i = 1; i < m.shapes.size(); ++i) {
        BOOST_TEST((m.shapes[i - 1].name < m.shapes[i].name));
    }
    BOOST_REQUIRE(m.find_operation("PutThing") != nullptr);
    BOOST_TEST(m.find_operation("Nope") == nullptr);
    BOOST_TEST(m.find_operation("Ping")->input == aj::no_shape);
    const auto idx = m.find_shape("PutThingInput");
    BOOST_REQUIRE(idx != aj::no_shape);
    BOOST_TEST(m.find_operation("PutThing")->input == idx);
    BOOST_TEST(m.find_shape("Zzz") == aj::no_shape);
}

BOOST_AUTO_TEST_CASE(members_keep_model_order_and_required_flags) {
    const auto& m = fx::k_model;
    const auto ms = m.members_of(m.shapes[m.find_shape("PutThingInput")]);
    BOOST_REQUIRE(ms.size() == 13U);
    BOOST_TEST(ms[0].name == "ThingName");
    BOOST_TEST(ms[0].required);
    BOOST_TEST(ms[1].name == "Kind");
    BOOST_TEST(ms[1].required);
    BOOST_TEST(ms[2].name == "Count");
    BOOST_TEST(!ms[2].required);
    const auto kinds = m.enum_of(m.shapes[m.find_shape("ThingKind")]);
    BOOST_REQUIRE(kinds.size() == 2U);
    BOOST_TEST(kinds[0] == "SMALL");
}

BOOST_AUTO_TEST_CASE(exception_and_pattern_flags) {
    const auto& m = fx::k_model;
    const auto& ise = m.shapes[m.find_shape("InternalServerError")];
    BOOST_TEST(ise.is(aj::shape_flag_exception));
    BOOST_TEST(ise.is(aj::shape_flag_fault));
    const auto& nf = m.shapes[m.find_shape("ThingNotFoundException")];
    BOOST_TEST(nf.is(aj::shape_flag_exception));
    BOOST_TEST(!nf.is(aj::shape_flag_fault));
    // \_ is rewritten for ECMAScript; \p{Print} is Java-only and flagged.
    BOOST_TEST(m.shapes[m.find_shape("ThingName")].pattern == "[a-zA-Z0-9_.-]+");
    BOOST_TEST(m.shapes[m.find_shape("Label")].is(aj::shape_flag_pattern_unchecked));
    const aj::shape_validator v(m);
    BOOST_REQUIRE(v.skipped_patterns().size() == 1U);
    BOOST_TEST(v.skipped_patterns()[0] == "Label");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(helpers)

BOOST_AUTO_TEST_CASE(base64_round_trips_rfc4648_vectors) {
    const std::vector<std::pair<std::string, std::string>> vectors = {
        {"", ""},
        {"f", "Zg=="},
        {"fo", "Zm8="},
        {"foo", "Zm9v"},
        {"foob", "Zm9vYg=="},
        {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& [plain, enc] : vectors) {
        BOOST_TEST(aj::base64_encode(bytes(plain)) == enc);
        const auto dec = aj::base64_decode(enc);
        BOOST_REQUIRE(dec.has_value());
        BOOST_TEST((*dec == bytes(plain)));
    }
}

BOOST_AUTO_TEST_CASE(base64_refuses_malformed_input) {
    BOOST_TEST(!aj::base64_decode("Zg=").has_value());       // length
    BOOST_TEST(!aj::base64_decode("Z===").has_value());      // too much padding
    BOOST_TEST(!aj::base64_decode("Zg==Zg==").has_value());  // padding mid-stream
    BOOST_TEST(!aj::base64_decode("Zh==").has_value());      // bits under padding
    BOOST_TEST(!aj::base64_decode("Zm9v!A==").has_value());  // alphabet
    BOOST_TEST(!aj::base64_decode("Zm-v").has_value());      // URL alphabet
}

BOOST_AUTO_TEST_CASE(utf16_length_counts_like_java) {
    BOOST_TEST(aj::detail::utf16_length("abc") == 3U);
    BOOST_TEST(aj::detail::utf16_length("\xC3\xA9") == 1U);          // é
    BOOST_TEST(aj::detail::utf16_length("\xE2\x82\xAC") == 1U);      // €
    BOOST_TEST(aj::detail::utf16_length("\xF0\x9F\x98\x80") == 2U);  // emoji: surrogate pair
    BOOST_TEST(aj::detail::utf16_length("\xFF\xFE") == 2U);          // invalid bytes count one each
}

BOOST_AUTO_TEST_CASE(crc32_header_matches_the_ieee_check_value) {
    // The CRC-32/ISO-HDLC check value of "123456789".
    BOOST_TEST(aj::crc32_header("123456789") == "3421780262");
    // zlib.crc32(b"{}"), the body of an empty DynamoDB response.
    BOOST_TEST(aj::crc32_header("{}") == "2745614147");
}

BOOST_AUTO_TEST_CASE(request_ids_have_the_service_shapes) {
    std::mt19937_64 rng{42};
    const auto ddb = aj::make_request_id(aj::request_id_style::dynamodb, rng);
    BOOST_TEST(ddb.size() == 52U);
    BOOST_TEST(ddb.find_first_not_of("0123456789ABCDEFGHIJKLMNOPQRSTUV") == std::string::npos);
    const auto uuid = aj::make_request_id(aj::request_id_style::uuid, rng);
    BOOST_REQUIRE(uuid.size() == 36U);
    BOOST_TEST(uuid[8] == '-');
    BOOST_TEST(uuid[13] == '-');
    BOOST_TEST(uuid[14] == '4');
    BOOST_TEST(std::string_view("89ab").find(uuid[19]) != std::string_view::npos);
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        seen.insert(aj::make_request_id(aj::request_id_style::dynamodb));
    }
    BOOST_TEST(seen.size() == 100U);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(framing)

BOOST_AUTO_TEST_CASE(resolves_the_target_and_parses_the_body) {
    const auto c = make_codec();
    auto p = c.parse(put(R"({"ThingName":"abc","Kind":"SMALL"})"));
    BOOST_REQUIRE(p.has_value());
    BOOST_TEST(p->op->name == "PutThing");
    BOOST_TEST(p->input.at("ThingName").as_string() == "abc");
}

BOOST_AUTO_TEST_CASE(content_type_parameters_and_case_are_ignored) {
    const auto c = make_codec();
    auto r = put(R"({"ThingName":"abc","Kind":"SMALL"})");
    r.content_type = "Application/X-Amz-Json-1.0; charset=utf-8";
    BOOST_TEST(c.parse(r).has_value());
}

BOOST_AUTO_TEST_CASE(unknown_targets_are_unknown_operations) {
    const auto c = make_codec();
    const std::vector<std::string_view> targets = {
        "",
        "PutThing",
        "Fixture_20261109.",
        "Fixture_20261109.Nope",
        "Other_1.PutThing",
        "Fixture_20261109.PutThing.Extra",
    };
    for (const auto t : targets) {
        const auto e = parse_error(c, request(t, "{}"));
        BOOST_TEST(e.type == "com.amazon.coral.service#UnknownOperationException");
        BOOST_TEST(e.status == 400U);
        BOOST_TEST(!e.message.has_value());
    }
    auto get = put("{}");
    get.method = "GET";
    BOOST_TEST(parse_error(c, get).type == "com.amazon.coral.service#UnknownOperationException");
    auto json11 = put("{}");
    json11.content_type = "application/x-amz-json-1.1";
    BOOST_TEST(parse_error(c, json11).type == "com.amazon.coral.service#UnknownOperationException");
}

BOOST_AUTO_TEST_CASE(unreadable_bodies_are_serialization_errors) {
    const auto c = make_codec();
    for (const auto body : {std::string_view("{"), std::string_view("[]"),
                            std::string_view("\"x\""), std::string_view("{\"a\":}")}) {
        const auto e = parse_error(c, put(body));
        BOOST_TEST(e.type == "com.amazon.coral.service#SerializationException");
        BOOST_TEST(e.message_key == "Message");
        BOOST_TEST(e.message.has_value());
    }
}

BOOST_AUTO_TEST_CASE(an_empty_body_is_an_empty_object) {
    const auto c = make_codec();
    BOOST_TEST(c.parse(request("Fixture_20261109.Ping", "")).has_value());
    BOOST_TEST(c.parse(request("Fixture_20261109.ListThings", "")).has_value());
}

BOOST_AUTO_TEST_CASE(body_and_depth_limits) {
    aj::codec_options o;
    o.style = aj::error_style::bare();
    o.limits.max_body_bytes = 64;
    o.limits.max_depth = 4;
    const aj::codec c(fx::k_model, o);
    const std::string big(65, ' ');
    const auto e = parse_error(c, put(big));
    BOOST_TEST(e.status == 413U);
    BOOST_TEST(e.type == "RequestEntityTooLargeException");
    const auto deep = parse_error(c, put(R"({"a":{"b":{"c":{"d":{}}}}})"));
    BOOST_TEST(deep.type == "SerializationException");
    BOOST_TEST(*deep.message == "Request nesting exceeds the maximum depth");
}

BOOST_AUTO_TEST_CASE(the_default_depth_admits_32_levels_of_attribute_values) {
    // {"M":{"a": ... }} nested 32 times inside an item map, as DynamoDB
    // allows. Parsing must not stop on depth (the fixture has no such
    // member, so validation then refuses it as unknown, which is fine).
    std::string v = R"({"S":"x"})";
    for (int i = 0; i < 32; ++i) {
        v = R"({"M":{"a":)" + v + "}}";
    }
    const auto c = make_codec();
    const auto e = parse_error(c, put(R"({"Item":{"k":)" + v + "}}"));
    BOOST_TEST(e.type == "com.amazon.coral.validate#ValidationException");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(validation)

BOOST_AUTO_TEST_CASE(wrong_json_types_stop_at_the_first_fault) {
    const auto c = make_codec();
    const std::vector<std::pair<std::string_view, std::string_view>> cases = {
        {R"({"ThingName":5,"Kind":"SMALL"})", "Expected string at 'thingName'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Count":"5"})", "Expected integer at 'count'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Count":1.5})", "Expected integer at 'count'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Count":4294967296})", "Expected integer at 'count'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Size":1e30})", "Expected long at 'size'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Enabled":"true"})", "Expected boolean at 'enabled'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Ratio":"x"})", "Expected double at 'ratio'"},
        {R"({"ThingName":"abc","Kind":"SMALL","CreatedAt":"2026-01-01"})",
         "Expected timestamp at 'createdAt'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Tags":{}})", "Expected list at 'tags'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Tags":[{"Key":1}]})",
         "Expected string at 'tags.1.member.key'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Attributes":[]})",
         "Expected structure or map at 'attributes'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Attributes":{"k":3}})",
         "Expected string at 'attributes.k.member'"},
        {R"({"ThingName":"abc","Kind":"SMALL","Payload":"!!!!"})",
         "Base64 encoded value expected at 'payload'"},
    };
    for (const auto& [body, message] : cases) {
        const auto e = parse_error(c, put(body));
        BOOST_TEST(e.type == "com.amazon.coral.service#SerializationException");
        BOOST_TEST(*e.message == message);
    }
}

BOOST_AUTO_TEST_CASE(integral_doubles_count_as_integers) {
    const auto c = make_codec();
    BOOST_TEST(
        c.parse(put(R"({"ThingName":"abc","Kind":"SMALL","Count":5.0,"Size":-7})")).has_value());
}

BOOST_AUTO_TEST_CASE(missing_required_members) {
    const auto c = make_codec();
    const auto e = parse_error(c, put("{}"));
    BOOST_TEST(e.type == "com.amazon.coral.validate#ValidationException");
    BOOST_TEST(e.message_key == "message");
    BOOST_TEST(*e.message ==
               "2 validation errors detected: "
               "Value null at 'thingName' failed to satisfy constraint: Member must not be null; "
               "Value null at 'kind' failed to satisfy constraint: Member must not be null");
    // JSON null is the same as absent.
    BOOST_TEST(parse_error(c, put(R"({"ThingName":null,"Kind":"SMALL"})"))
                   .message->starts_with("1 validation error detected: Value null at 'thingName'"));
}

BOOST_AUTO_TEST_CASE(length_range_enum_and_pattern_constraints) {
    const auto c = make_codec();
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"ab","Kind":"SMALL"})")).message ==
               "1 validation error detected: Value 'ab' at 'thingName' failed to satisfy "
               "constraint: Member must have length greater than or equal to 3");
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"abcdefghi","Kind":"SMALL"})")).message ==
               "1 validation error detected: Value 'abcdefghi' at 'thingName' failed to satisfy "
               "constraint: Member must have length less than or equal to 8");
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"a b","Kind":"SMALL"})")).message ==
               "1 validation error detected: Value 'a b' at 'thingName' failed to satisfy "
               "constraint: Member must satisfy regular expression pattern: [a-zA-Z0-9_.-]+");
    BOOST_TEST(c.parse(put(R"({"ThingName":"a_b","Kind":"SMALL"})")).has_value());
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"abc","Kind":"HUGE"})")).message ==
               "1 validation error detected: Value 'HUGE' at 'kind' failed to satisfy "
               "constraint: Member must satisfy enum value set: [SMALL, LARGE]");
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"abc","Kind":"SMALL","Count":0})")).message ==
               "1 validation error detected: Value '0' at 'count' failed to satisfy "
               "constraint: Member must have value greater than or equal to 1");
    BOOST_TEST(*parse_error(c, put(R"({"ThingName":"abc","Kind":"SMALL","Count":101})")).message ==
               "1 validation error detected: Value '101' at 'count' failed to satisfy "
               "constraint: Member must have value less than or equal to 100");
    // Blob length is of the decoded bytes: 5 bytes > 4.
    BOOST_TEST(parse_error(c, put(R"({"ThingName":"abc","Kind":"SMALL","Payload":"aGVsbG8="})"))
                   .message->ends_with("Member must have length less than or equal to 4"));
    BOOST_TEST(
        c.parse(put(R"({"ThingName":"abc","Kind":"SMALL","Payload":"aGVsbA=="})")).has_value());
    // The unchecked Java pattern does not reject anything.
    BOOST_TEST(c.parse(put(R"({"ThingName":"abc","Kind":"SMALL","Label":"\u0001"})")).has_value());
}

BOOST_AUTO_TEST_CASE(lengths_count_utf16_code_units) {
    const auto c = make_codec();
    // Note has max 2: one emoji is two UTF-16 units (fits), "é€x" is three.
    BOOST_TEST(
        c.parse(put("{\"ThingName\":\"abc\",\"Kind\":\"SMALL\",\"Note\":\"\xF0\x9F\x98\x80\"}"))
            .has_value());
    BOOST_TEST(
        !c.parse(put("{\"ThingName\":\"abc\",\"Kind\":\"SMALL\",\"Note\":\"\xC3\xA9\xE2\x82\xAC"
                     "x\"}"))
             .has_value());
}

BOOST_AUTO_TEST_CASE(nested_paths_lists_and_maps) {
    const auto c = make_codec();
    const auto e = parse_error(
        c,
        put(R"({"ThingName":"abc","Kind":"SMALL","Tags":[{"Key":"k"},{"Value":"v"},{"Key":"toolong"}],)"
            R"("Attributes":{"a":"1","bb":"2","toolongkey":"3"}})"));
    BOOST_TEST(
        *e.message ==
        "5 validation errors detected: "
        "Value '[{Key=k}, {Value=v}, {Key=toolong}]' at 'tags' failed to satisfy constraint: "
        "Member must have length less than or equal to 2; "
        "Value null at 'tags.2.member.key' failed to satisfy constraint: Member must not be "
        "null; "
        "Value 'toolong' at 'tags.3.member.key' failed to satisfy constraint: Member must "
        "have length less than or equal to 4; "
        "Value '{a=1, bb=2, toolongkey=3}' at 'attributes' failed to satisfy constraint: "
        "Member must have length less than or equal to 2; "
        "Value 'toolongkey' at 'attributes.toolongkey.key' failed to satisfy constraint: "
        "Member must have length less than or equal to 4");
}

BOOST_AUTO_TEST_CASE(map_keys_are_checked_against_the_key_shape) {
    const auto c = make_codec();
    const auto e = parse_error(
        c, put(R"({"ThingName":"abc","Kind":"SMALL","Attributes":{"toolongkey":"3"}})"));
    BOOST_TEST(*e.message ==
               "1 validation error detected: Value 'toolongkey' at 'attributes.toolongkey.key' "
               "failed to satisfy constraint: Member must have length less than or equal to 4");
}

BOOST_AUTO_TEST_CASE(sensitive_values_are_never_echoed) {
    const auto c = make_codec();
    const auto e = parse_error(c, put(R"({"ThingName":"abc","Kind":"SMALL","Secret":"s"})"));
    BOOST_TEST(*e.message ==
               "1 validation error detected: Value at 'secret' failed to satisfy constraint: "
               "Member must have length greater than or equal to 2");
    BOOST_TEST(e.message->find("'s'") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(unknown_members_are_refused_unless_relaxed) {
    const auto strict = make_codec();
    const auto e = parse_error(
        strict, put(R"({"ThingName":"abc","Kind":"SMALL","Bogus":1,"Tags":[{"Key":"k","X":1}]})"));
    BOOST_TEST(e.type == "com.amazon.coral.validate#ValidationException");
    BOOST_TEST(*e.message ==
               "2 validation errors detected: "
               "Value at 'tags.1.member.x' failed to satisfy constraint: Member is not defined in "
               "the request shape; "
               "Value at 'bogus' failed to satisfy constraint: Member is not defined in the "
               "request shape");
    BOOST_TEST(!strict.parse(request("Fixture_20261109.Ping", R"({"a":1})")).has_value());
    const auto relaxed = make_codec(aj::error_style::coral("com.example.fixture.v20261109"),
                                    aj::unknown_member_policy::ignore);
    BOOST_TEST(relaxed.parse(put(R"({"ThingName":"abc","Kind":"SMALL","Bogus":1})")).has_value());
    BOOST_TEST(relaxed.parse(request("Fixture_20261109.Ping", R"({"a":1})")).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(responses)

BOOST_AUTO_TEST_CASE(success_carries_the_standard_headers) {
    const auto c = make_codec();
    boost::json::object out;
    out["ThingName"] = "abc";
    const auto r = c.respond(out, "REQ1");
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(r.body == R"({"ThingName":"abc"})");
    BOOST_TEST(*r.header("content-type") == "application/x-amz-json-1.0");
    BOOST_TEST(*r.header("X-Amzn-RequestId") == "REQ1");
    BOOST_TEST(*r.header("x-amz-crc32") == aj::crc32_header(r.body));
}

BOOST_AUTO_TEST_CASE(errors_render_type_message_and_extras) {
    const auto c = make_codec();
    auto e = c.options().style.modeled("ThingNotFoundException", "Requested resource not found");
    BOOST_TEST(e.type == "com.example.fixture.v20261109#ThingNotFoundException");
    auto r = c.respond(e, "REQ2");
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(
        boost::json::parse(r.body) ==
        boost::json::parse(R"({"__type":"com.example.fixture.v20261109#ThingNotFoundException",)"
                           R"("message":"Requested resource not found"})"));
    BOOST_TEST(*r.header("x-amz-crc32") == aj::crc32_header(r.body));

    aj::error cancelled;
    cancelled.type = "com.example.fixture.v20261109#TransactionCanceledException";
    cancelled.message = "Transaction cancelled";
    cancelled.message_key = "Message";
    cancelled.extra["CancellationReasons"] =
        boost::json::array{boost::json::object{{"Code", "None"}}};
    r = c.respond(cancelled, "REQ3");
    const auto body = boost::json::parse(r.body).as_object();
    BOOST_TEST(body.at("Message").as_string() == "Transaction cancelled");
    BOOST_TEST(!body.contains("message"));
    BOOST_TEST(body.at("CancellationReasons").as_array().size() == 1U);

    aj::error bare;
    bare.type = "com.amazon.coral.service#UnknownOperationException";
    BOOST_TEST(c.respond(bare, "R").body ==
               R"({"__type":"com.amazon.coral.service#UnknownOperationException"})");
}

BOOST_AUTO_TEST_CASE(bare_style_for_json_1_1_services) {
    const auto s = aj::error_style::bare();
    BOOST_TEST(s.modeled("ResourceNotFoundException", "x").type == "ResourceNotFoundException");
    BOOST_TEST(s.validation_type == "ValidationException");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(dispatch)

BOOST_AUTO_TEST_CASE(routes_to_handlers_and_reports_their_errors) {
    const auto c = make_codec();
    aj::dispatcher d(c);
    d.on("PutThing", [](aj::request_context& ctx, boost::json::object& in) -> aj::handler_result {
        BOOST_TEST(ctx.op->name == "PutThing");
        BOOST_TEST(ctx.request_id.size() == 52U);
        if (in.at("ThingName").as_string() == "missing") {
            return aj::error_style::coral("com.example.fixture.v20261109")
                .modeled("ThingNotFoundException", "gone");
        }
        return boost::json::object{{"ThingName", in.at("ThingName")}};
    });
    BOOST_TEST(d.implements("PutThing"));
    BOOST_TEST(!d.implements("Ping"));

    auto r = d.handle(put(R"({"ThingName":"abc","Kind":"SMALL"})"));
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(r.body == R"({"ThingName":"abc"})");
    BOOST_TEST(r.header("x-amzn-RequestId")->size() == 52U);

    r = d.handle(put(R"({"ThingName":"missing","Kind":"SMALL"})"));
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.body.find("ThingNotFoundException") != std::string::npos);

    r = d.handle(put("{}"));
    BOOST_TEST(r.body.find("ValidationException") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(modeled_operations_without_handlers_are_unknown) {
    const auto c = make_codec();
    const aj::dispatcher d(c);
    const auto r = d.handle(request("Fixture_20261109.Ping", "{}"));
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(r.body == R"({"__type":"com.amazon.coral.service#UnknownOperationException"})");
    // Before validation: a body that would fail it still gets the same
    // answer.
    const auto invalid = d.handle(put(R"({"ThingName":5})"));
    BOOST_TEST(invalid.body ==
               R"({"__type":"com.amazon.coral.service#UnknownOperationException"})");
}

BOOST_AUTO_TEST_CASE(handler_exceptions_become_internal_errors_without_detail) {
    const auto c = make_codec();
    aj::dispatcher d(c);
    d.on("Ping", [](aj::request_context&, boost::json::object&) -> aj::handler_result {
        throw std::runtime_error("secret internal detail");
    });
    const auto r = d.handle(request("Fixture_20261109.Ping", "{}"));
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(r.body.find("com.example.fixture.v20261109#InternalServerError") !=
               std::string::npos);
    BOOST_TEST(r.body.find("secret") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(registration_refuses_unknown_and_duplicate_operations) {
    const auto c = make_codec();
    aj::dispatcher d(c);
    const auto h = [](aj::request_context&, boost::json::object&) -> aj::handler_result {
        return boost::json::object{};
    };
    BOOST_CHECK_THROW(d.on("Nope", h), std::invalid_argument);
    d.on("Ping", h);
    BOOST_CHECK_THROW(d.on("Ping", h), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
