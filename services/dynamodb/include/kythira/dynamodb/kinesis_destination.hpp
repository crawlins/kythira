// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_destination.hpp
/// @brief Kinesis streaming destinations: `EnableKinesisStreamingDestination`,
///        `DisableKinesisStreamingDestination`,
///        `DescribeKinesisStreamingDestination`,
///        `UpdateKinesisStreamingDestination` and the publisher behind them
///        (`.kiro/specs/dynamodb-compatible-api/` task 17, Requirement 16).
///
/// A table's destinations live in its catalog descriptor
/// (`kinesis_destination`). As on AWS a table streams to one Kinesis data
/// stream at a time; an enable answers `ENABLING`, a disable `DISABLING` and
/// an update `UPDATING`, and the publisher completes each.
///
/// While a destination streams, the table's writes append change records
/// (task 15), and `kinesis_publisher` reads them back through the same
/// `change_feed_backend` DynamoDB Streams uses (task 16) and puts them to
/// the destination with `PutRecords`, signed with SigV4. The endpoint can be
/// AWS Kinesis or a Kythira `kinesis_gateway_node`
/// (`kinesis-compatible-streams`): both speak the same protocol.
///
/// Each record is the JSON document AWS sends: `awsRegion`, `eventID`,
/// `eventName`, `userIdentity` (the TTL service's for a TTL delete, else
/// null), `recordFormat` `application/json`, `tableName`, `eventSource`
/// `aws:dynamodb`, and `dynamodb` with `ApproximateCreationDateTime` (in
/// milliseconds or microseconds since the epoch, per the destination's
/// precision; the change log keeps milliseconds, so microseconds end in
/// 000), `ApproximateCreationDateTimePrecision`, `Keys`, `NewImage`,
/// `OldImage` and `SizeBytes`. A destination always gets both images.
/// The Kinesis partition key is a hash of the item's key, so one item's
/// records always land in one Kinesis shard. `eventID` is the one DynamoDB
/// Streams gives the same record, stable across a resend.
///
/// Delivery is at least once and in order per item (16.2):
/// - Records of one item are in one change log stream, in order; a stream
///   a split continued is published only after its parent is drained.
/// - A `PutRecords` call carries at most one record per item, so a
///   per-record failure never lets a later record of that item overtake
///   it. The cursor stops at the first failed record; the records after it
///   that did land are sent again on the next pass (duplicates, never
///   gaps).
/// - Cursors (the next sequence number per change log stream) are stored
///   in the descriptor with a `destination_progress` entry after each pass
///   that moved them, so a new catalog leader resumes where the last left
///   off, resending at most the last unrecorded pass.
/// Records more than 24 hours old are trimmed from the change log (task
/// 16); a destination that cannot be reached for that long loses them, as
/// on AWS.
///
/// The gateway runs `pass()` once a second next to the TTL sweeper: on
/// every node for `local_backend`, on the catalog leader for
/// `raft_backend`.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/change_log.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/streams.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>
#include <raft/sigv4_verifier.hpp>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// One record for `PutRecords`.
struct kinesis_put_record {
    std::string data;
    std::string partition_key;
};

/// What `kinesis_sink::check_stream` found.
struct kinesis_stream_check {
    enum class outcome : std::uint8_t {
        /// The stream exists and the caller may put to it.
        ok,
        /// It does not, or the caller may not use it: the destination fails
        /// to enable, with `description`.
        refused,
        /// No answer: try again later.
        unreachable,
    };
    outcome result = outcome::unreachable;
    std::string description;
};

/// Where a destination's records go. The gateway's default speaks to a
/// Kinesis endpoint over HTTP (`kinesis_http_sink`); tests put fakes here.
struct kinesis_sink {
    /// Whether the stream named by ARN can be streamed to.
    std::function<kinesis_stream_check(const std::string& stream_arn)> check_stream;
    /// Puts `records`, in order. Returns, per record, whether it landed, or
    /// nothing when the call got no usable answer (none landed, as far as
    /// the caller can tell).
    std::function<std::optional<std::vector<bool>>(const std::string& stream_arn,
                                                   const std::vector<kinesis_put_record>& records)>
        put_records;

    [[nodiscard]] auto configured() const -> bool { return check_stream && put_records; }
};

