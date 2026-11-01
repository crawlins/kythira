// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// DRAFT written by `chaos_ctl report` (cairn-chaos-testing Requirement 6).
// Review it before committing; nothing was committed for you.
//
// Finding 2026-10-30-election-safety-432 (doc/chaos-findings.md):
//   election_safety, "2 nodes won term 26"
//   at seq 432 of run-2/schedule.jsonl,
//   35.4 min into a run on kythira-chaos-node:seeded-vote-term
//   (image id e0b023e45710, Kythira commit unknown).
//
// The test replays the schedule below at its recorded offsets on a fresh
// three-node cluster, heals, runs every checker, and fails if the
// invariant is violated again in any of 6 attempt(s). It must
// fail on the image the finding was made on and pass once the bug is fixed
// (Requirement 6.3):
//
//   KYTHIRA_CHAOS_NODE_IMAGE=kythira-chaos-node:seeded-vote-term  fails
//   KYTHIRA_CHAOS_NODE_IMAGE=kythira-chaos-node:dev  passes after the fix
//
// The docker-chaos-findings target runs every finding test;
// KYTHIRA_CHAOS_FINDING_REPEAT overrides the attempt count.

#define BOOST_TEST_MODULE finding_2026_10_30_election_safety_432
#include <boost/test/unit_test.hpp>

#include "chaos_finding.hpp"

namespace {

// Offset from the first call (ms), seq in the run's log, tool, arguments.
// 8 of the log's 433 calls; doc/chaos-findings.md says which were cut and why.
constexpr chaos_finding::step k_schedule[] = {
    {0, 4, "check.run", R"json({})json"},
    {180053, 273, "workload.start",
     R"json({"seed":2,"concurrency":3,"rate":30,"keys":3,"values":5})json"},
    {185527, 275, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {200744, 277, "check.run", R"json({})json"},
    {210356, 279, "run.reset", R"json({})json"},
    {929170, 380, "run.reset", R"json({})json"},
    {1316269, 399, "run.reset", R"json({})json"},
    {2084005, 432, "fault.failpoint",
     R"json({"node":1,"name":"candidate_term_seed","mode":"off"})json"},
};

const chaos_finding::finding k_finding{
    .id = "2026-10-30-election-safety-432",
    .invariants = {"election_safety"},
    .attempts = 6,
    .found_on_image = "kythira-chaos-node:seeded-vote-term",
};

}  // namespace

BOOST_AUTO_TEST_CASE(finding_2026_10_30_election_safety_432, *boost::unit_test::timeout(14304)) {
    chaos_finding::check(k_finding, k_schedule);
}
