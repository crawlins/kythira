// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file streams.hpp
/// @brief DynamoDB Streams over the change log
///        (`.kiro/specs/dynamodb-compatible-api/` task 16, Requirement 14).
///
/// A table's stream (`StreamSpecification`, `catalog.hpp`) is a view of
/// the change log every item group keeps (`change_log.hpp`, task 15):
///
/// - **Shards.** A shard is one store stream: one group in one epoch. A
///   split closes the parent's stream and opens one per child naming it,
///   which is the lineage `DescribeStream` shows as `ParentShardId`.
///   Merges are vetoed for groups holding a table with a stream
///   (`raft_backend::merge_vetoed`), so no shard has two parents. A stream
///   lists the open store stream of every group its table overlaps, and
///   each closed one that holds a record of the table written after the
///   stream was enabled.
/// - **Records.** Every logged write carries the label of the stream the
///   writer saw enabled; a stream's records are those of its table with
///   its label, whatever the clocks say. The store keeps both images, and
///   the view type picks what `GetRecords` shows.
/// - **Numbers.** A sequence number is the shard's epoch (6 digits) then
///   the record's place in its store stream (15 digits): 21 digits, as
///   AWS shows them, ordered within a shard. A shard id is
///   `shardId-<epoch, 20 digits>-<hash of the group's range start>`.
/// - **Iterators.** `<stream arn>|1|base64(store stream|next sequence|issued
///   ms)`, valid for 15 minutes as on AWS.
/// - **Retention.** Records stay readable for 24 hours; each item group's
///   leader proposes `trim_change_log` for older segments, closed streams
///   included (`raft_backend::trim_change_logs_once`).
///
/// Messages follow DynamoDB Local, probed for each case.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

namespace streams_detail {

/// How long an iterator stays valid.
inline constexpr std::uint64_t k_iterator_ttl_ms = std::uint64_t{15} * 60 * 1000;
/// Store reads one `GetRecords` makes before answering with what it has.
inline constexpr int k_max_reads = 4;

[[nodiscard]] inline auto fnv1a(std::string_view s, std::uint64_t h = 0xcbf29ce484222325ULL)
    -> std::uint64_t {
    for (const char ch : s) {
        h ^= static_cast<unsigned char>(ch);
        h *= 0x100000001b3ULL;
    }
    return h;
}

/// `shardId-<epoch>-<hash>` of a store stream.
[[nodiscard]] inline auto shard_id_of(std::string_view key) -> std::string {
    const auto parsed = parse_change_stream_key(key);
    const std::uint64_t epoch = parsed ? parsed->second : 0;
    const auto hash = static_cast<std::uint32_t>(fnv1a(parsed ? parsed->first : key));
    std::array<char, 48> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "shardId-%020llu-%08x",
                                 static_cast<unsigned long long>(epoch), hash);
    return {buf.data(), static_cast<std::size_t>(n)};
}

[[nodiscard]] inline auto epoch_of(std::string_view key) -> std::uint64_t {
    const auto parsed = parse_change_stream_key(key);
    return parsed ? parsed->second : 0;
}

/// The 21-digit sequence number of record `seq` of a store stream in
/// `epoch`.
[[nodiscard]] inline auto sequence_number(std::uint64_t epoch, std::uint64_t seq) -> std::string {
    std::array<char, 48> buf{};
    const auto n =
        std::snprintf(buf.data(), buf.size(), "%06llu%015llu",
                      static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(seq));
    return {buf.data(), static_cast<std::size_t>(n)};
}

/// The epoch and record a sequence number names, or nothing.
[[nodiscard]] inline auto parse_sequence_number(std::string_view text)
    -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    if (text.size() != 21) {
        return std::nullopt;
    }
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return std::nullopt;
        }
        auto& part = i < 6 ? epoch : seq;
        part = part * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    return std::pair{epoch, seq};
}

[[nodiscard]] inline auto to_u64(std::string_view text) -> std::optional<std::uint64_t> {
    if (text.empty() || text.size() > 19) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return std::nullopt;
        }
        v = v * 10 + static_cast<std::uint64_t>(ch - '0');
    }
    return v;
}

/// Where an iterator points.
struct iterator_position {
    std::string arn;
    std::string key;
    std::uint64_t seq = 0;
    std::uint64_t issued_ms = 0;
};

[[nodiscard]] inline auto encode_iterator(const iterator_position& p) -> std::string {
    const auto body = p.key + "|" + std::to_string(p.seq) + "|" + std::to_string(p.issued_ms);
    return p.arn + "|1|" + detail::base64_of(body);
}