namespace kinesis_destination_detail {

/// `arn:<partition>:kinesis:<region>:<account>:stream/<name>`: its region,
/// or nothing when it is not a stream ARN.
[[nodiscard]] inline auto stream_arn_region(std::string_view arn) -> std::optional<std::string> {
    std::array<std::string_view, 6> parts{};
    std::size_t start = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto colon = arn.find(':', start);
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        parts.at(i) = arn.substr(start, colon - start);
        start = colon + 1;
    }
    parts[5] = arn.substr(start);
    if (parts[0] != "arn" || parts[1].empty() || parts[2] != "kinesis" || parts[3].empty() ||
        parts[4].empty() || !parts[5].starts_with("stream/") || parts[5].size() <= 7 ||
        parts[5].find('/', 7) != std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(parts[3]);
}

[[nodiscard]] inline auto status_name(destination_status s) -> std::string_view {
    switch (s) {
        case destination_status::enabling:
            return "ENABLING";
        case destination_status::active:
            return "ACTIVE";
        case destination_status::disabling:
            return "DISABLING";
        case destination_status::disabled:
            return "DISABLED";
        case destination_status::enable_failed:
            return "ENABLE_FAILED";
        case destination_status::updating:
            return "UPDATING";
    }
    return "DISABLED";
}

[[nodiscard]] inline auto precision_name(time_precision p) -> std::string_view {
    return p == time_precision::microsecond ? "MICROSECOND" : "MILLISECOND";
}

[[nodiscard]] inline auto precision_of(std::string_view name) -> std::optional<time_precision> {
    if (name == "MILLISECOND") {
        return time_precision::millisecond;
    }
    if (name == "MICROSECOND") {
        return time_precision::microsecond;
    }
    return std::nullopt;
}

/// The Kinesis partition key of an item: 32 hex digits of a hash of its
/// key, so one item always maps to one Kinesis shard.
[[nodiscard]] inline auto partition_key_of(std::string_view item_key) -> std::string {
    const auto a = streams_detail::fnv1a(item_key);
    const auto b = streams_detail::fnv1a(item_key, 0x84222325cbf29ce4ULL);
    std::array<char, 40> buf{};
    const auto n =
        std::snprintf(buf.data(), buf.size(), "%016llx%016llx", static_cast<unsigned long long>(a),
                      static_cast<unsigned long long>(b));
    return {buf.data(), static_cast<std::size_t>(n)};
}

/// One change record as AWS puts it to a destination's stream.
[[nodiscard]] inline auto record_document(std::string_view region, std::string_view table,
                                          const std::string& stream_key, std::uint64_t seq,
                                          const change_record& rec, time_precision precision)
    -> boost::json::object {
    std::size_t size = item_size(rec.keys);
    boost::json::object body{
        {"ApproximateCreationDateTime",
         precision == time_precision::microsecond ? rec.written_ms * 1000 : rec.written_ms},
        {"ApproximateCreationDateTimePrecision", precision_name(precision)},
        {"Keys", item_to_json(rec.keys)},
    };
    if (rec.new_image) {
        body["NewImage"] = item_to_json(*rec.new_image);
        size += item_size(*rec.new_image);
    }
    if (rec.old_image) {
        body["OldImage"] = item_to_json(*rec.old_image);
        size += item_size(*rec.old_image);
    }
    body["SizeBytes"] = size;
    boost::json::object out{
        {"awsRegion", region},
        {"eventID", streams_detail::event_id(stream_key, seq)},
        {"eventName", streams_detail::event_name(rec.event)},
        {"userIdentity", nullptr},
        {"recordFormat", "application/json"},
        {"tableName", table},
        {"dynamodb", std::move(body)},
        {"eventSource", "aws:dynamodb"},
    };
    if (rec.ttl_delete) {
        out["userIdentity"] =
            boost::json::object{{"principalId", "dynamodb.amazonaws.com"}, {"type", "Service"}};
    }
    return out;
}

}  // namespace kinesis_destination_detail

