// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file export_import.hpp
/// @brief `ExportTableToPointInTime`, `DescribeExport`, `ListExports`,
///        `ImportTable`, `DescribeImport` and `ListImports`
///        (`.kiro/specs/dynamodb-compatible-api/` task 20, Requirement
///        17.3).
///
/// ## Buckets
///
/// A request names an S3 bucket; a `bucket_resolver` maps the name to a
/// `backup_store` (Requirement 17.3's "`s3://` URIs mapped to configured
/// stores"). The gateway maps a bucket to a directory under
/// `KYTHIRA_DYNAMODB_BUCKET_DIR` (or `<backup dir>/buckets`), to the S3
/// bucket of that name on the backup store's endpoint, or, for one node on
/// the memory engine, to a store in memory. A name that is not a valid S3
/// bucket name, or that the resolver does not map, is refused.
///
/// ## Jobs
///
/// Each export and import is a JSON record in the backup store, under
/// `exports/<id>/export.json` and `imports/<id>/import.json`, so neither
/// needs catalog state, as backups need none. `<id>` has the backup id's
/// form; the ARNs are the table's with `/export/<id>` and `/import/<id>`.
/// `export_import_worker`, on the catalog leader, runs every IN_PROGRESS
/// job; a worker that loses leadership leaves the job IN_PROGRESS and the
/// next leader runs it again from the start (every object is rewritten
/// whole, every put is idempotent). Records are kept 90 days after they
/// end, as AWS lists jobs for 90 days.
///
/// ## Exports
///
/// An export needs point-in-time recovery on the table, as on AWS. It is
/// written in AWS's layout under `<S3Prefix>/AWSDynamoDB/<id>/` in the
/// bucket: `_started`, `data/<n>.json.gz` (or `.ion.gz`),
/// `manifest-files.json` (one line per data file: item count, base64
/// MD5, ETag, key) and `manifest-summary.json` with
/// `manifest-summary.md5`. Data lines are `{"Item":{...}}` for a full
/// export and `{"Metadata":{"WriteTimestampMicros":...},"Keys":...,
/// "OldImage":...,"NewImage":...}` for an incremental one, in DynamoDB
/// JSON or Ion (`export_formats.hpp`).
///
/// - A full export with no `ExportTime` reads the table now, with
///   consistent range reads (the backup worker's read).
/// - A full export at `ExportTime` T rebuilds the table at T from the
///   point-in-time archive: the newest base that ended by T, then every
///   archived record up to T (`pitr_detail::replay`).
/// - An incremental export from F to T rebuilds the table at F the same
///   way, then applies the records after F up to T, and writes one line
///   per item they touched: its keys, the time of its last write, its
///   image at F (`OldImage`, with `NEW_AND_OLD_IMAGES`) and at T
///   (`NewImage`; none for an item deleted by T), as AWS does.
///
/// Deviations: the rebuild holds the table in memory, so a point-in-time
/// or incremental export of a table larger than the catalog leader's memory
/// fails; `S3SseAlgorithm` and `S3SseKmsKeyId` are recorded and reported
/// but Kythira's stores apply no server-side encryption of their own; and
/// without zlib (`export_formats.hpp`) data files are written uncompressed.
///
/// ## Imports
///
/// `ImportTable` creates the table CREATING from `TableCreationParameters`
/// (checked as `CreateTable` checks them), its `table_restore` naming the
/// import's ARN, so that it stays CREATING until loaded, the backup worker
/// and the archiver leave it alone, and `DescribeTable` shows no
/// `RestoreSummary`. The worker reads every object under `S3KeyPrefix`,
/// decompresses it (`NONE`, `GZIP`, `ZSTD`), parses it (`DYNAMODB_JSON`
/// and `ION` lines carrying `Item`, or `CSV`), and puts each item through
/// the ordinary write path, so GSIs are built. An item that cannot be read
/// or put (no key, a key of the wrong type, over 400 KB) counts in
/// `ErrorCount` and is skipped, as on AWS, where it would be logged to
/// CloudWatch Logs. When every object is read the record is COMPLETED and
/// the table ACTIVE. An import with no objects under the prefix FAILS, and
/// its table is deleted.
///
/// CSV follows AWS: the first line (or `HeaderList`) names the columns,
/// cells of the table's and its GSIs' key attributes take their declared
/// types (N, or base64 for B), and every other cell is a string. Empty
/// cells are left out of the item.

#include <kythira/dynamodb/backend.hpp>
#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/errors.hpp>
#include <kythira/dynamodb/export_formats.hpp>
#include <kythira/dynamodb/front_end.hpp>
#include <kythira/dynamodb/global_index.hpp>
#include <kythira/dynamodb/item_codec.hpp>
#include <kythira/dynamodb/key_layout.hpp>
#include <kythira/dynamodb/pitr.hpp>
#include <kythira/dynamodb/table_api.hpp>

#include <raft/aws_json_protocol.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kythira::dynamodb {

// -- Buckets ------------------------------------------------------------------

/// Maps an S3 bucket name to the store that holds it; nullptr when the
/// name is not mapped.
using bucket_resolver = std::function<std::shared_ptr<backup_store>(const std::string& bucket)>;

/// Buckets as directories under `root`, made on first use.
[[nodiscard]] inline auto directory_buckets(std::filesystem::path root) -> bucket_resolver {
    auto made = std::make_shared<std::map<std::string, std::shared_ptr<backup_store>>>();
    auto mu = std::make_shared<std::mutex>();
    return [root = std::move(root), made, mu](const std::string& bucket) {
        const std::lock_guard lock(*mu);
        auto& s = (*made)[bucket];
        if (!s) {
            s = std::make_shared<directory_backup_store>(root / bucket);
        }
        return s;
    };
}

/// Buckets in memory, made on first use: one node on the memory engine,
/// and tests. `store(name)` hands a test the bucket to fill or read.
class memory_buckets {
public:
    [[nodiscard]] auto store(const std::string& bucket) -> std::shared_ptr<memory_backup_store> {
        const std::lock_guard lock(_mu);
        auto& s = _buckets[bucket];
        if (!s) {
            s = std::make_shared<memory_backup_store>();
        }
        return s;
    }
    [[nodiscard]] auto resolver() -> bucket_resolver {
        return [this](const std::string& bucket) -> std::shared_ptr<backup_store> {
            return store(bucket);
        };
    }

private:
    std::mutex _mu;
    std::map<std::string, std::shared_ptr<memory_backup_store>> _buckets;
};

// -- Records ------------------------------------------------------------------

/// An export's record (`export.json`).
struct export_record {
    std::string id;
    std::string arn;
    std::string table_name;
    std::string table_arn;
    std::uint64_t table_id = 0;
    /// IN_PROGRESS, COMPLETED or FAILED.
    std::string status = "IN_PROGRESS";
    std::uint64_t start_ms = 0;
    std::uint64_t end_ms = 0;
    /// The time exported (FULL_EXPORT): the request's `ExportTime`, or the
    /// start when it gave none (`live`).
    std::uint64_t export_ms = 0;
    bool live = false;
    std::string client_token;
    std::string bucket;
    std::string bucket_owner;
    std::string prefix;
    std::string sse_algorithm;
    std::string sse_kms_key_id;
    /// DYNAMODB_JSON or ION.
    std::string format = "DYNAMODB_JSON";
    /// FULL_EXPORT or INCREMENTAL_EXPORT.
    std::string type = "FULL_EXPORT";
    std::uint64_t from_ms = 0;
    std::uint64_t to_ms = 0;
    /// NEW_AND_OLD_IMAGES or NEW_IMAGE.
    std::string view = "NEW_AND_OLD_IMAGES";
    std::string failure_code;
    std::string failure_message;
    std::uint64_t billed_bytes = 0;
    std::uint64_t item_count = 0;
    /// The manifest summary's key in the bucket, once written.
    std::string manifest;

    friend auto operator==(const export_record&, const export_record&) -> bool = default;
};

