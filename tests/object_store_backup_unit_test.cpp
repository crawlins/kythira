#define BOOST_TEST_MODULE object_store_backup_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/object_store_backup.hpp>

#include "mock_object_store.hpp"

// Unit coverage for `object_store_backup` (cloud-object-persistence
// Requirement 10, task 12).
//
// Registered unconditionally and deliberately, exactly like
// `object_store_persistence_unit_test`: `object_store_backup.hpp` names no
// provider and includes no SDK, so it must build and pass in a configuration
// with **every** cloud gate off (Requirement 16.3).
//
// The case this suite exists for is `a_smeared_source_fails_verify_by_name`.
// Everything else here checks a mechanism; that one checks the *claim* — that
// a manifest makes a torn copy detectable rather than merely disclosed. A
// backup catalog whose verify passes on a smear is a catalog that lies in a
// recovery window, which is the only window it is ever read in.
//
// Note on buckets: `mock_object_store` has a single flat keyspace and ignores
// the bucket name beyond requiring it non-empty, so "back up to a different
// bucket" (Requirement 10.5) is not something this suite can distinguish — the
// destination is separated by prefix instead. That is a property of the mock,
// not a gap in the code, and the real-store suites are where a genuinely
// separate bucket gets exercised.

#include <boost/json.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

using namespace kythira;

namespace {

constexpr const char* k_bucket = "kythira-bucket";
constexpr const char* k_source_prefix = "node-1";
constexpr const char* k_dest_prefix = "backups";

auto source_ref() -> backup_source {
    return backup_source{k_bucket, k_source_prefix};
}
auto dest_ref() -> backup_destination {
    return backup_destination{k_bucket, k_dest_prefix};
}

/// The engine's log-key padding, reproduced here so the fixtures speak the same
/// on-bucket format the backup reads.
auto log_key(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return std::string("log/") + std::string(20 - digits.size(), '0') + digits;
}

auto entry_json(std::uint64_t term, std::uint64_t index) -> std::string {
    return R"({"term":)" + std::to_string(term) + R"(,"index":)" + std::to_string(index) +
           R"(,"command":"","type":0})";
}

auto snapshot_json(std::uint64_t last_index, std::uint64_t last_term,
                   const std::vector<std::string>& nodes = {"n1", "n2", "n3"}) -> std::string {
    std::string json = R"({"last_included_index":)" + std::to_string(last_index) +
                       R"(,"last_included_term":)" + std::to_string(last_term) +
                       R"(,"state":"","nodes":[)";
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += '"' + nodes[i] + '"';
    }
    json += R"(],"is_joint_consensus":false})";
    return json;
}

/// A source prefix holding a plausible node: a snapshot through index 3, log
/// entries 4..6, a term and a vote.
auto seed_source(const mock_object_store& store) -> void {
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "voted_for", "n2");
    store.seed(p + "snapshot", snapshot_json(3, 4));
    for (std::uint64_t i = 4; i <= 6; ++i) {
        store.seed(p + log_key(i), entry_json(5, i));
    }
}

/// Every key under a prefix, with the prefix stripped — what a restored target
/// should look like when compared against its source.
auto relative_keys(const mock_object_store& store, const std::string& prefix)
    -> std::vector<std::string> {
    std::vector<std::string> out;
    const std::string root = prefix + "/";
    for (const auto& key : store.keys()) {
        if (key.starts_with(root)) {
            out.push_back(key.substr(root.size()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

auto target_ref(const char* prefix) -> backup_source {
    return backup_source{k_bucket, prefix};
}

auto problems_of_kind(const verification_report& report, std::string_view kind)
    -> std::vector<verification_problem> {
    std::vector<verification_problem> out;
    for (const auto& problem : report.problems) {
        if (problem.kind == kind) {
            out.push_back(problem);
        }
    }
    return out;
}

/// Binds the request log to a local before searching it. `request_log()`
/// returns **by value**, so `begin()`/`end()` written inline name two different
/// vectors — a shape this repo has already been bitten by, where the check
/// silently verified nothing on one architecture and crashed on another.
auto request_index(const mock_object_store& store, std::string_view needle) -> std::size_t {
    const auto log = store.request_log();
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i].find(needle) != std::string::npos) {
            return i;
        }
    }
    return log.size();
}

}  // namespace

// ── create ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_create)

BOOST_AUTO_TEST_CASE(copies_every_object_under_the_source_prefix) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    const auto manifest = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    BOOST_TEST(manifest.objects.size() == 6U);
    for (const auto& relative : {"term", "voted_for", "snapshot"}) {
        BOOST_TEST(store.has(std::string(k_dest_prefix) + "/b1/objects/" + relative));
    }
    for (std::uint64_t i = 4; i <= 6; ++i) {
        BOOST_TEST(store.has(std::string(k_dest_prefix) + "/b1/objects/" + log_key(i)));
    }
    // Byte for byte, not merely present.
    BOOST_TEST(store.body(std::string(k_dest_prefix) + "/b1/objects/term") == "5");
    BOOST_TEST(store.body(std::string(k_dest_prefix) + "/b1/objects/" + log_key(5)) ==
               entry_json(5, 5));
}

