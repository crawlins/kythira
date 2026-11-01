# Chaos findings

Findings from Cairn runs against Kythira (`.kiro/specs/cairn-chaos-testing/`,
Requirement 6). Each entry is written by `chaos_ctl report` from a run's
schedule log, together with a replay file and a draft `ChaosCluster` test in
`tests/docker_chaos/findings/`. Nothing is committed automatically: each
entry and draft is reviewed before it lands, and a draft test must fail on
the commit the finding was made against and pass after the fix before it
merges (Requirement 6.3).

A finding is a checker violation that reproduced from its schedule with no
model involved (Requirement 0.3). The replay rate is how many replays of the
run's log saw the same invariant broken again.

## 2026-10-30-election-safety-368: election_safety

| | |
|---|---|
| Date | 2026-10-30 18:50:30Z, 29.4 min into the run |
| Invariant | `election_safety`: 2 nodes won term 43 |
| Kythira commit | unknown |
| Image | `kythira-chaos-node:seeded-vote-term`, id `e0b023e45710` |
| Cairn version | v0.2.1 (e0ef2f85) |
| Run seed | 1 |
| Replay | 3 of 3 replays of the run's log reproduced it |
| Replay file | `tests/docker_chaos/findings/2026-10-30-election-safety-368.jsonl` |
| Draft test | `tests/docker_chaos/findings/2026-10-30-election-safety-368_test.cpp`, 1 attempt(s) |
| Status | draft, awaiting review. Fails-on-seeded not yet confirmed: on November 1 the draft (0 of 1) and a replay of the uncut log (0 of 1) both ran cleanly on a seeded image built in a cloud sandbox (Docker, Boost future backend), where the October 31 replays on tikhov had reproduced it 3 of 3. Confirm on tikhov before relying on it. |

Not a Kythira bug: M1 (task 11) finding the bug that task 9 seeds on purpose (`KYTHIRA_CHAOS_SEEDED_BUG=vote_term`), which only the `seeded-vote-term` image carries. Run 1 of the second M1 campaign (Qwen3.8-27B through the Pi worker); the uncut log is kept with that campaign's output, outside the repo. The Kythira commit was not recorded because the tool server's image has no git checkout; `scripts/cairn/start-run.sh` now passes it in.

**Schedule.** The run's log holds 369 calls; the finding is seq 368. The replay file keeps 35 of them, spanning 28.8 min. Cut: 1 after the finding, 40 read-only views, and 293 refused by the tool server before acting. Not minimized: `chaos_ctl minimize` has not judged these cuts, and the replay rate above is for the uncut log.

```text
    +0.0s  #4     workload.start       {"seed":1,"concurrency":4,"rate":20,"keys":3,"values":5}
  +642.4s  #244   workload.stop        {}
  +642.7s  #245   run.reset            {}
  +651.3s  #246   fault.partition      {"groups":[[1],[2,3]]}
  +683.6s  #247   fault.partition      {"heal":true}
  +709.7s  #248   fault.kill           {"node":1}
  +719.2s  #249   fault.kill           {"node":2}
  +729.5s  #251   fault.restart        {"node":1}
  +749.5s  #252   fault.restart        {"node":2}
  +805.4s  #254   check.run            {}
  +848.2s  #258   cluster.remove_member {"node":1}
  +860.0s  #260   cluster.add_member   {"node":1}
          ... 11 more calls ...
 +1504.3s  #339   cluster.add_member   {"node":2}
 +1520.2s  #341   cluster.add_member   {"node":1}
 +1526.6s  #343   fault.kill           {"node":3}
 +1558.2s  #347   fault.partition      {"groups":[[1],[2,3]]}  [error]
 +1567.0s  #348   check.run            {}
 +1574.7s  #349   fault.kill           {"node":3}  [error]
 +1579.5s  #350   fault.restart        {"node":3}
 +1604.5s  #351   fault.partition      {"groups":[[1],[2,3]]}
 +1641.0s  #353   fault.partition      {"heal":true}
 +1692.1s  #364   fault.partition      {"groups":[[1],[2,3]]}
 +1714.9s  #366   fault.kill           {"node":2}
 +1726.8s  #368   fault.restart        {"node":2}
```

**Log excerpt.** The finding's outcome line, without its result:

```json
{"seq":368,"status":"ok","mono_ms":1261200404,"raft_safety":{"verdict":"violation","terms_with_a_leader":38,"committed_entries_compared":7728,"violations":1},"violations":[{"invariant":"election_safety","detail":"2 nodes won term 43","nodes":[1,3],"term":43,"schedule_seq":368}],"notes":["snapshot of node 2 incomplete: node not readable"],"wall":"2026-10-30T18:50:30.465Z"}
```

## 2026-10-30-election-safety-432: election_safety

| | |
|---|---|
| Date | 2026-10-30 19:27:18Z, 35.4 min into the run |
| Invariant | `election_safety`: 2 nodes won term 26 |
| Kythira commit | unknown |
| Image | `kythira-chaos-node:seeded-vote-term`, id `e0b023e45710` |
| Cairn version | v0.2.1 (e0ef2f85) |
| Run seed | 2 |
| Replay | 1 of 3 replays of the run's log reproduced it |
| Replay file | `tests/docker_chaos/findings/2026-10-30-election-safety-432.jsonl` |
| Draft test | `tests/docker_chaos/findings/2026-10-30-election-safety-432_test.cpp`, 6 attempt(s) |
| Status | draft, awaiting review |

