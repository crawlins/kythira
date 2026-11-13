// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_service.hpp
/// @brief The Kinesis operation handlers over any `stream_cluster`
///        (`.kiro/specs/kinesis-compatible-streams/`): stream lifecycle and
///        writes (task 6, Requirements 6.1, 7.1-7.5, 8.2: CreateStream,
///        DeleteStream, DescribeStream, DescribeStreamSummary, ListStreams,
///        ListShards, DescribeLimits, PutRecord, PutRecords) and reads
///        (task 7, Requirement 9: GetShardIterator, GetRecords).
///
/// Lifecycle. CreateStream commits the stream record in CREATING at the
/// catalog group (which allocates the stream id), opens each shard under its
/// routing key, and moves the stream to ACTIVE; DeleteStream moves it to
/// DELETING, drops each shard and removes the record. Both run to the end
/// before they answer, so a client usually never sees CREATING; the states
/// are still real (a create interrupted between steps leaves CREATING, which
/// SDK waiters see until a retried CreateStream or the maintenance pass
/// finishes it, and a retried DeleteStream finishes the job).
///
/// Writes. A record's hash is its `ExplicitHashKey` or the MD5 of its
/// partition key (Requirement 7.1); the open shard owning the hash takes it.
/// PutRecords proposes one entry per shard with that shard's records in
/// request order, and answers per record (7.3): records of a shard whose
/// entry could not be committed fail with InternalFailure, the rest succeed.
/// An append that finds its shard closed (a reshard raced it) re-reads the
/// stream and retries. Sequence numbers come from the shard's counter
/// (8.1, 8.2), printed in the stream's id format.
///
/// Reads. GetShardIterator signs the position into a stateless token
/// (`kinesis_iterators.hpp`); GetRecords checks it, authorizes
/// `kinesis:GetRecords` on the token's stream, and reads the shard from
/// this replica's segment store (`kinesis_records.hpp`), so any node
/// answers. An iterator's expiry is the stream's `iterator-lifetime` switch
/// at read time: five minutes, or a day relaxed. A closed shard read to its
/// end answers no NextShardIterator and its ChildShards; closing shards
/// comes with resharding (task 11).
///
/// Reads of the catalog are served by the local replica, so a describe right
/// after a write on another node may briefly show the old state, as AWS's
/// describes are eventually consistent too.
///
/// Not here yet: naming a stream by `StreamId` and `DryRun` (refused with
/// InvalidArgumentException rather than silently ignored), and the
/// per-stream maximum record size (`MaxRecordSizeInKiB`, AWS's default of
/// 1 MiB applies).
///
/// Throughput (task 8, `kinesis_throttle.hpp`). With the stream's
/// `throttling` switch on (the default), appends are proposed with
/// `admission::shard_limits`: the leader admits the longest prefix of each
/// shard's records its buckets allow, and the rest fail with
/// ProvisionedThroughputExceededException (the whole PutRecord, or each
/// record of a PutRecords). GetRecords and GetShardIterator meet this
/// node's read buckets.

#include <kythira/kinesis/kinesis_catalog.hpp>
#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_compat_profile.hpp>
#include <kythira/kinesis/kinesis_errors.hpp>
#include <kythira/kinesis/kinesis_front_end.hpp>
#include <kythira/kinesis/kinesis_hash.hpp>
#include <kythira/kinesis/kinesis_iterators.hpp>
#include <kythira/kinesis/kinesis_kms.hpp>
#include <kythira/kinesis/kinesis_metrics.hpp>
#include <kythira/kinesis/kinesis_records.hpp>
#include <kythira/kinesis/kinesis_sequence.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::kinesis {

/// AWS's account-level limits (DescribeLimits), and the per-record ones.
struct kinesis_limits {
    /// Open shards per account and region: 500 in us-east-1, us-west-2 and
    /// eu-west-1, 200 elsewhere (AWS's default quota).
    std::uint32_t _shard_limit = 500;
    std::uint32_t _on_demand_stream_limit = 50;
    /// An on-demand stream's shards at creation.
    std::uint32_t _on_demand_initial_shards = 4;
    /// Data plus partition key, per record.
    std::size_t _max_record_bytes = std::size_t{1024} * 1024;
    /// A PutRecords request's records, data plus partition keys.
    std::size_t _max_put_records_bytes = std::size_t{5} * 1024 * 1024;
    /// A GetRecords answer's record bytes (AWS: 10 MiB), and the bound
    /// with the `limits` switch relaxed.
    std::uint64_t _max_get_records_bytes = std::uint64_t{10} * 1024 * 1024;
    std::uint64_t _relaxed_get_records_bytes = std::uint64_t{64} * 1024 * 1024;

    /// AWS's quota for `region`.
    [[nodiscard]] static auto for_region(std::string_view region) -> kinesis_limits {
        kinesis_limits l;
        const bool big = region == "us-east-1" || region == "us-west-2" || region == "eu-west-1";
        l._shard_limit = big ? 500 : 200;
        return l;
    }
};

struct kinesis_service_config {
    kinesis_compat_profile _cluster_profile{};
    kinesis_limits _limits{};
    /// Appends that find their shard closed (resharded since this replica's
    /// catalog was read) are retried this many times, the first at once and
    /// then after `_closed_shard_backoff`, doubling, while the catalog
    /// replica catches up with the reshard.
    int _closed_shard_retries = 8;
    std::chrono::milliseconds _closed_shard_backoff{10};
    /// Signs and checks shard iterators; every gateway of a cluster needs
    /// the same ring. Empty: the service makes a random key, which suits
    /// one node only (iterators from other nodes would not verify).
    iterator_key_ring _iterator_keys{};
    /// AWS's per-shard rates (task 8). Writes meet them on the proposing
    /// leader (the cluster's `write_throttle`); reads on this node.
    throughput_limits _throughput{};
    /// The clock the read buckets refill by.
    monotonic_ms _monotonic = steady_now_ms;
    /// Wraps and unwraps encrypted streams' data keys (task 14). Empty:
    /// `StartStreamEncryption` is refused, and an encrypted stream's
    /// records cannot be written or read on this node.
    key_provider _kms{};
    /// SubscribeToShard (task 13): AWS's five minutes per subscription,
    /// 2 MiB/s per consumer and shard, and an event at least every five
    /// seconds; how often an idle subscription looks for new records and
    /// checks that its consumer is still registered.
    std::chrono::milliseconds _subscription_lifetime{300'000};
    std::uint64_t _subscription_bytes_per_s = std::uint64_t{2} * 1024 * 1024;
    std::chrono::milliseconds _subscription_heartbeat{5000};
    std::chrono::milliseconds _subscription_poll{100};
    std::chrono::milliseconds _subscription_recheck{1000};
};

/// `shardId-` and the ordinal: twelve digits in the `aws` id format, bare
/// in `compact`.
[[nodiscard]] inline auto format_shard_id(std::uint32_t ordinal, sequence_profile profile)
    -> std::string {
    return profile == sequence_profile::compact ? "shardId-" + std::to_string(ordinal)
                                                : shard_id_of(ordinal);
}

namespace service_detail {

[[nodiscard]] inline auto not_found(std::string_view name, std::string_view account)
    -> aws_json::error {
    return resource_not_found("Stream " + std::string(name) + " under account " +
                              std::string(account) + " not found.");
}

[[nodiscard]] inline auto in_use(std::string message) -> aws_json::error {
    return kinesis_error_style().modeled("ResourceInUseException", std::move(message));
}

[[nodiscard]] inline auto limit_exceeded(std::string message) -> aws_json::error {
    return kinesis_error_style().modeled("LimitExceededException", std::move(message));
}

[[nodiscard]] inline auto epoch_seconds(std::int64_t ms) -> double {
    return static_cast<double>(ms) / 1000.0;
}

[[nodiscard]] inline auto int_member(const boost::json::object& o, std::string_view key)
    -> std::optional<std::int64_t> {
    if (const auto* v = o.if_contains(key)) {
        if (v->is_int64()) {
            return v->get_int64();
        }
        if (v->is_uint64()) {
            return static_cast<std::int64_t>(v->get_uint64());
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline auto number_member(const boost::json::object& o, std::string_view key)
    -> std::optional<double> {
    if (const auto* v = o.if_contains(key); v != nullptr && v->is_number()) {
        return v->to_number<double>();
    }
    return std::nullopt;
}

[[nodiscard]] inline auto bool_member(const boost::json::object& o, std::string_view key) -> bool {
    const auto* v = o.if_contains(key);
    return v != nullptr && v->is_bool() && v->get_bool();
}

/// Base64 for the opaque paging tokens (and the decoder for `Data`).
[[nodiscard]] inline auto base64_encode(std::string_view in) -> std::string {
    static constexpr std::string_view k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const auto n = (static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16U) |
                       (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8U) |
                       static_cast<unsigned>(static_cast<unsigned char>(in[i + 2]));
        out += k[(n >> 18U) & 63U];
        out += k[(n >> 12U) & 63U];
        out += k[(n >> 6U) & 63U];
        out += k[n & 63U];
    }
    if (i < in.size()) {
        auto n = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16U;
        if (i + 1 < in.size()) {
            n |= static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8U;
        }
        out += k[(n >> 18U) & 63U];
        out += k[(n >> 12U) & 63U];
        out += i + 1 < in.size() ? k[(n >> 6U) & 63U] : '=';
        out += '=';
    }
    return out;
}

[[nodiscard]] inline auto base64_decode(std::string_view in) -> std::optional<std::string> {
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    if (in.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        std::array<int, 4> v{};
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = in[i + static_cast<std::size_t>(j)];
            auto& slot = v.at(static_cast<std::size_t>(j));
            if (c == '=' && i + 4 == in.size() && j >= 2) {
                slot = 0;
                ++pad;
                continue;
            }
            slot = value(c);
            if (pad > 0 || slot < 0) {
                return std::nullopt;
            }
        }
        const auto n = (static_cast<unsigned>(v[0]) << 18U) | (static_cast<unsigned>(v[1]) << 12U) |
                       (static_cast<unsigned>(v[2]) << 6U) | static_cast<unsigned>(v[3]);
        out += static_cast<char>((n >> 16U) & 0xFFU);
        if (pad < 2) {
            out += static_cast<char>((n >> 8U) & 0xFFU);
        }
        if (pad < 1) {
            out += static_cast<char>(n & 0xFFU);
        }
    }
    return out;
}

/// One record of a PutRecord or PutRecords request, decoded.
struct pending_record {
    std::size_t _index = 0;
    std::string _partition_key;
    std::vector<std::byte> _data;
    hash_key _hash = 0;
};

}  // namespace service_detail

template<stream_cluster Cluster> class kinesis_service {
public:
    kinesis_service(Cluster& cluster, kinesis_service_config config = {})
        : _cluster(cluster),
          _config(std::move(config)),
          _reads(_config._throughput, _config._monotonic) {
        if (_config._iterator_keys._keys.empty()) {
            std::array<unsigned char, 32> key{};
            if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
                throw std::runtime_error("kinesis: no randomness for the iterator key");
            }
            _config._iterator_keys._keys.push_back(
                {._id = 0, ._secret = std::string(key.begin(), key.end())});
        }
    }

    /// Registers this service's operations with `fe`.
    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        const auto bind = [this](auto member) {
            return [this, member](call_context& ctx, boost::json::object& in) {
                return (this->*member)(ctx, in);
            };
        };
        fe.on("CreateStream", bind(&kinesis_service::create_stream));
        fe.on("DeleteStream", bind(&kinesis_service::delete_stream));
        fe.on("DescribeStream", bind(&kinesis_service::describe_stream));
        fe.on("DescribeStreamSummary", bind(&kinesis_service::describe_stream_summary));
        fe.on("ListStreams", bind(&kinesis_service::list_streams));
        fe.on("ListShards", bind(&kinesis_service::list_shards));
        fe.on("DescribeLimits", bind(&kinesis_service::describe_limits));
        fe.on("PutRecord", bind(&kinesis_service::put_record));
        fe.on("PutRecords", bind(&kinesis_service::put_records));
        fe.on("GetShardIterator", bind(&kinesis_service::get_shard_iterator));
        fe.on("GetRecords", bind(&kinesis_service::get_records));
        fe.on("IncreaseStreamRetentionPeriod",
              bind(&kinesis_service::increase_stream_retention_period));
        fe.on("DecreaseStreamRetentionPeriod",
              bind(&kinesis_service::decrease_stream_retention_period));
        fe.on("UpdateStreamMode", bind(&kinesis_service::update_stream_mode));
        fe.on("AddTagsToStream", bind(&kinesis_service::add_tags_to_stream));
        fe.on("RemoveTagsFromStream", bind(&kinesis_service::remove_tags_from_stream));
        fe.on("ListTagsForStream", bind(&kinesis_service::list_tags_for_stream));
        fe.on("TagResource", bind(&kinesis_service::tag_resource));
        fe.on("UntagResource", bind(&kinesis_service::untag_resource));
        fe.on("ListTagsForResource", bind(&kinesis_service::list_tags_for_resource));
        fe.on("PutResourcePolicy", bind(&kinesis_service::put_resource_policy));
        fe.on("GetResourcePolicy", bind(&kinesis_service::get_resource_policy));
        fe.on("DeleteResourcePolicy", bind(&kinesis_service::delete_resource_policy));
        fe.on("EnableEnhancedMonitoring", bind(&kinesis_service::enable_enhanced_monitoring));
        fe.on("DisableEnhancedMonitoring", bind(&kinesis_service::disable_enhanced_monitoring));
        fe.on("DescribeAccountSettings", bind(&kinesis_service::describe_account_settings));
        fe.on("UpdateAccountSettings", bind(&kinesis_service::update_account_settings));
        fe.on("UpdateMaxRecordSize", bind(&kinesis_service::update_max_record_size));
        fe.on("StartStreamEncryption", bind(&kinesis_service::start_stream_encryption));
        fe.on("StopStreamEncryption", bind(&kinesis_service::stop_stream_encryption));
        fe.on("RegisterStreamConsumer", bind(&kinesis_service::register_stream_consumer));
        fe.on("DeregisterStreamConsumer", bind(&kinesis_service::deregister_stream_consumer));
        fe.on("DescribeStreamConsumer", bind(&kinesis_service::describe_stream_consumer));
        fe.on("ListStreamConsumers", bind(&kinesis_service::list_stream_consumers));
        fe.on("SubscribeToShard", bind(&kinesis_service::subscribe_to_shard));
        if constexpr (transacts<Cluster>) {
            fe.on("SplitShard", bind(&kinesis_service::split_shard));
            fe.on("MergeShards", bind(&kinesis_service::merge_shards));
            fe.on("UpdateShardCount", bind(&kinesis_service::update_shard_count));
        }
    }

    /// The operations `register_with` adds, for implemented-operations.txt.
    static constexpr std::array<std::string_view, 38> k_operations{"AddTagsToStream",
                                                                   "CreateStream",
                                                                   "DecreaseStreamRetentionPeriod",
                                                                   "DeleteResourcePolicy",
                                                                   "DeleteStream",
                                                                   "DeregisterStreamConsumer",
                                                                   "DescribeAccountSettings",
                                                                   "DescribeLimits",
                                                                   "DescribeStream",
                                                                   "DescribeStreamConsumer",
                                                                   "DescribeStreamSummary",
                                                                   "DisableEnhancedMonitoring",
                                                                   "EnableEnhancedMonitoring",
                                                                   "GetRecords",
                                                                   "GetResourcePolicy",
                                                                   "GetShardIterator",
                                                                   "IncreaseStreamRetentionPeriod",
                                                                   "ListShards",
                                                                   "ListStreamConsumers",
                                                                   "ListStreams",
                                                                   "ListTagsForResource",
                                                                   "ListTagsForStream",
                                                                   "MergeShards",
                                                                   "PutRecord",
                                                                   "PutRecords",
                                                                   "PutResourcePolicy",
                                                                   "RegisterStreamConsumer",
                                                                   "RemoveTagsFromStream",
                                                                   "SplitShard",
                                                                   "StartStreamEncryption",
                                                                   "StopStreamEncryption",
                                                                   "SubscribeToShard",
                                                                   "TagResource",
                                                                   "UntagResource",
                                                                   "UpdateAccountSettings",
                                                                   "UpdateMaxRecordSize",
                                                                   "UpdateShardCount",
                                                                   "UpdateStreamMode"};

    using result = aws_json::result<boost::json::object>;

    // ---- lifecycle --------------------------------------------------------

