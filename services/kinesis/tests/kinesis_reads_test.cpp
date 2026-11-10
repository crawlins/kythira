// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Reads and iterators (`.kiro/specs/kinesis-compatible-streams/` task 7,
// Requirement 9): the iterator token (MAC, tampering, key rotation, size),
// the shard reader over sealed and active segments (a missing segment is
// fetched or reported, never skipped; a trimmed position starts at the trim
// point), and GetShardIterator / GetRecords through the front end over the
// in-process cluster: all five iterator types, paging, MillisBehindLatest,
// expiry under both lifetime settings, and AWS's errors.

#define BOOST_TEST_MODULE kinesis_reads_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_iterators.hpp>
#include <kythira/kinesis/kinesis_records.hpp>
#include <kythira/kinesis/kinesis_service.hpp>

#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kin = kythira::kinesis;
namespace aj = kythira::aws_json;
namespace az = kythira::aws_auth;
namespace sv4 = kythira::sigv4;
namespace json = boost::json;

namespace {

constexpr std::string_view k_account = "111122223333";

struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path =
            std::filesystem::temp_directory_path() /
            ("kinesis_reads_test." + std::to_string(::getpid()) + "." + std::to_string(++counter));
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

// Small segments, so a few records roll several; no fsync.
auto make_store(const temp_dir& dir) -> std::unique_ptr<kythira::record_segment_store> {
    return std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
        ._data_dir = dir._path,
        ._max_segment_bytes = 400,
        ._max_segment_age_ms = 0,
        ._max_record_bytes = 4096,
        ._index_interval_bytes = 64,
        ._barriers_enabled = false,
    });
}

struct allow_all {
    auto authenticate(const sv4::request&, const sv4::service_rules&) -> az::authn_result {
        az::principal_context p;
        p.account = std::string(k_account);
        p.arn = "arn:aws:iam::" + std::string(k_account) + ":user/alice";
        return p;
    }
    auto authorize(const az::principal_context&, const az::authz_request& r) -> az::authz_decision {
        last_resource = r.resource;
        az::authz_decision d;
        d.allowed = true;
        return d;
    }
    std::string last_resource;
};

auto ring_of(std::uint32_t id, std::string secret) -> kin::iterator_key_ring {
    kin::iterator_key_ring ring;
    ring._keys.push_back({._id = id, ._secret = std::move(secret)});
    return ring;
}

auto service_config() -> kin::kinesis_service_config {
    kin::kinesis_service_config c;
    c._iterator_keys = ring_of(7, "a test iterator key of some length");
    // These cases page fast; the read limits have their own tests.
    c._throughput._read_calls_per_s = 1e9;
    c._throughput._iterator_calls_per_s = 1e9;
    c._throughput._read_bytes_per_s = 1e12;
    return c;
}

struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store = make_store(dir);
    std::int64_t clock = 1'762'776'000'000;
    kin::local_stream_cluster cluster{store.get(), [this] { return clock; }};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    kin::kinesis_service<kin::local_stream_cluster> service{cluster, service_config()};

    fixture() { service.register_with(fe); }

    auto call(std::string_view op, const json::value& body) -> aj::http_response {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._body = json::serialize(body);
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
        return fe.handle(r);
    }

    auto ok(std::string_view op, const json::value& body) -> json::object {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, std::string(op) + ": " + r.body);
        return json::parse(r.body).as_object();
    }

    auto error_type(std::string_view op, const json::value& body) -> std::string {
        const auto r = call(op, body);
        BOOST_REQUIRE_MESSAGE(r.status != 200U, std::string(op) + " unexpectedly succeeded");
        return std::string(json::parse(r.body).as_object().at("__type").as_string());
    }

    // One shard, so every record lands on shardId-000000000000.
    void create(std::string_view name = "s", const json::object& tags = {}) {
        json::object body{{"StreamName", name}, {"ShardCount", 1}};
        if (!tags.empty()) {
            body["Tags"] = tags;
        }
        (void)ok("CreateStream", body);
    }

    auto put(std::string_view data_b64, std::string_view stream = "s") -> std::string {
        const auto r =
            ok("PutRecord", {{"StreamName", stream}, {"PartitionKey", "pk"}, {"Data", data_b64}});
        return std::string(r.at("SequenceNumber").as_string());
    }

    auto iterator(const json::object& extra, std::string_view stream = "s") -> std::string {
        json::object body{{"StreamName", stream}, {"ShardId", "shardId-000000000000"}};
        for (const auto& kv : extra) {
            body[kv.key()] = kv.value();
        }
        return std::string(ok("GetShardIterator", body).at("ShardIterator").as_string());
    }

    auto get(std::string_view it, std::int64_t limit = 10'000) -> json::object {
        return ok("GetRecords", {{"ShardIterator", it}, {"Limit", limit}});
    }
};

