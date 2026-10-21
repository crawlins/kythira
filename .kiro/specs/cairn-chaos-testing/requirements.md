# Cairn Chaos Testing — Requirements Document

## Introduction

Kythira's fault testing is strong and entirely scripted. The libfiu suite
(`tests/chaos/`, `ctest --label-regex chaos`) and the container suite
(`tests/docker_chaos/`, `ChaosCluster`/`ChaosNode` over a 3-node `chaos_node`
compose cluster) both run fault schedules a person wrote. They find the bugs
someone thought to look for. The recent chaos-crash fixes (PRs 589, 592 and
593) and two red Real Cloud Tests runs on `main` show that timing and
failure-ordering bugs nobody anticipated are still being found by accident.

This spec adds an **exploratory** layer on top of that harness. Cairn
(`oritera/Cairn`, AGPL-3.0, pinned to v0.2.1, commit
`e0ef2f850e5805f824815ee38f049e066deeb7d1`) is a blackboard engine whose
agents run observe/decide/act loops and coordinate only through a shared
graph of facts, intents and hints. Pointed at a disposable Kythira cluster
with the goal "violate a Raft safety guarantee", it becomes an adaptive fault
injector: each run starts from a healthy cluster, and every violation it
confirms becomes a deterministic, LLM-free regression test in the existing
suite.

The source design is Clark's doc "Cairn × Kythira: AI-Driven Chaos Testing
Spec" (claude.ai artifact `4CYMpy88KNjsXAC8Cuw6HB`, revision of
2026-10-21). This spec turns it into requirements and grounds each one in
what the harness can and cannot do today.

### What the harness cannot do today

Checked against `main` at `7f4f25b` on 2026-10-21. Each gap blocks at least
one invariant the doc names, so closing them is part of M0, not a nicety:

| Gap | Where | Blocks |
|---|---|---|
| `GET /log/<index>` always answers `404 {"error":"not_implemented"}` | `cmd/chaos_node/http_control.hpp` | log matching, leader completeness, state-machine safety |
| `GET /status` returns `node_id, role, term, commit_index, last_applied`; its own header comment also promises `leader_id`, which the body never writes; no membership, no last log index/term | same file | membership-change checks, triage |
| No read endpoint. The core has a linearizable `read_state()` (`include/raft/raft.hpp`), but `chaos_node` exposes only `POST /command` (put) | same file | linearizable-read checking |
| `test_key_value_state_machine` supports `put`, `get` and `del` only; no compare-and-set | `include/raft/test_state_machine.hpp` | the CAS workload the doc names |
| No membership endpoints; `node::add_server()`/`remove_server()` exist but nothing reaches them over HTTP | `cmd/chaos_node/` | `cluster.add_member` / `remove_member` |
| `k_node_map` is fixed at 3 nodes (`chaos_node_1..3`, ports 7001-7003 / 8081-8083 / 9001-9003) | `tests/docker_chaos/harness.hpp` | 5-node targets |
| `pause()` exists only on the quorum harness, not on `ChaosNode` | `tests/docker_chaos/quorum_harness.hpp` | `fault.pause` |
| `ChaosCluster::assert_no_split_brain()` samples `/status` once; two leaders that never overlap a sample go unseen | `tests/docker_chaos/harness.hpp` | election safety as a history property |
| No clock-skew mechanism. Containers share the host kernel clock, and rootless Podman cannot grant `CAP_SYS_TIME` | — | `fault.clock_skew` |

## Glossary

- **Run**: one Cairn session against one freshly built cluster, bounded by a
  wall-clock limit, targeting one invariant.
- **Schedule log**: the append-only JSONL record of every tool call in a run,
  with timestamp, seed, arguments and result. It is the replay source.
- **History**: the workload client's JSONL record of every client operation's
  invocation and completion, in the shape linearizability checkers consume.
- **Finding**: a checker violation that has been reproduced from its schedule
  log without agents and minimized.
