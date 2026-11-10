// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Retention and tiering (`.kiro/specs/kinesis-compatible-streams/` task 10,
// Requirements 11.2-11.4): IncreaseStreamRetentionPeriod and
// DecreaseStreamRetentionPeriod with AWS's refusals; the seal, trim and
// tier entries on the state machine (guards, snapshot, trim taking tiered
// entries with it); the maintenance pass sealing an idle shard, trimming
// past retention and tiering old segments to an object store; reads that
// fetch a tiered segment back with the local copy gone; and the disk guard
// refusing writes while reads go on.

#define BOOST_TEST_MODULE kinesis_retention_test

#include <boost/test/unit_test.hpp>

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_disk_guard.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_maintenance.hpp>
#include <kythira/kinesis/kinesis_service.hpp>
#include <kythira/kinesis/kinesis_tiering.hpp>

#include <raft/record_segment_store.hpp>

#include <boost/json.hpp>

#include <unistd.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
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
constexpr std::int64_t k_hour = kin::k_ms_per_hour;

struct temp_dir {
    std::filesystem::path _path;
    temp_dir() {
        static int counter = 0;
        _path = std::filesystem::temp_directory_path() /
                ("kinesis_retention_test." + std::to_string(::getpid()) + "." +
                 std::to_string(++counter));
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

// An object store in memory, counting its calls.
struct memory_objects {
    std::map<std::string, std::string> _objects;
    int _gets = 0;

    auto tier(std::string prefix = "t/") -> kin::tier_store {
        kin::tier_store t;
        t._prefix = std::move(prefix);
        t._put = [this](const std::string& k, std::string_view b) { _objects[k] = std::string(b); };
        t._get = [this](const std::string& k) -> std::optional<std::string> {
            ++_gets;
            const auto it = _objects.find(k);
            return it == _objects.end() ? std::nullopt : std::optional{it->second};
        };
        t._remove = [this](const std::string& k) { _objects.erase(k); };
        return t;
    }
};

// One node: a store, a cluster on a fake wall clock, the service, and a
// maintenance pass whose fetcher reads tiered segments back.
struct fixture {
    temp_dir dir;
    std::unique_ptr<kythira::record_segment_store> store =
        std::make_unique<kythira::record_segment_store>(kythira::record_segment_store_options{
            ._data_dir = dir._path, ._barriers_enabled = false});
    std::int64_t wall = 1'762'776'000'000;
    memory_objects objects;
    kin::tier_store tier = objects.tier();
    kin::fetched_segments fetched;
    kin::local_stream_cluster cluster{
        store.get(), [this] { return wall; },
        [this](const kythira::record_segment_descriptor& d) {
            if (!kin::fetch_from_tier(*store, tier, d)) {
                throw kythira::record_segment_error("not in the tier");
            }
            fetched.note(d._stream, d._first_seq, wall);
        }};
    std::uint64_t free_bytes = std::uint64_t{1} << 40U;
    kin::disk_guard guard{std::uint64_t{1} << 30U, [this] { return std::optional{free_bytes}; },
                          [this] { return wall; }, std::chrono::milliseconds{0}};
    allow_all auth;
    kin::front_end<allow_all> fe{kin::front_end_config{}, auth};
    std::unique_ptr<kin::kinesis_service<kin::local_stream_cluster>> service;

    fixture() {
        cluster.set_disk_guard(&guard);
        service = std::make_unique<kin::kinesis_service<kin::local_stream_cluster>>(
            cluster, kin::kinesis_service_config{});
        service->register_with(fe);
    }

    auto maintenance(std::int64_t tier_after_ms = 0)
        -> kin::stream_maintenance<kin::local_stream_cluster> {
        return {cluster, *store, tier_after_ms > 0 ? tier : kin::tier_store{}, fetched,
                kin::maintenance_config{._tier_after_ms = tier_after_ms,
                                        ._keep_fetched_ms = 10 * 60 * 1000}};
    }

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

    void create(std::string_view name) {
        (void)ok("CreateStream", {{"StreamName", name}, {"ShardCount", 1}});
    }

    void put(std::string_view stream, int n) {
        for (int i = 0; i < n; ++i) {
            (void)ok("PutRecord",
                     {{"StreamName", stream}, {"PartitionKey", "p"}, {"Data", "aGk="}});
        }
    }

    auto key() const -> std::string { return kin::routing_key(1, 0); }

    auto shard() -> kin::shard_state {
        std::optional<kin::shard_state> out;
        (void)cluster.read(key(), [&](const auto& sm) {
            if (const auto* s = sm.find_shard(key())) {
                out = *s;
            }
        });
        BOOST_REQUIRE(out);
        return *out;
    }

    // Every record from TRIM_HORIZON, as the data strings.
    auto read_all(std::string_view stream) -> std::vector<std::string> {
        auto it = ok("GetShardIterator", {{"StreamName", stream},
                                          {"ShardId", "shardId-000000000000"},
                                          {"ShardIteratorType", "TRIM_HORIZON"}})
                      .at("ShardIterator")
                      .as_string();
        std::vector<std::string> out;
        for (int i = 0; i < 20; ++i) {
            auto r = ok("GetRecords", {{"ShardIterator", it}});
            for (const auto& rec : r.at("Records").as_array()) {
                out.emplace_back(rec.as_object().at("SequenceNumber").as_string());
            }
            if (r.at("MillisBehindLatest").as_int64() == 0 && r.at("Records").as_array().empty()) {
                break;
            }
            it = r.at("NextShardIterator").as_string();
        }
        return out;
    }

    auto retention(std::string_view stream) -> std::int64_t {
        return ok("DescribeStreamSummary", {{"StreamName", stream}})
            .at("StreamDescriptionSummary")
            .as_object()
            .at("RetentionPeriodHours")
            .as_int64();
    }
};

auto type_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("__type").as_string());
}

