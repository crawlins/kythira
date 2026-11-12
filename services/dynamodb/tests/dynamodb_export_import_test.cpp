// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Exports and imports (`.kiro/specs/dynamodb-compatible-api/` task 20,
// Requirement 17.3) over local_backend with a change log store: the file
// formats (Ion written and read, CSV, gzip, zstd, MD5), full exports now
// and at a past time, incremental exports with both views, the AWS layout
// and manifests, imports of each format and compression into a new table,
// exports imported back, errors counted, client tokens, listings and the
// refusals.

#define BOOST_TEST_MODULE dynamodb_export_import_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/export_formats.hpp>
#include <kythira/dynamodb/export_import.hpp>
#include <kythira/dynamodb/pitr.hpp>

#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
#include <zstd.h>
#endif

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dynamodb_test;
namespace ef = kythira::dynamodb::export_formats;

namespace {

auto pitr_options() -> ddb::pitr_worker_options {
    ddb::pitr_worker_options o;
    o.latest_lag_ms = 0;
    return o;
}

auto api_options() -> ddb::export_import_options {
    ddb::export_import_options o;
    o.min_incremental_ms = 0;
    return o;
}

struct export_fixture : fixture {
    std::shared_ptr<ddb::memory_backup_store> store = std::make_shared<ddb::memory_backup_store>();
    ddb::memory_buckets buckets;
    ddb::pitr_api<ddb::local_backend<>> pitr{backend, store};
    ddb::pitr_worker<ddb::local_backend<>> archiver{backend, store, pitr_options()};
    /// Every bucket but "unmapped".
    ddb::bucket_resolver resolver = [this](const std::string& name) {
        return name == "unmapped" ? std::shared_ptr<ddb::backup_store>{}
                                  : std::shared_ptr<ddb::backup_store>(buckets.store(name));
    };
    ddb::export_import_api<ddb::local_backend<>> jobs{backend, store, resolver, api_options()};
    ddb::export_import_worker<ddb::local_backend<>> worker{backend, store, resolver};

    export_fixture() {
        without_throttling();
        pitr.register_with(*fe);
        jobs.register_with(*fe);
        jobs.on_started([this] { worker.poke(); });
    }

    void enable(const std::string& table) {
        (void)ok(
            "UpdateContinuousBackups",
            R"({"TableName":")" + table +
                R"(","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":true}})");
    }
    void put(const std::string& table, const std::string& pk, const std::string& v) {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":"1"},"v":{"S":")" + v + R"("}}})");
    }
    void remove(const std::string& table, const std::string& pk) {
        (void)ok("DeleteItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":"1"}}})");
    }
    /// pk -> v of every item, read consistently.
    auto contents(const std::string& table) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        const auto r = ok("Scan", R"({"TableName":")" + table + R"(","ConsistentRead":true})");
        for (const auto& i : r.at("Items").as_array()) {
            const auto& o = i.as_object();
            const auto* v = o.if_contains("v");
            out[std::string(o.at("pk").as_object().at("S").as_string())] =
                v != nullptr ? std::string(v->as_object().at("S").as_string()) : "";
        }
        return out;
    }
    auto status(const std::string& table) -> std::string {
        return std::string(ok("DescribeTable", R"({"TableName":")" + table + R"("})")
                               .at("Table")
                               .as_object()
                               .at("TableStatus")
                               .as_string());
    }
    [[nodiscard]] static auto arn(const std::string& table) -> std::string {
        return "arn:aws:dynamodb:us-east-1:" + std::string(k_account) + ":table/" + table;
    }
    static auto seconds(std::uint64_t ms) -> std::string {
        return std::to_string(static_cast<double>(ms) / 1000.0);
    }
    /// Starts an export and runs it; returns its description.
    auto exported(const std::string& body) -> json::object {
        const auto started = ok("ExportTableToPointInTime", body);
        const auto& d = started.at("ExportDescription").as_object();
        BOOST_TEST(d.at("ExportStatus").as_string() == "IN_PROGRESS");
        (void)worker.pass();
        return ok("DescribeExport",
                  R"({"ExportArn":")" + std::string(d.at("ExportArn").as_string()) + R"("})")
            .at("ExportDescription")
            .as_object();
    }
    /// Starts an import and runs it; returns its description.
    auto imported(const std::string& body) -> json::object {
        const auto r = call("ImportTable", body);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
        const auto& d = r.body.at("ImportTableDescription").as_object();
        BOOST_TEST(d.at("ImportStatus").as_string() == "IN_PROGRESS");
        (void)worker.pass();
        return ok("DescribeImport",
                  R"({"ImportArn":")" + std::string(d.at("ImportArn").as_string()) + R"("})")
            .at("ImportTableDescription")
            .as_object();
    }
    /// The data lines of a finished export, after checking its manifests.
    auto export_lines(const json::object& d) -> std::vector<std::string> {
        auto b = buckets.store(std::string(d.at("S3Bucket").as_string()));
        const std::string summary_key(d.at("ExportManifest").as_string());
        const auto summary_text = b->get(summary_key);
        BOOST_TEST_REQUIRE(summary_text.has_value());
        const auto root = summary_key.substr(0, summary_key.size() - 21);
        BOOST_TEST(b->get(root + "_started").has_value());
        BOOST_TEST(*b->get(root + "manifest-summary.md5") == ef::md5_base64(*summary_text));
        const auto summary = json::parse(*summary_text).as_object();
        BOOST_TEST(summary.at("version").as_string() == "2020-06-30");
        BOOST_TEST(summary.at("exportArn") == d.at("ExportArn"));
        BOOST_TEST(summary.at("itemCount") == d.at("ItemCount"));
        BOOST_TEST(summary.at("outputFormat") == d.at("ExportFormat"));
        const auto files = b->get(std::string(summary.at("manifestFilesS3Key").as_string()));
        BOOST_TEST_REQUIRE(files.has_value());
        std::vector<std::string> lines;
        std::uint64_t count = 0;
        std::string_view rest(*files);
        while (!rest.empty()) {
            const auto nl = rest.find('\n');
            const auto entry = json::parse(rest.substr(0, nl)).as_object();
            rest = rest.substr(nl + 1);
            const auto body = b->get(std::string(entry.at("dataFileS3Key").as_string()));
            BOOST_TEST_REQUIRE(body.has_value());
            BOOST_TEST(entry.at("md5").as_string() == ef::md5_base64(*body));
            BOOST_TEST(entry.at("etag").as_string() == ef::md5_hex(*body));
            const auto text = ef::decompress(*body, ef::has_gzip() ? "GZIP" : "NONE");
            std::string_view t(text);
            std::uint64_t in_file = 0;
            while (!t.empty()) {
                const auto n = t.find('\n');
                const auto line = t.substr(0, n);
                t = n == std::string_view::npos ? std::string_view{} : t.substr(n + 1);
                if (line == ef::k_ion_version_marker) {
                    continue;
                }
                lines.emplace_back(line);
                ++in_file;
            }
            BOOST_TEST(entry.at("itemCount").to_number<std::uint64_t>() == in_file);
            count += in_file;
        }
        BOOST_TEST(count == d.at("ItemCount").to_number<std::uint64_t>());
        return lines;
    }
    /// The S3KeyPrefix of an export's data files.
    static auto data_prefix(const json::object& d) -> std::string {
        const std::string summary_key(d.at("ExportManifest").as_string());
        return summary_key.substr(0, summary_key.size() - 21) + "data/";
    }
    static auto import_body(const std::string& table, const std::string& bucket,
                            const std::string& prefix, const std::string& format,
                            const std::string& extra = "") -> std::string {
        return R"({"S3BucketSource":{"S3Bucket":")" + bucket + R"(","S3KeyPrefix":")" + prefix +
               R"("},"InputFormat":")" + format + R"(",)" + extra +
               R"("TableCreationParameters":{"TableName":")" + table +
               R"(","AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
               R"({"AttributeName":"sk","AttributeType":"N"}],)"
               R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
               R"({"AttributeName":"sk","KeyType":"RANGE"}],"BillingMode":"PAY_PER_REQUEST"}})";
    }
};

}  // namespace