- **Tool server**: the only process that touches the cluster on the agents'
  behalf. Agents never hold the container-runtime socket.

## Requirements

### Requirement 0: framing

0.1 Cairn SHALL be run as an unmodified, pinned upstream release. No Cairn
source SHALL be committed to, linked into, or redistributed with Kythira.
Kythira's own toolset, worker images, configuration, checkers and generated
tests are Apache-2.0 and live in this repository.

0.2 Cairn SHALL run on demand only. Nothing in this spec adds a job to the
per-PR or per-push CI matrix, and no existing CI gate changes.

0.3 Every finding SHALL be replayable without an LLM. A violation that cannot
be reproduced from its schedule log is recorded as unconfirmed, not reported.

### Requirement 1: the chaos_node control plane exposes what the checkers need

1.1 `GET /log/<index>` SHALL return `{index, term, command_b64}` for an entry
the node holds, and `404` only when the index is outside the node's log or
below its snapshot (with a body that says which). The current stub SHALL be
removed.

1.2 `GET /status` SHALL add `leader_id` (as its header comment already
promises), `last_log_index`, `last_log_term`, `snapshot_index` and the
node's current configuration (voters, learners, and whether a joint
configuration is in effect).

1.3 `GET /elections` SHALL return the terms in which this node became
leader since it started, with a monotonic timestamp for each, bounded to the
most recent 1024. This makes election safety checkable over a whole run,
not only at sample instants (see the gap table).

1.4 `GET /kv/<key>?mode=linearizable|local` SHALL serve a read.
`linearizable` SHALL go through `node::read_state()`; `local` SHALL read the
node's applied state without a quorum round. Both SHALL report the
`commit_index` and `last_applied` they were served at.

1.5 `POST /command` SHALL accept an optional `"op":"cas"` with `expected`
and `value`. `test_key_value_state_machine` SHALL gain a `cas` command type
that applies only when the current value equals `expected`, and reports
whether it applied. The existing `put`/`get`/`del` encodings SHALL NOT
change, because every test and example that uses this state machine depends
on them.

1.6 `POST /membership/add` and `POST /membership/remove` SHALL call
`node::add_server()` / `remove_server()` and return when the change commits
or fails, with the error.

1.7 All new endpoints SHALL keep the existing binding rule: published ports
bind to `${KYTHIRA_BIND_ADDRESS:-127.0.0.1}`, because the control plane and
`fiu_rc_tcp` are unauthenticated.

### Requirement 2: the harness addresses N nodes and every fault the toolset names

2.1 `ChaosCluster` SHALL take its node table (ids, container names, ports)
from configuration rather than only from `k_node_map`, so 3- and 5-node
targets use one code path. The existing 3-node default and every current
caller SHALL keep working unchanged.

2.2 `ChaosNode` SHALL gain `pause()` and `unpause()`, built with
`os::container_runtime()` like every other lifecycle command.

2.3 `ChaosNode` SHALL gain `apply_tc_netem` variants for delay with jitter
and for loss alone, so `fault.delay(node, ms, jitter)` and
`fault.drop(node, pct)` map to one `tc qdisc replace` each.

2.4 `fault.partition(groups)` SHALL be expressed with the existing
`partition_from()`/`unpartition()` and SHALL resolve peers by service name
to an IP at call time, per CLAUDE.md rule 1. No static address SHALL be
introduced in any compose file.

2.5 `fault.clock_skew` SHALL NOT be implemented by changing the host or
container clock. If it is implemented at all it SHALL be through a
mechanism that works under rootless Podman (see design §6), and until then
the tool SHALL return `unsupported` rather than appear to succeed.

### Requirement 3: a typed tool server is the agents' only interface

