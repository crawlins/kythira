// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The AWS Query protocol codec (`.kiro/specs/iam-compatible-auth/` task 14,
// Requirement 9.1): form decoding with member lists, the XML writer and
// documents, sealed pagination markers and MaxItems.

#define BOOST_TEST_MODULE aws_query_protocol_test

#include <boost/test/unit_test.hpp>

#include <raft/aws_query_protocol.hpp>

#include <array>
#include <string>
#include <string_view>

namespace q = kythira::aws_query;

BOOST_AUTO_TEST_SUITE(form_decoding)

BOOST_AUTO_TEST_CASE(decodes_plus_and_percent_escapes) {
    const auto f = q::form::parse("Action=CreateUser&UserName=a%2Bb+c&Path=%2Fdev%2F&Empty=");
    BOOST_REQUIRE(f.has_value());
    BOOST_TEST(*f->get("Action") == "CreateUser");
    BOOST_TEST(*f->get("UserName") == "a+b c");
    BOOST_TEST(*f->get("Path") == "/dev/");
    BOOST_TEST(f->get("Empty").has_value());
    BOOST_TEST(f->get("Empty")->empty());
    BOOST_TEST(!f->get("Missing").has_value());
    BOOST_TEST(q::form::parse("").has_value());
    BOOST_TEST(q::form::parse("a=1&&b=2").has_value());
}

BOOST_AUTO_TEST_CASE(refuses_bad_escapes_and_repeats) {
    BOOST_TEST(!q::form::parse("A=%2").has_value());
    BOOST_TEST(!q::form::parse("A=%zz").has_value());
    BOOST_TEST(!q::form::parse("A=1&A=2").has_value());
    BOOST_TEST(!q::form::parse("=1").has_value());
}

BOOST_AUTO_TEST_CASE(reads_member_lists_in_index_order) {
    const auto f = q::form::parse(
        "PolicyArns.member.2=arn2&PolicyArns.member.10=arn10&PolicyArns.member.1=arn1");
    BOOST_REQUIRE(f.has_value());
    const auto m = f->members("PolicyArns");
    BOOST_REQUIRE(m.has_value());
    BOOST_TEST(m->size() == 3U);
    BOOST_TEST((*m)[0] == "arn1");
    BOOST_TEST((*m)[1] == "arn2");
    BOOST_TEST((*m)[2] == "arn10");
    BOOST_TEST(f->members("TagKeys")->empty());
    BOOST_TEST(!q::form::parse("K.member.0=x")->members("K").has_value());
    BOOST_TEST(!q::form::parse("K.member.x=x")->members("K").has_value());
}