auto b64(std::string_view s) -> std::string {
    return kin::service_detail::base64_encode(s);
}

auto data_of(const json::value& rec) -> std::string {
    return *kin::service_detail::base64_decode(rec.at("Data").as_string());
}

}  // namespace

// ---- the token --------------------------------------------------------------

BOOST_AUTO_TEST_CASE(an_iterator_round_trips_and_resists_tampering) {
    const auto ring = ring_of(3, "secret-one");
    kin::shard_iterator it{._account = std::string(k_account),
                           ._stream = "orders",
                           ._stream_id = 42,
                           ._ordinal = 5,
                           ._counter = 1234,
                           ._min_timestamp_ms = 99,
                           ._issued_ms = 1'762'776'000'000};
    const auto token = kin::encode_iterator(it, ring);
    const auto back = kin::decode_iterator(token, ring);
    BOOST_REQUIRE(back.has_value());
    BOOST_TEST((*back == it));

    // Any changed character fails the MAC (or the base64).
    for (std::size_t i = 0; i < token.size(); ++i) {
        auto bad = token;
        bad[i] = bad[i] == 'A' ? 'B' : 'A';
        BOOST_TEST(!kin::decode_iterator(bad, ring).has_value());
    }
    BOOST_TEST(!kin::decode_iterator(token, ring_of(3, "secret-two")).has_value());
    BOOST_TEST(!kin::decode_iterator("", ring).has_value());
    BOOST_TEST(!kin::decode_iterator("not base64!", ring).has_value());
    BOOST_TEST(!kin::decode_iterator(token.substr(0, 40), ring).has_value());

    BOOST_TEST(!kin::iterator_expired(it, it._issued_ms + kin::k_iterator_lifetime_ms,
                                      kin::k_iterator_lifetime_ms));
    BOOST_TEST(kin::iterator_expired(it, it._issued_ms + kin::k_iterator_lifetime_ms + 1,
                                     kin::k_iterator_lifetime_ms));
}

BOOST_AUTO_TEST_CASE(rotation_keeps_the_previous_key_valid) {
    const auto old_ring = ring_of(1, "old key");
    kin::shard_iterator it{._account = std::string(k_account), ._stream = "s", ._stream_id = 1};
    const auto token = kin::encode_iterator(it, old_ring);

    kin::iterator_key_ring rotated;
    rotated._keys.push_back({._id = 2, ._secret = "new key"});
    rotated._keys.push_back({._id = 1, ._secret = "old key"});
    BOOST_TEST(kin::decode_iterator(token, rotated).has_value());
    // New tokens use the new key...
    const auto fresh = kin::encode_iterator(it, rotated);
    BOOST_TEST(!kin::decode_iterator(fresh, old_ring).has_value());
    // ...and once the old key is dropped its tokens stop verifying.
    BOOST_TEST(!kin::decode_iterator(token, ring_of(2, "new key")).has_value());
}