BOOST_AUTO_TEST_CASE(the_manifest_is_written_last) {
    // The commit protocol, and the only one available: there is no cross-key
    // atomicity in any of these five services, so "the manifest exists" has to
    // be what "the backup finished" means.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto log = store.request_log();
    const auto manifest_at = request_index(store, "PUT backups/b1/manifest.json");
    BOOST_REQUIRE(manifest_at < log.size());
    // Every object PUT precedes it.
    for (std::size_t i = manifest_at + 1; i < log.size(); ++i) {
        BOOST_TEST(log[i].find("PUT backups/b1/objects/") == std::string::npos);
    }
    BOOST_TEST(manifest_at == log.size() - 1);
}

BOOST_AUTO_TEST_CASE(the_manifest_records_the_nodes_state) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    const auto manifest =
        backup.create(source_ref(), dest_ref(), {.backup_id = "b1", .source_quiesced = true});

    BOOST_TEST(manifest.format_version == 1);
    BOOST_TEST(manifest.provider == "mock");
    BOOST_TEST(manifest.source_bucket == k_bucket);
    BOOST_TEST(manifest.source_prefix == k_source_prefix);
    BOOST_TEST(manifest.backup_id == "b1");
    BOOST_TEST(manifest.source_quiesced);
    BOOST_REQUIRE(manifest.current_term.has_value());
    BOOST_TEST(*manifest.current_term == 5U);
    BOOST_REQUIRE(manifest.voted_for.has_value());
    BOOST_TEST(*manifest.voted_for == "n2");
    BOOST_REQUIRE(manifest.first_log_index.has_value());
    BOOST_TEST(*manifest.first_log_index == 4U);
    BOOST_REQUIRE(manifest.last_log_index.has_value());
    BOOST_TEST(*manifest.last_log_index == 6U);
    BOOST_REQUIRE(manifest.snapshot_last_included_index.has_value());
    BOOST_TEST(*manifest.snapshot_last_included_index == 3U);
    BOOST_REQUIRE(manifest.snapshot_last_included_term.has_value());
    BOOST_TEST(*manifest.snapshot_last_included_term == 4U);
    BOOST_REQUIRE(manifest.configuration_nodes.size() == 3U);
    BOOST_TEST(manifest.configuration_nodes[0] == "n1");
    BOOST_TEST(!manifest.configuration_is_joint);
    // Timestamp shape: `YYYY-MM-DDTHH:MM:SSZ`.
    BOOST_TEST(manifest.created_at.size() == 20U);
    BOOST_TEST(manifest.created_at.back() == 'Z');
}

BOOST_AUTO_TEST_CASE(a_fresh_prefix_backs_up_with_every_field_absent) {
    // A node that has never voted, never advanced a term and never snapshotted
    // is a legal thing to back up. Absent must stay absent rather than becoming
    // a zero that a later restore would believe.
    const mock_object_store store;
    store.seed(std::string(k_source_prefix) + "/owner", R"({"node":"n1"})");
    const object_store_backup<mock_object_store> backup{store};

    const auto manifest = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    BOOST_TEST(!manifest.current_term.has_value());
    BOOST_TEST(!manifest.voted_for.has_value());
    BOOST_TEST(!manifest.first_log_index.has_value());
    BOOST_TEST(!manifest.snapshot_last_included_index.has_value());
    BOOST_TEST(manifest.objects.size() == 1U);
}