    auto create_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const auto& account = ctx._principal.account;
        const std::string name(*detail::string_member(in, "StreamName"));
        auto mode = stream_mode::provisioned;
        if (const auto* d = in.if_contains("StreamModeDetails"); d != nullptr && d->is_object()) {
            if (auto m = detail::string_member(d->get_object(), "StreamMode");
                m && *m == "ON_DEMAND") {
                mode = stream_mode::on_demand;
            }
        }
        auto shards = service_detail::int_member(in, "ShardCount");
        if (mode == stream_mode::provisioned && !shards) {
            return invalid_argument("ShardCount is required for PROVISIONED streams.");
        }
        const auto count =
            static_cast<std::uint32_t>(shards.value_or(_config._limits._on_demand_initial_shards));

        std::map<std::string, std::string> tags;
        if (const auto* t = in.if_contains("Tags"); t != nullptr && t->is_object()) {
            std::vector<std::pair<std::string, std::string>> list;
            for (const auto& kv : t->get_object()) {
                const std::string value =
                    kv.value().is_string() ? std::string(kv.value().get_string()) : std::string();
                list.emplace_back(kv.key(), value);
                tags.emplace(kv.key(), value);
            }
            if (auto bad = check_compat_tags(list)) {
                return *bad;
            }
        }
        const auto profile = resolve_profile(_config._cluster_profile, {tags.begin(), tags.end()});

        // Account limits, from this replica's catalog.
        if (profile._aws_count_limits) {
            std::size_t open = 0;
            std::size_t on_demand = 0;
            if (!read_catalog([&](const auto& sm) {
                    for (const auto* s : sm.list_streams(account)) {
                        open += s->open_shard_count();
                        on_demand += s->_mode == stream_mode::on_demand ? 1 : 0;
                    }
                })) {
                return internal_failure();
            }
            if (open + count > _config._limits._shard_limit) {
                return service_detail::limit_exceeded(
                    "This request would exceed the shard limit for the account " + account +
                    " in " + ctx._region +
                    ". Current shard count for the account: " + std::to_string(open) +
                    ". Limit: " + std::to_string(_config._limits._shard_limit) +
                    ". Number of additional shards that would have resulted from this request: " +
                    std::to_string(count) +
                    ". Refer to the AWS Service Limits page "
                    "(http://docs.aws.amazon.com/general/latest/gr/aws_service_limits.html) for "
                    "current limits and how to request higher limits.");
            }
            if (mode == stream_mode::on_demand &&
                on_demand + 1 > _config._limits._on_demand_stream_limit) {
                return service_detail::limit_exceeded(
                    "This request would exceed the on-demand stream limit for the account " +
                    account + " in " + ctx._region +
                    ". Limit: " + std::to_string(_config._limits._on_demand_stream_limit) + ".");
            }
        }

