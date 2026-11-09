// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// The recorded wire evidence against the pinned models
// (`.kiro/specs/dynamodb-compatible-api/` task 1, Requirements 2.3 and 4.1):
// every DynamoDB and DynamoDB Streams request the AWS CLI, boto3,
// aws-sdk-java-v2, the lock client, KCL 2.x and 3.x and the Streams adapter
// sent to the recording stub (evidence/*.jsonl) must frame and validate in
// the core AWS JSON codec, with unknown members refused. A client sending a
// member the model lacks, a header form the codec rejects, or a value
// outside a constraint fails here, long before the front end exists.
//
// KYTHIRA_DYNAMODB_EVIDENCE_DIR, when set, points the test at a fresh
// recording instead (dynamodb-module.yml's evidence job re-records under
// Docker and rootless Podman and runs this against the result). Kinesis
// requests (KCL's) are skipped: the Kinesis module checks those.

#define BOOST_TEST_MODULE dynamodb_wire_evidence_test

#include <boost/test/unit_test.hpp>

#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/operations.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace aj = kythira::aws_json;
namespace ddb = kythira::dynamodb;
namespace fs = std::filesystem;

namespace {

auto evidence_dir() -> fs::path {
    if (const char* env = std::getenv("KYTHIRA_DYNAMODB_EVIDENCE_DIR");
        env != nullptr && *env != '\0') {
        return env;
    }
    return fs::path(KYTHIRA_DYNAMODB_SOURCE_DIR) / "evidence";
}

struct record {
    std::string client;
    std::size_t seq = 0;
    std::string service;
    std::string operation;
    std::string content_type;
    std::string body;
};

auto load() -> std::vector<record> {
    std::vector<record> out;
    const auto dir = evidence_dir();
    BOOST_REQUIRE_MESSAGE(fs::is_directory(dir), "no evidence directory " << dir);
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension() != ".jsonl") {
            continue;
        }
        std::ifstream in(entry.path());
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) {
                continue;
            }
            const auto v = boost::json::parse(line).as_object();
            record r;
            r.client = entry.path().stem().string();
            r.seq = static_cast<std::size_t>(v.at("seq").to_number<std::int64_t>());
            r.service = std::string(v.at("service").as_string());
            r.operation = std::string(v.at("operation").as_string());
            const auto& headers = v.at("headers").as_object();
            if (const auto* ct = headers.if_contains("content-type"); ct != nullptr) {
                r.content_type = std::string(ct->as_string());
            }
            r.body = boost::json::serialize(v.at("body"));
            out.push_back(std::move(r));
        }
    }
    return out;
}

}  // namespace

BOOST_AUTO_TEST_CASE(every_recorded_request_validates_against_the_pinned_model) {
    aj::codec_options o;
    o.style = aj::error_style::coral(ddb::k_dynamodb_error_namespace);
    const aj::codec dynamo(ddb::dynamodb_model::k_model, o);
    const aj::codec streams(ddb::dynamodbstreams_model::k_model, o);

    const auto records = load();
    std::map<std::string, std::size_t> checked;
    for (const auto& r : records) {
        const aj::codec* c = nullptr;
        if (r.service == ddb::k_dynamodb_target_prefix) {
            c = &dynamo;
        } else if (r.service == ddb::k_streams_target_prefix) {
            c = &streams;
        } else {
            continue;  // Kinesis (KCL's stream reads)
        }
        const std::string target = r.service + "." + r.operation;
        const auto parsed = c->parse({"POST", "/", r.content_type, target, r.body});
        BOOST_TEST_INFO(r.client << " #" << r.seq << " " << target << " " << r.body);
        BOOST_TEST(parsed.has_value(),
                   (parsed.has_value()
                        ? std::string()
                        : parsed.error().type + ": " + parsed.error().message.value_or("")));
        ++checked[r.client];
    }
    // Every client the spec names was recorded and sent DynamoDB requests.
    for (const auto* client :
         {"aws-cli", "boto3", "java-sdk", "lock-client", "kcl2", "kcl3", "streams-adapter"}) {
        BOOST_TEST_INFO("client " << client);
        BOOST_TEST(checked[client] > 0U);
    }
}

BOOST_AUTO_TEST_CASE(the_recorded_clients_use_only_modeled_operations) {
    std::set<std::string> ops;
    for (const auto& r : load()) {
        if (r.service == ddb::k_dynamodb_target_prefix) {
            BOOST_TEST_INFO(r.client << " " << r.operation);
            BOOST_TEST(ddb::dynamodb_model::k_model.find_operation(r.operation) != nullptr);
        } else if (r.service == ddb::k_streams_target_prefix) {
            BOOST_TEST_INFO(r.client << " " << r.operation);
            BOOST_TEST(ddb::dynamodbstreams_model::k_model.find_operation(r.operation) != nullptr);
        }
    }
}

namespace {

// Every attribute-value map under `v`: the members whose values are items,
// keys or placeholder values. RequestItems and TransactItems nest them.
void collect_items(const boost::json::value& v, std::vector<const boost::json::object*>& out) {
    static const std::set<std::string_view> k_item_members = {
        "Item", "Key", "ExpressionAttributeValues", "ExclusiveStartKey"};
    if (v.is_object()) {
        for (const auto& kv : v.get_object()) {
            if (k_item_members.contains(kv.key()) && kv.value().is_object()) {
                out.push_back(&kv.value().get_object());
            } else {
                collect_items(kv.value(), out);
            }
        }
    } else if (v.is_array()) {
        for (const auto& e : v.get_array()) {
            collect_items(e, out);
        }
    }
}

}  // namespace

// The values the clients sent are ones the item codec accepts (task 3): the
// numbers parse, sets are well formed, and each item survives the storage
// form.
BOOST_AUTO_TEST_CASE(every_recorded_item_and_key_passes_the_item_codec) {
    std::size_t checked = 0;
    for (const auto& r : load()) {
        if (r.service != ddb::k_dynamodb_target_prefix) {
            continue;
        }
        std::vector<const boost::json::object*> items;
        const auto body = boost::json::parse(r.body);
        collect_items(body, items);
        for (const auto* o : items) {
            const auto it = ddb::item_from_json(*o);
            BOOST_TEST_INFO(r.client << " #" << r.seq << " " << r.operation << " "
                                     << boost::json::serialize(*o));
            BOOST_TEST(it.has_value(), (it ? std::string() : it.error().message.value_or("")));
            if (it) {
                BOOST_TEST((ddb::decode_item(ddb::encode_item(*it)) == *it));
                ++checked;
            }
        }
    }
    BOOST_TEST(checked > 100U);
}
