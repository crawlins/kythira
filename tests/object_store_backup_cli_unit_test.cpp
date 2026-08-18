#define BOOST_TEST_MODULE object_store_backup_cli_unit_test
#include <boost/test/unit_test.hpp>

#include <raft/object_store_backup_cli.hpp>

#include "mock_object_store.hpp"

// Unit coverage for the logic behind `cmd/raft_object_backup`
// (cloud-object-persistence Requirements 10.6 and 16.4, task 14).
//
// Registered unconditionally, like the engine and backup suites and for the
// same reason (Requirement 16.3): nothing here names a provider or includes an
// SDK. That is also what makes task 14's verify bar reachable — "a full
// create → list → verify → restore-clone cycle works against
// `mock_object_store` in a build with zero cloud providers" is only a testable
// claim because the CLI's verb dispatch is separable from its provider
// selection.
//
// What is NOT covered here, and deliberately: the `--provider` table itself
// lives in `cmd/raft_object_backup/main.cpp`, because it is the one part that
// depends on which providers the build contains. It is exercised by running the
// built binary, not from a unit test that would have to fake the build
// configuration to say anything.

#include <sstream>
#include <string>
#include <vector>

using namespace kythira;

namespace {

constexpr const char* k_bucket = "kythira-bucket";

auto log_key(std::uint64_t index) -> std::string {
    std::string digits = std::to_string(index);
    return std::string("log/") + std::string(20 - digits.size(), '0') + digits;
}

auto entry_json(std::uint64_t term, std::uint64_t index) -> std::string {
    return R"({"term":)" + std::to_string(term) + R"(,"index":)" + std::to_string(index) +
           R"(,"command":"","type":0})";
}

auto snapshot_json(std::uint64_t last_index, std::uint64_t last_term) -> std::string {
    return R"({"last_included_index":)" + std::to_string(last_index) + R"(,"last_included_term":)" +
           std::to_string(last_term) +
           R"(,"state":"aGk=","nodes":["n1","n2","n3"],"is_joint_consensus":false})";
}

auto seed_source(const mock_object_store& store) -> void {
    store.seed("node-1/term", "5");
    store.seed("node-1/voted_for", "n2");
    store.seed("node-1/snapshot", snapshot_json(3, 4));
    for (std::uint64_t i = 4; i <= 6; ++i) {
        store.seed("node-1/" + log_key(i), entry_json(5, i));
    }
}

/// Runs the CLI the way `main` does — from an argv vector — and captures both
/// streams. Going through the real parser rather than filling in a
/// `backup_cli_args` by hand is the point: an operator types arguments, and a
/// test that skipped the parser would not notice a verb whose required options
/// were never enforced.
struct cli_run {
    int exit_code{0};
    std::string out;
    std::string err;
};

auto run_cli(const mock_object_store& store, const std::vector<std::string>& argv_strings)
    -> cli_run {
    std::vector<const char*> argv;
    argv.reserve(argv_strings.size());
    for (const auto& arg : argv_strings) {
        argv.push_back(arg.c_str());
    }

    cli_run result;
    std::ostringstream out;
    std::ostringstream err;
    try {
        const auto args = parse_backup_cli_args(static_cast<int>(argv.size()), argv.data());
        result.exit_code = run_backup_cli(store, args, out, err);
    } catch (const backup_cli_usage_error& usage) {
        err << "raft_object_backup: " << usage.what() << "\n";
        result.exit_code = 1;
    }
    result.out = out.str();
    result.err = err.str();
    return result;
}

}  // namespace

// ── Argument parsing ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(backup_cli_parsing)

BOOST_AUTO_TEST_CASE(each_verb_parses) {
    const std::vector<std::pair<std::string, backup_verb>> verbs{
        {"create", backup_verb::create},
        {"list", backup_verb::list},
        {"verify", backup_verb::verify},
        {"restore-clone", backup_verb::restore_clone},
        {"restore-seed", backup_verb::restore_seed},
    };
    for (const auto& [name, expected] : verbs) {
        std::vector<std::string> args{"raft_object_backup", name, "--dest-bucket", "b",
                                      "--dest-prefix",      "p"};
        // Fill in whatever else each verb requires; the point of this case is
        // the verb mapping, not the option checking.
        for (const auto& extra :
             {"--source-bucket", "sb", "--source-prefix", "sp", "--backup-id", "id",
              "--target-bucket", "tb", "--target-prefix", "tp", "--nodes", "a,b"}) {
            args.emplace_back(extra);
        }
        std::vector<const char*> argv;
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        const auto parsed = parse_backup_cli_args(static_cast<int>(argv.size()), argv.data());
        BOOST_TEST((parsed.verb == expected));
    }
}