3.1 Agents SHALL reach the cluster only through a tool server that exposes
exactly the doc's toolset: `fault.failpoint`, `fault.partition`,
`fault.delay`, `fault.drop`, `fault.kill`, `fault.pause`, `fault.restart`,
`fault.clock_skew`, `cluster.status`, `cluster.add_member`,
`cluster.remove_member`, `workload.start`, `workload.stop`,
`workload.history`, `check.run` and `run.reset`.

3.2 `fault.failpoint` SHALL accept only failpoint names in a checked-in
allowlist drawn from the names `chaos_node` registers (the `raft/network/*`,
`raft/persistence/*`, `raft/objstore/*` and `raft/state_machine/apply`
families), with modes `always`, `random` and `once` as
`tests/docker_chaos/fault_control.hpp` already builds them.

3.3 Every tool call SHALL be appended to the run's schedule log before it
executes and updated with its result after, so an aborted call is still on
record.

3.4 The tool server SHALL be the only component with access to the
container runtime, and only through a filtering proxy in the style of
`docker/container_api_proxy/`: lifecycle and `exec` calls on this run's
labelled cluster containers, with `exec` limited to the `tc` and `iptables`
argument shapes `tests/docker_chaos/os_faults.hpp` builds. Everything else
SHALL be refused with 403.

3.5 A replay command SHALL execute a schedule log against a fresh cluster
with no agents and no LLM, honouring recorded inter-call delays, and SHALL
exit non-zero when the recorded violation reproduces.

### Requirement 4: a workload client records a checkable history

4.1 A workload client SHALL run in a container on the cluster network and
issue reads (both modes), writes and CAS operations against a configurable
key set, with configurable concurrency and rate.

4.2 It SHALL record each operation as an invocation and a completion in
JSONL: process id, operation, key, value(s), invoke and complete times from
one monotonic clock, and an outcome of `ok`, `fail` (definitely did not
happen) or `info` (indeterminate, e.g. a timeout after the request was sent).
A timeout SHALL NOT be recorded as `fail`, because treating an
indeterminate write as absent is the classic way a checker reports false
violations.

4.3 It SHALL follow leader redirects by retrying against the node `/status`
names as leader, and record every attempt.

### Requirement 5: invariant checkers

5.1 **Linearizability.** Per the doc's decision, the checker SHALL be C++
and committed to this repository: a Wing-Gong / WGL search over the
history, partitioned per key, against a register model with CAS. Only
`linearizable` reads are checked against it; `local` reads are recorded
separately so stale-read findings are attributed correctly.

5.2 **Raft safety.** From `/log`, `/status` and `/elections` across all
nodes, the checker SHALL assert: at most one leader per term over the whole
run; log matching (same index and term implies same entry and same prefix);
leader completeness (every entry committed in a term is present in every
later leader's log); and state-machine safety (no two nodes apply different
commands at the same index).

5.3 **Liveness.** After `run.reset` or after the last fault is healed, a
leader SHALL be elected and `commit_index` SHALL advance within a bound
expressed as a multiple of `ELECTION_TIMEOUT_MAX_MS` and scaled by
`KYTHIRA_TEST_TIMEOUT_SCALE`, for both normal operation and an in-flight
membership change.

5.4 Every checker SHALL first pass against histories and states produced by
the existing scripted `tests/docker_chaos/` scenarios with no agent
involved. A checker that flags a scripted, known-good run is a checker bug
and SHALL be fixed before it is trusted (doc success metric: zero findings
that are harness or checker bugs).

5.5 A failing checker SHALL emit a violation record with the invariant, the
history window, the node states and the schedule-log offset, which the tool
server posts to the board as a fact.

### Requirement 6: reporting

6.1 For each finding the reporter SHALL produce a replay file, a draft
`ChaosCluster` test in `tests/docker_chaos/` that encodes the minimized
schedule and asserts the invariant, and an entry in `doc/chaos-findings.md`
with the date, invariant, minimized schedule, log excerpt, Kythira commit
and Cairn version.

