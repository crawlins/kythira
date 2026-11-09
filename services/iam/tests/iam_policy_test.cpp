// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The IAM policy parser and matchers (`.kiro/specs/iam-compatible-auth/`
// task 10, Requirements 5.1-5.3): the corpus under corpus/ (format in
// corpus/README.md), then the cases a corpus file cannot express well: the
// AST's shape, the error codes, the size counting and the glob engine's
// edge cases.

#define BOOST_TEST_MODULE iam_policy_test

#include <boost/test/unit_test.hpp>

#include <kythira/iam/iam_conditions.hpp>
#include <kythira/iam/iam_evaluator.hpp>
#include <kythira/iam/iam_policy.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace iam = kythira::iam;

namespace {

auto read_file(const std::filesystem::path& p) -> std::string {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

auto type_from(std::string_view s) -> std::optional<iam::policy_type> {
    for (auto t : {iam::policy_type::identity, iam::policy_type::trust, iam::policy_type::resource,
                   iam::policy_type::boundary, iam::policy_type::session}) {
        if (iam::to_string(t) == s) {
            return t;
        }
    }
    return std::nullopt;
}

auto corpus_files() -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::recursive_directory_iterator(KYTHIRA_IAM_CORPUS_DIR)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            out.push_back(e.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

// A case's `context`: each key maps to a string (single-valued) or an
// array (multivalued, possibly empty).
auto context_of(const boost::json::object& c) -> iam::context_keys {
    iam::context_keys ctx;
    if (const auto* o = c.if_contains("context")) {
        for (const auto& [k, v] : o->as_object()) {
            if (const auto* a = v.if_array()) {
                std::vector<std::string> values;
                for (const auto& x : *a) {
                    values.emplace_back(x.as_string());
                }
                ctx.set_multi(std::string_view(k.data(), k.size()), std::move(values));
            } else {
                ctx.set(std::string_view(k.data(), k.size()), std::string(v.as_string()));
            }
        }
    }
    return ctx;
}

// An `evaluate` case's `principal`, from its shorthand (corpus/README.md):
// `root`, `user/NAME`, `role/ROLE/SESSION`, `federated/NAME/USER`,
// `service/NAME`, `web/PROVIDER` or `anonymous`, in `principal_account`
// (default 111122223333).
auto principal_of(const boost::json::object& c) -> iam::principal {
    const std::string spec(c.at("principal").as_string());
    std::string account = "111122223333";
    if (const auto* a = c.if_contains("principal_account")) {
        account = std::string(a->as_string());
    }
    std::vector<std::string> parts;
    std::stringstream in(spec);
    for (std::string part; std::getline(in, part, '/');) {
        parts.push_back(part);
    }
    iam::principal p;
    const auto iam_arn = [&](const std::string& rest) {
        return "arn:aws:iam::" + account + ":" + rest;
    };
    const auto sts_arn = [&](const std::string& rest) {
        return "arn:aws:sts::" + account + ":" + rest;
    };
    const auto& head = parts.at(0);
    if (head == "root") {
        p.kind = iam::principal_kind::root;
        p.account = account;
        p.arn = iam_arn("root");
    } else if (head == "user") {
        p.kind = iam::principal_kind::user;
        p.account = account;
        p.arn = iam_arn("user/" + parts.at(1));
    } else if (head == "role") {
        p.kind = iam::principal_kind::role_session;
        p.account = account;
        p.role_arn = iam_arn("role/" + parts.at(1));
        p.arn = sts_arn("assumed-role/" + parts.at(1) + "/" + parts.at(2));
    } else if (head == "federated") {
        p.kind = iam::principal_kind::federated_user;
        p.account = account;
        p.arn = sts_arn("federated-user/" + parts.at(1));
        p.user_arn = iam_arn("user/" + parts.at(2));
    } else if (head == "service") {
        p.kind = iam::principal_kind::service;
        p.name = spec.substr(head.size() + 1);
    } else if (head == "web") {
        p.kind = iam::principal_kind::web_identity;
        p.name = spec.substr(head.size() + 1);
    } else {
        BOOST_REQUIRE_MESSAGE(head == "anonymous", "unknown principal shorthand " << spec);
    }
    return p;
}

void run_evaluate(const boost::json::object& c) {
    // The documents live here for the request's lifetime; a deque keeps
    // their addresses stable as more are added.
    std::deque<iam::policy_document> docs;
    const auto parse = [&](const boost::json::value& v,
                           iam::policy_type t) -> const iam::policy_document* {
        docs.push_back(iam::parse_policy(boost::json::serialize(v), t));
        return &docs.back();
    };
    iam::evaluation_request r;
    r.who = principal_of(c);
    r.action = std::string(c.at("action").as_string());
    r.resource = c.contains("resource") ? std::string(c.at("resource").as_string()) : "*";
    if (const auto* a = c.if_contains("resource_account")) {
        r.resource_account = std::string(a->as_string());
    }
    r.context = context_of(c);
    const auto list = [&](const char* key, iam::policy_type t,
                          std::vector<iam::named_policy>& out) {
        if (const auto* v = c.if_contains(key)) {
            std::size_t i = 0;
            for (const auto& d : v->as_array()) {
                out.push_back({std::string(key) + "[" + std::to_string(i++) + "]", parse(d, t)});
            }
        }
    };
    list("identity", iam::policy_type::identity, r.identity);
    list("session", iam::policy_type::session, r.session);
    if (const auto* b = c.if_contains("boundary")) {
        r.boundary = iam::named_policy{"boundary", parse(*b, iam::policy_type::boundary)};
    }
    std::string kind = "ordinary";
    if (const auto* k = c.if_contains("resource_kind")) {
        kind = std::string(k->as_string());
    }
    if (kind == "trust") {
        r.resource_kind = iam::resource_policy_kind::trust;
    } else if (kind == "key") {
        r.resource_kind = iam::resource_policy_kind::key;
    } else {
        BOOST_REQUIRE_MESSAGE(kind == "ordinary", "unknown resource_kind " << kind);
    }
    if (const auto* rp = c.if_contains("resource_policy")) {
        r.resource_policy = iam::named_policy{
            "resource",
            parse(*rp, kind == "trust" ? iam::policy_type::trust : iam::policy_type::resource)};
    }

    const auto d = iam::evaluate(r);
    const std::string want(c.at("expect").as_string());
    std::string got;
    switch (d.result) {
        case iam::decision::outcome::allow:
            got = "allow";
            break;
        case iam::decision::outcome::explicit_deny:
            got = "explicit_deny";
            break;
        case iam::decision::outcome::implicit_deny:
            got = "implicit_deny";
            break;
    }
    BOOST_TEST(got == want, "reason: " << d.reason);
    if (const auto* by = c.if_contains("decided_by")) {
        // "policy" or "policy#Sid"; null when no statement decided.
        if (by->is_null()) {
            BOOST_TEST(!d.deciding.has_value());
        } else {
            BOOST_REQUIRE(d.deciding.has_value());
            std::string where = d.deciding->policy;
            if (!d.deciding->sid.empty()) {
                where += "#" + d.deciding->sid;
            }
            BOOST_TEST(where == std::string(by->as_string()));
        }
    }
}

// Runs one corpus case. A case the runner does not understand (an unknown
// kind or policy type) is an error, so a typo in the corpus fails loudly
// instead of passing vacuously.
void run_case(const boost::json::object& c, const std::string& file_source) {
    const std::string kind(c.at("kind").as_string());
    const auto* source = c.if_contains("source");
    BOOST_TEST((source != nullptr || !file_source.empty()), "every case cites a source");

    if (kind == "parse") {
        const auto type = type_from(c.at("policy_type").as_string());
        BOOST_REQUIRE_MESSAGE(type.has_value(), "unknown policy_type");
        std::string text;
        if (const auto* t = c.if_contains("policy_text")) {
            text = std::string(t->as_string());
        } else {
            text = boost::json::serialize(c.at("policy"));
        }
        iam::parse_options opts;
        if (const auto* m = c.if_contains("max_size")) {
            opts.max_size = static_cast<std::size_t>(m->to_number<std::int64_t>());
        }
        const auto& expect = c.at("expect");
        if (expect.is_string()) {
            BOOST_REQUIRE_EQUAL(std::string(expect.as_string()), "valid");
            try {
                const auto doc = iam::parse_policy(text, *type, opts);
                if (const auto* n = c.if_contains("statements")) {
                    BOOST_TEST(doc.statements.size() ==
                               static_cast<std::size_t>(n->to_number<std::int64_t>()));
                }
            } catch (const iam::policy_error& e) {
                BOOST_ERROR("expected valid, got " << e.code() << ": " << e.what());
            }
            return;
        }
        const auto& err = expect.as_object();
        const std::string code(err.at("code").as_string());
        const std::string message(err.at("message").as_string());
        try {
            (void)iam::parse_policy(text, *type, opts);
            BOOST_ERROR("expected " << code << " containing \"" << message << "\", got valid");
        } catch (const iam::policy_error& e) {
            BOOST_TEST(e.code() == code);
            BOOST_TEST(std::string(e.what()).find(message) != std::string::npos,
                       "message \"" << e.what() << "\" lacks \"" << message << "\"");
        }
        return;
    }
    if (kind == "action_match" || kind == "arn_match") {
        const std::string pattern(c.at("pattern").as_string());
        const std::string value(c.at("value").as_string());
        const bool want = c.at("match").as_bool();
        const bool got = kind == "action_match" ? iam::action_matches(pattern, value)
                                                : iam::arn_matches(pattern, value);
        BOOST_TEST(got == want, pattern << " vs " << value);
        return;
    }
    if (kind == "condition") {
        // A one-statement identity policy holding `condition`, so the
        // operator goes through the parser exactly as in a real document.
        boost::json::object policy{
            {"Statement", boost::json::object{{"Effect", "Allow"},
                                              {"Action", "s3:GetObject"},
                                              {"Resource", "*"},
                                              {"Condition", c.at("condition")}}}};
        if (const auto* version = c.if_contains("version")) {
            policy["Version"] = *version;
        } else {
            policy["Version"] = "2012-10-17";
        }
        const auto doc =
            iam::parse_policy(boost::json::serialize(policy), iam::policy_type::identity);
        const auto ctx = context_of(c);
        const bool got = iam::conditions_match(doc.statements.at(0), ctx, doc.supports_variables());
        BOOST_TEST(got == c.at("match").as_bool());
        return;
    }
    if (kind == "variable") {
        const auto ctx = context_of(c);
        const auto got =
            iam::substitute(std::string(c.at("text").as_string()), ctx, iam::substitution::plain);
        const auto& want = c.at("expect");
        if (want.is_null()) {
            BOOST_TEST(!got.has_value(), "expected no value, got " << got.value_or(""));
        } else {
            BOOST_TEST(got.value_or("<none>") == std::string(want.as_string()));
        }
        return;
    }
    if (kind == "resource_match") {
        const std::string pattern(c.at("pattern").as_string());
        const auto* version = c.if_contains("version");
        const bool vars = version == nullptr || version->as_string() == "2012-10-17";
        const iam::resource_matcher m(std::vector<std::string>{pattern});
        const bool got =
            iam::resource_matches(m, std::string(c.at("value").as_string()), context_of(c), vars);
        BOOST_TEST(got == c.at("match").as_bool(), pattern << " vs " << c.at("value"));
        return;
    }
    if (kind == "evaluate") {
        run_evaluate(c);
        return;
    }
    BOOST_ERROR("unknown corpus case kind " << kind);
}

auto parse_ok(std::string_view json, iam::policy_type t = iam::policy_type::identity)
    -> iam::policy_document {
    return iam::parse_policy(json, t);
}

auto error_of(std::string_view json, iam::policy_type t = iam::policy_type::identity)
    -> std::string {
    try {
        (void)iam::parse_policy(json, t);
    } catch (const iam::policy_error& e) {
        return e.what();
    }
    return "<valid>";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(iam_policy_corpus)

BOOST_AUTO_TEST_CASE(every_corpus_case_holds) {
    const auto files = corpus_files();
    BOOST_REQUIRE(!files.empty());
    std::size_t cases = 0;
    for (const auto& f : files) {
        BOOST_TEST_CONTEXT("corpus file " << f.filename().string()) {
            boost::system::error_code ec;
            const auto root = boost::json::parse(read_file(f), ec);
            BOOST_REQUIRE_MESSAGE(!ec, "corpus file is not JSON: " << ec.message());
            const auto& obj = root.as_object();
            std::string file_source;
            if (const auto* s = obj.if_contains("source")) {
                file_source = std::string(s->as_string());
            }
            for (const auto& c : obj.at("cases").as_array()) {
                BOOST_TEST_CONTEXT("case " << c.as_object().at("name").as_string()) {
                    run_case(c.as_object(), file_source);
                }
                ++cases;
            }
        }
    }
    BOOST_TEST_MESSAGE("ran " << cases << " corpus cases from " << files.size() << " files");
    BOOST_TEST(cases >= 300u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_policy_ast)

BOOST_AUTO_TEST_CASE(statement_shape) {
    const auto doc = parse_ok(R"({
        "Version": "2012-10-17",
        "Id": "doc1",
        "Statement": [{
            "Sid": "ReadOnly",
            "Effect": "Allow",
            "Action": ["S3:GetObject", "s3:List*"],
            "Resource": "arn:aws:s3:::bucket/*",
            "Condition": {
                "StringEquals": {"aws:username": ["alice", "bob"]},
                "Bool": {"aws:SecureTransport": true},
                "NumericLessThan": {"s3:max-keys": 10}
            }
        }, {
            "Effect": "Deny",
            "NotAction": "iam:*",
            "NotResource": ["arn:aws:iam::123456789012:user/${aws:username}"]
        }]
    })");
    BOOST_TEST(doc.version == "2012-10-17");
    BOOST_TEST(doc.supports_variables());
    BOOST_TEST(doc.id == "doc1");
    BOOST_REQUIRE_EQUAL(doc.statements.size(), 2u);

    const auto& a = doc.statements[0];
    BOOST_TEST(a.sid == "ReadOnly");
    BOOST_TEST((a.eff == iam::effect::allow));
    BOOST_TEST(!a.not_action);
    BOOST_TEST(!a.principal.has_value());
    BOOST_TEST(a.action.matches("s3:getobject"));
    BOOST_TEST(a.action.matches("S3:ListBucket"));
    BOOST_TEST(!a.action.matches("s3:PutObject"));
    BOOST_REQUIRE(a.resource.has_value());
    BOOST_TEST(a.resource->matches("arn:aws:s3:::bucket/key"));
    BOOST_TEST(!a.resource->matches("arn:aws:s3:::other/key"));
    BOOST_REQUIRE_EQUAL(a.conditions.size(), 3u);
    BOOST_TEST(a.conditions[0].op == "StringEquals");
    BOOST_TEST(a.conditions[0].key == "aws:username");
    BOOST_TEST((a.conditions[0].values == std::vector<std::string>{"alice", "bob"}));
    BOOST_TEST((a.conditions[1].values == std::vector<std::string>{"true"}));
    BOOST_TEST((a.conditions[2].values == std::vector<std::string>{"10"}));

    const auto& b = doc.statements[1];
    BOOST_TEST((b.eff == iam::effect::deny));
    BOOST_TEST(b.not_action);
    BOOST_TEST(b.not_resource);
    BOOST_TEST(b.action.matches("IAM:CreateUser"));
    BOOST_REQUIRE(b.resource.has_value());
    BOOST_REQUIRE_EQUAL(b.resource->patterns().size(), 1u);
    BOOST_TEST(b.resource->patterns()[0].has_variables());
    // Without a request context a variable never resolves (see resource_matches).
    BOOST_TEST(!b.resource->matches("arn:aws:iam::123456789012:user/${aws:username}"));
}

BOOST_AUTO_TEST_CASE(version_defaults_to_2008) {
    const auto doc = parse_ok(R"({"Statement": {"Effect": "Allow", "Action": "*",
                                                "Resource": "*"}})");
    BOOST_TEST(doc.version == "2008-10-17");
    BOOST_TEST(!doc.supports_variables());
    BOOST_TEST(doc.statements.size() == 1u);
}

BOOST_AUTO_TEST_CASE(principal_shapes) {
    const auto trust = parse_ok(R"({
        "Version": "2012-10-17",
        "Statement": [{
            "Effect": "Allow",
            "Principal": {"AWS": ["123456789012", "arn:aws:iam::111122223333:role/r"],
                          "Service": "ec2.amazonaws.com"},
            "Action": "sts:AssumeRole"
        }, {
            "Effect": "Allow",
            "Principal": "*",
            "Action": "sts:AssumeRoleWithWebIdentity"
        }]
    })",
                                iam::policy_type::trust);
    const auto& p = trust.statements[0].principal;
    BOOST_REQUIRE(p.has_value());
    BOOST_TEST(!p->anyone);
    BOOST_REQUIRE(p->values("AWS") != nullptr);
    BOOST_TEST(p->values("AWS")->size() == 2u);
    BOOST_REQUIRE(p->values("Service") != nullptr);
    BOOST_TEST(p->values("Service")->front() == "ec2.amazonaws.com");
    BOOST_TEST(p->values("Federated") == nullptr);
    BOOST_TEST(!trust.statements[0].resource.has_value());
    BOOST_TEST(trust.statements[1].principal->anyone);

