# Cairn Chaos Testing — Design Document

## Overview

Three layers, built bottom-up, and only the top one involves an LLM:

```
                ┌──────────────────────────────────────────────┐
  cairn_net     │ Cairn engine v0.2.1 (unmodified)  panel :127.0.0.1 │
  (internal)    │ worker containers (Claude / Haiku / llama.cpp)     │
                └───────────────┬──────────────────────────────┘
                                │ MCP tool calls only
                ┌───────────────▼──────────────────────────────┐
                │ chaos tool server  ── schedule log (JSONL)   │
                │   chaos_ctl (C++, harness.hpp)               │
                │   checkers (C++)    reporter                 │
                └───┬───────────────────────┬──────────────────┘
       container    │ filtered API          │ HTTP / fiu_rc_tcp by service name
       API proxy ◄──┘                       ▼
  (only holder of   raft_net (internal) ── chaos_node × 3..5, workload client
   the socket)
```

- **Layer 1, the harness** (§1-§2): `chaos_node` endpoints, `ChaosCluster` and
  `ChaosNode` gaps from the requirements' gap table, a workload client and
  three checkers. All of it is useful and testable with no Cairn at all, and
  M0 is done when it passes against the existing scripted scenarios.
- **Layer 2, the tool server** (§3-§4): one typed surface, one schedule log,
  one replay command. This is the contract between agents and the cluster,
  and the replay path is what makes a finding a test.
- **Layer 3, Cairn** (§5): the pinned engine, our worker images and routing
  config, the network fence, and the project definitions.