BOOST_AUTO_TEST_CASE(the_longest_token_fits_the_model) {
    kin::shard_iterator it{._account = std::string(k_account),
                           ._stream = std::string(128, 'n'),
                           ._stream_id = UINT64_MAX,
                           ._ordinal = UINT32_MAX,
                           ._counter = UINT64_MAX,
                           ._min_timestamp_ms = INT64_MAX,
                           ._issued_ms = INT64_MAX};
    const auto token = kin::encode_iterator(it, ring_of(UINT32_MAX, "k"));
    BOOST_TEST(token.size() <= 512U);
    BOOST_CHECK_THROW((void)kin::encode_iterator(it, kin::iterator_key_ring{}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(expiry_messages_use_javas_date_form) {
    BOOST_TEST(kin::java_date_text(1'762'736'400'000) == "Mon Nov 10 01:00:00 UTC 2025");
}

// ---- the shard reader ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_reader_walks_sealed_and_active_segments) {
    fixture f;
    f.create();
    std::vector<std::string> sent;
    for (int i = 0; i < 40; ++i) {
        sent.push_back("record-" + std::to_string(i) + std::string(20, 'x'));
        (void)f.put(b64(sent.back()));
    }
    const auto key = kin::routing_key(1, 0);
    kin::shard_state state;
    BOOST_REQUIRE(f.cluster.read(
        key, kin::stream_reader([&](const auto& sm) { state = *sm.find_shard(key); })));
    BOOST_TEST_REQUIRE(state._sealed.size() >= 3U);

    // Everything, in order, across every segment.
    auto all = kin::read_shard(*f.store, {}, key, state, 1, 0, {});
    BOOST_REQUIRE(all._records.size() == sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        BOOST_TEST(all._records[i]._counter == i + 1);
        const auto& d = all._records[i]._record._data;
        BOOST_TEST((std::string(reinterpret_cast<const char*>(d.data()), d.size()) == sent[i]));
    }
    BOOST_TEST(all._caught_up);
    BOOST_TEST(all._next_counter == sent.size() + 1);

    // Pages of 7 cover the same records with no gap and no repeat.
    std::uint64_t next = 1;
    std::size_t seen = 0;
    for (int page = 0; page < 20 && seen < sent.size(); ++page) {
        auto r = kin::read_shard(*f.store, {}, key, state, next, 0,
                                 kythira::segment_read_limits{._max_records = 7});
        for (const auto& rec : r._records) {
            BOOST_TEST(rec._counter == seen + 1);
            ++seen;
        }
        next = r._next_counter;
    }
    BOOST_TEST(seen == sent.size());

    // A position below the trim point starts at the trim point.
    auto trimmed = state;
    trimmed._trim_before = 11;
    auto t = kin::read_shard(*f.store, {}, key, trimmed, 1, 0,
                             kythira::segment_read_limits{._max_records = 1});
    BOOST_REQUIRE(t._records.size() == 1U);
    BOOST_TEST(t._records[0]._counter == 11U);
}

BOOST_AUTO_TEST_CASE(a_missing_segment_is_fetched_or_reported_never_skipped) {
    fixture f;
    f.create();
    for (int i = 0; i < 30; ++i) {
        (void)f.put(b64("payload-" + std::to_string(i) + std::string(30, 'y')));
    }
    const auto key = kin::routing_key(1, 0);
    kin::shard_state state;
    BOOST_REQUIRE(f.cluster.read(
        key, kin::stream_reader([&](const auto& sm) { state = *sm.find_shard(key); })));
    BOOST_TEST_REQUIRE(!state._sealed.empty());

    // A replica with none of the files and no way to fetch them.
    temp_dir other_dir;
    auto other = make_store(other_dir);
    auto r = kin::read_shard(*other, {}, key, state, 1, 0, {});
    BOOST_TEST(r._missing);
    BOOST_TEST(r._records.empty());

    // With a fetcher that installs from the first store, it reads through.
    int fetched = 0;
    kin::segment_fetcher fetch = [&](const kythira::record_segment_descriptor& d) {
        ++fetched;
        const std::vector<std::string> peers{"peer"};
        const kythira::record_segment_store::fetch_transport t =
            [&](const std::string&, const std::string& stream, std::uint64_t first,
                std::uint64_t length) {
                return f.store->read_segment_bytes(stream, first, length);
            };
        if (!other->fetch_segment(d, peers, t)) {
            throw kythira::record_segment_error("fetch failed");
        }
    };
    // The active segment is not in the index: give the replica its prefix
    // as restore would.
    for (const auto& d : f.store->local_segments(key)) {
        if (!d._sealed) {
            fetch(d);
        }
    }
    r = kin::read_shard(*other, fetch, key, state, 1, 0, {});
    BOOST_TEST(!r._missing);
    BOOST_TEST(r._records.size() == 30U);
    BOOST_TEST(fetched >= static_cast<int>(state._sealed.size()));
}

// ---- GetShardIterator and GetRecords -----------------------------------------

BOOST_AUTO_TEST_CASE(trim_horizon_reads_everything_in_pages) {
    fixture f;
    f.create();
    for (int i = 0; i < 25; ++i) {
        f.clock += 10;
        (void)f.put(b64("r" + std::to_string(i)));
    }
    auto it = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}});
    std::vector<std::string> got;
    std::string last_seq;
    for (int page = 0; page < 10; ++page) {
        const auto r = f.get(it, 10);
        for (const auto& rec : r.at("Records").as_array()) {
            got.push_back(data_of(rec));
            const auto seq = std::string(rec.at("SequenceNumber").as_string());
            BOOST_TEST(seq > last_seq);
            last_seq = seq;
            BOOST_TEST(std::string(rec.at("PartitionKey").as_string()) == "pk");
            BOOST_TEST(rec.at("ApproximateArrivalTimestamp").is_number());
        }
        BOOST_REQUIRE(r.contains("NextShardIterator"));
        it = std::string(r.at("NextShardIterator").as_string());
        if (r.at("MillisBehindLatest").as_int64() == 0 && r.at("Records").as_array().empty()) {
            break;
        }
    }
    BOOST_REQUIRE(got.size() == 25U);
    for (int i = 0; i < 25; ++i) {
        BOOST_TEST(got[static_cast<std::size_t>(i)] == "r" + std::to_string(i));
    }
    // Caught up: an open shard keeps handing out a next iterator, and new
    // records show up through it.
    (void)f.put(b64("late"));
    const auto r = f.get(it);
    BOOST_REQUIRE(r.at("Records").as_array().size() == 1U);
    BOOST_TEST(data_of(r.at("Records").at(0)) == "late");
    BOOST_TEST(r.at("MillisBehindLatest").as_int64() == 0);
}

