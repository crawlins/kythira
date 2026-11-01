// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The runner behind every draft finding test in tests/docker_chaos/findings/
// (cairn-chaos-testing Requirement 6, task 12). `chaos_ctl report` writes a
// test that holds a finding's schedule as a table of steps and calls
// check(); check() replays it exactly as `chaos_ctl replay` does (the same
// chaos_ctl::real_attempt), on a fresh three-node cluster of the docker/
// compose file, and fails if any attempt breaks one of the finding's
// invariants again.
//
// Environment:
//   KYTHIRA_CHAOS_NODE_IMAGE      the image to test (default :dev). The image
//                                 the finding was made on must fail; the fix
//                                 must pass.
//   KYTHIRA_CHAOS_WORKLOAD        the chaos_workload binary (required; the
//                                 docker-chaos-findings target sets it).
//   KYTHIRA_COMPOSE_FILE          as for every scenario test.
//   KYTHIRA_CAIRN_FAILPOINTS      the failpoint allowlist (default: the
//                                 source tree's docker/cairn/ copy).
//   KYTHIRA_CHAOS_FINDING_REPEAT  attempts, overriding the finding's own.
//   KYTHIRA_CHAOS_ARTIFACT_DIR    keeps every attempt's run directory.

#include "attempt.hpp"
#include "harness.hpp"
#include "replay.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"

#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef KYTHIRA_SOURCE_DIR
#define KYTHIRA_SOURCE_DIR "."
#endif

namespace chaos_finding {

/// One call of the schedule: when (ms after the first call), its seq in the
/// run's log, the tool and its arguments as JSON.
struct step {
    std::int64_t offset_ms;
    std::int64_t seq;
    std::string_view tool;
    std::string_view args;
};

struct finding {
    std::string id;
    std::vector<std::string> invariants;
    int attempts{1};
    /// The image the finding was made on, for the failure message.
    std::string found_on_image;
};

[[nodiscard]] inline auto env(const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string{};
}

/// The steps as the call records replay takes.
[[nodiscard]] inline auto to_calls(std::span<const step> schedule)
    -> std::vector<chaos_ctl::call_record> {
    std::vector<chaos_ctl::call_record> out;
    out.reserve(schedule.size());
    for (const auto& s : schedule) {
        chaos_ctl::call_record r;
        r.seq = s.seq;
        r.tool = std::string(s.tool);
        r.args = boost::json::parse(s.args).as_object();
        r.started_mono_ms = s.offset_ms;
        r.status = "ok";
        r.started_line = {{"seq", r.seq},
                          {"status", "started"},
                          {"tool", r.tool},
                          {"args", r.args},
                          {"mono_ms", r.started_mono_ms}};
        out.push_back(std::move(r));
    }
    return out;
}

[[nodiscard]] inline auto config() -> chaos_ctl::run_config {
    chaos_ctl::run_config cfg;
    cfg.compose_file = docker_chaos::default_compose_file();
    cfg.nodes = docker_chaos::k_node_map;
    if (auto image = env("KYTHIRA_CHAOS_NODE_IMAGE"); !image.empty()) {
        cfg.image = image;
    }
    cfg.kythira_commit = "draft-test";
    cfg.workload_binary = env("KYTHIRA_CHAOS_WORKLOAD");
    return cfg;
}

[[nodiscard]] inline auto attempts(const finding& f) -> int {
    if (auto n = env("KYTHIRA_CHAOS_FINDING_REPEAT"); !n.empty()) {
        return std::max(1, std::stoi(n));
    }
    return f.attempts;
}

/// Replays `schedule` up to the finding's attempt count, stopping at the
/// first attempt that breaks one of its invariants, and fails if any did.
inline void check(const finding& f, std::span<const step> schedule) {
    namespace fs = std::filesystem;
    const auto cfg = config();
    BOOST_REQUIRE_MESSAGE(!cfg.workload_binary.empty() && fs::exists(cfg.workload_binary),
                          "KYTHIRA_CHAOS_WORKLOAD must name the chaos_workload binary");
    auto fp_path = env("KYTHIRA_CAIRN_FAILPOINTS");
    if (fp_path.empty()) {
        fp_path = std::string(KYTHIRA_SOURCE_DIR) + "/docker/cairn/failpoints.allowlist";
    }
    const auto failpoints = chaos_ctl::load_failpoints(fp_path);

    const auto artifacts = env("KYTHIRA_CHAOS_ARTIFACT_DIR");
    const fs::path out_dir = (artifacts.empty() ? fs::temp_directory_path() : fs::path(artifacts)) /
                             ("chaos_finding_" + f.id + "_" + std::to_string(::getpid()));

    const std::set<std::string> recorded(f.invariants.begin(), f.invariants.end());
    const auto calls = to_calls(schedule);
    const auto n = attempts(f);
    BOOST_TEST_MESSAGE("finding " << f.id << ": " << calls.size() << " calls, up to " << n
                                  << " attempt(s) on " << cfg.image);
    const auto r = chaos_ctl::replay::repeat(
        calls, 0, recorded, n, chaos_ctl::real_attempt(cfg, out_dir, failpoints, false),
        /*stop_on_first=*/true);
    BOOST_TEST_MESSAGE(boost::json::serialize(r.to_json(recorded)));
    BOOST_TEST(r.reproduced == 0, "finding " << f.id << " reproduced on " << cfg.image << " in "
                                             << r.reproduced << " of " << r.attempts
                                             << " attempt(s) (found on " << f.found_on_image
                                             << "); run directories under " << out_dir);
    if (r.reproduced == 0 && artifacts.empty()) {
        fs::remove_all(out_dir);
    }
}

}  // namespace chaos_finding
