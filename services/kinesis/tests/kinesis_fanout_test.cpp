// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Enhanced fan-out (`.kiro/specs/kinesis-compatible-streams/` task 13,
// Requirement 13): the consumer calls through the front end on the
// in-process cluster, and SubscribeToShard's event stream, pulled directly
// and over HTTP/1.1 through the gateway's streaming route. Covers
// registration limits and paging, consumer ARNs for tags and policies,
// DeleteStream's EnforceConsumerDeletion, one subscription per consumer and
// shard, records and continuation numbers in events, the stream ending on a
// split with child shards, and an exception event once the consumer goes.

#define BOOST_TEST_MODULE kinesis_fanout_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_gateway.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/aws_eventstream.hpp>
#include <raft/beast_https_server.hpp>
#include <raft/record_segment_store.hpp>

#if defined(KYTHIRA_HAS_NGHTTP2)
#include <h2_test_client.hpp>
#endif

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace es = kythira::aws_eventstream;
namespace https = kythira::https;
namespace json = boost::json;
namespace http = boost::beast::http;

namespace {

constexpr std::string_view k_account = "111122223333";

struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path =
            std::filesystem::temp_directory_path() /
            ("kinesis_fanout_test." + std::to_string(::getpid()) + "." + std::to_string(++counter));
        std::filesystem::remove_all(_path);
        std::filesystem::create_directories(_path);
    }
    ~temp_dir() {
        std::error_code ec;
        std::filesystem::remove_all(_path, ec);
    }
    temp_dir(const temp_dir&) = delete;
    auto operator=(const temp_dir&) -> temp_dir& = delete;
};

struct allow_all {
    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request&) -> az::authz_decision {
        az::authz_decision d;
        d.allowed = true;
        return d;
    }
};

// Quick enough for tests: checks the catalog every 50 ms, polls every
// 10 ms, and sends an empty event after 200 ms of nothing.
auto fast_config() -> kin::kinesis_service_config {
    kin::kinesis_service_config c;
    c._subscription_heartbeat = std::chrono::milliseconds(200);
    c._subscription_poll = std::chrono::milliseconds(10);
    c._subscription_recheck = std::chrono::milliseconds(50);
    return c;
}

using front_end_type = kin::front_end<allow_all>;

struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store =
        std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
            ._data_dir = dir._path, ._barriers_enabled = false});
    kin::local_stream_cluster cluster{store.get()};
    allow_all auth;
    front_end_type fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    explicit fixture(kin::kinesis_service_config config = fast_config()) {
        service =
            std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(cluster, config);
        service->register_with(fe);
        fe.set_resource_lookup(
            [this](std::string_view arn) { return service->resource_facts_of(arn); });
    }

    static auto request(std::string_view op, const json::value& body) -> kin::front_end_request {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._body = json::serialize(body);
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
        return r;
    }

    auto call(std::string_view op, const json::value& body) -> aj::http_response {
        return fe.handle(request(op, body));
    }

    auto ok(std::string_view op, const json::value& body) -> json::object {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, std::string(op) + ": " + r.body);
        return json::parse(r.body).as_object();
    }

    void create(std::string_view name, int shards) {
        (void)ok("CreateStream", {{"StreamName", name}, {"ShardCount", shards}});
    }

    auto arn(std::string_view stream) -> std::string {
        return std::string(ok("DescribeStreamSummary", {{"StreamName", stream}})
                               .at("StreamDescriptionSummary")
                               .as_object()
                               .at("StreamARN")
                               .as_string());
    }

    auto register_consumer(std::string_view stream, std::string_view name) -> std::string {
        const auto r =
            ok("RegisterStreamConsumer", {{"StreamARN", arn(stream)}, {"ConsumerName", name}});
        return std::string(r.at("Consumer").as_object().at("ConsumerARN").as_string());
    }

    // Puts `data` (raw text) under partition key "k"; its sequence number.
    auto put(std::string_view stream, std::string_view data) -> std::string {
        const auto r = ok("PutRecord", {{"StreamName", stream},
                                        {"PartitionKey", "k"},
                                        {"Data", kin::service_detail::base64_encode(data)}});
        return std::string(r.at("SequenceNumber").as_string());
    }

    auto subscribe(const std::string& consumer, std::string_view shard,
                   const json::object& position) -> front_end_type::reply {
        return fe.handle_streaming(request(
            "SubscribeToShard",
            {{"ConsumerARN", consumer}, {"ShardId", shard}, {"StartingPosition", position}}));
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

struct pulled_event {
    std::string _message_type;  // "event" or "exception"
    std::string _type;          // the :event-type or :exception-type
    json::object _payload;
};

auto parse_event(const std::string& bytes) -> pulled_event {
    const auto m = es::decode({reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
    pulled_event e;
    e._message_type = std::string(m.string_header(":message-type").value_or(""));
    e._type = std::string(
        m.string_header(e._message_type == "exception" ? ":exception-type" : ":event-type")
            .value_or(""));
    if (!m.payload.empty()) {
        const std::string body(reinterpret_cast<const char*>(m.payload.data()), m.payload.size());
        e._payload = json::parse(body).as_object();
    }
    return e;
}

// The next event within `limit`, or nullopt once the stream has ended.
auto next_event(kin::event_source& events,
                std::chrono::milliseconds limit = std::chrono::milliseconds(5000))
    -> std::optional<pulled_event> {
    const auto until = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < until) {
        auto p = events.next(std::chrono::milliseconds(100));
        if (!p._message.empty()) {
            return parse_event(p._message);
        }
        if (p._done) {
            return std::nullopt;
        }
    }
    BOOST_FAIL("no event in time");
    return std::nullopt;
}

// Pulls events until one carries records; their data, decoded.
auto next_records(kin::event_source& events, pulled_event* last = nullptr)
    -> std::vector<std::string> {
    for (int i = 0; i < 50; ++i) {
        auto e = next_event(events);
        BOOST_REQUIRE(e.has_value());
        BOOST_REQUIRE_MESSAGE(e->_type == "SubscribeToShardEvent", e->_type);
        const auto& records = e->_payload.at("Records").as_array();
        if (records.empty()) {
            continue;
        }
        std::vector<std::string> out;
        for (const auto& r : records) {
            out.push_back(
                *kin::service_detail::base64_decode(r.as_object().at("Data").as_string()));
        }
        if (last != nullptr) {
            *last = std::move(*e);
        }
        return out;
    }
    BOOST_FAIL("no records");
    return {};
}

const json::object k_latest{{"Type", "LATEST"}};
const json::object k_trim_horizon{{"Type", "TRIM_HORIZON"}};

}  // namespace

BOOST_AUTO_TEST_CASE(consumers_register_describe_list_and_deregister) {
    fixture f;
    f.create("s", 1);
    const auto stream_arn = f.arn("s");
    const auto a = f.register_consumer("s", "alpha");
    BOOST_TEST(a == stream_arn + "/consumer/alpha:" + a.substr(a.rfind(':') + 1));

    // By ARN, or by stream and name.
    auto d = f.ok("DescribeStreamConsumer", {{"ConsumerARN", a}});
    auto desc = d.at("ConsumerDescription").as_object();
    BOOST_TEST(desc.at("ConsumerName").as_string() == "alpha");
    BOOST_TEST(desc.at("ConsumerStatus").as_string() == "ACTIVE");
    BOOST_TEST(desc.at("StreamARN").as_string() == stream_arn);
    d = f.ok("DescribeStreamConsumer", {{"StreamARN", stream_arn}, {"ConsumerName", "alpha"}});
    BOOST_TEST(d.at("ConsumerDescription").as_object().at("ConsumerARN").as_string() == a);

    // A second registration of the name is refused.
    const auto again =
        f.call("RegisterStreamConsumer", {{"StreamARN", stream_arn}, {"ConsumerName", "alpha"}});
    BOOST_TEST(type_of(again) == "ResourceInUseException");

    // Twenty at most.
    for (int i = 1; i < 20; ++i) {
        (void)f.register_consumer("s", "c" + std::to_string(100 + i));
    }
    const auto over =
        f.call("RegisterStreamConsumer", {{"StreamARN", stream_arn}, {"ConsumerName", "late"}});
    BOOST_TEST(type_of(over) == "LimitExceededException");

    // Listed by name, a page at a time.
    std::vector<std::string> names;
    json::object in{{"StreamARN", stream_arn}, {"MaxResults", 7}};
    for (int pages = 0; pages < 10; ++pages) {
        const auto page = f.ok("ListStreamConsumers", in);
        for (const auto& c : page.at("Consumers").as_array()) {
            names.emplace_back(c.as_object().at("ConsumerName").as_string());
        }
        if (!page.contains("NextToken")) {
            break;
        }
        in["NextToken"] = page.at("NextToken");
    }
    BOOST_REQUIRE(names.size() == 20U);
    BOOST_TEST(names.front() == "alpha");
    BOOST_TEST(std::ranges::is_sorted(names));
    const auto bad = f.call("ListStreamConsumers",
                            {{"StreamARN", stream_arn}, {"NextToken", "not-a-next-token"}});
    BOOST_TEST(type_of(bad) == "ExpiredNextTokenException");

    // Gone after deregistering, by ARN or by name.
    (void)f.ok("DeregisterStreamConsumer", {{"ConsumerARN", a}});
    (void)f.ok("DeregisterStreamConsumer", {{"StreamARN", stream_arn}, {"ConsumerName", "c101"}});
    const auto missing = f.call("DescribeStreamConsumer", {{"ConsumerARN", a}});
    BOOST_TEST(type_of(missing) == "ResourceNotFoundException");
    const auto listed = f.ok("ListStreamConsumers", {{"StreamARN", stream_arn}});
    BOOST_TEST(listed.at("Consumers").as_array().size() == 18U);

    // A consumer of the same name later is a new one, with a new ARN.
    (void)f.register_consumer("s", "late");
    BOOST_TEST(type_of(f.call("DescribeStreamConsumer", {{"ConsumerARN", a}})) ==
               "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(relaxed_count_limits_allow_more_consumers) {
    fixture f;
    f.create("r", 1);
    (void)f.ok("AddTagsToStream",
               {{"StreamName", "r"}, {"Tags", {{"kythira:compat:count-limits", "relaxed"}}}});
    for (int i = 0; i < 25; ++i) {
        (void)f.register_consumer("r", "c" + std::to_string(i));
    }
    BOOST_TEST(f.ok("ListStreamConsumers", {{"StreamARN", f.arn("r")}})
                   .at("Consumers")
                   .as_array()
                   .size() == 25U);
}

BOOST_AUTO_TEST_CASE(consumer_arns_take_tags_and_policies_and_block_delete) {
    fixture f;
    f.create("s", 1);
    const auto a = f.register_consumer("s", "alpha");

    (void)f.ok("TagResource", {{"ResourceARN", a}, {"Tags", {{"team", "red"}, {"x", "1"}}}});
    (void)f.ok("UntagResource", {{"ResourceARN", a}, {"TagKeys", {"x"}}});
    const auto tags = f.ok("ListTagsForResource", {{"ResourceARN", a}});
    BOOST_REQUIRE(tags.at("Tags").as_array().size() == 1U);
    BOOST_TEST(tags.at("Tags").as_array()[0].as_object().at("Key").as_string() == "team");
    // The stream's own tags are separate.
    BOOST_TEST(f.ok("ListTagsForStream", {{"StreamName", "s"}}).at("Tags").as_array().empty());

    const std::string policy =
        R"({"Version":"2012-10-17","Statement":[{"Effect":"Allow","Principal":{"AWS":"arn:aws:iam::111122223333:root"},"Action":"kinesis:SubscribeToShard","Resource":")" +
        a + R"("}]})";
    (void)f.ok("PutResourcePolicy", {{"ResourceARN", a}, {"Policy", policy}});
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", a}}).at("Policy").as_string() == policy);
    const auto facts = f.service->resource_facts_of(a);
    BOOST_REQUIRE(facts.has_value());
    BOOST_REQUIRE(facts->_tags.size() == 1U);
    BOOST_TEST(facts->_tags[0].second == "red");
    BOOST_TEST(facts->_policy.has_value());
    (void)f.ok("DeleteResourcePolicy", {{"ResourceARN", a}});
    BOOST_TEST(f.ok("GetResourcePolicy", {{"ResourceARN", a}}).at("Policy").as_string() == "{}");

    // A consumer keeps its stream unless deletion is enforced.
    const auto refused = f.call("DeleteStream", {{"StreamName", "s"}});
    BOOST_TEST(type_of(refused) == "ResourceInUseException");
    (void)f.ok("DeleteStream", {{"StreamName", "s"}, {"EnforceConsumerDeletion", true}});
    BOOST_TEST(type_of(f.call("DescribeStreamConsumer", {{"ConsumerARN", a}})) ==
               "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(subscribe_streams_records_with_continuation_numbers) {
    fixture f;
    f.create("s", 1);
    const auto before = f.put("s", "old");
    const auto a = f.register_consumer("s", "alpha");

    auto r = f.subscribe(a, "shardId-000000000000", k_trim_horizon);
    BOOST_REQUIRE_MESSAGE(r._response.status == 200U, r._response.body);
    BOOST_REQUIRE(r._events != nullptr);
    BOOST_TEST(std::ranges::any_of(r._response.headers,
                                   [](const auto& h) { return h.first == "Content-Type"; }));
    auto& events = *r._events;

    const auto initial = next_event(events);
    BOOST_REQUIRE(initial.has_value());
    BOOST_TEST(initial->_type == "initial-response");

    pulled_event last;
    BOOST_TEST((next_records(events, &last) == std::vector<std::string>{"old"}));
    BOOST_TEST(last._payload.at("ContinuationSequenceNumber").as_string() == before);
    BOOST_TEST(last._payload.at("MillisBehindLatest").is_number());

    // New records arrive on the open subscription.
    (void)f.put("s", "one");
    const auto two = f.put("s", "two");
    std::vector<std::string> got;
    while (got.size() < 2) {
        for (auto& d : next_records(events, &last)) {
            got.push_back(std::move(d));
        }
    }
    BOOST_TEST((got == std::vector<std::string>{"one", "two"}));
    BOOST_TEST(last._payload.at("ContinuationSequenceNumber").as_string() == two);

    // Quiet stretches still get (empty) events.
    const auto heartbeat = next_event(events);
    BOOST_REQUIRE(heartbeat.has_value());
    BOOST_TEST(heartbeat->_payload.at("Records").as_array().empty());

    // One subscription per consumer and shard; a second consumer is fine.
    const auto busy = f.subscribe(a, "shardId-000000000000", k_latest);
    BOOST_TEST(busy._events == nullptr);
    BOOST_TEST(type_of(busy._response) == "ResourceInUseException");
    const auto b = f.register_consumer("s", "beta");
    auto other = f.subscribe(b, "shardId-000000000000",
                             {{"Type", "AFTER_SEQUENCE_NUMBER"}, {"SequenceNumber", before}});
    BOOST_REQUIRE(other._events != nullptr);
    BOOST_TEST(next_event(*other._events)->_type == "initial-response");
    BOOST_TEST((next_records(*other._events) == std::vector<std::string>{"one", "two"}));

    // Once the subscription is dropped, the slot is free again.
    BOOST_TEST(f.service->metrics_text().find("fanout") != std::string::npos);
    r._events.reset();
    auto again = f.subscribe(a, "shardId-000000000000", k_latest);
    BOOST_TEST(again._events != nullptr);
}

