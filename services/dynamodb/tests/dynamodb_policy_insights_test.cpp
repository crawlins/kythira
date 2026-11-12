// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Resource-based policies and Contributor Insights
// (`.kiro/specs/dynamodb-compatible-api/` task 21, Requirements 5.3 and
// 19.4) over local_backend: the policy calls, revisions and their
// refusals, stream policies, `CreateTable`'s `ResourcePolicy`, what the
// authorizer is handed (through a test authorizer that evaluates a
// policy's denials), the self-lockout guard and the root's delete; the
// insights calls, rule names and listing, the counters, and the signed
// report.

#define BOOST_TEST_MODULE dynamodb_policy_insights_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/insights.hpp>
#include <kythira/dynamodb/insights_api.hpp>
#include <kythira/dynamodb/resource_policy.hpp>

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

/// Authenticates as the static table does, then denies what a resource
/// policy's `Deny` statements name (by exact action or `dynamodb:*`, for
/// `"*"` or the caller's ARN) and allows the rest. Records every request.
struct policy_authorizer {
    az::static_key_authorizer& inner;
    std::vector<az::authz_request> asked;
    bool as_root = false;

    auto authenticate(const sv4::request& req, const sv4::service_rules& rules)
        -> az::authn_result {
        auto who = inner.authenticate(req, rules);
        if (!who || !as_root) {
            return who;
        }
        auto p = *who;
        p.type = az::principal_type::root;
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":root";
        return p;
    }

    auto authorize(const az::principal_context& who, const az::authz_request& r)
        -> az::authz_decision {
        asked.push_back(r);
        az::authz_decision d;
        d.allowed = true;
        if (!r.resource_policy) {
            return d;
        }
        const auto doc = json::parse(*r.resource_policy).as_object();
        std::vector<json::value> statements;
        if (doc.at("Statement").is_array()) {
            for (const auto& s : doc.at("Statement").as_array()) {
                statements.push_back(s);
            }
        } else {
            statements.push_back(doc.at("Statement"));
        }
        const auto names = [](const json::value& v, const std::string& want) {
            if (v.is_string()) {
                return v.as_string() == want || v.as_string() == "*";
            }
            for (const auto& e : v.as_array()) {
                if (e.as_string() == want || e.as_string() == "*") {
                    return true;
                }
            }
            return false;
        };
        for (const auto& s : statements) {
            const auto& o = s.as_object();
            if (o.at("Effect").as_string() != "Deny") {
                continue;
            }
            const auto& principal = o.at("Principal");
            const bool who_matches = principal.is_string()
                                         ? principal.as_string() == "*"
                                         : names(principal.as_object().at("AWS"), who.arn);
            if (who_matches &&
                (names(o.at("Action"), r.action) || names(o.at("Action"), "dynamodb:*"))) {
                d.allowed = false;
                d.explicit_deny = true;
                d.policy = "resource";
                return d;
            }
        }
        return d;
    }
};

static_assert(az::aws_request_authorizer<policy_authorizer>);

auto deny(const std::string& action, const std::string& principal = R"("*")") -> std::string {
    return R"({"Version":"2012-10-17","Statement":[{"Effect":"Deny","Principal":)" + principal +
           R"(,"Action":")" + action + R"(","Resource":"*"}]})";
}

const std::string k_allow =
    R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"arn:aws:iam::111122223333:root"},"Action":"dynamodb:GetItem","Resource":"*"}]})";

struct policy_fixture : fixture {
    ddb::resource_policy_api<ddb::local_backend<>> policies{backend};
    ddb::insights_api<ddb::local_backend<>> insights{backend};
    policy_authorizer judge{auth};
    std::unique_ptr<ddb::front_end<policy_authorizer>> judged;

    policy_fixture() {
        policies.register_with(*fe);
        insights.register_with(*fe);
        ddb::front_end_config cfg;
        cfg.advertised_endpoint = "localhost:8000";
        judged = std::make_unique<ddb::front_end<policy_authorizer>>(cfg, judge);
        tables.register_with(*judged);
        items.register_with(*judged);
        queries.register_with(*judged);
        streams.register_with(*judged);
        policies.register_with(*judged);
        insights.register_with(*judged);
        judged->set_resource_lookup(ddb::resource_lookup_for(backend));
    }

    auto judged_call(std::string_view op, const std::string& body) -> reply {
        const auto r = judged->handle(signed_request(op, body));
        return reply{r.status, json::parse(r.body).as_object()};
    }

    static auto arn(const std::string& table) -> std::string {
        return "arn:aws:dynamodb:us-east-1:" + std::string(k_account) + ":table/" + table;
    }

