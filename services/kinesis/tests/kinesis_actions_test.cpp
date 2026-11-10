// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The generated `kinesis` action table, operation map and static-key roles
// (`.kiro/specs/kinesis-compatible-streams/` task 4, Requirement 5.2, 5.3),
// held to the pinned model and to what the static-key authorizer allows.

#define BOOST_TEST_MODULE kinesis_actions_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_actions.hpp>
#include <kythira/kinesis/kinesis_operations.hpp>

#include <raft/static_key_authorizer.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace az = kythira::aws_auth;

namespace {

auto find_action(std::string_view name) -> const az::action_entry* {
    static const auto table = kin::kinesis_actions();
    for (const auto& a : table.actions) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

}  // namespace

BOOST_AUTO_TEST_CASE(every_model_operation_has_its_same_named_action) {
    // The front end authorizes an operation as its same-named action, so a
    // model operation without one would go unauthorized.
    for (const auto& op : kin::operations) {
        BOOST_TEST_INFO(op._name);
        BOOST_TEST(find_action(op._name) != nullptr);
        const auto* entry = kin::find_operation_actions(op._name);
        BOOST_REQUIRE(entry != nullptr);
        BOOST_TEST((std::find(entry->actions.begin(), entry->actions.end(), op._name) !=
                    entry->actions.end()));
    }
    BOOST_TEST(kin::find_operation_actions("NoSuchOperation") == nullptr);
}

BOOST_AUTO_TEST_CASE(resources_and_writes_follow_the_reference) {
    BOOST_TEST(kin::kinesis_actions().service_prefix == "kinesis");
    const auto* put = find_action("PutRecord");
    BOOST_REQUIRE(put != nullptr);
    BOOST_TEST((put->resource_templates ==
                std::vector<std::string>{
                    "arn:${Partition}:kinesis:${Region}:${Account}:stream/${StreamName}"}));
    BOOST_TEST((put->condition_keys == std::vector<std::string>{"aws:ResourceTag/${TagKey}"}));
    BOOST_TEST((find_action("ListStreams")->resource_templates == std::vector<std::string>{"*"}));
    BOOST_TEST(find_action("SubscribeToShard")->resource_templates.at(0).find("/consumer/") !=
               std::string::npos);

    BOOST_TEST(kin::find_operation_actions("PutRecords")->writes);
    BOOST_TEST(kin::find_operation_actions("CreateStream")->writes);
    BOOST_TEST(!kin::find_operation_actions("GetRecords")->writes);
    BOOST_TEST(!kin::find_operation_actions("DescribeStreamSummary")->writes);
    BOOST_TEST(!kin::find_operation_actions("SubscribeToShard")->writes);
    // CreateStream may also need AddTagsToStream (with Tags).
    const auto& create = kin::find_operation_actions("CreateStream")->actions;
    BOOST_TEST((std::find(create.begin(), create.end(), "AddTagsToStream") != create.end()));
}

BOOST_AUTO_TEST_CASE(static_roles_split_reads_writes_and_admin) {
    const auto now = std::chrono::system_clock::now();
    az::static_key_authorizer auth{
        std::vector<az::static_key>{
            {"AKIDR", "s", "111122223333", "r", az::static_role::read_only},
            {"AKIDW", "s", "111122223333", "w", az::static_role::read_write},
            {"AKIDA", "s", "111122223333", "a", az::static_role::admin},
        },
        "aws", [now] { return now; }};
    auth.register_service(kin::kinesis_static_roles());

    const auto principal = [](std::string_view name) {
        az::principal_context p;
        p.account = "111122223333";
        p.arn = "arn:aws:iam::111122223333:user/" + std::string(name);
        p.username = std::string(name);
        p.access_key_id = name == "r" ? "AKIDR" : name == "w" ? "AKIDW" : "AKIDA";
        return p;
    };
    const auto allowed = [&](std::string_view who, std::string_view action) {
        az::authz_request r;
        r.action = "kinesis:" + std::string(action);
        r.resource = "arn:aws:kinesis:us-east-1:111122223333:stream/s";
        return auth.authorize(principal(who), r).allowed;
    };

    for (const auto* read : {"GetRecords", "GetShardIterator", "DescribeStreamSummary",
                             "ListShards", "ListStreams", "SubscribeToShard"}) {
        BOOST_TEST_INFO(read);
        BOOST_TEST(allowed("r", read));
        BOOST_TEST(allowed("w", read));
    }
    for (const auto* write : {"PutRecord", "PutRecords"}) {
        BOOST_TEST_INFO(write);
        BOOST_TEST(!allowed("r", write));
        BOOST_TEST(allowed("w", write));
    }
    for (const auto* admin : {"CreateStream", "DeleteStream", "SplitShard", "PutResourcePolicy",
                              "AddTagsToStream", "RegisterStreamConsumer"}) {
        BOOST_TEST_INFO(admin);
        BOOST_TEST(!allowed("w", admin));
        BOOST_TEST(allowed("a", admin));
    }
}