// -- Formats ------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ion_round_trips_every_type) {
    const auto item = json::parse(R"({
        "s":{"S":"a \"quoted\"\nline\u0001 caf\u00e9"},
        "n":{"N":"123"}, "f":{"N":"-1.5"}, "e":{"N":"1.5E+30"}, "small":{"N":"2E-7"},
        "b":{"B":"AAEC"}, "t":{"BOOL":true}, "z":{"NULL":true},
        "ss":{"SS":["x","y"]}, "ns":{"NS":["1","2.5"]}, "bs":{"BS":["AA==","AQ=="]},
        "l":{"L":[{"S":"in"},{"N":"7"},{"L":[]},{"M":{}}]},
        "m":{"M":{"inner key":{"S":"v"},"null":{"N":"0"},"x":{"M":{"y":{"BOOL":false}}}}},
        "with space":{"S":""}, "$odd":{"S":"d"}
    })")
                          .as_object();
    std::string text = "$ion_1_0 {Item:";
    ef::write_ion_item(text, item);
    text += "}";
    BOOST_TEST(text.find("n:123.") != std::string::npos);
    BOOST_TEST(text.find("e:1.5d30") != std::string::npos);
    BOOST_TEST(text.find("ss:$dynamodb_SS::[\"x\",\"y\"]") != std::string::npos);
    BOOST_TEST(text.find("b:{{AAEC}}") != std::string::npos);
    BOOST_TEST(text.find("'with space':") != std::string::npos);
    BOOST_TEST(text.find("'null':") != std::string::npos);
    ef::ion_reader reader(text);
    const auto v = reader.next();
    BOOST_TEST_REQUIRE(v.has_value());
    BOOST_TEST(!reader.next().has_value());
    const auto& back = v->as_object().at("M").as_object().at("Item").as_object().at("M");
    // Numbers come back in DynamoDB's spelling, which item_from_json reads
    // to the same values.
    const auto a = ddb::item_from_json(item);
    const auto b = ddb::item_from_json(back.as_object());
    BOOST_TEST_REQUIRE(a.has_value());
    BOOST_TEST_REQUIRE(b.has_value());
    BOOST_TEST((*a == *b));
}