    auto put_policy(const std::string& resource, const std::string& policy,
                    const std::string& extra = "") -> reply {
        return call("PutResourcePolicy",
                    json::serialize(json::object{{"ResourceArn", resource}, {"Policy", policy}}) +
                        "" + extra);
    }
    auto put_policy_with(const std::string& resource, const std::string& policy, json::object more)
        -> reply {
        more["ResourceArn"] = resource;
        more["Policy"] = policy;
        return call("PutResourcePolicy", json::serialize(more));
    }
    auto get_policy(const std::string& resource) -> reply {
        return call("GetResourcePolicy", json::serialize(json::object{{"ResourceArn", resource}}));
    }
    auto delete_policy(const std::string& resource, json::object more = {}) -> reply {
        more["ResourceArn"] = resource;
        return call("DeleteResourcePolicy", json::serialize(more));
    }

    auto put(const std::string& table, const std::string& pk, int sk) -> reply {
        return call("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":")" + std::to_string(sk) + R"("}}})");
    }
    auto get(const std::string& table, const std::string& pk, int sk) -> reply {
        return call("GetItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":")" + std::to_string(sk) + R"("}}})");
    }

    auto enable(const std::string& table, const std::string& extra = "") -> json::object {
        return ok("UpdateContributorInsights", R"({"TableName":")" + table +
                                                   R"(","ContributorInsightsAction":"ENABLE")" +
                                                   extra + "}");
    }
    auto describe_insights(const std::string& table, const std::string& extra = "")
        -> json::object {
        return ok("DescribeContributorInsights",
                  R"({"TableName":")" + table + R"(")" + extra + "}");
    }

    /// A signed `GET` of the report, through the static-key front end or
    /// the judged one.
    auto report(const std::string& query, bool judged_front = false) -> reply {
        ddb::front_end_request r;
        r.method = "GET";
        r.path = std::string(ddb::k_insights_report_path);
        r.query = query;
        r.source_ip = "192.0.2.10";
        r.headers = {{"Host", "localhost:8000"}, {"X-Amz-Date", "20261110T120000Z"}};
        sv4::service_rules rules;
        rules.service = "dynamodb";
        rules.protocol = sv4::error_protocol::json;
        const sv4::credential_scope scope{"20261110", "us-east-1", "dynamodb"};
        r.headers.emplace_back("Authorization", "AWS4-HMAC-SHA256 Credential=AKIDADMIN/" +
                                                    scope.text() +
                                                    ", SignedHeaders=host;x-amz-date"
                                                    ", Signature=" +
                                                    std::string(64, '0'));
        const auto parsed =
            sv4::parse(sv4::request{r.method, r.path, r.query, r.headers, r.body}, rules, k_now);
        BOOST_REQUIRE(parsed.has_value());
        const auto sig = sv4::compute_signature(*parsed, sv4::derive_signing_key(k_secret, scope));
        r.headers.back().second.replace(r.headers.back().second.size() - 64, 64, sig);
        const auto out = judged_front ? judged->handle(r) : fe->handle(r);
        return reply{out.status, json::parse(out.body).as_object()};
    }
};

auto text(const json::object& o, std::string_view key) -> std::string {
    return std::string(o.at(key).as_string());
}

}  // namespace

// -- resource policies --------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(policies_put_get_and_delete_with_revisions, policy_fixture) {
    (void)create("tbl");
    const auto a = arn("tbl");
    BOOST_TEST(get_policy(a).type() == "PolicyNotFoundException");

    auto r = put_policy(a, k_allow);
    BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
    const auto first = text(r.body, "RevisionId");
    BOOST_TEST(first == std::to_string(clock_ms));

    auto g = get_policy(a);
    BOOST_TEST(text(g.body, "Policy") == k_allow);
    BOOST_TEST(text(g.body, "RevisionId") == first);

    // The same document again keeps its revision (AWS's idempotent put).
    clock_ms += 1000;
    BOOST_TEST(text(put_policy(a, k_allow).body, "RevisionId") == first);

    // A new document gets a later one, even within the same millisecond.
    clock_ms -= 1000;
    const auto second = text(put_policy(a, deny("dynamodb:Scan")).body, "RevisionId");
    BOOST_TEST(std::stoull(second) > std::stoull(first));

    // Deleting answers the deleted policy's revision; again, nothing.
    auto d = delete_policy(a);
    BOOST_REQUIRE(d.status == 200U);
    BOOST_TEST(text(d.body, "RevisionId") == second);
    BOOST_TEST(get_policy(a).type() == "PolicyNotFoundException");
    d = delete_policy(a);
    BOOST_TEST(d.status == 200U);
    BOOST_TEST(!d.body.contains("RevisionId"));
}

