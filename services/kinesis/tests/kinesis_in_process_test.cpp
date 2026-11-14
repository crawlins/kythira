// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// `kinesis.put_records` (.kiro/specs/service-colocation/ task 5,
// Requirement 7): a co-located module's producer calls reach the front end
// without a network hop, are authenticated and authorized as a network
// request is, and nothing but the producer calls gets through.

#define BOOST_TEST_MODULE kinesis_in_process_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_in_process.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <raft/module_interfaces.hpp>
#include <raft/module_registry.hpp>

#include <boost/json.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace kin = kythira::kinesis;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace sn = kythira::service_node;
namespace json = boost::json;

namespace {

constexpr std::string_view k_account = "111122223333";

// Signed in when the Authorization header is "alice" (a stand-in for a
// SigV4 check); alice may create streams, and write to, describe and list
// the shards of "orders" alone. Records the source address the last authorization saw.
struct alice_only {
    std::string last_source_ip;

    auto authenticate(const sv4::request& r, const sv4::service_rules&) -> az::authn_result {
        for (const auto& [k, v] : r.headers) {
            if (kin::detail::iequals(k, "authorization") && v == "alice") {
                az::principal_context p;
                p.account = std::string(k_account);
                p.arn = "arn:aws:iam::" + p.account + ":user/alice";
                return p;
            }
        }
        return sv4::aws_error{.code = "UnrecognizedClientException",
                              .message = "The security token included in the request is invalid.",
                              .http_status = 400};
    }

    auto authorize(const az::principal_context&, const az::authz_request& r) -> az::authz_decision {
        last_source_ip = r.source_ip;
        az::authz_decision d;
        d.allowed =
            r.action == "kinesis:CreateStream" ||
            (r.resource.ends_with(":stream/orders") &&
             (r.action == "kinesis:PutRecords" || r.action == "kinesis:PutRecord" ||
              r.action == "kinesis:DescribeStreamSummary" || r.action == "kinesis:ListShards"));
        return d;
    }
};

struct fixture {
    std::int64_t clock = 1'762'776'000'000;
    kin::local_stream_cluster cluster{nullptr, [this] { return clock; }};
    alice_only auth;
    kin::front_end<alice_only> fe{kin::front_end_config{}, auth};
    kin::kinesis_service<kin::local_stream_cluster> service{cluster, kin::kinesis_service_config{}};
    std::shared_ptr<kin::in_process_producer<kin::front_end<alice_only>>> producer =
        std::make_shared<kin::in_process_producer<kin::front_end<alice_only>>>(fe);

    fixture() {
        service.register_with(fe);
        create("orders");
    }

    // Straight to the front end, as a client of the listener would.
    void create(std::string_view name) {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202.CreateStream"},
                      {"Authorization", "alice"}};
        r._body = json::serialize(json::object{{"StreamName", name}, {"ShardCount", 2}});
        const auto res = fe.handle(r);
        BOOST_REQUIRE_MESSAGE(res.status == 200U, res.body);
    }

    auto call(std::string_view op, const json::value& body, std::string_view who = "alice")
        -> sn::aws_json_response {
        sn::aws_json_request r;
        r.headers = {{"Content-Type", "application/x-amz-json-1.1"},
                     {"X-Amz-Target", "Kinesis_20131202." + std::string(op)},
                     {"Authorization", std::string(who)}};
        r.body = json::serialize(body);
        return producer->call(sn::caller{.module = "dynamodb"}, r);
    }

    static auto type_of(const sn::aws_json_response& r) -> std::string {
        return std::string(json::parse(r.body).as_object().at("__type").as_string());
    }
};

const std::string k_arn = "arn:aws:kinesis:us-east-1:111122223333:stream/orders";

}  // namespace

