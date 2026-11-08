// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The request-parameter rules of AWS's IAM and STS API references
// (iam_validation.hpp; the catalogue is
// .kiro/specs/iam-compatible-auth/invariants.md). Each case names the
// invariant ids it covers and tests each bound at, below and above it.

#define BOOST_TEST_MODULE iam_validation_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_validation.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace v = kythira::iam::validate;
using v::credential_kind;

namespace {

auto s(std::size_t n, char c = 'a') -> std::string {
    return std::string(n, c);
}

}  // namespace

BOOST_AUTO_TEST_SUITE(text)

BOOST_AUTO_TEST_CASE(utf8_is_decoded_strictly) {
    BOOST_TEST(v::detail::decode_utf8("caf\xC3\xA9")->size() == 4U);
    BOOST_TEST(v::detail::decode_utf8("\xF0\x9F\x98\x80")->size() == 1U);
    BOOST_TEST(!v::detail::decode_utf8("\xC3").has_value());              // truncated
    BOOST_TEST(!v::detail::decode_utf8("\xC0\xAF").has_value());          // overlong '/'
    BOOST_TEST(!v::detail::decode_utf8("\xED\xA0\x80").has_value());      // surrogate
    BOOST_TEST(!v::detail::decode_utf8("\xF4\x90\x80\x80").has_value());  // > U+10FFFF
    BOOST_TEST(!v::detail::decode_utf8("\xFF").has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_entities)

// INV-NAME-4
BOOST_AUTO_TEST_CASE(inline_policy_names) {
    BOOST_TEST(v::inline_policy_name("a"));
    BOOST_TEST(v::inline_policy_name(s(128)));
    BOOST_TEST(v::inline_policy_name("Read-Only!#$%&()[]{}<>|~'\"`^;:,.=+@_"));
    BOOST_TEST(!v::inline_policy_name(""));
    BOOST_TEST(!v::inline_policy_name(s(129)));
    for (const char* bad : {"a\\b", "a/b", "a*b", "a?b", "a b", "a\tb", "caf\xC3\xA9"}) {
        BOOST_TEST(!v::inline_policy_name(bad), bad);
    }
}

// INV-ACCT-2
BOOST_AUTO_TEST_CASE(account_aliases) {
    BOOST_TEST(v::account_alias("abc"));
    BOOST_TEST(v::account_alias(s(63)));
    BOOST_TEST(v::account_alias("my-corp-2"));
    BOOST_TEST(v::account_alias("12345678901"));    // 11 digits
    BOOST_TEST(v::account_alias("1234567890123"));  // 13 digits
    BOOST_TEST(!v::account_alias("ab"));
    BOOST_TEST(!v::account_alias(s(64)));
    BOOST_TEST(!v::account_alias("-abc"));
    BOOST_TEST(!v::account_alias("abc-"));
    BOOST_TEST(!v::account_alias("ab--c"));
    BOOST_TEST(!v::account_alias("Abc"));
    BOOST_TEST(!v::account_alias("a_c"));
    BOOST_TEST(!v::account_alias("a.c"));
    BOOST_TEST(!v::account_alias("123456789012"));
}

// INV-TAG-2, INV-TAG-3, INV-TAG-4
BOOST_AUTO_TEST_CASE(tag_keys_and_values) {
    BOOST_TEST(v::tag_key("a"));
    BOOST_TEST(v::tag_key(s(128)));
    BOOST_TEST(v::tag_key("Cost Center_.:/=+-@1"));
    BOOST_TEST(v::tag_key("Abteilung-\xC3\x9C"));  // a non-ASCII letter
    BOOST_TEST(v::tag_key(s(127) + "\xC3\xA9"));   // 128 characters, 129 bytes
    BOOST_TEST(!v::tag_key(""));
    BOOST_TEST(!v::tag_key(s(129)));
    for (const char* bad : {"a,b", "a*b", "a#b", "a\tb", "a\"b", "\xC3"}) {
        BOOST_TEST(!v::tag_key(bad), bad);
    }
    BOOST_TEST(!v::tag_key("aws:team"));
    BOOST_TEST(!v::tag_key("AWS:team"));
    BOOST_TEST(v::tag_key("team:aws"));

    BOOST_TEST(v::tag_value(""));
    BOOST_TEST(v::tag_value(s(256)));
    BOOST_TEST(!v::tag_value(s(257)));
    BOOST_TEST(!v::tag_value("aws:x"));
    BOOST_TEST(!v::tag_value("a,b"));
}

// INV-TAG-1, INV-TAG-5, INV-TAG-6
BOOST_AUTO_TEST_CASE(tag_lists) {
    v::tag_list fifty;
    for (int i = 0; i < 50; ++i) {
        fifty.emplace_back("k" + std::to_string(i), "v");
    }
    BOOST_TEST(!v::tags_error(fifty, v::tag_keys::case_sensitive).has_value());
    auto fifty_one = fifty;
    fifty_one.emplace_back("k50", "v");
    BOOST_TEST(v::tags_error(fifty_one, v::tag_keys::case_sensitive).has_value());

    const v::tag_list cased{{"Dept", "a"}, {"dept", "b"}};
    BOOST_TEST(!v::tags_error(cased, v::tag_keys::case_sensitive).has_value());
    BOOST_TEST(v::tags_error(cased, v::tag_keys::case_insensitive).has_value());
    BOOST_TEST(v::tags_error({{"a", "1"}, {"a", "2"}}, v::tag_keys::case_sensitive).has_value());
    // One bad tag among good ones fails the list.
    BOOST_TEST(
        v::tags_error({{"a", "1"}, {"aws:b", "2"}}, v::tag_keys::case_sensitive).has_value());
    BOOST_TEST(v::tags_error({{"a", "1"}, {"b", s(257)}}, v::tag_keys::case_sensitive).has_value());
}

// INV-ROLE-1, INV-ROLE-2
BOOST_AUTO_TEST_CASE(role_parameters) {
    BOOST_TEST(!v::max_session_duration(3599));
    BOOST_TEST(v::max_session_duration(3600));
    BOOST_TEST(v::max_session_duration(43200));
    BOOST_TEST(!v::max_session_duration(43201));

    BOOST_TEST(v::role_description(""));
    BOOST_TEST(v::role_description(s(1000)));
    BOOST_TEST(v::role_description("tab\tnewline\nbr\r caf\xC3\xA9 \xC2\xA1"));
    BOOST_TEST(!v::role_description(s(1001)));
    BOOST_TEST(!v::role_description("\xC2\xA0"));      // U+00A0 is outside the set
    BOOST_TEST(!v::role_description("\x7F"));          // DEL
    BOOST_TEST(!v::role_description("\xE2\x82\xAC"));  // U+20AC
    BOOST_TEST(!v::role_description(std::string(1, '\0')));
}

// INV-POL-1
BOOST_AUTO_TEST_CASE(policy_document_characters) {
    BOOST_TEST(v::policy_document_text("{}"));
    BOOST_TEST(v::policy_document_text("{\t\n\r \"r\":\"caf\xC3\xA9 \xC3\xBF\"}"));
    BOOST_TEST(v::policy_document_text(s(131072)));
    BOOST_TEST(!v::policy_document_text(""));
    BOOST_TEST(!v::policy_document_text(s(131073)));
    BOOST_TEST(!v::policy_document_text("{\"r\":\"\xC4\x80\"}"));  // U+0100
    BOOST_TEST(!v::policy_document_text("{\"r\":\"\x01\"}"));
    BOOST_TEST(!v::policy_document_text("\xC3"));
}

// INV-POL-6
BOOST_AUTO_TEST_CASE(policy_version_ids) {
    for (const char* ok : {"v1", "v9", "v10", "v123", "v1.abc", "v2.a-B-9", "v3."}) {
        BOOST_TEST(v::policy_version_id(ok), ok);
    }
    for (const char* bad : {"", "v", "1", "V1", "v0", "v01", "v1a", "v1.a_b", "v1.a.b", "vv1"}) {
        BOOST_TEST(!v::policy_version_id(bad), bad);
    }
}

// INV-ARN-1
BOOST_AUTO_TEST_CASE(arn_lengths) {
    BOOST_TEST(!v::arn_length(s(19)));
    BOOST_TEST(v::arn_length(s(20)));
    BOOST_TEST(v::arn_length(s(2048)));
    BOOST_TEST(!v::arn_length(s(2049)));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(oidc)

// INV-OIDC-1
BOOST_AUTO_TEST_CASE(provider_urls) {
    BOOST_TEST(v::oidc_url("https://issuer.example.com"));
    BOOST_TEST(v::oidc_url("https://issuer.example.com/tenant/v2.0"));
    BOOST_TEST(v::oidc_url("https://" + s(247)));  // 255 characters
    BOOST_TEST(!v::oidc_url("https://" + s(248)));
    BOOST_TEST(!v::oidc_url("http://issuer.example.com"));
    BOOST_TEST(!v::oidc_url("issuer.example.com"));
    BOOST_TEST(!v::oidc_url("https://"));
    BOOST_TEST(!v::oidc_url("https:///path"));
    BOOST_TEST(!v::oidc_url("https://issuer.example.com:8443"));
    BOOST_TEST(!v::oidc_url("https://issuer.example.com/?a=b"));
    BOOST_TEST(!v::oidc_url("https://issuer.example.com/#frag"));
    BOOST_TEST(!v::oidc_url("https://user@issuer.example.com"));
    BOOST_TEST(!v::oidc_url("https://issuer.example.com/a b"));
}

// INV-OIDC-2, INV-OIDC-3
BOOST_AUTO_TEST_CASE(client_ids_and_thumbprints) {
    BOOST_TEST(v::oidc_client_id("sts.amazonaws.com"));
    BOOST_TEST(v::oidc_client_id(s(255)));
    BOOST_TEST(!v::oidc_client_id(""));
    BOOST_TEST(!v::oidc_client_id(s(256)));

    BOOST_TEST(v::oidc_thumbprint(s(40, 'f')));
    BOOST_TEST(v::oidc_thumbprint("0123456789abcdefABCDEF0123456789abcdef01"));
    BOOST_TEST(!v::oidc_thumbprint(s(39, 'f')));
    BOOST_TEST(!v::oidc_thumbprint(s(41, 'f')));
    BOOST_TEST(!v::oidc_thumbprint(s(40, 'g')));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sts_parameters)

// INV-STS-1, INV-STS-2: session and federated user names.
BOOST_AUTO_TEST_CASE(session_names) {
    BOOST_TEST(!v::role_session_name("a"));
    BOOST_TEST(v::role_session_name("ab"));
    BOOST_TEST(v::role_session_name("a_b+c=d,e.f@g-h"));
    BOOST_TEST(v::role_session_name(s(64)));
    BOOST_TEST(!v::role_session_name(s(65)));
    BOOST_TEST(!v::role_session_name("a b"));
    BOOST_TEST(!v::role_session_name("a/b"));
    BOOST_TEST(!v::role_session_name("a:b"));

    BOOST_TEST(!v::federated_user_name("a"));
    BOOST_TEST(v::federated_user_name("ab"));
    BOOST_TEST(v::federated_user_name(s(32)));
    BOOST_TEST(!v::federated_user_name(s(33)));
    BOOST_TEST(!v::federated_user_name("a b"));
}

// INV-STS-3
BOOST_AUTO_TEST_CASE(external_ids) {
    BOOST_TEST(!v::external_id("a"));
    BOOST_TEST(v::external_id("ab"));
    BOOST_TEST(v::external_id("urn:a/b+c=d,e.f@g-h_i"));
    BOOST_TEST(v::external_id(s(1224)));
    BOOST_TEST(!v::external_id(s(1225)));
    BOOST_TEST(!v::external_id("a b"));
    BOOST_TEST(!v::external_id("a*b"));
}

// INV-STS-4, INV-STS-5
BOOST_AUTO_TEST_CASE(mfa_parameters) {
    BOOST_TEST(!v::mfa_serial_number(s(8)));
    BOOST_TEST(v::mfa_serial_number("GAHT12345678"));
    BOOST_TEST(v::mfa_serial_number("arn:aws:iam::111122223333:mfa/user"));
    BOOST_TEST(v::mfa_serial_number(s(256)));
    BOOST_TEST(!v::mfa_serial_number(s(257)));
    BOOST_TEST(!v::mfa_serial_number("arn:aws:iam::1 :mfa/u"));

    BOOST_TEST(v::mfa_token_code("012345"));
    BOOST_TEST(!v::mfa_token_code("12345"));
    BOOST_TEST(!v::mfa_token_code("1234567"));
    BOOST_TEST(!v::mfa_token_code("12345a"));
}

// INV-STS-6, INV-STS-7, INV-WEB-1
BOOST_AUTO_TEST_CASE(source_identity_and_sizes) {
    BOOST_TEST(!v::source_identity("a"));
    BOOST_TEST(v::source_identity("ab"));
    BOOST_TEST(v::source_identity("alice@example.com"));
    BOOST_TEST(v::source_identity(s(64)));
    BOOST_TEST(!v::source_identity(s(65)));
    BOOST_TEST(!v::source_identity("a b"));
    // ':' is outside the set, so no value can start with "aws:" either.
    BOOST_TEST(!v::source_identity("aws:alice"));

    BOOST_TEST(!v::minimum_session_token_size(-1));
    BOOST_TEST(v::minimum_session_token_size(0));
    BOOST_TEST(v::minimum_session_token_size(4096));
    BOOST_TEST(!v::minimum_session_token_size(4097));

    BOOST_TEST(!v::web_identity_token_length(s(3)));
    BOOST_TEST(v::web_identity_token_length(s(4)));
    BOOST_TEST(v::web_identity_token_length(s(20000)));
    BOOST_TEST(!v::web_identity_token_length(s(20001)));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(durations)

// INV-DUR-1..3
BOOST_AUTO_TEST_CASE(assume_role) {
    BOOST_TEST(v::assume_role_duration(std::nullopt, 3600, false).value() == 3600);
    BOOST_TEST(v::assume_role_duration(std::nullopt, 43200, true).value() == 3600);
    BOOST_TEST(!v::assume_role_duration(899, 43200, false).has_value());
    BOOST_TEST(v::assume_role_duration(900, 3600, false).value() == 900);
    BOOST_TEST(v::assume_role_duration(43200, 43200, false).value() == 43200);
    BOOST_TEST(!v::assume_role_duration(43201, 43200, false).has_value());
    // Never past the role's own maximum: refused, not shortened.
    BOOST_TEST(v::assume_role_duration(7200, 7200, false).value() == 7200);
    BOOST_TEST(!v::assume_role_duration(7201, 7200, false).has_value());
    // Role chaining caps at an hour.
    BOOST_TEST(v::assume_role_duration(3600, 43200, true).value() == 3600);
    BOOST_TEST(!v::assume_role_duration(3601, 43200, true).has_value());
}

// INV-DUR-4
BOOST_AUTO_TEST_CASE(web_identity) {
    BOOST_TEST(v::web_identity_duration(std::nullopt, 3600).value() == 3600);
    BOOST_TEST(v::web_identity_duration(43200, 43200).value() == 43200);
    BOOST_TEST(!v::web_identity_duration(3601, 3600).has_value());
    BOOST_TEST(!v::web_identity_duration(899, 3600).has_value());
}

// INV-DUR-5, INV-DUR-6
BOOST_AUTO_TEST_CASE(session_and_federation_tokens) {
    BOOST_TEST(v::long_term_duration(std::nullopt, false).value() == 43200);
    BOOST_TEST(!v::long_term_duration(899, false).has_value());
    BOOST_TEST(v::long_term_duration(900, false).value() == 900);
    BOOST_TEST(v::long_term_duration(129600, false).value() == 129600);
    BOOST_TEST(!v::long_term_duration(129601, false).has_value());
    // The root: default an hour, a longer ask shortened to an hour.
    BOOST_TEST(v::long_term_duration(std::nullopt, true).value() == 3600);
    BOOST_TEST(v::long_term_duration(900, true).value() == 900);
    BOOST_TEST(v::long_term_duration(129600, true).value() == 3600);
    BOOST_TEST(!v::long_term_duration(129601, true).has_value());
    BOOST_TEST(!v::long_term_duration(899, true).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(sessions)

constexpr std::string_view account = "111122223333";

// INV-SPOL-1..4
BOOST_AUTO_TEST_CASE(session_policies) {
    const std::string own = "arn:aws:iam::111122223333:policy/Mine";
    const std::string aws = "arn:aws:iam::aws:policy/ReadOnlyAccess";
    BOOST_TEST(!v::session_policies_error(std::nullopt, {}, account).has_value());
    BOOST_TEST(!v::session_policies_error(R"({"Statement":[]})", {own, aws}, account).has_value());

    auto code = [](const auto& e) { return e ? e->first : std::string("<none>"); };
    BOOST_TEST(code(v::session_policies_error("", {}, account)) == "MalformedPolicyDocument");
    BOOST_TEST(code(v::session_policies_error(s(2049), {}, account)) == "MalformedPolicyDocument");
    BOOST_TEST(code(v::session_policies_error("{\"\xC4\x80\"}", {}, account)) ==
               "MalformedPolicyDocument");

    std::vector<std::string> ten(10, own);
    BOOST_TEST(!v::session_policies_error(std::nullopt, ten, account).has_value());
    ten.push_back(own);
    BOOST_TEST(code(v::session_policies_error(std::nullopt, ten, account)) == "InvalidInput");
    BOOST_TEST(code(v::session_policies_error(
                   std::nullopt, {"arn:aws:iam::444455556666:policy/Theirs"}, account)) ==
               "InvalidInput");
    BOOST_TEST(code(v::session_policies_error(std::nullopt, {"Mine"}, account)) == "InvalidInput");

    // 2048 together, white space not counted; one more is too large.
    const auto fill = 2048 - own.size();
    BOOST_TEST(!v::session_policies_error(s(fill) + "  \n", {own}, account).has_value());
    BOOST_TEST(code(v::session_policies_error(s(fill + 1), {own}, account)) ==
               "PackedPolicyTooLarge");
}

// INV-STAG-1..4
BOOST_AUTO_TEST_CASE(session_tags) {
    BOOST_TEST(!v::session_tags_error({{"Project", "x"}}, {"Project"}, {}).has_value());
    // Session tag keys are case-insensitive.
    BOOST_TEST(v::session_tags_error({{"Project", "x"}, {"project", "y"}}, {}, {}).has_value());
    v::tag_list fifty_one;
    for (int i = 0; i <= 50; ++i) {
        fifty_one.emplace_back("k" + std::to_string(i), "");
    }
    BOOST_TEST(v::session_tags_error(fifty_one, {}, {}).has_value());
    BOOST_TEST(v::session_tags_error({}, std::vector<std::string>(51, "k"), {}).has_value());
    BOOST_TEST(!v::session_tags_error({}, std::vector<std::string>(50, "k"), {}).has_value());
    BOOST_TEST(v::session_tags_error({}, {""}, {}).has_value());
    BOOST_TEST(v::session_tags_error({}, {s(129)}, {}).has_value());
    // A transitive tag of the calling session cannot be set again.
    BOOST_TEST(v::session_tags_error({{"project", "y"}}, {}, {{"Project", "x"}}).has_value());
    BOOST_TEST(!v::session_tags_error({{"team", "y"}}, {}, {{"Project", "x"}}).has_value());
}

// INV-STAG-5
BOOST_AUTO_TEST_CASE(session_tags_override_principal_tags) {
    const auto merged = v::merge_session_tags({{"Dept", "finance"}, {"Site", "a"}},
                                              {{"dept", "hr"}}, {{"Project", "p"}});
    BOOST_TEST(merged.size() == 3U);
    const auto has = [&](const std::string& k, const std::string& val) {
        return std::ranges::find(merged, std::pair{k, val}) != merged.end();
    };
    BOOST_TEST(has("dept", "hr"));
    BOOST_TEST(has("Site", "a"));
    BOOST_TEST(has("Project", "p"));
    BOOST_TEST(!has("Dept", "finance"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(caller_restrictions)

// INV-CRED-1..4
BOOST_AUTO_TEST_CASE(sts_actions_by_credential) {
    const std::vector<std::string> all{
        "AssumeRole",         "AssumeRoleWithWebIdentity",  "GetCallerIdentity", "GetSessionToken",
        "GetFederationToken", "DecodeAuthorizationMessage", "GetAccessKeyInfo"};
    for (const auto& a : all) {
        BOOST_TEST(v::sts_action_allowed(credential_kind::root, a), a);
        BOOST_TEST(v::sts_action_allowed(credential_kind::user, a), a);
        BOOST_TEST(v::sts_action_allowed(credential_kind::assumed_role, a) ==
                       (a != "GetSessionToken" && a != "GetFederationToken"),
                   a);
        BOOST_TEST(
            v::sts_action_allowed(credential_kind::session_token, a) ==
                (a == "AssumeRole" || a == "GetCallerIdentity" || a == "AssumeRoleWithWebIdentity"),
            a);
        BOOST_TEST(v::sts_action_allowed(credential_kind::federated_user, a) ==
                       (a == "GetCallerIdentity" || a == "AssumeRoleWithWebIdentity"),
                   a);
    }
}

// INV-CRED-5, INV-CRED-6
BOOST_AUTO_TEST_CASE(iam_api_by_credential) {
    BOOST_TEST(v::iam_api_allowed(credential_kind::root, false));
    BOOST_TEST(v::iam_api_allowed(credential_kind::user, false));
    BOOST_TEST(v::iam_api_allowed(credential_kind::assumed_role, false));
    BOOST_TEST(!v::iam_api_allowed(credential_kind::session_token, false));
    BOOST_TEST(v::iam_api_allowed(credential_kind::session_token, true));
    BOOST_TEST(!v::iam_api_allowed(credential_kind::federated_user, true));
}

BOOST_AUTO_TEST_SUITE_END()