[[nodiscard]] inline auto decode_iterator(std::string_view text)
    -> std::optional<iterator_position> {
    const auto bar = text.find('|');
    if (bar == std::string_view::npos || text.substr(bar, 3) != "|1|") {
        return std::nullopt;
    }
    iterator_position p;
    p.arn = std::string(text.substr(0, bar));
    const auto raw = aws_json::base64_decode(text.substr(bar + 3));
    if (!raw) {
        return std::nullopt;
    }
    const std::string body(reinterpret_cast<const char*>(raw->data()), raw->size());  // NOLINT
    const auto a = body.find('|');
    const auto b = a == std::string::npos ? a : body.find('|', a + 1);
    if (b == std::string::npos) {
        return std::nullopt;
    }
    p.key = body.substr(0, a);
    const auto seq = to_u64(std::string_view(body).substr(a + 1, b - a - 1));
    const auto issued = to_u64(std::string_view(body).substr(b + 1));
    if (!seq || !issued || !parse_change_stream_key(p.key)) {
        return std::nullopt;
    }
    p.seq = *seq;
    p.issued_ms = *issued;
    return p;
}

/// A stable, UUID-shaped `eventID` for a record, so a reader that sees it
/// twice can tell.
[[nodiscard]] inline auto event_id(std::string_view key, std::uint64_t seq) -> std::string {
    const auto seq_text = std::to_string(seq);
    const auto a = fnv1a(seq_text, fnv1a(key));
    const auto b = fnv1a(key, fnv1a(seq_text, 0x84222325cbf29ce4ULL));
    std::array<char, 48> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "%08llx-%04llx-4%03llx-%04llx-%012llx",
                                 static_cast<unsigned long long>(a >> 32U),
                                 static_cast<unsigned long long>((a >> 16U) & 0xFFFFU),
                                 static_cast<unsigned long long>(a & 0xFFFU),
                                 static_cast<unsigned long long>(0x8000U | ((b >> 48U) & 0x3FFFU)),
                                 static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFULL));
    return {buf.data(), static_cast<std::size_t>(n)};
}

[[nodiscard]] inline auto event_name(change_event e) -> std::string_view {
    switch (e) {
        case change_event::insert:
            return "INSERT";
        case change_event::modify:
            return "MODIFY";
        case change_event::remove:
            return "REMOVE";
    }
    return "INSERT";
}

[[nodiscard]] inline auto not_found_stream(std::string_view arn) -> aws_json::error {
    return table_api_detail::modeled(
        "ResourceNotFoundException",
        "Requested resource not found: Stream: " + std::string(arn) + " not found");
}

[[nodiscard]] inline auto no_shard() -> aws_json::error {
    return table_api_detail::modeled("ResourceNotFoundException", "Shard does not exist");
}

[[nodiscard]] inline auto bad_iterator() -> aws_json::error {
    return validation_error("Invalid ShardIterator");
}

[[nodiscard]] inline auto invalid_sequence() -> aws_json::error {
    return table_api_detail::invalid("Invalid SequenceNumber");
}

[[nodiscard]] inline auto trimmed() -> aws_json::error {
    return table_api_detail::modeled(
        "TrimmedDataAccessException",
        "The operation attempted to read past the oldest stream record in a shard.");
}

[[nodiscard]] inline auto expired(std::uint64_t issued, std::uint64_t now) -> aws_json::error {
    return table_api_detail::modeled(
        "ExpiredIteratorException",
        "Iterator expired. The iterator was created at time " + std::to_string(issued) +
            " while right now it is " + std::to_string(now) +
            " which is further in the future than the tolerated delay of " +
            std::to_string(k_iterator_ttl_ms) + " milliseconds.");
}

}  // namespace streams_detail