BOOST_AUTO_TEST_CASE(objects_this_file_does_not_understand_are_still_copied) {
    // A backup that silently dropped keys it could not interpret would restore
    // an incomplete prefix. `owner`, retained snapshots and genuinely foreign
    // objects all copy byte for byte and contribute nothing to the manifest's
    // interpretation.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/owner", R"({"node":"n1","epoch":3})");
    store.seed(std::string(k_source_prefix) + "/snapshots/00000000000000000003",
               snapshot_json(3, 4));
    store.seed(std::string(k_source_prefix) + "/something-else-entirely", "opaque bytes");
    const object_store_backup<mock_object_store> backup{store};

    const auto manifest = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    BOOST_TEST(manifest.objects.size() == 9U);
    BOOST_TEST(store.body(std::string(k_dest_prefix) + "/b1/objects/something-else-entirely") ==
               "opaque bytes");
    BOOST_TEST(
        store.has(std::string(k_dest_prefix) + "/b1/objects/snapshots/00000000000000000003"));
}

BOOST_AUTO_TEST_CASE(a_generated_backup_id_sorts_chronologically) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    const auto manifest = backup.create(source_ref(), dest_ref());

    // `YYYYMMDDTHHMMSSZ` — fixed width, zero padded, no punctuation, so
    // lexicographic order is chronological order. That is the property
    // Requirement 10.2 asks for, and it is the only ordering these stores give.
    BOOST_TEST(manifest.backup_id.size() == 16U);
    BOOST_TEST(manifest.backup_id[8] == 'T');
    BOOST_TEST(manifest.backup_id.back() == 'Z');
    BOOST_TEST(manifest.backup_id.find('-') == std::string::npos);
    BOOST_TEST(manifest.backup_id.find(':') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_backup_id_containing_a_separator_is_refused) {
    // An id names one directory level. One containing `/` would restructure the
    // layout `list` and `verify` depend on, and would do it silently.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    BOOST_CHECK_THROW(
        std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "2026/08/17"}),
        std::invalid_argument);
    BOOST_TEST(store.count_requests("PUT") == 0U);
}

BOOST_AUTO_TEST_CASE(a_corrupt_snapshot_refuses_the_whole_backup) {
    // Absent is fine; present-and-unparseable is not. A manifest that guessed
    // at a corrupt source would be worse than no backup, because its only job
    // is to be the thing an operator trusts in a recovery window.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/snapshot", "{not json");
    const object_store_backup<mock_object_store> backup{store};

    try {
        std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});
        BOOST_FAIL("expected create to throw on a corrupt snapshot");
    } catch (const std::runtime_error& err) {
        const std::string what = err.what();
        BOOST_TEST(what.find("node-1/snapshot") != std::string::npos);
    }
    // And no manifest was written, so the partial copy is invisible to `list`.
    BOOST_TEST(!store.has(std::string(k_dest_prefix) + "/b1/manifest.json"));
}

BOOST_AUTO_TEST_CASE(a_corrupt_term_refuses_the_whole_backup) {
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/term", "not-a-number");
    const object_store_backup<mock_object_store> backup{store};

    BOOST_CHECK_THROW(std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"}),
                      std::runtime_error);
    BOOST_TEST(!store.has(std::string(k_dest_prefix) + "/b1/manifest.json"));
}

BOOST_AUTO_TEST_CASE(a_misplaced_log_key_refuses_the_whole_backup) {
    // The engine's own load path treats a log key it cannot order as corruption
    // rather than as a key to skip. Quietly excluding it would produce a
    // manifest asserting a contiguity it never checked.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/log/7", entry_json(5, 7));
    const object_store_backup<mock_object_store> backup{store};

    try {
        std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});
        BOOST_FAIL("expected create to throw on an unpadded log key");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("20 decimal digits") != std::string::npos);
    }
}

BOOST_AUTO_TEST_SUITE_END()

// ── list ─────────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_list)

BOOST_AUTO_TEST_CASE(lists_finished_backups_in_chronological_order) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};

    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "20260817T100000Z"});
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "20260817T120000Z"});

    const auto listed = backup.list(dest_ref());
    BOOST_REQUIRE_EQUAL(listed.size(), 2U);
    BOOST_TEST(listed[0].backup_id == "20260817T100000Z");
    BOOST_TEST(listed[1].backup_id == "20260817T120000Z");
    BOOST_TEST(*listed[0].current_term == 5U);
}

BOOST_AUTO_TEST_CASE(a_torn_backup_with_no_manifest_is_ignored) {
    // The other half of the commit protocol. An unfinished copy is not a backup,
    // and surfacing it as one would invite restoring from it.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "good"});

    // A copy that died before its manifest: objects present, no manifest.
    store.seed(std::string(k_dest_prefix) + "/torn/objects/term", "5");
    store.seed(std::string(k_dest_prefix) + "/torn/objects/" + log_key(4), entry_json(5, 4));

    const auto listed = backup.list(dest_ref());
    BOOST_REQUIRE_EQUAL(listed.size(), 1U);
    BOOST_TEST(listed[0].backup_id == "good");
}

