// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// Point-in-time recovery (`.kiro/specs/dynamodb-compatible-api/` task 19,
// Requirement 17.2) over local_backend with a change log store: the three
// actions and their refusals, restores to chosen times across puts,
// updates and deletes, a later base, the restorable window and pruning,
// re-enables and orphaned archives, and the stored forms.

#define BOOST_TEST_MODULE dynamodb_pitr_test

#include <boost/test/unit_test.hpp>

#include "dynamodb_test_fixture.hpp"

#include <kythira/dynamodb/backup.hpp>
#include <kythira/dynamodb/catalog.hpp>
#include <kythira/dynamodb/pitr.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace dynamodb_test;

namespace {

constexpr std::uint64_t k_day = 86'400'000;

auto worker_options() -> ddb::pitr_worker_options {
    ddb::pitr_worker_options o;
    o.latest_lag_ms = 0;
    return o;
}

struct pitr_fixture : fixture {
    std::shared_ptr<ddb::memory_backup_store> store = std::make_shared<ddb::memory_backup_store>();
    ddb::pitr_api<ddb::local_backend<>> pitr{backend, store};
    ddb::pitr_worker<ddb::local_backend<>> worker{backend, store, worker_options()};

    pitr_fixture() {
        without_throttling();
        pitr.register_with(*fe);
        pitr.on_changed([this] { worker.poke(); });
    }

    auto enable(const std::string& table, const std::string& extra = "") -> json::object {
        return ok("UpdateContinuousBackups",
                  R"({"TableName":")" + table +
                      R"(","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":true)" +
                      extra + "}}")
            .at("ContinuousBackupsDescription")
            .as_object();
    }
    auto disable(const std::string& table) -> void {
        (void)ok(
            "UpdateContinuousBackups",
            R"({"TableName":")" + table +
                R"(","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":false}})");
    }
    auto describe(const std::string& table) -> json::object {
        return ok("DescribeContinuousBackups", R"({"TableName":")" + table + R"("})")
            .at("ContinuousBackupsDescription")
            .as_object()
            .at("PointInTimeRecoveryDescription")
            .as_object();
    }
    auto put(const std::string& table, const std::string& pk, const std::string& v) -> void {
        (void)ok("PutItem", R"({"TableName":")" + table + R"(","Item":{"pk":{"S":")" + pk +
                                R"("},"sk":{"N":"1"},"v":{"S":")" + v + R"("}}})");
    }
    auto remove(const std::string& table, const std::string& pk) -> void {
        (void)ok("DeleteItem", R"({"TableName":")" + table + R"(","Key":{"pk":{"S":")" + pk +
                                   R"("},"sk":{"N":"1"}}})");
    }
    auto restore(const std::string& source, const std::string& target, const std::string& when)
        -> reply {
        return call("RestoreTableToPointInTime", R"({"SourceTableName":")" + source +
                                                     R"(","TargetTableName":")" + target + R"(",)" +
                                                     when + "}");
    }
    static auto at(std::uint64_t ms) -> std::string {
        return R"("RestoreDateTime":)" + std::to_string(static_cast<double>(ms) / 1000.0);
    }
    /// pk -> v of every item, read consistently.
    auto contents(const std::string& table) -> std::map<std::string, std::string> {
        std::map<std::string, std::string> out;
        const auto r = ok("Scan", R"({"TableName":")" + table + R"(","ConsistentRead":true})");
        for (const auto& i : r.at("Items").as_array()) {
            const auto& o = i.as_object();
            out[std::string(o.at("pk").as_object().at("S").as_string())] =
                std::string(o.at("v").as_object().at("S").as_string());
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
    /// Restores `source` to `when` as `target` and loads it.
    auto restored(const std::string& source, const std::string& target, const std::string& when)
        -> std::map<std::string, std::string> {
        const auto r = restore(source, target, when);
        BOOST_REQUIRE_MESSAGE(r.status == 200U, r.type() + ": " + r.message());
        (void)worker.pass();
        BOOST_TEST(status(target) == "ACTIVE");
        return contents(target);
    }
};

}  // namespace