/// The four DynamoDB Streams actions over a backend's change log.
template<typename Backend>
requires dynamo_backend<Backend> && change_feed_backend<Backend>
class streams_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit streams_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("ListStreams",
              [this](call_context& c, boost::json::object& in) { return list_streams(c, in); });
        fe.on("DescribeStream",
              [this](call_context& c, boost::json::object& in) { return describe_stream(c, in); });
        fe.on("GetShardIterator", [this](call_context& c, boost::json::object& in) {
            return get_shard_iterator(c, in);
        });
        fe.on("GetRecords",
              [this](call_context& c, boost::json::object& in) { return get_records(c, in); });
    }

    /// Newest first, per table; a table that does not exist has none.
    auto list_streams(call_context& ctx, boost::json::object& in) -> result {
        std::vector<std::string> tables;
        if (const auto* t = in.if_contains("TableName")) {
            tables.push_back(ctx.table_name(t->as_string()));
        } else {
            tables = _backend.list_tables("", SIZE_MAX);
            std::erase_if(tables, [&](const std::string& n) { return !ctx.owns(n); });
        }
        const auto limit = in.contains("Limit")
                               ? static_cast<std::size_t>(in.at("Limit").to_number<std::int64_t>())
                               : std::size_t{100};
        std::optional<std::string> after;
        if (const auto* a = in.if_contains("ExclusiveStartStreamArn")) {
            after = std::string(a->as_string());
        }
        const auto now = _backend.now_ms();
        boost::json::array out;
        std::optional<std::string> last;
        bool skipping = after.has_value();
        bool more = false;
        for (const auto& name : tables) {
            const auto d = _backend.describe(name);
            if (!d) {
                continue;
            }
            for (auto s = d->streams.rbegin(); s != d->streams.rend(); ++s) {
                if (!listed(*s, now)) {
                    continue;
                }
                auto arn = table_api_detail::stream_arn(ctx, d->name, s->label_ms);
                if (skipping) {
                    skipping = arn != *after;
                    continue;
                }
                if (out.size() == limit) {
                    more = true;
                    break;
                }
                out.push_back(boost::json::object{
                    {"StreamArn", arn},
                    {"TableName", call_context::bare_name(d->name)},
                    {"StreamLabel", table_api_detail::stream_label(s->label_ms)}});
                last = std::move(arn);
            }
            if (more) {
                break;
            }
        }
        boost::json::object r{{"Streams", std::move(out)}};
        if (more && last) {
            r["LastEvaluatedStreamArn"] = *last;
        }
        return r;
    }

    auto describe_stream(call_context& ctx, boost::json::object& in) -> result {
        using namespace streams_detail;
        const std::string arn(in.at("StreamArn").as_string());
        const auto found = find_stream(ctx, arn);
        if (!found) {
            return not_found_stream(arn);
        }
        const auto& [table, stream] = *found;
        const auto limit = in.contains("Limit")
                               ? static_cast<std::size_t>(in.at("Limit").to_number<std::int64_t>())
                               : std::size_t{100};
        std::optional<std::string> after;
        if (const auto* a = in.if_contains("ExclusiveStartShardId")) {
            after = std::string(a->as_string());
        }
        const auto shards = shards_of(table, stream);
        std::set<std::string> listed_keys;
        for (const auto& s : shards) {
            listed_keys.insert(s.stream.key);
        }
        boost::json::array out;
        bool skipping = after.has_value();
        std::optional<std::string> last;
        bool more = false;
        for (const auto& s : shards) {
            auto id = shard_id_of(s.stream.key);
            if (skipping) {
                skipping = id != *after;
                continue;
            }
            if (out.size() == limit) {
                more = true;
                break;
            }
            const auto epoch = epoch_of(s.stream.key);
            boost::json::object range{
                {"StartingSequenceNumber", sequence_number(epoch, s.stream.first_seq)}};
            if (!s.open || stream.disabled_ms != 0) {
                range["EndingSequenceNumber"] =
                    sequence_number(epoch, std::max(s.stream.last_seq, s.stream.first_seq));
            }
            boost::json::object shard{{"ShardId", id}, {"SequenceNumberRange", std::move(range)}};
            for (const auto& p : s.stream.parents) {
                if (listed_keys.contains(p)) {
                    shard["ParentShardId"] = shard_id_of(p);
                    break;
                }
            }
            out.push_back(std::move(shard));
            last = std::move(id);
        }
        boost::json::array keys{boost::json::object{{"AttributeName", table.schema.partition_name},
                                                    {"KeyType", "HASH"}}};
        if (table.schema.sort_name) {
            keys.push_back(boost::json::object{{"AttributeName", *table.schema.sort_name},
                                               {"KeyType", "RANGE"}});
        }
        boost::json::object desc{
            {"StreamArn", arn},
            {"StreamLabel", table_api_detail::stream_label(stream.label_ms)},
            {"StreamStatus", stream.disabled_ms == 0 ? "ENABLED" : "DISABLED"},
            {"StreamViewType", table_api_detail::view_type_name(stream.view)},
            {"CreationRequestDateTime", table_api_detail::millis_to_seconds(stream.label_ms)},
            {"TableName", call_context::bare_name(table.name)},
            {"KeySchema", std::move(keys)},
            {"Shards", std::move(out)},
        };
        if (more && last) {
            desc["LastEvaluatedShardId"] = *last;
        }
        return boost::json::object{{"StreamDescription", std::move(desc)}};
    }

    auto get_shard_iterator(call_context& ctx, boost::json::object& in) -> result {
        using namespace streams_detail;
        const std::string arn(in.at("StreamArn").as_string());
        const std::string shard_id(in.at("ShardId").as_string());
        const std::string_view type = in.at("ShardIteratorType").as_string();
        const auto* seq_text = in.if_contains("SequenceNumber");
        const bool positioned = type == "AT_SEQUENCE_NUMBER" || type == "AFTER_SEQUENCE_NUMBER";
        if (!positioned && seq_text != nullptr) {
            return table_api_detail::invalid(
                "SequenceNumber should not be passed for this "
                "iterator type");
        }
        if (positioned && seq_text == nullptr) {
            return invalid_sequence();
        }
        const auto found = find_stream(ctx, arn);
        if (!found) {
            return not_found_stream(arn);
        }
        const auto& [table, stream] = *found;
        const change_shard* shard = nullptr;
        const auto shards = shards_of(table, stream);
        for (const auto& s : shards) {
            if (shard_id_of(s.stream.key) == shard_id) {
                shard = &s;
                break;
            }
        }
        if (shard == nullptr) {
            return no_shard();
        }
        iterator_position p;
        p.arn = arn;
        p.key = shard->stream.key;
        p.issued_ms = _backend.now_ms();
        if (type == "TRIM_HORIZON") {
            p.seq = shard->stream.first_seq;
        } else if (type == "LATEST") {
            p.seq = shard->stream.last_seq + 1;
        } else {
            const auto at = parse_sequence_number(seq_text->as_string());
            if (!at || at->first != epoch_of(shard->stream.key) || at->second == 0 ||
                at->second > shard->stream.last_seq) {
                return invalid_sequence();
            }
            if (at->second < shard->stream.first_seq) {
                return trimmed();
            }
            p.seq = type == "AT_SEQUENCE_NUMBER" ? at->second : at->second + 1;
        }
        return boost::json::object{{"ShardIterator", encode_iterator(p)}};
    }

    auto get_records(call_context& ctx, boost::json::object& in) -> result {
        using namespace streams_detail;
        const auto pos = decode_iterator(in.at("ShardIterator").as_string());
        if (!pos) {
            return bad_iterator();
        }
        const auto now = _backend.now_ms();
        if (now > pos->issued_ms + k_iterator_ttl_ms) {
            return expired(pos->issued_ms, now);
        }
        const auto found = find_stream(ctx, pos->arn);
        if (!found) {
            return not_found_stream(pos->arn);
        }
        const auto& [table, stream] = *found;
        const auto limit = in.contains("Limit")
                               ? static_cast<std::size_t>(in.at("Limit").to_number<std::int64_t>())
                               : std::size_t{1000};
        const auto epoch = epoch_of(pos->key);
        boost::json::array records;
        std::uint64_t next = pos->seq;
        change_page page;
        for (int read = 0; read < k_max_reads && records.size() < limit; ++read) {
            page = _backend.read_changes(pos->key, next, limit - records.size());
            if (!page.known) {
                return no_shard();
            }
            if (read == 0 && next < page.first_seq) {
                return trimmed();
            }
            for (const auto& [seq, rec] : page.records) {
                next = seq + 1;
                if (rec.table_id == table.table_id && in_stream(table, stream, rec)) {
                    records.push_back(record_json(ctx, pos->key, epoch, seq, rec, stream.view));
                }
            }
            if (page.records.empty() || next > page.last_seq) {
                break;
            }
        }
        boost::json::object r{{"Records", std::move(records)}};
        const bool ended = (!page.open || stream.disabled_ms != 0) && next > page.last_seq;
        if (!ended) {
            iterator_position p = *pos;
            p.seq = next;
            p.issued_ms = now;
            r["NextShardIterator"] = encode_iterator(p);
        }
        return r;
    }

