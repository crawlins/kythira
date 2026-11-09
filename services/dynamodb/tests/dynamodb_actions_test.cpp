// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The generated `dynamodb` action table and static-key roles
// (`.kiro/specs/dynamodb-compatible-api/` task 4, Requirement 5.2, 5.3):
// the facts the front end and the IAM registry depend on.

#define BOOST_TEST_MODULE dynamodb_actions_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/dynamodb_actions.hpp>
#include <kythira/dynamodb/operations.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <string_view>

namespace ddb = kythira::dynamodb;

namespace {

auto actions() -> std::map<std::string, kythira::aws_auth::action_entry> {
    std::map<std::string, kythira::aws_auth::action_entry> out;
    for (auto& a : ddb::dynamodb_actions().actions) {
        out.emplace(a.name, a);
    }
    return out;
}

auto has(const std::vector<std::string>& v, std::string_view s) -> bool {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

BOOST_AUTO_TEST_CASE(item_actions_carry_their_condition_keys) {
    const auto a = actions();
    BOOST_TEST(ddb::dynamodb_actions().service_prefix == "dynamodb");
    BOOST_TEST(has(a.at("GetItem").condition_keys, "dynamodb:LeadingKeys"));
    BOOST_TEST(has(a.at("GetItem").condition_keys, "dynamodb:Attributes"));
    BOOST_TEST(has(a.at("Scan").condition_keys, "dynamodb:Select"));
    BOOST_TEST(has(a.at("CreateTable").condition_keys, "aws:RequestTag/${TagKey}"));
    BOOST_TEST(has(a.at("Query").resource_templates,
                   "arn:${Partition}:dynamodb:${Region}:${Account}:table/${TableName}/index/"
                   "${IndexName}"));
    BOOST_TEST(has(a.at("GetRecords").resource_templates,
                   "arn:${Partition}:dynamodb:${Region}:${Account}:table/${TableName}/stream/"
                   "${StreamLabel}"));
}

BOOST_AUTO_TEST_CASE(every_implemented_operation_has_an_action_or_a_mapping) {
    const auto a = actions();
    for (const auto op : ddb::k_implemented_dynamodb_operations) {
        BOOST_TEST_INFO(op);
        BOOST_TEST(a.contains(std::string(op)));
    }
    // Operations without a same-named action map to element actions.
    const auto* tw =
        std::find_if(ddb::k_operation_actions.begin(), ddb::k_operation_actions.end(),
                     [](const auto& o) { return o.operation == "TransactWriteItems"; });
    BOOST_REQUIRE(tw != ddb::k_operation_actions.end());
    BOOST_TEST(std::count(tw->actions.begin(), tw->actions.end(), "PutItem") == 1);
    BOOST_TEST(
        std::is_sorted(ddb::k_operation_actions.begin(), ddb::k_operation_actions.end(),
                       [](const auto& x, const auto& y) { return x.operation < y.operation; }));
}

BOOST_AUTO_TEST_CASE(static_roles_split_reads_from_writes) {
    const auto roles = ddb::dynamodb_static_roles();
    BOOST_TEST(roles.service_prefix == "dynamodb");
    for (const auto* read : {"GetItem", "Query", "Scan", "DescribeTable", "ListTables",
                             "GetRecords", "DescribeEndpoints"}) {
        BOOST_TEST_INFO(read);
        BOOST_TEST(has(roles.read_only, read));
    }
    for (const auto* write : {"PutItem", "UpdateItem", "DeleteItem", "BatchWriteItem"}) {
        BOOST_TEST_INFO(write);
        BOOST_TEST(!has(roles.read_only, write));
        BOOST_TEST(has(roles.read_write, write));
    }
    for (const auto* admin : {"CreateTable", "DeleteTable", "PutResourcePolicy", "TagResource"}) {
        BOOST_TEST_INFO(admin);
        BOOST_TEST(!has(roles.read_only, admin));
        BOOST_TEST(!has(roles.read_write, admin));
    }
}
