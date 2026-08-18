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