struct kinesis_publisher_options {
    /// The region records name (`awsRegion`): the gateway's.
    std::string region = "us-east-1";
    /// Records per `PutRecords` call (Kinesis allows 500).
    std::size_t max_batch_records = 500;
    /// Data bytes per call (Kinesis allows 5 MiB; leave room for keys).
    std::size_t max_batch_bytes = std::size_t{4} << 20U;
    /// Change records read per page.
    std::size_t page_records = 1000;
    /// Change records looked at per destination per pass, so one busy
    /// table cannot hold the others back.
    std::size_t max_records_per_pass = 20000;
};

/// Publishes change records to Kinesis streaming destinations.
template<typename Backend>
requires dynamo_backend<Backend> && change_feed_backend<Backend>
class kinesis_publisher {
public:
    kinesis_publisher(Backend& backend, kinesis_sink sink, kinesis_publisher_options options = {})
        : _backend(backend), _sink(std::move(sink)), _options(std::move(options)) {}

    void set_sink(kinesis_sink sink) { _sink = std::move(sink); }
    void set_region(std::string region) { _options.region = std::move(region); }

    /// One pass over every table's destinations. Returns the records that
    /// landed.
    auto pass() -> std::size_t {
        if (!_sink.configured()) {
            return 0;
        }
        std::size_t landed = 0;
        for (const auto& name : _backend.list_tables("", SIZE_MAX)) {
            const auto d = _backend.describe(name);
            if (!d || d->destinations.empty()) {
                continue;
            }
            for (const auto& k : d->destinations) {
                landed += serve(*d, k);
            }
        }
        return landed;
    }

private:
    using ds = destination_status;

    auto serve(const table_descriptor& d, const kinesis_destination& k) -> std::size_t {
        switch (k.status) {
            case ds::enabling: {
                const auto check = _sink.check_stream(k.stream_arn);
                if (check.result == kinesis_stream_check::outcome::ok) {
                    progress(d, k, ds::active, {}, k.cursors);
                } else if (check.result == kinesis_stream_check::outcome::refused) {
                    progress(d, k, ds::enable_failed, check.description, {});
                }
                return 0;
            }
            case ds::disabling:
                progress(d, k, ds::disabled, {}, {});
                return 0;
            case ds::updating: {
                // Records from here on take the new precision.
                auto now = k;
                now.status = ds::active;
                progress(d, k, ds::active, {}, k.cursors);
                return publish(d, now);
            }
            case ds::active:
                return publish(d, k);
            case ds::disabled:
            case ds::enable_failed:
                return 0;
        }
        return 0;
    }

    void progress(const table_descriptor& d, const kinesis_destination& k, ds to,
                  std::string description,
                  std::vector<std::pair<std::string, std::uint64_t>> cursors) {
        catalog_command c;
        c.op = catalog_command::kind::destination_progress;
        c.table.name = d.name;
        kinesis_destination got;
        got.stream_arn = k.stream_arn;
        got.status = to;
        got.precision = k.precision;
        got.status_description = std::move(description);
        got.enabled_ms = k.enabled_ms;
        got.cursors = std::move(cursors);
        c.table.destinations.push_back(std::move(got));
        c.now_ms = _backend.now_ms();
        (void)_backend.catalog(c);
    }

    struct pending {
        std::uint64_t seq = 0;
        kinesis_put_record record;
    };

    /// Publishes what the destination has not yet seen, within the pass's
    /// budget, then records the cursors if they moved.
    auto publish(const table_descriptor& d, const kinesis_destination& k) -> std::size_t {
        auto shards = _backend.change_shards(table_range(d.table_id));
        // Parents (older epochs) first, as DynamoDB Streams lists them.
        std::ranges::sort(shards, [](const change_shard& a, const change_shard& b) {
            const auto ea = streams_detail::epoch_of(a.stream.key);
            const auto eb = streams_detail::epoch_of(b.stream.key);
            return ea != eb ? ea < eb : a.stream.key < b.stream.key;
        });
        std::map<std::string, std::uint64_t> stored(k.cursors.begin(), k.cursors.end());
        std::map<std::string, std::uint64_t> next;
        std::set<std::string> listed;
        std::set<std::string> undrained;
        for (const auto& s : shards) {
            listed.insert(s.stream.key);
        }
        std::size_t budget = _options.max_records_per_pass;
        std::size_t landed = 0;
        bool reachable = true;
        for (const auto& s : shards) {
            const auto& key = s.stream.key;
            auto from = stored.contains(key) ? stored[key] : s.stream.first_seq;
            from = std::max(from, s.stream.first_seq);
            next[key] = from;
            const bool parent_pending = std::ranges::any_of(s.stream.parents, [&](const auto& p) {
                return listed.contains(p) && undrained.contains(p);
            });
            if (parent_pending || !reachable || budget == 0) {
                undrained.insert(key);
                continue;
            }
            const auto r = publish_shard(d, k, key, from, budget);
            next[key] = r.next;
            landed += r.landed;
            reachable = r.reachable;
            if (r.open || !r.caught_up) {
                undrained.insert(key);
            }
        }
        std::vector<std::pair<std::string, std::uint64_t>> cursors(next.begin(), next.end());
        if (cursors != k.cursors) {
            progress(d, k, ds::active, {}, std::move(cursors));
        }
        return landed;
    }

