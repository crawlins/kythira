// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// One replay attempt against a real cluster (.kiro/specs/cairn-chaos-testing/
// design §4): a fresh cluster on the run's image, the calls at their logged
// offsets, then the scripted end (stop the workload, heal, every checker).
// `chaos_ctl replay` and `minimize` use it, and so does every draft test the
// reporter writes (tests/docker_chaos/chaos_finding.hpp), so a finding's
// test and its replay cannot drift apart.

#include "replay.hpp"
#include "run_state.hpp"
#include "schedule_log.hpp"
#include "tools.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace chaos_ctl {

inline void real_fiu_send(docker_chaos::ChaosNode& n, const std::string& cmd) {
    docker_chaos::fiu::send_fiu_cmd(n.host(), n.fiu_port(), cmd);
}

/// Points every compose call this process makes at the run's image. The
/// compose files read it as ${KYTHIRA_CHAOS_NODE_IMAGE:-kythira-chaos-node:dev},
/// so a run on the seeded image (cairn-chaos-testing task 9) replays on it
/// rather than on whatever :dev happens to be.
inline void use_image(const run_config& cfg) {
    ::setenv("KYTHIRA_CHAOS_NODE_IMAGE", cfg.image.c_str(), 1);
}

inline void start_cluster(docker_chaos::ChaosCluster& cluster) {
    cluster.stop();
    cluster.start();
    cluster.wait_for_leader(std::chrono::seconds{30});
}

/// One replay attempt in its own run directory under `out_dir`.
[[nodiscard]] inline auto real_attempt(const run_config& cfg, const fs::path& out_dir,
                                       const std::set<std::string>& fps, bool keep_cluster)
    -> replay::attempt_fn {
    return [=](const std::vector<call_record>& calls, std::int64_t base, int attempt) {
        run_paths paths{out_dir / ("attempt-" + std::to_string(attempt))};
        fs::remove_all(paths.dir);
        fs::create_directories(paths.dir);
        write_json_file(paths.config(), to_json(cfg));
        use_image(cfg);
        docker_chaos::ChaosCluster cluster(cfg.compose_file, cfg.nodes, std::chrono::seconds{60});
        start_cluster(cluster);
        if (keep_cluster) {
            // The next attempt's start takes it down.
            cluster.detach();
        }
        context ctx{.paths = paths,
                    .cfg = cfg,
                    .cluster = cluster,
                    .fiu_send = real_fiu_send,
                    .procs = real_process_ops(),
                    .mono_ms = real_mono_ms,
                    .wall = real_wall,
                    .failpoints = fps};
        const auto t0 = real_mono_ms();
        for (const auto& rec : calls) {
            if (!replay::replayable(rec)) {
                continue;
            }
            const auto due = t0 + (rec.started_mono_ms - base);
            if (const auto wait = due - real_mono_ms(); wait > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{wait});
            }
            static_cast<void>(call(ctx, rec.tool, rec.args));
        }
        // The same end as a scripted case: stop the workload, heal, and run
        // every checker.
        static_cast<void>(call(ctx, "workload.stop", {}));
        static_cast<void>(call(ctx, "run.reset", {}));
        static_cast<void>(
            call(ctx, "check.run",
                 {{"checks", boost::json::array{"raft_safety", "linearizability", "liveness"}}}));
        const auto replayed = read_calls(paths.schedule().string());
        replay::attempt_outcome out;
        out.violated = violated(replayed);
        out.summary = {{"run_dir", paths.dir.string()}, {"calls", replayed.size()}};
        return out;
    };
}

}  // namespace chaos_ctl