BOOST_FIXTURE_TEST_CASE(expected_revisions_guard_changes, policy_fixture) {
    (void)create("tbl");
    const auto a = arn("tbl");
    // NO_POLICY: only while there is none.
    auto r = put_policy_with(a, k_allow, {{"ExpectedRevisionId", "NO_POLICY"}});
    BOOST_REQUIRE(r.status == 200U);
    const auto rev = text(r.body, "RevisionId");
    r = put_policy_with(a, deny("dynamodb:Scan"), {{"ExpectedRevisionId", "NO_POLICY"}});
    BOOST_TEST(r.type() == "PolicyNotFoundException");

    // A revision that is not the current one.
    clock_ms += 5;
    r = put_policy_with(a, deny("dynamodb:Scan"), {{"ExpectedRevisionId", "12345"}});
    BOOST_TEST(r.type() == "PolicyNotFoundException");
    BOOST_TEST(r.message().find("revision id 12345") != std::string::npos);
    BOOST_TEST(r.message().find(rev) != std::string::npos);
    BOOST_TEST(text(get_policy(a).body, "Policy") == k_allow);

    // The current one.
    r = put_policy_with(a, deny("dynamodb:Scan"), {{"ExpectedRevisionId", rev}});
    BOOST_REQUIRE(r.status == 200U);
    const auto next = text(r.body, "RevisionId");

    BOOST_TEST(delete_policy(a, {{"ExpectedRevisionId", rev}}).type() == "PolicyNotFoundException");
    BOOST_TEST(delete_policy(a, {{"ExpectedRevisionId", next}}).status == 200U);
    // With an expected revision, deleting nothing is refused.
    BOOST_TEST(delete_policy(a, {{"ExpectedRevisionId", next}}).type() ==
               "PolicyNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(policy_calls_refuse_bad_documents_and_resources, policy_fixture) {
    (void)create("tbl");
    const auto a = arn("tbl");
    BOOST_TEST(put_policy(a, "not json").type() == "ValidationException");
    BOOST_TEST(put_policy(a, "[]").type() == "ValidationException");
    BOOST_TEST(put_policy(a, R"({"Version":"2012-10-17"})").type() == "ValidationException");
    BOOST_TEST(put_policy(a, R"({"Statement":[]})").type() == "ValidationException");
    BOOST_TEST(put_policy(a, R"({"Statement":{"Effect":"Allow","Action":"dynamodb:*"}})").type() ==
               "ValidationException");
    BOOST_TEST(
        put_policy(a, R"({"Statement":{"Effect":"Maybe","Principal":"*","Action":"*"}})").type() ==
        "ValidationException");
    BOOST_TEST(put_policy(a, R"({"Statement":{"Effect":"Allow","Principal":"*"}})").type() ==
               "ValidationException");

    // 20 KB, whitespace counted.
    std::string big = k_allow;
    big.insert(1, ddb::table_api_detail::k_max_policy_bytes - big.size(), ' ');
    BOOST_TEST(put_policy(a, big).status == 200U);
    big.insert(1, " ");
    const auto over = put_policy(a, big);
    BOOST_TEST(over.type() == "ValidationException");
    BOOST_TEST(over.message().find("20 KB") != std::string::npos);

    BOOST_TEST(put_policy(a + "/index/gsi", k_allow).type() == "ValidationException");
    BOOST_TEST(put_policy("arn:aws:s3:::bucket", k_allow).type() == "ValidationException");
    BOOST_TEST(put_policy(arn("missing"), k_allow).type() == "ResourceNotFoundException");
    BOOST_TEST(get_policy(arn("missing")).type() == "ResourceNotFoundException");
    BOOST_TEST(delete_policy(arn("missing")).type() == "ResourceNotFoundException");
    BOOST_TEST(put_policy("arn:aws:dynamodb:us-east-1:999999999999:table/tbl", k_allow).type() ==
               "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(streams_carry_their_own_policies, policy_fixture) {
    const auto t = create("tbl", R"(,"StreamSpecification":{"StreamEnabled":true,)"
                                 R"("StreamViewType":"KEYS_ONLY"})");
    const auto stream = text(t.at("TableDescription").as_object(), "LatestStreamArn");
    BOOST_REQUIRE(put_policy(stream, deny("dynamodb:GetRecords")).status == 200U);
    BOOST_TEST(text(get_policy(stream).body, "Policy") == deny("dynamodb:GetRecords"));
    BOOST_TEST(get_policy(arn("tbl")).type() == "PolicyNotFoundException");
    BOOST_TEST(put_policy(arn("tbl") + "/stream/2001-01-01T00:00:00.000", k_allow).type() ==
               "ResourceNotFoundException");
    BOOST_TEST(delete_policy(stream).status == 200U);
    BOOST_TEST(get_policy(stream).type() == "PolicyNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(create_table_attaches_a_resource_policy, policy_fixture) {
    (void)create("tbl", R"(,"ResourcePolicy":)" + json::serialize(json::value(k_allow)));
    const auto g = get_policy(arn("tbl"));
    BOOST_REQUIRE_MESSAGE(g.status == 200U, g.type() + ": " + g.message());
    BOOST_TEST(text(g.body, "Policy") == k_allow);
    BOOST_TEST(text(g.body, "RevisionId") == std::to_string(clock_ms));

    const auto bad = call("CreateTable",
                          R"({"TableName":"other","ResourcePolicy":"nope",)"
                          R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                          R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"}],)"
                          R"("BillingMode":"PAY_PER_REQUEST"})");
    BOOST_TEST(bad.type() == "ValidationException");
    BOOST_TEST(call("DescribeTable", R"({"TableName":"other"})").type() ==
               "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(the_authorizer_gets_the_resource_policy_and_tags, policy_fixture) {
    const auto t = create(
        "tbl", R"(,"Tags":[{"Key":"team","Value":"payments"}],)"
               R"("StreamSpecification":{"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    const auto stream = text(t.at("TableDescription").as_object(), "LatestStreamArn");
    BOOST_REQUIRE(put_policy(arn("tbl"), k_allow).status == 200U);
    BOOST_REQUIRE(put_policy(stream, deny("dynamodb:GetRecords")).status == 200U);

    BOOST_TEST(
        judged_call("GetItem", R"({"TableName":"tbl","Key":{"pk":{"S":"a"},"sk":{"N":"1"}}})")
            .status == 200U);
    const auto& get = judge.asked.back();
    BOOST_TEST(get.action == "dynamodb:GetItem");
    BOOST_TEST(get.resource_policy.value_or("") == k_allow);
    BOOST_REQUIRE(get.resource_tags.size() == 1U);
    BOOST_TEST(get.resource_tags[0].second == "payments");
    BOOST_TEST(get.resource_account.empty());

    // A stream gets its own policy and the table's tags.
    BOOST_TEST(judged_call("DescribeStream", json::serialize(json::object{{"StreamArn", stream}}))
                   .status == 200U);
    BOOST_TEST(judge.asked.back().resource_policy.value_or("") == deny("dynamodb:GetRecords"));
    BOOST_TEST(judge.asked.back().resource_tags.size() == 1U);

    // A table that does not exist has no facts.
    (void)judged_call("GetItem", R"({"TableName":"nope","Key":{"pk":{"S":"a"},"sk":{"N":"1"}}})");
    BOOST_TEST(!judge.asked.back().resource_policy.has_value());
    BOOST_TEST(judge.asked.back().resource_tags.empty());
}

BOOST_FIXTURE_TEST_CASE(an_index_is_governed_by_its_table_policy, policy_fixture) {
    (void)create(
        "tbl", R"(,"GlobalSecondaryIndexes":[{"IndexName":"by_sk","KeySchema":)"
               R"([{"AttributeName":"sk","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"},)"
               R"("ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5}}])");
    BOOST_REQUIRE(put_policy(arn("tbl"), deny("dynamodb:Query")).status == 200U);
    const auto r = judged_call(
        "Query", R"({"TableName":"tbl","IndexName":"by_sk","KeyConditionExpression":"sk = :v",)"
                 R"("ExpressionAttributeValues":{":v":{"N":"1"}}})");
    BOOST_TEST(r.type() == "AccessDeniedException");
    BOOST_TEST(judge.asked.back().resource == arn("tbl") + "/index/by_sk");
    BOOST_TEST(r.message().find("explicit deny in a resource-based policy") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(a_resource_policy_denial_is_enforced, policy_fixture) {
    (void)create("tbl");
    BOOST_REQUIRE(put_policy(arn("tbl"), deny("dynamodb:GetItem")).status == 200U);
    const auto r =
        judged_call("GetItem", R"({"TableName":"tbl","Key":{"pk":{"S":"a"},"sk":{"N":"1"}}})");
    BOOST_TEST(r.type() == "AccessDeniedException");
    // Other actions still pass, and the static-key front end has no policies.
    BOOST_TEST(judged_call("DescribeTable", R"({"TableName":"tbl"})").status == 200U);
    BOOST_TEST(get("tbl", "a", 1).status == 200U);
}

BOOST_FIXTURE_TEST_CASE(a_policy_that_locks_the_caller_out_needs_confirmation, policy_fixture) {
    (void)create("tbl");
    const auto a = arn("tbl");
    const auto lock = deny("dynamodb:*");
    const auto body = [&](bool confirm) {
        json::object o{{"ResourceArn", a}, {"Policy", lock}};
        if (confirm) {
            o["ConfirmRemoveSelfResourceAccess"] = true;
        }
        return json::serialize(o);
    };
    auto r = judged_call("PutResourcePolicy", body(false));
    BOOST_TEST(r.type() == "ValidationException");
    BOOST_TEST(r.message().find("ConfirmRemoveSelfResourceAccess") != std::string::npos);
    BOOST_TEST(get_policy(a).type() == "PolicyNotFoundException");

    BOOST_REQUIRE(judged_call("PutResourcePolicy", body(true)).status == 200U);
    // Now locked out, even from deleting it ...
    BOOST_TEST(judged_call("PutResourcePolicy", body(true)).type() == "AccessDeniedException");
    BOOST_TEST(
        judged_call("DeleteResourcePolicy", json::serialize(json::object{{"ResourceArn", a}}))
            .type() == "AccessDeniedException");
    // ... but the account root may always delete.
    judge.as_root = true;
    BOOST_TEST(judged_call("GetResourcePolicy", json::serialize(json::object{{"ResourceArn", a}}))
                   .type() == "AccessDeniedException");
    BOOST_TEST(
        judged_call("DeleteResourcePolicy", json::serialize(json::object{{"ResourceArn", a}}))
            .status == 200U);
    BOOST_TEST(!judge.asked.back().resource_policy.has_value());
}

BOOST_AUTO_TEST_CASE(policies_and_insights_survive_the_stored_forms) {
    ddb::table_descriptor d;
    d.name = "tbl";
    d.table_id = 7;
    d.schema.partition_name = "pk";
    d.policies = {ddb::resource_policy{0, k_allow, "1794300000123"},
                  ddb::resource_policy{1794300000000, deny("dynamodb:GetRecords"), "17"}};
    d.insights = {
        ddb::contributor_insights{"", true, ddb::insights_mode::accessed_and_throttled, 5},
        ddb::contributor_insights{"gsi", false, ddb::insights_mode::throttled, 9}};
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 8);
    BOOST_TEST((ddb::decode_table_descriptor(bytes) == d));

    // Without either, a descriptor keeps its older version.
    auto plain = d;
    plain.policies.clear();
    plain.insights.clear();
    BOOST_TEST(static_cast<int>(ddb::encode_table_descriptor(plain)[0]) == 3);

    for (const auto op : {ddb::catalog_command::kind::set_resource_policy,
                          ddb::catalog_command::kind::set_contributor_insights}) {
        ddb::catalog_command c;
        c.op = op;
        c.table.name = "tbl";
        c.now_ms = 11;
        const auto enc = ddb::encode_catalog_command(c);
        BOOST_TEST(static_cast<int>(enc[0]) == 8);
        bool newer = false;
        BOOST_TEST((ddb::decode_catalog_command(enc, newer) == c));
        // An older form cannot carry the new commands.
        auto old = enc;
        old[0] = 7;
        BOOST_TEST(!ddb::decode_catalog_command(old, newer).has_value());
    }
}

// -- contributor insights -----------------------------------------------------

BOOST_FIXTURE_TEST_CASE(insights_enable_describe_and_disable, policy_fixture) {
    (void)create("tbl");
    auto d = describe_insights("tbl");
    BOOST_TEST(text(d, "ContributorInsightsStatus") == "DISABLED");
    BOOST_TEST(!d.contains("ContributorInsightsRuleList"));
    BOOST_TEST(!d.contains("LastUpdateDateTime"));

    auto u = enable("tbl");
    BOOST_TEST(text(u, "TableName") == "tbl");
    BOOST_TEST(text(u, "ContributorInsightsStatus") == "ENABLING");
    BOOST_TEST(text(u, "ContributorInsightsMode") == "ACCESSED_AND_THROTTLED_KEYS");

    d = describe_insights("tbl");
    BOOST_TEST(text(d, "ContributorInsightsStatus") == "ENABLED");
    BOOST_TEST(d.at("LastUpdateDateTime").as_double() == static_cast<double>(clock_ms) / 1000.0);
    const auto& rules = d.at("ContributorInsightsRuleList").as_array();
    BOOST_REQUIRE(rules.size() == 4U);
    const auto stamp = std::to_string(clock_ms);
    BOOST_TEST(rules[0].as_string() == "DynamoDBContributorInsights-PKC-tbl-" + stamp);
    BOOST_TEST(rules[1].as_string() == "DynamoDBContributorInsights-SKC-tbl-" + stamp);
    BOOST_TEST(rules[2].as_string() == "DynamoDBContributorInsights-PKT-tbl-" + stamp);
    BOOST_TEST(rules[3].as_string() == "DynamoDBContributorInsights-SKT-tbl-" + stamp);

    // THROTTLED_KEYS keeps only the throttle rules.
    (void)enable("tbl", R"(,"ContributorInsightsMode":"THROTTLED_KEYS")");
    d = describe_insights("tbl");
    BOOST_TEST(d.at("ContributorInsightsRuleList").as_array().size() == 2U);
    BOOST_TEST(text(d, "ContributorInsightsMode") == "THROTTLED_KEYS");

    u = ok("UpdateContributorInsights",
           R"({"TableName":"tbl","ContributorInsightsAction":"DISABLE"})");
    BOOST_TEST(text(u, "ContributorInsightsStatus") == "DISABLING");
    BOOST_TEST(text(u, "ContributorInsightsMode") == "THROTTLED_KEYS");
    d = describe_insights("tbl");
    BOOST_TEST(text(d, "ContributorInsightsStatus") == "DISABLED");
    BOOST_TEST(!d.contains("ContributorInsightsRuleList"));
    BOOST_TEST(d.contains("LastUpdateDateTime"));

    BOOST_TEST(call("UpdateContributorInsights",
                    R"({"TableName":"nope","ContributorInsightsAction":"ENABLE"})")
                   .type() == "ResourceNotFoundException");
    BOOST_TEST(
        call("DescribeContributorInsights", R"({"TableName":"tbl","IndexName":"nope"})").type() ==
        "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(insights_cover_gsis_but_not_lsis, policy_fixture) {
    (void)create(
        "tbl",
        R"(,"AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"g","AttributeType":"S"}],)"
        R"("GlobalSecondaryIndexes":[{"IndexName":"by_g","KeySchema":)"
        R"([{"AttributeName":"g","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"},)"
        R"("ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5}}],)"
        R"("LocalSecondaryIndexes":[{"IndexName":"by_lg","KeySchema":)"
        R"([{"AttributeName":"pk","KeyType":"HASH"},{"AttributeName":"g","KeyType":"RANGE"}],)"
        R"("Projection":{"ProjectionType":"KEYS_ONLY"}}])");
    const auto u = enable("tbl", R"(,"IndexName":"by_g")");
    BOOST_TEST(text(u, "IndexName") == "by_g");
    const auto d = describe_insights("tbl", R"(,"IndexName":"by_g")");
    BOOST_TEST(text(d, "ContributorInsightsStatus") == "ENABLED");
    // The GSI has no sort key: two rules, named with the index.
    const auto& rules = d.at("ContributorInsightsRuleList").as_array();
    BOOST_REQUIRE(rules.size() == 2U);
    BOOST_TEST(rules[0].as_string() ==
               "DynamoDBContributorInsights-PKC-tbl-by_g-" + std::to_string(clock_ms));
    BOOST_TEST(text(describe_insights("tbl"), "ContributorInsightsStatus") == "DISABLED");
    BOOST_TEST(
        call("UpdateContributorInsights",
             R"({"TableName":"tbl","IndexName":"by_lg","ContributorInsightsAction":"ENABLE"})")
            .type() == "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(rule_names_fit_within_128_characters, policy_fixture) {
    const std::string name(255, 'n');
    (void)create(name);
    (void)enable(name);
    const std::regex pattern("[A-Za-z0-9][A-Za-z0-9\\-\\_\\.]{0,126}[A-Za-z0-9]");
    const auto d = describe_insights(name);
    for (const auto& r : d.at("ContributorInsightsRuleList").as_array()) {
        const std::string s(r.as_string());
        BOOST_TEST(s.size() <= 128U);
        BOOST_TEST(std::regex_match(s, pattern));
        BOOST_TEST(s.ends_with("-" + std::to_string(clock_ms)));
    }
}