    struct shard_result {
        std::uint64_t next = 0;
        std::size_t landed = 0;
        bool reachable = true;
        bool caught_up = false;
        bool open = true;
    };

    auto publish_shard(const table_descriptor& d, const kinesis_destination& k,
                       const std::string& key, std::uint64_t from, std::size_t& budget)
        -> shard_result {
        shard_result out;
        out.next = from;
        std::vector<pending> batch;
        std::set<std::string> items_in_batch;
        std::size_t batch_bytes = 0;
        // Sends the batch; false when the shard must stop for this pass.
        // `out.next` moves past every record before the first that failed,
        // and past `skipped_to` (records of no interest after the batch)
        // only when the whole batch landed.
        const auto flush = [&](std::uint64_t skipped_to) -> bool {
            if (batch.empty()) {
                out.next = std::max(out.next, skipped_to);
                return true;
            }
            std::vector<kinesis_put_record> records;
            records.reserve(batch.size());
            for (const auto& p : batch) {
                records.push_back(p.record);
            }
            const auto got = _sink.put_records(k.stream_arn, records);
            if (!got || got->size() != batch.size()) {
                out.next = batch.front().seq;
                out.reachable = false;
                batch.clear();
                return false;
            }
            for (std::size_t i = 0; i < batch.size(); ++i) {
                if (!(*got)[i]) {
                    out.next = batch[i].seq;
                    batch.clear();
                    return false;
                }
                ++out.landed;
            }
            out.next = std::max(batch.back().seq + 1, skipped_to);
            batch.clear();
            items_in_batch.clear();
            batch_bytes = 0;
            return true;
        };
        auto at = from;
        while (budget > 0) {
            const auto page =
                _backend.read_changes(key, at, std::min(_options.page_records, budget));
            out.open = page.open;
            if (!page.known) {
                // Trimmed away whole: nothing left to send.
                out.caught_up = true;
                out.open = false;
                return out;
            }
            if (page.records.empty()) {
                // The end of the stream as it stands, unless records the
                // state names could not be read (then nothing moves past
                // them).
                const bool end = at > page.last_seq;
                out.caught_up = flush(end ? at : 0) && end;
                return out;
            }
            for (const auto& [seq, rec] : page.records) {
                --budget;
                at = seq + 1;
                if (rec.table_id != d.table_id || rec.written_ms < k.enabled_ms) {
                    if (batch.empty()) {
                        out.next = at;
                    }
                    continue;
                }
                const auto item_key = base_item_key(d.table_id, d.schema, rec.keys).value_or("");
                auto data = boost::json::serialize(kinesis_destination_detail::record_document(
                    _options.region, d.name, key, seq, rec, k.precision));
                if (items_in_batch.contains(item_key) ||
                    batch.size() >= _options.max_batch_records ||
                    batch_bytes + data.size() > _options.max_batch_bytes) {
                    if (!flush(0)) {
                        return out;
                    }
                }
                batch_bytes += data.size();
                items_in_batch.insert(item_key);
                batch.push_back(pending{
                    seq,
                    kinesis_put_record{std::move(data),
                                       kinesis_destination_detail::partition_key_of(item_key)}});
                if (budget == 0) {
                    break;
                }
            }
        }
        (void)flush(at);
        return out;
    }

