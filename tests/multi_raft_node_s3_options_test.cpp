// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#define BOOST_TEST_MODULE multi_raft_node_s3_options_test
#include <boost/test/unit_test.hpp>

// `multi_raft_node --persistence s3`'s options: the `--s3-*` parser, its
// validation, the help text and the engine preflight
// (.kiro/specs/multi-raft-node-s3-persistence/ task 2, Requirements 1.1, 1.5,
// 2.1, 2.2, 3.1-3.5, 5.1-5.3, 8 and 10.1).
//
// Compiles cmd/multi_raft_node/config.cpp itself and reaches s3_store.hpp by
// path. Neither names the AWS SDK, so this runs in every build, with or
// without the S3 arm.

#include "config.hpp"
#include "s3_describe.hpp"
#include "s3_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

using namespace kythira::bench;

namespace {

auto contains(const std::string& haystack, const std::string& needle) -> bool {
    return haystack.find(needle) != std::string::npos;
}

/// Unsets the two endpoint variables for one case and restores them after,
/// so a developer's or CI's AWS_ENDPOINT_URL cannot change what is asserted.
struct endpoint_env_guard {
    std::vector<std::pair<std::string, std::string>> saved;
    endpoint_env_guard() {
        for (const char* name : {"AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_S3"}) {
            if (const char* value = std::getenv(name); value != nullptr) {
                saved.emplace_back(name, value);
            }
            ::unsetenv(name);
        }
    }
    ~endpoint_env_guard() {
        for (const char* name : {"AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_S3"}) {
            ::unsetenv(name);
        }
        for (const auto& [name, value] : saved) {
            ::setenv(name.c_str(), value.c_str(), 1);
        }
    }
    endpoint_env_guard(const endpoint_env_guard&) = delete;
    auto operator=(const endpoint_env_guard&) -> endpoint_env_guard& = delete;
};

/// The ports and peer every host needs, then `extra`.
auto parse(std::initializer_list<std::string> extra) -> node_options {
    std::vector<std::string> args{"multi_raft_node",
                                  "--raft-port",
                                  "9001",
                                  "--data-port",
                                  "9002",
                                  "--control-port",
                                  "9003",
                                  "--peer",
                                  "1=http://127.0.0.1:9001"};
    args.insert(args.end(), extra);
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_node_options(static_cast<int>(argv.size()), argv.data());
}

/// `--persistence s3` with the two required flags, then `extra`.
auto parse_s3(std::initializer_list<std::string> extra) -> node_options {
    std::vector<std::string> args{"--persistence", "s3", "--s3-bucket", "b", "--s3-prefix", "run"};
    args.insert(args.end(), extra);
    std::vector<std::string> all{"multi_raft_node",
                                 "--raft-port",
                                 "9001",
                                 "--data-port",
                                 "9002",
                                 "--control-port",
                                 "9003",
                                 "--peer",
                                 "1=http://127.0.0.1:9001"};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : all) {
        argv.push_back(arg.data());
    }
    return parse_node_options(static_cast<int>(argv.size()), argv.data());
}