BOOST_FIXTURE_TEST_CASE(restores_reach_each_chosen_time, pitr_fixture) {
    (void)create("tbl");
    put("tbl", "old", "0");
    const auto t0 = clock_ms;
    const auto enabled = enable("tbl");
    BOOST_TEST(enabled.at("ContinuousBackupsStatus").as_string() == "ENABLED");
    const auto& d0 = enabled.at("PointInTimeRecoveryDescription").as_object();
    BOOST_TEST(d0.at("PointInTimeRecoveryStatus").as_string() == "ENABLED");
    BOOST_TEST(d0.at("RecoveryPeriodInDays").to_number<int>() == 35);
    // No restorable time before the first base.
    BOOST_TEST(!d0.contains("EarliestRestorableDateTime"));
    BOOST_TEST(restore("tbl", "early", R"("UseLatestRestorableTime":true)").type() ==
               "InvalidRestoreTimeException");

    (void)worker.pass();
    auto d = describe("tbl");
    BOOST_TEST(d.at("EarliestRestorableDateTime").to_number<double>() ==
               static_cast<double>(t0) / 1000.0);

    clock_ms = t0 + 1000;
    put("tbl", "a", "1");
    put("tbl", "b", "1");
    (void)worker.pass();
    clock_ms = t0 + 2000;
    put("tbl", "a", "2");
    remove("tbl", "b");
    remove("tbl", "old");
    put("tbl", "c", "2");
    clock_ms = t0 + 3000;
    (void)worker.pass();
    d = describe("tbl");
    BOOST_TEST(d.at("LatestRestorableDateTime").to_number<double>() ==
               static_cast<double>(t0 + 3000) / 1000.0);
    // Written after the last pass: past the latest restorable time.
    clock_ms = t0 + 4000;
    put("tbl", "late", "4");

    using items = std::map<std::string, std::string>;
    BOOST_TEST((restored("tbl", "at0", at(t0)) == items{{"old", "0"}}));
    BOOST_TEST(
        (restored("tbl", "at1", at(t0 + 1000)) == items{{"a", "1"}, {"b", "1"}, {"old", "0"}}));
    BOOST_TEST((restored("tbl", "at2", at(t0 + 2000)) == items{{"a", "2"}, {"c", "2"}}));
    // The passes those restores ran archived "late" too.
    BOOST_TEST((restored("tbl", "latest", R"("UseLatestRestorableTime":true)") ==
                items{{"a", "2"}, {"c", "2"}, {"late", "4"}}));

    // The summary names the source and the time restored to.
    const auto t = ok("DescribeTable", R"({"TableName":"at1"})").at("Table").as_object();
    const auto& summary = t.at("RestoreSummary").as_object();
    BOOST_TEST(summary.at("SourceTableArn").as_string() ==
               "arn:aws:dynamodb:us-east-1:" + std::string(k_account) + ":table/tbl");
    BOOST_TEST(summary.at("RestoreDateTime").to_number<double>() ==
               static_cast<double>(t0 + 1000) / 1000.0);
    BOOST_TEST(!summary.at("RestoreInProgress").as_bool());
    BOOST_TEST(!summary.contains("SourceBackupArn"));
    // Recovery is the source's setting: the copy has none.
    BOOST_TEST(describe("at1").at("PointInTimeRecoveryStatus").as_string() == "DISABLED");
}

BOOST_FIXTURE_TEST_CASE(a_restore_loads_before_it_takes_writes, pitr_fixture) {
    (void)create("tbl");
    (void)enable("tbl");
    (void)worker.pass();
    clock_ms += 1000;
    put("tbl", "a", "1");
    (void)worker.pass();
    const auto r = restore("tbl", "copy", R"("UseLatestRestorableTime":true)");
    BOOST_TEST_REQUIRE(r.status == 200U);
    const auto& t = r.body.at("TableDescription").as_object();
    BOOST_TEST(t.at("TableStatus").as_string() == "CREATING");
    BOOST_TEST(t.at("RestoreSummary").as_object().at("RestoreInProgress").as_bool());
    BOOST_TEST(
        call("PutItem", R"({"TableName":"copy","Item":{"pk":{"S":"x"},"sk":{"N":"1"}}})").type() ==
        "ResourceNotFoundException");
    (void)worker.pass();
    BOOST_TEST(status("copy") == "ACTIVE");
    put("copy", "x", "9");
    BOOST_TEST((contents("copy") == std::map<std::string, std::string>{{"a", "1"}, {"x", "9"}}));
}