BOOST_AUTO_TEST_CASE(millis_behind_latest_counts_from_the_last_record_returned) {
    fixture f;
    f.create();
    (void)f.put(b64("a"));
    f.clock += 1000;
    (void)f.put(b64("b"));
    f.clock += 500;
    const auto it = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}});
    const auto r = f.get(it, 1);
    BOOST_REQUIRE(r.at("Records").as_array().size() == 1U);
    BOOST_TEST(r.at("MillisBehindLatest").as_int64() == 1500);
}

BOOST_AUTO_TEST_CASE(latest_and_the_sequence_number_types) {
    fixture f;
    f.create();
    const auto s1 = f.put(b64("one"));
    const auto s2 = f.put(b64("two"));
    (void)f.put(b64("three"));

    const auto latest = f.iterator({{"ShardIteratorType", "LATEST"}});
    BOOST_TEST(f.get(latest).at("Records").as_array().empty());
    (void)f.put(b64("four"));
    const auto after_latest = f.get(latest).at("Records").as_array();
    BOOST_REQUIRE(after_latest.size() == 1U);
    BOOST_TEST(data_of(after_latest[0]) == "four");

    const auto at = f.get(
        f.iterator({{"ShardIteratorType", "AT_SEQUENCE_NUMBER"}, {"StartingSequenceNumber", s2}}));
    BOOST_REQUIRE(at.at("Records").as_array().size() == 3U);
    BOOST_TEST(data_of(at.at("Records").at(0)) == "two");

    const auto after = f.get(f.iterator(
        {{"ShardIteratorType", "AFTER_SEQUENCE_NUMBER"}, {"StartingSequenceNumber", s1}}));
    BOOST_REQUIRE(after.at("Records").as_array().size() == 3U);
    BOOST_TEST(data_of(after.at("Records").at(0)) == "two");

    BOOST_TEST(f.error_type("GetShardIterator", {{"StreamName", "s"},
                                                 {"ShardId", "shardId-000000000000"},
                                                 {"ShardIteratorType", "AT_SEQUENCE_NUMBER"}}) ==
               "InvalidArgumentException");
    // A sequence number of another shard.
    const auto foreign = kin::format_sequence(kin::sequence_number{._ordinal = 3, ._counter = 1});
    BOOST_TEST(f.error_type("GetShardIterator", {{"StreamName", "s"},
                                                 {"ShardId", "shardId-000000000000"},
                                                 {"ShardIteratorType", "AT_SEQUENCE_NUMBER"},
                                                 {"StartingSequenceNumber", foreign}}) ==
               "InvalidArgumentException");
}