template<typename F> auto error_of(F&& f) -> std::string {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

BOOST_AUTO_TEST_SUITE(s3_flags)

BOOST_AUTO_TEST_CASE(defaults_follow_the_requirements) {
    endpoint_env_guard env;
    const auto opt = parse_s3({});
    BOOST_TEST((opt._persistence == persistence_mode::s3));
    BOOST_TEST(opt._s3.bucket == "b");
    BOOST_TEST(opt._s3.prefix == "run");
    BOOST_TEST(opt._s3.region.empty());
    BOOST_TEST(opt._s3.endpoint.empty());
    BOOST_TEST(opt._s3.timeout.count() == 30000);
    BOOST_TEST((opt._s3.fencing == s3_fencing::cas));
    // Requirement 5.2: combined, unlike the struct; everything else the
    // struct's own default.
    const kythira::object_persistence_options struct_default;
    const auto& e = opt._s3.engine;
    BOOST_TEST((e.hard_state == kythira::hard_state_layout::combined));
    BOOST_TEST((struct_default.hard_state == kythira::hard_state_layout::separate));
    BOOST_TEST((e.log == struct_default.log));
    BOOST_TEST(e.max_segment_entries == struct_default.max_segment_entries);
    BOOST_TEST(e.max_segment_bytes == struct_default.max_segment_bytes);
    BOOST_TEST(e.write_behind == false);
    BOOST_TEST(e.append_concurrency == struct_default.append_concurrency);
    BOOST_TEST(e.write_retries == struct_default.write_retries);
    BOOST_TEST(e.snapshot_retention == struct_default.snapshot_retention);
    BOOST_TEST(e.verify_checksums == struct_default.verify_checksums);
    BOOST_TEST(e.max_object_bytes == struct_default.max_object_bytes);
}

BOOST_AUTO_TEST_CASE(every_flag_reaches_its_field) {
    endpoint_env_guard env;
    const auto opt = parse_s3({"--s3-region",
                               "eu-west-3",
                               "--s3-endpoint",
                               "http://localhost:4566/",
                               "--s3-timeout",
                               "2500",
                               "--s3-fencing",
                               "none",
                               "--s3-hard-state",
                               "separate",
                               "--s3-log",
                               "segmented",
                               "--s3-max-segment-entries",
                               "50",
                               "--s3-max-segment-bytes",
                               "4096",
                               "--s3-write-behind",
                               "1",
                               "--s3-append-concurrency",
                               "4",
                               "--s3-write-retries",
                               "3",
                               "--s3-snapshot-retention",
                               "2",
                               "--s3-verify-checksums",
                               "0",
                               "--s3-max-object-bytes",
                               "1048576"});
    BOOST_TEST(opt._s3.region == "eu-west-3");
    BOOST_TEST(opt._s3.endpoint == "http://localhost:4566");  // normalised
    BOOST_TEST(opt._s3.timeout.count() == 2500);
    BOOST_TEST((opt._s3.fencing == s3_fencing::none));
    const auto& e = opt._s3.engine;
    BOOST_TEST((e.hard_state == kythira::hard_state_layout::separate));
    BOOST_TEST((e.log == kythira::log_layout::segmented));
    BOOST_TEST(e.max_segment_entries == 50U);
    BOOST_TEST(e.max_segment_bytes == 4096U);
    BOOST_TEST(e.write_behind == true);
    BOOST_TEST(e.append_concurrency == 4U);
    BOOST_TEST(e.write_retries == 3U);
    BOOST_TEST(e.snapshot_retention == 2U);
    BOOST_TEST(e.verify_checksums == false);
    BOOST_TEST(e.max_object_bytes == 1048576U);
}

BOOST_AUTO_TEST_CASE(enum_spellings_are_the_documented_ones) {
    endpoint_env_guard env;
    BOOST_TEST((parse_s3({"--s3-fencing", "cas"})._s3.fencing == s3_fencing::cas));
    BOOST_TEST((parse_s3({"--s3-hard-state", "combined"})._s3.engine.hard_state ==
                kythira::hard_state_layout::combined));
    BOOST_TEST(
        (parse_s3({"--s3-log", "per-entry"})._s3.engine.log == kythira::log_layout::per_entry));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-log", "per_entry"}); }),
                        "per-entry|segmented"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-fencing", "compare_and_swap"}); }),
                        "none|cas"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-hard-state", "both"}); }),
                        "separate|combined"));
}