auto message_of(const aj::http_response& r) -> std::string {
    return std::string(json::parse(r.body).as_object().at("message").as_string());
}

}  // namespace

// ---- the retention calls ------------------------------------------------------

BOOST_AUTO_TEST_CASE(retention_moves_up_and_down_within_aws_range) {
    fixture f;
    f.create("s");
    BOOST_TEST(f.retention("s") == 24);
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 168}});
    BOOST_TEST(f.retention("s") == 168);
    // The same period is accepted and changes nothing.
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 168}});
    (void)f.ok("DecreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 48}});
    BOOST_TEST(f.retention("s") == 48);
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 8760}});
    BOOST_TEST(f.retention("s") == 8760);
}

BOOST_AUTO_TEST_CASE(retention_refusals_carry_aws_messages) {
    fixture f;
    f.create("s");
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 48}});

    auto r = f.call("IncreaseStreamRetentionPeriod",
                    {{"StreamName", "s"}, {"RetentionPeriodHours", 24}});
    BOOST_TEST(r.status == 400U);
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(
        message_of(r) ==
        "Requested retention period (24 hours) for stream s can not be shorter than existing "
        "retention period (48 hours). Use DecreaseRetentionPeriod API.");

    r = f.call("DecreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 72}});
    BOOST_TEST(type_of(r) == "InvalidArgumentException");
    BOOST_TEST(message_of(r) ==
               "Requested retention period (72 hours) for stream s can not be longer than existing "
               "retention period (48 hours). Use IncreaseRetentionPeriod API.");

    r = f.call("DecreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 12}});
    BOOST_TEST(message_of(r) ==
               "Minimum allowed retention period is 24 hours. Requested retention period (12 "
               "hours) is too short.");
    r = f.call("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 9000}});
    BOOST_TEST(message_of(r) ==
               "Maximum allowed retention period is 8760 hours. Requested retention period (9000 "
               "hours) is too long.");

    r = f.call("IncreaseStreamRetentionPeriod",
               {{"StreamName", "missing"}, {"RetentionPeriodHours", 48}});
    BOOST_TEST(type_of(r) == "ResourceNotFoundException");
    BOOST_TEST(f.retention("s") == 48);
}

// ---- the entries ----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(seal_tier_and_trim_entries_are_guarded_and_snapshotted) {
    fixture f;
    f.create("s");
    f.put("s", 3);
    const auto key = f.key();
    // A seal naming a different active segment does nothing.
    auto r = f.cluster.propose(key, kin::encode_command(kin::seal_shard_command{key, 99}));
    BOOST_REQUIRE(r);
    BOOST_TEST(f.shard()._sealed.empty());
    r = f.cluster.propose(key, kin::encode_command(kin::seal_shard_command{key, 1}));
    BOOST_REQUIRE(f.shard()._sealed.size() == 1U);
    const auto d = f.shard()._sealed.front();
    BOOST_TEST(d._last_seq == 3U);

    // A tier entry with another digest is refused and keeps the local copy.
    auto wrong = d._sha256;
    wrong[0] ^= std::byte{1};
    r = f.cluster.propose(key, kin::encode_command(kin::tier_segment_command{key, 1, wrong}));
    BOOST_TEST((r->_status == kin::apply_status::not_found));
    BOOST_TEST(f.store->describe(key, 1).has_value());

    r = f.cluster.propose(key, kin::encode_command(kin::tier_segment_command{key, 1, d._sha256}));
    BOOST_TEST((r->_status == kin::apply_status::ok));
    BOOST_TEST(f.shard().is_tiered(1));
    BOOST_TEST(!f.store->describe(key, 1).has_value());

    // The snapshot carries the tiered list.
    std::vector<std::byte> snap;
    (void)f.cluster.read(key, [&](const auto& sm) { snap = sm.get_state(); });
    kin::stream_state_machine<std::uint64_t> other(nullptr);
    other.restore_from_snapshot(snap, 10);
    BOOST_REQUIRE(other.find_shard(key) != nullptr);
    BOOST_TEST(other.find_shard(key)->_tiered == std::vector<std::uint64_t>{1});

    // A trim takes the tiered entry with its segment.
    r = f.cluster.propose(key, kin::encode_command(kin::trim_shard_command{key, 4}));
    BOOST_TEST(f.shard()._tiered.empty());
    BOOST_TEST(f.shard()._sealed.empty());
    BOOST_TEST(f.shard()._trim_before == 4U);
}

BOOST_AUTO_TEST_CASE(retention_entry_checks_range_and_stream_id) {
    kin::stream_state_machine<std::uint64_t> sm(nullptr);
    const auto apply = [&](const kin::stream_command& c, std::uint64_t i) {
        return kin::decode_result(sm.apply(kin::encode_command(c), i));
    };
    kin::create_stream_command c;
    c._account = std::string(k_account);
    c._name = "s";
    BOOST_TEST((apply(c, 1)._status == kin::apply_status::ok));
    BOOST_TEST((apply(kin::set_retention_command{c._account, "s", 1, 23}, 2)._status ==
                kin::apply_status::invalid));
    BOOST_TEST((apply(kin::set_retention_command{c._account, "s", 1, 8761}, 3)._status ==
                kin::apply_status::invalid));
    BOOST_TEST((apply(kin::set_retention_command{c._account, "s", 7, 48}, 4)._status ==
                kin::apply_status::not_found));
    BOOST_TEST((apply(kin::set_retention_command{c._account, "s", 1, 48}, 5)._status ==
                kin::apply_status::ok));
    BOOST_TEST(sm.find_stream(c._account, "s")->_retention_hours == 48U);
    // Every new command round-trips through the codec.
    const kin::stream_command cmds[] = {
        kin::set_retention_command{c._account, "s", 1, 48},
        kin::seal_shard_command{"k", 5},
        kin::tier_segment_command{"k", 5, kythira::segment_sha256{std::byte{7}}},
    };
    for (const auto& cmd : cmds) {
        const auto bytes = kin::encode_command(cmd);
        BOOST_TEST(kin::decode_command(bytes).index() == cmd.index());
        BOOST_TEST((kin::encode_command(kin::decode_command(bytes)) == bytes));
    }
}

// ---- the maintenance pass ---------------------------------------------------------

BOOST_AUTO_TEST_CASE(maintenance_trims_segments_past_retention) {
    fixture f;
    f.create("s");
    f.put("s", 3);
    f.wall += 2 * k_hour;
    f.put("s", 2);  // past the store's one-hour age bound: the first segment rolls
    BOOST_REQUIRE(f.shard()._sealed.size() == 1U);
    BOOST_TEST(f.read_all("s").size() == 5U);

    auto m = f.maintenance();
    f.wall += 22 * k_hour;  // the first segment's records are 24 h old, not past
    auto stats = m.run_once();
    BOOST_TEST(stats._trims == 0U);
    f.wall += 1;
    stats = m.run_once();
    BOOST_TEST(stats._trims == 1U);
    BOOST_TEST(f.shard()._trim_before == 4U);
    BOOST_TEST(f.read_all("s").size() == 2U);
    // Nothing more to do on the next pass.
    stats = m.run_once();
    BOOST_TEST(stats._trims == 0U);
    BOOST_TEST(stats._seals == 0U);
}

BOOST_AUTO_TEST_CASE(maintenance_seals_an_idle_shard_then_trims_it) {
    fixture f;
    f.create("s");
    f.put("s", 4);
    auto m = f.maintenance();
    f.wall += 25 * k_hour;
    auto stats = m.run_once();
    BOOST_TEST(stats._seals == 1U);
    stats = m.run_once();
    BOOST_TEST(stats._trims == 1U);
    BOOST_TEST(f.read_all("s").empty());
    // The shard's position survives: the next record continues the count.
    f.put("s", 1);
    BOOST_TEST(f.shard()._last_counter == 5U);
    BOOST_TEST(f.read_all("s").size() == 1U);
}

BOOST_AUTO_TEST_CASE(a_shorter_retention_trims_on_the_next_pass) {
    fixture f;
    f.create("s");
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 72}});
    f.put("s", 2);
    f.wall += 2 * k_hour;
    f.put("s", 1);
    auto m = f.maintenance();
    f.wall += 30 * k_hour;
    BOOST_TEST(m.run_once()._trims == 0U);
    (void)f.ok("DecreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 24}});
    BOOST_TEST(m.run_once()._trims == 1U);
    BOOST_TEST(f.read_all("s").size() == 1U);
}