BOOST_AUTO_TEST_CASE(ion_reader_takes_what_writers_write_and_refuses_the_rest) {
    const auto one = [](std::string_view text) {
        ef::ion_reader r(text);
        auto v = r.next();
        BOOST_TEST_REQUIRE(v.has_value());
        return *v;
    };
    BOOST_TEST(one("0x1F").as_object().at("N").as_string() == "31");
    BOOST_TEST(one("-0b101").as_object().at("N").as_string() == "-5");
    BOOST_TEST(one("1_000").as_object().at("N").as_string() == "1000");
    BOOST_TEST(one("12.50").as_object().at("N").as_string() == "12.50");
    BOOST_TEST(one("2E3").as_object().at("N").as_string() == "2E3");
    BOOST_TEST(one("4.5d-2").as_object().at("N").as_string() == "4.5E-2");
    BOOST_TEST(one("'''ab''' /* c */ '''cd'''").as_object().at("S").as_string() == "abcd");
    BOOST_TEST(one("sym").as_object().at("S").as_string() == "sym");
    BOOST_TEST(one("\"\\u00e9\\x41\"").as_object().at("S").as_string() ==
               "\xc3\xa9"
               "A");
    BOOST_TEST(one("{{\"clob\"}}").as_object().at("B").as_string() == "Y2xvYg==");
    BOOST_TEST(one("null.null").as_object().at("NULL").as_bool());
    BOOST_TEST(one("ann::{a:1, // comment\n b:[true]}").as_object().at("M").as_object().size() ==
               2U);
    for (const auto* bad : {"2026-11-12T00:00Z", "(a b)", "nan", "+inf", "null.string", "{a 1}",
                            "\"open", "[1,", "{{not base64!}}", "$dynamodb_SS::[1]"}) {
        ef::ion_reader r(bad);
        BOOST_CHECK_THROW((void)r.next(), ef::format_error);
    }
}

BOOST_AUTO_TEST_CASE(csv_reader_follows_rfc_4180) {
    std::vector<std::vector<std::string>> rows;
    ef::read_csv("a,b,c\r\n\"x,1\",\"say \"\"hi\"\"\",\n\n\"multi\nline\",,z", ',',
                 [&](const std::vector<std::string>& f) {
                     rows.push_back(f);
                     return true;
                 });
    BOOST_TEST_REQUIRE(rows.size() == 3U);
    BOOST_TEST((rows[0] == std::vector<std::string>{"a", "b", "c"}));
    BOOST_TEST((rows[1] == std::vector<std::string>{"x,1", "say \"hi\"", ""}));
    BOOST_TEST((rows[2] == std::vector<std::string>{"multi\nline", "", "z"}));
    rows.clear();
    ef::read_csv("p|q\n1|2", '|', [&](const std::vector<std::string>& f) {
        rows.push_back(f);
        return true;
    });
    BOOST_TEST((rows[1] == std::vector<std::string>{"1", "2"}));
    BOOST_CHECK_THROW(ef::read_csv("\"open", ',', [](const auto&) { return true; }),
                      ef::format_error);
}

BOOST_AUTO_TEST_CASE(digests_and_compression) {
    BOOST_TEST(ef::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    BOOST_TEST(ef::md5_base64("") == "1B2M2Y8AsgTpgAmY7PhCfg==");
    BOOST_TEST(ef::decompress("plain", "NONE") == "plain");
    if (ef::has_gzip()) {
        const std::string text(100000, 'k');
        const auto z = ef::gzip(text);
        BOOST_TEST(z.size() < text.size());
        BOOST_TEST(ef::gunzip(z) == text);
        // Concatenated members are one file.
        BOOST_TEST(ef::gunzip(ef::gzip("ab") + ef::gzip("cd")) == "abcd");
        BOOST_CHECK_THROW((void)ef::gunzip("not gzip"), ef::format_error);
        BOOST_CHECK_THROW((void)ef::gunzip(z.substr(0, z.size() / 2)), ef::format_error);
    }
#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
    const std::string text = "zstd text zstd text zstd text";
    std::string z(ZSTD_compressBound(text.size()), '\0');
    z.resize(ZSTD_compress(z.data(), z.size(), text.data(), text.size(), 3));
    BOOST_TEST(ef::unzstd(z) == text);
    BOOST_CHECK_THROW((void)ef::unzstd("not zstd"), ef::format_error);
#endif
}

// -- Exports ------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(a_full_export_writes_the_aws_layout, export_fixture) {
    (void)create("tbl");
    enable("tbl");
    (void)archiver.pass();
    put("tbl", "a", "1");
    put("tbl", "b", "2");
    const auto d = exported(R"({"TableArn":")" + arn("tbl") +
                            R"(","S3Bucket":"exports-bkt","S3Prefix":"daily","ClientToken":"t1"})");
    BOOST_TEST(d.at("ExportStatus").as_string() == "COMPLETED");
    BOOST_TEST(d.at("ExportType").as_string() == "FULL_EXPORT");
    BOOST_TEST(d.at("ExportFormat").as_string() == "DYNAMODB_JSON");
    BOOST_TEST(d.at("ItemCount").to_number<int>() == 2);
    BOOST_TEST(d.at("S3Prefix").as_string() == "daily");
    BOOST_TEST(d.at("ClientToken").as_string() == "t1");
    BOOST_TEST(d.at("ExportTime").to_number<double>() == d.at("StartTime").to_number<double>());
    const std::string export_arn(d.at("ExportArn").as_string());
    BOOST_TEST(export_arn.starts_with(arn("tbl") + "/export/"));
    const auto id = export_arn.substr(export_arn.rfind('/') + 1);
    BOOST_TEST(d.at("ExportManifest").as_string() ==
               "daily/AWSDynamoDB/" + id + "/manifest-summary.json");
    std::map<std::string, std::string> seen;
    for (const auto& line : export_lines(d)) {
        const auto o = json::parse(line).as_object();
        const auto& it = o.at("Item").as_object();
        seen[std::string(it.at("pk").as_object().at("S").as_string())] =
            std::string(it.at("v").as_object().at("S").as_string());
    }
    BOOST_TEST((seen == std::map<std::string, std::string>{{"a", "1"}, {"b", "2"}}));
    if (ef::has_gzip()) {
        BOOST_TEST(buckets.store("exports-bkt")
                       ->get("daily/AWSDynamoDB/" + id + "/data/000000.json.gz")
                       .has_value());
    }

    // The same token and request is the same export; another request under
    // it is a conflict.
    const auto again =
        ok("ExportTableToPointInTime",
           R"({"TableArn":")" + arn("tbl") +
               R"(","S3Bucket":"exports-bkt","S3Prefix":"daily","ClientToken":"t1"})");
    BOOST_TEST(again.at("ExportDescription").as_object().at("ExportArn").as_string() == export_arn);
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") +
                        R"(","S3Bucket":"exports-bkt","S3Prefix":"other","ClientToken":"t1"})")
                   .type() == "ExportConflictException");
}

