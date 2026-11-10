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
/// SDK waiters see, and a retried DeleteStream finishes the job).
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
/// Not here yet: consumers (EnforceConsumerDeletion has nothing to enforce
/// until task 13), naming a stream by `StreamId` and `DryRun` (refused with
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
#include <kythira/kinesis/kinesis_records.hpp>
#include <kythira/kinesis/kinesis_sequence.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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
    /// Appends that find their shard closed are retried this many times.
    int _closed_shard_retries = 3;
    /// Signs and checks shard iterators; every gateway of a cluster needs
    /// the same ring. Empty: the service makes a random key, which suits
    /// one node only (iterators from other nodes would not verify).
    iterator_key_ring _iterator_keys{};
    /// AWS's per-shard rates (task 8). Writes meet them on the proposing
    /// leader (the cluster's `write_throttle`); reads on this node.
    throughput_limits _throughput{};
    /// The clock the read buckets refill by.
    monotonic_ms _monotonic = steady_now_ms;
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
    }

    /// The operations `register_with` adds, for implemented-operations.txt.
    static constexpr std::array<std::string_view, 11> k_operations{
        "CreateStream",          "DeleteStream", "DescribeLimits",   "DescribeStream",
        "DescribeStreamSummary", "GetRecords",   "GetShardIterator", "ListShards",
        "ListStreams",           "PutRecord",    "PutRecords"};

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
            return service_detail::in_use("Stream " + name + " under account " + account +
                                          " already exists.");
        }
        if (created->_status != apply_status::ok) {
            return internal_failure();
        }
        const auto id = created->_id;
        for (std::uint32_t ordinal = 0; ordinal < count; ++ordinal) {
            const auto opened = propose(open_shard_command{routing_key(id, ordinal), 0});
            if (!opened || (opened->_status != apply_status::ok &&
                            opened->_status != apply_status::already_exists)) {
                // Left in CREATING; a retry of CreateStream reports it in
                // use, and DeleteStream clears it.
                return internal_failure();
            }
        }
        const auto active =
            propose(set_stream_status_command{account, name, id, stream_status::active});
        if (!active || active->_status != apply_status::ok) {
            return internal_failure();
        }
        return boost::json::object{};
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
        d["StreamId"] = stream_id_text(rec._id);
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
        if (r._error) {
            return internal_failure();
        }
        boost::json::object out;
        out["ShardId"] = r._shard_id;
        out["SequenceNumber"] = r._sequence;
        out["EncryptionType"] = "NONE";
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
        for (const auto& r : *results) {
            boost::json::object entry;
            if (r._throttled) {
                ++failed;
                entry["ErrorCode"] = "ProvisionedThroughputExceededException";
                entry["ErrorMessage"] = r._message;
            } else if (r._error) {
                ++failed;
                entry["ErrorCode"] = "InternalFailure";
                entry["ErrorMessage"] = "Internal service failure.";
            } else {
                entry["SequenceNumber"] = r._sequence;
                entry["ShardId"] = r._shard_id;
            }
            records.push_back(std::move(entry));
        }
        boost::json::object out;
        out["FailedRecordCount"] = failed;
        out["Records"] = std::move(records);
        out["EncryptionType"] = "NONE";
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
            return throughput_exceeded(
                shard_rate_exceeded(format_shard_id(entry->_ordinal, profile._id_format), rec));
        }
        const auto state = read_shard_state(key);
        if (!state) {
            return internal_failure();
        }
        if (!state->_found) {
            return shard_not_found(shard_text, rec);
        }
        const auto first = std::max(entry->_starting_counter + 1, state->_state._trim_before);
        shard_iterator it;
        it._account = rec._account;
        it._stream = rec._name;
        it._stream_id = rec._id;
        it._ordinal = entry->_ordinal;
        const std::string type(*detail::string_member(in, "ShardIteratorType"));
        if (type == "TRIM_HORIZON") {
            it._counter = first;
        } else if (type == "LATEST") {
            it._counter = entry->_ending_counter ? *entry->_ending_counter + 1
                                                 : state->_state._last_counter + 1;
        } else if (type == "AT_SEQUENCE_NUMBER" || type == "AFTER_SEQUENCE_NUMBER") {
            const auto text = detail::string_member(in, "StartingSequenceNumber");
            if (!text) {
                return invalid_argument(
                    "StartingSequenceNumber must be provided for ShardIteratorType "
                    "AT_SEQUENCE_NUMBER or AFTER_SEQUENCE_NUMBER.");
            }
            auto seq = parse_sequence(*text, sequence_profile::aws);
            if (!seq) {
                seq = parse_sequence(*text, sequence_profile::compact);
            }
            if (!seq) {
                return invalid_argument("StartingSequenceNumber " + std::string(*text) +
                                        " used in GetShardIterator on shard " + shard_text +
                                        " in stream " + rec._name + " under account " +
                                        rec._account + " is invalid.");
            }
            if (seq->_ordinal != entry->_ordinal) {
                return invalid_argument("Invalid StartingSequenceNumber. It encodes " +
                                        format_shard_id(seq->_ordinal, profile._id_format) +
                                        ", while it was used in a call to a shard with " +
                                        format_shard_id(entry->_ordinal, profile._id_format));
            }
            it._counter = seq->_counter + (type == "AFTER_SEQUENCE_NUMBER" ? 1 : 0);
        } else if (type == "AT_TIMESTAMP") {
            const auto ts = service_detail::number_member(in, "Timestamp");
            if (!ts) {
                return invalid_argument(
                    "Timestamp must be provided for ShardIteratorType AT_TIMESTAMP.");
            }
            it._counter = first;
            it._min_timestamp_ms =
                std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(*ts * 1000.0)));
        } else {
            return invalid_argument("Invalid ShardIteratorType: " + type + ".");
        }
        it._issued_ms = _cluster.now_ms();
        boost::json::object out;
        out["ShardIterator"] = encode_iterator(it, _config._iterator_keys);
        return out;
    }

    auto get_records(call_context& ctx, boost::json::object& in) -> result {
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

        std::optional<shard_read> got;
        std::int64_t tip_arrival = 0;
        for (int attempt = 0; attempt < 2 && (!got || got->_missing); ++attempt) {
            // A segment can go missing between the copy and the read (a trim
            // or tiering in between): read again with a fresh copy once.
            const auto state = read_shard_state(key);
            if (!state || !state->_found || state->_store == nullptr) {
                if (state && !state->_found) {
                    return shard_not_found(format_shard_id(it._ordinal, profile._id_format), rec);
                }
                return internal_failure();
            }
            got = read_shard(*state->_store, state->_fetch, key, state->_state, it._counter,
                             it._min_timestamp_ms, limits);
            tip_arrival = state->_state._last_arrival_ms;
        }
        if (!got || got->_missing) {
            return internal_failure();
        }
        if (profile._throttling) {
            std::uint64_t bytes = 0;
            for (const auto& rr : got->_records) {
                bytes += rr._record._data.size() + rr._record._partition_key.size();
            }
            _reads.charge(key, bytes);
        }

        boost::json::array records;
        for (const auto& rr : got->_records) {
            boost::json::object o;
            o["SequenceNumber"] =
                format_sequence(sequence_number{._ordinal = it._ordinal, ._counter = rr._counter},
                                profile._id_format);
            o["ApproximateArrivalTimestamp"] = service_detail::epoch_seconds(rr._arrival_ms);
            o["Data"] = service_detail::base64_encode(std::string_view(
                reinterpret_cast<const char*>(rr._record._data.data()), rr._record._data.size()));
            o["PartitionKey"] = rr._record._partition_key;
            records.push_back(std::move(o));
        }
        boost::json::object out;
        std::int64_t behind = 0;
        if (!got->_caught_up && !got->_records.empty()) {
            behind = std::max<std::int64_t>(0, now - got->_records.back()._arrival_ms);
        } else if (!got->_caught_up) {
            behind = std::max<std::int64_t>(0, now - tip_arrival);
        }
        out["MillisBehindLatest"] = behind;
        const bool exhausted =
            entry->_ending_counter && got->_next_counter > *entry->_ending_counter;
        if (exhausted) {
            // A closed shard read to its end: no next iterator, and the
            // children to move to (Requirement 9.3).
            out["MillisBehindLatest"] = 0;
            boost::json::array children;
            for (const auto& sh : sorted_shards(rec)) {
                if (sh._parent != it._ordinal && sh._adjacent_parent != it._ordinal) {
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
                children.push_back(std::move(c));
            }
            out["ChildShards"] = std::move(children);
        } else {
            shard_iterator next = it;
            next._counter = got->_next_counter;
            if (!got->_records.empty()) {
                next._min_timestamp_ms = 0;  // resolved: a plain position now
            }
            next._issued_ms = now;
            out["NextShardIterator"] = encode_iterator(next, _config._iterator_keys);
        }
        out["Records"] = std::move(records);
        return out;
    }

private:
    Cluster& _cluster;
    kinesis_service_config _config;
    read_throttle _reads;

    [[nodiscard]] static auto shard_rate_exceeded(std::string_view shard, const stream_record& rec)
        -> std::string {
        return "Rate exceeded for shard " + std::string(shard) + " in stream " + rec._name +
               " under account " + rec._account + ".";
    }

    [[nodiscard]] static auto throughput_exceeded(std::string message) -> aws_json::error {
        return kinesis_error_style().modeled("ProvisionedThroughputExceededException",
                                             std::move(message));
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
        if (r._data.size() + r._partition_key.size() > _config._limits._max_record_bytes) {
            return invalid_argument(
                "Record size including partition key exceeds the maximum "
                "record size of 1024 KiB for this stream.");
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
            std::vector<service_detail::pending_record> retry;
            std::vector<shard_append> appends;
            appends.reserve(by_shard.size());
            for (auto& [ordinal, records] : by_shard) {
                shard_append a{._ordinal = ordinal, ._records = std::move(records), ._command = {}};
                a._command._key = routing_key(rec._id, ordinal);
                a._command._arrival_ms = _cluster.now_ms();
                for (const auto* r : a._records) {
                    a._command._records.push_back({r->_partition_key, r->_data});
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
                for (std::size_t i = 0; i < a._records.size(); ++i) {
                    auto& result = out[a._records[i]->_index];
                    result._error = !ok || i >= admitted;
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

    /// AWS's StreamId shape is `[a-z0-9]{20}-[a-z0-9]{3}`: the catalog's
    /// stream id in twenty decimal digits and a fixed `kyt` suffix.
    [[nodiscard]] static auto stream_id_text(std::uint64_t id) -> std::string {
        auto digits = std::to_string(id);
        return std::string(20 - digits.size(), '0') + digits + "-kyt";
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
        boost::json::array monitoring;
        monitoring.push_back(boost::json::object{{"ShardLevelMetrics", boost::json::array{}}});
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
            seq["EndingSequenceNumber"] = format_sequence(
                sequence_number{._ordinal = sh._ordinal, ._counter = *sh._ending_counter},
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