BOOST_AUTO_TEST_CASE(put_records_lands_through_the_front_end) {
    fixture f;
    const auto r = f.call(
        "PutRecords",
        {{"StreamARN", k_arn},
         {"Records", json::array{json::object{{"Data", "aGk="}, {"PartitionKey", "a"}},
                                 json::object{{"Data", "dGhlcmU="}, {"PartitionKey", "b"}}}}});
    BOOST_REQUIRE_MESSAGE(r.status == 200, r.body);
    const auto out = json::parse(r.body).as_object();
    BOOST_TEST(out.at("FailedRecordCount").as_int64() == 0);
    BOOST_TEST(out.at("Records").as_array().size() == 2U);
    // The call never left the host.
    BOOST_TEST(f.auth.last_source_ip == "127.0.0.1");

    const auto one =
        f.call("PutRecord", {{"StreamARN", k_arn}, {"PartitionKey", "c"}, {"Data", "aGk="}});
    BOOST_TEST(one.status == 200, one.body);
    const auto summary = f.call("DescribeStreamSummary", {{"StreamARN", k_arn}});
    BOOST_TEST(summary.status == 200, summary.body);
}

BOOST_AUTO_TEST_CASE(the_signature_and_policies_still_decide) {
    fixture f;
    // Not signed in: refused as over the network.
    const auto stranger = f.call(
        "PutRecord", {{"StreamARN", k_arn}, {"PartitionKey", "a"}, {"Data", "aGk="}}, "mallory");
    BOOST_TEST(stranger.status == 400);
    BOOST_TEST(fixture::type_of(stranger) == "UnrecognizedClientException");
    // Signed in, but no policy allows the stream.
    f.create("audit");
    const auto other =
        f.call("PutRecord", {{"StreamARN", "arn:aws:kinesis:us-east-1:111122223333:stream/audit"},
                             {"PartitionKey", "a"},
                             {"Data", "aGk="}});
    BOOST_TEST(other.status == 400);
    BOOST_TEST(fixture::type_of(other) == "AccessDeniedException");
}

BOOST_AUTO_TEST_CASE(only_the_producer_calls_are_taken) {
    fixture f;
    // Allowed by alice's policies, but not producer calls: refused before the
    // front end sees them.
    const auto shards = f.call("ListShards", {{"StreamARN", k_arn}});
    BOOST_TEST(shards.status == 400);
    BOOST_TEST(fixture::type_of(shards) == "UnknownOperationException");
    const auto create = f.call("CreateStream", {{"StreamName", "orders2"}, {"ShardCount", 1}});
    BOOST_TEST(create.status == 400);
    BOOST_TEST(fixture::type_of(create) == "UnknownOperationException");

    sn::aws_json_request bare;
    bare.body = "{}";
    BOOST_TEST(f.producer->call({}, bare).status == 400);
    bare.headers = {{"X-Amz-Target", "Kinesis_20131202PutRecords"}};
    BOOST_TEST(f.producer->call({}, bare).status == 400);
    bare.headers = {{"X-Amz-Target", "DynamoDB_20120810.PutRecords"}};
    BOOST_TEST(f.producer->call({}, bare).status == 400);
}

BOOST_AUTO_TEST_CASE(closed_answers_unavailable) {
    fixture f;
    f.producer->close();
    const auto r =
        f.call("PutRecord", {{"StreamARN", k_arn}, {"PartitionKey", "a"}, {"Data", "aGk="}});
    BOOST_TEST(r.status == 503);
    BOOST_TEST(fixture::type_of(r) == "ServiceUnavailableException");
}

BOOST_AUTO_TEST_CASE(published_and_found_by_name_and_type) {
    fixture f;
    sn::module_registry registry;
    registry.publish<sn::aws_json_endpoint>(std::string(sn::interface_names::kinesis_put_records),
                                            f.producer);
    const auto found =
        registry.lookup<sn::aws_json_endpoint>(sn::interface_names::kinesis_put_records);
    BOOST_REQUIRE(found != nullptr);
    const auto r = f.call("DescribeStreamSummary", {{"StreamARN", k_arn}});
    BOOST_TEST(r.status == 200, r.body);
    // Not co-located: a miss, and the consumer keeps its network client.
    BOOST_TEST(registry.lookup<sn::aws_json_endpoint>("kinesis.nothing") == nullptr);
}