BOOST_AUTO_TEST_CASE(reads_member_structures) {
    const auto f = q::form::parse(
        "Tags.member.1.Key=team&Tags.member.1.Value=iam&Tags.member.2.Key=env&Tags.member.2."
        "Value=");
    BOOST_REQUIRE(f.has_value());
    constexpr std::array<std::string_view, 2> fields{"Key", "Value"};
    const auto tags = f->member_structs("Tags", fields);
    BOOST_REQUIRE(tags.has_value());
    BOOST_TEST(tags->size() == 2U);
    BOOST_TEST((*tags)[0].at("Key") == "team");
    BOOST_TEST((*tags)[0].at("Value") == "iam");
    BOOST_TEST((*tags)[1].at("Value").empty());
    BOOST_TEST(!q::form::parse("Tags.member.1.Colour=x")->member_structs("Tags", fields));
    BOOST_TEST(!q::form::parse("Tags.member.1=x")->member_structs("Tags", fields));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(xml)

BOOST_AUTO_TEST_CASE(escapes_and_drops_unencodable_characters) {
    BOOST_TEST(q::xml_escape("<a & 'b' \"c\">") == "&lt;a &amp; &apos;b&apos; &quot;c&quot;&gt;");
    BOOST_TEST(q::xml_escape(std::string("x\x01y\tz", 5)) == "xy\tz");
}

BOOST_AUTO_TEST_CASE(url_encodes_policy_documents_as_iam_returns_them) {
    BOOST_TEST(q::url_encode(R"({"Effect":"Allow"} a~b-c_d.e/f+)") ==
               "%7B%22Effect%22%3A%22Allow%22%7D%20a~b-c_d.e%2Ff%2B");
    BOOST_TEST(q::url_encode(std::string("\xC3\xA9", 2)) == "%C3%A9");
    // What the form decoder reads back is the original text.
    const auto doc = std::string(R"({"Version":"2012-10-17","Statement":[]})");
    BOOST_TEST(*q::form::parse("D=" + q::url_encode(doc))->get("D") == doc);
}

BOOST_AUTO_TEST_CASE(writes_nested_elements) {
    q::xml_writer w;
    w.open("User").element("UserName", "a<b").boolean("Enabled", true).close();
    BOOST_TEST(w.depth() == 0U);
    BOOST_TEST(w.str() == "<User><UserName>a&lt;b</UserName><Enabled>true</Enabled></User>");
}

BOOST_AUTO_TEST_CASE(wraps_results_and_errors_in_aws_documents) {
    constexpr std::string_view ns = "https://iam.amazonaws.com/doc/2010-05-08/";
    BOOST_TEST(q::response_xml("GetUser", ns, "<User/>", "req-1") ==
               "<GetUserResponse xmlns=\"https://iam.amazonaws.com/doc/2010-05-08/\">"
               "<GetUserResult><User/></GetUserResult>"
               "<ResponseMetadata><RequestId>req-1</RequestId></ResponseMetadata>"
               "</GetUserResponse>");
    BOOST_TEST(q::response_xml("DeleteUser", ns, "", "r") ==
               "<DeleteUserResponse xmlns=\"https://iam.amazonaws.com/doc/2010-05-08/\">"
               "<ResponseMetadata><RequestId>r</RequestId></ResponseMetadata>"
               "</DeleteUserResponse>");
    BOOST_TEST(
        q::error_xml(ns, true, "NoSuchEntity", "The user with name x cannot be found.", "r") ==
        "<ErrorResponse xmlns=\"https://iam.amazonaws.com/doc/2010-05-08/\"><Error>"
        "<Type>Sender</Type><Code>NoSuchEntity</Code>"
        "<Message>The user with name x cannot be found.</Message></Error>"
        "<RequestId>r</RequestId></ErrorResponse>");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(pagination)

const std::array<unsigned char, 32> k_key{1, 2, 3, 4, 5, 6, 7, 8};

BOOST_AUTO_TEST_CASE(a_marker_opens_only_for_the_list_that_issued_it) {
    const q::marker_sealer s(k_key);
    const auto m = s.seal("ListUsers:111122223333:/", "alice");
    BOOST_TEST(
        m.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") ==
        std::string::npos);
    BOOST_TEST(*s.open("ListUsers:111122223333:/", m) == "alice");
    BOOST_TEST(!s.open("ListGroups:111122223333:/", m).has_value());
    BOOST_TEST(!s.open("ListUsers:444455556666:/", m).has_value());
    // Another cluster's key opens nothing.
    const std::array<unsigned char, 32> other{9};
    BOOST_TEST(!q::marker_sealer(other).open("ListUsers:111122223333:/", m).has_value());
    // An empty last key round-trips too.
    BOOST_TEST(s.open("c", s.seal("c", "")).value().empty());
}

BOOST_AUTO_TEST_CASE(a_forged_or_damaged_marker_is_refused) {
    const q::marker_sealer s(k_key);
    auto m = s.seal("ctx", "bob");
    m[0] = m[0] == 'A' ? 'B' : 'A';
    BOOST_TEST(!s.open("ctx", m).has_value());
    BOOST_TEST(!s.open("ctx", "not base64!").has_value());
    BOOST_TEST(!s.open("ctx", "").has_value());
    BOOST_TEST(!s.open("ctx", "Ym9i").has_value());  // "bob" with no tag
}

BOOST_AUTO_TEST_CASE(max_items_defaults_and_bounds) {
    BOOST_TEST(*q::max_items(*q::form::parse("")) == 100U);
    BOOST_TEST(*q::max_items(*q::form::parse("MaxItems=1")) == 1U);
    BOOST_TEST(*q::max_items(*q::form::parse("MaxItems=1000")) == 1000U);
    BOOST_TEST(!q::max_items(*q::form::parse("MaxItems=0")).has_value());
    BOOST_TEST(!q::max_items(*q::form::parse("MaxItems=1001")).has_value());
    BOOST_TEST(!q::max_items(*q::form::parse("MaxItems=ten")).has_value());
    BOOST_TEST(!q::max_items(*q::form::parse("MaxItems=-1")).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