BOOST_FIXTURE_TEST_CASE(insights_list_by_table_and_across_tables, policy_fixture) {
    (void)create("aaa");
    (void)create(
        "bbb", R"(,"GlobalSecondaryIndexes":[{"IndexName":"by_sk","KeySchema":)"
               R"([{"AttributeName":"sk","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"},)"
               R"("ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5}}])");
    (void)create("ccc");

    // By table: the table and each GSI, set or not.
    auto l = ok("ListContributorInsights", R"({"TableName":"bbb"})");
    auto s = l.at("ContributorInsightsSummaries").as_array();
    BOOST_REQUIRE(s.size() == 2U);
    BOOST_TEST(!s[0].as_object().contains("IndexName"));
    BOOST_TEST(text(s[1].as_object(), "IndexName") == "by_sk");
    BOOST_TEST(text(s[1].as_object(), "ContributorInsightsStatus") == "DISABLED");

    // Across tables: what was ever set.
    BOOST_TEST(
        ok("ListContributorInsights", "{}").at("ContributorInsightsSummaries").as_array().empty());
    (void)enable("aaa");
    (void)enable("bbb", R"(,"IndexName":"by_sk")");
    (void)enable("ccc");
    (void)ok("UpdateContributorInsights",
             R"({"TableName":"ccc","ContributorInsightsAction":"DISABLE"})");
    l = ok("ListContributorInsights", "{}");
    s = l.at("ContributorInsightsSummaries").as_array();
    BOOST_REQUIRE(s.size() == 3U);
    BOOST_TEST(text(s[0].as_object(), "TableName") == "aaa");
    BOOST_TEST(text(s[1].as_object(), "IndexName") == "by_sk");
    BOOST_TEST(text(s[2].as_object(), "ContributorInsightsStatus") == "DISABLED");
    BOOST_TEST(!l.contains("NextToken"));

    // Paged one at a time.
    std::vector<std::string> seen;
    std::string token;
    for (int i = 0; i < 10; ++i) {
        json::object in{{"MaxResults", 1}};
        if (!token.empty()) {
            in["NextToken"] = token;
        }
        const auto page = ok("ListContributorInsights", json::serialize(in));
        for (const auto& e : page.at("ContributorInsightsSummaries").as_array()) {
            seen.push_back(text(e.as_object(), "TableName"));
        }
        if (!page.contains("NextToken")) {
            break;
        }
        token = text(page, "NextToken");
    }
    BOOST_TEST((seen == std::vector<std::string>{"aaa", "bbb", "ccc"}));

    BOOST_TEST(call("ListContributorInsights", R"({"MaxResults":0})").type() ==
               "ValidationException");
    BOOST_TEST(call("ListContributorInsights", R"({"NextToken":"junk"})").type() ==
               "ValidationException");
    BOOST_TEST(call("ListContributorInsights", R"({"TableName":"nope"})").type() ==
               "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(the_report_ranks_accessed_keys, policy_fixture) {
    without_throttling();
    (void)create("tbl");
    // Not counted before the enable.
    for (int i = 0; i < 5; ++i) {
        BOOST_REQUIRE(put("tbl", "cold", 1).status == 200U);
    }
    (void)enable("tbl");
    for (int i = 0; i < 6; ++i) {
        BOOST_REQUIRE(put("tbl", "hot", 1).status == 200U);
        BOOST_REQUIRE(get("tbl", "hot", 1).status == 200U);
    }
    BOOST_REQUIRE(put("tbl", "hot", 2).status == 200U);
    BOOST_REQUIRE(put("tbl", "warm", 1).status == 200U);

    auto r = report("Rule=PKC&TableName=tbl");
    BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
    BOOST_TEST(text(r.body, "ContributorInsightsStatus") == "ENABLED");
    BOOST_TEST(r.body.at("Minutes").as_int64() == 60);
    auto rules = r.body.at("Rules").as_array();
    BOOST_REQUIRE(rules.size() == 1U);
    BOOST_TEST(text(rules[0].as_object(), "Rule") == "PKC");
    auto top = rules[0].as_object().at("Contributors").as_array();
    BOOST_REQUIRE(top.size() == 2U);
    BOOST_TEST(json::serialize(top[0].as_object().at("Key")) == R"({"pk":{"S":"hot"}})");
    // Six one-unit writes, six half-unit eventually consistent reads, one more write.
    BOOST_TEST(top[0].as_object().at("Value").as_double() == 10.0);
    BOOST_TEST(json::serialize(top[1].as_object().at("Key")) == R"({"pk":{"S":"warm"}})");

    // The whole key separates hot/1 from hot/2.
    r = report("TableName=tbl&Rule=SKC&MaxContributors=1");
    top = r.body.at("Rules").as_array()[0].as_object().at("Contributors").as_array();
    BOOST_REQUIRE(top.size() == 1U);
    BOOST_TEST(json::serialize(top[0].as_object().at("Key")) ==
               R"({"pk":{"S":"hot"},"sk":{"N":"1"}})");

    // All rules by default; nothing throttled.
    r = report("TableName=tbl");
    rules = r.body.at("Rules").as_array();
    BOOST_REQUIRE(rules.size() == 4U);
    BOOST_TEST(rules[2].as_object().at("Contributors").as_array().empty());

    // A rule asked for by name.
    const auto name = describe_insights("tbl").at("ContributorInsightsRuleList").as_array()[1];
    r = report("TableName=tbl&Rule=" + std::string(name.as_string()));
    BOOST_TEST(text(r.body.at("Rules").as_array()[0].as_object(), "Rule") == "SKC");

    // Past the window, nothing.
    clock_ms += 61ULL * 60'000ULL;
    r = report("TableName=tbl&Rule=PKC");
    BOOST_TEST(r.body.at("Rules").as_array()[0].as_object().at("Contributors").as_array().empty());

    BOOST_TEST(report("TableName=tbl&Minutes=0").type() == "ValidationException");
    BOOST_TEST(report("TableName=tbl&Minutes=61").type() == "ValidationException");
    BOOST_TEST(report("TableName=tbl&MaxContributors=101").type() == "ValidationException");
    BOOST_TEST(report("TableName=tbl&Rule=XYZ").type() == "ValidationException");
    BOOST_TEST(report("Rule=PKC").type() == "ValidationException");
    BOOST_TEST(report("TableName=nope").type() == "ResourceNotFoundException");
    BOOST_TEST(report("TableName=tbl&IndexName=nope").type() == "ResourceNotFoundException");
}