Not a Kythira bug: M1 (task 11) finding the bug that task 9 seeds on purpose (`KYTHIRA_CHAOS_SEEDED_BUG=vote_term`), which only the `seeded-vote-term` image carries. Run 2 of the second M1 campaign (Qwen3.8-27B through the Pi worker); the uncut log is kept with that campaign's output, outside the repo. The Kythira commit was not recorded because the tool server's image has no git checkout; `scripts/cairn/start-run.sh` now passes it in.

**Schedule.** The run's log holds 433 calls; the finding is seq 432. The replay file keeps 8 of them, spanning 34.7 min. Cut: 1 after the finding, 10 read-only views, and 414 refused by the tool server before acting. Not minimized: `chaos_ctl minimize` has not judged these cuts, and the replay rate above is for the uncut log.

```text
    +0.0s  #4     check.run            {}
  +180.1s  #273   workload.start       {"seed":2,"concurrency":3,"rate":30,"keys":3,"values":5}
  +185.5s  #275   fault.partition      {"groups":[[1],[2,3]]}
  +200.7s  #277   check.run            {}
  +210.4s  #279   run.reset            {}
  +929.2s  #380   run.reset            {}
 +1316.3s  #399   run.reset            {}
 +2084.0s  #432   fault.failpoint      {"node":1,"name":"candidate_term_seed","mode":"off"}  [error]
```

**Log excerpt.** The finding's outcome line, without its result:

```json
{"seq":432,"status":"error","error":"failpoint 'candidate_term_seed' is not in docker/cairn/failpoints.allowlist","raft_safety":{"verdict":"violation","terms_with_a_leader":20,"committed_entries_compared":34490,"violations":1},"violations":[{"invariant":"election_safety","detail":"2 nodes won term 26","nodes":[2,3],"term":26,"schedule_seq":432}],"notes":[],"wall":"2026-10-30T19:27:18.036Z","mono_ms":1263407975}
```

## 2026-10-30-election-safety-1373: election_safety

| | |
|---|---|
| Date | 2026-10-30 20:40:31Z, 72.1 min into the run |
| Invariant | `election_safety`: 2 nodes won term 39 |
| Kythira commit | unknown |
| Image | `kythira-chaos-node:seeded-vote-term`, id `e0b023e45710` |
| Cairn version | v0.2.1 (e0ef2f85) |
| Run seed | 3 |
| Replay | 3 of 3 replays of the run's log reproduced it |
| Replay file | `tests/docker_chaos/findings/2026-10-30-election-safety-1373.jsonl` |
| Draft test | `tests/docker_chaos/findings/2026-10-30-election-safety-1373_test.cpp`, 1 attempt(s) |
| Status | draft, awaiting review |

Not a Kythira bug: M1 (task 11) finding the bug that task 9 seeds on purpose (`KYTHIRA_CHAOS_SEEDED_BUG=vote_term`), which only the `seeded-vote-term` image carries. Run 3 of the second M1 campaign (Qwen3.8-27B through the Pi worker); the uncut log is kept with that campaign's output, outside the repo. The Kythira commit was not recorded because the tool server's image has no git checkout; `scripts/cairn/start-run.sh` now passes it in.

**Schedule.** The run's log holds 1374 calls; the finding is seq 1373. The replay file keeps 205 of them, spanning 71.3 min. Cut: 1 after the finding, 147 read-only views, and 1021 refused by the tool server before acting. Not minimized: `chaos_ctl minimize` has not judged these cuts, and the replay rate above is for the uncut log.

```text
    +0.0s  #4     workload.start       {"concurrency":4,"rate":50,"keys":64,"values":4,"mix":"write,read,cas","seed":7}
   +16.0s  #8     workload.start       {"seed":3,"concurrency":4,"rate":50,"keys":64,"values":4,"mix":"write,read,cas"}
   +38.5s  #13    check.run            {"checks":["raft_safety","linearizability"]}
   +42.4s  #14    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"write"}
   +52.8s  #17    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"w"}
   +63.2s  #20    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"writes"}
   +73.6s  #23    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"read,write"}
   +84.3s  #26    workload.stop        {}
   +85.4s  #27    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"write,read"}
   +95.5s  #29    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"put"}
  +106.8s  #32    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"set"}
  +110.1s  #34    workload.start       {"seed":3,"concurrency":2,"rate":10,"keys":8,"values":2,"mix":"get"}
          ... 181 more calls ...
 +3983.7s  #1176  fault.partition      {"groups":[[3],[1,2]]}
 +3989.1s  #1177  fault.pause          {"node":3,"resume":false}
 +4000.4s  #1178  fault.partition      {"heal":true}
 +4081.5s  #1183  fault.restart        {"node":3}  [error]
 +4105.0s  #1184  run.reset            {}
 +4229.7s  #1367  fault.pause          {"node":3,"resume":true}  [error]
 +4230.1s  #1368  fault.pause          {"node":2,"resume":true}  [error]
 +4230.5s  #1369  fault.pause          {"node":1,"resume":true}  [error]
 +4233.5s  #1370  fault.pause          {"node":3}
 +4246.5s  #1371  fault.partition      {"groups":[[1,3],[2]]}  [error]
 +4263.0s  #1372  fault.partition      {"groups":[[2],[1,3]]}  [error]
 +4279.9s  #1373  run.reset            {}
```

**Log excerpt.** The finding's outcome line, without its result:

```json
{"seq":1373,"status":"ok","mono_ms":1267801299,"raft_safety":{"verdict":"violation","terms_with_a_leader":30,"committed_entries_compared":192,"violations":1},"violations":[{"invariant":"election_safety","detail":"2 nodes won term 39","nodes":[2,3],"term":39,"schedule_seq":1373}],"notes":["snapshot of node 3 incomplete: node not readable"],"wall":"2026-10-30T20:40:31.359Z"}
```