/// An import's record (`import.json`).
struct import_record {
    std::string id;
    std::string arn;
    std::string table_name;
    std::string table_arn;
    std::uint64_t table_id = 0;
    /// IN_PROGRESS, COMPLETED or FAILED.
    std::string status = "IN_PROGRESS";
    std::uint64_t start_ms = 0;
    std::uint64_t end_ms = 0;
    std::string client_token;
    std::string bucket;
    std::string bucket_owner;
    std::string key_prefix;
    /// DYNAMODB_JSON, ION or CSV.
    std::string format;
    std::string csv_delimiter = ",";
    std::vector<std::string> csv_headers;
    /// NONE, GZIP or ZSTD.
    std::string compression = "NONE";
    /// The request's `TableCreationParameters`, as given.
    boost::json::object creation;
    std::string failure_code;
    std::string failure_message;
    std::uint64_t processed_bytes = 0;
    std::uint64_t processed_items = 0;
    std::uint64_t imported_items = 0;
    std::uint64_t errors = 0;

    friend auto operator==(const import_record&, const import_record&) -> bool = default;
};

namespace export_detail {

inline constexpr std::string_view k_exports_prefix = "exports/";
inline constexpr std::string_view k_imports_prefix = "imports/";
inline constexpr std::uint64_t k_minute_ms = 60'000;
inline constexpr std::uint64_t k_hour_ms = 60 * k_minute_ms;
inline constexpr std::uint64_t k_day_ms = 24 * k_hour_ms;
/// How long finished jobs stay listed.
inline constexpr std::uint64_t k_retention_ms = 90 * k_day_ms;
/// How long a `ClientToken` names its job.
inline constexpr std::uint64_t k_token_ms = 8 * k_hour_ms;

[[nodiscard]] inline auto export_key(std::string_view id) -> std::string {
    return std::string(k_exports_prefix) + std::string(id) + "/export.json";
}
[[nodiscard]] inline auto import_key(std::string_view id) -> std::string {
    return std::string(k_imports_prefix) + std::string(id) + "/import.json";
}

/// The id at the end of an export or import ARN (`.../<kind>/<id>`), in
/// the backup id's form, or nothing.
[[nodiscard]] inline auto id_of(std::string_view arn, std::string_view kind)
    -> std::optional<std::string> {
    const auto marker = "/" + std::string(kind) + "/";
    const auto at = arn.rfind(marker);
    if (at == std::string_view::npos || !arn.starts_with("arn:")) {
        return std::nullopt;
    }
    // The backup id check, on the same form.
    const auto id = arn.substr(at + marker.size());
    if (!backup_detail::id_of_arn("arn:x/backup/" + std::string(id))) {
        return std::nullopt;
    }
    return std::string(id);
}

/// S3's bucket naming rules: 3 to 63 lowercase letters, digits, dots and
/// hyphens, starting and ending with a letter or digit, no `..`.
[[nodiscard]] inline auto valid_bucket(std::string_view b) -> bool {
    if (b.size() < 3 || b.size() > 63 || b.find("..") != std::string_view::npos) {
        return false;
    }
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return alnum(b.front()) && alnum(b.back()) &&
           std::ranges::all_of(b, [&](char c) { return alnum(c) || c == '.' || c == '-'; });
}

/// The export's directory in its bucket: `<prefix>/AWSDynamoDB/<id>/`.
[[nodiscard]] inline auto export_root(std::string_view prefix, std::string_view id) -> std::string {
    std::string out(prefix);
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    return out + "AWSDynamoDB/" + std::string(id) + "/";
}

/// `2020-11-04T07:28:34.028Z`.
[[nodiscard]] inline auto iso8601(std::uint64_t ms) -> std::string {
    const auto secs = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&secs, &tm);
    std::array<char, 40> buf{};
    const auto n = std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                                 tm.tm_min, tm.tm_sec, static_cast<int>(ms % 1000));
    return {buf.data(), static_cast<std::size_t>(n)};
}

[[nodiscard]] inline auto text(const boost::json::object& o, std::string_view key) -> std::string {
    return backup_detail::text(o, key);
}
[[nodiscard]] inline auto num(const boost::json::object& o, std::string_view key) -> std::uint64_t {
    const auto* v = o.if_contains(key);
    return v != nullptr && v->is_number() ? v->to_number<std::uint64_t>() : 0;
}

[[nodiscard]] inline auto encode_export(const export_record& r) -> std::string {
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
        {"exportMs", r.export_ms},
        {"live", r.live},
        {"clientToken", r.client_token},
        {"bucket", r.bucket},
        {"bucketOwner", r.bucket_owner},
        {"prefix", r.prefix},
        {"sseAlgorithm", r.sse_algorithm},
        {"sseKmsKeyId", r.sse_kms_key_id},
        {"format", r.format},
        {"type", r.type},
        {"fromMs", r.from_ms},
        {"toMs", r.to_ms},
        {"view", r.view},
        {"failureCode", r.failure_code},
        {"failureMessage", r.failure_message},
        {"billedBytes", r.billed_bytes},
        {"itemCount", r.item_count},
        {"manifest", r.manifest},
    });
}