BOOST_FIXTURE_TEST_CASE(the_report_ranks_throttled_keys_and_restarts_on_enable, policy_fixture) {
    (void)create("tbl", "");
    (void)enable("tbl", R"(,"ContributorInsightsMode":"THROTTLED_KEYS")");
    // A 300 KB write drains the five-unit table's burst; the next throttle.
    const std::string big(300 * 1024, 'x');
    const auto write = [&](const std::string& pk) {
        return call("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":"1"},"v":{"S":")" + big + R"("}}})");
    };
    int throttled = 0;
    for (int i = 0; i < 8; ++i) {
        if (write("hot").type() == "ProvisionedThroughputExceededException") {
            ++throttled;
        }
    }
    BOOST_REQUIRE(throttled > 0);
    auto r = report("TableName=tbl&Rule=PKT");
    auto top = r.body.at("Rules").as_array()[0].as_object().at("Contributors").as_array();
    BOOST_REQUIRE(top.size() == 1U);
    BOOST_TEST(json::serialize(top[0].as_object().at("Key")) == R"({"pk":{"S":"hot"}})");
    BOOST_TEST(top[0].as_object().at("Value").as_double() == static_cast<double>(throttled));
    // THROTTLED_KEYS has no accessed rules.
    BOOST_TEST(report("TableName=tbl&Rule=PKC").type() == "ValidationException");

    // A new enable starts its rules afresh.
    clock_ms += 1;
    (void)enable("tbl");
    r = report("TableName=tbl&Rule=PKT");
    BOOST_TEST(r.body.at("Rules").as_array()[0].as_object().at("Contributors").as_array().empty());

    // Disabled: no rules, nothing to report.
    (void)ok("UpdateContributorInsights",
             R"({"TableName":"tbl","ContributorInsightsAction":"DISABLE"})");
    r = report("TableName=tbl");
    BOOST_TEST(r.status == 200U);
    BOOST_TEST(r.body.at("Rules").as_array().empty());
}