    const auto res = parse_ok(R"({
        "Statement": {"Effect": "Deny", "NotPrincipal": {"AWS": "arn:aws:iam::123456789012:root"},
                      "Action": "kinesis:*", "Resource": "*"}
    })",
                              iam::policy_type::resource);
    BOOST_TEST(res.statements[0].not_principal);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_policy_errors)

BOOST_AUTO_TEST_CASE(codes) {
    try {
        (void)iam::parse_policy("{", iam::policy_type::identity);
        BOOST_FAIL("expected an error");
    } catch (const iam::policy_error& e) {
        BOOST_TEST(e.code() == "MalformedPolicyDocument");
    }
    const std::string big =
        R"({"Statement":{"Effect":"Allow","Action":"*","Resource":"arn:aws:s3:::)" +
        std::string(3000, 'x') + R"("}})";
    try {
        (void)iam::parse_policy(big, iam::policy_type::session);
        BOOST_FAIL("expected an error");
    } catch (const iam::policy_error& e) {
        BOOST_TEST(e.code() == "LimitExceeded");
        BOOST_TEST(std::string(e.what()) == "Cannot exceed quota for PolicySize: 2048");
    }
    // The same document fits a managed policy, and a raised quota.
    BOOST_CHECK_NO_THROW((void)iam::parse_policy(big, iam::policy_type::identity));
    BOOST_CHECK_NO_THROW((void)iam::parse_policy(big, iam::policy_type::session,
                                                 iam::parse_options{.max_size = 4096}));
}