    Backend& _backend;
    kinesis_sink _sink;
    kinesis_publisher_options _options;
};

/// The four Kinesis streaming destination actions over a backend.
template<dynamo_backend Backend> class kinesis_destination_api {
public:
    using result = aws_json::result<boost::json::object>;

    explicit kinesis_destination_api(Backend& backend) : _backend(backend) {}

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("EnableKinesisStreamingDestination",
              [this](call_context& c, boost::json::object& in) { return enable(c, in); });
        fe.on("DisableKinesisStreamingDestination",
              [this](call_context& c, boost::json::object& in) { return disable(c, in); });
        fe.on("DescribeKinesisStreamingDestination",
              [this](call_context& c, boost::json::object& in) { return describe(c, in); });
        fe.on("UpdateKinesisStreamingDestination",
              [this](call_context& c, boost::json::object& in) { return update(c, in); });
    }

    auto enable(call_context& ctx, boost::json::object& in) -> result {
        return change(ctx, in, destination_status::enabling, "EnableKinesisStreamingConfiguration");
    }

    auto disable(call_context& ctx, boost::json::object& in) -> result {
        return change(ctx, in, destination_status::disabling,
                      "EnableKinesisStreamingConfiguration");
    }

    auto update(call_context& ctx, boost::json::object& in) -> result {
        return change(ctx, in, destination_status::updating, "UpdateKinesisStreamingConfiguration");
    }

    auto describe(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        if (auto e = check_name(name)) {
            return *e;
        }
        const auto d = _backend.describe(name);
        if (!d) {
            return not_found(name);
        }
        boost::json::array list;
        for (const auto& k : d->destinations) {
            boost::json::object o{
                {"StreamArn", k.stream_arn},
                {"DestinationStatus", kinesis_destination_detail::status_name(k.status)},
                {"ApproximateCreationDateTimePrecision",
                 kinesis_destination_detail::precision_name(k.precision)},
            };
            if (!k.status_description.empty()) {
                o["DestinationStatusDescription"] = k.status_description;
            }
            list.push_back(std::move(o));
        }
        return boost::json::object{{"TableName", d->name},
                                   {"KinesisDataStreamDestinations", std::move(list)}};
    }

private:
    auto change(call_context& ctx, boost::json::object& in, destination_status want,
                std::string_view config_member) -> result {
        namespace kd = kinesis_destination_detail;
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        if (auto e = check_name(name)) {
            return *e;
        }
        const auto arn = std::string(in.at("StreamArn").as_string());
        const auto region = kd::stream_arn_region(arn);
        if (!region) {
            return validation_error("StreamArn " + arn + " is not a Kinesis data stream ARN");
        }
        if (*region != ctx.region) {
            // AWS streams only to a stream in the table's account and region.
            return validation_error("The Kinesis data stream must be in the table's region");
        }
        auto precision = time_precision::millisecond;
        if (const auto* cfg = in.if_contains(config_member); cfg != nullptr && cfg->is_object()) {
            if (const auto* p =
                    cfg->as_object().if_contains("ApproximateCreationDateTimePrecision");
                p != nullptr && p->is_string()) {
                const auto parsed = kd::precision_of(p->as_string());
                if (!parsed) {
                    return validation_error("Invalid ApproximateCreationDateTimePrecision");
                }
                precision = *parsed;
            } else if (want == destination_status::updating) {
                return validation_error("ApproximateCreationDateTimePrecision is required");
            }
        } else if (want == destination_status::updating) {
            return validation_error("UpdateKinesisStreamingConfiguration is required");
        }
        catalog_command c;
        c.op = catalog_command::kind::set_destination;
        c.table.name = name;
        kinesis_destination k;
        k.stream_arn = arn;
        k.status = want;
        k.precision = precision;
        c.table.destinations.push_back(std::move(k));
        c.now_ms = _backend.now_ms();
        const auto r = _backend.catalog(c);
        if (!r.ok()) {
            return table_api_detail::error_of(r);
        }
        boost::json::object out{{"TableName", r.table ? r.table->name : name},
                                {"StreamArn", arn},
                                {"DestinationStatus", kd::status_name(want)}};
        if (want != destination_status::disabling) {
            out[config_member] = boost::json::object{
                {"ApproximateCreationDateTimePrecision", kd::precision_name(precision)}};
        }
        return out;
    }

    static auto check_name(std::string_view name) -> std::optional<aws_json::error> {
        if (!table_api_detail::valid_table_name(name)) {
            return validation_error(
                "Invalid table/index name.  Table/index names must be between 3 and 255 "
                "characters long, and may contain only the characters a-z, A-Z, 0-9, '_', '-', "
                "and '.'");
        }
        return std::nullopt;
    }

    static auto not_found(std::string_view name) -> aws_json::error {
        return table_api_detail::modeled(
            "ResourceNotFoundException",
            "Requested resource not found: Table: " + std::string(name) + " not found");
    }

    Backend& _backend;
};