BOOST_FIXTURE_TEST_CASE(a_later_base_serves_later_times, pitr_fixture) {
    ddb::pitr_worker_options o = worker_options();
    o.base_interval_ms = 10'000;
    ddb::pitr_worker<ddb::local_backend<>> w{backend, store, o};
    (void)create("tbl");
    const auto t0 = clock_ms;
    (void)enable("tbl");
    put("tbl", "a", "1");
    (void)w.pass();
    clock_ms = t0 + 5000;
    put("tbl", "a", "2");
    (void)w.pass();
    clock_ms = t0 + 12'000;
    (void)w.pass();  // takes the second base
    clock_ms = t0 + 13'000;
    put("tbl", "a", "3");
    put("tbl", "b", "3");
    clock_ms = t0 + 14'000;
    (void)w.pass();
    const auto archive = ddb::pitr_detail::archive_prefix(backend.describe("tbl")->table_id,
                                                          backend.describe("tbl")->pitr.enabled_ms);
    BOOST_TEST(ddb::pitr_detail::bases(*store, archive).size() == 2U);

    const auto load = [&](const std::string& target, std::uint64_t when) {
        const auto r = restore("tbl", target, at(when));
        BOOST_TEST_REQUIRE(r.status == 200U, r.type() << ": " << r.message());
        (void)w.pass();
        return contents(target);
    };
    using items = std::map<std::string, std::string>;
    BOOST_TEST((load("at5", t0 + 5000) == items{{"a", "2"}}));
    BOOST_TEST((load("at12", t0 + 12'000) == items{{"a", "2"}}));
    BOOST_TEST((load("at13", t0 + 13'000) == items{{"a", "3"}, {"b", "3"}}));
}

BOOST_FIXTURE_TEST_CASE(the_window_prunes_old_bases_and_records, pitr_fixture) {
    ddb::pitr_worker_options o = worker_options();
    o.base_interval_ms = k_day;
    ddb::pitr_worker<ddb::local_backend<>> w{backend, store, o};
    (void)create("tbl");
    const auto t0 = clock_ms;
    const auto d0 = enable("tbl", R"(,"RecoveryPeriodInDays":1)");
    BOOST_TEST(d0.at("PointInTimeRecoveryDescription")
                   .as_object()
                   .at("RecoveryPeriodInDays")
                   .to_number<int>() == 1);
    (void)w.pass();
    // A day of history, a base each day.
    for (int hour = 1; hour <= 60; ++hour) {
        clock_ms = t0 + static_cast<std::uint64_t>(hour) * 3'600'000;
        put("tbl", "h", std::to_string(hour));
        (void)w.pass();
    }
    const auto desc = backend.describe("tbl");
    const auto archive = ddb::pitr_detail::archive_prefix(desc->table_id, desc->pitr.enabled_ms);
    const auto bases = ddb::pitr_detail::bases(*store, archive);
    // Bases at 0 h, 24 h, 48 h; the first ended before the window (60 h -
    // 24 h) and the second still covers its start, so only the first goes.
    BOOST_TEST_REQUIRE(bases.size() == 2U);
    BOOST_TEST(bases.front().start_ms == t0 + 24 * 3'600'000ULL);
    const auto logs = ddb::pitr_detail::log_objects(*store, archive);
    BOOST_TEST_REQUIRE(!logs.empty());
    BOOST_TEST(logs.begin()->first == bases.front().log_from);
    // The window's start, not the oldest base, bounds a restore.
    const auto d = describe("tbl");
    const auto earliest = d.at("EarliestRestorableDateTime").to_number<double>();
    BOOST_TEST(earliest == static_cast<double>(clock_ms - k_day) / 1000.0);
    BOOST_TEST(restore("tbl", "gone", at(t0 + 30 * 3'600'000ULL)).type() ==
               "InvalidRestoreTimeException");
    BOOST_TEST_REQUIRE(restore("tbl", "kept", at(t0 + 40 * 3'600'000ULL)).status == 200U);
    (void)w.pass();
    BOOST_TEST((contents("kept") == std::map<std::string, std::string>{{"h", "40"}}));
}

BOOST_FIXTURE_TEST_CASE(refusals_follow_aws, pitr_fixture) {
    (void)create("tbl");
    (void)create("other");
    BOOST_TEST(call("DescribeContinuousBackups", R"({"TableName":"nope"})").type() ==
               "TableNotFoundException");
    BOOST_TEST(
        call(
            "UpdateContinuousBackups",
            R"({"TableName":"nope","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":true}})")
            .type() == "TableNotFoundException");
    BOOST_TEST(
        call(
            "UpdateContinuousBackups",
            R"({"TableName":"tbl","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":false,"RecoveryPeriodInDays":3}})")
            .type() == "ValidationException");
    BOOST_TEST(
        call(
            "UpdateContinuousBackups",
            R"({"TableName":"tbl","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":true,"RecoveryPeriodInDays":36}})")
            .type() == "ValidationException");
    BOOST_TEST(describe("tbl").at("PointInTimeRecoveryStatus").as_string() == "DISABLED");
    BOOST_TEST(restore("tbl", "copy", R"("UseLatestRestorableTime":true)").type() ==
               "PointInTimeRecoveryUnavailableException");
    BOOST_TEST(restore("nope", "copy", R"("UseLatestRestorableTime":true)").type() ==
               "TableNotFoundException");

    (void)enable("tbl");
    (void)worker.pass();
    clock_ms += 1000;
    (void)worker.pass();
    BOOST_TEST(restore("tbl", "copy", R"("UseLatestRestorableTime":true,)" + at(clock_ms)).type() ==
               "ValidationException");
    BOOST_TEST(
        call("RestoreTableToPointInTime", R"({"SourceTableName":"tbl","TargetTableName":"copy"})")
            .type() == "ValidationException");
    BOOST_TEST(restore("tbl", "copy", at(clock_ms + 60'000)).type() ==
               "InvalidRestoreTimeException");
    BOOST_TEST(restore("tbl", "copy", at(clock_ms - 60'000)).type() ==
               "InvalidRestoreTimeException");
    BOOST_TEST(restore("tbl", "other", R"("UseLatestRestorableTime":true)").type() ==
               "TableAlreadyExistsException");
    // By ARN, too.
    const auto r =
        call("RestoreTableToPointInTime",
             R"({"SourceTableArn":"arn:aws:dynamodb:us-east-1:)" + std::string(k_account) +
                 R"(:table/tbl","TargetTableName":"copy","UseLatestRestorableTime":true})");
    BOOST_TEST(r.status == 200U);

    // Without a store nothing can be archived.
    ddb::pitr_api<ddb::local_backend<>> bare{backend, nullptr};
    auto in =
        json::parse(
            R"({"TableName":"other","PointInTimeRecoverySpecification":{"PointInTimeRecoveryEnabled":true}})")
            .as_object();
    ddb::call_context ctx;
    const auto refused = bare.update(ctx, in);
    BOOST_TEST_REQUIRE(!refused.has_value());
    BOOST_TEST(refused.error().type.ends_with("ContinuousBackupsUnavailableException"));
}

BOOST_FIXTURE_TEST_CASE(a_re_enable_starts_a_new_archive, pitr_fixture) {
    (void)create("tbl");
    const auto t0 = clock_ms;
    (void)enable("tbl");
    put("tbl", "a", "1");
    (void)worker.pass();
    const auto id = backend.describe("tbl")->table_id;
    const auto first = ddb::pitr_detail::archive_prefix(id, t0);
    BOOST_TEST(!store->list(first).empty());
    // Changing the period keeps the archive.
    clock_ms = t0 + 1000;
    (void)enable("tbl", R"(,"RecoveryPeriodInDays":7)");
    BOOST_TEST(backend.describe("tbl")->pitr.enabled_ms == t0);
    BOOST_TEST(describe("tbl").at("RecoveryPeriodInDays").to_number<int>() == 7);

    clock_ms = t0 + 2000;
    disable("tbl");
    BOOST_TEST(describe("tbl").at("PointInTimeRecoveryStatus").as_string() == "DISABLED");
    BOOST_TEST(!backend.describe("tbl")->logs_changes());
    (void)worker.pass();
    BOOST_TEST(store->list(first).empty());

    clock_ms = t0 + 3000;
    (void)enable("tbl");
    (void)worker.pass();
    BOOST_TEST(!store->list(ddb::pitr_detail::archive_prefix(id, t0 + 3000)).empty());
    // Times before the new enable are gone.
    BOOST_TEST(restore("tbl", "copy", at(t0 + 1000)).type() == "InvalidRestoreTimeException");

    // A deleted table's archive is swept.
    (void)ok("DeleteTable", R"({"TableName":"tbl"})");
    for (int i = 0; i < 20 && backend.describe("tbl"); ++i) {
        (void)ok("ListTables", "{}");
    }
    worker.poke();
    (void)worker.pass();
    BOOST_TEST(store->list(ddb::pitr_detail::table_prefix(id)).empty());
}

BOOST_FIXTURE_TEST_CASE(only_the_table_s_own_records_are_archived, pitr_fixture) {
    (void)create("tbl");
    (void)create("noisy",
                 R"(,"StreamSpecification":{"StreamEnabled":true,"StreamViewType":"KEYS_ONLY"})");
    (void)enable("tbl");
    (void)worker.pass();
    clock_ms += 1000;
    put("noisy", "n", "1");
    put("tbl", "a", "1");
    (void)worker.pass();
    const auto d = backend.describe("tbl");
    const auto logs = ddb::pitr_detail::log_objects(
        *store, ddb::pitr_detail::archive_prefix(d->table_id, d->pitr.enabled_ms));
    BOOST_TEST_REQUIRE(logs.size() == 1U);
    const auto body = store->get(logs.begin()->second).value();
    BOOST_TEST(std::ranges::count(body, '\n') == 1);
    BOOST_TEST(body.find(R"("S":"a")") != std::string::npos);
    // A pass with nothing new writes nothing.
    clock_ms += 1000;
    (void)worker.pass();
    BOOST_TEST(ddb::pitr_detail::log_objects(
                   *store, ddb::pitr_detail::archive_prefix(d->table_id, d->pitr.enabled_ms))
                   .size() == 1U);
}

BOOST_AUTO_TEST_CASE(stored_forms_round_trip) {
    ddb::table_descriptor d;
    d.name = "tbl";
    d.table_id = 4;
    d.schema.partition_name = "pk";
    BOOST_TEST(static_cast<int>(ddb::encode_table_descriptor(d)[0]) == 3);
    d.pitr.enabled = true;
    d.pitr.recovery_days = 9;
    d.pitr.enabled_ms = 11;
    d.pitr.earliest_ms = 12;
    d.pitr.latest_ms = 13;
    d.pitr.next_object = 14;
    d.pitr.cursors = {{"s1", 5}, {"s2", 6}};
    BOOST_TEST(d.logs_changes());
    const auto bytes = ddb::encode_table_descriptor(d);
    BOOST_TEST(static_cast<int>(bytes[0]) == 7);
    const auto back = ddb::decode_table_descriptor(bytes);
    BOOST_TEST_REQUIRE(back.has_value());
    BOOST_TEST((*back == d));

    ddb::table_descriptor copy;
    copy.name = "copy";
    copy.restore = ddb::table_restore{"", "arn:t", 77, true, 4, 1234};
    BOOST_TEST(static_cast<int>(ddb::encode_table_descriptor(copy)[0]) == 6);
    BOOST_TEST((ddb::decode_table_descriptor(ddb::encode_table_descriptor(copy)) == copy));

    for (const auto op :
         {ddb::catalog_command::kind::set_pitr, ddb::catalog_command::kind::pitr_progress}) {
        ddb::catalog_command c;
        c.op = op;
        c.table.name = "tbl";
        c.table.pitr.recovery_days = 35;
        auto encoded = ddb::encode_catalog_command(c);
        BOOST_TEST(static_cast<int>(encoded[0]) == 7);
        bool newer = false;
        const auto decoded = ddb::decode_catalog_command(encoded, newer);
        BOOST_TEST_REQUIRE(decoded.has_value());
        BOOST_TEST((decoded->op == op));
        // A replica on format 6 would not know the command.
        encoded[0] = '\6';
        BOOST_TEST(!ddb::decode_catalog_command(encoded, newer).has_value());
    }

    ddb::pitr_base b{100, 200, 3, 2, {ddb::backup_object{"k", 2, 10, "ab"}}};
    BOOST_TEST((ddb::pitr_detail::decode_base(ddb::pitr_detail::encode_base(b)) == b));
    BOOST_TEST((ddb::pitr_detail::log_number(ddb::pitr_detail::log_key("pitr/x/", 42)) == 42U));
    const auto owner =
        ddb::pitr_detail::archive_of(ddb::pitr_detail::archive_prefix(7, 99) + "log/x");
    BOOST_TEST_REQUIRE(owner.has_value());
    BOOST_TEST(owner->first == 7U);
    BOOST_TEST(owner->second == 99U);
}