BOOST_FIXTURE_TEST_CASE(an_export_at_a_past_time_rebuilds_the_table, export_fixture) {
    (void)create("tbl");
    put("tbl", "old", "0");
    enable("tbl");
    (void)archiver.pass();
    const auto t0 = clock_ms;
    clock_ms += 1000;
    put("tbl", "a", "1");
    remove("tbl", "old");
    const auto t1 = clock_ms;
    clock_ms += 1000;
    put("tbl", "a", "2");
    put("tbl", "b", "2");
    (void)archiver.pass();
    const auto collect = [&](const json::object& d) {
        std::map<std::string, std::string> seen;
        for (const auto& line : export_lines(d)) {
            const auto it = json::parse(line).as_object().at("Item").as_object();
            seen[std::string(it.at("pk").as_object().at("S").as_string())] =
                std::string(it.at("v").as_object().at("S").as_string());
        }
        return seen;
    };
    using items = std::map<std::string, std::string>;
    const auto at0 = exported(R"({"TableArn":")" + arn("tbl") +
                              R"(","S3Bucket":"bkt","ExportTime":)" + seconds(t0) + "}");
    BOOST_TEST(at0.at("ExportStatus").as_string() == "COMPLETED");
    BOOST_TEST((collect(at0) == items{{"old", "0"}}));
    const auto at1 = exported(R"({"TableArn":")" + arn("tbl") +
                              R"(","S3Bucket":"bkt","ExportTime":)" + seconds(t1) + "}");
    BOOST_TEST((collect(at1) == items{{"a", "1"}}));
    BOOST_TEST(at1.at("ExportTime").to_number<double>() == static_cast<double>(t1) / 1000.0);

    // Outside the restorable window.
    BOOST_TEST(call("ExportTableToPointInTime", R"({"TableArn":")" + arn("tbl") +
                                                    R"(","S3Bucket":"bkt","ExportTime":)" +
                                                    seconds(clock_ms + 60000) + "}")
                   .type() == "InvalidExportTimeException");
    BOOST_TEST(call("ExportTableToPointInTime", R"({"TableArn":")" + arn("tbl") +
                                                    R"(","S3Bucket":"bkt","ExportTime":)" +
                                                    seconds(t0 - 60000) + "}")
                   .type() == "InvalidExportTimeException");
}