BOOST_FIXTURE_TEST_CASE(the_report_is_signed_and_authorized, policy_fixture) {
    (void)create("tbl");
    ddb::front_end_request r;
    r.method = "GET";
    r.path = std::string(ddb::k_insights_report_path);
    r.query = "TableName=tbl";
    const auto out = fe->handle(r);
    BOOST_TEST(out.status >= 400U);
    BOOST_TEST(out.body.find("Rules") == std::string::npos);

    // Authorized as DescribeContributorInsights on the table or index.
    BOOST_TEST(report("TableName=tbl", true).status == 200U);
    BOOST_TEST(judge.asked.back().action == "dynamodb:DescribeContributorInsights");
    BOOST_TEST(judge.asked.back().resource == arn("tbl"));
    BOOST_REQUIRE(put_policy(arn("tbl"), deny("dynamodb:DescribeContributorInsights")).status ==
                  200U);
    BOOST_TEST(report("TableName=tbl", true).type() == "AccessDeniedException");
    BOOST_TEST(report("TableName=tbl&IndexName=idx", true).type() == "AccessDeniedException");
    BOOST_TEST(judge.asked.back().resource == arn("tbl") + "/index/idx");
}

BOOST_AUTO_TEST_CASE(space_saving_keeps_heavy_hitters) {
    ddb::top_k_counter c(2);
    for (int i = 0; i < 100; ++i) {
        c.add("heavy", 1.0);
        c.add("noise" + std::to_string(i), 1.0);
    }
    BOOST_TEST(c.counts().size() == 2U);
    BOOST_TEST(c.counts().contains("heavy"));
    BOOST_TEST(c.counts().at("heavy") >= 100.0);
}