BOOST_AUTO_TEST_CASE(subscribe_refusals) {
    fixture f;
    f.create("s", 1);
    const auto a = f.register_consumer("s", "alpha");
    const auto missing_shard = f.subscribe(a, "shardId-000000000009", k_latest);
    BOOST_TEST(type_of(missing_shard._response) == "ResourceNotFoundException");
    const auto missing_consumer =
        f.subscribe(a.substr(0, a.rfind(':')) + ":1", "shardId-000000000000", k_latest);
    BOOST_TEST(type_of(missing_consumer._response) == "ResourceNotFoundException");
    const auto bad_position =
        f.subscribe(a, "shardId-000000000000", {{"Type", "AT_SEQUENCE_NUMBER"}});
    BOOST_TEST(type_of(bad_position._response) == "InvalidArgumentException");
    // A front end that cannot stream drops the events but answers.
    const auto plain = f.call(
        "SubscribeToShard",
        {{"ConsumerARN", a}, {"ShardId", "shardId-000000000000"}, {"StartingPosition", k_latest}});
    BOOST_TEST(plain.status == 200U);
    auto after = f.subscribe(a, "shardId-000000000000", k_latest);
    BOOST_TEST(after._events != nullptr);
}

BOOST_AUTO_TEST_CASE(subscription_ends_on_split_and_on_deregister) {
    fixture f;
    f.create("s", 1);
    const auto a = f.register_consumer("s", "alpha");
    auto r = f.subscribe(a, "shardId-000000000000", k_latest);
    BOOST_REQUIRE(r._events != nullptr);
    BOOST_TEST(next_event(*r._events)->_type == "initial-response");
    (void)f.put("s", "before split");
    BOOST_TEST((next_records(*r._events) == std::vector<std::string>{"before split"}));

    (void)f.ok("SplitShard", {{"StreamName", "s"},
                              {"ShardToSplit", "shardId-000000000000"},
                              {"NewStartingHashKey", kin::to_decimal(kin::hash_key{1} << 127U)}});
    // The closed shard's last event names its children, then the stream ends.
    std::optional<pulled_event> e;
    for (int i = 0; i < 100; ++i) {
        e = next_event(*r._events);
        BOOST_REQUIRE(e.has_value());
        if (e->_payload.contains("ChildShards")) {
            break;
        }
    }
    BOOST_REQUIRE(e->_payload.contains("ChildShards"));
    BOOST_TEST(e->_payload.at("ContinuationSequenceNumber").is_null());
    std::set<std::string> children;
    for (const auto& c : e->_payload.at("ChildShards").as_array()) {
        children.emplace(c.as_object().at("ShardId").as_string());
    }
    BOOST_TEST((children == std::set<std::string>{"shardId-000000000001", "shardId-000000000002"}));
    BOOST_TEST(!next_event(*r._events).has_value());

    // A child's subscription ends with an exception once the consumer goes.
    auto child = f.subscribe(a, "shardId-000000000001", k_trim_horizon);
    BOOST_REQUIRE(child._events != nullptr);
    BOOST_TEST(next_event(*child._events)->_type == "initial-response");
    (void)f.ok("DeregisterStreamConsumer", {{"ConsumerARN", a}});
    std::optional<pulled_event> last;
    for (int i = 0; i < 100; ++i) {
        last = next_event(*child._events);
        BOOST_REQUIRE(last.has_value());
        if (last->_message_type == "exception") {
            break;
        }
    }
    BOOST_TEST(last->_message_type == "exception");
    BOOST_TEST(last->_type == "ResourceNotFoundException");
    BOOST_TEST(!next_event(*child._events).has_value());
}