// -- The HTTP sink ------------------------------------------------------------

struct kinesis_credentials {
    std::string access_key_id;
    std::string secret_access_key;
    std::string session_token;
};

struct kinesis_http_sink_config {
    /// "host:port" of the Kinesis endpoint; empty is AWS's for the stream's
    /// region (`kinesis.<region>.amazonaws.com:443`).
    std::string endpoint;
    bool tls = true;
    /// PEM bundle the endpoint's certificate chains to; empty uses the
    /// system's.
    std::string ca_path;
    std::chrono::milliseconds timeout{10000};
    std::function<kinesis_credentials()> credentials;
    /// The signing clock; the system clock when empty (tests pin it).
    std::function<std::chrono::sys_seconds()> clock;
};

/// One HTTP answer: status and body.
struct kinesis_http_answer {
    int status = 0;
    std::string body;
};

/// One signed POST: (host:port, headers, body) to the answer, or nothing
/// when there was none.
using kinesis_http_transport = std::function<std::optional<kinesis_http_answer>(
    const std::string& host_port, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body)>;

namespace kinesis_destination_detail {

[[nodiscard]] inline auto split_host_port(std::string_view hp)
    -> std::optional<std::pair<std::string, std::string>> {
    const auto colon = hp.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == hp.size()) {
        return std::nullopt;
    }
    auto host = std::string(hp.substr(0, colon));
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    return std::pair{std::move(host), std::string(hp.substr(colon + 1))};
}

