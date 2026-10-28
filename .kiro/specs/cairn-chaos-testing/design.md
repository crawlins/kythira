# Cairn Chaos Testing — Design Document

## Overview

Three layers, built bottom-up, and only the top one involves an LLM:

```
                ┌──────────────────────────────────────────────┐
  engine_net    │ Cairn engine v0.2.1 (unmodified)  panel :127.0.0.1 │
  cairn_net     │ worker containers (Pi on llama.cpp for M1)         │
  (internal)    └───────────────┬──────────────────────────────┘
                                │ `chaos` command → HTTP, only
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
JSON in and JSON out. Cairn v0.2.1 has no MCP (task 1a): a worker's tools
are its agent CLI's own, and anything else is a command the agent runs. So
a thin HTTP front (`docker/cairn/tool_server/chaos_http.py`) answers
`GET /tools` and `POST /tool/<name>` on `cairn_net`, runs
`chaos_ctl <name> -` one call at a time, and refuses any name
`chaos_ctl tools` does not list (so not `init`, `replay` or `minimize`).
The worker image carries a `chaos` command (`docker/cairn/worker/chaos`)
that calls it. Keeping the logic in `chaos_ctl` means:

- replay (§4) is the same binary with no front and no agent;
- the tool surface is unit-testable with the existing `CmdExecutor` /
  `HttpGet` stubs;
- the front is small enough to audit at a glance.

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

Agents never see the proxy either. They see the `chaos` command's tools
with typed JSON arguments, and the failpoint names are checked against
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

- `cairn-compose.yml`: the engine (Cairn v0.2.1 built unmodified from its
  pinned commit by `scripts/cairn/build-engine.sh`, since Cairn publishes no
  engine image), its dispatcher, the tool server, two API proxies, an
  egress proxy, an SSH tunnel to the model, and the networks. The panel
  publishes `127.0.0.1:<port>` only. Workers are not compose services:
  Cairn's dispatcher creates one per project.
- Networks: `cairn_net` (`internal: true`) for workers, the tool server, the
  egress proxy and the tunnel. `engine_net` (`internal: true`) for the
  engine and the dispatcher only, because the engine's API would let an
  agent write the board it is judged by. `dispatch_net` (`internal: true`)
  for the dispatcher and its API proxy. `raft_net` (`internal: true`) for
  the cluster, the workload and the tool server. `egress_net` connects only
  the egress proxy, which forwards to the allowlisted model hosts and
  refuses the rest, and the tunnel. Service-name addressing throughout and
  no static IPs (CLAUDE.md rules 1 and 5); `cairn_net` has a fixed name,
  `${CAIRN_RUN}-cairn_net`, because the dispatcher creates workers on it by
  name.
- The dispatcher's runtime access goes through its own instance of the API
  proxy (`PROXY_MODE=cairn_dispatcher`): create only `cairn-dispatch-*` /
  `cairn-startup-healthcheck-*` containers of the worker image running
  `sleep infinity` on `cairn_net` with no other host config, exec in them
  unprivileged, copy files in, stop and remove them. The proxy labels what
  it creates and hides every other container. So a compromised dispatcher
  can start more sandboxed workers and nothing else.
- Worker image: our own, `node` slim pinned by digest plus Pi 0.73.0 (the
  version Cairn's image carries, its dependency tree pinned by a
  lockfile), `rg` and `fd` for Pi's search tools, and the `chaos` command.
  Not FROM Cairn's worker image: that is Kali with a security toolchain,
  which Requirement 7.4 rules out, and its other two agent CLIs cannot reach
  llama.cpp anyway (task 1b).
- Model routing: Cairn configuration only, no fork (`dispatch.yaml`). Task
  1b found that only the Pi worker can use llama.cpp's OpenAI-compatible
  endpoint, so M1 runs on one tier: Pi on a local model, reached through
  `llm-tunnel`, an SSH forward to `llama-server` on the model host's
  loopback. The API tiers are written out but commented: they need a
  spend-limited key (Requirement 8.1) and their CLI in the worker image.
- Projects: one per invariant, under `docker/cairn/projects/`, created by
  `scripts/cairn/start-run.sh`, which also starts the stack, makes a fresh
  run directory current, stops any project left active by an earlier run
  and arms the wall-clock limit. The origin is how to use the `chaos`
  command plus the healthy cluster's facts from `cluster.status`: node ids,
  leader, term, commit index and configuration. The goal is one invariant.
  Seed hints are the doc's four: snapshot install during a partition,
  joint-consensus transitions, gossip replication racing leader
  replication, and object-store persistence failures. The last two apply
  only to a target built with peer-to-peer catch-up or object-store
  persistence, which the Docker `chaos_node` is not, so they are added
  only when `CAIRN_TARGET_FEATURES` names them.
- `scripts/cairn/kill.sh`: stops every compose project this spec starts and
  the workers the dispatcher made, for the configured runtime, and is also
  what the wall-clock limit fires.

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

M1 starts with the local tier alone and no API key at all: no machine
here has a GPU that can hold a useful model, so the model host is a rented
cloud GPU (task 10's model-server notes), started for the runs and stopped
between them. Its cost per run (instance hours, plus the stopped volume)
is recorded with the tokens. Pi reports each turn's token counts in its
JSON event stream, which the dispatcher logs. Whether the API tiers are
needed is what M1 shows: if the local model cannot find the seeded bug, the
miss is recorded (Requirement 9.7) before any key is set up.

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