[[nodiscard]] inline auto decode_export(std::string_view body) -> std::optional<export_record> {
    try {
        const auto v = boost::json::parse(body);
        const auto& o = v.as_object();
        if (num(o, "formatVersion") != 1) {
            return std::nullopt;
        }
        export_record r;
        r.id = text(o, "id");
        r.arn = text(o, "arn");
        r.table_name = text(o, "tableName");
        r.table_arn = text(o, "tableArn");
        r.table_id = num(o, "tableId");
        r.status = text(o, "status");
        r.start_ms = num(o, "startMs");
        r.end_ms = num(o, "endMs");
        r.export_ms = num(o, "exportMs");
        r.live = o.at("live").as_bool();
        r.client_token = text(o, "clientToken");
        r.bucket = text(o, "bucket");
        r.bucket_owner = text(o, "bucketOwner");
        r.prefix = text(o, "prefix");
        r.sse_algorithm = text(o, "sseAlgorithm");
        r.sse_kms_key_id = text(o, "sseKmsKeyId");
        r.format = text(o, "format");
        r.type = text(o, "type");
        r.from_ms = num(o, "fromMs");
        r.to_ms = num(o, "toMs");
        r.view = text(o, "view");
        r.failure_code = text(o, "failureCode");
        r.failure_message = text(o, "failureMessage");
        r.billed_bytes = num(o, "billedBytes");
        r.item_count = num(o, "itemCount");
        r.manifest = text(o, "manifest");
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

[[nodiscard]] inline auto encode_import(const import_record& r) -> std::string {
    boost::json::array headers;
    for (const auto& h : r.csv_headers) {
        headers.emplace_back(h);
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
        {"bucket", r.bucket},
        {"bucketOwner", r.bucket_owner},
        {"keyPrefix", r.key_prefix},
        {"format", r.format},
        {"csvDelimiter", r.csv_delimiter},
        {"csvHeaders", std::move(headers)},
        {"compression", r.compression},
        {"creation", r.creation},
        {"failureCode", r.failure_code},
        {"failureMessage", r.failure_message},
        {"processedBytes", r.processed_bytes},
        {"processedItems", r.processed_items},
        {"importedItems", r.imported_items},
        {"errors", r.errors},
    });
}

[[nodiscard]] inline auto decode_import(std::string_view body) -> std::optional<import_record> {
    try {
        const auto v = boost::json::parse(body);
        const auto& o = v.as_object();
        if (num(o, "formatVersion") != 1) {
            return std::nullopt;
        }
        import_record r;
        r.id = text(o, "id");
        r.arn = text(o, "arn");
        r.table_name = text(o, "tableName");
        r.table_arn = text(o, "tableArn");
        r.table_id = num(o, "tableId");
        r.status = text(o, "status");
        r.start_ms = num(o, "startMs");
        r.end_ms = num(o, "endMs");
        r.client_token = text(o, "clientToken");
        r.bucket = text(o, "bucket");
        r.bucket_owner = text(o, "bucketOwner");
        r.key_prefix = text(o, "keyPrefix");
        r.format = text(o, "format");
        r.csv_delimiter = text(o, "csvDelimiter");
        for (const auto& h : o.at("csvHeaders").as_array()) {
            r.csv_headers.emplace_back(h.as_string());
        }
        r.compression = text(o, "compression");
        r.creation = o.at("creation").as_object();
        r.failure_code = text(o, "failureCode");
        r.failure_message = text(o, "failureMessage");
        r.processed_bytes = num(o, "processedBytes");
        r.processed_items = num(o, "processedItems");
        r.imported_items = num(o, "importedItems");
        r.errors = num(o, "errors");
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Every record under `prefix` whose key ends with `leaf`, decoded, oldest
/// first.
template<typename Record, typename Decode>
[[nodiscard]] auto all_records(backup_store& store, std::string_view prefix, std::string_view leaf,
                               Decode decode) -> std::vector<Record> {
    std::vector<Record> out;
    for (const auto& key : store.list(std::string(prefix))) {
        if (!key.ends_with(leaf)) {
            continue;
        }
        if (const auto body = store.get(key)) {
            if (auto r = decode(*body)) {
                out.push_back(std::move(*r));
            }
        }
    }
    std::ranges::sort(out, [](const Record& a, const Record& b) {
        return a.start_ms != b.start_ms ? a.start_ms < b.start_ms : a.id < b.id;
    });
    return out;
}

[[nodiscard]] inline auto all_exports(backup_store& store) -> std::vector<export_record> {
    return all_records<export_record>(store, k_exports_prefix, "/export.json",
                                      [](std::string_view b) { return decode_export(b); });
}
[[nodiscard]] inline auto all_imports(backup_store& store) -> std::vector<import_record> {
    return all_records<import_record>(store, k_imports_prefix, "/import.json",
                                      [](std::string_view b) { return decode_import(b); });
}

[[nodiscard]] inline auto modeled(std::string_view type, std::string message) -> aws_json::error {
    return table_api_detail::modeled(type, std::move(message));
}

[[nodiscard]] inline auto ms_of(const boost::json::value& seconds) -> std::uint64_t {
    return pitr_detail::ms_of_seconds(seconds.to_number<double>());
}

/// The bucket's store, or the refusal.
[[nodiscard]] inline auto bucket_store(const bucket_resolver& buckets, const std::string& bucket)
    -> aws_json::result<std::shared_ptr<backup_store>> {
    if (!valid_bucket(bucket)) {
        return validation_error("Invalid S3 bucket name: " + bucket);
    }
    std::shared_ptr<backup_store> s;
    if (buckets) {
        s = buckets(bucket);
    }
    if (!s) {
        return validation_error("S3 bucket " + bucket +
                                " is not mapped to a store on this cluster: set "
                                "KYTHIRA_DYNAMODB_BUCKET_DIR, KYTHIRA_DYNAMODB_BACKUP_DIR or "
                                "KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET");
    }
    return s;
}

/// `ListImports`' `NextToken`: the model wants 112 to 1024 lowercase hex
/// digits in runs of 16, so the last ARN listed is written in hex and
/// padded with zero bytes.
[[nodiscard]] inline auto import_page_token(std::string_view arn) -> std::string {
    static constexpr std::string_view k_hex = "0123456789abcdef";
    std::string out;
    for (const auto ch : arn) {
        const auto c = static_cast<unsigned char>(ch);
        out.push_back(k_hex[c >> 4U]);
        out.push_back(k_hex[c & 0xFU]);
    }
    while (out.size() < 112 || out.size() % 16 != 0) {
        out += "00";
    }
    return out;
}

/// The ARN an `import_page_token` holds, or empty.
[[nodiscard]] inline auto import_of_page_token(std::string_view token) -> std::string {
    std::string out;
    const auto digit = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i + 1 < token.size(); i += 2) {
        const auto hi = digit(token[i]);
        const auto lo = digit(token[i + 1]);
        if (hi < 0 || lo < 0) {
            return {};
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    while (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

/// An item's DynamoDB JSON keys under `schema`.
[[nodiscard]] inline auto keys_of(const item& it, const key_schema& schema) -> boost::json::object {
    boost::json::object out;
    if (const auto* pk = find_attribute(it, schema.partition_name)) {
        out[schema.partition_name] = attribute_to_json(*pk);
    }
    if (schema.sort_name) {
        if (const auto* sk = find_attribute(it, *schema.sort_name)) {
            out[*schema.sort_name] = attribute_to_json(*sk);
        }
    }
    return out;
}

/// A DynamoDB JSON record from an Ion one: `{Item:{...}}` reads as
/// `{"M":{"Item":{"M":{...}}}}`, and the condition members a bulk put
/// record may carry (`ConditionExpression`, `ExpressionAttributeNames`,
/// `ExpressionAttributeValues`) come back to their request forms.
[[nodiscard]] inline auto record_of_ion(const boost::json::value& v)
    -> std::optional<boost::json::object> {
    const auto* m = v.as_object().if_contains("M");
    if (m == nullptr) {
        return std::nullopt;
    }
    const auto& fields = m->as_object();
    const auto* wrapped = fields.if_contains("Item");
    const auto* inner = wrapped != nullptr ? wrapped->as_object().if_contains("M") : nullptr;
    if (inner == nullptr) {
        return std::nullopt;
    }
    boost::json::object out{{"Item", *inner}};
    if (const auto* c = fields.if_contains("ConditionExpression")) {
        const auto* sv = c->as_object().if_contains("S");
        if (sv == nullptr) {
            return std::nullopt;
        }
        out["ConditionExpression"] = *sv;
    }
    if (const auto* n = fields.if_contains("ExpressionAttributeNames")) {
        const auto* names = n->as_object().if_contains("M");
        if (names == nullptr) {
            return std::nullopt;
        }
        boost::json::object plain;
        for (const auto& kv : names->as_object()) {
            const auto* sv = kv.value().as_object().if_contains("S");
            if (sv == nullptr) {
                return std::nullopt;
            }
            plain[kv.key()] = *sv;
        }
        out["ExpressionAttributeNames"] = std::move(plain);
    }
    if (const auto* x = fields.if_contains("ExpressionAttributeValues")) {
        const auto* values = x->as_object().if_contains("M");
        if (values == nullptr) {
            return std::nullopt;
        }
        out["ExpressionAttributeValues"] = *values;
    }
    return out;
}

/// Reads one decompressed input object in `format` (`DYNAMODB_JSON`,
/// `ION` or `CSV`), calling `on_record` with each record as a DynamoDB
/// JSON object carrying `Item`, and `on_error(why, raw, one_item)` for a
/// record that cannot be read (`one_item`) or for the rest of an object
/// that stops making sense (not `one_item`). CSV uses `delimiter`,
/// `headers` (empty: the first row), and `typed` for the attributes whose
/// cells take a declared type.
template<typename OnRecord, typename OnError>
void read_records(std::string_view body, std::string_view format, char delimiter,
                  const std::vector<std::string>& headers,
                  const std::map<std::string, attribute_type>& typed, OnRecord on_record,
                  OnError on_error) {
    if (format == "DYNAMODB_JSON") {
        std::string_view rest(body);
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            const auto line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
                continue;
            }
            boost::system::error_code ec;
            const auto v = boost::json::parse(line, ec);
            if (ec || !v.is_object()) {
                on_error("The record is not a JSON object", line, true);
                continue;
            }
            const auto* it = v.as_object().if_contains("Item");
            if (it == nullptr || !it->is_object()) {
                on_error("The record has no Item map", line, true);
                continue;
            }
            on_record(v.as_object());
        }
        return;
    }
    if (format == "ION") {
        try {
            export_formats::ion_reader reader(body);
            while (auto v = reader.next()) {
                if (auto record = record_of_ion(*v)) {
                    on_record(*record);
                } else {
                    on_error("The record has no Item struct", boost::json::serialize(*v), true);
                }
            }
        } catch (const export_formats::format_error& e) {
            // The rest of an object that stops making sense is lost.
            on_error(e.what(), {}, false);
        }
        return;
    }
    std::vector<std::string> names = headers;
    bool first = names.empty();
    try {
        export_formats::read_csv(body, delimiter, [&](const std::vector<std::string>& fields) {
            if (first) {
                names = fields;
                first = false;
                return true;
            }
            if (fields.size() > names.size()) {
                std::string raw;
                for (const auto& f : fields) {
                    raw += (raw.empty() ? "" : std::string(1, delimiter)) + f;
                }
                on_error("The row has more cells than there are columns", raw, true);
                return true;
            }
            boost::json::object image;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].empty() || names[i].empty()) {
                    continue;
                }
                const auto t = typed.find(names[i]);
                const auto* type = t == typed.end()                 ? "S"
                                   : t->second == attribute_type::N ? "N"
                                   : t->second == attribute_type::B ? "B"
                                                                    : "S";
                image[names[i]] = boost::json::object{{type, fields[i]}};
            }
            on_record(boost::json::object{{"Item", std::move(image)}});
            return true;
        });
    } catch (const export_formats::format_error& e) {
        on_error(e.what(), {}, false);
    }
}

/// Writes an export's data files: lines go in, objects of about
/// `k_object_items` lines or `k_object_bytes` come out, gzipped when the
/// build has zlib, each named in `manifest_lines`.
class data_writer {
public:
    data_writer(backup_store& bucket, std::string root, bool ion)
        : _bucket(bucket), _root(std::move(root)), _ion(ion) {}

    void line(std::string_view l) {
        if (_chunk.empty() && _ion) {
            _chunk += export_formats::k_ion_version_marker;
            _chunk.push_back('\n');
        }
        _chunk += l;
        _chunk.push_back('\n');
        ++_chunk_items;
        ++items;
        if (_chunk_items >= backup_detail::k_object_items ||
            _chunk.size() >= backup_detail::k_object_bytes) {
            flush();
        }
    }

    /// Writes what is left and removes data files of an earlier, longer
    /// run.
    void finish() {
        flush();
        for (const auto& key : _bucket.list(_root + "data/")) {
            if (std::ranges::find(_written, key) == _written.end()) {
                _bucket.remove(key);
            }
        }
    }

    std::uint64_t items = 0;
    std::string manifest_lines;

private:
    void flush() {
        if (_chunk_items == 0) {
            return;
        }
        std::string name = pitr_detail::digits(_written.size(), 6) + (_ion ? ".ion" : ".json");
        std::string body;
        if (export_formats::has_gzip()) {
            body = export_formats::gzip(_chunk);
            name += ".gz";
        } else {
            body = std::move(_chunk);
        }
        const auto key = _root + "data/" + name;
        _bucket.put(key, body);
        manifest_lines += boost::json::serialize(boost::json::object{
            {"itemCount", _chunk_items},
            {"md5", export_formats::md5_base64(body)},
            {"etag", export_formats::md5_hex(body)},
            {"dataFileS3Key", key},
        });
        manifest_lines.push_back('\n');
        _written.push_back(key);
        _chunk.clear();
        _chunk_items = 0;
    }

    backup_store& _bucket;
    std::string _root;
    bool _ion;
    std::string _chunk;
    std::uint64_t _chunk_items = 0;
    std::vector<std::string> _written;
};

/// One full-export line.
[[nodiscard]] inline auto item_line(const boost::json::object& image, bool ion) -> std::string {
    if (!ion) {
        return boost::json::serialize(boost::json::object{{"Item", image}});
    }
    std::string out = "{Item:";
    export_formats::write_ion_item(out, image);
    out.push_back('}');
    return out;
}

/// One incremental-export line.
[[nodiscard]] inline auto change_line(std::uint64_t written_ms, const boost::json::object& keys,
                                      const boost::json::object* old_image,
                                      const boost::json::object* new_image, bool ion)
    -> std::string {
    const auto micros = std::to_string(written_ms * 1000);
    if (!ion) {
        boost::json::object o{
            {"Metadata",
             boost::json::object{{"WriteTimestampMicros", boost::json::object{{"N", micros}}}}},
            {"Keys", keys},
        };
        if (old_image != nullptr) {
            o["OldImage"] = *old_image;
        }
        if (new_image != nullptr) {
            o["NewImage"] = *new_image;
        }
        return boost::json::serialize(o);
    }
    std::string out = "{Metadata:{WriteTimestampMicros:" + micros + "},Keys:";
    export_formats::write_ion_item(out, keys);
    if (old_image != nullptr) {
        out += ",OldImage:";
        export_formats::write_ion_item(out, *old_image);
    }
    if (new_image != nullptr) {
        out += ",NewImage:";
        export_formats::write_ion_item(out, *new_image);
    }
    out.push_back('}');
    return out;
}

}  // namespace export_detail