BOOST_AUTO_TEST_CASE(the_recorder_keeps_an_hour_of_minutes) {
    ddb::insights_recorder rec(16, 60);
    ddb::table_descriptor t;
    t.table_id = 3;
    t.insights = {
        ddb::contributor_insights{"", true, ddb::insights_mode::accessed_and_throttled, 1}};
    const std::uint64_t t0 = 1'000'000ULL * 60'000ULL;
    rec.accessed(t, {"", "k1", ""}, 2.0, t0);
    rec.accessed(t, {"", "k2", ""}, 1.0, t0 + 10ULL * 60'000ULL);
    auto top = rec.top(t, t.insights[0], ddb::insights_rule::pkc, t0 + 10ULL * 60'000ULL, 60, 10);
    BOOST_REQUIRE(top.size() == 2U);
    BOOST_TEST(top[0].key == "k1");
    // The last five minutes hold only k2.
    top = rec.top(t, t.insights[0], ddb::insights_rule::pkc, t0 + 10ULL * 60'000ULL, 5, 10);
    BOOST_REQUIRE(top.size() == 1U);
    BOOST_TEST(top[0].key == "k2");
    // An hour on, k1's minute is gone even from the counters.
    rec.accessed(t, {"", "k3", ""}, 1.0, t0 + 60ULL * 60'000ULL);
    top = rec.top(t, t.insights[0], ddb::insights_rule::pkc, t0 + 60ULL * 60'000ULL, 60, 10);
    BOOST_TEST(top.size() == 2U);
    // A disabled setting counts nothing; a dropped table forgets.
    t.insights[0].enabled = false;
    rec.accessed(t, {"", "k4", ""}, 1.0, t0 + 60ULL * 60'000ULL);
    t.insights[0].enabled = true;
    BOOST_TEST(
        rec.top(t, t.insights[0], ddb::insights_rule::pkc, t0 + 60ULL * 60'000ULL, 60, 10).size() ==
        2U);
    rec.forget(3);
    BOOST_TEST(
        rec.top(t, t.insights[0], ddb::insights_rule::pkc, t0 + 60ULL * 60'000ULL, 60, 10).empty());
}

BOOST_FIXTURE_TEST_CASE(deleting_a_gsi_drops_its_insights, policy_fixture) {
    (void)create(
        "tbl", R"(,"GlobalSecondaryIndexes":[{"IndexName":"by_sk","KeySchema":)"
               R"([{"AttributeName":"sk","KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"},)"
               R"("ProvisionedThroughput":{"ReadCapacityUnits":5,"WriteCapacityUnits":5}}])");
    (void)enable("tbl", R"(,"IndexName":"by_sk")");
    (void)ok(
        "UpdateTable",
        R"({"TableName":"tbl","GlobalSecondaryIndexUpdates":[{"Delete":{"IndexName":"by_sk"}}]})");
    const auto d = backend.describe("tbl");
    BOOST_REQUIRE(d.has_value());
    BOOST_TEST(d->insights_of("by_sk") == nullptr);
}