        create_stream_command c;
        c._account = account;
        c._name = name;
        c._shard_count = count;
        c._mode = mode;
        c._created_ms = _cluster.now_ms();
        c._tags = std::move(tags);
        const auto created = propose(c);
        if (!created) {
            return internal_failure();
        }
        if (created->_status == apply_status::already_exists) {
            // A retry of a create interrupted after the catalog commit (the
            // SDKs retry InternalFailure) finds its own stream in CREATING:
            // finish it, so the stream does not wait for the maintenance
            // pass, and answer as AWS does for an existing stream.
            (void)finish_create(account, name, std::nullopt);
            return service_detail::in_use("Stream " + name + " under account " + account +
                                          " already exists.");
        }
        if (created->_status != apply_status::ok) {
            return internal_failure();
        }
        if (!finish_create(account, name, created->_id)) {
            // Left in CREATING; a retry of CreateStream or the maintenance
            // pass (`repair_creates`) finishes it, and DeleteStream clears
            // it.
            return internal_failure();
        }
        return boost::json::object{};
    }

    /// The maintenance pass's half of CreateStream: a stream CREATING for
    /// longer than `stale_ms` (seen by this node's earlier calls; a live
    /// create takes a few commit round trips) lost its caller between the
    /// catalog commit and ACTIVE, so open its shards and make it ACTIVE.
    /// Without this a create interrupted by a leader change stays CREATING
    /// forever, and every write to it is answered not found. Returns how
    /// many streams it finished.
    auto repair_creates(std::int64_t now_ms, std::int64_t stale_ms) -> std::size_t {
        std::vector<stream_record> streams;
        (void)read_catalog([&](const auto& sm) {
            const auto prefix = streams_prefix();
            const auto& entries = sm.entries();
            for (auto it = entries.lower_bound(prefix);
                 it != entries.end() && it->first.starts_with(prefix); ++it) {
                if (const auto* r = std::get_if<stream_record>(&it->second);
                    r != nullptr && r->_status == stream_status::creating) {
                    streams.push_back(*r);
                }
            }
        });
        std::size_t repaired = 0;
        const std::scoped_lock lock(_creating_mutex);
        std::map<std::uint64_t, std::int64_t> still;
        for (const auto& rec : streams) {
            const auto seen = _creating_since.find(rec._id);
            const auto since = seen == _creating_since.end() ? now_ms : seen->second;
            if (now_ms - since < stale_ms || !finish_create(rec._account, rec._name, rec._id)) {
                still[rec._id] = since;
                continue;
            }
            ++repaired;
        }
        _creating_since = std::move(still);
        return repaired;
    }

    auto delete_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (!rec._consumers.empty() &&
            !service_detail::bool_member(in, "EnforceConsumerDeletion")) {
            return service_detail::in_use("Stream " + rec._name + " under account " + rec._account +
                                          " has registered consumers. Deregister them first, or "
                                          "set EnforceConsumerDeletion.");
        }
        if (rec._status != stream_status::deleting) {
            const auto r = propose(set_stream_status_command{rec._account, rec._name, rec._id,
                                                             stream_status::deleting});
            if (!r) {
                return internal_failure();
            }
            if (r->_status == apply_status::not_found) {
                return service_detail::not_found(rec._name, rec._account);
            }
            if (r->_status != apply_status::ok) {
                return internal_failure();
            }
        }
        for (const auto& shard : rec._shards) {
            const auto r = propose(drop_shard_command{routing_key(rec._id, shard._ordinal)});
            if (!r || (r->_status != apply_status::ok && r->_status != apply_status::not_found)) {
                return internal_failure();
            }
        }
        const auto removed = propose(remove_stream_command{rec._account, rec._name, rec._id});
        if (!removed ||
            (removed->_status != apply_status::ok && removed->_status != apply_status::not_found)) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    // ---- retention (task 10) ----------------------------------------------

    auto increase_stream_retention_period(call_context& ctx, boost::json::object& in) -> result {
        return set_retention(ctx, in, true);
    }

    auto decrease_stream_retention_period(call_context& ctx, boost::json::object& in) -> result {
        return set_retention(ctx, in, false);
    }

    // AWS's wording for each refusal (Requirement 11.2). The retention
    // change itself is one catalog entry; the leader's maintenance pass
    // trims to the new period on its next round (kinesis_maintenance.hpp).
    auto set_retention(call_context& ctx, boost::json::object& in, bool increase) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const auto hours = service_detail::int_member(in, "RetentionPeriodHours");
        if (!hours) {
            return invalid_argument("RetentionPeriodHours is required.");
        }
        if (*hours < k_min_retention_hours) {
            return invalid_argument("Minimum allowed retention period is " +
                                    std::to_string(k_min_retention_hours) +
                                    " hours. Requested retention period (" +
                                    std::to_string(*hours) + " hours) is too short.");
        }
        if (*hours > k_max_retention_hours) {
            return invalid_argument("Maximum allowed retention period is " +
                                    std::to_string(k_max_retention_hours) +
                                    " hours. Requested retention period (" +
                                    std::to_string(*hours) + " hours) is too long.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return service_detail::in_use("Stream " + rec._name + " under account " + rec._account +
                                          " not ACTIVE, instead in state " +
                                          std::string(to_string(rec._status)));
        }
        const auto wanted = static_cast<std::uint32_t>(*hours);
        if (increase && wanted < rec._retention_hours) {
            return invalid_argument(
                "Requested retention period (" + std::to_string(wanted) + " hours) for stream " +
                rec._name + " can not be shorter than existing retention period (" +
                std::to_string(rec._retention_hours) + " hours). Use DecreaseRetentionPeriod API.");
        }
        if (!increase && wanted > rec._retention_hours) {
            return invalid_argument(
                "Requested retention period (" + std::to_string(wanted) + " hours) for stream " +
                rec._name + " can not be longer than existing retention period (" +
                std::to_string(rec._retention_hours) + " hours). Use IncreaseRetentionPeriod API.");
        }
        if (wanted == rec._retention_hours) {
            return boost::json::object{};
        }
        const auto r = propose(set_retention_command{rec._account, rec._name, rec._id, wanted});
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return service_detail::not_found(rec._name, rec._account);
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    // ---- tags, policies, monitoring, account (task 15) -----------------------

    /// AWS's limit on a stream's tags.
    static constexpr std::size_t k_max_tags = 50;

    auto add_tags_to_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        return change_tags(*s, tag_map_of(in), {});
    }

    auto remove_tags_from_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        return change_tags(*s, {}, tag_keys_of(in));
    }

    /// Tags in key order, from after `ExclusiveStartTagKey`, `Limit` of them
    /// (10 when absent).
    auto list_tags_for_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto start = detail::string_member(in, "ExclusiveStartTagKey");
        const auto limit =
            static_cast<std::size_t>(service_detail::int_member(in, "Limit").value_or(10));
        boost::json::array tags;
        bool more = false;
        for (auto it = start ? s->_tags.upper_bound(std::string(*start)) : s->_tags.begin();
             it != s->_tags.end(); ++it) {
            if (tags.size() == limit) {
                more = true;
                break;
            }
            tags.push_back(boost::json::object{{"Key", it->first}, {"Value", it->second}});
        }
        boost::json::object out;
        out["Tags"] = std::move(tags);
        out["HasMoreTags"] = more;
        return out;
    }

    auto tag_resource(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::tag);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        return change_tags(*s, tag_map_of(in), {});
    }

    auto untag_resource(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::untag);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        return change_tags(*s, {}, tag_keys_of(in));
    }

    auto list_tags_for_resource(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::list_tags);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        boost::json::array tags;
        for (const auto& [k, v] : s->_tags) {
            tags.push_back(boost::json::object{{"Key", k}, {"Value", v}});
        }
        boost::json::object out;
        out["Tags"] = std::move(tags);
        return out;
    }

    /// AWS's limit on a resource policy's size.
    static constexpr std::size_t k_max_policy_bytes = 20 * 1024;

    /// The policy is stored as given and evaluated by the authorizer (IAM's
    /// evaluator) on every later request to the stream. Here it only has to
    /// be a JSON object with a `Statement`.
    auto put_resource_policy(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const std::string policy(*detail::string_member(in, "Policy"));
        if (policy.size() > k_max_policy_bytes) {
            return service_detail::limit_exceeded(
                "The resource policy exceeds the maximum size of " +
                std::to_string(k_max_policy_bytes) + " bytes.");
        }
        boost::system::error_code ec;
        const auto doc = boost::json::parse(policy, ec);
        if (ec || !doc.is_object() || !doc.as_object().contains("Statement")) {
            return invalid_argument("The resource policy is not a valid JSON policy document.");
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::put_policy);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        return set_policy(*s, policy);
    }

    /// The stored policy; `{}` when the stream has none.
    auto get_resource_policy(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::get_policy);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        boost::json::object out;
        out["Policy"] = s->_policy.value_or("{}");
        return out;
    }

    auto delete_resource_policy(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (is_consumer_arn(in)) {
            return consumer_resource(ctx, in, consumer_resource_op::delete_policy);
        }
        auto s = find_resource(ctx, in);
        if (!s) {
            return s.error();
        }
        if (!s->_policy) {
            return resource_not_found("No resource policy found for resource " +
                                      std::string(*detail::string_member(in, "ResourceARN")) + ".");
        }
        return set_policy(*s, std::nullopt);
    }

    auto enable_enhanced_monitoring(call_context& ctx, boost::json::object& in) -> result {
        return monitoring(ctx, in, true);
    }

    auto disable_enhanced_monitoring(call_context& ctx, boost::json::object& in) -> result {
        return monitoring(ctx, in, false);
    }

    /// Kythira has no billing commitment: the account setting is always
    /// DISABLED.
    auto describe_account_settings(call_context& /*ctx*/, boost::json::object& /*in*/) -> result {
        boost::json::object out;
        out["MinimumThroughputBillingCommitment"] = boost::json::object{{"Status", "DISABLED"}};
        return out;
    }

    auto update_account_settings(call_context& ctx, boost::json::object& in) -> result {
        const auto* c = in.if_contains("MinimumThroughputBillingCommitment");
        const auto status = c != nullptr && c->is_object()
                                ? detail::string_member(c->get_object(), "Status")
                                : std::nullopt;
        if (status && *status == "ENABLED") {
            return invalid_argument(
                "A minimum throughput billing commitment is not available on this endpoint.");
        }
        return describe_account_settings(ctx, in);
    }

    /// Records up to 1 MiB, the default, are all this endpoint stores
    /// (`refuse_unsupported` refuses any other size); setting it is a no-op
    /// on a stream that exists and is ACTIVE.
    auto update_max_record_size(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        if (s->_status != stream_status::active) {
            return not_active(*s);
        }
        return boost::json::object{};
    }

    // ---- encryption at rest (task 14) ---------------------------------------

    /// `StartStreamEncryption`: new records are sealed under a fresh data key
    /// wrapped by `KeyId`. On an ACTIVE stream; the change is one catalog
    /// entry, so the stream does not pass through UPDATING. Starting again
    /// with the key the stream already uses changes nothing; another key
    /// re-keys it (records already written keep their old data key).
    auto start_stream_encryption(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const auto type = detail::string_member(in, "EncryptionType");
        const std::string key(detail::string_member(in, "KeyId").value_or(""));
        if (!type || *type != "KMS") {
            return invalid_argument("EncryptionType must be KMS.");
        }
        if (!_config._kms) {
            return invalid_argument("Encryption at rest is not configured on this endpoint.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return not_active(rec);
        }
        if (rec._encryption == encryption_type::kms && rec._key_id == key) {
            return boost::json::object{};
        }
        generated_data_key fresh;
        try {
            fresh = _config._kms.generate_data_key(key, encryption_context_of(rec));
        } catch (const kms_error& e) {
            return kms_error_answer(e);
        }
        const auto r = propose(set_encryption_command{
            ._account = rec._account,
            ._name = rec._name,
            ._id = rec._id,
            ._encryption = encryption_type::kms,
            ._key_id = key,
            ._data_key = data_key_record{._key_id = fresh._key_id, ._wrapped = fresh._wrapped}});
        if (r && r->_status == apply_status::ok) {
            const std::scoped_lock lock(_data_key_mutex);
            _data_key_cache.insert_or_assign(
                {rec._id, static_cast<std::uint32_t>(r->_first_counter)}, fresh._plaintext);
        }
        OPENSSL_cleanse(fresh._plaintext.data(), fresh._plaintext.size());
        return encryption_answer(rec, r);
    }

    // ---- enhanced fan-out consumers (task 13) -------------------------------

    /// `RegisterStreamConsumer`: at most 20 per stream, on an ACTIVE stream.
    /// The consumer can subscribe at once, so it is ACTIVE from the start
    /// (AWS shows CREATING for a few seconds; clients wait for ACTIVE).
    auto register_stream_consumer(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const std::string name(*detail::string_member(in, "ConsumerName"));
        auto tags = tag_map_of(in);
        if (tags.size() > k_max_tags) {
            return invalid_argument("A consumer can have at most " + std::to_string(k_max_tags) +
                                    " tags.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return not_active(rec);
        }
        const auto now = _cluster.now_ms();
        const consumer_record c{._name = name, ._created_ms = now, ._tags = tags, ._policy = {}};
        if (const auto* old = rec.find_consumer(name)) {
            return consumer_exists(ctx, rec, *old);
        }
        const auto max_consumers = max_consumers_of(rec);
        if (rec._consumers.size() >= max_consumers) {
            return consumer_limit(rec, max_consumers);
        }
        const auto r = propose(consumer_command{._account = rec._account,
                                                ._name = rec._name,
                                                ._id = rec._id,
                                                ._op = consumer_op::register_consumer,
                                                ._consumer = name,
                                                ._created_ms = now,
                                                ._set = std::move(tags),
                                                ._remove = {},
                                                ._policy = std::nullopt,
                                                ._max = static_cast<std::uint32_t>(max_consumers)});
        if (!r) {
            return internal_failure();
        }
        switch (r->_status) {
            case apply_status::ok:
                break;
            case apply_status::not_found:
                return service_detail::not_found(rec._name, rec._account);
            case apply_status::bad_transition:
                return not_active(rec);
            case apply_status::already_exists:
                return consumer_exists(ctx, rec, c);
            case apply_status::invalid:
                return consumer_limit(rec, max_consumers);
            default:
                return internal_failure();
        }
        boost::json::object out;
        out["Consumer"] = consumer_json(ctx, rec, c, false);
        return out;
    }

    /// Ends the consumer's subscriptions too: each notices on its next pass.
    auto deregister_stream_consumer(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto found = find_consumer(ctx, in);
        if (!found) {
            return found.error();
        }
        return change_consumer(ctx, *found, consumer_op::deregister, {}, {}, std::nullopt);
    }

    auto describe_stream_consumer(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto found = find_consumer(ctx, in);
        if (!found) {
            return found.error();
        }
        boost::json::object out;
        out["ConsumerDescription"] = consumer_json(ctx, found->_stream, found->_consumer, true);
        return out;
    }

    /// By name, `MaxResults` at a time (100 when absent).
    auto list_stream_consumers(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (const auto t = service_detail::number_member(in, "StreamCreationTimestamp");
            t && static_cast<std::int64_t>(*t) != rec._created_ms / 1000) {
            return service_detail::not_found(rec._name, rec._account);
        }
        const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(
            service_detail::int_member(in, "MaxResults").value_or(100), 1, 10'000));
        std::string after;
        if (const auto token = detail::string_member(in, "NextToken")) {
            const auto decoded = service_detail::base64_decode(*token);
            const auto prefix = consumers_token_prefix(rec);
            if (!decoded || !decoded->starts_with(prefix)) {
                return kinesis_error_style().modeled("ExpiredNextTokenException",
                                                     "The NextToken is invalid or has expired.");
            }
            after = decoded->substr(prefix.size());
        }
        std::vector<const consumer_record*> sorted;
        for (const auto& c : rec._consumers) {
            if (after.empty() || c._name > after) {
                sorted.push_back(&c);
            }
        }
        std::ranges::sort(sorted, {}, &consumer_record::_name);
        boost::json::array list;
        for (std::size_t i = 0; i < sorted.size() && i < limit; ++i) {
            list.push_back(consumer_json(ctx, rec, *sorted[i], false));
        }
        boost::json::object out;
        out["Consumers"] = std::move(list);
        if (sorted.size() > limit) {
            out["NextToken"] = service_detail::base64_encode(consumers_token_prefix(rec) +
                                                             sorted[limit - 1]->_name);
        }
        return out;
    }

    /// `SubscribeToShard`: the answer is the initial response, and the
    /// subscription's events follow it through `ctx._events` (see
    /// `subscription`). One subscription per consumer and shard on this
    /// node at a time.
    auto subscribe_to_shard(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const std::string arn(*detail::string_member(in, "ConsumerARN"));
        auto found = consumer_by_arn(ctx, arn);
        if (!found) {
            return found.error();
        }
        const auto& rec = found->_stream;
        if (rec._status != stream_status::active && rec._status != stream_status::updating) {
            return not_active(rec);
        }
        const std::string shard_text(*detail::string_member(in, "ShardId"));
        const auto* entry = shard_named(rec, shard_text);
        if (entry == nullptr) {
            return shard_not_found(shard_text, rec);
        }
        const auto& position = in.at("StartingPosition").as_object();
        const auto state = read_shard_state(routing_key(rec._id, entry->_ordinal));
        if (!state || !state->_found) {
            return internal_failure();
        }
        auto start =
            start_position(rec, *entry, state->_state, *detail::string_member(position, "Type"),
                           detail::string_member(position, "SequenceNumber"),
                           service_detail::number_member(position, "Timestamp"), "SequenceNumber",
                           "SubscribeToShard");
        if (!start) {
            return start.error();
        }
        const auto key = arn + "|" + std::to_string(entry->_ordinal);
        std::shared_ptr<subscription> sub;
        {
            const std::scoped_lock lock(_subscriptions_mutex);
            if (const auto it = _subscriptions.find(key);
                it != _subscriptions.end() && !it->second._sub.expired()) {
                return service_detail::in_use("Another active subscription exists for consumer " +
                                              arn + " on shard " + shard_text + ".");
            }
            sub = std::make_shared<subscription>(*this, key, ctx._encoding, found->_stream,
                                                 found->_consumer, entry->_ordinal, start->first,
                                                 start->second);
            _subscriptions.insert_or_assign(key, subscription_slot{sub, rec._account, rec._name});
        }
        ctx._events = sub;
        return boost::json::object{};
    }

    /// `StopStreamEncryption`: new records are stored in the clear again.
    /// Records sealed before stay sealed and readable. Stopping a stream
    /// that is not encrypted changes nothing; `KeyId` must name the key the
    /// stream uses.
    auto stop_stream_encryption(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const auto type = detail::string_member(in, "EncryptionType");
        const std::string key(detail::string_member(in, "KeyId").value_or(""));
        if (!type || *type != "KMS") {
            return invalid_argument("EncryptionType must be KMS.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return not_active(rec);
        }
        if (rec._encryption != encryption_type::kms) {
            return boost::json::object{};
        }
        if (rec._key_id != key) {
            return invalid_argument("KeyId " + key + " is not the key stream " + rec._name +
                                    " under account " + rec._account + " is encrypted with.");
        }
        const auto r = propose(set_encryption_command{._account = rec._account,
                                                      ._name = rec._name,
                                                      ._id = rec._id,
                                                      ._encryption = encryption_type::none,
                                                      ._key_id = {},
                                                      ._data_key = std::nullopt});
        return encryption_answer(rec, r);
    }

    /// What the front end's authorization needs about `arn`: the stream's
    /// tags and resource policy. Nullopt for anything but an existing
    /// stream (the daemon's `resource_lookup`).
    auto resource_facts_of(std::string_view arn) -> std::optional<resource_facts> {
        if (arn.find("/consumer/") != std::string_view::npos) {
            // A consumer's own tags and policy (task 13).
            call_context any;
            if (const auto p = parse_kinesis_arn(arn)) {
                any._partition = std::string(p->_partition);
                any._region = std::string(p->_region);
            }
            auto found = consumer_by_arn(any, arn);
            if (!found) {
                return std::nullopt;
            }
            return resource_facts{
                ._tags = {found->_consumer._tags.begin(), found->_consumer._tags.end()},
                ._policy = found->_consumer._policy};
        }
        const auto parts = parse_kinesis_arn(arn);
        const auto name = stream_name_of(arn);
        if (!parts || !name) {
            return std::nullopt;
        }
        std::optional<resource_facts> out;
        (void)read_catalog([&](const auto& sm) {
            if (const auto* rec =
                    sm.find_stream(std::string(parts->_account), std::string(*name))) {
                out = resource_facts{._tags = {rec->_tags.begin(), rec->_tags.end()},
                                     ._policy = rec->_policy};
            }
        });
        return out;
    }

    // ---- capacity mode (task 12) ---------------------------------------------

    /// `UpdateStreamMode`: PROVISIONED or ON_DEMAND, on an ACTIVE stream, at
    /// most twice in 24 hours. The shards stay as they are; in ON_DEMAND
    /// the capacity controller (`kinesis_on_demand.hpp`) splits them as
    /// writes grow. `WarmThroughputMiBps` is refused: there is no billing
    /// commitment to unlock it.
    auto update_stream_mode(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        if (in.contains("WarmThroughputMiBps")) {
            return invalid_argument("WarmThroughputMiBps is not supported by this endpoint.");
        }
        std::optional<stream_mode> mode;
        if (const auto* d = in.if_contains("StreamModeDetails"); d != nullptr && d->is_object()) {
            if (auto m = detail::string_member(d->get_object(), "StreamMode")) {
                if (*m == "ON_DEMAND") {
                    mode = stream_mode::on_demand;
                } else if (*m == "PROVISIONED") {
                    mode = stream_mode::provisioned;
                }
            }
        }
        if (!mode) {
            return invalid_argument(
                "StreamModeDetails.StreamMode must be PROVISIONED or ON_DEMAND.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return not_active(rec);
        }
        if (rec._mode == *mode) {
            return boost::json::object{};
        }
        const auto now = _cluster.now_ms();
        constexpr std::int64_t k_day_ms = std::int64_t{24} * 3600 * 1000;
        if (rec._mode_changes_ms.size() >= 2 && now - rec._mode_changes_ms.front() < k_day_ms) {
            return service_detail::limit_exceeded(
                "Stream " + rec._name + " under account " + rec._account +
                " has had its capacity mode updated twice in the last 24 hours.");
        }
        if (*mode == stream_mode::on_demand && profile_of(rec)._aws_count_limits) {
            std::size_t on_demand = 0;
            if (!read_catalog([&](const auto& sm) {
                    for (const auto* other : sm.list_streams(rec._account)) {
                        on_demand += other->_mode == stream_mode::on_demand ? 1 : 0;
                    }
                })) {
                return internal_failure();
            }
            if (on_demand + 1 > _config._limits._on_demand_stream_limit) {
                return service_detail::limit_exceeded(
                    "This request would exceed the on-demand stream limit for the account " +
                    rec._account + " in " + ctx._region +
                    ". Limit: " + std::to_string(_config._limits._on_demand_stream_limit) + ".");
            }
        }
        const auto r =
            propose(set_stream_mode_command{rec._account, rec._name, rec._id, *mode, now});
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return service_detail::not_found(rec._name, rec._account);
        }
        if (r->_status == apply_status::bad_transition) {
            return not_active(rec);
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    /// The capacity controller's split: shard `ordinal` of stream `id` at the
    /// middle of its hash range, while the stream is still ON_DEMAND and the
    /// shard still open. False when it did not happen (another reshard, the
    /// shard limit, or a failure); the controller tries again later.
    auto split_for_capacity(const std::string& account, const std::string& name, std::uint64_t id,
                            std::uint32_t ordinal) -> bool
    requires transacts<Cluster>
    {
        std::optional<stream_record> rec;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* found = sm.find_stream(account, name)) {
                    rec = *found;
                }
            }) ||
            !rec || rec->_id != id || rec->_status != stream_status::active) {
            return false;
        }
        const auto done =
            run_reshard(*rec, [&](const stream_record& r) -> aws_json::result<reshard_plan> {
                const auto* sh = r.find_shard(ordinal);
                if (r._mode != stream_mode::on_demand || sh == nullptr || !sh->open() ||
                    sh->_range._end - sh->_range._start < 2) {
                    return invalid_argument("no longer a capacity split");
                }
                if (profile_of(r)._aws_count_limits &&
                    r.open_shard_count() + 1 > _config._limits._shard_limit) {
                    return service_detail::limit_exceeded("shard limit");
                }
                const auto width = sh->_range._end - sh->_range._start;
                const auto mid = sh->_range._start + width / 2 + 1;
                reshard_plan plan;
                plan._closing.push_back(ordinal);
                plan._children.push_back(
                    child(r._next_ordinal, ordinal, std::nullopt, {sh->_range._start, mid - 1}));
                plan._children.push_back(
                    child(r._next_ordinal + 1, ordinal, std::nullopt, {mid, sh->_range._end}));
                return plan;
            });
        return done.has_value();
    }

    // ---- resharding (task 11) ------------------------------------------------

    /// One reshard: which shards close and which open.
    struct reshard_plan {
        std::vector<std::uint32_t> _closing;
        std::vector<shard_entry> _children;
    };

    auto split_shard(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const std::string shard_text(*detail::string_member(in, "ShardToSplit"));
        const std::string key_text(*detail::string_member(in, "NewStartingHashKey"));
        const auto new_start = parse_hash_key(key_text);
        if (!new_start) {
            return invalid_argument("NewStartingHashKey " + key_text + " is not a valid hash key.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        auto done =
            run_reshard(*s, [&](const stream_record& rec) -> aws_json::result<reshard_plan> {
                if (auto e = provisioned_only(rec, "SplitShard")) {
                    return *e;
                }
                auto parent = shard_to_reshard(rec, shard_text);
                if (!parent) {
                    return parent.error();
                }
                const auto& p = **parent;
                // AWS's rule: strictly inside the range, with room on each
                // side.
                if (*new_start <= p._range._start + 1 || *new_start >= p._range._end) {
                    return invalid_argument(
                        "NewStartingHashKey " + key_text + " used in SplitShard() on shard " +
                        shard_text + " in stream " + rec._name + " under account " + rec._account +
                        " is not both greater than one plus the shard's StartingHashKey " +
                        to_decimal(p._range._start) + " and less than the shard's EndingHashKey " +
                        to_decimal(p._range._end) + ".");
                }
                reshard_plan plan;
                plan._closing.push_back(p._ordinal);
                plan._children.push_back(child(rec._next_ordinal, p._ordinal, std::nullopt,
                                               {p._range._start, *new_start - 1}));
                plan._children.push_back(child(rec._next_ordinal + 1, p._ordinal, std::nullopt,
                                               {*new_start, p._range._end}));
                return plan;
            });
        if (!done) {
            return done.error();
        }
        return boost::json::object{};
    }

    auto merge_shards(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const std::string first_text(*detail::string_member(in, "ShardToMerge"));
        const std::string second_text(*detail::string_member(in, "AdjacentShardToMerge"));
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        auto done =
            run_reshard(*s, [&](const stream_record& rec) -> aws_json::result<reshard_plan> {
                if (auto e = provisioned_only(rec, "MergeShards")) {
                    return *e;
                }
                auto a = shard_to_reshard(rec, first_text);
                if (!a) {
                    return a.error();
                }
                auto b = shard_to_reshard(rec, second_text);
                if (!b) {
                    return b.error();
                }
                const auto& x = **a;
                const auto& y = **b;
                if (!adjacent(x._range, y._range)) {
                    return invalid_argument(
                        "Shards " + first_text + " and " + second_text + " in stream " + rec._name +
                        " under account " + rec._account +
                        " are not an adjacent pair of shards eligible for merging");
                }
                reshard_plan plan;
                plan._closing = {x._ordinal, y._ordinal};
                plan._children.push_back(child(rec._next_ordinal, x._ordinal, y._ordinal,
                                               {std::min(x._range._start, y._range._start),
                                                std::max(x._range._end, y._range._end)}));
                return plan;
            });
        if (!done) {
            return done.error();
        }
        return boost::json::object{};
    }

    /// UNIFORM_SCALING as a series of single reshards: split the widest open
    /// shard at its middle, or merge the narrowest adjacent pair, until the
    /// target count. AWS's limits apply: at most double and at least half
    /// the open shards, per call.
    auto update_shard_count(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        const auto target = service_detail::int_member(in, "TargetShardCount");
        const auto scaling = detail::string_member(in, "ScalingType");
        if (!scaling || *scaling != "UNIFORM_SCALING") {
            return invalid_argument("ScalingType must be UNIFORM_SCALING.");
        }
        if (!target || *target < 1) {
            return invalid_argument("TargetShardCount must be at least 1.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto current = static_cast<std::int64_t>(s->open_shard_count());
        if (*target > 2 * current) {
            return invalid_argument(
                "UpdateShardCount cannot scale up over double your current open shard count. "
                "Current open shard count: " +
                std::to_string(current) + " Target shard count: " + std::to_string(*target));
        }
        if (2 * *target < current) {
            return invalid_argument(
                "UpdateShardCount cannot scale down below half your current open shard count. "
                "Current open shard count: " +
                std::to_string(current) + " Target shard count: " + std::to_string(*target));
        }
        if (*target > static_cast<std::int64_t>(_config._limits._shard_limit)) {
            return service_detail::limit_exceeded(
                "This request would exceed the shard limit for the account " + s->_account +
                ". Current shard count: " + std::to_string(current) +
                ". Limit: " + std::to_string(_config._limits._shard_limit) + ".");
        }
        const auto wanted = static_cast<std::size_t>(*target);
        while (s->open_shard_count() != wanted) {
            auto done =
                run_reshard(*s, [&](const stream_record& rec) -> aws_json::result<reshard_plan> {
                    if (auto e = provisioned_only(rec, "UpdateShardCount")) {
                        return *e;
                    }
                    return uniform_step(rec, wanted);
                });
            if (!done) {
                return done.error();
            }
            // The next step plans from the table this one left, so wait
            // for this replica to show the stream ACTIVE again.
            auto fresh = record_once(s->_account, s->_name, s->_id, [](const stream_record& r) {
                return r._status == stream_status::active;
            });
            if (!fresh) {
                return internal_failure();
            }
            s = std::move(*fresh);
        }
        boost::json::object out;
        out["StreamName"] = s->_name;
        out["StreamARN"] = arn_of(ctx, *s);
        out["CurrentShardCount"] = current;
        out["TargetShardCount"] = *target;
        return out;
    }

    // ---- describes ----------------------------------------------------------

    auto describe_stream_summary(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        const auto profile = profile_of(rec);
        boost::json::object d = common_description(ctx, rec);
        d["OpenShardCount"] = rec.open_shard_count();
        d["ConsumerCount"] = 0;
        // As AWS answers today (task 17 recordings): the record size limit
        // and the channel count, and no StreamId, which the model lists but
        // AWS does not return.
        d["MaxRecordSizeInKiB"] = 1024;
        d["ChannelCount"] = 0;
        if (const auto relaxed = profile.relaxed(); !relaxed.empty()) {
            // Kythira's extension (Requirement 3.3): which AWS behaviours
            // this stream runs without. Absent when it runs as AWS.
            boost::json::array names;
            for (const auto n : relaxed) {
                names.emplace_back(n);
            }
            d["KythiraRelaxedCompatibility"] = std::move(names);
        }
        boost::json::object out;
        out["StreamDescriptionSummary"] = std::move(d);
        return out;
    }

    auto describe_stream(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        const auto profile = profile_of(rec);
        const auto limit =
            static_cast<std::size_t>(service_detail::int_member(in, "Limit").value_or(100));
        std::optional<std::uint32_t> after;
        if (auto start = detail::string_member(in, "ExclusiveStartShardId")) {
            after = parse_shard_id(*start);
        }
        boost::json::array shards;
        bool more = false;
        for (const auto& sh : sorted_shards(rec)) {
            if (after && sh._ordinal <= *after) {
                continue;
            }
            if (shards.size() == limit) {
                more = true;
                break;
            }
            shards.push_back(shard_json(sh, profile));
        }
        boost::json::object d = common_description(ctx, rec);
        d["Shards"] = std::move(shards);
        d["HasMoreShards"] = more;
        boost::json::object out;
        out["StreamDescription"] = std::move(d);
        return out;
    }

    auto list_streams(call_context& ctx, boost::json::object& in) -> result {
        const auto& account = ctx._principal.account;
        const auto limit =
            static_cast<std::size_t>(service_detail::int_member(in, "Limit").value_or(100));
        std::string start;
        if (auto token = detail::string_member(in, "NextToken")) {
            auto decoded = service_detail::base64_decode(*token);
            if (!decoded || !decoded->starts_with("ls1:")) {
                return invalid_argument("Invalid NextToken.");
            }
            start = decoded->substr(4);
        } else if (auto exclusive = detail::string_member(in, "ExclusiveStartStreamName")) {
            start = std::string(*exclusive);
        }
        std::vector<stream_record> page;
        bool more = false;
        if (!read_catalog([&](const auto& sm) {
                for (const auto* s : sm.list_streams(account, start, limit + 1)) {
                    page.push_back(*s);
                }
            })) {
            return internal_failure();
        }
        if (page.size() > limit) {
            page.resize(limit);
            more = true;
        }
        boost::json::array names;
        boost::json::array summaries;
        for (const auto& s : page) {
            names.emplace_back(s._name);
            boost::json::object summary;
            summary["StreamName"] = s._name;
            summary["StreamARN"] = arn_of(ctx, s);
            summary["StreamStatus"] = to_string(s._status);
            summary["StreamModeDetails"] = boost::json::object{{"StreamMode", to_string(s._mode)}};
            summary["StreamCreationTimestamp"] = service_detail::epoch_seconds(s._created_ms);
            summaries.push_back(std::move(summary));
        }
        boost::json::object out;
        out["StreamNames"] = std::move(names);
        out["HasMoreStreams"] = more;
        out["StreamSummaries"] = std::move(summaries);
        if (more) {
            out["NextToken"] = service_detail::base64_encode("ls1:" + page.back()._name);
        }
        return out;
    }

    auto list_shards(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        std::optional<std::uint32_t> after;
        std::string account = ctx._principal.account;
        std::string name;
        std::optional<std::int64_t> created;
        if (auto token = detail::string_member(in, "NextToken")) {
            if (ctx._stream || in.contains("ExclusiveStartShardId") ||
                in.contains("StreamCreationTimestamp") || in.contains("ShardFilter")) {
                return invalid_argument(
                    "NextToken and StreamName/StreamARN/ExclusiveStartShardId/"
                    "StreamCreationTimestamp/ShardFilter cannot be provided together.");
            }
            auto t = parse_shards_token(*token);
            if (!t) {
                return invalid_argument("NextToken is invalid.");
            }
            account = t->_account;
            name = t->_name;
            after = t->_after;
            created = t->_created_ms;
            // The token names the stream: authorize it now.
            aws_auth::authz_request r;
            r.action = "kinesis:ListShards";
            r.resource = "arn:" + ctx._partition + ":kinesis:" + ctx._region + ":" + account +
                         ":stream/" + name;
            if (auto denied = ctx.authorize(std::move(r))) {
                return *denied;
            }
        } else if (ctx._stream) {
            account = ctx._stream->_account;
            name = ctx._stream->_name;
            if (auto start = detail::string_member(in, "ExclusiveStartShardId")) {
                after = parse_shard_id(*start);
            }
        } else {
            return invalid_argument("Either NextToken or StreamName/StreamARN should be provided.");
        }

        std::optional<stream_record> found;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(account, name)) {
                    found = *s;
                }
            })) {
            return internal_failure();
        }
        if (!found || (created && *created != found->_created_ms)) {
            // A token for a deleted (or recreated) stream lists nothing.
            return service_detail::not_found(name, account);
        }
        const auto& rec = *found;
        const auto profile = profile_of(rec);

        // ShardFilter: AT_LATEST lists the open shards; AFTER_SHARD_ID those
        // after the given one; the trim-horizon and timestamp filters list
        // every shard still in the stream (no shard has expired yet: closed
        // shards stay until retention, task 10).
        bool open_only = false;
        if (const auto* f = in.if_contains("ShardFilter"); f != nullptr && f->is_object()) {
            const auto type = detail::string_member(f->get_object(), "Type").value_or("");
            if (type == "AFTER_SHARD_ID") {
                auto id = detail::string_member(f->get_object(), "ShardId");
                if (!id) {
                    return invalid_argument(
                        "ShardId must be provided when ShardFilter Type is AFTER_SHARD_ID.");
                }
                after = parse_shard_id(*id);
            } else if (type == "AT_LATEST") {
                open_only = true;
            } else if ((type == "AT_TIMESTAMP" || type == "FROM_TIMESTAMP") &&
                       !f->get_object().contains("Timestamp")) {
                return invalid_argument(
                    "Timestamp must be provided when ShardFilter Type is AT_TIMESTAMP or "
                    "FROM_TIMESTAMP.");
            }
        }

        const auto max =
            static_cast<std::size_t>(service_detail::int_member(in, "MaxResults").value_or(1000));
        boost::json::array shards;
        std::optional<std::uint32_t> last;
        bool more = false;
        for (const auto& sh : sorted_shards(rec)) {
            if ((after && sh._ordinal <= *after) || (open_only && !sh.open())) {
                continue;
            }
            if (shards.size() == max) {
                more = true;
                break;
            }
            shards.push_back(shard_json(sh, profile));
            last = sh._ordinal;
        }
        boost::json::object out;
        out["Shards"] = std::move(shards);
        if (more && last) {
            out["NextToken"] = make_shards_token(rec, *last);
        }
        return out;
    }

    auto describe_limits(call_context& ctx, boost::json::object& /*in*/) -> result {
        std::size_t open = 0;
        std::size_t on_demand = 0;
        if (!read_catalog([&](const auto& sm) {
                for (const auto* s : sm.list_streams(ctx._principal.account)) {
                    open += s->open_shard_count();
                    on_demand += s->_mode == stream_mode::on_demand ? 1 : 0;
                }
            })) {
            return internal_failure();
        }
        boost::json::object out;
        out["ShardLimit"] = _config._limits._shard_limit;
        out["OpenShardCount"] = open;
        out["OnDemandStreamCount"] = on_demand;
        out["OnDemandStreamCountLimit"] = _config._limits._on_demand_stream_limit;
        return out;
    }

    // ---- writes -------------------------------------------------------------

    auto put_record(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find_writable(ctx);
        if (!s) {
            return s.error();
        }
        if (auto seq = detail::string_member(in, "SequenceNumberForOrdering")) {
            // Honoured by construction (Requirement 7.2): a partition key's
            // records go to one shard, or to its descendants, whose ordinals
            // and so sequence numbers are higher; a shard's counter only
            // grows. What is left to check is that it is a sequence number.
            if (!parse_sequence(*seq, sequence_profile::aws) &&
                !parse_sequence(*seq, sequence_profile::compact)) {
                return invalid_argument("Invalid SequenceNumberForOrdering: " + std::string(*seq) +
                                        ".");
            }
        }
        auto rec = decode_record(in, 0);
        if (!rec) {
            return rec.error();
        }
        std::vector<service_detail::pending_record> batch;
        batch.push_back(std::move(*rec));
        auto results = append(*s, std::move(batch));
        if (!results) {
            return results.error();
        }
        const auto& r = results->at(0);
        if (r._throttled) {
            return throughput_exceeded(r._message);
        }
        if (r._storage_full) {
            return storage_full_error();
        }
        if (r._error) {
            return internal_failure();
        }
        boost::json::object out;
        out["ShardId"] = r._shard_id;
        out["SequenceNumber"] = r._sequence;
        // AWS answers EncryptionType only for an encrypted stream (seen in
        // the task 17 recordings): absent, not "NONE", otherwise.
        if (r._encrypted) {
            out["EncryptionType"] = "KMS";
        }
        return out;
    }

    auto put_records(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find_writable(ctx);
        if (!s) {
            return s.error();
        }
        const auto& list = in.at("Records").as_array();
        std::vector<service_detail::pending_record> batch;
        batch.reserve(list.size());
        std::size_t total = 0;
        for (std::size_t i = 0; i < list.size(); ++i) {
            auto rec = decode_record(list[i].as_object(), i);
            if (!rec) {
                return rec.error();
            }
            total += rec->_data.size() + rec->_partition_key.size();
            batch.push_back(std::move(*rec));
        }
        if (profile_of(*s)._aws_limits && total > _config._limits._max_put_records_bytes) {
            return invalid_argument("Records size exceeds 5 MB limit");
        }
        auto results = append(*s, std::move(batch));
        if (!results) {
            return results.error();
        }
        boost::json::array records;
        std::int64_t failed = 0;
        bool encrypted = false;
        for (const auto& r : *results) {
            encrypted = encrypted || r._encrypted;
            boost::json::object entry;
            if (r._throttled) {
                ++failed;
                entry["ErrorCode"] = "ProvisionedThroughputExceededException";
                entry["ErrorMessage"] = r._message;
            } else if (r._error) {
                ++failed;
                entry["ErrorCode"] = "InternalFailure";
                entry["ErrorMessage"] =
                    r._storage_full ? k_disk_guard_message : "Internal service failure.";
            } else {
                entry["SequenceNumber"] = r._sequence;
                entry["ShardId"] = r._shard_id;
            }
            records.push_back(std::move(entry));
        }
        boost::json::object out;
        out["FailedRecordCount"] = failed;
        out["Records"] = std::move(records);
        if (encrypted) {
            out["EncryptionType"] = "KMS";
        }
        return out;
    }

    // ---- reads --------------------------------------------------------------

    auto get_shard_iterator(call_context& ctx, boost::json::object& in) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find_readable(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        const auto profile = profile_of(rec);
        const std::string shard_text(*detail::string_member(in, "ShardId"));
        const auto* entry = shard_named(rec, shard_text);
        if (entry == nullptr) {
            return shard_not_found(shard_text, rec);
        }
        const auto key = routing_key(rec._id, entry->_ordinal);
        if (profile._throttling && !_reads.admit_get_shard_iterator(key)) {
            _metrics.read_throttled({rec._account, rec._name, entry->_ordinal});
            return throughput_exceeded(
                shard_rate_exceeded(format_shard_id(entry->_ordinal, profile._id_format), rec));
        }
        const auto state = read_shard_state(key);
        if (!state) {
            return internal_failure();
        }
        if (!state->_found) {
            // The catalog lists the shard but this replica of its group has
            // not applied the open yet (a node just restarted, or behind a
            // reshard): a retryable failure, not a missing shard.
            return internal_failure();
        }
        shard_iterator it;
        it._account = rec._account;
        it._stream = rec._name;
        it._stream_id = rec._id;
        it._ordinal = entry->_ordinal;
        const std::string type(*detail::string_member(in, "ShardIteratorType"));
        auto start = start_position(rec, *entry, state->_state, type,
                                    detail::string_member(in, "StartingSequenceNumber"),
                                    service_detail::number_member(in, "Timestamp"),
                                    "StartingSequenceNumber", "GetShardIterator");
        if (!start) {
            return start.error();
        }
        it._counter = start->first;
        it._min_timestamp_ms = start->second;
        it._issued_ms = _cluster.now_ms();
        boost::json::object out;
        out["ShardIterator"] = encode_iterator(it, _config._iterator_keys);
        return out;
    }

    auto get_records(call_context& ctx, boost::json::object& in) -> result {
        const auto started = std::chrono::steady_clock::now();
        const auto now = _cluster.now_ms();
        const std::string token(*detail::string_member(in, "ShardIterator"));
        const auto decoded = decode_iterator(token, _config._iterator_keys);
        if (!decoded) {
            return invalid_argument("Invalid ShardIterator.");
        }
        const auto& it = *decoded;
        if (ctx._stream &&
            (ctx._stream->_account != it._account || ctx._stream->_name != it._stream)) {
            return invalid_argument("StreamARN " + ctx._stream->_arn +
                                    " does not match the stream of the ShardIterator.");
        }
        // The token names the stream: authorize it now.
        aws_auth::authz_request r;
        r.action = "kinesis:GetRecords";
        r.resource = "arn:" + ctx._partition + ":kinesis:" + ctx._region + ":" + it._account +
                     ":stream/" + it._stream;
        if (auto denied = ctx.authorize(std::move(r))) {
            return *denied;
        }

        std::optional<stream_record> found;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(it._account, it._stream)) {
                    found = *s;
                }
            })) {
            return internal_failure();
        }
        if (!found || found->_id != it._stream_id ||
            (found->_status != stream_status::active &&
             found->_status != stream_status::updating)) {
            return service_detail::not_found(it._stream, it._account);
        }
        const auto& rec = *found;
        const auto profile = profile_of(rec);
        if (const auto lifetime = lifetime_of(profile); iterator_expired(it, now, lifetime)) {
            return kinesis_error_style().modeled(
                "ExpiredIteratorException",
                "Iterator expired. The iterator was created at time " +
                    java_date_text(it._issued_ms) + " while right now it is " +
                    java_date_text(now) +
                    " which is further in the future than the tolerated delay of " +
                    std::to_string(lifetime) + " milliseconds.");
        }
        const shard_entry* entry = nullptr;
        for (const auto& sh : rec._shards) {
            if (sh._ordinal == it._ordinal) {
                entry = &sh;
            }
        }
        if (entry == nullptr) {
            return shard_not_found(format_shard_id(it._ordinal, profile._id_format), rec);
        }
        const auto key = routing_key(rec._id, it._ordinal);
        if (profile._throttling && !_reads.admit_get_records(key)) {
            _metrics.read_throttled({rec._account, rec._name, it._ordinal});
            return throughput_exceeded("Rate exceeded for Shard - " + rec._account + ":" +
                                       rec._name + ":" +
                                       format_shard_id(it._ordinal, profile._id_format));
        }
        const auto limit =
            static_cast<std::size_t>(service_detail::int_member(in, "Limit").value_or(10'000));
        const segment_read_limits limits{
            ._max_records = limit,
            ._max_bytes = profile._aws_limits ? _config._limits._max_get_records_bytes
                                              : _config._limits._relaxed_get_records_bytes};

        auto page = read_page(rec, *entry, it._counter, it._min_timestamp_ms, limits, now);
        if (!page) {
            return page.error();
        }
        if (profile._throttling) {
            _reads.charge(key, page->_bytes);
        }
        boost::json::object out;
        out["MillisBehindLatest"] = page->_behind;
        if (page->_exhausted) {
            // A closed shard read to its end: no next iterator, and the
            // children to move to (Requirement 9.3).
            out["ChildShards"] = std::move(page->_children);
        } else {
            shard_iterator next = it;
            next._counter = page->_next_counter;
            if (page->_count > 0) {
                next._min_timestamp_ms = 0;  // resolved: a plain position now
            }
            next._issued_ms = now;
            out["NextShardIterator"] = encode_iterator(next, _config._iterator_keys);
        }
        out["Records"] = std::move(page->_records);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        _metrics.read({rec._account, rec._name, it._ordinal}, page->_count, page->_bytes,
                      static_cast<std::uint64_t>(took), page->_behind);
        return out;
    }

    /// This node's metrics in the Prometheus text format (`GET /metrics`):
    /// the counters, with every ACTIVE or UPDATING stream's shard table,
    /// enhanced-monitoring switches and segment bytes from this replica.
    auto metrics_text() -> std::string {
        std::vector<stream_record> recs;
        (void)read_catalog([&](const auto& sm) {
            const auto prefix = streams_prefix();
            const auto& entries = sm.entries();
            for (auto it = entries.lower_bound(prefix);
                 it != entries.end() && it->first.starts_with(prefix); ++it) {
                if (const auto* r = std::get_if<stream_record>(&it->second);
                    r != nullptr && (r->_status == stream_status::active ||
                                     r->_status == stream_status::updating)) {
                    recs.push_back(*r);
                }
            }
        });
        std::vector<stream_metric_view> views;
        for (const auto& r : recs) {
            const auto profile = profile_of(r);
            stream_metric_view v;
            v._account = r._account;
            v._stream = r._name;
            v._shard_metrics = {r._shard_metrics.begin(), r._shard_metrics.end()};
            v._fanout_subscriptions = subscriptions_of(r._account, r._name);
            for (const auto& sh : r._shards) {
                stream_metric_view::shard row;
                row._ordinal = sh._ordinal;
                row._id = format_shard_id(sh._ordinal, profile._id_format);
                const auto key = routing_key(r._id, sh._ordinal);
                if (const auto st = read_shard_state(key); st && st->_found) {
                    for (const auto& d : st->_state._sealed) {
                        if (st->_state.is_tiered(d._first_seq)) {
                            row._tiered_bytes += d._bytes;
                        }
                    }
                    if (st->_store != nullptr) {
                        for (const auto& d : st->_store->local_segments(key)) {
                            row._local_bytes += d._bytes;
                        }
                    }
                }
                v._shards.push_back(std::move(row));
            }
            views.push_back(std::move(v));
        }
        return render_metrics(views, _metrics.snapshot());
    }

private:
    Cluster& _cluster;
    kinesis_service_config _config;
    read_throttle _reads;
    stream_metrics _metrics;

    [[nodiscard]] static auto shard_rate_exceeded(std::string_view shard, const stream_record& rec)
        -> std::string {
        return "Rate exceeded for shard " + std::string(shard) + " in stream " + rec._name +
               " under account " + rec._account + ".";
    }

    [[nodiscard]] static auto throughput_exceeded(std::string message) -> aws_json::error {
        return kinesis_error_style().modeled("ProvisionedThroughputExceededException",
                                             std::move(message));
    }

    // ---- data keys (task 14) ------------------------------------------------

    std::mutex _data_key_mutex;
    /// Unwrapped data keys by (stream id, generation). A stream id is never
    /// reused, so an entry never names another stream's key.
    std::map<std::pair<std::uint64_t, std::uint32_t>, data_key_bytes> _data_key_cache;

    /// What a stream's data keys are bound to: its account, name and id.
    [[nodiscard]] static auto encryption_context_of(const stream_record& rec)
        -> encryption_context {
        return {{"kythira:kinesis:stream",
                 rec._account + "/" + rec._name + "/" + std::to_string(rec._id)}};
    }

    [[nodiscard]] static auto kms_error_answer(const kms_error& e) -> aws_json::error {
        const char* type = "KMSInvalidStateException";
        switch (e.failure()) {
            case kms_failure::not_found:
            case kms_failure::unavailable:
                type = "KMSNotFoundException";
                break;
            case kms_failure::access_denied:
                type = "KMSAccessDeniedException";
                break;
            case kms_failure::disabled:
                type = "KMSDisabledException";
                break;
            case kms_failure::invalid_state:
                type = "KMSInvalidStateException";
                break;
            case kms_failure::throttling:
                type = "KMSThrottlingException";
                break;
            case kms_failure::opt_in_required:
                type = "KMSOptInRequired";
                break;
        }
        return kinesis_error_style().modeled(type, e.what());
    }

    auto encryption_answer(const stream_record& rec, const std::optional<apply_result>& r)
        -> result {
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return service_detail::not_found(rec._name, rec._account);
        }
        if (r->_status == apply_status::bad_transition) {
            return not_active(rec);
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    /// The plaintext data key of `rec`'s generation `generation`, from the
    /// cache or unwrapped by the provider. A generation newer than `rec`
    /// (started since it was read) is looked up in a fresh catalog read.
    /// Throws `kms_error` from the provider, `record_open_error` when no
    /// such generation exists.
    auto data_key_of(const stream_record& rec, std::uint32_t generation) -> data_key_bytes {
        {
            const std::scoped_lock lock(_data_key_mutex);
            if (const auto it = _data_key_cache.find({rec._id, generation});
                it != _data_key_cache.end()) {
                return it->second;
            }
        }
        std::optional<data_key_record> wrapped;
        if (const auto* k = rec.find_data_key(generation)) {
            wrapped = *k;
        } else {
            (void)read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(rec._account, rec._name);
                    s != nullptr && s->_id == rec._id) {
                    if (const auto* k = s->find_data_key(generation)) {
                        wrapped = *k;
                    }
                }
            });
        }
        if (!wrapped) {
            throw record_open_error("kinesis: a sealed record names an unknown data key");
        }
        // The provider may be a network call: not under the cache's lock.
        const auto key = _config._kms.decrypt_data_key(wrapped->_key_id, wrapped->_wrapped,
                                                       encryption_context_of(rec));
        const std::scoped_lock lock(_data_key_mutex);
        _data_key_cache.insert_or_assign({rec._id, generation}, key);
        return key;
    }

    /// Where a read of `entry` starts for iterator type `type` (as
    /// GetShardIterator and SubscribeToShard's StartingPosition take it):
    /// the counter, and the minimum arrival time for AT_TIMESTAMP (0
    /// otherwise). `seq_field` and `call` name things in the messages.
    [[nodiscard]] auto start_position(const stream_record& rec, const shard_entry& entry,
                                      const shard_state& state, std::string_view type,
                                      std::optional<std::string_view> seq_text,
                                      std::optional<double> timestamp, std::string_view seq_field,
                                      std::string_view call) const
        -> aws_json::result<std::pair<std::uint64_t, std::int64_t>> {
        const auto profile = profile_of(rec);
        const auto shard_text = format_shard_id(entry._ordinal, profile._id_format);
        const auto first = std::max(entry._starting_counter + 1, state._trim_before);
        if (type == "TRIM_HORIZON") {
            return std::pair{first, std::int64_t{0}};
        }
        if (type == "LATEST") {
            return std::pair{
                entry._ending_counter ? *entry._ending_counter + 1 : state._last_counter + 1,
                std::int64_t{0}};
        }
        if (type == "AT_SEQUENCE_NUMBER" || type == "AFTER_SEQUENCE_NUMBER") {
            if (!seq_text) {
                return invalid_argument(std::string(seq_field) +
                                        " must be provided for ShardIteratorType "
                                        "AT_SEQUENCE_NUMBER or AFTER_SEQUENCE_NUMBER.");
            }
            auto seq = parse_sequence(*seq_text, sequence_profile::aws);
            if (!seq) {
                seq = parse_sequence(*seq_text, sequence_profile::compact);
            }
            if (!seq) {
                return invalid_argument(std::string(seq_field) + " " + std::string(*seq_text) +
                                        " used in " + std::string(call) + " on shard " +
                                        shard_text + " in stream " + rec._name + " under account " +
                                        rec._account + " is invalid.");
            }
            if (seq->_ordinal != entry._ordinal) {
                return invalid_argument("Invalid " + std::string(seq_field) + ". It encodes " +
                                        format_shard_id(seq->_ordinal, profile._id_format) +
                                        ", while it was used in a call to a shard with " +
                                        shard_text);
            }
            return std::pair{seq->_counter + (type == "AFTER_SEQUENCE_NUMBER" ? 1 : 0),
                             std::int64_t{0}};
        }
        if (type == "AT_TIMESTAMP") {
            if (!timestamp) {
                return invalid_argument(
                    "Timestamp must be provided for ShardIteratorType AT_TIMESTAMP.");
            }
            const auto at_ms = static_cast<std::int64_t>(std::llround(*timestamp * 1000.0));
            // AWS refuses a timestamp in the future (task 17 recordings).
            if (const auto now = _cluster.now_ms(); at_ms > now) {
                return invalid_argument(
                    "The timestampInMillis parameter cannot be greater than the "
                    "currentTimestampInMillis. timestampInMillis: " +
                    std::to_string(at_ms) + ", currentTimestampInMillis: " + std::to_string(now));
            }
            return std::pair{first, std::max<std::int64_t>(1, at_ms)};
        }
        return invalid_argument("Invalid ShardIteratorType: " + std::string(type) + ".");
    }

    /// One read of a shard from this replica, as GetRecords and a
    /// subscription's events carry it.
    struct page {
        boost::json::array _records;
        std::size_t _count = 0;
        /// The records' partition keys and data.
        std::uint64_t _bytes = 0;
        /// Where the next read starts.
        std::uint64_t _next_counter = 0;
        std::int64_t _behind = 0;
        /// A closed shard read to its end; `_children` are its children.
        bool _exhausted = false;
        boost::json::array _children;
    };

    auto read_page(const stream_record& rec, const shard_entry& entry, std::uint64_t counter,
                   std::int64_t min_timestamp_ms, const segment_read_limits& limits,
                   std::int64_t now) -> aws_json::result<page> {
        const auto profile = profile_of(rec);
        const auto key = routing_key(rec._id, entry._ordinal);
        std::optional<shard_read> got;
        std::int64_t tip_arrival = 0;
        for (int attempt = 0; attempt < 2 && (!got || got->_missing); ++attempt) {
            // A segment can go missing between the copy and the read (a trim
            // or tiering in between): read again with a fresh copy once.
            const auto state = read_shard_state(key);
            if (!state || !state->_found || state->_store == nullptr) {
                // Not found here although the catalog lists it: this
                // replica is behind (see GetShardIterator). Retryable.
                return internal_failure();
            }
            try {
                got = read_shard(*state->_store, state->_fetch, key, state->_state, counter,
                                 min_timestamp_ms, limits, [&](std::span<const std::byte> bytes) {
                                     const auto g = sealed_generation(bytes);
                                     if (!g) {
                                         throw record_open_error("kinesis: truncated record");
                                     }
                                     return open_record(data_key_of(rec, *g), key, bytes);
                                 });
            } catch (const kms_error& e) {
                return kms_error_answer(e);
            } catch (const record_open_error&) {
                return internal_failure();
            }
            tip_arrival = state->_state._last_arrival_ms;
        }
        if (!got || got->_missing) {
            return internal_failure();
        }
        page out;
        for (const auto& rr : got->_records) {
            boost::json::object o;
            o["SequenceNumber"] = format_sequence(
                sequence_number{._ordinal = entry._ordinal, ._counter = rr._counter},
                profile._id_format);
            o["ApproximateArrivalTimestamp"] = service_detail::epoch_seconds(rr._arrival_ms);
            o["Data"] = service_detail::base64_encode(std::string_view(
                reinterpret_cast<const char*>(rr._record._data.data()), rr._record._data.size()));
            o["PartitionKey"] = rr._record._partition_key;
            if (rr._encrypted) {
                o["EncryptionType"] = "KMS";
            }
            out._records.push_back(std::move(o));
            out._bytes += rr._record._data.size() + rr._record._partition_key.size();
        }
        out._count = got->_records.size();
        out._next_counter = got->_next_counter;
        if (!got->_caught_up && !got->_records.empty()) {
            out._behind = std::max<std::int64_t>(0, now - got->_records.back()._arrival_ms);
        } else if (!got->_caught_up) {
            out._behind = std::max<std::int64_t>(0, now - tip_arrival);
        }
        out._exhausted = entry._ending_counter && got->_next_counter > *entry._ending_counter;
        if (out._exhausted) {
            out._behind = 0;
            for (const auto& sh : sorted_shards(rec)) {
                if (sh._parent != entry._ordinal && sh._adjacent_parent != entry._ordinal) {
                    continue;
                }
                boost::json::array parents;
                if (sh._parent) {
                    parents.emplace_back(format_shard_id(*sh._parent, profile._id_format));
                }
                if (sh._adjacent_parent) {
                    parents.emplace_back(format_shard_id(*sh._adjacent_parent, profile._id_format));
                }
                boost::json::object c;
                c["ShardId"] = format_shard_id(sh._ordinal, profile._id_format);
                c["ParentShards"] = std::move(parents);
                c["HashKeyRange"] =
                    boost::json::object{{"StartingHashKey", to_decimal(sh._range._start)},
                                        {"EndingHashKey", to_decimal(sh._range._end)}};
                out._children.push_back(std::move(c));
            }
        }
        return out;
    }

    /// A shard's state as this replica has it, copied out of the read so
    /// the store is read without holding the state machine.
    struct shard_snapshot {
        bool _found = false;
        shard_state _state;
        record_segment_store* _store = nullptr;
        segment_fetcher _fetch;
    };

    auto read_shard_state(const std::string& key) -> std::optional<shard_snapshot> {
        shard_snapshot snap;
        if (!_cluster.read(key, stream_reader([&](const auto& sm) {
                               if (const auto* st = sm.find_shard(key)) {
                                   snap._found = true;
                                   snap._state = *st;
                               }
                               snap._store = sm.store();
                               snap._fetch = sm.fetcher();
                           }))) {
            return std::nullopt;
        }
        return snap;
    }

    [[nodiscard]] static auto lifetime_of(const kinesis_compat_profile& p) -> std::int64_t {
        return p._aws_iterator_lifetime ? k_iterator_lifetime_ms : k_relaxed_iterator_lifetime_ms;
    }

    [[nodiscard]] static auto shard_named(const stream_record& rec, std::string_view text)
        -> const shard_entry* {
        const auto ordinal = parse_shard_id(text);
        if (!ordinal) {
            return nullptr;
        }
        for (const auto& sh : rec._shards) {
            if (sh._ordinal == *ordinal) {
                return &sh;
            }
        }
        return nullptr;
    }

    [[nodiscard]] static auto shard_not_found(std::string_view shard, const stream_record& rec)
        -> aws_json::error {
        return resource_not_found("Shard " + std::string(shard) + " in stream " + rec._name +
                                  " under account " + rec._account + " does not exist");
    }

    // Reads need an ACTIVE or UPDATING stream, as writes do.
    auto find_readable(const call_context& ctx) -> aws_json::result<stream_record> {
        return find_writable(ctx);
    }

    struct record_result {
        bool _error = false;
        /// The shard's write buckets refused it (with `_error`).
        bool _throttled = false;
        /// The disk guard refused it (with `_error`).
        bool _storage_full = false;
        /// Stored sealed (an encrypted stream, task 14).
        bool _encrypted = false;
        std::string _shard_id;
        std::string _sequence;
        std::string _message;
    };

    struct shards_token {
        std::string _account;
        std::string _name;
        std::uint32_t _after = 0;
        std::int64_t _created_ms = 0;
    };

    template<typename Command>
    auto propose(const Command& c, admission mode = admission::none)
        -> std::optional<apply_result> {
        const stream_command cmd = c;
        return _cluster.propose(command_key(cmd), encode_command(cmd), mode);
    }

    template<typename F> auto read_catalog(F&& f) -> bool {
        return _cluster.read(next_id_key(), stream_reader([&f](const auto& sm) { f(sm); }));
    }

    [[nodiscard]] auto profile_of(const stream_record& rec) const -> kinesis_compat_profile {
        return resolve_profile(_config._cluster_profile, {rec._tags.begin(), rec._tags.end()});
    }

    [[nodiscard]] static auto refuse_unsupported(const boost::json::object& in)
        -> std::optional<aws_json::error> {
        if (in.contains("StreamId")) {
            return invalid_argument(
                "StreamId is not supported by this endpoint; name the stream with StreamName "
                "or StreamARN.");
        }
        if (service_detail::bool_member(in, "DryRun")) {
            return invalid_argument("DryRun is not supported by this endpoint.");
        }
        if (const auto size = service_detail::int_member(in, "MaxRecordSizeInKiB");
            size && *size != 1024) {
            return invalid_argument(
                "MaxRecordSizeInKiB other than 1024 is not supported by this endpoint.");
        }
        return std::nullopt;
    }

    // ---- resharding internals ------------------------------------------------

    // ---- SubscribeToShard (task 13) -------------------------------------------

    /// One SubscribeToShard, pulled by the transport. The first message is
    /// the initial response. Then each pull reads the shard from this
    /// replica where the last event stopped, within the subscription's
    /// 2 MiB/s, and answers an event when there are records, when the
    /// shard ends (with its children and no continuation), or when five
    /// seconds passed without one. It ends after five minutes, when its
    /// consumer is deregistered or its stream deleted (with
    /// ResourceNotFoundException), or when a read fails (with
    /// InternalFailureException or a KMS exception): the client subscribes
    /// again from the last ContinuationSequenceNumber, possibly on another
    /// node. Reads are replica-local, so a leader change of the shard's
    /// group does not end it.
    class subscription final : public event_source {
    public:
        subscription(kinesis_service& svc, std::string key, aws_json::body_encoding encoding,
                     stream_record rec, consumer_record consumer, std::uint32_t ordinal,
                     std::uint64_t counter, std::int64_t min_timestamp_ms)
            : _svc(svc),
              _key(std::move(key)),
              _encoding(encoding),
              _rec(std::move(rec)),
              _consumer(std::move(consumer)),
              _ordinal(ordinal),
              _counter(counter),
              _min_timestamp_ms(min_timestamp_ms),
              _started(std::chrono::steady_clock::now()),
              _end(_started + svc._config._subscription_lifetime),
              _next_check(_started + svc._config._subscription_recheck),
              _last_event(_started),
              _refilled(_started),
              _tokens(static_cast<double>(svc._config._subscription_bytes_per_s)) {}

        subscription(const subscription&) = delete;
        subscription(subscription&&) = delete;
        auto operator=(const subscription&) -> subscription& = delete;
        auto operator=(subscription&&) -> subscription& = delete;
        ~subscription() override { _svc.forget_subscription(_key, this); }

        auto next(std::chrono::milliseconds wait) -> pull override {
            const auto until = std::chrono::steady_clock::now() + wait;
            if (_done || _cancelled.load()) {
                _done = true;
                return {._message = {}, ._done = true};
            }
            if (!_sent_initial) {
                _sent_initial = true;
                return {._message = event("initial-response", boost::json::object{}, ""),
                        ._done = false};
            }
            const auto& cfg = _svc._config;
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= _end || _cancelled.load()) {
                    _done = true;
                    return {._message = {}, ._done = true};
                }
                if (now >= _next_check) {
                    _next_check = now + cfg._subscription_recheck;
                    if (auto gone = refresh()) {
                        _done = true;
                        return {._message = std::move(*gone), ._done = false};
                    }
                }
                // With throttling off, a page is limited only by its size.
                const auto rate = _svc.profile_of(_rec)._throttling
                                      ? static_cast<double>(cfg._subscription_bytes_per_s)
                                      : static_cast<double>(std::uint64_t{1} << 30U);
                _tokens = std::min(
                    rate, _tokens + rate * std::chrono::duration<double>(now - _refilled).count());
                _refilled = now;
                const auto* entry = _rec.find_shard(_ordinal);
                if (entry == nullptr) {
                    _done = true;
                    return {._message = exception("ResourceNotFoundException",
                                                  "The shard of this subscription is gone."),
                            ._done = false};
                }
                if (_tokens >= 1.0) {
                    const segment_read_limits limits{
                        ._max_records = 10'000,
                        ._max_bytes = std::min<std::uint64_t>(static_cast<std::uint64_t>(_tokens),
                                                              std::uint64_t{1} << 20U)};
                    auto page = _svc.read_page(_rec, *entry, _counter, _min_timestamp_ms, limits,
                                               _svc._cluster.now_ms());
                    if (!page) {
                        _done = true;
                        const auto& e = page.error();
                        return {._message = exception(e.type == kinesis_error_style().internal_type
                                                          ? "InternalFailureException"
                                                          : e.type,
                                                      e.message.value_or("")),
                                ._done = false};
                    }
                    if (page->_count > 0 || page->_exhausted ||
                        now - _last_event >= cfg._subscription_heartbeat) {
                        _tokens -= static_cast<double>(page->_bytes);
                        _counter = page->_next_counter;
                        if (page->_count > 0) {
                            _min_timestamp_ms = 0;
                        }
                        _last_event = now;
                        _svc._metrics.fanned_out({_rec._account, _rec._name, _ordinal},
                                                 page->_count, page->_bytes, page->_behind);
                        const auto profile = _svc.profile_of(_rec);
                        boost::json::object payload;
                        payload["Records"] = std::move(page->_records);
                        payload["MillisBehindLatest"] = page->_behind;
                        if (page->_exhausted) {
                            payload["ContinuationSequenceNumber"] = nullptr;
                            payload["ChildShards"] = std::move(page->_children);
                            _done = true;  // the next pull ends the stream
                        } else {
                            payload["ContinuationSequenceNumber"] = format_sequence(
                                sequence_number{._ordinal = _ordinal, ._counter = _counter - 1},
                                profile._id_format);
                        }
                        return {._message = event("SubscribeToShardEvent", payload,
                                                  "SubscribeToShardEvent"),
                                ._done = false};
                    }
                }
                if (std::chrono::steady_clock::now() + cfg._subscription_poll > until) {
                    return {._message = {}, ._done = false};
                }
                std::this_thread::sleep_for(cfg._subscription_poll);
            }
        }

        void cancel() override { _cancelled = true; }

    private:
        kinesis_service& _svc;
        std::string _key;
        aws_json::body_encoding _encoding;
        stream_record _rec;
        consumer_record _consumer;
        std::uint32_t _ordinal;
        std::uint64_t _counter;
        std::int64_t _min_timestamp_ms;
        std::chrono::steady_clock::time_point _started;
        std::chrono::steady_clock::time_point _end;
        std::chrono::steady_clock::time_point _next_check;
        std::chrono::steady_clock::time_point _last_event;
        std::chrono::steady_clock::time_point _refilled;
        double _tokens;
        bool _sent_initial = false;
        bool _done = false;
        std::atomic<bool> _cancelled{false};

        // A fresh copy of the stream (its shard table and data keys move
        // on); the exception to end with when it or the consumer is gone.
        auto refresh() -> std::optional<std::string> {
            std::optional<stream_record> fresh;
            if (!_svc.read_catalog([&](const auto& sm) {
                    if (const auto* s = sm.find_stream(_rec._account, _rec._name);
                        s != nullptr && s->_id == _rec._id) {
                        fresh = *s;
                    }
                })) {
                return std::nullopt;  // try again on the next check
            }
            const auto* c = fresh ? fresh->find_consumer(_consumer._name) : nullptr;
            if (c == nullptr || c->_created_ms != _consumer._created_ms ||
                fresh->_status == stream_status::deleting) {
                return exception("ResourceNotFoundException",
                                 "Consumer " + _consumer._name + " of stream " + _rec._name +
                                     " under account " + _rec._account + " not found.");
            }
            _rec = std::move(*fresh);
            return std::nullopt;
        }

        [[nodiscard]] auto body_of(const boost::json::object& payload, std::string_view shape) const
            -> std::string {
            if (_encoding == aws_json::body_encoding::cbor) {
                return aws_json::json_to_cbor(
                    kinesis_model::k_model,
                    shape.empty() ? aws_json::no_shape : kinesis_model::k_model.find_shape(shape),
                    boost::json::value(payload), aws_json::cbor_timestamp_unit::milliseconds);
            }
            return boost::json::serialize(payload);
        }

        [[nodiscard]] auto content_type() const -> std::string {
            return _encoding == aws_json::body_encoding::cbor ? "application/x-amz-cbor-1.1"
                                                              : "application/json";
        }

        [[nodiscard]] static auto encoded(const aws_eventstream::message& m) -> std::string {
            const auto bytes = aws_eventstream::encode(m);
            return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        }

        [[nodiscard]] auto event(std::string type, const boost::json::object& payload,
                                 std::string_view shape) const -> std::string {
            const auto body = body_of(payload, shape);
            return encoded(aws_eventstream::make_event(
                std::move(type), content_type(),
                {reinterpret_cast<const std::byte*>(body.data()),
                 reinterpret_cast<const std::byte*>(body.data()) + body.size()}));
        }

        [[nodiscard]] auto exception(std::string type, const std::string& message) const
            -> std::string {
            const auto body = body_of(boost::json::object{{"message", message}}, "");
            return encoded(aws_eventstream::make_exception(
                std::move(type), content_type(),
                {reinterpret_cast<const std::byte*>(body.data()),
                 reinterpret_cast<const std::byte*>(body.data()) + body.size()}));
        }
    };

    struct subscription_slot {
        std::weak_ptr<subscription> _sub;
        std::string _account;
        std::string _stream;
    };

    std::mutex _subscriptions_mutex;
    std::map<std::string, subscription_slot> _subscriptions;

    void forget_subscription(const std::string& key, const subscription* sub) {
        const std::scoped_lock lock(_subscriptions_mutex);
        if (const auto it = _subscriptions.find(key); it != _subscriptions.end()) {
            const auto held = it->second._sub.lock();
            if (held == nullptr || held.get() == sub) {
                _subscriptions.erase(it);
            }
        }
    }

    [[nodiscard]] auto subscriptions_of(const std::string& account, const std::string& stream)
        -> std::uint64_t {
        const std::scoped_lock lock(_subscriptions_mutex);
        std::uint64_t n = 0;
        for (const auto& [key, slot] : _subscriptions) {
            n += slot._account == account && slot._stream == stream && !slot._sub.expired() ? 1 : 0;
        }
        return n;
    }

    // ---- task 13 internals ----------------------------------------------------

    struct consumer_ref {
        stream_record _stream;
        consumer_record _consumer;
    };

    enum class consumer_resource_op : std::uint8_t {
        tag,
        untag,
        list_tags,
        put_policy,
        get_policy,
        delete_policy
    };

    [[nodiscard]] static auto consumer_arn_of(const call_context& ctx, const stream_record& s,
                                              const consumer_record& c) -> std::string {
        return call_context::consumer_arn(arn_of(ctx, s), c._name, c.created_seconds());
    }

    [[nodiscard]] static auto consumer_json(const call_context& ctx, const stream_record& s,
                                            const consumer_record& c, bool with_stream)
        -> boost::json::object {
        boost::json::object o;
        o["ConsumerName"] = c._name;
        o["ConsumerARN"] = consumer_arn_of(ctx, s, c);
        o["ConsumerStatus"] = "ACTIVE";
        o["ConsumerCreationTimestamp"] = static_cast<double>(c.created_seconds());
        if (with_stream) {
            o["StreamARN"] = arn_of(ctx, s);
        }
        return o;
    }

    [[nodiscard]] static auto consumer_exists(const call_context& ctx, const stream_record& s,
                                              const consumer_record& c) -> aws_json::error {
        return service_detail::in_use("Consumer " + consumer_arn_of(ctx, s, c) + " under account " +
                                      s._account + " already exists.");
    }

    [[nodiscard]] static auto consumer_limit(const stream_record& s, std::size_t max)
        -> aws_json::error {
        return service_detail::limit_exceeded("Stream " + s._name + " under account " + s._account +
                                              " already has " + std::to_string(max) +
                                              " registered consumers.");
    }

    // AWS's 20, or more with the `count-limits` switch relaxed.
    [[nodiscard]] auto max_consumers_of(const stream_record& s) const -> std::size_t {
        return profile_of(s)._aws_count_limits ? k_max_consumers : k_relaxed_max_consumers;
    }

    [[nodiscard]] static auto consumer_not_found(std::string_view arn, std::string_view account)
        -> aws_json::error {
        return resource_not_found("Consumer " + std::string(arn) + " under account " +
                                  std::string(account) + " not found.");
    }

    [[nodiscard]] static auto consumers_token_prefix(const stream_record& s) -> std::string {
        return "consumers\n" + s._account + "\n" + s._name + "\n" + std::to_string(s._id) + "\n";
    }

    /// `stream/<stream>/consumer/<name>:<epoch seconds>`, split.
    struct consumer_arn_parts {
        std::string _account;
        std::string _stream;
        std::string _consumer;
        std::int64_t _created_seconds = 0;
    };

    [[nodiscard]] static auto parse_consumer_arn(const call_context& ctx, std::string_view arn)
        -> std::optional<consumer_arn_parts> {
        const auto parts = parse_kinesis_arn(arn);
        if (!parts || parts->_partition != ctx._partition || parts->_region != ctx._region ||
            !parts->_resource.starts_with("stream/")) {
            return std::nullopt;
        }
        auto rest = parts->_resource.substr(7);
        const auto slash = rest.find("/consumer/");
        if (slash == std::string_view::npos || slash == 0) {
            return std::nullopt;
        }
        const auto stream = rest.substr(0, slash);
        rest.remove_prefix(slash + 10);
        const auto colon = rest.rfind(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 == rest.size()) {
            return std::nullopt;
        }
        std::int64_t secs = 0;
        for (const char ch : rest.substr(colon + 1)) {
            if (ch < '0' || ch > '9' || secs > INT64_MAX / 10 - 9) {
                return std::nullopt;
            }
            secs = secs * 10 + (ch - '0');
        }
        return consumer_arn_parts{std::string(parts->_account), std::string(stream),
                                  std::string(rest.substr(0, colon)), secs};
    }

    [[nodiscard]] static auto is_consumer_arn(const boost::json::object& in) -> bool {
        const auto arn = detail::string_member(in, "ResourceARN");
        return arn && arn->find("/consumer/") != std::string_view::npos;
    }

    /// The consumer `arn` names, if it and its stream exist.
    auto consumer_by_arn(const call_context& ctx, std::string_view arn)
        -> aws_json::result<consumer_ref> {
        const auto parts = parse_consumer_arn(ctx, arn);
        if (!parts) {
            return invalid_argument("ConsumerARN " + std::string(arn) +
                                    " is not a consumer ARN in " + ctx._region + ".");
        }
        std::optional<consumer_ref> out;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(parts->_account, parts->_stream)) {
                    if (const auto* c = s->find_consumer(parts->_consumer);
                        c != nullptr && c->created_seconds() == parts->_created_seconds) {
                        out = consumer_ref{*s, *c};
                    }
                }
            })) {
            return internal_failure();
        }
        if (!out) {
            return consumer_not_found(arn, parts->_account);
        }
        return std::move(*out);
    }

    /// By `ConsumerARN`, or by the stream and `ConsumerName`.
    auto find_consumer(call_context& ctx, const boost::json::object& in)
        -> aws_json::result<consumer_ref> {
        const auto arn = detail::string_member(in, "ConsumerARN");
        const auto name = detail::string_member(in, "ConsumerName");
        if (arn) {
            if (name || ctx._stream) {
                return invalid_argument(
                    "Specify either ConsumerARN, or StreamARN and ConsumerName, not both.");
            }
            return consumer_by_arn(ctx, *arn);
        }
        if (!name || !ctx._stream) {
            return invalid_argument("Specify either ConsumerARN, or StreamARN and ConsumerName.");
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto* c = s->find_consumer(*name);
        if (c == nullptr) {
            return consumer_not_found(ctx._stream->_arn + "/consumer/" + std::string(*name),
                                      s->_account);
        }
        // The front end could not name this resource (its ARN carries the
        // creation time), so it is authorized here.
        aws_auth::authz_request r;
        r.action = "kinesis:" + std::string(ctx._operation);
        r.resource = consumer_arn_of(ctx, *s, *c);
        if (auto denied = ctx.authorize(std::move(r))) {
            return *denied;
        }
        return consumer_ref{*s, *c};
    }

    auto change_consumer(const call_context& ctx, const consumer_ref& ref, consumer_op op,
                         std::map<std::string, std::string> set, std::vector<std::string> remove,
                         std::optional<std::string> policy) -> result {
        const auto r = propose(consumer_command{._account = ref._stream._account,
                                                ._name = ref._stream._name,
                                                ._id = ref._stream._id,
                                                ._op = op,
                                                ._consumer = ref._consumer._name,
                                                ._created_ms = ref._consumer._created_ms,
                                                ._set = std::move(set),
                                                ._remove = std::move(remove),
                                                ._policy = std::move(policy),
                                                ._max = k_max_tags});
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return consumer_not_found(consumer_arn_of(ctx, ref._stream, ref._consumer),
                                      ref._stream._account);
        }
        if (r->_status == apply_status::invalid) {
            return service_detail::limit_exceeded("A consumer can have at most " +
                                                  std::to_string(k_max_tags) + " tags.");
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    /// The tag and policy calls on a consumer ARN.
    auto consumer_resource(call_context& ctx, const boost::json::object& in,
                           consumer_resource_op op) -> result {
        const std::string arn(*detail::string_member(in, "ResourceARN"));
        auto found = consumer_by_arn(ctx, arn);
        if (!found) {
            return found.error();
        }
        const auto& c = found->_consumer;
        switch (op) {
            case consumer_resource_op::tag:
                return change_consumer(ctx, *found, consumer_op::retag, tag_map_of(in), {},
                                       std::nullopt);
            case consumer_resource_op::untag:
                return change_consumer(ctx, *found, consumer_op::retag, {}, tag_keys_of(in),
                                       std::nullopt);
            case consumer_resource_op::list_tags: {
                boost::json::array tags;
                for (const auto& [k, v] : c._tags) {
                    tags.push_back(boost::json::object{{"Key", k}, {"Value", v}});
                }
                boost::json::object out;
                out["Tags"] = std::move(tags);
                return out;
            }
            case consumer_resource_op::put_policy:
                return change_consumer(ctx, *found, consumer_op::set_policy, {}, {},
                                       std::string(*detail::string_member(in, "Policy")));
            case consumer_resource_op::get_policy: {
                boost::json::object out;
                out["Policy"] = c._policy.value_or("{}");
                return out;
            }
            case consumer_resource_op::delete_policy:
                if (!c._policy) {
                    return resource_not_found("No resource policy found for resource " + arn + ".");
                }
                return change_consumer(ctx, *found, consumer_op::set_policy, {}, {}, std::nullopt);
        }
        return internal_failure();
    }

    // ---- task 15 internals ----------------------------------------------------

    [[nodiscard]] static auto tag_map_of(const boost::json::object& in)
        -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        if (const auto* t = in.if_contains("Tags"); t != nullptr && t->is_object()) {
            for (const auto& kv : t->get_object()) {
                out[std::string(kv.key())] =
                    kv.value().is_string() ? std::string(kv.value().get_string()) : std::string();
            }
        }
        return out;
    }

    [[nodiscard]] static auto tag_keys_of(const boost::json::object& in)
        -> std::vector<std::string> {
        return strings_of(in, "TagKeys");
    }

    // A list-of-strings member, empty when absent.
    [[nodiscard]] static auto strings_of(const boost::json::object& in, std::string_view member)
        -> std::vector<std::string> {
        std::vector<std::string> out;
        if (const auto* k = in.if_contains(member); k != nullptr && k->is_array()) {
            for (const auto& v : k->get_array()) {
                if (v.is_string()) {
                    out.emplace_back(v.get_string());
                }
            }
        }
        return out;
    }

    // The stream a `ResourceARN` names. A consumer ARN is a resource this
    // endpoint does not have until enhanced fan-out (task 13).
    auto find_resource(const call_context& ctx, const boost::json::object& in)
        -> aws_json::result<stream_record> {
        const std::string arn(*detail::string_member(in, "ResourceARN"));
        const auto parts = parse_kinesis_arn(arn);
        const auto name = stream_name_of(arn);
        if (!parts || parts->_partition != ctx._partition || parts->_region != ctx._region) {
            return invalid_argument("ResourceARN " + arn + " is not a Kinesis ARN in " +
                                    ctx._region + ".");
        }
        if (!name) {
            return resource_not_found("Resource " + arn + " not found.");
        }
        std::optional<stream_record> found;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* rec =
                        sm.find_stream(std::string(parts->_account), std::string(*name))) {
                    found = *rec;
                }
            })) {
            return internal_failure();
        }
        if (!found) {
            return service_detail::not_found(std::string(*name), std::string(parts->_account));
        }
        return *found;
    }

    auto change_tags(const stream_record& rec, std::map<std::string, std::string> set,
                     std::vector<std::string> remove) -> result {
        if (rec._status != stream_status::active && rec._status != stream_status::updating) {
            return not_active(rec);
        }
        if (auto bad = check_compat_tags({set.begin(), set.end()})) {
            return *bad;
        }
        auto after = rec._tags;
        for (const auto& [k, v] : set) {
            after[k] = v;
        }
        for (const auto& k : remove) {
            after.erase(k);
        }
        if (after.size() > k_max_tags) {
            return service_detail::limit_exceeded(
                "Failed to add tags to stream " + rec._name + " under account " + rec._account +
                " because a stream can have at most " + std::to_string(k_max_tags) + " tags.");
        }
        const auto r =
            propose(tag_stream_command{rec._account, rec._name, rec._id, std::move(set),
                                       std::move(remove), static_cast<std::uint32_t>(k_max_tags)});
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return service_detail::not_found(rec._name, rec._account);
        }
        if (r->_status == apply_status::invalid) {
            return service_detail::limit_exceeded(
                "Failed to add tags to stream " + rec._name + " under account " + rec._account +
                " because a stream can have at most " + std::to_string(k_max_tags) + " tags.");
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    auto set_policy(const stream_record& rec, std::optional<std::string> policy) -> result {
        const auto r =
            propose(set_policy_command{rec._account, rec._name, rec._id, std::move(policy)});
        if (!r) {
            return internal_failure();
        }
        if (r->_status == apply_status::not_found) {
            return service_detail::not_found(rec._name, rec._account);
        }
        if (r->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
    }

    // The shard-level metrics AWS defines; `ALL` stands for every one.
    [[nodiscard]] static auto all_shard_metrics() -> const std::vector<std::string>& {
        static const std::vector<std::string> k_all{"IncomingBytes",
                                                    "IncomingRecords",
                                                    "IteratorAgeMilliseconds",
                                                    "OutgoingBytes",
                                                    "OutgoingRecords",
                                                    "ReadProvisionedThroughputExceeded",
                                                    "WriteProvisionedThroughputExceeded"};
        return k_all;
    }

    auto monitoring(call_context& ctx, boost::json::object& in, bool enable) -> result {
        if (auto e = refuse_unsupported(in)) {
            return *e;
        }
        auto s = find(ctx);
        if (!s) {
            return s.error();
        }
        const auto& rec = *s;
        if (rec._status != stream_status::active) {
            return not_active(rec);
        }
        std::set<std::string> asked;
        for (const auto& m : strings_of(in, "ShardLevelMetrics")) {
            if (m == "ALL") {
                asked.insert(all_shard_metrics().begin(), all_shard_metrics().end());
            } else {
                asked.insert(m);
            }
        }
        std::set<std::string> now(rec._shard_metrics.begin(), rec._shard_metrics.end());
        auto desired = now;
        for (const auto& m : asked) {
            if (enable) {
                desired.insert(m);
            } else {
                desired.erase(m);
            }
        }
        if (desired != now) {
            const auto r = propose(set_monitoring_command{
                rec._account, rec._name, rec._id, {desired.begin(), desired.end()}});
            if (r && r->_status == apply_status::not_found) {
                return service_detail::not_found(rec._name, rec._account);
            }
            if (!r || r->_status != apply_status::ok) {
                return internal_failure();
            }
        }
        const auto list = [](const std::set<std::string>& names) {
            boost::json::array a;
            for (const auto& n : names) {
                a.emplace_back(n);
            }
            return a;
        };
        boost::json::object out;
        out["StreamName"] = rec._name;
        out["StreamARN"] = arn_of(ctx, rec);
        out["CurrentShardLevelMetrics"] = list(now);
        out["DesiredShardLevelMetrics"] = list(desired);
        return out;
    }

    // AWS: SplitShard, MergeShards and UpdateShardCount are "only supported
    // for the data streams with the provisioned capacity mode". The error
    // code and wording are ours until a recording shows AWS's.
    [[nodiscard]] static auto provisioned_only(const stream_record& rec, std::string_view op)
        -> std::optional<aws_json::error> {
        if (rec._mode != stream_mode::on_demand) {
            return std::nullopt;
        }
        return kinesis_error_style().modeled(
            "ValidationException", std::string(op) + " is not supported for stream " + rec._name +
                                       " under account " + rec._account +
                                       " in ON_DEMAND capacity mode.");
    }

    [[nodiscard]] static auto child(std::uint32_t ordinal, std::uint32_t parent,
                                    std::optional<std::uint32_t> adjacent_parent, hash_range range)
        -> shard_entry {
        shard_entry c;
        c._ordinal = ordinal;
        c._parent = parent;
        c._adjacent_parent = adjacent_parent;
        c._range = range;
        return c;
    }

    [[nodiscard]] static auto adjacent(const hash_range& a, const hash_range& b) -> bool {
        return (a._end != max_hash_key && a._end + 1 == b._start) ||
               (b._end != max_hash_key && b._end + 1 == a._start);
    }

    // The open shard a reshard names, or AWS's refusal.
    [[nodiscard]] static auto shard_to_reshard(const stream_record& rec, const std::string& text)
        -> aws_json::result<const shard_entry*> {
        const auto ordinal = parse_shard_id(text);
        const auto* sh = ordinal ? rec.find_shard(*ordinal) : nullptr;
        if (sh == nullptr) {
            return resource_not_found("Could not find shard " + text + " in stream " + rec._name +
                                      " under account " + rec._account + ".");
        }
        if (!sh->open()) {
            return service_detail::in_use(
                "Shard " + text + " in stream " + rec._name + " under account " + rec._account +
                " has already been merged or split, and thus is not eligible for merging or "
                "splitting.");
        }
        return sh;
    }

    // One step of UNIFORM_SCALING towards `target` open shards.
    [[nodiscard]] static auto uniform_step(const stream_record& rec, std::size_t target)
        -> aws_json::result<reshard_plan> {
        std::vector<const shard_entry*> open;
        for (const auto& sh : rec._shards) {
            if (sh.open()) {
                open.push_back(&sh);
            }
        }
        std::sort(open.begin(), open.end(),
                  [](const auto* a, const auto* b) { return a->_range._start < b->_range._start; });
        const auto width = [](const hash_range& r) { return r._end - r._start; };
        reshard_plan plan;
        if (open.size() < target) {
            const auto* widest = *std::max_element(
                open.begin(), open.end(),
                [&](const auto* a, const auto* b) { return width(a->_range) < width(b->_range); });
            const auto mid = widest->_range._start + width(widest->_range) / 2 + 1;
            plan._closing.push_back(widest->_ordinal);
            plan._children.push_back(child(rec._next_ordinal, widest->_ordinal, std::nullopt,
                                           {widest->_range._start, mid - 1}));
            plan._children.push_back(child(rec._next_ordinal + 1, widest->_ordinal, std::nullopt,
                                           {mid, widest->_range._end}));
            return plan;
        }
        std::size_t best = 0;
        for (std::size_t i = 1; i + 1 < open.size(); ++i) {
            if (width(open[i]->_range) + width(open[i + 1]->_range) <
                width(open[best]->_range) + width(open[best + 1]->_range)) {
                best = i;
            }
        }
        const auto* x = open[best];
        const auto* y = open[best + 1];
        plan._closing = {x->_ordinal, y->_ordinal};
        plan._children.push_back(
            child(rec._next_ordinal, x->_ordinal, y->_ordinal, {x->_range._start, y->_range._end}));
        return plan;
    }

    // A reshard (Requirement 10.1). The stream goes UPDATING first, which
    // only one reshard can do at a time, so the plan made from the catalog
    // read after that cannot be overtaken. The catalog's half, the parents'
    // close and the children's open then commit as one tier-1 transaction:
    // appends that reach a parent before it are in the parent, appends after
    // it find the parent closed and go to the children, and appends during
    // it wait out the transaction's locks. Last, the counters the parents
    // ended at go into the catalog and the stream returns to ACTIVE. A
    // reshard abandoned after the transaction is finished by the
    // maintenance pass (`complete_reshard`).
    template<typename Planner>
    auto run_reshard(const stream_record& rec, Planner&& plan_of) -> aws_json::result<bool>
    requires transacts<Cluster>
    {
        // The compare-and-set decides, not `rec`: this replica can still
        // show the UPDATING of a reshard that has just finished.
        const auto began = propose(
            set_stream_status_command{rec._account, rec._name, rec._id, stream_status::updating});
        if (!began) {
            return internal_failure();
        }
        if (began->_status == apply_status::bad_transition) {
            if (rec._status != stream_status::active) {
                return not_active(rec);
            }
            return service_detail::in_use("Stream " + rec._name + " under account " + rec._account +
                                          " not ACTIVE, instead in state UPDATING");
        }
        if (began->_status != apply_status::ok) {
            return service_detail::not_found(rec._name, rec._account);
        }
        const auto back_to_active = [&] {
            (void)propose(
                set_stream_status_command{rec._account, rec._name, rec._id, stream_status::active});
        };
        // The catalog as UPDATING left it. Read through the log, so a
        // replica that is behind cannot hand the plan a stale shard table.
        auto fresh = linearizable_record(rec);
        if (!fresh) {
            back_to_active();
            return internal_failure();
        }
        auto plan = plan_of(*fresh);
        if (!plan) {
            back_to_active();
            return plan.error();
        }
        std::vector<txn_write> writes;
        const auto add = [&writes](const stream_command& c) {
            writes.push_back({command_key(c), encode_command(c)});
        };
        add(reshard_catalog_command{rec._account, rec._name, rec._id, plan->_closing,
                                    plan->_children});
        for (const auto o : plan->_closing) {
            add(close_shard_command{routing_key(rec._id, o)});
        }
        for (const auto& c : plan->_children) {
            add(open_shard_command{routing_key(rec._id, c._ordinal), 0});
        }
        switch (_cluster.transact(std::move(writes))) {
            case transact_outcome::committed:
                break;
            case transact_outcome::aborted:
                back_to_active();
                return internal_failure();
            case transact_outcome::unknown:
                // Left UPDATING: the maintenance pass finishes it either way
                // once the transaction's fate is settled.
                return internal_failure();
        }
        // Wait for this replica to show the children, so the completion
        // below sees the parents closing.
        const auto first_child = plan->_children.front()._ordinal;
        if (!record_once(rec._account, rec._name, rec._id,
                         [first_child](const stream_record& r) {
                             return r.find_shard(first_child) != nullptr;
                         }) ||
            !complete_reshard(rec._account, rec._name, rec._id)) {
            return internal_failure();
        }
        return true;
    }

public:
    /// Finish a reshard whose transaction committed: record the counters the
    /// closing shards ended at (asked of their groups by re-proposing the
    /// close, which answers a closed shard's last counter and changes
    /// nothing) and return the stream to ACTIVE. On a stream whose
    /// transaction aborted it just returns it to ACTIVE. Safe to repeat.
    auto complete_reshard(const std::string& account, const std::string& name, std::uint64_t id)
        -> bool {
        std::optional<stream_record> rec;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(account, name)) {
                    rec = *s;
                }
            }) ||
            !rec || rec->_id != id) {
            return false;
        }
        set_ending_counters_command endings{account, name, id, {}};
        for (const auto& sh : rec->_shards) {
            if (!sh._closing) {
                continue;
            }
            const auto r = propose(close_shard_command{routing_key(id, sh._ordinal)});
            if (!r || r->_status != apply_status::ok) {
                return false;
            }
            endings._endings.emplace_back(sh._ordinal, r->_last_counter);
        }
        if (!endings._endings.empty()) {
            const auto r = propose(endings);
            if (!r || r->_status != apply_status::ok) {
                return false;
            }
        }
        if (rec->_status == stream_status::updating) {
            const auto r =
                propose(set_stream_status_command{account, name, id, stream_status::active});
            if (!r || r->_status != apply_status::ok) {
                return false;
            }
        }
        return true;
    }

    /// The maintenance pass's half of resharding: a stream UPDATING for
    /// longer than `stale_ms` (seen by this node's earlier calls; a live
    /// reshard takes a few commit round trips) had its reshard abandoned
    /// after the transaction, or before it; either way its transaction's
    /// fate is settled by now, so complete it. Shards still closing on an
    /// ACTIVE stream get their ending counters too. Returns how many streams
    /// it completed.
    auto repair_reshards(std::int64_t now_ms, std::int64_t stale_ms) -> std::size_t
    requires transacts<Cluster>
    {
        std::vector<stream_record> streams;
        (void)read_catalog([&](const auto& sm) {
            const auto prefix = streams_prefix();
            const auto& entries = sm.entries();
            for (auto it = entries.lower_bound(prefix);
                 it != entries.end() && it->first.starts_with(prefix); ++it) {
                if (const auto* r = std::get_if<stream_record>(&it->second)) {
                    streams.push_back(*r);
                }
            }
        });
        std::size_t repaired = 0;
        const std::scoped_lock lock(_updating_mutex);
        std::map<std::uint64_t, std::int64_t> still;
        for (const auto& rec : streams) {
            const bool closing = std::any_of(rec._shards.begin(), rec._shards.end(),
                                             [](const shard_entry& s) { return s._closing; });
            if (rec._status == stream_status::updating) {
                const auto seen = _updating_since.find(rec._id);
                const auto since = seen == _updating_since.end() ? now_ms : seen->second;
                still[rec._id] = since;
                if (now_ms - since < stale_ms) {
                    continue;
                }
            } else if (!closing) {
                continue;
            }
            if (complete_reshard(rec._account, rec._name, rec._id)) {
                ++repaired;
                still.erase(rec._id);
            }
        }
        _updating_since = std::move(still);
        return repaired;
    }