private:
    /// Whether `rec` belongs to `stream`. A record carries the label of
    /// the stream its writer saw enabled. A strongly consistent table's
    /// stream is each region's own while every region writes (task 27.7),
    /// and AWS gives every replica the same records, so there a record
    /// belongs to the stream by when it was written instead: from the
    /// stream's label until it was disabled.
    [[nodiscard]] static auto in_stream(const table_descriptor& table, const table_stream& stream,
                                        const change_record& rec) -> bool {
        if (!table.strong) {
            return rec.stream_ms == stream.label_ms;
        }
        return rec.written_ms >= stream.label_ms &&
               (stream.disabled_ms == 0 || rec.written_ms < stream.disabled_ms);
    }

    /// Whether a stream is still listed: enabled, or disabled less than
    /// retention ago.
    [[nodiscard]] static auto listed(const table_stream& s, std::uint64_t now) -> bool {
        return s.disabled_ms == 0 || s.disabled_ms + k_stream_retention_ms >= now;
    }

    /// The table and stream an ARN names in this account and region.
    auto find_stream(const call_context& ctx, std::string_view arn)
        -> std::optional<std::pair<table_descriptor, table_stream>> {
        const auto at = arn.rfind("/stream/");
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        const auto name = ctx.table_name(arn.substr(0, at));
        if (ctx.table_arn(name) != arn.substr(0, at)) {
            return std::nullopt;
        }
        auto d = _backend.describe(name);
        if (!d) {
            return std::nullopt;
        }
        const auto label = arn.substr(at + 8);
        const auto now = _backend.now_ms();
        for (const auto& s : d->streams) {
            if (table_api_detail::stream_label(s.label_ms) == label && listed(s, now)) {
                return std::pair{std::move(*d), s};
            }
        }
        return std::nullopt;
    }

    /// The stream's shards, parents before children: every open store
    /// stream of a group the table overlaps, and each closed one holding a
    /// record of the table written since the stream was enabled.
    auto shards_of(const table_descriptor& table, const table_stream& stream)
        -> std::vector<change_shard> {
        auto all = _backend.change_shards(table_range(table.table_id));
        std::erase_if(all, [&](const change_shard& s) {
            if (s.open) {
                return false;
            }
            if (!std::ranges::binary_search(s.stream.tables, table.table_id)) {
                return true;
            }
            return !s.stream.sealed.empty() &&
                   static_cast<std::uint64_t>(s.stream.sealed.back()._last_timestamp_ms) <
                       stream.label_ms;
        });
        std::ranges::sort(all, [](const change_shard& a, const change_shard& b) {
            const auto ea = streams_detail::epoch_of(a.stream.key);
            const auto eb = streams_detail::epoch_of(b.stream.key);
            return ea != eb ? ea < eb : a.stream.key < b.stream.key;
        });
        return all;
    }

    /// One record as `GetRecords` shows it, with the images `view` asks
    /// for.
    static auto record_json(const call_context& ctx, const std::string& key, std::uint64_t epoch,
                            std::uint64_t seq, const change_record& rec, stream_view_type view)
        -> boost::json::object {
        using namespace streams_detail;
        const bool new_image =
            view == stream_view_type::new_image || view == stream_view_type::new_and_old_images;
        const bool old_image =
            view == stream_view_type::old_image || view == stream_view_type::new_and_old_images;
        std::size_t size = item_size(rec.keys);
        // Seconds since the epoch, rounded down to the minute, as DynamoDB
        // Local shows it.
        const auto minute_ms = rec.written_ms / 60000 * 60000;
        boost::json::object body{
            {"ApproximateCreationDateTime", table_api_detail::millis_to_seconds(minute_ms)},
            {"Keys", item_to_json(rec.keys)},
        };
        if (new_image && rec.new_image) {
            body["NewImage"] = item_to_json(*rec.new_image);
            size += item_size(*rec.new_image);
        }
        if (old_image && rec.old_image) {
            body["OldImage"] = item_to_json(*rec.old_image);
            size += item_size(*rec.old_image);
        }
        body["SequenceNumber"] = sequence_number(epoch, seq);
        body["SizeBytes"] = size;
        body["StreamViewType"] = table_api_detail::view_type_name(view);
        boost::json::object out{
            {"eventID", event_id(key, seq)}, {"eventName", event_name(rec.event)},
            {"eventVersion", "1.1"},         {"eventSource", "aws:dynamodb"},
            {"awsRegion", ctx.region},       {"dynamodb", std::move(body)},
        };
        if (rec.ttl_delete) {
            out["userIdentity"] =
                boost::json::object{{"PrincipalId", "dynamodb.amazonaws.com"}, {"Type", "Service"}};
        }
        return out;
    }

    Backend& _backend;
};

}  // namespace kythira::dynamodb