// -- The API ------------------------------------------------------------------

struct export_import_options {
    /// The shortest and longest incremental export period (AWS: 15 minutes
    /// and 24 hours).
    std::uint64_t min_incremental_ms = 15 * export_detail::k_minute_ms;
    std::uint64_t max_incremental_ms = export_detail::k_day_ms;
};

/// The six export and import actions over a backend, the backup store
/// (for job records and the point-in-time archive) and the buckets.
template<dynamo_backend Backend> class export_import_api {
public:
    using result = aws_json::result<boost::json::object>;

    export_import_api(Backend& backend, std::shared_ptr<backup_store> store,
                      bucket_resolver buckets, export_import_options options = {})
        : _backend(backend),
          _store(std::move(store)),
          _buckets(std::move(buckets)),
          _options(options) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    void set_buckets(bucket_resolver buckets) { _buckets = std::move(buckets); }
    /// Called when a job is started, so the worker looks at once.
    void on_started(std::function<void()> fn) { _started = std::move(fn); }

    template<typename FrontEnd> void register_with(FrontEnd& fe) {
        fe.on("ExportTableToPointInTime",
              [this](call_context& c, boost::json::object& in) { return start_export(c, in); });
        fe.on("DescribeExport",
              [this](call_context& c, boost::json::object& in) { return describe_export(c, in); });
        fe.on("ListExports",
              [this](call_context& c, boost::json::object& in) { return list_exports(c, in); });
        fe.on("ImportTable",
              [this](call_context& c, boost::json::object& in) { return start_import(c, in); });
        fe.on("DescribeImport",
              [this](call_context& c, boost::json::object& in) { return describe_import(c, in); });
        fe.on("ListImports",
              [this](call_context& c, boost::json::object& in) { return list_imports(c, in); });
    }

    auto start_export(call_context& ctx, boost::json::object& in) -> result {
        namespace ed = export_detail;
        const auto table_arn = ed::text(in, "TableArn");
        const auto name = ctx.table_name(table_arn);
        const auto d = _backend.describe(name);
        if (!d) {
            return ed::modeled("TableNotFoundException", "Table not found: " + table_arn);
        }
        if (!d->pitr.enabled || !_store) {
            return ed::modeled("PointInTimeRecoveryUnavailableException",
                               "Point in time recovery is not enabled for table '" + name + "'");
        }
        export_record r;
        r.bucket = ed::text(in, "S3Bucket");
        auto bucket = ed::bucket_store(_buckets, r.bucket);
        if (!bucket) {
            return bucket.error();
        }
        r.format = ed::text(in, "ExportFormat");
        if (r.format.empty()) {
            r.format = "DYNAMODB_JSON";
        }
        if (r.format != "DYNAMODB_JSON" && r.format != "ION") {
            return validation_error("ExportFormat must be DYNAMODB_JSON or ION");
        }
        r.type = ed::text(in, "ExportType");
        if (r.type.empty()) {
            r.type = "FULL_EXPORT";
        }
        r.sse_algorithm = ed::text(in, "S3SseAlgorithm");
        if (!r.sse_algorithm.empty() && r.sse_algorithm != "AES256" && r.sse_algorithm != "KMS") {
            return validation_error("S3SseAlgorithm must be AES256 or KMS");
        }
        r.sse_kms_key_id = ed::text(in, "S3SseKmsKeyId");
        r.bucket_owner = ed::text(in, "S3BucketOwner");
        r.prefix = ed::text(in, "S3Prefix");
        r.client_token = ed::text(in, "ClientToken");
        const auto now = _backend.now_ms();
        const auto earliest = pitr_detail::earliest(d->pitr, now);
        const auto latest = d->pitr.latest_ms;
        const auto window_error = [&](std::string what) {
            return ed::modeled("InvalidExportTimeException",
                               what + " must be between " +
                                   std::to_string(table_api_detail::millis_to_seconds(earliest)) +
                                   " and " +
                                   std::to_string(table_api_detail::millis_to_seconds(latest)));
        };
        const auto* at = in.if_contains("ExportTime");
        if (r.type == "FULL_EXPORT") {
            if (at != nullptr && at->is_number()) {
                r.export_ms = ed::ms_of(*at);
                if (earliest == 0 || r.export_ms < earliest || r.export_ms > latest) {
                    return window_error("ExportTime");
                }
            } else {
                r.live = true;
                r.export_ms = now;
            }
        } else if (r.type == "INCREMENTAL_EXPORT") {
            if (at != nullptr) {
                return validation_error("ExportTime cannot be specified for an incremental export");
            }
            const auto* spec = in.if_contains("IncrementalExportSpecification");
            if (spec == nullptr || !spec->is_object() ||
                !spec->as_object().contains("ExportFromTime")) {
                return validation_error(
                    "IncrementalExportSpecification with ExportFromTime is required for an "
                    "incremental export");
            }
            const auto& s = spec->as_object();
            r.from_ms = ed::ms_of(s.at("ExportFromTime"));
            const auto* to = s.if_contains("ExportToTime");
            r.to_ms = to != nullptr && to->is_number() ? ed::ms_of(*to) : latest;
            r.view = ed::text(s, "ExportViewType");
            if (r.view.empty()) {
                r.view = "NEW_AND_OLD_IMAGES";
            }
            if (r.view != "NEW_AND_OLD_IMAGES" && r.view != "NEW_IMAGE") {
                return validation_error("ExportViewType must be NEW_AND_OLD_IMAGES or NEW_IMAGE");
            }
            if (r.to_ms <= r.from_ms) {
                return validation_error("ExportToTime must be after ExportFromTime");
            }
            if (r.to_ms - r.from_ms < _options.min_incremental_ms ||
                r.to_ms - r.from_ms > _options.max_incremental_ms) {
                return validation_error(
                    "The incremental export period must be between " +
                    std::to_string(_options.min_incremental_ms / ed::k_minute_ms) +
                    " minutes and " + std::to_string(_options.max_incremental_ms / ed::k_hour_ms) +
                    " hours");
            }
            if (earliest == 0 || r.from_ms < earliest || r.to_ms > latest) {
                return window_error("ExportFromTime and ExportToTime");
            }
        } else {
            return validation_error("ExportType must be FULL_EXPORT or INCREMENTAL_EXPORT");
        }
        r.table_name = d->name;
        r.table_arn = ctx.table_arn(d->name);
        r.table_id = d->table_id;
        if (!r.client_token.empty()) {
            for (const auto& old : ed::all_exports(*_store)) {
                if (old.client_token != r.client_token || now - old.start_ms > ed::k_token_ms) {
                    continue;
                }
                if (old.table_arn == r.table_arn && old.bucket == r.bucket &&
                    old.prefix == r.prefix && old.format == r.format && old.type == r.type &&
                    (old.live ? r.live : old.export_ms == r.export_ms) &&
                    old.from_ms == r.from_ms && old.to_ms == r.to_ms && old.view == r.view) {
                    return boost::json::object{{"ExportDescription", description(old)}};
                }
                return ed::modeled("ExportConflictException",
                                   "An export with the same client token but different "
                                   "parameters exists: " +
                                       old.arn);
            }
        }
        r.start_ms = now;
        r.id = backup_detail::new_backup_id(now);
        r.arn = r.table_arn + "/export/" + r.id;
        _store->put(ed::export_key(r.id), ed::encode_export(r));
        if (_started) {
            _started();
        }
        return boost::json::object{{"ExportDescription", description(r)}};
    }

    auto describe_export(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto arn = export_detail::text(in, "ExportArn");
        const auto r = find_export(arn);
        if (!r) {
            return export_detail::modeled("ExportNotFoundException", "Export not found: " + arn);
        }
        return boost::json::object{{"ExportDescription", description(*r)}};
    }

    auto list_exports(call_context& ctx, boost::json::object& in) -> result {
        namespace ed = export_detail;
        boost::json::array out;
        if (!_store) {
            return boost::json::object{{"ExportSummaries", std::move(out)}};
        }
        const auto table = ed::text(in, "TableArn");
        auto limit = ed::num(in, "MaxResults");
        if (in.contains("MaxResults") && (limit < 1 || limit > 25)) {
            return validation_error("MaxResults must be between 1 and 25");
        }
        if (limit == 0) {
            limit = 25;
        }
        const auto start = ed::text(in, "NextToken");
        bool started = start.empty();
        std::string last;
        bool more = false;
        for (const auto& r : ed::all_exports(*_store)) {
            if (!started) {
                started = r.arn == start;
                continue;
            }
            if (!ctx.owns_arn(r.arn) || (!table.empty() && r.table_arn != table)) {
                continue;
            }
            if (out.size() >= limit) {
                more = true;
                break;
            }
            out.push_back(boost::json::object{
                {"ExportArn", r.arn}, {"ExportStatus", r.status}, {"ExportType", r.type}});
            last = r.arn;
        }
        boost::json::object res{{"ExportSummaries", std::move(out)}};
        if (more) {
            res["NextToken"] = last;
        }
        return res;
    }

    auto start_import(call_context& ctx, boost::json::object& in) -> result {
        namespace ed = export_detail;
        if (!_store) {
            return validation_error(
                "Imports are not configured on this cluster: set KYTHIRA_DYNAMODB_BACKUP_DIR "
                "or KYTHIRA_DYNAMODB_BACKUP_S3_BUCKET");
        }
        import_record r;
        const auto& source = in.at("S3BucketSource").as_object();
        r.bucket = ed::text(source, "S3Bucket");
        r.bucket_owner = ed::text(source, "S3BucketOwner");
        r.key_prefix = ed::text(source, "S3KeyPrefix");
        auto bucket = ed::bucket_store(_buckets, r.bucket);
        if (!bucket) {
            return bucket.error();
        }
        r.format = ed::text(in, "InputFormat");
        if (r.format != "DYNAMODB_JSON" && r.format != "ION" && r.format != "CSV") {
            return validation_error("InputFormat must be DYNAMODB_JSON, ION or CSV");
        }
        if (const auto* opts = in.if_contains("InputFormatOptions");
            opts != nullptr && opts->is_object()) {
            if (r.format != "CSV") {
                return validation_error("InputFormatOptions apply only to CSV input");
            }
            if (const auto* csv = opts->as_object().if_contains("Csv");
                csv != nullptr && csv->is_object()) {
                const auto delimiter = ed::text(csv->as_object(), "Delimiter");
                if (!delimiter.empty()) {
                    if (delimiter.size() != 1 ||
                        std::string_view(",\t:;| ").find(delimiter[0]) == std::string_view::npos) {
                        return validation_error(
                            "Delimiter must be one of ',', '\\t', ':', ';', '|' or ' '");
                    }
                    r.csv_delimiter = delimiter;
                }
                if (const auto* h = csv->as_object().if_contains("HeaderList");
                    h != nullptr && h->is_array()) {
                    for (const auto& e : h->as_array()) {
                        r.csv_headers.emplace_back(e.as_string());
                    }
                }
            }
        }
        r.compression = ed::text(in, "InputCompressionType");
        if (r.compression.empty()) {
            r.compression = "NONE";
        }
        if (r.compression != "NONE" && r.compression != "GZIP" && r.compression != "ZSTD") {
            return validation_error("InputCompressionType must be GZIP, ZSTD or NONE");
        }
        if ((r.compression == "GZIP" && !export_formats::has_gzip()) ||
            (r.compression == "ZSTD" && !export_formats::has_zstd())) {
            return validation_error(r.compression + " input is not supported: this build has no " +
                                    (r.compression == "GZIP" ? "zlib" : "libzstd"));
        }
        r.creation = in.at("TableCreationParameters").as_object();
        r.client_token = ed::text(in, "ClientToken");
        const auto now = _backend.now_ms();
        if (!r.client_token.empty()) {
            for (const auto& old : ed::all_imports(*_store)) {
                if (old.client_token != r.client_token || now - old.start_ms > ed::k_token_ms) {
                    continue;
                }
                if (old.bucket == r.bucket && old.key_prefix == r.key_prefix &&
                    old.format == r.format && old.compression == r.compression &&
                    old.csv_delimiter == r.csv_delimiter && old.csv_headers == r.csv_headers &&
                    old.creation == r.creation) {
                    return boost::json::object{{"ImportTableDescription", description(old)}};
                }
                return ed::modeled("ImportConflictException",
                                   "An import with the same client token but different "
                                   "parameters exists: " +
                                       old.arn);
            }
        }
        // The table, checked as CreateTable checks it, with what an import
        // cannot set refused there too.
        boost::json::object create;
        for (const auto* member : {"TableName", "AttributeDefinitions", "KeySchema", "BillingMode",
                                   "ProvisionedThroughput", "OnDemandThroughput",
                                   "SSESpecification", "GlobalSecondaryIndexes"}) {
            if (const auto* v = r.creation.if_contains(member)) {
                create[member] = *v;
            }
        }
        auto d = table_api<Backend>::descriptor_of_create(ctx, create);
        if (!d) {
            return d.error();
        }
        r.table_name = d->name;
        r.table_arn = ctx.table_arn(d->name);
        r.start_ms = now;
        r.id = backup_detail::new_backup_id(now);
        r.arn = r.table_arn + "/import/" + r.id;
        d->restore.source_backup_arn = r.arn;
        d->restore.source_table_arn = r.arn;
        catalog_command c;
        c.op = catalog_command::kind::create_table;
        c.table = std::move(*d);
        c.now_ms = now;
        const auto res = _backend.catalog(c);
        if (!res.ok()) {
            return table_api_detail::error_of(res);
        }
        r.table_id = res.table->table_id;
        _store->put(ed::import_key(r.id), ed::encode_import(r));
        if (_started) {
            _started();
        }
        return boost::json::object{{"ImportTableDescription", description(r)}};
    }

    auto describe_import(call_context& /*ctx*/, boost::json::object& in) -> result {
        const auto arn = export_detail::text(in, "ImportArn");
        const auto r = find_import(arn);
        if (!r) {
            return export_detail::modeled("ImportNotFoundException", "Import not found: " + arn);
        }
        return boost::json::object{{"ImportTableDescription", description(*r)}};
    }

    auto list_imports(call_context& ctx, boost::json::object& in) -> result {
        namespace ed = export_detail;
        boost::json::array out;
        if (!_store) {
            return boost::json::object{{"ImportSummaryList", std::move(out)}};
        }
        const auto table = ed::text(in, "TableArn");
        auto limit = ed::num(in, "PageSize");
        if (in.contains("PageSize") && (limit < 1 || limit > 25)) {
            return validation_error("PageSize must be between 1 and 25");
        }
        if (limit == 0) {
            limit = 25;
        }
        const auto token = ed::text(in, "NextToken");
        const auto start = ed::import_of_page_token(token);
        bool started = token.empty();
        std::string last;
        bool more = false;
        for (const auto& r : ed::all_imports(*_store)) {
            if (!started) {
                started = r.arn == start;
                continue;
            }
            if (!ctx.owns_arn(r.arn) || (!table.empty() && r.table_arn != table)) {
                continue;
            }
            if (out.size() >= limit) {
                more = true;
                break;
            }
            boost::json::object s{
                {"ImportArn", r.arn},
                {"ImportStatus", r.status},
                {"TableArn", r.table_arn},
                {"S3BucketSource", bucket_source(r)},
                {"InputFormat", r.format},
                {"StartTime", table_api_detail::millis_to_seconds(r.start_ms)},
            };
            if (r.end_ms != 0) {
                s["EndTime"] = table_api_detail::millis_to_seconds(r.end_ms);
            }
            out.push_back(std::move(s));
            last = r.arn;
        }
        boost::json::object res{{"ImportSummaryList", std::move(out)}};
        if (more) {
            res["NextToken"] = ed::import_page_token(last);
        }
        return res;
    }

private:
    [[nodiscard]] auto find_export(const std::string& arn) -> std::optional<export_record> {
        const auto id = export_detail::id_of(arn, "export");
        if (!id || !_store) {
            return std::nullopt;
        }
        const auto body = _store->get(export_detail::export_key(*id));
        auto r = body ? export_detail::decode_export(*body) : std::nullopt;
        if (!r || r->arn != arn) {
            return std::nullopt;
        }
        return r;
    }

    [[nodiscard]] auto find_import(const std::string& arn) -> std::optional<import_record> {
        const auto id = export_detail::id_of(arn, "import");
        if (!id || !_store) {
            return std::nullopt;
        }
        const auto body = _store->get(export_detail::import_key(*id));
        auto r = body ? export_detail::decode_import(*body) : std::nullopt;
        if (!r || r->arn != arn) {
            return std::nullopt;
        }
        return r;
    }

    static auto description(const export_record& r) -> boost::json::object {
        using table_api_detail::millis_to_seconds;
        boost::json::object o{
            {"ExportArn", r.arn},
            {"ExportStatus", r.status},
            {"StartTime", millis_to_seconds(r.start_ms)},
            {"TableArn", r.table_arn},
            {"TableId", table_api_detail::table_uuid(r.table_id)},
            {"S3Bucket", r.bucket},
            {"ExportFormat", r.format},
            {"ExportType", r.type},
            {"BilledSizeBytes", r.billed_bytes},
            {"ItemCount", r.item_count},
        };
        if (r.type == "FULL_EXPORT") {
            o["ExportTime"] = millis_to_seconds(r.export_ms);
        } else {
            o["IncrementalExportSpecification"] = boost::json::object{
                {"ExportFromTime", millis_to_seconds(r.from_ms)},
                {"ExportToTime", millis_to_seconds(r.to_ms)},
                {"ExportViewType", r.view},
            };
        }
        if (r.end_ms != 0) {
            o["EndTime"] = millis_to_seconds(r.end_ms);
        }
        if (!r.manifest.empty()) {
            o["ExportManifest"] = r.manifest;
        }
        for (const auto& [k, v] :
             {std::pair<std::string_view, const std::string*>{"ClientToken", &r.client_token},
              {"S3BucketOwner", &r.bucket_owner},
              {"S3Prefix", &r.prefix},
              {"S3SseAlgorithm", &r.sse_algorithm},
              {"S3SseKmsKeyId", &r.sse_kms_key_id},
              {"FailureCode", &r.failure_code},
              {"FailureMessage", &r.failure_message}}) {
            if (!v->empty()) {
                o[k] = *v;
            }
        }
        return o;
    }

    static auto bucket_source(const import_record& r) -> boost::json::object {
        boost::json::object s{{"S3Bucket", r.bucket}};
        if (!r.bucket_owner.empty()) {
            s["S3BucketOwner"] = r.bucket_owner;
        }
        if (!r.key_prefix.empty()) {
            s["S3KeyPrefix"] = r.key_prefix;
        }
        return s;
    }

    static auto description(const import_record& r) -> boost::json::object {
        using table_api_detail::millis_to_seconds;
        boost::json::object o{
            {"ImportArn", r.arn},
            {"ImportStatus", r.status},
            {"TableArn", r.table_arn},
            {"TableId", table_api_detail::table_uuid(r.table_id)},
            {"S3BucketSource", bucket_source(r)},
            {"ErrorCount", r.errors},
            {"InputFormat", r.format},
            {"InputCompressionType", r.compression},
            {"TableCreationParameters", r.creation},
            {"StartTime", millis_to_seconds(r.start_ms)},
            {"ProcessedSizeBytes", r.processed_bytes},
            {"ProcessedItemCount", r.processed_items},
            {"ImportedItemCount", r.imported_items},
        };
        if (r.format == "CSV") {
            boost::json::object csv{{"Delimiter", r.csv_delimiter}};
            if (!r.csv_headers.empty()) {
                boost::json::array h;
                for (const auto& n : r.csv_headers) {
                    h.emplace_back(n);
                }
                csv["HeaderList"] = std::move(h);
            }
            o["InputFormatOptions"] = boost::json::object{{"Csv", std::move(csv)}};
        }
        if (r.end_ms != 0) {
            o["EndTime"] = millis_to_seconds(r.end_ms);
        }
        for (const auto& [k, v] :
             {std::pair<std::string_view, const std::string*>{"ClientToken", &r.client_token},
              {"FailureCode", &r.failure_code},
              {"FailureMessage", &r.failure_message}}) {
            if (!v->empty()) {
                o[k] = *v;
            }
        }
        return o;
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    bucket_resolver _buckets;
    export_import_options _options;
    std::function<void()> _started;
};

// -- The worker ---------------------------------------------------------------

struct export_import_worker_options {
    /// How often the store is listed for jobs when none was started on this
    /// node.
    std::chrono::milliseconds list_interval{10000};
};

/// Runs exports and imports, on the catalog leader.
template<dynamo_backend Backend> class export_import_worker {
public:
    export_import_worker(Backend& backend, std::shared_ptr<backup_store> store,
                         bucket_resolver buckets, export_import_worker_options options = {})
        : _backend(backend),
          _store(std::move(store)),
          _buckets(std::move(buckets)),
          _options(options) {}

    void set_store(std::shared_ptr<backup_store> store) { _store = std::move(store); }
    void set_buckets(bucket_resolver buckets) { _buckets = std::move(buckets); }
    /// Look at the store on the next pass, whatever the interval.
    void poke() { _poked.store(true); }

    /// One pass: every IN_PROGRESS export and import run to its end.
    /// Returns how many ended.
    auto pass() -> std::size_t {
        namespace ed = export_detail;
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
        for (auto& r : ed::all_exports(*_store)) {
            if (r.status == "IN_PROGRESS") {
                run(r);
                ++done;
            } else if (r.end_ms != 0 && now - r.end_ms > ed::k_retention_ms) {
                _store->remove(ed::export_key(r.id));
            }
        }
        for (auto& r : ed::all_imports(*_store)) {
            if (r.status == "IN_PROGRESS") {
                run(r);
                ++done;
            } else if (r.status == "COMPLETED") {
                // A leader that stopped between the record and the catalog
                // leaves the table loading; finish it.
                const auto d = _backend.describe(r.table_name);
                if (d && d->table_id == r.table_id && d->restore.in_progress) {
                    backup_detail::finish_restore(_backend, *d);
                }
            } else if (r.status == "FAILED") {
                // A failed import's table is dropped; a refused delete (the
                // table not yet ACTIVE after its load was ended) is retried.
                drop_table(r);
            }
            if (r.status != "IN_PROGRESS" && r.end_ms != 0 && now - r.end_ms > ed::k_retention_ms) {
                _store->remove(ed::import_key(r.id));
            }
        }
        return done;
    }

private:
    // -- Exports --

    void fail(export_record& r, std::string code, std::string message) {
        r.status = "FAILED";
        r.failure_code = std::move(code);
        r.failure_message = std::move(message);
        r.end_ms = _backend.now_ms();
        _store->put(export_detail::export_key(r.id), export_detail::encode_export(r));
    }

    void run(export_record& r) {
        namespace ed = export_detail;
        const auto d = _backend.describe(r.table_name);
        if (!d || d->table_id != r.table_id) {
            fail(r, "TableNotFound", "Table " + r.table_name + " was deleted during the export");
            return;
        }
        std::shared_ptr<backup_store> bucket;
        if (_buckets && ed::valid_bucket(r.bucket)) {
            bucket = _buckets(r.bucket);
        }
        if (!bucket) {
            fail(r, "S3NoSuchBucket", "S3 bucket " + r.bucket + " is not mapped to a store");
            return;
        }
        const auto root = ed::export_root(r.prefix, r.id);
        const bool ion = r.format == "ION";
        bucket->put(root + "_started", "");
        ed::data_writer out(*bucket, root, ion);
        std::uint64_t billed = 0;
        try {
            if (r.type == "FULL_EXPORT" && r.live) {
                _backend.read_range(table_range(r.table_id), kythira::scan_direction::forward, true,
                                    [&](std::string_view, const item& it) {
                                        out.line(ed::item_line(item_to_json(it), ion));
                                        billed += item_size(it);
                                        return true;
                                    });
            } else if (r.type == "FULL_EXPORT") {
                const auto state = rebuild(*d, r.export_ms, r.export_ms, nullptr);
                for (const auto& [key, it] : state) {
                    out.line(ed::item_line(item_to_json(it), ion));
                    billed += item_size(it);
                }
            } else {
                std::map<std::string, change> changes;
                const auto state = rebuild(*d, r.from_ms, r.to_ms, &changes);
                const bool old_images = r.view == "NEW_AND_OLD_IMAGES";
                for (const auto& [key, ch] : changes) {
                    const auto now_it = state.find(key);
                    std::optional<boost::json::object> old_json;
                    std::optional<boost::json::object> new_json;
                    if (old_images && ch.old_image) {
                        old_json = item_to_json(*ch.old_image);
                    }
                    if (now_it != state.end()) {
                        new_json = item_to_json(now_it->second);
                        billed += item_size(now_it->second);
                    }
                    out.line(ed::change_line(ch.written_ms, ch.keys,
                                             old_json ? &*old_json : nullptr,
                                             new_json ? &*new_json : nullptr, ion));
                }
            }
        } catch (const pitr_rebuild_error& e) {
            fail(r, "PointInTimeRecoveryUnavailable", e.what());
            return;
        }
        out.finish();
        const auto manifest_files = root + "manifest-files.json";
        bucket->put(manifest_files, out.manifest_lines);
        r.end_ms = std::max(_backend.now_ms(), r.start_ms);
        r.item_count = out.items;
        r.billed_bytes = billed;
        boost::json::object summary{
            {"version", "2020-06-30"},
            {"exportArn", r.arn},
            {"startTime", ed::iso8601(r.start_ms)},
            {"endTime", ed::iso8601(r.end_ms)},
            {"tableArn", r.table_arn},
            {"tableId", table_api_detail::table_uuid(r.table_id)},
            {"s3Bucket", r.bucket},
            {"s3Prefix",
             r.prefix.empty() ? boost::json::value(nullptr) : boost::json::value(r.prefix)},
            {"s3SseAlgorithm", r.sse_algorithm.empty() ? "AES256" : r.sse_algorithm},
            {"s3SseKmsKeyId", r.sse_kms_key_id.empty() ? boost::json::value(nullptr)
                                                       : boost::json::value(r.sse_kms_key_id)},
            {"manifestFilesS3Key", manifest_files},
            {"billedSizeBytes", r.billed_bytes},
            {"itemCount", r.item_count},
            {"outputFormat", r.format},
            {"exportType", r.type},
        };
        if (r.type == "FULL_EXPORT") {
            summary["exportTime"] = ed::iso8601(r.export_ms);
        } else {
            summary["exportFromTime"] = ed::iso8601(r.from_ms);
            summary["exportToTime"] = ed::iso8601(r.to_ms);
            summary["outputView"] = r.view;
        }
        const auto summary_text = boost::json::serialize(summary);
        bucket->put(root + "manifest-summary.json", summary_text);
        bucket->put(root + "manifest-summary.md5", export_formats::md5_base64(summary_text));
        r.manifest = root + "manifest-summary.json";
        r.status = "COMPLETED";
        _store->put(ed::export_key(r.id), ed::encode_export(r));
    }

    /// No archive covers the time asked for.
    class pitr_rebuild_error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    /// An item an incremental export reports.
    struct change {
        boost::json::object keys;
        std::uint64_t written_ms = 0;
        /// The item at the period's start.
        std::optional<item> old_image;
    };

    /// The table at `upto_ms`, by base item key, rebuilt from the newest
    /// base that ended by `base_ms` and the records after it. With
    /// `changes`, the records after `base_ms` are the period's: each item
    /// they touch is noted with its image at `base_ms`.
    auto rebuild(const table_descriptor& d, std::uint64_t base_ms, std::uint64_t upto_ms,
                 std::map<std::string, change>* changes) -> std::map<std::string, item> {
        std::map<std::string, item> state;
        const auto parse = [](const boost::json::object& o) {
            auto it = item_from_json(o);
            if (!it) {
                throw std::runtime_error("the archive holds an unreadable item");
            }
            return std::move(*it);
        };
        const auto key_of = [&](const item& it) {
            auto k = base_item_key(d.table_id, d.schema, it);
            if (!k) {
                throw std::runtime_error("the archive holds an item without its key");
            }
            return std::move(*k);
        };
        try {
            pitr_detail::replay(
                *_store, d.table_id, base_ms, upto_ms,
                [&](const boost::json::object& image) {
                    auto it = parse(image);
                    auto k = key_of(it);
                    state[std::move(k)] = std::move(it);
                },
                [&](const boost::json::object& rec) {
                    const auto written = rec.at("WrittenMs").to_number<std::uint64_t>();
                    auto keys = parse(rec.at("Keys").as_object());
                    const auto k = key_of(keys);
                    if (changes != nullptr && written > base_ms) {
                        auto [at, fresh] = changes->try_emplace(k);
                        if (fresh) {
                            at->second.keys = rec.at("Keys").as_object();
                            if (const auto s = state.find(k); s != state.end()) {
                                at->second.old_image = s->second;
                            }
                        }
                        at->second.written_ms = std::max(at->second.written_ms, written);
                    }
                    if (const auto* image = rec.if_contains("NewImage")) {
                        state[k] = parse(image->as_object());
                    } else {
                        state.erase(k);
                    }
                });
        } catch (const std::runtime_error& e) {
            if (std::string_view(e.what()).starts_with("no point-in-time recovery base")) {
                throw pitr_rebuild_error(e.what());
            }
            throw;
        }
        return state;
    }

    // -- Imports --

    void fail(import_record& r, std::string code, std::string message) {
        r.status = "FAILED";
        r.failure_code = std::move(code);
        r.failure_message = std::move(message);
        r.end_ms = _backend.now_ms();
        _store->put(export_detail::import_key(r.id), export_detail::encode_import(r));
        drop_table(r);
    }

    void drop_table(const import_record& r) {
        const auto d = _backend.describe(r.table_name);
        if (!d || d->table_id != r.table_id || d->status == table_status::deleting) {
            return;
        }
        // A CREATING table cannot be deleted: end the load first.
        if (d->restore.in_progress) {
            backup_detail::finish_restore(_backend, *d);
        }
        catalog_command c;
        c.op = catalog_command::kind::delete_table;
        c.table.name = d->name;
        c.now_ms = _backend.now_ms();
        (void)_backend.catalog(c);
    }

    void run(import_record& r) {
        namespace ed = export_detail;
        const auto d = _backend.describe(r.table_name);
        if (!d || d->table_id != r.table_id) {
            fail(r, "TableNotFound", "Table " + r.table_name + " was deleted during the import");
            return;
        }
        if (!d->restore.in_progress) {
            // Loaded by an earlier leader that stopped before the record.
            r.status = "COMPLETED";
            r.end_ms = std::max(_backend.now_ms(), r.start_ms);
            _store->put(ed::import_key(r.id), ed::encode_import(r));
            return;
        }
        std::shared_ptr<backup_store> bucket;
        if (_buckets && ed::valid_bucket(r.bucket)) {
            bucket = _buckets(r.bucket);
        }
        if (!bucket) {
            fail(r, "S3NoSuchBucket", "S3 bucket " + r.bucket + " is not mapped to a store");
            return;
        }
        auto keys = bucket->list(r.key_prefix);
        std::erase_if(keys, [](const std::string& k) { return k.empty() || k.ends_with("/"); });
        std::ranges::sort(keys);
        if (keys.empty()) {
            fail(r, "InvalidInput",
                 "No objects were found under s3://" + r.bucket + "/" + r.key_prefix);
            return;
        }
        r.processed_bytes = 0;
        r.processed_items = 0;
        r.imported_items = 0;
        r.errors = 0;
        // Attributes CSV cells take a declared type for: the table's and
        // its GSIs' keys.
        std::map<std::string, attribute_type> typed;
        typed[d->schema.partition_name] = d->schema.partition_type;
        if (d->schema.sort_name) {
            typed[*d->schema.sort_name] = d->schema.sort_type;
        }
        for (const auto& g : d->gsis) {
            typed[g.schema.partition_name] = g.schema.partition_type;
            if (g.schema.sort_name) {
                typed[*g.schema.sort_name] = g.schema.sort_type;
            }
        }
        const auto gsis = maintained_indexes(d->gsis);
        const auto put = [&](const boost::json::object& image) {
            ++r.processed_items;
            auto it = item_from_json(image);
            if (!it || validate_key_attributes(*it, d->schema) ||
                validate_global_index_keys(*it, gsis) || check_item_size(*it)) {
                ++r.errors;
                return;
            }
            item_command c;
            c.op = item_command::kind::put;
            c.table_id = d->table_id;
            c.schema = d->schema;
            c.payload = std::move(*it);
            c.lsis = d->lsis;
            c.gsis = gsis;
            c.log_changes = d->logs_changes();
            c.stream_ms = d->stream_label();
            c.region = d->region;
            if (_backend.write(c).outcome == item_result::status::ok) {
                ++r.imported_items;
            } else {
                ++r.errors;
            }
        };
        for (const auto& key : keys) {
            const auto raw = bucket->get(key);
            if (!raw) {
                continue;
            }
            r.processed_bytes += raw->size();
            std::string body;
            try {
                body = export_formats::decompress(*raw, r.compression);
            } catch (const export_formats::format_error&) {
                ++r.errors;
                continue;
            }
            ed::read_records(
                body, r.format, r.csv_delimiter[0], r.csv_headers, typed,
                [&](const boost::json::object& record) { put(record.at("Item").as_object()); },
                [&](std::string_view /*why*/, std::string_view /*raw*/, bool one_item) {
                    if (one_item) {
                        ++r.processed_items;
                    }
                    ++r.errors;
                });
            _store->put(ed::import_key(r.id), ed::encode_import(r));
        }
        r.status = "COMPLETED";
        r.end_ms = std::max(_backend.now_ms(), r.start_ms);
        _store->put(ed::import_key(r.id), ed::encode_import(r));
        backup_detail::finish_restore(_backend, *d);
    }

    Backend& _backend;
    std::shared_ptr<backup_store> _store;
    bucket_resolver _buckets;
    export_import_worker_options _options;
    std::atomic<bool> _poked{true};
    std::chrono::steady_clock::time_point _next_list{};
};

}  // namespace kythira::dynamodb