BOOST_AUTO_TEST_CASE(subscription_ends_at_its_lifetime) {
    auto config = fast_config();
    config._subscription_lifetime = std::chrono::milliseconds(300);
    fixture f(config);
    f.create("s", 1);
    const auto a = f.register_consumer("s", "alpha");
    auto r = f.subscribe(a, "shardId-000000000000", k_latest);
    BOOST_REQUIRE(r._events != nullptr);
    const auto started = std::chrono::steady_clock::now();
    while (next_event(*r._events).has_value()) {
    }
    BOOST_TEST((std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(250)));
}

BOOST_AUTO_TEST_CASE(gateway_streams_events_over_chunked_http) {
    auto config = fast_config();
    config._subscription_lifetime = std::chrono::milliseconds(1500);
    fixture f(config);
    f.create("s", 1);
    const auto a = f.register_consumer("s", "alpha");
    (void)f.put("s", "over the wire");

    kin::gateway<front_end_type> gw(
        f.fe,
        [&f](const std::string& key, const std::vector<std::byte>& cmd, kin::admission mode) {
            return f.cluster.propose(key, cmd, mode);
        },
        f.store.get(), "a-forwarding-secret-of-some-length");
    https::server_config sc;
    sc.bind_address = "127.0.0.1";
    sc.insecure_plaintext = true;
    https::server server(sc, [&gw](const https::request& req, const https::request_context& ctx) {
        return gw.handle(req, ctx);
    });
    server.set_stream_handler([&gw](const https::request& req, const https::request_context& ctx) {
        return gw.stream(req, ctx);
    });
    server.start();

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::resolver resolver(ioc);
    boost::beast::tcp_stream sock(ioc);
    sock.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));
    auto send = [&](const json::value& body) {
        http::request<http::string_body> req{http::verb::post, "/", 11};
        req.set(http::field::host, "127.0.0.1");
        req.set(http::field::content_type, "application/x-amz-json-1.1");
        req.set("X-Amz-Target", "Kinesis_20131202.SubscribeToShard");
        req.body() = json::serialize(body);
        req.prepare_payload();
        http::write(sock, req);
    };
    send({{"ConsumerARN", a},
          {"ShardId", "shardId-000000000000"},
          {"StartingPosition", k_trim_horizon}});

    boost::beast::flat_buffer buf;
    http::response<http::string_body> res;
    sock.expires_after(std::chrono::seconds(10));
    http::read(sock, buf, res);  // the whole chunked body, until the lifetime ends it
    BOOST_TEST(res.result_int() == 200U);
    BOOST_TEST(res[http::field::content_type] == "application/vnd.amazon.eventstream");
    BOOST_TEST(res.chunked());

    es::decoder dec;
    dec.feed({reinterpret_cast<const std::byte*>(res.body().data()), res.body().size()});
    std::vector<std::string> types;
    std::vector<std::string> data;
    while (auto m = dec.next()) {
        types.emplace_back(m->string_header(":event-type").value_or(""));
        const std::string body(reinterpret_cast<const char*>(m->payload.data()), m->payload.size());
        const auto o = json::parse(body).as_object();
        if (o.contains("Records")) {
            for (const auto& r : o.at("Records").as_array()) {
                data.push_back(
                    *kin::service_detail::base64_decode(r.as_object().at("Data").as_string()));
            }
        }
    }
    BOOST_REQUIRE(types.size() >= 2U);
    BOOST_TEST(types.front() == "initial-response");
    BOOST_TEST((data == std::vector<std::string>{"over the wire"}));

    // A refusal comes back whole, as an ordinary JSON error.
    boost::beast::tcp_stream sock2(ioc);
    sock2.connect(resolver.resolve("127.0.0.1", std::to_string(server.port())));
    http::request<http::string_body> req{http::verb::post, "/", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/x-amz-json-1.1");
    req.set("X-Amz-Target", "Kinesis_20131202.SubscribeToShard");
    req.body() = json::serialize(json::object{
        {"ConsumerARN", a}, {"ShardId", "shardId-000000000042"}, {"StartingPosition", k_latest}});
    req.prepare_payload();
    http::write(sock2, req);
    http::response<http::string_body> refused;
    boost::beast::flat_buffer buf2;
    http::read(sock2, buf2, refused);
    BOOST_TEST(refused.result_int() == 400U);
    BOOST_TEST(json::parse(refused.body()).as_object().at("__type").as_string() ==
               "ResourceNotFoundException");
    server.stop();
}