BOOST_AUTO_TEST_CASE(at_timestamp_starts_at_the_first_record_at_or_after_it) {
    fixture f;
    f.create();
    for (int i = 0; i < 20; ++i) {
        f.clock += 1000;
        (void)f.put(b64("t" + std::to_string(i)));
    }
    // Record i arrived at base + 1000 * (i + 1); ask for record 12's time.
    const auto base = 1'762'776'000'000;
    const double ts = static_cast<double>(base + 1000 * 13) / 1000.0;
    auto it = f.iterator({{"ShardIteratorType", "AT_TIMESTAMP"}, {"Timestamp", ts}});
    const auto r = f.get(it);
    BOOST_REQUIRE(r.at("Records").as_array().size() == 8U);
    BOOST_TEST(data_of(r.at("Records").at(0)) == "t12");

    // A time past every record: nothing yet, and later records that are
    // past it are returned through the next iterator.
    const double future = static_cast<double>(f.clock + 5000) / 1000.0;
    it = f.iterator({{"ShardIteratorType", "AT_TIMESTAMP"}, {"Timestamp", future}});
    auto r2 = f.get(it);
    BOOST_TEST(r2.at("Records").as_array().empty());
    (void)f.put(b64("too-early"));
    f.clock += 10'000;
    (void)f.put(b64("in-time"));
    r2 = f.get(std::string(r2.at("NextShardIterator").as_string()));
    BOOST_REQUIRE(r2.at("Records").as_array().size() == 1U);
    BOOST_TEST(data_of(r2.at("Records").at(0)) == "in-time");

    BOOST_TEST(f.error_type("GetShardIterator", {{"StreamName", "s"},
                                                 {"ShardId", "shardId-000000000000"},
                                                 {"ShardIteratorType", "AT_TIMESTAMP"}}) ==
               "InvalidArgumentException");
}

BOOST_AUTO_TEST_CASE(a_trimmed_position_starts_at_the_trim_horizon) {
    fixture f;
    f.create();
    for (int i = 0; i < 10; ++i) {
        (void)f.put(b64("v" + std::to_string(i)));
    }
    const auto at_first = f.iterator(
        {{"ShardIteratorType", "AT_SEQUENCE_NUMBER"},
         {"StartingSequenceNumber", kin::format_sequence(kin::sequence_number{._counter = 1})}});
    const kin::stream_command trim = kin::trim_shard_command{kin::routing_key(1, 0), 6};
    BOOST_REQUIRE(f.cluster.propose(kin::command_key(trim), kin::encode_command(trim)));
    const auto r = f.get(at_first);
    BOOST_REQUIRE(r.at("Records").as_array().size() == 5U);
    BOOST_TEST(data_of(r.at("Records").at(0)) == "v5");
    const auto horizon = f.get(f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}}));
    BOOST_TEST(data_of(horizon.at("Records").at(0)) == "v5");
}