BOOST_FIXTURE_TEST_CASE(an_incremental_export_reports_each_changed_item, export_fixture) {
    (void)create("tbl");
    put("tbl", "keep", "0");
    put("tbl", "gone", "0");
    put("tbl", "edit", "0");
    enable("tbl");
    (void)archiver.pass();
    const auto from = clock_ms;
    clock_ms += 1000;
    put("tbl", "edit", "1");
    put("tbl", "new", "1");
    remove("tbl", "gone");
    clock_ms += 1000;
    put("tbl", "edit", "2");
    const auto last_write = clock_ms;
    clock_ms += 1000;
    const auto to = clock_ms;
    (void)archiver.pass();
    for (const auto* view : {"NEW_AND_OLD_IMAGES", "NEW_IMAGE"}) {
        const auto d = exported(R"({"TableArn":")" + arn("tbl") +
                                R"(","S3Bucket":"bkt","ExportType":"INCREMENTAL_EXPORT",)"
                                R"("IncrementalExportSpecification":{"ExportFromTime":)" +
                                seconds(from) + R"(,"ExportToTime":)" + seconds(to) +
                                R"(,"ExportViewType":")" + view + R"("}})");
        BOOST_TEST(d.at("ExportStatus").as_string() == "COMPLETED");
        BOOST_TEST(d.at("ExportType").as_string() == "INCREMENTAL_EXPORT");
        BOOST_TEST(d.at("IncrementalExportSpecification").as_object().at("ExportViewType") ==
                   json::value(view));
        BOOST_TEST(!d.contains("ExportTime"));
        std::map<std::string, json::object> by_pk;
        for (const auto& line : export_lines(d)) {
            auto o = json::parse(line).as_object();
            by_pk[std::string(o.at("Keys").as_object().at("pk").as_object().at("S").as_string())] =
                o;
        }
        BOOST_TEST_REQUIRE(by_pk.size() == 3U);  // edit, new, gone; not keep
        const bool old = std::string_view(view) == "NEW_AND_OLD_IMAGES";
        const auto& edit = by_pk.at("edit");
        BOOST_TEST(edit.at("Metadata").as_object().at("WriteTimestampMicros").as_object().at("N") ==
                   json::value(std::to_string(last_write * 1000)));
        BOOST_TEST(edit.at("NewImage").as_object().at("v").as_object().at("S").as_string() == "2");
        BOOST_TEST(edit.contains("OldImage") == old);
        if (old) {
            BOOST_TEST(edit.at("OldImage").as_object().at("v").as_object().at("S").as_string() ==
                       "0");
            BOOST_TEST(by_pk.at("gone").contains("OldImage"));
        }
        BOOST_TEST(!by_pk.at("new").contains("OldImage"));
        BOOST_TEST(!by_pk.at("gone").contains("NewImage"));
    }
    // Period checks.
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") +
                        R"(","S3Bucket":"bkt","ExportType":"INCREMENTAL_EXPORT",)"
                        R"("IncrementalExportSpecification":{"ExportFromTime":)" +
                        seconds(to) + R"(,"ExportToTime":)" + seconds(from) + "}}")
                   .type() == "ValidationException");
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") +
                        R"(","S3Bucket":"bkt","ExportType":"INCREMENTAL_EXPORT",)"
                        R"("ExportTime":)" +
                        seconds(to) + "}")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(an_ion_export_imports_back, export_fixture) {
    (void)create("tbl");
    enable("tbl");
    (void)archiver.pass();
    (void)ok("PutItem", R"({"TableName":"tbl","Item":{"pk":{"S":"rich"},"sk":{"N":"1"},)"
                        R"("v":{"S":"x"},"n":{"N":"-12.5E3"},"b":{"B":"AAEC"},)"
                        R"("ss":{"SS":["p","q"]},"m":{"M":{"odd key":{"L":[{"NULL":true}]}}}}})");
    for (int i = 0; i < 20; ++i) {
        put("tbl", "p" + std::to_string(i), std::to_string(i));
    }
    for (const auto* format : {"ION", "DYNAMODB_JSON"}) {
        const auto d = exported(R"({"TableArn":")" + arn("tbl") +
                                R"(","S3Bucket":"bkt","ExportFormat":")" + format + R"("})");
        BOOST_TEST(d.at("ExportStatus").as_string() == "COMPLETED");
        if (std::string_view(format) == "ION") {
            for (const auto& line : export_lines(d)) {
                BOOST_TEST(line.starts_with("{Item:{"));
            }
        }
        const std::string copy = std::string("copy_") + format;
        const auto imp =
            imported(import_body(copy, "bkt", data_prefix(d), format,
                                 ef::has_gzip() ? R"("InputCompressionType":"GZIP",)" : ""));
        BOOST_TEST(imp.at("ImportStatus").as_string() == "COMPLETED");
        BOOST_TEST(imp.at("ImportedItemCount").to_number<int>() == 21);
        BOOST_TEST(imp.at("ErrorCount").to_number<int>() == 0);
        BOOST_TEST((contents(copy) == contents("tbl")));
        const auto rich = ok("GetItem", R"({"TableName":")" + copy +
                                            R"(","Key":{"pk":{"S":"rich"},"sk":{"N":"1"}}})");
        const auto original =
            ok("GetItem", R"({"TableName":"tbl","Key":{"pk":{"S":"rich"},"sk":{"N":"1"}}})");
        BOOST_TEST(rich.at("Item") == original.at("Item"));
    }
}

BOOST_FIXTURE_TEST_CASE(export_refusals, export_fixture) {
    (void)create("tbl");
    const auto start = [&](const std::string& extra) {
        return call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") + R"(","S3Bucket":"bkt")" + extra + "}");
    };
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("nope") + R"(","S3Bucket":"bkt"})")
                   .type() == "TableNotFoundException");
    BOOST_TEST(start("").type() == "PointInTimeRecoveryUnavailableException");
    enable("tbl");
    (void)archiver.pass();
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") + R"(","S3Bucket":"Bad_Bucket"})")
                   .type() == "ValidationException");
    BOOST_TEST(start(R"(,"S3SseAlgorithm":"ROT13")").type() == "ValidationException");
    BOOST_TEST(call("DescribeExport",
                    R"({"ExportArn":")" + arn("tbl") + R"(/export/01794300000123-0000abcd"})")
                   .type() == "ExportNotFoundException");

    // A bucket the cluster does not map.
    BOOST_TEST(call("ExportTableToPointInTime",
                    R"({"TableArn":")" + arn("tbl") + R"(","S3Bucket":"unmapped"})")
                   .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(exports_list_newest_last_by_table, export_fixture) {
    (void)create("one");
    (void)create("two");
    enable("one");
    enable("two");
    (void)archiver.pass();
    for (int i = 0; i < 3; ++i) {
        clock_ms += 10;
        (void)exported(R"({"TableArn":")" + arn("one") + R"(","S3Bucket":"bkt"})");
    }
    clock_ms += 10;
    (void)exported(R"({"TableArn":")" + arn("two") + R"(","S3Bucket":"bkt"})");
    BOOST_TEST(ok("ListExports", "{}").at("ExportSummaries").as_array().size() == 4U);
    const auto page = ok("ListExports", R"({"TableArn":")" + arn("one") + R"(","MaxResults":2})");
    const auto& s = page.at("ExportSummaries").as_array();
    BOOST_TEST_REQUIRE(s.size() == 2U);
    BOOST_TEST(s[0].as_object().at("ExportStatus").as_string() == "COMPLETED");
    BOOST_TEST(s[0].as_object().at("ExportType").as_string() == "FULL_EXPORT");
    const auto rest =
        ok("ListExports", R"({"TableArn":")" + arn("one") + R"(","MaxResults":2,"NextToken":")" +
                              std::string(page.at("NextToken").as_string()) + R"("})");
    BOOST_TEST(rest.at("ExportSummaries").as_array().size() == 1U);
    BOOST_TEST(!rest.contains("NextToken"));
    BOOST_TEST(call("ListExports", R"({"MaxResults":26})").type() == "ValidationException");
}

