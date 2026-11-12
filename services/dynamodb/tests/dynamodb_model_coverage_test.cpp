// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Operation coverage (`.kiro/specs/dynamodb-compatible-api/` task 1,
// Requirements 2.1 and 2.2): every operation in the pinned DynamoDB and
// DynamoDB Streams models is implemented (operations.hpp) or listed in
// doc/unsupported.md, never both, every listed one is out of scope (task
// 23), and every listed one answers the error the document says. Also that
// the generated shape tables load into the core AWS JSON codec as expected.

#define BOOST_TEST_MODULE dynamodb_model_coverage_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/operations.hpp>

#include <raft/aws_json_protocol.hpp>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aj = kythira::aws_json;
namespace ddb = kythira::dynamodb;

namespace {

/// Service name as unsupported.md writes it -> listed operations.
auto read_unsupported() -> std::map<std::string, std::vector<std::string>> {
    const std::string path = std::string(KYTHIRA_DYNAMODB_SOURCE_DIR) + "/doc/unsupported.md";
    std::ifstream in(path);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::map<std::string, std::vector<std::string>> out;
    std::string line;
    while (std::getline(in, line)) {
        // | Service | `Operation` | Why |
        if (!line.starts_with("| ")) {
            continue;
        }
        const auto tick = line.find("| `");
        if (tick == std::string::npos) {
            continue;
        }
        const auto service = line.substr(2, tick - 3);
        const auto start = tick + 3;
        const auto end = line.find('`', start);
        BOOST_REQUIRE_MESSAGE(end != std::string::npos, "malformed row: " << line);
        out[service].push_back(line.substr(start, end - start));
    }
    return out;
}

void check_service(const aj::service_model& model, std::span<const std::string_view> implemented,
                   const std::vector<std::string>& listed) {
    std::set<std::string> model_ops;
    for (const auto& op : model.operations) {
        model_ops.emplace(op.name);
    }
    std::set<std::string> impl(implemented.begin(), implemented.end());
    std::set<std::string> unsupported;
    for (const auto& op : listed) {
        BOOST_TEST_INFO("listed twice: " << op);
        BOOST_TEST(unsupported.insert(op).second);
    }
    for (const auto& op : model_ops) {
        BOOST_TEST_INFO(model.target_prefix << "." << op);
        // Exactly one of the two (Requirement 2.2).
        BOOST_TEST(impl.contains(op) != unsupported.contains(op));
    }
    for (const auto& op : unsupported) {
        BOOST_TEST_INFO("listed but not in the model: " << op);
        BOOST_TEST(model_ops.contains(op));
    }
    for (const auto& op : impl) {
        BOOST_TEST_INFO("implemented but not in the model: " << op);
        BOOST_TEST(model_ops.contains(op));
    }
}

/// What the module's dispatcher answers for `op`, with no handlers
/// registered (as for every unsupported operation).
auto unsupported_answer(const aj::service_model& model, std::string_view op) -> aj::http_response {
    aj::codec_options o;
    o.style = aj::error_style::coral(ddb::k_dynamodb_error_namespace);
    const aj::codec c(model, o);
    const aj::dispatcher d(c);
    const std::string target = std::string(model.target_prefix) + "." + std::string(op);
    // A body that would fail validation for nearly every operation: the
    // answer must not depend on it.
    return d.handle({"POST", "/", aj::content_type_of(model.version), target, R"({"x":1})"});
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_pinned_models_are_the_expected_services) {
    const auto& m = ddb::dynamodb_model::k_model;
    BOOST_TEST(m.target_prefix == ddb::k_dynamodb_target_prefix);
    BOOST_TEST((m.version == aj::json_version::v1_0));
    const auto& s = ddb::dynamodbstreams_model::k_model;
    BOOST_TEST(s.target_prefix == ddb::k_streams_target_prefix);
    BOOST_TEST((s.version == aj::json_version::v1_0));
    BOOST_TEST(s.operations.size() == 4U);
    // Pinned botocore 1.43.126: 62 DynamoDB operations. A model upgrade
    // changes this on purpose, with the generated diff (Requirement 2.4).
    BOOST_TEST(m.operations.size() == 62U);
}

BOOST_AUTO_TEST_CASE(every_operation_is_implemented_or_listed) {
    const auto listed = read_unsupported();
    for (const auto& [service, ops] : listed) {
        BOOST_TEST_INFO("service column: " << service);
        BOOST_TEST((service == "DynamoDB" || service == "DynamoDB Streams"));
    }
    const auto get = [&](const std::string& s) {
        const auto it = listed.find(s);
        return it == listed.end() ? std::vector<std::string>{} : it->second;
    };
    check_service(ddb::dynamodb_model::k_model, ddb::k_implemented_dynamodb_operations,
                  get("DynamoDB"));
    check_service(ddb::dynamodbstreams_model::k_model, ddb::k_implemented_streams_operations,
                  get("DynamoDB Streams"));
}

BOOST_AUTO_TEST_CASE(every_listed_operation_is_out_of_scope) {
    // Task 23 closed the coverage: what is left unimplemented is out of
    // scope, with the reason, not waiting for a task (Requirement 2.1).
    const std::string path = std::string(KYTHIRA_DYNAMODB_SOURCE_DIR) + "/doc/unsupported.md";
    std::ifstream in(path);
    BOOST_REQUIRE_MESSAGE(in.good(), "cannot read " << path);
    std::string line;
    std::size_t rows = 0;
    while (std::getline(in, line)) {
        const auto tick = line.find("| `");
        if (!line.starts_with("| ") || tick == std::string::npos) {
            continue;
        }
        const auto why = line.find("` | ", tick + 3);
        BOOST_REQUIRE_MESSAGE(why != std::string::npos, "malformed row: " << line);
        BOOST_TEST_INFO(line);
        BOOST_TEST(line.substr(why + 4).starts_with("out of scope: "));
        ++rows;
    }
    BOOST_TEST(rows > 0U);
}

BOOST_AUTO_TEST_CASE(every_listed_operation_answers_unknown_operation) {
    const auto listed = read_unsupported();
    const std::string want = R"({"__type":"com.amazon.coral.service#UnknownOperationException"})";
    for (const auto& [service, ops] : listed) {
        const auto& model = service == "DynamoDB" ? ddb::dynamodb_model::k_model
                                                  : ddb::dynamodbstreams_model::k_model;
        for (const auto& op : ops) {
            const auto r = unsupported_answer(model, op);
            BOOST_TEST_INFO(service << " " << op);
            BOOST_TEST(r.status == 400U);
            BOOST_TEST(r.body == want);
            BOOST_TEST(r.header("x-amz-crc32").has_value());
        }
    }
}

BOOST_AUTO_TEST_CASE(the_shape_tables_load_into_the_codec) {
    // Only the two auto-scaling shapes use Java-only pattern syntax; every
    // other pattern compiles for std::regex.
    const aj::shape_validator v(ddb::dynamodb_model::k_model);
    std::vector<std::string> skipped = v.skipped_patterns();
    std::ranges::sort(skipped);
    BOOST_TEST(
        (skipped == std::vector<std::string>{"AutoScalingPolicyName", "AutoScalingRoleArn"}));
    const aj::shape_validator sv(ddb::dynamodbstreams_model::k_model);
    BOOST_TEST(sv.skipped_patterns().empty());

    // Every operation's input and output shape exists and is a structure.
    for (const auto* m : {&ddb::dynamodb_model::k_model, &ddb::dynamodbstreams_model::k_model}) {
        for (const auto& op : m->operations) {
            BOOST_TEST_INFO(op.name);
            BOOST_REQUIRE(op.input != aj::no_shape);
            BOOST_TEST((m->shapes[op.input].kind == aj::shape_kind::structure));
            if (op.output != aj::no_shape) {
                BOOST_TEST((m->shapes[op.output].kind == aj::shape_kind::structure));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(a_valid_request_and_aws_style_violations) {
    aj::codec_options o;
    o.style = aj::error_style::coral(ddb::k_dynamodb_error_namespace);
    const aj::codec c(ddb::dynamodb_model::k_model, o);
    const auto ok =
        c.parse({"POST", "/", "application/x-amz-json-1.0", "DynamoDB_20120810.PutItem",
                 R"({"TableName":"leases","Item":{"leaseKey":{"S":"shardId-0"},)"
                 R"("leaseCounter":{"N":"1"}},"Expected":{"leaseKey":{"Exists":false}}})"});
    BOOST_TEST(ok.has_value());
    const auto bad = c.parse({"POST", "/", "application/x-amz-json-1.0", "DynamoDB_20120810.Query",
                              R"({"TableName":"t","Limit":0,"Select":"SOME"})"});
    BOOST_REQUIRE(!bad.has_value());
    BOOST_TEST(bad.error().type == "com.amazon.coral.validate#ValidationException");
    BOOST_TEST(*bad.error().message ==
               "2 validation errors detected: Value 'SOME' at 'select' failed to satisfy "
               "constraint: Member must satisfy enum value set: [ALL_ATTRIBUTES, "
               "ALL_PROJECTED_ATTRIBUTES, SPECIFIC_ATTRIBUTES, COUNT]; Value '0' at 'limit' "
               "failed to satisfy constraint: Member must have value greater than or equal to 1");
}