6.2 Nothing SHALL be committed, pushed or merged automatically. Clark
reviews each finding and each draft test.

6.3 A draft test SHALL fail on the commit the finding was made against and
SHALL be confirmed to pass after the fix before it is merged.

### Requirement 7: isolation and safety

7.1 The Cairn panel SHALL bind to `127.0.0.1` only.

7.2 Agent containers SHALL reach the tool server and the configured model
endpoints (the Anthropic API and the local llama.cpp server) and nothing
else: no general internet and no package installs at runtime. The fence
SHALL be built from compose networks marked `internal: true` and an egress
proxy that allowlists the model hosts, so it works under rootless Podman.

7.3 No component SHALL use `--privileged`, host networking or a capability
rootless Podman cannot grant (CLAUDE.md rule 4). `NET_ADMIN` on the cluster
nodes is already required and grantable, and is the only one this spec
needs.

7.4 Agent images SHALL contain only the worker runtime and the tool-server
client. No scanners, fuzzers, offensive-security images or browser
automation.

7.5 The Docker phase SHALL run with no cloud credentials in any container.

7.6 One command SHALL stop every agent and tear down the cluster, the tool
server and the engine.

7.7 Every compose file and script SHALL work with Docker and rootless Podman
alike, using `container_runtime()` / `compose_prefix()` (or
`$KYTHIRA_CONTAINER_RUNTIME` / `$KYTHIRA_COMPOSE_COMMAND` in shell) and
service-name addressing.

### Requirement 8: cost

8.1 Total spend SHALL stay within $20 per month, enforced by a monthly limit
on the API key rather than by convention.

8.2 Model routing SHALL follow the doc's three tiers: a top Claude model for
planning, triage and minimization; a cheaper API model (such as Claude
Haiku) for routine work a local model cannot do; local models on llama.cpp
for step-by-step fault loops and log reading. M1 SHALL measure which tier
each agent role needs and the cost per run, and the run cadence SHALL be set
from that measurement.

8.3 Each run SHALL have a wall-clock limit, starting at 2 hours, after which
the kill switch fires.

### Requirement 9: milestones are gates

9.1 **M0, harness without agents.** Requirements 1-5 are met, the existing
scripted scenarios pass all three checkers, and it is confirmed that Cairn
v0.2.1 can route a worker to a llama.cpp server. The doc infers the Pi
worker as the likeliest route because llama.cpp serves an OpenAI-compatible
API; this is untested until M0 records it.

9.2 **M1, seeded bug.** A build-time-only planted bug (the doc's example:
skip the term check on vote grant) is found in at least 4 of 5 runs within
the run budget. The planted bug SHALL exist only in a separately tagged
image built with an explicit CMake option, SHALL NOT be reachable through
any failpoint the agents can toggle, and SHALL NOT be in any default build.

9.3 **M2, first real hunt.** Every invariant project runs against current
`main` in Docker; anything found is triaged and minimized.

9.4 **M3, flaky cloud test.** A project targets the failing Real Cloud Tests
scenario and tries to make it fail on demand.

9.5 **M4, EC2 target.** M2 repeated on real EC2 with real network faults and
object-store persistence, under a separate AWS profile whose policy allows
only start, stop and modify on instances tagged `cairn-target`. This SHALL
NOT start before M3 is done.

9.6 **M5, routine.** A scheduled monthly run against `main`, appending to
`doc/chaos-findings.md`.

9.7 A milestone SHALL NOT start until the previous one's success metric is
met or the miss is recorded with its reason.

### Success metrics (from the source doc)

- Seeded bug found in at least 4 of 5 M1 runs.
- At least one new confirmed safety or liveness bug by M2 or M4, or a
  documented clean result over a stated number of agent-hours.
- Every confirmed finding has a deterministic replay that fails before the
  fix and passes after.
- Monthly spend under $20.
- Zero findings that turn out to be harness or checker bugs after triage,
  or each one fixed in the harness.