private:
    std::mutex _creating_mutex;
    /// When this node first saw each CREATING stream (`repair_creates`).
    std::map<std::uint64_t, std::int64_t> _creating_since;
    std::mutex _updating_mutex;
    /// When this node first saw each UPDATING stream (`repair_reshards`).
    std::map<std::uint64_t, std::int64_t> _updating_since;

    [[nodiscard]] static auto not_active(const stream_record& rec) -> aws_json::error {
        return service_detail::in_use("Stream " + rec._name + " under account " + rec._account +
                                      " not ACTIVE, instead in state " +
                                      std::string(to_string(rec._status)));
    }

    // The stream record once this replica shows `done(record)`, waiting up
    // to a second for it to catch up. The catalog's log is ordered, so once
    // the replica has applied an entry it has applied everything before it.
    template<typename Done>
    auto record_once(const std::string& account, const std::string& name, std::uint64_t id,
                     Done&& done) -> std::optional<stream_record> {
        for (int attempt = 0; attempt < 50; ++attempt) {
            std::optional<stream_record> found;
            if (!read_catalog([&](const auto& sm) {
                    if (const auto* s = sm.find_stream(account, name)) {
                        found = *s;
                    }
                })) {
                return std::nullopt;
            }
            if (found && found->_id == id && done(*found)) {
                return found;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return std::nullopt;
    }

    // The record once this replica has applied our UPDATING entry: the
    // shard table every step of this reshard builds on.
    auto linearizable_record(const stream_record& rec) -> std::optional<stream_record> {
        return record_once(rec._account, rec._name, rec._id, [](const stream_record& r) {
            return r._status == stream_status::updating;
        });
    }

    // CreateStream's steps after the catalog commit: open every shard of
    // the stream's table under its routing key and move it to ACTIVE. Each
    // step is idempotent, so a retry, another node's maintenance pass or
    // both at once are safe. `id` pins the stream a caller just created;
    // nullopt takes whichever stream has the name. True once the stream is
    // ACTIVE (or was not CREATING to begin with).
    auto finish_create(const std::string& account, const std::string& name,
                       std::optional<std::uint64_t> id) -> bool {
        std::optional<stream_record> rec;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(account, name)) {
                    rec = *s;
                }
            })) {
            return false;
        }
        if (id && (!rec || rec->_id != *id)) {
            // Our own commit is not on this replica yet: wait for it.
            rec = record_once(account, name, *id, [](const stream_record&) { return true; });
        }
        if (!rec || (id && rec->_id != *id)) {
            return false;
        }
        if (rec->_status != stream_status::creating) {
            return true;
        }
        for (const auto& sh : rec->_shards) {
            const auto opened = propose(open_shard_command{routing_key(rec->_id, sh._ordinal), 0});
            if (!opened || (opened->_status != apply_status::ok &&
                            opened->_status != apply_status::already_exists)) {
                return false;
            }
        }
        const auto active =
            propose(set_stream_status_command{account, name, rec->_id, stream_status::active});
        if (active && active->_status == apply_status::ok) {
            return true;
        }
        // Someone else (a retry, another node's pass) may have got there
        // first: ACTIVE is ACTIVE whoever set it.
        return active && active->_status == apply_status::bad_transition &&
               record_once(account, name, rec->_id, [](const stream_record& r) {
                   return r._status == stream_status::active;
               }).has_value();
    }

    // The stream the request names, as this replica's catalog has it.
    auto find(const call_context& ctx) -> aws_json::result<stream_record> {
        if (!ctx._stream) {
            return invalid_argument("Either StreamName or StreamARN should be provided.");
        }
        std::optional<stream_record> found;
        if (!read_catalog([&](const auto& sm) {
                if (const auto* s = sm.find_stream(ctx._stream->_account, ctx._stream->_name)) {
                    found = *s;
                }
            })) {
            return internal_failure();
        }
        if (!found) {
            return service_detail::not_found(ctx._stream->_name, ctx._stream->_account);
        }
        return *found;
    }

    // Writes need an ACTIVE or UPDATING stream; AWS reports the others as
    // not found.
    auto find_writable(const call_context& ctx) -> aws_json::result<stream_record> {
        auto s = find(ctx);
        if (s && s->_status != stream_status::active && s->_status != stream_status::updating) {
            return service_detail::not_found(s->_name, s->_account);
        }
        return s;
    }

    auto decode_record(const boost::json::object& in, std::size_t index)
        -> aws_json::result<service_detail::pending_record> {
        service_detail::pending_record r;
        r._index = index;
        r._partition_key = std::string(*detail::string_member(in, "PartitionKey"));
        auto data = service_detail::base64_decode(*detail::string_member(in, "Data"));
        if (!data) {
            return kinesis_error_style().modeled("SerializationException",
                                                 "Data is not valid base64.");
        }
        r._data.resize(data->size());
        std::transform(data->begin(), data->end(), r._data.begin(),
                       [](char c) { return static_cast<std::byte>(c); });
        // AWS limits the data blob alone to MaxRecordSizeInKiB: a 1 MiB blob
        // with a partition key is accepted (task 17 recordings).
        if (r._data.size() > _config._limits._max_record_bytes) {
            return invalid_argument(
                "Record size exceeds the maximum record size of 1024 KiB for this stream.");
        }
        if (auto explicit_key = detail::string_member(in, "ExplicitHashKey")) {
            auto k = parse_hash_key(*explicit_key);
            if (!k) {
                return invalid_argument(
                    "Invalid ExplicitHashKey. ExplicitHashKey must be in the "
                    "range: [0, 2^128-1]. Specified value was " +
                    std::string(*explicit_key));
            }
            r._hash = *k;
        } else {
            r._hash = partition_key_hash(r._partition_key);
        }
        return r;
    }

    /// One shard's share of a write, and what committing it answered.
    struct shard_append {
        std::uint32_t _ordinal = 0;
        std::vector<service_detail::pending_record*> _records;
        append_records_command _command;
        std::optional<append_part_result> _result{};
    };

    /// Commits every shard's append. Shards the cluster places in one group
    /// share one entry (`append_batch_command`, group commit); a part that
    /// finds its shard gone from that group (a split since this node
    /// routed) is proposed again on its own, under its own key.
    void commit(std::vector<shard_append>& appends, admission mode) {
        const auto alone = [this, mode](shard_append& a) {
            const auto r = propose(a._command, mode);
            if (r) {
                a._result = append_part_result{._status = r->_status,
                                               ._first_counter = r->_first_counter,
                                               ._last_counter = r->_last_counter,
                                               ._arrival_ms = r->_arrival_ms};
            }
        };
        std::vector<std::vector<shard_append*>> batches;
        if constexpr (groups_keys<Cluster>) {
            std::map<std::uint64_t, std::size_t> slot;
            for (auto& a : appends) {
                const auto g = _cluster.group_of(a._command._key);
                if (!g) {
                    batches.push_back({&a});
                    continue;
                }
                const auto [it, fresh] = slot.try_emplace(*g, batches.size());
                if (fresh) {
                    batches.emplace_back();
                }
                batches[it->second].push_back(&a);
            }
        } else {
            for (auto& a : appends) {
                batches.push_back({&a});
            }
        }
        for (auto& batch : batches) {
            if (batch.size() == 1) {
                alone(*batch.front());
                continue;
            }
            append_batch_command c;
            c._appends.reserve(batch.size());
            for (const auto* a : batch) {
                c._appends.push_back(a->_command);
            }
            const auto r = propose(c, mode);
            if (r && r->_status == apply_status::storage_full) {
                for (auto* a : batch) {
                    a->_result = append_part_result{._status = apply_status::storage_full};
                }
                continue;
            }
            if (!r || r->_parts.size() != batch.size()) {
                continue;  // not committed in time: every record fails
            }
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (r->_parts[i]._status == apply_status::not_found) {
                    alone(*batch[i]);
                } else {
                    batch[i]->_result = r->_parts[i];
                }
            }
        }
    }

    // Requirement 7.1: one entry per shard, records in request order within
    // it. Retries the records of a shard found closed against a fresh read
    // of the stream.
    auto append(stream_record rec, std::vector<service_detail::pending_record> batch)
        -> aws_json::result<std::vector<record_result>> {
        std::vector<record_result> out(batch.size());
        const auto profile = profile_of(rec);
        for (int attempt = 0;; ++attempt) {
            std::map<std::uint32_t, std::vector<service_detail::pending_record*>> by_shard;
            for (auto& r : batch) {
                const auto* sh = rec.shard_for(r._hash);
                if (sh == nullptr) {
                    // No open shard owns the hash: the shard table is mid
                    // reshard on this replica.
                    out[r._index]._error = true;
                    continue;
                }
                by_shard[sh->_ordinal].push_back(&r);
            }
            // An encrypted stream's records are sealed here, before they
            // reach the log (task 14).
            std::optional<data_key_bytes> key;
            std::uint32_t generation = 0;
            if (const auto* dk = rec.current_data_key()) {
                generation = dk->_generation;
                try {
                    key = data_key_of(rec, generation);
                } catch (const kms_error& e) {
                    return kms_error_answer(e);
                } catch (const record_open_error&) {
                    return internal_failure();
                }
            }
            std::vector<service_detail::pending_record> retry;
            std::vector<shard_append> appends;
            appends.reserve(by_shard.size());
            for (auto& [ordinal, records] : by_shard) {
                shard_append a{._ordinal = ordinal, ._records = std::move(records), ._command = {}};
                a._command._key = routing_key(rec._id, ordinal);
                a._command._arrival_ms = _cluster.now_ms();
                for (const auto* r : a._records) {
                    if (key) {
                        a._command._records.push_back(
                            {{},
                             seal_record(*key, generation, a._command._key, r->_partition_key,
                                         r->_data)});
                    } else {
                        a._command._records.push_back({r->_partition_key, r->_data});
                    }
                }
                appends.push_back(std::move(a));
            }
            commit(appends, profile._throttling ? admission::shard_limits : admission::none);
            for (auto& a : appends) {
                const auto& res = a._result;
                if (res && res->_status == apply_status::shard_closed &&
                    attempt < _config._closed_shard_retries) {
                    for (auto* r : a._records) {
                        retry.push_back(std::move(*r));
                    }
                    continue;
                }
                const bool ok = res && res->_status == apply_status::ok;
                // The leader's buckets admit a prefix of a shard's records;
                // the rest are throttled one by one (Requirement 12.1).
                const std::size_t admitted =
                    ok ? static_cast<std::size_t>(res->_last_counter - res->_first_counter + 1) : 0;
                const bool throttling = res && (res->_status == apply_status::throttled ||
                                                (ok && admitted < a._records.size()));
                const bool full = res && res->_status == apply_status::storage_full;
                if (ok && admitted > 0) {
                    std::uint64_t bytes = 0;
                    for (std::size_t i = 0; i < admitted && i < a._records.size(); ++i) {
                        bytes += a._records[i]->_data.size() + a._records[i]->_partition_key.size();
                    }
                    _metrics.wrote({rec._account, rec._name, a._ordinal}, admitted, bytes);
                }
                if (throttling && a._records.size() > admitted) {
                    _metrics.write_throttled({rec._account, rec._name, a._ordinal},
                                             a._records.size() - admitted);
                }
                for (std::size_t i = 0; i < a._records.size(); ++i) {
                    auto& result = out[a._records[i]->_index];
                    result._error = !ok || i >= admitted;
                    result._storage_full = full;
                    result._encrypted = key.has_value();
                    if (throttling && i >= admitted) {
                        result._throttled = true;
                        result._message = shard_rate_exceeded(
                            format_shard_id(a._ordinal, profile._id_format), rec);
                        continue;
                    }
                    if (ok) {
                        result._shard_id = format_shard_id(a._ordinal, profile._id_format);
                        result._sequence =
                            format_sequence(sequence_number{._ordinal = a._ordinal,
                                                            ._counter = res->_first_counter + i},
                                            profile._id_format);
                    }
                }
            }
            if (retry.empty()) {
                return out;
            }
            if (attempt > 0) {
                std::this_thread::sleep_for(_config._closed_shard_backoff *
                                            (1 << std::min(attempt - 1, 6)));
            }
            std::optional<stream_record> fresh;
            if (!read_catalog([&](const auto& sm) {
                    if (const auto* s = sm.find_stream(rec._account, rec._name)) {
                        fresh = *s;
                    }
                }) ||
                !fresh || fresh->_id != rec._id) {
                return service_detail::not_found(rec._name, rec._account);
            }
            rec = std::move(*fresh);
            batch = std::move(retry);
        }
    }

    [[nodiscard]] static auto arn_of(const call_context& ctx, const stream_record& s)
        -> std::string {
        return "arn:" + ctx._partition + ":kinesis:" + ctx._region + ":" + s._account + ":stream/" +
               s._name;
    }

    [[nodiscard]] static auto common_description(const call_context& ctx, const stream_record& rec)
        -> boost::json::object {
        boost::json::object d;
        d["StreamName"] = rec._name;
        d["StreamARN"] = arn_of(ctx, rec);
        d["StreamStatus"] = to_string(rec._status);
        d["StreamModeDetails"] = boost::json::object{{"StreamMode", to_string(rec._mode)}};
        d["RetentionPeriodHours"] = rec._retention_hours;
        d["StreamCreationTimestamp"] = service_detail::epoch_seconds(rec._created_ms);
        boost::json::array metrics;
        for (const auto& m : rec._shard_metrics) {
            metrics.emplace_back(m);
        }
        boost::json::array monitoring;
        monitoring.push_back(boost::json::object{{"ShardLevelMetrics", std::move(metrics)}});
        d["EnhancedMonitoring"] = std::move(monitoring);
        d["EncryptionType"] = rec._encryption == encryption_type::kms ? "KMS" : "NONE";
        if (!rec._key_id.empty()) {
            d["KeyId"] = rec._key_id;
        }
        return d;
    }

    [[nodiscard]] static auto sorted_shards(const stream_record& rec) -> std::vector<shard_entry> {
        auto shards = rec._shards;
        std::sort(shards.begin(), shards.end(), [](const shard_entry& a, const shard_entry& b) {
            return a._ordinal < b._ordinal;
        });
        return shards;
    }

    [[nodiscard]] static auto shard_json(const shard_entry& sh, const kinesis_compat_profile& p)
        -> boost::json::object {
        boost::json::object o;
        o["ShardId"] = format_shard_id(sh._ordinal, p._id_format);
        if (sh._parent) {
            o["ParentShardId"] = format_shard_id(*sh._parent, p._id_format);
        }
        if (sh._adjacent_parent) {
            o["AdjacentParentShardId"] = format_shard_id(*sh._adjacent_parent, p._id_format);
        }
        o["HashKeyRange"] = boost::json::object{{"StartingHashKey", to_decimal(sh._range._start)},
                                                {"EndingHashKey", to_decimal(sh._range._end)}};
        boost::json::object seq;
        seq["StartingSequenceNumber"] = format_sequence(
            sequence_number{._ordinal = sh._ordinal, ._counter = sh._starting_counter},
            p._id_format);
        if (sh._ending_counter) {
            // One past the shard's last record: AWS's EndingSequenceNumber
            // is a number no record carries (the task 17 recordings show a
            // closed parent's ending as new, never its last record's).
            seq["EndingSequenceNumber"] = format_sequence(
                sequence_number{._ordinal = sh._ordinal, ._counter = *sh._ending_counter + 1},
                p._id_format);
        }
        o["SequenceNumberRange"] = std::move(seq);
        return o;
    }

    [[nodiscard]] static auto make_shards_token(const stream_record& rec, std::uint32_t after)
        -> std::string {
        return service_detail::base64_encode("lsh1\n" + rec._account + "\n" + rec._name + "\n" +
                                             std::to_string(after) + "\n" +
                                             std::to_string(rec._created_ms));
    }

    [[nodiscard]] static auto parse_shards_token(std::string_view token)
        -> std::optional<shards_token> {
        auto decoded = service_detail::base64_decode(token);
        if (!decoded) {
            return std::nullopt;
        }
        std::vector<std::string> parts;
        std::string_view rest(*decoded);
        while (true) {
            const auto nl = rest.find('\n');
            parts.emplace_back(rest.substr(0, nl));
            if (nl == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(nl + 1);
        }
        if (parts.size() != 5 || parts[0] != "lsh1" || !valid_account(parts[1]) ||
            !valid_stream_name(parts[2])) {
            return std::nullopt;
        }
        const auto after = detail::parse_u64(parts[3]);
        if (!after || *after > UINT32_MAX) {
            return std::nullopt;
        }
        std::int64_t created = 0;
        try {
            std::size_t used = 0;
            created = std::stoll(parts[4], &used);
            if (used != parts[4].size()) {
                return std::nullopt;
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
        return shards_token{parts[1], parts[2], static_cast<std::uint32_t>(*after), created};
    }
};

}  // namespace kythira::kinesis