BOOST_AUTO_TEST_CASE(an_empty_destination_lists_nothing) {
    const mock_object_store store;
    const object_store_backup<mock_object_store> backup{store};
    BOOST_TEST(backup.list(dest_ref()).empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── verify ───────────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_verify)

BOOST_AUTO_TEST_CASE(a_clean_backup_verifies) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.verify(dest_ref(), "b1");
    for (const auto& problem : report.problems) {
        BOOST_TEST_MESSAGE("unexpected problem: " << problem.kind << " " << problem.detail);
    }
    BOOST_TEST(report.problems.empty());
    BOOST_TEST(report.ok);
    BOOST_TEST(report.manifest.backup_id == "b1");
}

BOOST_AUTO_TEST_CASE(verify_of_an_unknown_id_throws) {
    const mock_object_store store;
    const object_store_backup<mock_object_store> backup{store};
    BOOST_CHECK_THROW(std::ignore = backup.verify(dest_ref(), "nope"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(a_deleted_object_is_reported_by_name) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.remove(std::string(k_dest_prefix) + "/b1/objects/" + log_key(5));

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    const auto missing = problems_of_kind(report, "missing_object");
    BOOST_REQUIRE_EQUAL(missing.size(), 1U);
    BOOST_TEST(missing[0].subject.find(log_key(5)) != std::string::npos);
    // And the gap it leaves is reported too — two symptoms of one fault, both
    // named, because an operator needs to know the log is short and not merely
    // that a file vanished.
    BOOST_TEST(problems_of_kind(report, "log_gap").size() == 1U);
}

BOOST_AUTO_TEST_CASE(a_rewritten_object_fails_its_checksum) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    // Same length, different bytes — so this is caught by the checksum and not
    // by the size, which is the point of recording both.
    store.seed(std::string(k_dest_prefix) + "/b1/objects/term", "9");

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    const auto mismatched = problems_of_kind(report, "checksum_mismatch");
    BOOST_REQUIRE_EQUAL(mismatched.size(), 1U);
    BOOST_TEST(mismatched[0].subject.find("term") != std::string::npos);
    BOOST_TEST(mismatched[0].detail.find("hashes to") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_resized_object_is_reported_as_a_size_mismatch) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed(std::string(k_dest_prefix) + "/b1/objects/term", "5555");

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    BOOST_REQUIRE_EQUAL(problems_of_kind(report, "size_mismatch").size(), 1U);
}

BOOST_AUTO_TEST_CASE(a_gap_between_the_snapshot_and_the_first_entry_is_reported) {
    // The snapshot covers through 3 and the log starts at 6, so 4 and 5 are in
    // neither — a backup that would restore a node with a hole in its history.
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot", snapshot_json(3, 4));
    store.seed(p + log_key(6), entry_json(5, 6));
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    const auto gaps = problems_of_kind(report, "log_gap");
    BOOST_REQUIRE_EQUAL(gaps.size(), 1U);
    BOOST_TEST(gaps[0].detail.find("4..5") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_log_entry_from_a_later_term_than_the_manifest_is_reported) {
    // Tests the term-monotonicity *check*. Note this is a source that was never
    // consistent rather than one that stopped being consistent midway — the
    // smear case is the next suite, and conflating the two would leave the
    // check looking tested when only the fixture had been.
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot", snapshot_json(3, 4));
    store.seed(p + log_key(4), entry_json(9, 4));
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    const auto regressions = problems_of_kind(report, "term_regression");
    BOOST_REQUIRE_EQUAL(regressions.size(), 1U);
    BOOST_TEST(regressions[0].detail.find("term 9") != std::string::npos);
    BOOST_TEST(regressions[0].detail.find("current_term 5") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(log_entries_with_no_term_object_are_reported_not_passed_over) {
    // A backup carrying log entries but no term cannot have its monotonicity
    // checked at all. Silence there would read as "checked and fine".
    const mock_object_store store;
    store.seed(std::string(k_source_prefix) + "/" + log_key(1), entry_json(2, 1));
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    BOOST_REQUIRE_EQUAL(problems_of_kind(report, "term_regression").size(), 1U);
}

BOOST_AUTO_TEST_CASE(an_unrecognised_manifest_version_refuses_rather_than_guesses) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const std::string manifest_key = std::string(k_dest_prefix) + "/b1/manifest.json";
    std::string body = store.body(manifest_key);
    const auto at = body.find("\"format_version\":1");
    BOOST_REQUIRE(at != std::string::npos);
    body.replace(at, std::string("\"format_version\":1").size(), "\"format_version\":99");
    store.seed(manifest_key, body);

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    BOOST_REQUIRE_EQUAL(problems_of_kind(report, "manifest_version").size(), 1U);
    // And it stopped there rather than reporting a cascade of checksum failures
    // it had no basis to interpret.
    BOOST_TEST(report.problems.size() == 1U);
}

BOOST_AUTO_TEST_CASE(verify_reports_every_problem_not_just_the_first) {
    // An operator in a recovery window wants the whole list.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.remove(std::string(k_dest_prefix) + "/b1/objects/" + log_key(5));
    store.seed(std::string(k_dest_prefix) + "/b1/objects/voted_for", "n9");

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);
    BOOST_TEST(problems_of_kind(report, "missing_object").size() == 1U);
    BOOST_TEST(problems_of_kind(report, "checksum_mismatch").size() == 1U);
    BOOST_TEST(problems_of_kind(report, "log_gap").size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The case this suite exists for ───────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_smear)

BOOST_AUTO_TEST_CASE(a_smeared_source_fails_verify_by_name) {
    // Requirement 10.4's real claim, and the one that distinguishes a manifest
    // from a warning: a backup taken from a *running* node must be **detectably**
    // inconsistent, not merely disclosed as possibly inconsistent.
    //
    // The smear is produced the only honest way — by mutating the source while
    // the copy is in flight, rather than by seeding a source that was never
    // consistent. The listing is taken once, up front; the node then truncates
    // an entry that the listing had already promised, and the copy reaches it
    // to find it gone.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/" + log_key(7), entry_json(5, 7));
    store.seed(std::string(k_source_prefix) + "/" + log_key(8), entry_json(5, 8));

    // When the copy reads entry 5, entry 7 is truncated out from under it.
    store.state()->on_get = [&store](const std::string& key) {
        if (key == std::string(k_source_prefix) + "/" + log_key(5)) {
            store.remove(std::string(k_source_prefix) + "/" + log_key(7));
        }
    };

    const object_store_backup<mock_object_store> backup{store};
    // The caller even *claims* the source was quiesced. The claim is recorded
    // and not believed — which is the whole design.
    const auto manifest =
        backup.create(source_ref(), dest_ref(), {.backup_id = "b1", .source_quiesced = true});
    store.state()->on_get = nullptr;

    BOOST_TEST(manifest.source_quiesced);

    const auto report = backup.verify(dest_ref(), "b1");
    BOOST_TEST(!report.ok);

    const auto gaps = problems_of_kind(report, "log_gap");
    BOOST_REQUIRE_EQUAL(gaps.size(), 1U);
    // Named specifically enough to act on: "this backup is bad" is not
    // actionable, "log index 7 is missing between 6 and 8" is.
    BOOST_TEST(gaps[0].detail.find("log index 7 is missing") != std::string::npos);
    BOOST_TEST(gaps[0].detail.find("at 6 and 8") != std::string::npos);

    // And the smear shows up as a *gap*, not as a phantom missing object. An
    // object that vanished before it could be copied is simply not in this
    // backup, so the manifest — which is an inventory of what the backup
    // actually holds — must not claim it. Recording it with an empty checksum
    // would turn "this backup is missing index 7" into "this backup is
    // corrupt", which is a different and wrong diagnosis.
    BOOST_TEST(problems_of_kind(report, "missing_object").empty());
    const auto claimed = std::count_if(
        manifest.objects.begin(), manifest.objects.end(),
        [](const backup_object_entry& entry) { return entry.relative_key == log_key(7); });
    BOOST_TEST(claimed == 0);
    BOOST_TEST(manifest.objects.size() == 7U);
}

BOOST_AUTO_TEST_CASE(the_same_source_backed_up_quiesced_verifies_clean) {
    // The negative control, and it is not optional: "the smeared backup failed
    // verify" says nothing unless the identical source, copied without
    // interference, passes. Without this, a verify that rejected *every* backup
    // would look like a working detector.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/" + log_key(7), entry_json(5, 7));
    store.seed(std::string(k_source_prefix) + "/" + log_key(8), entry_json(5, 8));

    const object_store_backup<mock_object_store> backup{store};
    std::ignore =
        backup.create(source_ref(), dest_ref(), {.backup_id = "b1", .source_quiesced = true});

    const auto report = backup.verify(dest_ref(), "b1");
    for (const auto& problem : report.problems) {
        BOOST_TEST_MESSAGE("unexpected problem: " << problem.kind << " " << problem.detail);
    }
    BOOST_TEST(report.ok);
}

BOOST_AUTO_TEST_SUITE_END()

// ── restore_clone ────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_restore_clone)

BOOST_AUTO_TEST_CASE(a_cloned_prefix_matches_the_original_field_for_field) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.restore_clone(dest_ref(), "b1", target_ref("node-1-restored"));

    BOOST_TEST(report.backup_id == "b1");
    BOOST_TEST(report.objects_written == 6U);
    BOOST_REQUIRE_EQUAL(report.nodes.size(), 1U);
    BOOST_TEST(report.nodes[0].prefix == "node-1-restored");

    // Same keys, and the same bytes under each — byte for byte is the claim,
    // so bytes are what is compared.
    const auto original = relative_keys(store, k_source_prefix);
    const auto restored = relative_keys(store, "node-1-restored");
    BOOST_REQUIRE(original == restored);
    for (const auto& relative : original) {
        BOOST_TEST(store.body(std::string(k_source_prefix) + "/" + relative) ==
                   store.body("node-1-restored/" + relative));
    }
}

BOOST_AUTO_TEST_CASE(the_restored_owner_claims_a_higher_epoch) {
    // Requirement 11.1. This is what makes a split-brain loud instead of
    // silent: the original, if it comes back, finds its own epoch stale and
    // fences itself out on its next write rather than writing alongside.
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/owner",
               R"({"owner_id":"n1","epoch":7,"started_at":"2026-08-17T00:00:00Z"})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.restore_clone(dest_ref(), "b1", target_ref("node-1-restored"));

    BOOST_REQUIRE(report.owner_epoch.has_value());
    BOOST_TEST(*report.owner_epoch == 8U);
    const auto owner = boost::json::parse(store.body("node-1-restored/owner")).as_object();
    BOOST_TEST(owner.at("epoch").to_number<std::uint64_t>() == 8U);
    // Strictly higher than the original, which is untouched.
    const auto source_owner =
        boost::json::parse(store.body(std::string(k_source_prefix) + "/owner")).as_object();
    BOOST_TEST(source_owner.at("epoch").to_number<std::uint64_t>() == 7U);
    // And the rest of the record is preserved — this is an epoch bump, not a
    // rewrite of who the owner is.
    BOOST_TEST(std::string(owner.at("owner_id").as_string()) == "n1");
}

BOOST_AUTO_TEST_CASE(a_backup_with_no_owner_object_restores_without_inventing_one) {
    // The owner object only exists under `compare_and_swap`. Inventing one for
    // an unfenced engine would make the restored prefix look fenced when it is
    // not.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report = backup.restore_clone(dest_ref(), "b1", target_ref("node-1-restored"));

    BOOST_TEST(!report.owner_epoch.has_value());
    BOOST_TEST(!store.has("node-1-restored/owner"));
}

BOOST_AUTO_TEST_CASE(an_owner_record_with_no_epoch_is_refused) {
    const mock_object_store store;
    seed_source(store);
    store.seed(std::string(k_source_prefix) + "/owner", R"({"owner_id":"n1"})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    try {
        std::ignore = backup.restore_clone(dest_ref(), "b1", target_ref("node-1-restored"));
        BOOST_FAIL("expected an epoch-less owner record to be refused");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("no epoch") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(a_non_empty_target_is_refused_without_force) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed("occupied/term", "99");
    store.seed("occupied/" + log_key(1), entry_json(99, 1));

    try {
        std::ignore = backup.restore_clone(dest_ref(), "b1", target_ref("occupied"));
        BOOST_FAIL("expected a non-empty target to be refused");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("without force") != std::string::npos);
    }
    // Nothing was written, so the refusal is total rather than partial.
    BOOST_TEST(store.body("occupied/term") == "99");
    BOOST_TEST(relative_keys(store, "occupied").size() == 2U);
}

BOOST_AUTO_TEST_CASE(force_deletes_engine_owned_keys_and_never_merges) {
    // Requirement 11.4's real claim. The target holds a *different* node's
    // state; after a forced restore it must hold the backup's state and no
    // trace of what was there — not the union of the two, which is a log that
    // is two histories interleaved and which no later repair untangles.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed("occupied/term", "99");
    store.seed("occupied/voted_for", "n9");
    store.seed("occupied/" + log_key(40), entry_json(99, 40));
    store.seed("occupied/" + log_key(41), entry_json(99, 41));
    store.seed("occupied/snapshots/00000000000000000039", snapshot_json(39, 99));

    const auto report =
        backup.restore_clone(dest_ref(), "b1", target_ref("occupied"), {.force = true});

    // The old node's log entries are gone, not merged alongside 4..6.
    BOOST_TEST(!store.has("occupied/" + log_key(40)));
    BOOST_TEST(!store.has("occupied/" + log_key(41)));
    BOOST_TEST(!store.has("occupied/snapshots/00000000000000000039"));
    BOOST_TEST(store.body("occupied/term") == "5");
    BOOST_TEST(store.body("occupied/voted_for") == "n2");

    const auto original = relative_keys(store, k_source_prefix);
    const auto restored = relative_keys(store, "occupied");
    BOOST_TEST(original == restored);
    BOOST_TEST(report.keys_deleted.size() == 5U);
}

BOOST_AUTO_TEST_CASE(force_leaves_foreign_objects_alone) {
    // The engine neither reads, writes nor deletes objects it does not own, and
    // neither does this. They also cannot merge with Raft state, so leaving
    // them is safe as well as correct.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed("occupied/term", "99");
    store.seed("occupied/operators-own-notes.txt", "do not delete");

    const auto report =
        backup.restore_clone(dest_ref(), "b1", target_ref("occupied"), {.force = true});

    BOOST_TEST(store.body("occupied/operators-own-notes.txt") == "do not delete");
    BOOST_REQUIRE_EQUAL(report.keys_deleted.size(), 1U);
    BOOST_TEST(report.keys_deleted[0] == "occupied/term");
}

BOOST_AUTO_TEST_CASE(a_failing_verify_aborts_before_anything_is_written) {
    // Requirement 11.5. The restore is the one operation where a warning is
    // worthless, so it aborts on the *first* problem rather than collecting
    // them the way `verify` does.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.remove(std::string(k_dest_prefix) + "/b1/objects/" + log_key(5));

    try {
        std::ignore = backup.restore_clone(dest_ref(), "b1", target_ref("node-1-restored"));
        BOOST_FAIL("expected a failing verify to abort the restore");
    } catch (const std::runtime_error& err) {
        const std::string what = err.what();
        BOOST_TEST(what.find("refusing to restore") != std::string::npos);
        BOOST_TEST(what.find("missing_object") != std::string::npos);
    }
    BOOST_TEST(relative_keys(store, "node-1-restored").empty());
}

BOOST_AUTO_TEST_SUITE_END()

// ── restore_seed ─────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(object_store_backup_restore_seed)

BOOST_AUTO_TEST_CASE(seeds_one_prefix_per_new_node_with_the_new_configuration) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    const auto report =
        backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha", "beta", "gamma"});

    BOOST_REQUIRE_EQUAL(report.nodes.size(), 3U);
    BOOST_TEST(report.nodes[0].node_id == "alpha");
    BOOST_TEST(report.nodes[0].prefix == "fresh/alpha");
    BOOST_TEST(report.nodes[2].prefix == "fresh/gamma");

    for (const auto& node : {"alpha", "beta", "gamma"}) {
        const std::string prefix = std::string("fresh/") + node;
        // The snapshot's term, the vote cleared, the log empty.
        BOOST_TEST(store.body(prefix + "/term") == "4");
        BOOST_TEST(!store.has(prefix + "/voted_for"));
        const auto keys = relative_keys(store, prefix);
        BOOST_REQUIRE_EQUAL(keys.size(), 2U);
        BOOST_TEST(keys[0] == "snapshot");
        BOOST_TEST(keys[1] == "term");

        const auto snap = boost::json::parse(store.body(prefix + "/snapshot")).as_object();
        // The state-machine bytes survive; the configuration does not.
        BOOST_TEST(snap.at("last_included_index").to_number<std::uint64_t>() == 3U);
        BOOST_TEST(snap.at("last_included_term").to_number<std::uint64_t>() == 4U);
        const auto& nodes = snap.at("nodes").as_array();
        BOOST_REQUIRE_EQUAL(nodes.size(), 3U);
        BOOST_TEST(std::string(nodes[0].as_string()) == "alpha");
        BOOST_TEST(std::string(nodes[2].as_string()) == "gamma");
        BOOST_TEST(!snap.at("is_joint_consensus").as_bool());
    }
}

BOOST_AUTO_TEST_CASE(the_state_machine_bytes_are_preserved_verbatim) {
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot",
               R"({"last_included_index":3,"last_included_term":4,"state":"aGVsbG8=",)"
               R"("nodes":["n1","n2"],"is_joint_consensus":false})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha"});

    const auto snap = boost::json::parse(store.body("fresh/alpha/snapshot")).as_object();
    BOOST_TEST(std::string(snap.at("state").as_string()) == "aGVsbG8=");
}