/// A one-connection-per-call HTTP(S) POST to `/`.
[[nodiscard]] inline auto http_post(const kinesis_http_sink_config& cfg,
                                    const std::string& host_port,
                                    const std::vector<std::pair<std::string, std::string>>& headers,
                                    const std::string& body) -> std::optional<kinesis_http_answer> {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = boost::asio::ssl;
    const auto hp = split_host_port(host_port);
    if (!hp) {
        return std::nullopt;
    }
    try {
        http::request<http::string_body> req{http::verb::post, "/", 11};
        for (const auto& [k, v] : headers) {
            req.set(k, v);
        }
        req.set(http::field::user_agent, "kythira-dynamodb-kinesis-destination");
        req.body() = body;
        req.prepare_payload();
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::resolver resolver(ioc);
        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(std::uint64_t{8} << 20U);
        if (!cfg.tls) {
            beast::tcp_stream stream(ioc);
            stream.expires_after(cfg.timeout);
            stream.connect(resolver.resolve(hp->first, hp->second));
            http::write(stream, req);
            http::read(stream, buffer, parser);
            beast::error_code ec;
            (void)stream.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        } else {
            ssl::context ctx(ssl::context::tls_client);
            if (cfg.ca_path.empty()) {
                ctx.set_default_verify_paths();
            } else {
                ctx.load_verify_file(cfg.ca_path);
            }
            ctx.set_verify_mode(ssl::verify_peer);
            beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
            if (SSL_set_tlsext_host_name(stream.native_handle(), hp->first.c_str()) != 1) {
                return std::nullopt;
            }
            stream.set_verify_callback(ssl::host_name_verification(hp->first));
            beast::get_lowest_layer(stream).expires_after(cfg.timeout);
            beast::get_lowest_layer(stream).connect(resolver.resolve(hp->first, hp->second));
            stream.handshake(ssl::stream_base::client);
            http::write(stream, req);
            http::read(stream, buffer, parser);
            // The answer is read; a missing close_notify costs nothing here.
            beast::error_code ec;
            (void)stream.shutdown(ec);
        }
        return kinesis_http_answer{static_cast<int>(parser.get().result_int()),
                                   parser.get().body()};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace kinesis_destination_detail

/// A `kinesis_sink` over the Kinesis JSON 1.1 protocol, signed with SigV4
/// for the stream's region: `DescribeStreamSummary` to check a stream and
/// `PutRecords` to publish.
class kinesis_http_sink {
public:
    explicit kinesis_http_sink(kinesis_http_sink_config config,
                               kinesis_http_transport transport = {})
        : _config(std::move(config)), _transport(std::move(transport)) {
        if (!_transport) {
            _transport = [cfg = _config](const std::string& host_port,
                                         const std::vector<std::pair<std::string, std::string>>& h,
                                         const std::string& body) {
                return kinesis_destination_detail::http_post(cfg, host_port, h, body);
            };
        }
    }

    /// As a `kinesis_sink`; the sink shares this one's state.
    [[nodiscard]] auto as_sink() const -> kinesis_sink {
        auto self = std::make_shared<kinesis_http_sink>(*this);
        return kinesis_sink{
            .check_stream = [self](const std::string& arn) { return self->check_stream(arn); },
            .put_records =
                [self](const std::string& arn, const std::vector<kinesis_put_record>& records) {
                    return self->put_records(arn, records);
                }};
    }

    [[nodiscard]] auto check_stream(const std::string& arn) const -> kinesis_stream_check {
        const auto res =
            call(arn, "DescribeStreamSummary", boost::json::object{{"StreamARN", arn}});
        if (!res) {
            return {kinesis_stream_check::outcome::unreachable, {}};
        }
        if (res->status == 200) {
            return {kinesis_stream_check::outcome::ok, {}};
        }
        const auto [type, message] = error_of(res->body);
        if (type == "ResourceNotFoundException" || type == "AccessDeniedException" ||
            type == "InvalidArgumentException" || type == "UnrecognizedClientException" ||
            type == "MissingAuthenticationTokenException") {
            return {kinesis_stream_check::outcome::refused,
                    message.empty() ? "The Kinesis data stream cannot be used: " + type : message};
        }
        return {kinesis_stream_check::outcome::unreachable, {}};
    }

    [[nodiscard]] auto put_records(const std::string& arn,
                                   const std::vector<kinesis_put_record>& records) const
        -> std::optional<std::vector<bool>> {
        boost::json::array list;
        for (const auto& r : records) {
            list.push_back(boost::json::object{{"Data", detail::base64_of(r.data)},
                                               {"PartitionKey", r.partition_key}});
        }
        const auto res =
            call(arn, "PutRecords", boost::json::object{{"StreamARN", arn}, {"Records", list}});
        if (!res || res->status != 200) {
            return std::nullopt;
        }
        try {
            const auto v = boost::json::parse(res->body);
            const auto& got = v.as_object().at("Records").as_array();
            if (got.size() != records.size()) {
                return std::nullopt;
            }
            std::vector<bool> out;
            out.reserve(got.size());
            for (const auto& r : got) {
                out.push_back(!r.as_object().contains("ErrorCode"));
            }
            return out;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// The headers of a signed call: exposed for tests.
    [[nodiscard]] auto signed_headers(const std::string& region, const std::string& endpoint,
                                      std::string_view target, std::string_view body) const
        -> std::vector<std::pair<std::string, std::string>> {
        namespace sd = sigv4::detail;
        const auto creds = _config.credentials ? _config.credentials() : kinesis_credentials{};
        const auto now =
            _config.clock
                ? _config.clock()
                : std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto amz_date = sd::format_amz_date(now);
        const sigv4::credential_scope scope{
            .date = amz_date.substr(0, 8), .region = region, .service = "kinesis"};
        // Signed in sorted order.
        std::vector<std::pair<std::string, std::string>> h{
            {"content-type", "application/x-amz-json-1.1"},
            {"host", host_header(endpoint)},
            {"x-amz-date", amz_date},
        };
        if (!creds.session_token.empty()) {
            h.emplace_back("x-amz-security-token", creds.session_token);
        }
        h.emplace_back("x-amz-target", "Kinesis_20131202." + std::string(target));
        std::string canonical = "POST\n/\n\n";
        std::string names;
        for (const auto& [k, v] : h) {
            canonical += k + ":" + v + "\n";
            names += (names.empty() ? "" : ";") + k;
        }
        canonical += "\n" + names + "\n" + sd::sha256_hex(body);
        const auto to_sign = std::string(sigv4::k_algorithm) + "\n" + amz_date + "\n" +
                             scope.text() + "\n" + sd::sha256_hex(canonical);
        const auto key = sigv4::derive_signing_key(creds.secret_access_key, scope);
        const auto signature = sd::to_hex(sd::hmac_sha256(key, to_sign));
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(h.size() + 1);
        for (auto& [k, v] : h) {
            out.emplace_back(k == "host" ? "Host" : k, std::move(v));
        }
        out.emplace_back("Authorization", std::string(sigv4::k_algorithm) +
                                              " Credential=" + creds.access_key_id + "/" +
                                              scope.text() + ", SignedHeaders=" + names +
                                              ", Signature=" + signature);
        return out;
    }

private:
    kinesis_http_sink_config _config;
    kinesis_http_transport _transport;

    // The Host header without the default port, as SDKs send it.
    [[nodiscard]] auto host_header(const std::string& e) const -> std::string {
        if ((_config.tls && e.ends_with(":443")) || (!_config.tls && e.ends_with(":80"))) {
            return e.substr(0, e.rfind(':'));
        }
        return e;
    }

    [[nodiscard]] auto call(const std::string& arn, std::string_view target,
                            const boost::json::object& req) const
        -> std::optional<kinesis_http_answer> {
        const auto region = kinesis_destination_detail::stream_arn_region(arn);
        if (!region) {
            return std::nullopt;
        }
        const auto endpoint = _config.endpoint.empty() ? "kinesis." + *region + ".amazonaws.com:443"
                                                       : _config.endpoint;
        const auto body = boost::json::serialize(req);
        return _transport(endpoint, signed_headers(*region, endpoint, target, body), body);
    }

    static auto error_of(const std::string& body) -> std::pair<std::string, std::string> {
        std::string type;
        std::string message;
        try {
            const auto v = boost::json::parse(body);
            if (const auto* o = v.if_object()) {
                if (const auto* t = o->if_contains("__type"); t != nullptr && t->is_string()) {
                    type = std::string(t->get_string());
                }
                for (const auto* k : {"message", "Message"}) {
                    if (const auto* m = o->if_contains(k); m != nullptr && m->is_string()) {
                        message = std::string(m->get_string());
                    }
                }
            }
        } catch (const std::exception&) {
            // Judged by the status alone.
        }
        if (const auto hash = type.rfind('#'); hash != std::string::npos) {
            type = type.substr(hash + 1);
        }
        return {type, message};
    }
};

/// The sink the gateway uses unless told otherwise, from the environment:
/// `KYTHIRA_DYNAMODB_KINESIS_ENDPOINT` ("host:port"; unset is AWS's),
/// `KYTHIRA_DYNAMODB_KINESIS_TLS` (`0` for plain HTTP; default on),
/// `KYTHIRA_DYNAMODB_KINESIS_CA` (a PEM bundle), and the standard
/// `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`,
/// read at each call so rotated keys take effect.
[[nodiscard]] inline auto kinesis_sink_from_env() -> kinesis_sink {
    const auto env = [](const char* name) -> std::string {
        const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
        return v == nullptr ? std::string{} : std::string(v);
    };
    kinesis_http_sink_config cfg;
    cfg.endpoint = env("KYTHIRA_DYNAMODB_KINESIS_ENDPOINT");
    cfg.tls = env("KYTHIRA_DYNAMODB_KINESIS_TLS") != "0";
    cfg.ca_path = env("KYTHIRA_DYNAMODB_KINESIS_CA");
    cfg.credentials = [env] {
        return kinesis_credentials{env("AWS_ACCESS_KEY_ID"), env("AWS_SECRET_ACCESS_KEY"),
                                   env("AWS_SESSION_TOKEN")};
    };
    return kinesis_http_sink(std::move(cfg)).as_sink();
}

}  // namespace kythira::dynamodb