BOOST_AUTO_TEST_CASE(iterators_expire_after_the_profiles_lifetime) {
    fixture f;
    f.create();
    f.create("relaxed", {{"kythira:compat:iterator-lifetime", "relaxed"}});
    (void)f.put(b64("x"));
    (void)f.put(b64("y"), "relaxed");
    const auto strict = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}});
    const auto loose = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}}, "relaxed");

    f.clock += kin::k_iterator_lifetime_ms;
    BOOST_TEST(f.get(strict).at("Records").as_array().size() == 1U);
    f.clock += 1;
    const auto r = f.call("GetRecords", {{"ShardIterator", strict}});
    BOOST_TEST(r.status == 400U);
    const auto body = json::parse(r.body).as_object();
    BOOST_TEST(std::string(body.at("__type").as_string()) == "ExpiredIteratorException");
    BOOST_TEST(std::string(body.at("message").as_string()).starts_with("Iterator expired."));

    f.clock += 60 * 60 * 1000;
    BOOST_TEST(f.get(loose).at("Records").as_array().size() == 1U);
}

BOOST_AUTO_TEST_CASE(errors_and_authorization) {
    fixture f;
    f.create();
    (void)f.put(b64("x"));
    const auto it = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}});

    // GetRecords is authorized on the token's stream.
    f.auth.last_resource.clear();
    (void)f.get(it);
    BOOST_TEST(f.auth.last_resource ==
               "arn:aws:kinesis:us-east-1:" + std::string(k_account) + ":stream/s");

    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator", "AAAA"}}) ==
               "InvalidArgumentException");
    auto tampered = it;
    tampered[10] = tampered[10] == 'A' ? 'B' : 'A';
    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator", tampered}}) ==
               "InvalidArgumentException");
    BOOST_TEST(f.error_type("GetShardIterator", {{"StreamName", "s"},
                                                 {"ShardId", "shardId-000000000009"},
                                                 {"ShardIteratorType", "LATEST"}}) ==
               "ResourceNotFoundException");
    BOOST_TEST(f.error_type("GetShardIterator", {{"StreamName", "nope"},
                                                 {"ShardId", "shardId-000000000000"},
                                                 {"ShardIteratorType", "LATEST"}}) ==
               "ResourceNotFoundException");
    // A StreamARN that is not the token's stream.
    BOOST_TEST(f.error_type("GetRecords",
                            {{"ShardIterator", it},
                             {"StreamARN", "arn:aws:kinesis:us-east-1:" + std::string(k_account) +
                                               ":stream/other"}}) == "InvalidArgumentException");

    // A token from another cluster's key.
    kin::shard_iterator foreign{
        ._account = std::string(k_account), ._stream = "s", ._stream_id = 1, ._issued_ms = f.clock};
    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator",
                                            kin::encode_iterator(foreign, ring_of(7, "other"))}}) ==
               "InvalidArgumentException");

    // A deleted and recreated stream does not honour old iterators.
    (void)f.ok("DeleteStream", {{"StreamName", "s"}});
    f.create();
    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator", it}}) == "ResourceNotFoundException");
}

BOOST_AUTO_TEST_CASE(a_shard_without_a_store_is_an_internal_failure) {
    std::int64_t clock = 1'762'776'000'000;
    kin::local_stream_cluster cluster{nullptr, [&clock] { return clock; }};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    kin::kinesis_service<kin::local_stream_cluster> service{cluster};
    service.register_with(fe);
    const auto call = [&](std::string_view op, const json::value& body) {
        kin::front_end_request r;
        r._method = "POST";
        r._path = "/";
        r._body = json::serialize(body);
        r._headers = {{"Content-Type", "application/x-amz-json-1.1"},
                      {"X-Amz-Target", "Kinesis_20131202." + std::string(op)}};
        return fe.handle(r);
    };
    BOOST_REQUIRE(call("CreateStream", {{"StreamName", "s"}, {"ShardCount", 1}}).status == 200U);
    const auto it = call("GetShardIterator", {{"StreamName", "s"},
                                              {"ShardId", "shardId-000000000000"},
                                              {"ShardIteratorType", "TRIM_HORIZON"}});
    BOOST_REQUIRE(it.status == 200U);
    const auto token = std::string(json::parse(it.body).at("ShardIterator").as_string());
    BOOST_TEST(call("GetRecords", {{"ShardIterator", token}}).status == 500U);
}