BOOST_AUTO_TEST_CASE(bad_values_are_refused) {
    endpoint_env_guard env;
    BOOST_TEST(
        contains(error_of([] { (void)parse_s3({"--s3-write-behind", "2"}); }), "must be 0 or 1"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-verify-checksums", "yes"}); }),
                        "not a number"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-append-concurrency", "x"}); }),
                        "not a number"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-timeout", "0"}); }),
                        "--s3-timeout must be at least 1 ms"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-region", ""}); }),
                        "--s3-region needs a non-empty value"));
    BOOST_TEST(
        contains(error_of([] { (void)parse_s3({"--s3-prefix", "///"}); }), "--s3-prefix is empty"));
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--s3-bogus", "1"}); }),
                        "unknown option --s3-bogus"));
}

BOOST_AUTO_TEST_CASE(trailing_slashes_leave_the_prefix) {
    endpoint_env_guard env;
    BOOST_TEST(parse_s3({"--s3-prefix", "runs/r1//"})._s3.prefix == "runs/r1");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(s3_validation)

BOOST_AUTO_TEST_CASE(bucket_and_prefix_are_required) {
    endpoint_env_guard env;
    BOOST_TEST(
        contains(error_of([] { (void)parse({"--persistence", "s3", "--s3-prefix", "run"}); }),
                 "requires --s3-bucket"));
    BOOST_TEST(contains(error_of([] { (void)parse({"--persistence", "s3", "--s3-bucket", "b"}); }),
                        "requires --s3-prefix"));
}

BOOST_AUTO_TEST_CASE(data_dir_is_refused_under_s3) {
    endpoint_env_guard env;
    BOOST_TEST(contains(error_of([] { (void)parse_s3({"--data-dir", "/tmp/x"}); }),
                        "--data-dir is not used under --persistence s3"));
}

BOOST_AUTO_TEST_CASE(s3_flags_are_refused_under_other_modes) {
    endpoint_env_guard env;
    BOOST_TEST(contains(error_of([] { (void)parse({"--s3-bucket", "b"}); }),
                        "--s3-bucket applies to --persistence s3 only, and this host is "
                        "--persistence memory"));
    BOOST_TEST(contains(error_of([] {
                            (void)parse({"--persistence", "file-barrier", "--data-dir", "/tmp/x",
                                         "--s3-endpoint", "http://localhost:4566"});
                        }),
                        "--s3-endpoint applies to --persistence s3 only, and this host is "
                        "--persistence file-barrier"));
}

BOOST_AUTO_TEST_CASE(file_modes_still_need_a_data_dir) {
    endpoint_env_guard env;
    BOOST_TEST(contains(error_of([] { (void)parse({"--persistence", "file-buffered"}); }),
                        "requires --data-dir"));
    BOOST_TEST((parse({})._persistence == persistence_mode::memory));
}

BOOST_AUTO_TEST_CASE(endpoint_uses_the_shared_validator) {
    endpoint_env_guard env;
    const auto schemeless = error_of([] { (void)parse_s3({"--s3-endpoint", "localhost:4566"}); });
    BOOST_TEST(contains(schemeless, "--s3-endpoint 'localhost:4566'"));
    BOOST_TEST(contains(schemeless, "needs a scheme"));
    BOOST_TEST(
        contains(error_of([] { (void)parse_s3({"--s3-endpoint", "http://minio:9000/path"}); }),
                 "path is not accepted"));
    const auto userinfo =
        error_of([] { (void)parse_s3({"--s3-endpoint", "http://u:hunter2@minio:9000"}); });
    BOOST_TEST(contains(userinfo, "--s3-endpoint"));
    BOOST_TEST(!contains(userinfo, "hunter2"));
}

BOOST_AUTO_TEST_CASE(plaintext_endpoint_off_loopback_warns) {
    endpoint_env_guard env;
    const auto opt =
        parse_s3({"--s3-endpoint", "http://minio.internal:9000", "--election-timeout-min", "1000"});
    BOOST_REQUIRE_EQUAL(opt._s3.warnings.size(), 1U);
    BOOST_TEST(contains(opt._s3.warnings.front(), "--s3-endpoint"));
    BOOST_TEST(contains(opt._s3.warnings.front(), "unencrypted"));
}

BOOST_AUTO_TEST_CASE(ignored_endpoint_variables_warn_by_name_only) {
    endpoint_env_guard env;
    ::setenv("AWS_ENDPOINT_URL", "https://secret-cache.example", 1);
    const auto opt = parse_s3({"--election-timeout-min", "1000"});
    BOOST_REQUIRE_EQUAL(opt._s3.warnings.size(), 1U);
    BOOST_TEST(contains(opt._s3.warnings.front(), "AWS_ENDPOINT_URL is set but ignored"));
    BOOST_TEST(contains(opt._s3.warnings.front(), "--s3-endpoint"));
    BOOST_TEST(!contains(opt._s3.warnings.front(), "secret-cache"));
    // An explicit endpoint makes the variable moot, so no warning.
    BOOST_TEST(
        parse_s3({"--s3-endpoint", "http://localhost:4566", "--election-timeout-min", "1000"})
            ._s3.warnings.empty());
}

BOOST_AUTO_TEST_CASE(short_election_timeout_warns_and_does_not_refuse) {
    endpoint_env_guard env;
    const auto opt = parse_s3({"--s3-endpoint", "http://localhost:4566"});
    BOOST_REQUIRE_EQUAL(opt._s3.warnings.size(), 1U);
    const auto& warning = opt._s3.warnings.front();
    BOOST_TEST(contains(warning, "--election-timeout-min 150 ms is below 1000 ms"));
    BOOST_TEST(contains(warning, "Election-timeout sizing"));
    BOOST_TEST(contains(warning, "doc/cloud_object_persistence.md"));
    BOOST_TEST(k_s3_min_election_timeout.count() == 1000);
    BOOST_TEST(
        parse_s3({"--s3-endpoint", "http://localhost:4566", "--election-timeout-min", "1000"})
            ._s3.warnings.empty());
}

BOOST_AUTO_TEST_CASE(config_file_takes_the_same_flags) {
    endpoint_env_guard env;
    const auto path = std::filesystem::temp_directory_path() /
                      ("multi_raft_node_s3_options_test_" + std::to_string(::getpid()) + ".conf");
    {
        std::ofstream out(path);
        out << "persistence=s3\n"
               "s3-bucket=from-file\n"
               "s3-prefix=runs/file/\n"
               "s3-log=segmented\n"
               "s3-write-behind=1\n";
    }
    const auto opt = parse({"--config", path.string()});
    std::filesystem::remove(path);
    BOOST_TEST((opt._persistence == persistence_mode::s3));
    BOOST_TEST(opt._s3.bucket == "from-file");
    BOOST_TEST(opt._s3.prefix == "runs/file");
    BOOST_TEST((opt._s3.engine.log == kythira::log_layout::segmented));
    BOOST_TEST(opt._s3.engine.write_behind);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(s3_derived)

BOOST_AUTO_TEST_CASE(host_prefix_appends_the_node_id) {
    endpoint_env_guard env;
    auto opt = parse_s3({"--s3-prefix", "runs/r1/"});
    BOOST_TEST(s3_host_prefix(opt) == "runs/r1/nodes/" + kythira::host_node_id_text(opt._node_id));
}

BOOST_AUTO_TEST_CASE(owner_id_is_node_id_at_own_url_under_cas_only) {
    endpoint_env_guard env;
    const auto cas = parse_s3({});
    BOOST_TEST(s3_engine_options(cas).owner_id ==
               kythira::host_node_id_text(cas._node_id) + "@http://127.0.0.1:9001");
    BOOST_TEST(s3_engine_options(parse_s3({"--s3-fencing", "none"})).owner_id.empty());
}

BOOST_AUTO_TEST_CASE(timeout_rounds_up_to_whole_seconds) {
    BOOST_TEST(s3_api_timeout(std::chrono::milliseconds{1}).count() == 1);
    BOOST_TEST(s3_api_timeout(std::chrono::milliseconds{30000}).count() == 30);
    BOOST_TEST(s3_api_timeout(std::chrono::milliseconds{30001}).count() == 31);
}

BOOST_AUTO_TEST_CASE(preflight_passes_the_defaults_under_both_fencing_modes) {
    endpoint_env_guard env;
    BOOST_CHECK_NO_THROW(preflight_s3_options(parse_s3({})));
    BOOST_CHECK_NO_THROW(preflight_s3_options(parse_s3({"--s3-fencing", "none"})));
    BOOST_CHECK_NO_THROW(preflight_s3_options(parse_s3(
        {"--s3-log", "segmented", "--s3-max-segment-entries", "16", "--s3-write-behind", "1"})));
}

// Requirement 5.3: the engine's rule and the engine's message, not a copy.
BOOST_AUTO_TEST_CASE(preflight_reports_the_engines_own_message) {
    endpoint_env_guard env;
    const auto one_entry_segments = error_of([] {
        preflight_s3_options(parse_s3({"--s3-log", "segmented", "--s3-max-segment-entries", "1"}));
    });
    BOOST_TEST(contains(one_entry_segments, "object_store_persistence_engine"));
    BOOST_TEST(contains(one_entry_segments, "max_segment_entries"));
    BOOST_TEST(contains(
        error_of([] { preflight_s3_options(parse_s3({"--s3-max-segment-entries", "50"})); }),
        "object_store_persistence_engine"));
    BOOST_TEST(
        contains(error_of([] { preflight_s3_options(parse_s3({"--s3-append-concurrency", "0"})); }),
                 "append_concurrency must be at least 1"));
    BOOST_TEST(
        contains(error_of([] { preflight_s3_options(parse_s3({"--s3-snapshot-retention", "0"})); }),
                 "snapshot_retention must be at least 1"));
}

BOOST_AUTO_TEST_CASE(describe_names_the_prefix_and_every_option) {
    endpoint_env_guard env;
    const auto opt = parse_s3({"--s3-endpoint", "http://localhost:4566", "--s3-write-behind", "1"});
    const auto line = describe_s3(opt, s3_host_prefix(opt));
    BOOST_TEST(contains(line, "s3://b/run/nodes/"));
    BOOST_TEST(contains(line, "endpoint=http://localhost:4566"));
    BOOST_TEST(contains(line, "fencing=cas"));
    BOOST_TEST(contains(line, "hard_state=combined"));
    BOOST_TEST(contains(line, "log=per-entry"));
    BOOST_TEST(contains(line, "write_behind=1"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(s3_help)

BOOST_AUTO_TEST_CASE(help_lists_s3_and_every_flag) {
    const auto text = node_usage();
    BOOST_TEST(contains(text, "memory | file-buffered | file-barrier | s3"));
    for (const char* flag :
         {"--s3-bucket", "--s3-prefix", "--s3-region", "--s3-endpoint", "--s3-timeout",
          "--s3-fencing", "--s3-hard-state", "--s3-log", "--s3-max-segment-entries",
          "--s3-max-segment-bytes", "--s3-write-behind", "--s3-append-concurrency",
          "--s3-write-retries", "--s3-snapshot-retention", "--s3-verify-checksums",
          "--s3-max-object-bytes"}) {
        BOOST_TEST(contains(text, flag), "help is missing " << flag);
    }
    BOOST_TEST(contains(text, "AWS_DEFAULT_REGION"));
#ifdef KYTHIRA_NODE_HAS_S3_PERSISTENCE
    BOOST_TEST(!contains(text, "UNAVAILABLE IN THIS BUILD"));
#else
    BOOST_TEST(contains(text, "UNAVAILABLE IN THIS BUILD"));
    BOOST_TEST(contains(text, "CONFIG_AWS_S3_PERSISTENCE=y"));
#endif
}

BOOST_AUTO_TEST_SUITE_END()
