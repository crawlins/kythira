// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file bulk_put.hpp
/// @brief `StartBulkPutItems`, `DescribeBulkOperation`, `ListBulkOperations`
///        and `CancelBulkOperation` (`.kiro/specs/dynamodb-compatible-api/`
///        task 20, Requirement 22.1).
///
/// A bulk put is an import into an existing table through ordinary writes:
/// every object under the input prefix is decompressed and read as
/// DynamoDB JSON or Ion records (`export_import.hpp`'s reader), and each
/// record's `Item` is put through the item write path, so conditions,
/// LSIs, GSIs, streams and the change log see every item. A record may
/// carry `ConditionExpression`, `ExpressionAttributeNames` and
/// `ExpressionAttributeValues`, compiled as `PutItem` compiles them.
///
/// Each operation is a JSON record in the backup store
/// (`bulk/<id>/bulk.json`); its ARN is the table's with
/// `/bulk-operation/<id>`. `bulk_put_worker`, on the catalog leader beside
/// the export worker, runs every IN_PROGRESS operation. Progress is saved
/// after each input object with the objects already done, so a new leader
/// resumes at the first object not done; only that object's records are
/// put twice, which matters only to a record with a condition (it may
/// report a condition failure the first attempt did not).
///
/// - `MaxWriteRequestUnits` caps the rate: each put costs a write request
///   unit per started KB, drawn from a bucket refilled at that rate with
///   one second's worth of burst. Without it the operation writes as fast
///   as the groups accept, as the table's own capacity settings do not
///   apply to bulk operations on AWS either.
/// - `OutputS3Configuration` names what is reported: `ERROR` (records
///   that could not be read or were refused: no key, a key of the wrong
///   type, over 400 KB, a bad expression, or an object that could not be
///   decompressed) and `CONDITION_CHECK_FAILURE` (the attempted item and
///   the stored one). Reports are JSON lines under
///   `<prefix>/AWSDynamoDB/BulkOperations/<id>/errors/` and
///   `.../condition-check-failures/` in the output bucket, one object per
///   input object that had any; the layout is Kythira's, as AWS does not
///   document its own.
/// - The status ends COMPLETED, or COMPLETED_WITH_ERRORS when any record or
///   object was in error; condition failures do not count as errors.
/// - `CancelBulkOperation` writes a cancel marker beside the record and
///   answers CANCELLING; the worker stops at the next record it checks
///   (every 256) and ends CANCELLED. Items already written stay.
/// - Operations are listed newest first for 90 days, then dropped.
///
/// `PerformanceTier` is recorded and reported; both tiers write the same
/// way. Refusal wording is Kythira's (DynamoDB Local has no bulk
/// operations, and no AWS run was made).

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/export_formats.hpp>
#include <kythira/dynamodb/export_import.hpp>
#include <kythira/dynamodb/expr_request.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/local_index.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

/// A bulk operation's record (`bulk.json`).
struct bulk_record {
    std::string id;
    std::string arn;
    std::string table_name;
    std::string table_arn;
    std::uint64_t table_id = 0;
    /// IN_PROGRESS, COMPLETED, COMPLETED_WITH_ERRORS, CANCELLED or FAILED
    /// (CANCELLING is the cancel marker's, shown while IN_PROGRESS).
    std::string status = "IN_PROGRESS";
    std::uint64_t start_ms = 0;
    std::uint64_t end_ms = 0;
    std::string client_token;
    /// `InputS3Configuration` and `OutputS3Configuration`, as given.
    boost::json::object input;
    boost::json::object output;
    std::uint64_t max_units = 0;
    std::string tier = "STANDARD";
    std::uint64_t processed_items = 0;
    std::uint64_t processed_bytes = 0;
    /// Known once every input object was read.
    std::uint64_t total_bytes = 0;
    std::uint64_t failed_items = 0;
    /// Objects that could not be decompressed.
    std::uint64_t failed_objects = 0;
    std::uint64_t condition_failures = 0;
    std::uint64_t put_bytes = 0;
    /// Input objects fully processed, and report objects written.
    std::vector<std::string> done;
    std::uint64_t reports = 0;
    std::string failure_code;
    std::string failure_message;