BOOST_AUTO_TEST_CASE(tiered_segments_leave_the_disk_and_read_back_transparently) {
    fixture f;
    f.create("s");
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 8760}});
    f.put("s", 3);
    f.wall += 2 * k_hour;
    f.put("s", 2);
    const auto before = f.read_all("s");
    BOOST_REQUIRE(before.size() == 5U);

    auto m = f.maintenance(k_hour);
    f.wall += 2 * k_hour;
    auto stats = m.run_once();
    // The idle active segment is sealed, the older one tiered.
    BOOST_TEST(stats._seals == 1U);
    BOOST_TEST(stats._tiered == 1U);
    stats = m.run_once();
    BOOST_TEST(stats._tiered == 1U);
    const auto sh = f.shard();
    BOOST_TEST(sh._tiered.size() == 2U);
    BOOST_TEST(f.store->local_segments(f.key()).empty());
    BOOST_TEST(f.objects._objects.size() == 2U);
    for (const auto& d : sh._sealed) {
        BOOST_TEST(f.objects._objects.contains(kin::segment_object_key("t/", d)));
    }

    // A long-retention read with every local copy gone.
    BOOST_TEST(f.read_all("s") == before);
    BOOST_TEST(f.objects._gets >= 2);
    BOOST_TEST(f.store->local_segments(f.key()).size() == 2U);

    // Fetched copies stay while recent, then go.
    BOOST_TEST(m.run_once()._evicted == 0U);
    f.wall += 11 * 60 * 1000;
    BOOST_TEST(m.run_once()._evicted == 2U);
    BOOST_TEST(f.store->local_segments(f.key()).empty());

    // Trimming a tiered segment deletes its object.
    (void)f.ok("DecreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 24}});
    f.wall += 30 * k_hour;
    stats = m.run_once();
    BOOST_TEST(stats._trims == 1U);
    BOOST_TEST(stats._objects_deleted == 2U);
    BOOST_TEST(f.objects._objects.empty());
    BOOST_TEST(f.read_all("s").empty());
}