BOOST_AUTO_TEST_CASE(a_joint_consensus_marker_is_not_carried_into_a_new_cluster) {
    // The old cluster may have been mid-reconfiguration. A new one never is,
    // and carrying the marker across would seed every node with a membership
    // change nobody proposed.
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot",
               R"({"last_included_index":3,"last_included_term":4,"state":"",)"
               R"("nodes":["n1","n2"],"is_joint_consensus":true,"old_nodes":["n0"]})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha"});

    const auto snap = boost::json::parse(store.body("fresh/alpha/snapshot")).as_object();
    BOOST_TEST(!snap.at("is_joint_consensus").as_bool());
    BOOST_TEST(!snap.contains("old_nodes"));
}

BOOST_AUTO_TEST_CASE(numeric_node_ids_are_seeded_as_numbers) {
    // The engine reads this array with `as_int64()` when its NodeId is an
    // integer and `as_string()` when it is a string, and boost::json throws on
    // the wrong one. Writing strings unconditionally would produce a snapshot
    // that parses here and explodes when the operator starts the new cluster.
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot", R"({"last_included_index":3,"last_included_term":4,"state":"",)"
                               R"("nodes":[1,2,3],"is_joint_consensus":false})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"7", "8"});

    const auto snap = boost::json::parse(store.body("fresh/7/snapshot")).as_object();
    const auto& nodes = snap.at("nodes").as_array();
    BOOST_REQUIRE_EQUAL(nodes.size(), 2U);
    BOOST_TEST(nodes[0].is_number());
    BOOST_TEST(nodes[0].to_number<std::int64_t>() == 7);
    BOOST_TEST(!nodes[0].is_string());
}

BOOST_AUTO_TEST_CASE(a_non_numeric_id_for_a_numeric_cluster_is_refused) {
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + "snapshot", R"({"last_included_index":3,"last_included_term":4,"state":"",)"
                               R"("nodes":[1,2],"is_joint_consensus":false})");
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    try {
        std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha"});
        BOOST_FAIL("expected a string id for a numeric cluster to be refused");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("node ids are numbers") != std::string::npos);
    }
    BOOST_TEST(relative_keys(store, "fresh/alpha").empty());
}