// -- Imports ------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(a_csv_import_types_keys_and_counts_errors, export_fixture) {
    auto b = buckets.store("csv");
    b->put("in/one.csv",
           "pk,sk,v,gk,extra\r\na,1,x,10,\n\"b,2\",2,\"say \"\"hi\"\"\",11,e\n"
           ",3,no key,12,\nc,notanumber,bad,13,\nd,4,too,14,many,cells\n");
    b->put("in/two.csv", "pk,sk,v,gk,extra\ne,5,y,15,\n");
    const auto d = imported(
        R"({"S3BucketSource":{"S3Bucket":"csv","S3KeyPrefix":"in/"},"InputFormat":"CSV",)"
        R"("TableCreationParameters":{"TableName":"from_csv",)"
        R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"},)"
        R"({"AttributeName":"sk","AttributeType":"N"},{"AttributeName":"gk","AttributeType":"N"}],)"
        R"("KeySchema":[{"AttributeName":"pk","KeyType":"HASH"},)"
        R"({"AttributeName":"sk","KeyType":"RANGE"}],"BillingMode":"PAY_PER_REQUEST",)"
        R"("GlobalSecondaryIndexes":[{"IndexName":"by_gk","KeySchema":[{"AttributeName":"gk",)"
        R"("KeyType":"HASH"}],"Projection":{"ProjectionType":"ALL"}}]}})");
    BOOST_TEST(d.at("ImportStatus").as_string() == "COMPLETED");
    BOOST_TEST(d.at("ProcessedItemCount").to_number<int>() == 6);
    BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 3);
    BOOST_TEST(d.at("ErrorCount").to_number<int>() == 3);
    BOOST_TEST(d.at("InputFormatOptions").as_object().at("Csv").as_object().at("Delimiter") ==
               json::value(","));
    BOOST_TEST(d.contains("EndTime"));
    BOOST_TEST(status("from_csv") == "ACTIVE");
    const auto item = ok("GetItem", R"({"TableName":"from_csv","Key":{"pk":{"S":"b,2"},)"
                                    R"("sk":{"N":"2"}}})")
                          .at("Item")
                          .as_object();
    BOOST_TEST(item.at("v").as_object().at("S").as_string() == "say \"hi\"");
    BOOST_TEST(item.at("gk").as_object().at("N").as_string() == "11");
    BOOST_TEST(item.at("extra").as_object().at("S").as_string() == "e");
    // Empty cells are left out.
    const auto a = ok("GetItem", R"({"TableName":"from_csv","Key":{"pk":{"S":"a"},)"
                                 R"("sk":{"N":"1"}}})")
                       .at("Item")
                       .as_object();
    BOOST_TEST(!a.contains("extra"));
    // The GSI was built from the imported items.
    const auto q = ok("Query", R"({"TableName":"from_csv","IndexName":"by_gk",)"
                               R"("KeyConditionExpression":"gk = :g",)"
                               R"("ExpressionAttributeValues":{":g":{"N":"15"}}})");
    BOOST_TEST(q.at("Count").to_number<int>() == 1);
}

BOOST_FIXTURE_TEST_CASE(a_csv_import_takes_a_header_list_and_delimiter, export_fixture) {
    buckets.store("csv")->put("h/data", "a|1|x\nb|2|y\n");
    const auto d = imported(import_body(
        "headed", "csv", "h/", "CSV",
        R"("InputFormatOptions":{"Csv":{"Delimiter":"|","HeaderList":["pk","sk","v"]}},)"));
    BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 2);
    BOOST_TEST((contents("headed") == std::map<std::string, std::string>{{"a", "x"}, {"b", "y"}}));
    BOOST_TEST(
        call("ImportTable", import_body("t2", "csv", "h/", "CSV",
                                        R"("InputFormatOptions":{"Csv":{"Delimiter":"x"}},)"))
            .type() == "ValidationException");
    BOOST_TEST(
        call("ImportTable", import_body("t3", "csv", "h/", "ION",
                                        R"("InputFormatOptions":{"Csv":{"Delimiter":","}},)"))
            .type() == "ValidationException");
}