BOOST_AUTO_TEST_CASE(a_tiered_segment_whose_object_is_gone_fails_the_read) {
    fixture f;
    f.create("s");
    f.put("s", 2);
    auto m = f.maintenance(k_hour);
    f.wall += 2 * k_hour;
    (void)m.run_once();
    (void)m.run_once();
    BOOST_REQUIRE(f.shard()._tiered.size() == 1U);
    f.objects._objects.clear();
    const auto it = f.ok("GetShardIterator", {{"StreamName", "s"},
                                              {"ShardId", "shardId-000000000000"},
                                              {"ShardIteratorType", "TRIM_HORIZON"}})
                        .at("ShardIterator")
                        .as_string();
    const auto r = f.call("GetRecords", {{"ShardIterator", it}});
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(type_of(r) == "InternalFailure");
}

BOOST_AUTO_TEST_CASE(object_keys_name_the_shard_counter_and_digest) {
    kythira::record_segment_descriptor d;
    d._stream = std::string("\0ab", 3);
    d._first_seq = 42;
    d._sha256[0] = std::byte{0xab};
    const auto key = kin::segment_object_key("pfx/", d);
    BOOST_TEST(key == "pfx/kinesis/segments/006162/00000000000000000042-ab" + std::string(62, '0'));
}

// ---- the disk guard -----------------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_disk_guard_refuses_writes_and_lets_reads_through) {
    fixture f;
    f.create("s");
    f.put("s", 2);
    f.free_bytes = std::uint64_t{512} << 20U;  // half the 1 GiB floor

    auto r = f.call("PutRecord", {{"StreamName", "s"}, {"PartitionKey", "p"}, {"Data", "aGk="}});
    BOOST_TEST(r.status == 500U);
    BOOST_TEST(type_of(r) == "InternalFailure");
    BOOST_TEST(message_of(r).find("Kythira disk guard") == 0U);

    const auto batch = f.ok(
        "PutRecords",
        {{"StreamName", "s"}, {"Records", json::array{{{"PartitionKey", "p"}, {"Data", "aGk="}}}}});
    BOOST_TEST(batch.at("FailedRecordCount").as_int64() == 1);
    const auto& entry = batch.at("Records").as_array().at(0).as_object();
    BOOST_TEST(entry.at("ErrorCode").as_string() == "InternalFailure");
    BOOST_TEST(std::string(entry.at("ErrorMessage").as_string()).find("Kythira disk guard") == 0U);

    BOOST_TEST(f.read_all("s").size() == 2U);
    // Control-plane and maintenance entries are not appends.
    (void)f.ok("IncreaseStreamRetentionPeriod",
               {{"StreamName", "s"}, {"RetentionPeriodHours", 48}});

    f.free_bytes = std::uint64_t{2} << 30U;
    f.put("s", 1);
    BOOST_TEST(f.read_all("s").size() == 3U);
}