BOOST_AUTO_TEST_CASE(a_backup_with_no_snapshot_is_refused) {
    // There is nothing to seed from: a log without a snapshot is the history of
    // a cluster whose configuration is exactly what seed restore discards.
    const mock_object_store store;
    const std::string p = std::string(k_source_prefix) + "/";
    store.seed(p + "term", "5");
    store.seed(p + log_key(1), entry_json(5, 1));
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    try {
        std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha"});
        BOOST_FAIL("expected a snapshot-less backup to be refused");
    } catch (const std::runtime_error& err) {
        BOOST_TEST(std::string(err.what()).find("no snapshot") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(an_empty_node_set_is_refused) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    BOOST_CHECK_THROW(std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {}),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(every_target_is_checked_before_any_is_written) {
    // A refusal on the third node must not leave the first two seeded into a
    // cluster that will never reach a quorum — so all targets are prepared
    // before any is written.
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed("fresh/gamma/term", "99");

    BOOST_CHECK_THROW(std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"),
                                                        {"alpha", "beta", "gamma"}),
                      std::runtime_error);
    BOOST_TEST(relative_keys(store, "fresh/alpha").empty());
    BOOST_TEST(relative_keys(store, "fresh/beta").empty());
    BOOST_TEST(store.body("fresh/gamma/term") == "99");
}

BOOST_AUTO_TEST_CASE(a_failing_verify_aborts_the_seed_before_anything_is_written) {
    const mock_object_store store;
    seed_source(store);
    const object_store_backup<mock_object_store> backup{store};
    std::ignore = backup.create(source_ref(), dest_ref(), {.backup_id = "b1"});

    store.seed(std::string(k_dest_prefix) + "/b1/objects/snapshot", "tampered");

    BOOST_CHECK_THROW(
        std::ignore = backup.restore_seed(dest_ref(), "b1", target_ref("fresh"), {"alpha"}),
        std::runtime_error);
    BOOST_TEST(relative_keys(store, "fresh/alpha").empty());
}

BOOST_AUTO_TEST_SUITE_END()