BOOST_FIXTURE_TEST_CASE(json_and_ion_imports_count_unreadable_lines, export_fixture) {
    auto b = buckets.store("src");
    b->put("j/1.json", R"({"Item":{"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"1"}}})"
                       "\n\n"
                       R"({"NotItem":{}})"
                       "\nnot json\n"
                       R"({"Item":{"pk":{"N":"5"},"sk":{"N":"1"}}})"
                       "\n"
                       R"({"Item":{"pk":{"S":"b"},"sk":{"N":"2"},"v":{"S":"2"}}})");
    auto d = imported(import_body("from_json", "src", "j/", "DYNAMODB_JSON"));
    BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 2);
    BOOST_TEST(d.at("ErrorCount").to_number<int>() == 3);
    BOOST_TEST(d.at("ProcessedItemCount").to_number<int>() == 5);

    b->put("i/1.ion",
           "$ion_1_0\n{Item:{pk:\"a\",sk:1.,v:\"1\"}}\n{Other:1}\n"
           "{Item:{pk:\"b\",sk:2,v:'''2'''}}\n{Item:{pk:\"c\" oops");
    d = imported(import_body("from_ion", "src", "i/", "ION"));
    BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 2);
    BOOST_TEST(d.at("ErrorCount").to_number<int>() == 2);  // {Other:1}, the broken tail
    BOOST_TEST(
        (contents("from_ion") == std::map<std::string, std::string>{{"a", "1"}, {"b", "2"}}));
}

BOOST_FIXTURE_TEST_CASE(compressed_imports, export_fixture) {
    const std::string lines = R"({"Item":{"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"z"}}})"
                              "\n";
    if (ef::has_gzip()) {
        buckets.store("src")->put("g/1.json.gz", ef::gzip(lines));
        const auto d = imported(import_body("imp_gz", "src", "g/", "DYNAMODB_JSON",
                                            R"("InputCompressionType":"GZIP",)"));
        BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 1);
        BOOST_TEST(d.at("InputCompressionType").as_string() == "GZIP");
        // Uncompressed input under GZIP is an error for that object.
        buckets.store("src")->put("g2/1.json", lines);
        const auto e = imported(import_body("imp_gz2", "src", "g2/", "DYNAMODB_JSON",
                                            R"("InputCompressionType":"GZIP",)"));
        BOOST_TEST(e.at("ErrorCount").to_number<int>() == 1);
        BOOST_TEST(e.at("ImportStatus").as_string() == "COMPLETED");
    }
#if defined(KYTHIRA_DYNAMODB_HAS_ZSTD)
    std::string z(ZSTD_compressBound(lines.size()), '\0');
    z.resize(ZSTD_compress(z.data(), z.size(), lines.data(), lines.size(), 3));
    buckets.store("src")->put("z/1.json.zst", z);
    const auto d = imported(
        import_body("imp_zs", "src", "z/", "DYNAMODB_JSON", R"("InputCompressionType":"ZSTD",)"));
    BOOST_TEST(d.at("ImportedItemCount").to_number<int>() == 1);
#endif
}

BOOST_FIXTURE_TEST_CASE(an_import_table_loads_before_it_takes_writes, export_fixture) {
    buckets.store("src")->put("p/1", R"({"Item":{"pk":{"S":"a"},"sk":{"N":"1"},"v":{"S":"1"}}})");
    const auto r = call(
        "ImportTable", import_body("imp", "src", "p/", "DYNAMODB_JSON", R"("ClientToken":"tok",)"));
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& d = r.body.at("ImportTableDescription").as_object();
    BOOST_TEST(d.at("TableArn").as_string() == arn("imp"));
    BOOST_TEST(std::string(d.at("ImportArn").as_string()).starts_with(arn("imp") + "/import/"));
    BOOST_TEST(d.at("TableCreationParameters").as_object().at("TableName").as_string() == "imp");
    const auto t = ok("DescribeTable", R"({"TableName":"imp"})").at("Table").as_object();
    BOOST_TEST(t.at("TableStatus").as_string() == "CREATING");
    BOOST_TEST(!t.contains("RestoreSummary"));
    BOOST_TEST(
        call("PutItem", R"({"TableName":"imp","Item":{"pk":{"S":"x"},"sk":{"N":"1"}}})").type() ==
        "ResourceNotFoundException");
    // The same token and request is the same import, another is a conflict,
    // and the name is taken.
    const auto again = ok(
        "ImportTable", import_body("imp", "src", "p/", "DYNAMODB_JSON", R"("ClientToken":"tok",)"));
    BOOST_TEST(again.at("ImportTableDescription").as_object().at("ImportArn") == d.at("ImportArn"));
    BOOST_TEST(call("ImportTable",
                    import_body("imp", "src", "q/", "DYNAMODB_JSON", R"("ClientToken":"tok",)"))
                   .type() == "ImportConflictException");
    BOOST_TEST(call("ImportTable", import_body("imp", "src", "q/", "DYNAMODB_JSON")).type() ==
               "ResourceInUseException");
    (void)worker.pass();
    BOOST_TEST(status("imp") == "ACTIVE");
    const auto after = ok("DescribeTable", R"({"TableName":"imp"})").at("Table").as_object();
    BOOST_TEST(!after.contains("RestoreSummary"));
    put("imp", "x", "9");
    BOOST_TEST((contents("imp") == std::map<std::string, std::string>{{"a", "1"}, {"x", "9"}}));
}

BOOST_FIXTURE_TEST_CASE(an_import_with_nothing_to_read_fails_and_drops_its_table, export_fixture) {
    const auto d = imported(import_body("empty", "src", "nothing/", "DYNAMODB_JSON"));
    BOOST_TEST(d.at("ImportStatus").as_string() == "FAILED");
    BOOST_TEST(d.at("FailureCode").as_string() == "InvalidInput");
    // Deleted, or on its way.
    const auto t = call("DescribeTable", R"({"TableName":"empty"})");
    BOOST_TEST((t.type() == "ResourceNotFoundException" ||
                t.body.at("Table").as_object().at("TableStatus").as_string() == "DELETING"));
}