BOOST_AUTO_TEST_CASE(the_iterator_key_file_and_the_derived_key) {
    const auto ring = kin::parse_iterator_keys(
        "# rotated 2026-11-10\n"
        "2 a-new-key-of-16-bytes-or-more\n"
        "\n"
        "1\tthe-previous-key-still-valid\r\n");
    BOOST_REQUIRE(ring._keys.size() == 2U);
    BOOST_TEST(ring._keys[0]._id == 2U);
    BOOST_TEST(ring._keys[1]._secret == "the-previous-key-still-valid");

    const auto fails_without_echo = [](std::string_view text, std::string_view secret) {
        try {
            (void)kin::parse_iterator_keys(text);
        } catch (const std::invalid_argument& e) {
            return std::string_view(e.what()).find(secret) == std::string_view::npos;
        }
        return false;
    };
    BOOST_TEST(fails_without_echo("1 short-secret\n", "short-secret"));
    BOOST_TEST(fails_without_echo("x some-secret-long-enough\n", "some-secret-long-enough"));
    BOOST_TEST(fails_without_echo("1 a b\n", "zzz"));
    BOOST_TEST(fails_without_echo("4294967296 some-secret-long-enough\n", "some-secret"));
    BOOST_TEST(fails_without_echo("1 some-secret-long-enough\n1 other-secret-long-enough\n",
                                  "other-secret"));
    BOOST_TEST(fails_without_echo("# nothing\n", "zzz"));

    // Every gateway with the same forwarding secret derives the same key,
    // and it is not the secret itself.
    const auto a = kin::derived_iterator_key("0123456789abcdef0123");
    const auto b = kin::derived_iterator_key("0123456789abcdef0123");
    BOOST_TEST(a._keys.at(0)._secret == b._keys.at(0)._secret);
    BOOST_TEST(a._keys.at(0)._secret != "0123456789abcdef0123");
    BOOST_TEST(a._keys.at(0)._secret !=
               kin::derived_iterator_key("another-secret-0123")._keys.at(0)._secret);
}

// Requirement 7.5: a KPL aggregated record (magic, protobuf body, MD5
// trailer) is opaque data, stored and returned byte for byte, NULs and high
// bytes included; only the outer partition key is the record's.
BOOST_AUTO_TEST_CASE(kpl_aggregated_records_come_back_byte_for_byte) {
    fixture f;
    f.create();
    using std::string_literals::operator""s;
    std::string aggregated = "\xF3\x89\x9A\xC2"s;
    aggregated += "\x0A\x03pk1\x0A\x03pk2\x1A\x07\x08\x00\x1A\x03one"s;
    aggregated += "\x1A\x07\x08\x01\x1A\x03two"s;
    aggregated += "\x00\xFF\x10\x80\x7F\x00\x01\x02\xFE\xFD\x00\x00\x9A\xC2\x89\xF3"s;
    BOOST_REQUIRE(aggregated.size() == 4U + 19U + 9U + 16U);
    (void)f.put(b64(aggregated));
    const auto r = f.get(f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}}));
    BOOST_REQUIRE(r.at("Records").as_array().size() == 1U);
    const auto& rec = r.at("Records").at(0);
    BOOST_TEST((data_of(rec) == aggregated));
    BOOST_TEST(std::string(rec.at("PartitionKey").as_string()) == "pk");
}

// AWS's GetRecords takes a Limit of 1 to 10 000 records; anything else is
// the model's ValidationException, never silently clamped.
BOOST_AUTO_TEST_CASE(get_records_limit_follows_the_model) {
    fixture f;
    f.create();
    (void)f.put(b64("one"));
    const auto it = f.iterator({{"ShardIteratorType", "TRIM_HORIZON"}});
    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator", it}, {"Limit", 0}}) ==
               "ValidationException");
    BOOST_TEST(f.error_type("GetRecords", {{"ShardIterator", it}, {"Limit", 10'001}}) ==
               "ValidationException");
    BOOST_TEST(f.get(it, 1).at("Records").as_array().size() == 1U);
}