    friend auto operator==(const bulk_record&, const bulk_record&) -> bool = default;
};

namespace bulk_detail {

inline constexpr std::string_view k_prefix = "bulk/";
/// Records looked at between checks of the cancel marker.
inline constexpr std::uint64_t k_cancel_check = 256;

[[nodiscard]] inline auto record_key(std::string_view id) -> std::string {
    return std::string(k_prefix) + std::string(id) + "/bulk.json";
}
[[nodiscard]] inline auto cancel_key(std::string_view id) -> std::string {
    return std::string(k_prefix) + std::string(id) + "/cancel";
}

[[nodiscard]] inline auto encode(const bulk_record& r) -> std::string {
    boost::json::array done;
    for (const auto& k : r.done) {
        done.emplace_back(k);
    }
    return boost::json::serialize(boost::json::object{
        {"formatVersion", 1},
        {"id", r.id},
        {"arn", r.arn},
        {"tableName", r.table_name},
        {"tableArn", r.table_arn},
        {"tableId", r.table_id},
        {"status", r.status},
        {"startMs", r.start_ms},
        {"endMs", r.end_ms},
        {"clientToken", r.client_token},
        {"input", r.input},
        {"output", r.output},
        {"maxUnits", r.max_units},
        {"tier", r.tier},
        {"processedItems", r.processed_items},
        {"processedBytes", r.processed_bytes},
        {"totalBytes", r.total_bytes},
        {"failedItems", r.failed_items},
        {"failedObjects", r.failed_objects},
        {"conditionFailures", r.condition_failures},
        {"putBytes", r.put_bytes},
        {"done", std::move(done)},
        {"reports", r.reports},
        {"failureCode", r.failure_code},
        {"failureMessage", r.failure_message},
    });
}

[[nodiscard]] inline auto decode(std::string_view body) -> std::optional<bulk_record> {
    namespace ed = export_detail;
    try {
        const auto v = boost::json::parse(body);
        const auto& o = v.as_object();
        if (ed::num(o, "formatVersion") != 1) {
            return std::nullopt;
        }
        bulk_record r;
        r.id = ed::text(o, "id");
        r.arn = ed::text(o, "arn");
        r.table_name = ed::text(o, "tableName");
        r.table_arn = ed::text(o, "tableArn");
        r.table_id = ed::num(o, "tableId");
        r.status = ed::text(o, "status");
        r.start_ms = ed::num(o, "startMs");
        r.end_ms = ed::num(o, "endMs");
        r.client_token = ed::text(o, "clientToken");
        r.input = o.at("input").as_object();
        r.output = o.at("output").as_object();
        r.max_units = ed::num(o, "maxUnits");
        r.tier = ed::text(o, "tier");
        r.processed_items = ed::num(o, "processedItems");
        r.processed_bytes = ed::num(o, "processedBytes");
        r.total_bytes = ed::num(o, "totalBytes");
        r.failed_items = ed::num(o, "failedItems");
        r.failed_objects = ed::num(o, "failedObjects");
        r.condition_failures = ed::num(o, "conditionFailures");
        r.put_bytes = ed::num(o, "putBytes");
        for (const auto& k : o.at("done").as_array()) {
            r.done.emplace_back(k.as_string());
        }
        r.reports = ed::num(o, "reports");
        r.failure_code = ed::text(o, "failureCode");
        r.failure_message = ed::text(o, "failureMessage");
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Every bulk operation's record, oldest first.
[[nodiscard]] inline auto all(backup_store& store) -> std::vector<bulk_record> {
    return export_detail::all_records<bulk_record>(store, k_prefix, "/bulk.json",
                                                   [](std::string_view b) { return decode(b); });
}

/// The directory an operation's reports go to in the output bucket.
[[nodiscard]] inline auto report_root(const boost::json::object& output, std::string_view id)
    -> std::string {
    std::string out = export_detail::text(output, "S3KeyPrefix");
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    return out + "AWSDynamoDB/BulkOperations/" + std::string(id) + "/";
}

[[nodiscard]] inline auto wants(const boost::json::object& output, std::string_view type) -> bool {
    const auto* t = output.if_contains("OutputType");
    return t != nullptr && t->is_array() &&
           std::ranges::any_of(t->as_array(), [&](const boost::json::value& v) {
               return v.is_string() && v.as_string() == type;
           });
}

[[nodiscard]] inline auto not_found(std::string_view arn) -> aws_json::error {
    return table_api_detail::modeled("ResourceNotFoundException",
                                     "Bulk operation not found: " + std::string(arn));
}

/// Write request units for a put of `bytes`: one per started KB.
[[nodiscard]] inline auto units_of(std::uint64_t bytes) -> std::uint64_t {
    return std::max<std::uint64_t>(1, (bytes + 1023) / 1024);
}

}  // namespace bulk_detail

// -- The API ------------------------------------------------------------------

/// The four bulk operation actions.
template<dynamo_backend Backend> class bulk_put_api {
public:
    using result = aws_json::result<boost::json::object>;

    bulk_put_api(Backend& backend, std::shared_ptr<backup_store> store, bucket_resolver buckets)
        : _backend(backend), _store(std::move(store)), _buckets(std::move(buckets)) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    void set_buckets(bucket_resolver buckets) { _buckets = std::move(buckets); }
    /// Called when an operation is started or cancelled.
    void on_started(std::function<void()> fn) { _started = std::move(fn); }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("StartBulkPutItems",
              [this](call_context& c, boost::json::object& in) { return start(c, in); });
        fe.on("DescribeBulkOperation",
              [this](call_context& c, boost::json::object& in) { return describe(c, in); });
        fe.on("ListBulkOperations",
              [this](call_context& c, boost::json::object& in) { return list(c, in); });
        fe.on("CancelBulkOperation",
              [this](call_context& c, boost::json::object& in) { return cancel(c, in); });
    }

    auto start(call_context& ctx, boost::json::object& in) -> result {
        namespace ed = export_detail;
        namespace bd = bulk_detail;
        const auto name = table_api_detail::table_name_of(in.at("TableName").as_string());
        const auto d = _backend.describe(name);
        if (!d) {
            return table_api_detail::modeled(
                "ResourceNotFoundException",
                "Requested resource not found: Table: " + name + " not found");
        }
        if (d->status != table_status::active) {
            return table_api_detail::modeled("ResourceInUseException",
                                             "Table is not ACTIVE: " + name);
        }
        if (!_store) {
            return validation_error(
                "Bulk operations are not configured on this cluster: set "
                "KYTHIRA_DYNAMODB_BACKUP_DIR or KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET");
        }
        bulk_record r;
        r.input = in.at("InputS3Configuration").as_object();
        r.output = in.at("OutputS3Configuration").as_object();
        for (const auto* config : {&r.input, &r.output}) {
            const auto bucket = ed::text(*config, "S3Bucket");
            if (config == &r.output && bucket.empty()) {
                continue;
            }
            if (auto s = ed::bucket_store(_buckets, bucket); !s) {
                return s.error();
            }
        }
        const auto format = ed::text(r.input, "InputFormat");
        if (format != "DYNAMODB_JSON" && format != "ION") {
            return validation_error("InputFormat must be DYNAMODB_JSON or ION");
        }
        auto compression = ed::text(r.input, "InputCompressionType");
        if (compression.empty()) {
            compression = "NONE";
            r.input["InputCompressionType"] = compression;
        }
        if ((compression == "GZIP" && !export_formats::has_gzip()) ||
            (compression == "ZSTD" && !export_formats::has_zstd())) {
            return validation_error(compression + " input is not supported: this build has no " +
                                    (compression == "GZIP" ? "zlib" : "libzstd"));
        }
        if ((bd::wants(r.output, "ERROR") || bd::wants(r.output, "CONDITION_CHECK_FAILURE")) &&
            ed::text(r.output, "S3Bucket").empty()) {
            return validation_error("OutputS3Configuration needs an S3Bucket for its OutputType");
        }
        r.max_units = ed::num(in, "MaxWriteRequestUnits");
        if (in.contains("MaxWriteRequestUnits") && r.max_units < 1024) {
            return validation_error("MaxWriteRequestUnits must be at least 1024");
        }
        if (const auto tier = ed::text(in, "PerformanceTier"); !tier.empty()) {
            r.tier = tier;
        }
        r.client_token = ed::text(in, "ClientToken");
        r.table_name = d->name;
        r.table_arn = ctx.table_arn(d->name);
        r.table_id = d->table_id;
        const auto now = _backend.now_ms();
        if (!r.client_token.empty()) {
            for (const auto& old : bd::all(*_store)) {
                if (old.client_token != r.client_token || now - old.start_ms > ed::k_token_ms) {
                    continue;
                }
                if (old.table_arn == r.table_arn && old.input == r.input &&
                    old.output == r.output && old.max_units == r.max_units && old.tier == r.tier) {
                    return boost::json::object{{"BulkOperationDescription", description(old)}};
                }
                return table_api_detail::modeled(
                    "IdempotentParameterMismatchException",
                    "A bulk operation with the same client token but different parameters "
                    "exists: " +
                        old.arn);
            }
        }
        r.start_ms = now;
        r.id = backup_detail::new_backup_id(now);
        r.arn = r.table_arn + "/bulk-operation/" + r.id;
        // The front end authorized BulkPutItems on the table; starting the
        // operation is its own action, on the operation's ARN.
        if (ctx.authorize) {
            aws_auth::authz_request start;
            start.action = "dynamodb:StartBulkOperation";
            start.resource = r.arn;
            if (auto denied = ctx.authorize(std::move(start))) {
                return *denied;
            }
        }
        _store->put(bd::record_key(r.id), bd::encode(r));
        if (_started) {
            _started();
        }
        return boost::json::object{{"BulkOperationDescription", description(r)}};
    }

    auto describe(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto arn = export_detail::text(in, "BulkOperationArn");
        const auto r = find(arn);
        if (!r) {
            return bulk_detail::not_found(arn);
        }
        return boost::json::object{{"BulkOperationDescription", description(*r)}};
    }

    auto list(call_context& /*ctx*/, boost::json::object& in) -> result {
        namespace ed = export_detail;
        boost::json::array out;
        if (!_store) {
            return boost::json::object{{"BulkOperationsSummaryList", std::move(out)}};
        }
        auto limit = ed::num(in, "MaxResults");
        if (limit == 0) {
            limit = 25;
        }
        const auto token = ed::text(in, "NextToken");
        const auto start = ed::import_of_page_token(token);
        bool started = token.empty();
        std::string last;
        bool more = false;
        auto records = bulk_detail::all(*_store);
        std::ranges::reverse(records);
        for (const auto& r : records) {
            if (!started) {
                started = r.arn == start;
                continue;
            }
            if (out.size() >= limit) {
                more = true;
                break;
            }
            boost::json::object s{
                {"BulkOperationArn", r.arn},
                {"BulkOperationStatus", shown_status(r)},
                {"StartTime", table_api_detail::millis_to_seconds(r.start_ms)},
            };
            if (r.end_ms != 0) {
                s["EndTime"] = table_api_detail::millis_to_seconds(r.end_ms);
            }
            out.push_back(std::move(s));
            last = r.arn;
        }
        boost::json::object res{{"BulkOperationsSummaryList", std::move(out)}};
        if (more) {
            res["NextToken"] = ed::import_page_token(last);
        }
        return res;
    }

    auto cancel(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto arn = export_detail::text(in, "BulkOperationArn");
        const auto r = find(arn);
        if (!r) {
            return bulk_detail::not_found(arn);
        }
        if (r->status != "IN_PROGRESS") {
            return validation_error("Bulk operation " + arn + " is " + r->status +
                                    ", not IN_PROGRESS");
        }
        _store->put(bulk_detail::cancel_key(r->id), "");
        if (_started) {
            _started();
        }
        return boost::json::object{{"BulkOperationArn", r->arn},
                                   {"BulkOperationStatus", "CANCELLING"}};
    }

private:
    [[nodiscard]] auto find(const std::string& arn) -> std::optional<bulk_record> {
        const auto id = export_detail::id_of(arn, "bulk-operation");
        if (!id || !_store) {
            return std::nullopt;
        }
        const auto body = _store->get(bulk_detail::record_key(*id));
        auto r = body ? bulk_detail::decode(*body) : std::nullopt;
        if (!r || r->arn != arn) {
            return std::nullopt;
        }
        return r;
    }

    [[nodiscard]] auto shown_status(const bulk_record& r) -> std::string {
        if (r.status == "IN_PROGRESS" && _store->get(bulk_detail::cancel_key(r.id))) {
            return "CANCELLING";
        }
        return r.status;
    }

    [[nodiscard]] auto description(const bulk_record& r) -> boost::json::object {
        using table_api_detail::millis_to_seconds;
        boost::json::object o{
            {"BulkOperationArn", r.arn},
            {"BulkOperationStatus", shown_status(r)},
            {"TableDetails", boost::json::array{boost::json::object{
                                 {"TableName", r.table_arn},
                                 {"TableId", table_api_detail::table_uuid(r.table_id)}}}},
            {"InputS3Configuration", r.input},
            {"OutputS3Configuration", r.output},
            {"StartTime", millis_to_seconds(r.start_ms)},
            {"ProcessedItemCount", r.processed_items},
            {"ProcessedInputBytes", r.processed_bytes},
            {"FailedItemCount", r.failed_items},
            {"PerformanceTier", r.tier},
            {"BulkOperationUsage",
             boost::json::array{boost::json::object{
                 {"TableName", r.table_arn},
                 {"BulkPutBytes", r.put_bytes},
                 {"Table", boost::json::object{{"BulkPutBytes", r.put_bytes}}}}}},
        };
        if (r.end_ms != 0) {
            o["EndTime"] = millis_to_seconds(r.end_ms);
            o["TotalInputBytes"] = r.total_bytes;
        }
        if (r.max_units != 0) {
            o["MaxWriteRequestUnits"] = r.max_units;
        }
        if (!r.client_token.empty()) {
            o["ClientToken"] = r.client_token;
        }
        if (!r.failure_code.empty()) {
            o["FailureCode"] = r.failure_code;
            o["FailureMessage"] = r.failure_message;
        }
        return o;
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    bucket_resolver _buckets;
    std::function<void()> _started;
};

// -- The worker ---------------------------------------------------------------

struct bulk_put_worker_options {
    /// How often the store is listed when nothing was started here.
    std::chrono::milliseconds list_interval{10000};
    /// The clock and sleep the rate limit uses (tests replace both).
    std::function<std::chrono::steady_clock::time_point()> now = [] {
        return std::chrono::steady_clock::now();
    };
    std::function<void(std::chrono::microseconds)> sleep = [](std::chrono::microseconds d) {
        std::this_thread::sleep_for(d);
    };
};

/// Runs bulk operations, on the catalog leader.
template<dynamo_backend Backend> class bulk_put_worker {
public:
    bulk_put_worker(Backend& backend, std::shared_ptr<backup_store> store, bucket_resolver buckets,
                    bulk_put_worker_options options = {})
        : _backend(backend),
          _store(std::move(store)),
          _buckets(std::move(buckets)),
          _options(std::move(options)) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    void set_buckets(bucket_resolver buckets) { _buckets = std::move(buckets); }
    void poke() { _poked.store(true); }

    /// One pass: every IN_PROGRESS operation run to its end. Returns how
    /// many ended.
    auto pass() -> std::size_t {
        namespace ed = export_detail;
        namespace bd = bulk_detail;
        if (!_store) {
            return 0;
        }
        const auto now_tp = std::chrono::steady_clock::now();
        if (!_poked.exchange(false) && now_tp < _next_list) {
            return 0;
        }
        _next_list = now_tp + _options.list_interval;
        std::size_t done = 0;
        const auto now = _backend.now_ms();
        for (auto& r : bd::all(*_store)) {
            if (r.status == "IN_PROGRESS") {
                run(r);
                ++done;
            } else if (r.end_ms != 0 && now - r.end_ms > ed::k_retention_ms) {
                for (const auto& key : _store->list(std::string(bd::k_prefix) + r.id + "/")) {
                    _store->remove(key);
                }
            }
        }
        return done;
    }

private:
    void save(const bulk_record& r) {
        _store->put(bulk_detail::record_key(r.id), bulk_detail::encode(r));
    }

    void end(bulk_record& r, std::string status, std::string code = {}, std::string message = {}) {
        r.status = std::move(status);
        r.failure_code = std::move(code);
        r.failure_message = std::move(message);
        r.end_ms = std::max(_backend.now_ms(), r.start_ms);
        r.total_bytes = r.processed_bytes;
        save(r);
        _store->remove(bulk_detail::cancel_key(r.id));
    }

    [[nodiscard]] auto cancelled(const bulk_record& r) -> bool {
        return _store->get(bulk_detail::cancel_key(r.id)).has_value();
    }

    /// Waits until `units` write request units are available.
    void take(std::uint64_t rate, std::uint64_t units) {
        if (rate == 0) {
            return;
        }
        const auto now = _options.now();
        const auto cap = static_cast<double>(rate);
        if (!_bucket_at) {
            _tokens = cap;
        } else {
            const auto elapsed = std::chrono::duration<double>(now - *_bucket_at).count();
            _tokens = std::min(cap, _tokens + elapsed * cap);
        }
        _bucket_at = now;
        _tokens -= static_cast<double>(units);
        if (_tokens < 0) {
            const auto wait = std::chrono::duration<double>(-_tokens / cap);
            _options.sleep(std::chrono::duration_cast<std::chrono::microseconds>(wait));
        }
    }

    void run(bulk_record& r) {
        namespace ed = export_detail;
        namespace bd = bulk_detail;
        if (cancelled(r)) {
            end(r, "CANCELLED", "UserCancelled", "The bulk operation was cancelled");
            return;
        }
        const auto d = _backend.describe(r.table_name);
        if (!d || d->table_id != r.table_id || d->status == table_status::deleting) {
            end(r, "FAILED", "TableNotFound",
                "Table " + r.table_name + " was deleted during the bulk operation");
            return;
        }
        const auto resolve = [&](const boost::json::object& config) {
            const auto name = ed::text(config, "S3Bucket");
            std::shared_ptr<backup_store> s;
            if (_buckets && ed::valid_bucket(name)) {
                s = _buckets(name);
            }
            return s;
        };
        const auto in = resolve(r.input);
        const auto out = ed::text(r.output, "S3Bucket").empty() ? nullptr : resolve(r.output);
        if (!in || (!ed::text(r.output, "S3Bucket").empty() && !out)) {
            end(r, "FAILED", "S3NoSuchBucket", "An S3 bucket is not mapped to a store");
            return;
        }
        const bool report_errors = out && bd::wants(r.output, "ERROR");
        const bool report_conditions = out && bd::wants(r.output, "CONDITION_CHECK_FAILURE");
        const auto root = bd::report_root(r.output, r.id);
        const auto format = ed::text(r.input, "InputFormat");
        const auto compression = ed::text(r.input, "InputCompressionType");
        const auto gsis = maintained_indexes(d->gsis);
        const request_keys keys{d->schema, std::nullopt};
        auto objects = in->list(ed::text(r.input, "S3KeyPrefix"));
        std::erase_if(objects, [&](const std::string& k) {
            return k.empty() || k.ends_with("/") || std::ranges::find(r.done, k) != r.done.end();
        });
        std::ranges::sort(objects);
        _bucket_at.reset();
        std::uint64_t since_check = 0;
        for (const auto& key : objects) {
            std::string errors;
            std::string conditions;
            const auto error = [&](std::string_view code, std::string_view why,
                                   std::string_view raw) {
                if (!report_errors) {
                    return;
                }
                errors += boost::json::serialize(boost::json::object{
                    {"InputObject", key},
                    {"ErrorCode", code},
                    {"ErrorMessage", why},
                    {"Record", raw},
                });
                errors.push_back('\n');
            };
            bool stop = false;
            const auto raw = in->get(key);
            if (!raw) {
                continue;
            }
            r.processed_bytes += raw->size();
            std::string body;
            try {
                body = export_formats::decompress(*raw, compression);
            } catch (const export_formats::format_error& e) {
                ++r.failed_objects;
                error("InputDecompressionError", e.what(), {});
                body.clear();
            }
            ed::read_records(
                body, format, ',', {}, {},
                [&](const boost::json::object& record) {
                    if (stop) {
                        return;
                    }
                    if (++since_check >= bd::k_cancel_check) {
                        since_check = 0;
                        if (cancelled(r)) {
                            stop = true;
                            return;
                        }
                    }
                    ++r.processed_items;
                    const auto refuse = [&](std::string_view why) {
                        ++r.failed_items;
                        error("ValidationException", why, boost::json::serialize(record));
                    };
                    auto compiled = compile_request("PutItem", record, keys);
                    if (!compiled) {
                        refuse(compiled.error().message.value_or("invalid expression"));
                        return;
                    }
                    auto it = item_from_json(record.at("Item").as_object());
                    if (!it) {
                        refuse(it.error().message.value_or("invalid item"));
                        return;
                    }
                    for (const auto& check :
                         {validate_key_attributes(*it, d->schema), check_item_size(*it),
                          validate_index_keys(*it, d->lsis),
                          validate_global_index_keys(*it, gsis)}) {
                        if (check) {
                            refuse(check->message.value_or("invalid item"));
                            return;
                        }
                    }
                    const auto bytes = item_size(*it);
                    take(r.max_units, bd::units_of(bytes));
                    item_command c;
                    c.op = item_command::kind::put;
                    c.table_id = d->table_id;
                    c.schema = d->schema;
                    c.payload = *it;
                    c.expressions = std::move(*compiled);
                    c.lsis = d->lsis;
                    c.gsis = gsis;
                    c.log_changes = d->logs_changes();
                    c.stream_ms = d->stream_label();
                    c.region = d->region;
                    c.old_on_condition_failure = report_conditions;
                    const auto res = _backend.write(c);
                    if (res.outcome == item_result::status::ok) {
                        r.put_bytes += bytes;
                    } else if (res.outcome == item_result::status::condition_failed) {
                        ++r.condition_failures;
                        if (report_conditions) {
                            boost::json::object line{{"InputObject", key},
                                                     {"Item", record.at("Item")}};
                            if (res.attributes) {
                                line["ExistingItem"] = item_to_json(*res.attributes);
                            }
                            conditions += boost::json::serialize(line);
                            conditions.push_back('\n');
                        }
                    } else {
                        refuse(res.error_message.empty() ? res.error_type : res.error_message);
                    }
                },
                [&](std::string_view why, std::string_view rec, bool one_item) {
                    if (one_item) {
                        ++r.processed_items;
                        ++r.failed_items;
                    } else {
                        ++r.failed_objects;
                    }
                    error("ParseError", why, rec);
                });
            for (const auto& [dir, lines] :
                 {std::pair<std::string_view, const std::string*>{"errors", &errors},
                  {"condition-check-failures", &conditions}}) {
                if (!lines->empty()) {
                    out->put(
                        root + std::string(dir) + "/" + pitr_detail::digits(r.reports, 6) + ".json",
                        *lines);
                    ++r.reports;
                }
            }
            if (stop) {
                end(r, "CANCELLED", "UserCancelled", "The bulk operation was cancelled");
                return;
            }
            r.done.push_back(key);
            save(r);
        }
        end(r, r.failed_items + r.failed_objects > 0 ? "COMPLETED_WITH_ERRORS" : "COMPLETED");
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    bucket_resolver _buckets;
    bulk_put_worker_options _options;
    std::atomic<bool> _poked{true};
    std::chrono::steady_clock::time_point _next_list{};
    std::optional<std::chrono::steady_clock::time_point> _bucket_at;
    double _tokens = 0;
};

}  // namespace kythira::dynamodb