BOOST_FIXTURE_TEST_CASE(import_refusals_and_listings, export_fixture) {
    BOOST_TEST(call("ImportTable", import_body("t", "Bad_Bucket", "p/", "CSV")).type() ==
               "ValidationException");
    BOOST_TEST(call("ImportTable", import_body("t", "src", "p/", "XML")).type() ==
               "ValidationException");
    BOOST_TEST(call("ImportTable",
                    import_body("t", "src", "p/", "CSV", R"("InputCompressionType":"LZ4",)"))
                   .type() == "ValidationException");
    // The table is checked as CreateTable checks it.
    BOOST_TEST(call("ImportTable",
                    R"({"S3BucketSource":{"S3Bucket":"src"},"InputFormat":"CSV",)"
                    R"("TableCreationParameters":{"TableName":"t",)"
                    R"("AttributeDefinitions":[{"AttributeName":"pk","AttributeType":"S"}],)"
                    R"("KeySchema":[{"AttributeName":"other","KeyType":"HASH"}],)"
                    R"("BillingMode":"PAY_PER_REQUEST"}})")
                   .type() == "ValidationException");
    BOOST_TEST(call("DescribeImport",
                    R"({"ImportArn":")" + arn("t") + R"(/import/01794300000123-0000abcd"})")
                   .type() == "ImportNotFoundException");

    buckets.store("src")->put("p/1", R"({"Item":{"pk":{"S":"a"},"sk":{"N":"1"}}})");
    for (int i = 0; i < 3; ++i) {
        clock_ms += 10;
        (void)imported(import_body("imp" + std::to_string(i), "src", "p/", "DYNAMODB_JSON"));
    }
    const auto all = ok("ListImports", "{}").at("ImportSummaryList").as_array();
    BOOST_TEST_REQUIRE(all.size() == 3U);
    const auto& s = all[0].as_object();
    BOOST_TEST(s.at("ImportStatus").as_string() == "COMPLETED");
    BOOST_TEST(s.at("InputFormat").as_string() == "DYNAMODB_JSON");
    BOOST_TEST(s.at("S3BucketSource").as_object().at("S3KeyPrefix").as_string() == "p/");
    BOOST_TEST(s.contains("EndTime"));
    const auto one = ok("ListImports", R"({"TableArn":")" + arn("imp1") + R"("})");
    BOOST_TEST(one.at("ImportSummaryList").as_array().size() == 1U);
    const auto page = ok("ListImports", R"({"PageSize":2})");
    BOOST_TEST(page.at("ImportSummaryList").as_array().size() == 2U);
    const auto rest =
        ok("ListImports", R"({"PageSize":2,"NextToken":")" +
                              std::string(page.at("NextToken").as_string()) + R"("})");
    BOOST_TEST(rest.at("ImportSummaryList").as_array().size() == 1U);
}

BOOST_FIXTURE_TEST_CASE(records_survive_their_encoding, export_fixture) {
    ddb::export_record e;
    e.id = "01794300000123-0000abcd";
    e.arn = arn("t") + "/export/" + e.id;
    e.table_name = "t";
    e.table_arn = arn("t");
    e.table_id = 7;
    e.status = "COMPLETED";
    e.start_ms = 1;
    e.end_ms = 2;
    e.export_ms = 1;
    e.client_token = "c";
    e.bucket = "bkt";
    e.prefix = "p";
    e.sse_algorithm = "KMS";
    e.sse_kms_key_id = "k";
    e.format = "ION";
    e.type = "INCREMENTAL_EXPORT";
    e.from_ms = 3;
    e.to_ms = 4;
    e.view = "NEW_IMAGE";
    e.billed_bytes = 5;
    e.item_count = 6;
    e.manifest = "m";
    const auto e2 = ddb::export_detail::decode_export(ddb::export_detail::encode_export(e));
    BOOST_TEST_REQUIRE(e2.has_value());
    BOOST_TEST((*e2 == e));
    ddb::import_record i;
    i.id = e.id;
    i.arn = arn("t") + "/import/" + i.id;
    i.format = "CSV";
    i.csv_delimiter = "|";
    i.csv_headers = {"a", "b"};
    i.creation = json::object{{"TableName", "t"}};
    i.errors = 3;
    const auto i2 = ddb::export_detail::decode_import(ddb::export_detail::encode_import(i));
    BOOST_TEST_REQUIRE(i2.has_value());
    BOOST_TEST((*i2 == i));
    BOOST_TEST((ddb::export_detail::id_of(i.arn, "import") == i.id));
    BOOST_TEST(!ddb::export_detail::id_of(i.arn, "export").has_value());
    BOOST_TEST(ddb::export_detail::export_root("", "x") == "AWSDynamoDB/x/");
    BOOST_TEST(ddb::export_detail::export_root("a/b/", "x") == "a/b/AWSDynamoDB/x/");
    BOOST_TEST(ddb::export_detail::iso8601(1604474914028ULL) == "2020-11-04T07:28:34.028Z");
    BOOST_TEST(ddb::export_detail::valid_bucket("my.bucket-1"));
    BOOST_TEST(!ddb::export_detail::valid_bucket("../etc"));
    BOOST_TEST(!ddb::export_detail::valid_bucket("ab"));
}