BOOST_AUTO_TEST_CASE(an_unknown_verb_names_the_alternatives) {
    const char* argv[] = {"raft_object_backup", "restore"};
    try {
        std::ignore = parse_backup_cli_args(2, argv);
        BOOST_FAIL("expected an unknown verb to be refused");
    } catch (const backup_cli_usage_error& err) {
        const std::string what = err.what();
        BOOST_TEST(what.find("restore-clone") != std::string::npos);
        BOOST_TEST(what.find("restore-seed") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(each_verbs_required_options_are_enforced) {
    // Checked at parse time rather than at dispatch, so an operator learns
    // about a missing --backup-id before the tool has talked to anything.
    const char* verify_missing_id[] = {"raft_object_backup", "verify", "--dest-bucket", "b",
                                       "--dest-prefix",      "p"};
    BOOST_CHECK_THROW(std::ignore = parse_backup_cli_args(6, verify_missing_id),
                      backup_cli_usage_error);

    const char* create_missing_source[] = {"raft_object_backup", "create", "--dest-bucket", "b",
                                           "--dest-prefix",      "p"};
    BOOST_CHECK_THROW(std::ignore = parse_backup_cli_args(6, create_missing_source),
                      backup_cli_usage_error);

    const char* clone_missing_target[] = {"raft_object_backup", "restore-clone",
                                          "--dest-bucket",      "b",
                                          "--dest-prefix",      "p",
                                          "--backup-id",        "id"};
    BOOST_CHECK_THROW(std::ignore = parse_backup_cli_args(8, clone_missing_target),
                      backup_cli_usage_error);
}

BOOST_AUTO_TEST_CASE(restore_seed_demands_the_new_node_set) {
    // It is building a new cluster's membership, and that is not something the
    // tool can infer from the backup — the whole point of seed restore is that
    // the old configuration is discarded.
    const char* argv[] = {
        "raft_object_backup", "restore-seed", "--dest-bucket",   "b",  "--dest-prefix",   "p",
        "--backup-id",        "id",           "--target-bucket", "tb", "--target-prefix", "tp"};
    try {
        std::ignore = parse_backup_cli_args(12, argv);
        BOOST_FAIL("expected restore-seed without --nodes to be refused");
    } catch (const backup_cli_usage_error& err) {
        BOOST_TEST(std::string(err.what()).find("--nodes") != std::string::npos);
    }
}

BOOST_AUTO_TEST_CASE(node_lists_split_on_commas_and_ignore_empties) {
    const char* argv[] = {"raft_object_backup",
                          "restore-seed",
                          "--dest-bucket",
                          "b",
                          "--dest-prefix",
                          "p",
                          "--backup-id",
                          "id",
                          "--target-bucket",
                          "tb",
                          "--target-prefix",
                          "tp",
                          "--nodes",
                          "alpha,,beta,"};
    const auto parsed = parse_backup_cli_args(14, argv);
    BOOST_REQUIRE_EQUAL(parsed.nodes.size(), 2U);
    BOOST_TEST(parsed.nodes[0] == "alpha");
    BOOST_TEST(parsed.nodes[1] == "beta");
}

BOOST_AUTO_TEST_CASE(an_option_with_no_value_is_refused) {
    const char* argv[] = {"raft_object_backup", "list", "--dest-bucket"};
    BOOST_CHECK_THROW(std::ignore = parse_backup_cli_args(3, argv), backup_cli_usage_error);
}

BOOST_AUTO_TEST_CASE(an_unknown_option_is_refused_rather_than_ignored) {
    // Silently ignoring an unrecognised flag is how an operator ends up
    // believing they passed --force when they typed --forse.
    const char* argv[] = {"raft_object_backup", "list", "--dest-bucket", "b",
                          "--dest-prefix",      "p",    "--forse"};
    BOOST_CHECK_THROW(std::ignore = parse_backup_cli_args(7, argv), backup_cli_usage_error);
}

BOOST_AUTO_TEST_SUITE_END()

// ── The verify bar: a full cycle with zero providers ─────────────────────────

BOOST_AUTO_TEST_SUITE(backup_cli_full_cycle)

BOOST_AUTO_TEST_CASE(create_list_verify_restore_clone_against_the_mock) {
    const mock_object_store store;
    seed_source(store);

    // create
    const auto created =
        run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket, "--dest-prefix",
                        "backups", "--source-bucket", k_bucket, "--source-prefix", "node-1",
                        "--backup-id", "b1", "--quiesced"});
    BOOST_TEST(created.exit_code == 0);
    BOOST_TEST(created.out.find("created b1") != std::string::npos);
    BOOST_TEST(created.out.find("quiesced  declared") != std::string::npos);

    // list
    const auto listed = run_cli(store, {"raft_object_backup", "list", "--dest-bucket", k_bucket,
                                        "--dest-prefix", "backups"});
    BOOST_TEST(listed.exit_code == 0);
    BOOST_TEST(listed.out.find("b1") != std::string::npos);
    BOOST_TEST(listed.out.find("6 objects") != std::string::npos);

    // verify
    const auto verified = run_cli(store, {"raft_object_backup", "verify", "--dest-bucket", k_bucket,
                                          "--dest-prefix", "backups", "--backup-id", "b1"});
    BOOST_TEST(verified.exit_code == 0);
    BOOST_TEST(verified.out.find("no problems") != std::string::npos);

    // restore-clone
    const auto restored =
        run_cli(store, {"raft_object_backup", "restore-clone", "--dest-bucket", k_bucket,
                        "--dest-prefix", "backups", "--backup-id", "b1", "--target-bucket",
                        k_bucket, "--target-prefix", "node-1-restored"});
    BOOST_TEST(restored.exit_code == 0);
    BOOST_TEST(restored.out.find("objects written  6") != std::string::npos);
    BOOST_TEST(restored.out.find("split-brain") != std::string::npos);

    // …and the restored prefix really holds the state, which is the only thing
    // that makes the four exit codes above mean anything.
    BOOST_TEST(store.body("node-1-restored/term") == "5");
    BOOST_TEST(store.body("node-1-restored/voted_for") == "n2");
    BOOST_TEST(store.body("node-1-restored/" + log_key(6)) == entry_json(5, 6));
}

BOOST_AUTO_TEST_CASE(a_create_that_was_not_declared_quiesced_says_so) {
    // The tool records the claim and does not believe it; where no claim was
    // made at all, it says the copy may be a smear rather than staying silent.
    const mock_object_store store;
    seed_source(store);
    const auto created = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                         "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                         "--source-prefix", "node-1", "--backup-id", "b1"});
    BOOST_TEST(created.exit_code == 0);
    BOOST_TEST(created.out.find("not declared") != std::string::npos);
    BOOST_TEST(created.out.find("run verify") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_empty_destination_lists_nothing_and_succeeds) {
    const mock_object_store store;
    const auto listed = run_cli(store, {"raft_object_backup", "list", "--dest-bucket", k_bucket,
                                        "--dest-prefix", "backups"});
    BOOST_TEST(listed.exit_code == 0);
    BOOST_TEST(listed.out.find("no finished backups") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

// ── Exit codes ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(backup_cli_exit_codes)

BOOST_AUTO_TEST_CASE(verify_finding_problems_exits_three_not_two) {
    // The distinction this whole exit-code scheme exists for: "I could not
    // check this backup" and "I checked it and it is broken" call for different
    // operator responses, and a script gating a restore on verify has to tell
    // them apart. The first is worth retrying; the second never is.
    const mock_object_store store;
    seed_source(store);
    std::ignore = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                  "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                  "--source-prefix", "node-1", "--backup-id", "b1"});
    store.remove(std::string("backups/b1/objects/") + log_key(5));

    const auto verified = run_cli(store, {"raft_object_backup", "verify", "--dest-bucket", k_bucket,
                                          "--dest-prefix", "backups", "--backup-id", "b1"});
    BOOST_TEST(verified.exit_code == 3);
    BOOST_TEST(verified.out.find("missing_object") != std::string::npos);
    // Every problem is printed, not just the first — verify is the diagnostic.
    BOOST_TEST(verified.out.find("log_gap") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_failed_operation_exits_two) {
    // No such backup: the store throws, and that is "could not check", not
    // "checked and broken".
    const mock_object_store store;
    const auto verified = run_cli(store, {"raft_object_backup", "verify", "--dest-bucket", k_bucket,
                                          "--dest-prefix", "backups", "--backup-id", "nope"});
    BOOST_TEST(verified.exit_code == 2);
    BOOST_TEST(verified.err.find("no manifest") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_refused_restore_exits_two_and_explains) {
    const mock_object_store store;
    seed_source(store);
    std::ignore = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                  "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                  "--source-prefix", "node-1", "--backup-id", "b1"});
    store.seed("occupied/term", "99");

    const auto restored =
        run_cli(store, {"raft_object_backup", "restore-clone", "--dest-bucket", k_bucket,
                        "--dest-prefix", "backups", "--backup-id", "b1", "--target-bucket",
                        k_bucket, "--target-prefix", "occupied"});
    BOOST_TEST(restored.exit_code == 2);
    BOOST_TEST(restored.err.find("without force") != std::string::npos);
    BOOST_TEST(store.body("occupied/term") == "99");
}

BOOST_AUTO_TEST_CASE(force_lets_the_same_restore_through) {
    // The negative control for the case above: without it, a restore that
    // refused everything would look like working safety.
    const mock_object_store store;
    seed_source(store);
    std::ignore = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                  "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                  "--source-prefix", "node-1", "--backup-id", "b1"});
    store.seed("occupied/term", "99");

    const auto restored =
        run_cli(store, {"raft_object_backup", "restore-clone", "--dest-bucket", k_bucket,
                        "--dest-prefix", "backups", "--backup-id", "b1", "--target-bucket",
                        k_bucket, "--target-prefix", "occupied", "--force"});
    BOOST_TEST(restored.exit_code == 0);
    BOOST_TEST(restored.out.find("keys deleted") != std::string::npos);
    BOOST_TEST(store.body("occupied/term") == "5");
}

BOOST_AUTO_TEST_SUITE_END()

// ── restore-seed output ──────────────────────────────────────────────────────

BOOST_AUTO_TEST_SUITE(backup_cli_restore_seed)

BOOST_AUTO_TEST_CASE(seed_prints_exactly_what_the_operator_must_configure) {
    // Requirement 11.3. The tool writes the state; the operator writes the
    // deployment, and cannot do that without the node ids and prefixes.
    const mock_object_store store;
    seed_source(store);
    std::ignore = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                  "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                  "--source-prefix", "node-1", "--backup-id", "b1"});

    const auto seeded =
        run_cli(store, {"raft_object_backup", "restore-seed", "--dest-bucket", k_bucket,
                        "--dest-prefix", "backups", "--backup-id", "b1", "--target-bucket",
                        k_bucket, "--target-prefix", "fresh", "--nodes", "alpha,beta,gamma"});
    BOOST_TEST(seeded.exit_code == 0);
    for (const auto* node : {"alpha", "beta", "gamma"}) {
        BOOST_TEST(seeded.out.find(std::string("  ") + node + "  ") != std::string::npos);
        BOOST_TEST(seeded.out.find(std::string("fresh/") + node) != std::string::npos);
    }
    // And it says what was discarded, because a seeded cluster that an operator
    // believes carries the old log is worse than one they know is fresh.
    BOOST_TEST(seeded.out.find("log is empty") != std::string::npos);
    BOOST_TEST(seeded.out.find("unrelated to the old cluster") != std::string::npos);

    BOOST_TEST(store.body("fresh/alpha/term") == "4");
    BOOST_TEST(!store.has("fresh/alpha/voted_for"));
}

BOOST_AUTO_TEST_CASE(a_snapshotless_backup_is_refused_with_exit_two) {
    const mock_object_store store;
    store.seed("node-1/term", "5");
    store.seed("node-1/" + log_key(1), entry_json(5, 1));
    std::ignore = run_cli(store, {"raft_object_backup", "create", "--dest-bucket", k_bucket,
                                  "--dest-prefix", "backups", "--source-bucket", k_bucket,
                                  "--source-prefix", "node-1", "--backup-id", "b1"});

    const auto seeded =
        run_cli(store, {"raft_object_backup", "restore-seed", "--dest-bucket", k_bucket,
                        "--dest-prefix", "backups", "--backup-id", "b1", "--target-bucket",
                        k_bucket, "--target-prefix", "fresh", "--nodes", "alpha"});
    BOOST_TEST(seeded.exit_code == 2);
    BOOST_TEST(seeded.err.find("no snapshot") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