BOOST_AUTO_TEST_CASE(duplicate_keys_at_every_depth) {
    BOOST_TEST(error_of(R"({"Statement": {}, "Statement": {}})").find("duplicate key") !=
               std::string::npos);
    BOOST_TEST(error_of(R"({"Statement": [{"Effect": "Allow", "Effect": "Deny",
                                         "Action": "*", "Resource": "*"}]})")
                   .find("duplicate key \"Effect\"") != std::string::npos);
    BOOST_TEST(error_of(R"({"Statement": {"Effect": "Allow", "Action": "*", "Resource": "*",
        "Condition": {"StringEquals": {"aws:username": "a", "aws:username": "b"}}}})")
                   .find("duplicate key \"aws:username\"") != std::string::npos);
    // The same key in sibling objects is not a duplicate.
    BOOST_CHECK_NO_THROW(parse_ok(R"({"Statement": [
        {"Effect": "Allow", "Action": "*", "Resource": "*"},
        {"Effect": "Deny", "Action": "*", "Resource": "*"}]})"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_policy_size)

BOOST_AUTO_TEST_CASE(whitespace_outside_strings_is_free) {
    BOOST_TEST(iam::detail::policy_size(R"({ "a" : "b c" })") == 11u);
    BOOST_TEST(iam::detail::policy_size("{\n\t\"a\":\r\n1}") == 7u);
    // An escaped quote does not end the string, so the space after it counts.
    BOOST_TEST(iam::detail::policy_size(R"({"a\" b":1})") == 11u);
    // Characters, not bytes: "é" is one.
    BOOST_TEST(iam::detail::policy_size("\"\xC3\xA9\"") == 3u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(iam_glob)

BOOST_AUTO_TEST_CASE(engine_edge_cases) {
    using iam::glob;
    BOOST_TEST(glob("").matches(""));
    BOOST_TEST(!glob("").matches("a"));
    BOOST_TEST(glob("*").matches(""));
    BOOST_TEST(glob("**").matches("anything"));
    BOOST_TEST(glob("**").matches_everything());
    BOOST_TEST(glob("a*b*c").matches("abc"));
    BOOST_TEST(glob("a*b*c").matches("aXbYbZc"));
    BOOST_TEST(!glob("a*b*c").matches("aXbYbZ"));
    BOOST_TEST(glob("*ab").matches("aab"));  // backtrack past a partial match
    BOOST_TEST(glob("a?c").matches("abc"));
    BOOST_TEST(!glob("a?c").matches("ac"));
    BOOST_TEST(glob("?").matches("\xC3\xA9"));  // one code point, two bytes
    BOOST_TEST(!glob("??").matches("\xC3\xA9"));
    BOOST_TEST(glob("*?").matches("\xE2\x82\xAC"));  // the star never splits it
    BOOST_TEST(!glob("A").matches("a"));
    BOOST_TEST(glob("A", true).matches("a"));
    BOOST_TEST(glob("a", true).matches("A"));
    // Linear enough for a pathological pattern on a long input.
    const std::string input(10000, 'a');
    BOOST_TEST(!glob("*a*a*a*a*a*a*b").matches(input));
}

BOOST_AUTO_TEST_CASE(arn_split) {
    const auto f = iam::split_arn("arn:aws:s3:::bucket/a:b");
    BOOST_REQUIRE(f.has_value());
    BOOST_TEST((*f)[1] == "aws");
    BOOST_TEST((*f)[2] == "s3");
    BOOST_TEST((*f)[3].empty());
    BOOST_TEST((*f)[4].empty());
    BOOST_TEST((*f)[5] == "bucket/a:b");
    BOOST_TEST(!iam::split_arn("arn:aws:s3").has_value());
    BOOST_TEST(!iam::split_arn("urn:aws:s3:::x").has_value());
    BOOST_TEST(iam::arn_pattern("arn:aws:s3:::*").is_arn_shaped());
    BOOST_TEST(!iam::arn_pattern("arn:aws:s3:*").is_arn_shaped());
}

BOOST_AUTO_TEST_SUITE_END()

// ── Task 11: what a corpus file cannot express well ────────────────────────

BOOST_AUTO_TEST_SUITE(iam_conditions)

BOOST_AUTO_TEST_CASE(global_condition_keys_present_only_when_known) {
    iam::request_facts f;
    f.now = std::chrono::sys_days{std::chrono::year{2026} / 11 / 9} + std::chrono::hours(12) +
            std::chrono::minutes(3) + std::chrono::seconds(4);
    f.secure_transport = true;
    f.source_ip = "203.0.113.7";
    f.principal_arn = "arn:aws:iam::111122223333:user/alice";
    f.principal_account = "111122223333";
    f.principal_type = "User";
    f.username = "alice";
    f.principal_tags = {{"Team", "blue"}};
    f.request_tags = std::vector<std::pair<std::string, std::string>>{{"env", "prod"}};
    f.called_via = {"cloudformation.amazonaws.com", "s3.amazonaws.com"};
    const auto k = iam::global_condition_keys(f);
    BOOST_TEST(k.single("aws:CurrentTime").value_or("") == "2026-11-09T12:03:04Z");
    BOOST_TEST(k.single("aws:EpochTime").value_or("") == "1794225784");
    BOOST_TEST(k.single("aws:SecureTransport").value_or("") == "true");
    BOOST_TEST(k.single("aws:MultiFactorAuthPresent").value_or("") == "false");
    BOOST_TEST(k.single("aws:username").value_or("") == "alice");
    BOOST_TEST(k.single("aws:PrincipalTag/team").value_or("") == "blue");
    BOOST_TEST(k.single("aws:RequestTag/env").value_or("") == "prod");
    BOOST_TEST((*k.find("aws:TagKeys") == std::vector<std::string>{"env"}));
    BOOST_TEST(!k.single("aws:TagKeys").has_value());  // multivalued
    BOOST_TEST(k.single("aws:CalledViaFirst").value_or("") == "cloudformation.amazonaws.com");
    BOOST_TEST(k.single("aws:CalledViaLast").value_or("") == "s3.amazonaws.com");
    // Never invented: absent facts leave their keys absent.
    for (const char* absent : {"aws:TokenIssueTime", "aws:SourceIdentity", "aws:userid",
                               "aws:UserAgent", "aws:ResourceAccount", "aws:SourceVpc"}) {
        BOOST_TEST(k.find(absent) == nullptr, absent);
    }
    // A request with no tags has no aws:TagKeys at all.
    f.request_tags.reset();
    BOOST_TEST(iam::global_condition_keys(f).find("aws:TagKeys") == nullptr);
}

BOOST_AUTO_TEST_CASE(pattern_substitution_escapes_literals) {
    iam::context_keys ctx;
    ctx.set("aws:PrincipalTag/p", "a*b?");
    const auto s = iam::substitute("x/${aws:PrincipalTag/p}/*", ctx, iam::substitution::pattern);
    BOOST_REQUIRE(s.has_value());
    const iam::glob g(*s, false, true);
    BOOST_TEST(g.matches("x/a*b?/anything"));
    BOOST_TEST(!g.matches("x/aZZbQ/anything"));
    BOOST_TEST(iam::substitute("${aws:PrincipalTag/p}", ctx, iam::substitution::plain).value() ==
               "a*b?");
    // An escaped trailing star in an ARN segment is not a segment wildcard.
    const auto arn = iam::substitute("arn:aws:s3:::b${*}", ctx, iam::substitution::pattern);
    BOOST_TEST(iam::arn_pattern(*arn, true).matches("arn:aws:s3:::b*"));
    BOOST_TEST(!iam::arn_pattern(*arn, true).matches("arn:aws:s3:::bx"));
}

BOOST_AUTO_TEST_CASE(operator_table_resolves_every_name) {
    const auto op = iam::parse_condition_operator("ForAnyValue:NumericLessThanEqualsIfExists");
    BOOST_REQUIRE(op.has_value());
    BOOST_TEST((op->fam == iam::condition_operator::family::numeric));
    BOOST_TEST((op->cmp == iam::condition_operator::comparison::le));
    BOOST_TEST(op->if_exists);
    BOOST_TEST((op->set == iam::condition_operator::set_prefix::for_any_value));
    BOOST_TEST(!op->takes_variables());
    BOOST_TEST(iam::parse_condition_operator("ArnNotLike")->negated);
    BOOST_TEST(!iam::parse_condition_operator("Null").value().negated);
    BOOST_TEST(!iam::parse_condition_operator("NullIfExists").has_value());
    BOOST_TEST(!iam::parse_condition_operator("StringEqualsIfExistsIfExists").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