BOOST_AUTO_TEST_CASE(the_disk_guard_rechecks_at_most_once_per_interval) {
    std::int64_t now = 0;
    int calls = 0;
    std::uint64_t free = 10;
    kin::disk_guard g(
        100,
        [&] {
            ++calls;
            return std::optional{free};
        },
        [&] { return now; }, std::chrono::milliseconds{1000});
    BOOST_TEST(g.below_floor());
    free = 1000;
    BOOST_TEST(g.below_floor());  // cached
    BOOST_TEST(calls == 1);
    now = 1000;
    BOOST_TEST(!g.below_floor());
    BOOST_TEST(calls == 2);
    // Unknown free space admits; a zero floor never refuses.
    kin::disk_guard unknown(100, [] { return std::optional<std::uint64_t>{}; }, [] { return 0; });
    BOOST_TEST(!unknown.below_floor());
    kin::disk_guard off(0, [] { return std::optional<std::uint64_t>{0}; }, [] { return 0; });
    BOOST_TEST(!off.below_floor());
}

// doc/retention.md: apply() never consults the disk guard. An append that
// is already in the log (proposed through another node) applies on a
// replica whose own disk is below the floor, so replicas never diverge on
// a local condition.
BOOST_AUTO_TEST_CASE(an_append_in_the_log_applies_whatever_the_local_disk) {
    fixture f;
    f.create("s");
    f.put("s", 1);
    f.free_bytes = std::uint64_t{1} << 20U;  // far below the 1 GiB floor
    BOOST_REQUIRE(f.guard.below_floor());
    kin::append_records_command a{._key = f.key(), ._arrival_ms = f.wall};
    a._records.push_back({._partition_key = "p", ._data = std::vector<std::byte>(4, std::byte{7})});
    const kin::stream_command cmd = a;
    // A replica applies committed entries; the local cluster's transact is
    // that path (no proposal-time checks).
    BOOST_TEST((f.cluster.transact({{f.key(), kin::encode_command(cmd)}}) ==
                kin::transact_outcome::committed));
    BOOST_TEST(f.shard()._last_counter == 2U);
    BOOST_TEST(f.read_all("s").size() == 2U);
    // A new proposal from this node is still refused.
    const auto again = f.cluster.propose(f.key(), kin::encode_command(cmd));
    BOOST_REQUIRE(again.has_value());
    BOOST_TEST((again->_status != kin::apply_status::ok));
}