#if defined(KYTHIRA_HAS_NGHTTP2)
BOOST_AUTO_TEST_CASE(gateway_streams_events_over_http2) {
    fixture f;
    f.create("s", 2);
    const auto a = f.register_consumer("s", "alpha");
    // Both records share a partition key, so one shard holds both.
    (void)f.put("s", "h2 record");
    const auto shard =
        std::string(f.ok("PutRecord", {{"StreamName", "s"},
                                       {"PartitionKey", "k"},
                                       {"Data", kin::service_detail::base64_encode("second")}})
                        .at("ShardId")
                        .as_string());
    const auto* other =
        shard == "shardId-000000000000" ? "shardId-000000000001" : "shardId-000000000000";

    kin::gateway<front_end_type> gw(
        f.fe,
        [&f](const std::string& key, const std::vector<std::byte>& cmd, kin::admission mode) {
            return f.cluster.propose(key, cmd, mode);
        },
        f.store.get(), "a-forwarding-secret-of-some-length");
    https::server_config sc;
    sc.bind_address = "127.0.0.1";
    sc.insecure_plaintext = true;
    sc.http2 = true;
    https::server server(sc, [&gw](const https::request& req, const https::request_context& ctx) {
        return gw.handle(req, ctx);
    });
    server.set_stream_handler([&gw](const https::request& req, const https::request_context& ctx) {
        return gw.stream(req, ctx);
    });
    server.start();

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
    kythira::testing::set_receive_timeout(sock.native_handle(), std::chrono::seconds(10));
    kythira::testing::h2_client client(sock);
    const std::vector<std::pair<std::string, std::string>> headers{
        {"content-type", "application/x-amz-json-1.1"},
        {"x-amz-target", "Kinesis_20131202.SubscribeToShard"}};
    auto body = [&](std::string_view shard_id) {
        return json::serialize(json::object{
            {"ConsumerARN", a}, {"ShardId", shard_id}, {"StartingPosition", k_trim_horizon}});
    };
    // Both shards on one connection, as an SDK multiplexes them.
    const auto s1 = client.submit("POST", "/", headers, body(shard));
    const auto s2 = client.submit("POST", "/", headers, body(other));
    std::map<std::int32_t, es::decoder> decoders;
    std::map<std::int32_t, std::vector<std::string>> data;
    std::map<std::int32_t, std::vector<std::string>> types;
    auto drain = [&](std::int32_t id) {
        while (auto m = decoders[id].next()) {
            types[id].emplace_back(m->string_header(":event-type").value_or(""));
            const std::string text(reinterpret_cast<const char*>(m->payload.data()),
                                   m->payload.size());
            const auto o = json::parse(text).as_object();
            if (o.contains("Records")) {
                for (const auto& r : o.at("Records").as_array()) {
                    data[id].push_back(
                        *kin::service_detail::base64_decode(r.as_object().at("Data").as_string()));
                }
            }
        }
    };
    (void)client.wait(s1, [&](std::int32_t id, std::string_view piece) {
        decoders[id].feed({reinterpret_cast<const std::byte*>(piece.data()), piece.size()});
        drain(id);
        return data[s1].size() < 2 || types[s2].empty();
    });
    BOOST_REQUIRE(!types[s1].empty());
    BOOST_REQUIRE(!types[s2].empty());
    BOOST_TEST(types[s1].front() == "initial-response");
    BOOST_TEST(types[s2].front() == "initial-response");
    BOOST_TEST((data[s1] == std::vector<std::string>{"h2 record", "second"}));
    BOOST_TEST(data[s2].empty());

    // A refusal on the same connection is an ordinary error answer.
    const auto refused =
        client.wait(client.submit("POST", "/", headers, body("shardId-000000000042")));
    BOOST_TEST(refused.status == 400);
    BOOST_TEST(json::parse(refused.body).as_object().at("__type").as_string() ==
               "ResourceNotFoundException");
    // A reset ends the subscription, so the shard is free again.
    client.reset(s1);
    client.reset(s2);
    std::optional<kythira::testing::h2_reply> again;
    for (int i = 0; i < 100; ++i) {
        const auto id = client.submit("POST", "/", headers, body(shard));
        int pieces = 0;
        auto r = client.wait(id, [&](std::int32_t, std::string_view) { return ++pieces < 1; });
        if (r.status == 200) {
            again = r;
            client.reset(id);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    BOOST_TEST(again.has_value());
    server.stop();
}
#endif

BOOST_AUTO_TEST_CASE(consumer_commands_survive_the_codec) {
    const kin::consumer_command c{._account = std::string(k_account),
                                  ._name = "s",
                                  ._id = 7,
                                  ._op = kin::consumer_op::retag,
                                  ._consumer = "alpha",
                                  ._created_ms = 1234,
                                  ._set = {{"a", "1"}},
                                  ._remove = {"b"},
                                  ._policy = std::string("{}"),
                                  ._max = 20};
    const auto bytes = kin::encode_command(c);
    const auto back = kin::decode_command(bytes);
    const auto* d = std::get_if<kin::consumer_command>(&back);
    BOOST_REQUIRE(d != nullptr);
    BOOST_TEST(d->_consumer == "alpha");
    BOOST_TEST(d->_created_ms == 1234);
    BOOST_TEST(d->_set.at("a") == "1");
    BOOST_TEST(d->_remove.size() == 1U);
    BOOST_TEST(*d->_policy == "{}");
    BOOST_TEST(d->_max == 20U);
    BOOST_TEST((d->_op == kin::consumer_op::retag));
}
