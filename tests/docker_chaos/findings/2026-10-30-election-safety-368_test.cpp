// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// DRAFT written by `chaos_ctl report` (cairn-chaos-testing Requirement 6).
// Review it before committing; nothing was committed for you.
//
// Finding 2026-10-30-election-safety-368 (doc/chaos-findings.md):
//   election_safety, "2 nodes won term 43"
//   at seq 368 of run-1/schedule.jsonl,
//   29.4 min into a run on kythira-chaos-node:seeded-vote-term
//   (image id e0b023e45710, Kythira commit unknown).
//
// The test replays the schedule below at its recorded offsets on a fresh
// three-node cluster, heals, runs every checker, and fails if the
// invariant is violated again in any of 1 attempt(s). It must
// fail on the image the finding was made on and pass once the bug is fixed
// (Requirement 6.3):
//
//   KYTHIRA_CHAOS_NODE_IMAGE=kythira-chaos-node:seeded-vote-term  fails
//   KYTHIRA_CHAOS_NODE_IMAGE=kythira-chaos-node:dev  passes after the fix
//
// The docker-chaos-findings target runs every finding test;
// KYTHIRA_CHAOS_FINDING_REPEAT overrides the attempt count.

#define BOOST_TEST_MODULE finding_2026_10_30_election_safety_368
#include <boost/test/unit_test.hpp>

#include "chaos_finding.hpp"

namespace {

// Offset from the first call (ms), seq in the run's log, tool, arguments.
// 35 of the log's 369 calls; doc/chaos-findings.md says which were cut and why.
constexpr chaos_finding::step k_schedule[] = {
    {0, 4, "workload.start", R"json({"seed":1,"concurrency":4,"rate":20,"keys":3,"values":5})json"},
    {642423, 244, "workload.stop", R"json({})json"},
    {642732, 245, "run.reset", R"json({})json"},
    {651252, 246, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {683570, 247, "fault.partition", R"json({"heal":true})json"},
    {709718, 248, "fault.kill", R"json({"node":1})json"},
    {719161, 249, "fault.kill", R"json({"node":2})json"},
    {729465, 251, "fault.restart", R"json({"node":1})json"},
    {749465, 252, "fault.restart", R"json({"node":2})json"},
    {805446, 254, "check.run", R"json({})json"},
    {848238, 258, "cluster.remove_member", R"json({"node":1})json"},
    {860013, 260, "cluster.add_member", R"json({"node":1})json"},
    {874166, 262, "cluster.remove_member", R"json({"node":2})json"},
    {881686, 264, "fault.kill", R"json({"node":3})json"},
    {894482, 266, "fault.restart", R"json({"node":3})json"},
    {946074, 268, "check.run", R"json({})json"},
    {1370781, 325, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {1393391, 327, "fault.kill", R"json({"node":1})json"},
    {1403419, 329, "fault.partition", R"json({"heal":true})json"},
    {1412800, 331, "fault.restart", R"json({"node":1})json"},
    {1448579, 333, "cluster.remove_member", R"json({"node":2})json"},
    {1468667, 335, "cluster.remove_member", R"json({"node":1})json"},
    {1484357, 337, "cluster.add_member", R"json({"node":1})json"},
    {1504285, 339, "cluster.add_member", R"json({"node":2})json"},
    {1520213, 341, "cluster.add_member", R"json({"node":1})json"},
    {1526606, 343, "fault.kill", R"json({"node":3})json"},
    {1558169, 347, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {1566961, 348, "check.run", R"json({})json"},
    {1574702, 349, "fault.kill", R"json({"node":3})json"},
    {1579546, 350, "fault.restart", R"json({"node":3})json"},
    {1604512, 351, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {1641034, 353, "fault.partition", R"json({"heal":true})json"},
    {1692115, 364, "fault.partition", R"json({"groups":[[1],[2,3]]})json"},
    {1714900, 366, "fault.kill", R"json({"node":2})json"},
    {1726843, 368, "fault.restart", R"json({"node":2})json"},
};

const chaos_finding::finding k_finding{
    .id = "2026-10-30-election-safety-368",
    .invariants = {"election_safety"},
    .attempts = 1,
    .found_on_image = "kythira-chaos-node:seeded-vote-term",
};

}  // namespace

BOOST_AUTO_TEST_CASE(finding_2026_10_30_election_safety_368, *boost::unit_test::timeout(2026)) {
    chaos_finding::check(k_finding, k_schedule);
}