The decisions worth arguing about are §3 (agents never touch the runtime
socket), §4 (the schedule log, not the agents' reasoning, is the artifact),
§6 (clock skew) and §8 (the seeded bug cannot be found by toggling a
failpoint).

## 1. chaos_node: closing the control-plane gaps

All additions go in `cmd/chaos_node/http_control.hpp` and keep its existing
style (hand-built JSON, `httplib::Server`, one handler per route).

| Route | Implementation | Notes |
|---|---|---|
| `GET /log/<i>` | `node.log_entries_between(i, i)` | Already public and copies under the node's lock, unlike `debug_state()`'s span. Empty result → `404` with `{"error":"compacted"}` below `snapshot_index`, `{"error":"beyond_end"}` above `last_log_index`. |
| `GET /log?from=a&to=b` | `log_entries_between(a, b)` | Bounded to 1024 entries per call. The checker reads whole logs, and 1024 single-entry requests per node per check is what makes a checker too slow to run after every step. |
| `GET /status` | add `leader_id`, `last_log_index`, `last_log_term`, `snapshot_index`, `config{voters,learners,joint}` | `leader_id` is already promised by the file's header comment; the body never wrote it. |
| `GET /elections` | ring buffer appended in the node's become-leader path | See §1.1. |
| `GET /kv/<k>?mode=linearizable` | `node.read_state(timeout)`, then decode the key from the returned state bytes | `read_state()` confirms leadership with a heartbeat quorum and returns the whole state machine; decoding one key is cheap at chaos-test key counts. |
| `GET /kv/<k>?mode=local` | `node.with_state_machine(...)` | No quorum round. Deliberately allowed to be stale, so stale-read findings are attributable to the mode that permits them. |
| `POST /command` `"op":"cas"` | new `command_type::cas` in `test_key_value_state_machine` | Appended after `del`; the existing three encodings and their byte values are unchanged. |
| `POST /membership/{add,remove}` | `node.add_server()` / `remove_server()` | Returns when the future resolves; maps a pending change to `409`. |

### 1.1 Why `/elections` instead of better sampling

`assert_no_split_brain()` reads every node's `/status` once and compares.
Two leaders in one term that never overlap one of those samples are
invisible to it, and a sampler cannot fix that by polling faster. The node
itself knows every term it won, so it records them. The checker then has
the whole run's `(term, node)` pairs, and "at most one leader per term"
becomes an exact check instead of a probabilistic one. The ring buffer
lives in the node process and is lost on restart, which is acceptable:
the checker reads `/elections` before every `fault.kill` and `fault.restart`
the tool server performs (§3.3).

## 2. Harness, workload and checkers

### 2.1 `ChaosCluster` with a node table

`ChaosCluster` gains a constructor that takes a `std::vector<NodePorts>`.
The existing constructor builds that vector from `k_node_map`, so every
current caller is unchanged. The 5-node target reuses the shape of
`docker/docker-compose.quorum-5.yml` (container names are parameterised
there by `QUORUM_CLUSTER`), and its node table is generated from the same
variable, not hard-coded twice.

`ChaosNode` gains `pause()`/`unpause()` (lifting the existing
`quorum_harness.hpp` implementation), `apply_delay(ms, jitter)` and
`apply_loss(pct)` as thin wrappers over `tc_netem_cmd`, and every new
command vector gets a pure builder in `os_faults.hpp` with a unit case in
`harness_unit_test.cpp`, matching how every existing builder is tested.

Rootless Podman note: `pause` needs the cgroup v2 freezer, which rootless
Podman has on cgroup v2 hosts and lacks on cgroup v1. The tool reports
`unsupported` on a host where `pause` fails with that error, instead of
treating it as a fault that took effect.

### 2.2 Workload client: `cmd/chaos_workload`

A small C++ binary, built into its own image, run as a compose service on
`raft_net`. It is C++ because it reuses `harness.hpp`'s HTTP helpers and
the checker's history types directly. The history line format:

```json
{"p":3,"type":"invoke","f":"cas","key":"k2","value":[4,7],"t":18273645}
{"p":3,"type":"ok","f":"cas","key":"k2","value":[4,7],"t":18291002,"node":2}
```

`type` is `invoke`, `ok`, `fail` or `info`, with Jepsen's meaning:
`fail` only when the operation definitely did not take effect (an HTTP
`503 not_leader` before submission, a `400`); `info` for every timeout or
connection loss after the request was sent. A process that records `info`
retires and a new process id takes its place, so no process ever has two
outstanding operations. Wing-Gong / WGL relies on that.

### 2.3 Checkers: `tests/docker_chaos/checkers/`

Header-only C++, each with a unit test fed by hand-written histories,
including known-bad ones. A checker with no failing input in its unit tests
has not been shown to detect anything.

- **`linearizability.hpp`**: WGL search with per-key partitioning (P-compositionality)
  against a register model with `read`, `write` and `cas`. `info` operations
  may linearize anywhere after their invocation or not at all. There is a hard
  search budget: a search that exceeds it reports `unknown`, never `ok`.
- **`raft_safety.hpp`**: fed by `/log` ranges, `/status` and `/elections` from every
  node. It checks one leader per term, log matching, leader completeness and
  state-machine safety (Requirement 5.2). It runs after each tool step and at run
  end.
- **`liveness.hpp`**: once no fault is active, polls for a leader and a
  `commit_index` advance within `k * ELECTION_TIMEOUT_MAX_MS *
  KYTHIRA_TEST_TIMEOUT_SCALE`, with `k` recorded beside the constant as the
  existing tests do.

M0's acceptance is that all three pass on histories collected while the
existing scripted scenarios in `tests/docker_chaos/` run (`safety_assertions_test`,
`network_degradation_test`, `crash_recovery_test`, `election_recovery_test`,
`persistence_faults_test`, `az_partition_test`).

## 3. Tool server

### 3.1 Shape

`cmd/chaos_ctl` is a C++ CLI over `harness.hpp`, one subcommand per tool,
JSON in and JSON out. A thin MCP adapter (`docker/cairn/tool_server/`)
exposes those subcommands as MCP tools to Cairn workers and does nothing
else. Keeping the logic in `chaos_ctl` means:

- replay (§4) is the same binary with no MCP and no agent;
- the tool surface is unit-testable with the existing `CmdExecutor` /
  `HttpGet` stubs;
- the adapter is small enough to audit at a glance.

### 3.2 The runtime socket stays behind a filter

The tool server needs `kill`, `start`, `pause`, `unpause` and `exec` (for
`tc`/`iptables`) on cluster containers. It does **not** mount the
container-runtime socket. It talks to a filtering proxy built on
`docker/container_api_proxy/`, which already exists for exactly this reason
(its header: one unfiltered `POST /containers/create` with a host bind mount
is root on the host). The proxy is extended with an allowlist for
`POST /containers/<name>/{kill,start,pause,unpause}` and
`POST /containers/<name>/exec` whose `Cmd` must match one of the argument
shapes `os_faults.hpp` builds, on containers carrying this run's label.
Everything else is `403`.

Agents never see the proxy either. They see MCP tools with typed
arguments, and the failpoint names are checked against
`docker/cairn/failpoints.allowlist`, generated from
`include/raft/fault_injection.hpp` so the two cannot drift.

### 3.3 Ordering inside one tool call

1. Append the call to the schedule log (status `started`).
2. If the call kills or restarts a node, snapshot that node's
   `/elections` and `/log` first (§1.1).
3. Execute.
4. Run `raft_safety` on the post-step state.
5. Update the schedule-log line with the result and any violation.

## 4. Schedule log and replay

The schedule log is the artifact of a run, not the agents' reasoning. Each
line holds the wall-clock and monotonic time, the run seed, the tool, its
arguments, its result and the cluster's Kythira commit and image digest.
`chaos_ctl replay <log>` builds a fresh cluster from the recorded image and
replays the calls with their recorded gaps, then runs the checkers.

Two caveats are part of the design, not footnotes:

- Replay reproduces the **schedule**, not the interleaving. A violation that
  depends on a race inside one tool step may need several replays.
  `replay --repeat N` reports the reproduction rate, and a finding is
  confirmed only at ≥ 1 reproduction in a stated N.
- Minimization (fewest calls that still reproduce) is delta debugging over
  the log, run by `chaos_ctl minimize`, again without an LLM. The doc assigns
  minimization to "a second agent". The agent picks which subsets to try and
  `chaos_ctl` judges each one, so an agent can never declare a minimal
  schedule that does not reproduce.

## 5. Cairn layer: `docker/cairn/`

- `cairn-compose.yml`: the engine at the v0.2.1 image pinned by digest, the
  worker services, the tool server, the extended API proxy, an egress proxy,
  and the networks. The panel publishes `127.0.0.1:<port>` only.
- Networks: `cairn_net` (`internal: true`) for engine, workers and tool
  server. `raft_net` (`internal: true`) for the cluster, the workload and the
  tool server. `egress_net` connects only the egress proxy, which forwards to
  the Anthropic API host and the llama.cpp host and refuses the rest.
  Service-name addressing throughout and no static IPs (CLAUDE.md rules 1
  and 5).
- Worker images: our own Dockerfiles, each FROM the upstream worker image at
  a pinned digest, adding only the MCP client config. No other tooling is
  installed (Requirement 7.4).
- Model routing: Cairn configuration only, no fork. M0 records whether a
  worker can point at llama.cpp's OpenAI-compatible endpoint. If none can
  without changing Cairn, routing falls back to the two API tiers and the
  cost model in §7 is redone before M1.
- Projects: one per invariant, under `docker/cairn/projects/`. The origin is
  the healthy cluster's facts: node ids, leader, term, commit index and
  configuration. The goal is one invariant. Seed hints are the doc's four:
  snapshot install during a partition, joint-consensus transitions, gossip
  replication racing leader replication, and object-store persistence
  failures. The last applies only to a target built with object-store
  persistence, which is M4.
- `scripts/cairn/kill.sh`: stops every compose project this spec starts, for
  the configured runtime, and is also what the wall-clock limit fires.

## 6. Clock skew

`fault.clock_skew` is the one doc tool the Docker phase cannot deliver
honestly. Containers share the host's clock, and changing it needs
`CAP_SYS_TIME`, which rootless Podman cannot grant and CLAUDE.md rule 4
forbids relying on. Options, in order of preference:

1. **A clock seam in Kythira.** Route the timer reads the election and
   heartbeat paths use through an injectable clock, and add a
   `raft/clock/offset_ms` failpoint that applies a skew. This is a change to
   production code and needs its own spec. It is listed here, not decided.
2. **libfaketime** via `LD_PRELOAD` in the `chaos_node` image. The offset is
   fixed at process start, so a skew means a restart. That is a different
   fault from skewing a running node, and it would be recorded as such.

Until one is chosen, the tool returns `unsupported`. Note that Kythira's
`read_state()` confirms leadership with a heartbeat quorum, not a time
lease, so linearizable reads do not depend on clock agreement today. Skew
matters for liveness (election timing) more than for safety.

## 7. Cost model

The doc sets a budget of $20 per month, enforced by a spend limit on the API
key. Per run: Claude plans, triages and minimizes. The cheaper API tier
handles work a local model fails at. llama.cpp runs the bulk of the
step-by-step tool loops and log reading. M1 measures tokens per role per run
and records them in `tasks.md`. The monthly cadence for M5 is set from that
measurement, not assumed.

## 8. The seeded bug (M1)

M1 proves the loop finds something before a clean result means anything.
The planted bug, which skips the term check when granting a vote, is
compiled in only under `-DKYTHIRA_CHAOS_SEEDED_BUG=vote_term`, into an image
tagged `kythira-chaos-node:seeded-vote-term`. It is never enabled by a
failpoint, because the agents can toggle failpoints and a bug they could
switch on is not a bug they found. The CMake option defaults off, no
defconfig or CI job sets it, and configure prints a warning banner whenever
it is on so a seeded binary cannot be mistaken for a normal one.

## 9. Licensing

Cairn is AGPL-3.0 and is run unmodified from its published image. This
repository holds only Apache-2.0 material: configuration that references
the image by digest, our worker Dockerfiles, `chaos_ctl`, the checkers, the
workload client, the reporter and generated tests. If routing ever needs a
Cairn change, that change goes in a separate fork outside this repository,
and AGPL source-sharing applies to that fork if it is offered over a
network.

## 10. Phases beyond Docker

M3 (the flaky Real Cloud Tests scenario) and M4 (EC2) reuse `chaos_ctl` with
a different executor. M4 adds an AWS profile whose IAM policy allows only
`ec2:StartInstances`, `StopInstances` and `ModifyInstanceAttribute`
conditioned on `aws:ResourceTag/cairn-target`, plus the existing real-EC2
fixture's teardown and leak audit. M4 is designed in its own follow-up spec
once M3 is done. This spec fixes only its guardrails (Requirement 9.5).
