# Cairn Chaos Testing — Implementation Plan

## Status: In progress (7/14 tasks: 2-8)

**Last Updated**: October 24, 2026. The spec was written from Clark's doc
"Cairn × Kythira: AI-Driven Chaos Testing Spec" and checked against `main`
at `7f4f25b`.

Ordered by the milestones in requirements.md Requirement 9. Tasks 1-8 are
M0 and involve no LLM; they are useful even if Cairn is never run. No task
before 9 needs network access beyond what the existing `tests/docker_chaos/`
suite already uses. Every new source, compose file, Dockerfile and script
carries the CLAUDE.md copyright header. Every container-facing piece works
under Docker and rootless Podman (CLAUDE.md "Container runtime
compatibility").

---

## M0: harness without agents

- [ ] **1. Probe Cairn before building against it.** No repo changes except
  the answers recorded here.
  - [ ] 1a. Pull Cairn v0.2.1 (commit
        `e0ef2f850e5805f824815ee38f049e066deeb7d1`). Record the engine and
        worker image digests, and record how a project declares its tools
        (MCP or otherwise).
  - [ ] 1b. Can any worker type point at a llama.cpp server's
        OpenAI-compatible endpoint without changing Cairn? The doc infers
        the Pi worker; test it. If none can, record that, and design §7's cost model
        is redone before task 11.
  - [ ] 1c. Does the engine run unmodified under rootless Podman? Record
        what breaks, if anything.
  - [ ] Record all answers with the date. An unrecorded probe gets re-run by
        the next person.

- [x] **2. chaos_node control-plane endpoints.** (Requirement 1, design §1.)
  - [x] 2.1 Replace the `GET /log/<i>` stub; add the `GET /log?from=&to=`
        range form.
  - [x] 2.2 Extend `GET /status` with `leader_id`, `last_log_index`,
        `last_log_term`, `snapshot_index` and `config`.
  - [x] 2.3 Add the `GET /elections` ring buffer.
  - [x] 2.4 Add `GET /kv/<k>?mode=linearizable|local`.
  - [x] 2.5 Add `command_type::cas` to `test_key_value_state_machine` and
        `"op":"cas"` to `POST /command`. Leave the existing put/get/del
        encodings untouched, and add a unit case pinning their bytes.
  - [x] 2.6 Add `POST /membership/add` and `/remove`.
  - [x] 2.7 Extend the `parse_*` helpers in `harness.hpp` and their unit
        cases in `fault_control_unit_test.cpp`. Every existing
        `tests/docker_chaos/` scenario still passes.
        Container Scenario Tests run 40033765207 passed under Docker and
        rootless Podman on the PR #614 head (October 23).
  - Notes (October 23, 2026): `GET /status`'s `leader_id` is the node
    itself on a leader, `known_leader()` elsewhere, and `null` before any
    leader is known. `/elections` is backed by `node::leadership_history()`,
    recorded in `become_leader()`, with `at_ms` from the process's steady
    clock. CAS is `command_type::cas = 3` with an expected-present byte so
    "key must be absent" is expressible; it returns one byte, 1 if it
    applied. Membership takes `{"node_id":…}` only: the leader reaches the
    new node through the RPC client's peer resolver
    (`PEER_ADDRESS_TEMPLATE`), as a JOIN=1 node is reached today. The new
    routes build JSON with `boost::json` (`cmd/chaos_node/control_codec.hpp`)
    because their keys, values and ids are caller-chosen strings. Covered by
    `tests/chaos_node_control_test.cpp`, which serves every route from a
    one-node cluster on loopback.

- [x] **3. Harness gaps.** (Requirement 2, design §2.1.)
  - [x] 3.1 A node-table constructor for `ChaosCluster`. The 3-node default
        and every current caller are unchanged.
  - [x] 3.2 `ChaosNode::pause()`/`unpause()`, `apply_delay`, `apply_loss`.
        Each command vector gets a pure builder in `os_faults.hpp` and a
        `harness_unit_test.cpp` case.
  - [x] 3.3 A 5-node compose target for `chaos_node` with service-name
        peers and no static IPs, plus its node table derived from the same
        variables.
  - [x] 3.4 `pause` under rootless Podman on cgroup v1 reports
        `unsupported` rather than succeeding silently.
  - Notes (October 24, 2026): the 5-node target is
    `docker/chaos-5-compose.yml` (host ports 7201-7205, 8281-8285,
    9201-9205; containers `${CHAOS5_CLUSTER:-kythira-chaos5}-<id>`), and
    `chaos5_node_table()` builds the matching table from the same variable.
    `harness_unit_test` parses the compose file and fails if the two drift.
    `ChaosNode::pause()` throws `fault_unsupported` when the runtime's error
    says the host cannot freeze containers; it matches loosely, on "cgroup"
    plus "v1", "freezer", "not available" or "not supported", because the
    wording differs between Podman versions. No cgroup v1 host was
    available, so that path is covered by unit cases over the error text
    only. `five_node_test.cpp` runs in `docker-chaos-tests`: pause and
    replace a leader, delay and loss on a follower, two followers down,
    with election safety checked from every node's `/elections`.

- [x] **4. Workload client `cmd/chaos_workload`.** (Requirement 4, design §2.2.)
  - [x] 4.1 Reads (both modes), writes and CAS over a configurable key set,
        concurrency and rate, as a compose service on `raft_net`.
  - [x] 4.2 JSONL history with `invoke`/`ok`/`fail`/`info`. Timeouts after
        send are `info`, and a process retires after an `info`.
  - [x] 4.3 Leader redirect via `/status`, with every attempt recorded.
  - Notes (October 24, 2026): the history format lives in
    `tests/docker_chaos/history.hpp`, shared with the checkers to come, with
    a `structural_error()` that checks invoke/complete pairing and process
    retirement. The client's outcome rules are in
    `cmd/chaos_workload/workload.hpp`: only a refused connection, a
    `not_leader` refusal, a `400` or a CAS that did not match is `fail`; a
    timeout after sending, a `commit_failed` from a leader, or an
    unrecognised reply is `info`. Every request an operation made is listed
    in its completion's `attempts`. The binary ships in the chaos_node
    image instead of an image of its own, because it shares that image's
    gate and base and a second image would only add a build step. It runs
    as `c5workload` under the `workload` compose profile in
    `docker/chaos-5-compose.yml`, and `five_node_test.cpp` runs it through a
    leader pause.

- [x] **5. Checkers.** (Requirement 5, design §2.3.)
  - [x] 5.1 `linearizability.hpp` (WGL, per-key, register-with-CAS model,
        search budget that yields `unknown`, never `ok`).
  - [x] 5.2 `raft_safety.hpp` (one leader per term from `/elections`, log
        matching, leader completeness, state-machine safety).
  - [x] 5.3 `liveness.hpp` (bound as a recorded multiple of
        `ELECTION_TIMEOUT_MAX_MS × KYTHIRA_TEST_TIMEOUT_SCALE`).
  - [x] 5.4 Unit tests for each one with hand-written histories, including at
        least one known-bad history per invariant that the checker must
        flag.

- [x] **6. Checkers pass on the scripted suite.** (Requirement 5.4.)
  - [x] Collect histories and node state while `safety_assertions_test`,
        `network_degradation_test`, `crash_recovery_test`,
        `election_recovery_test`, `persistence_faults_test` and
        `az_partition_test` run, and run all three checkers on them. Any
        flag is a checker or harness bug and is fixed before going on.
        Record the run here.

        Recorded October 24, 2026, Container Scenario Tests run 40059957978
        (PR #616, head b3f05ae). `ChaosFixture`
        (`tests/docker_chaos/chaos_fixture.hpp`) now runs the workload
        and all three checkers over every case of those six suites. In 11
        cases under Docker and 11 under rootless Podman, every checker
        reported `ok` and none flagged anything:
        - Liveness: a leader within 9 ms of the heal and a commit within
          116 ms, against a 6000 ms bound (20 × 300 ms).
        - Raft safety: 1-3 terms with a leader per case, and 8-139
          committed entries compared.
        - Linearizability: 3-166 checked operations per case over three
          keys, 12-492 history events. The `fail` and local-read counts
          are reported beside them.
        No checker or harness bug surfaced, so none needed fixing. The
        Podman log of `safety_assertions_test` does show two nodes reporting
        `leader` at once. They are in different terms, a deposed leader that
        has not heard yet, and `raft_safety` correctly passes it: its
        election check is per term, and its leader-completeness check skips
        leaders behind the highest sampled term.

- [x] **7. `chaos_ctl` and the schedule log.** (Requirement 3, design §3-§4.)
  - [x] 7.1 One subcommand per tool from Requirement 3.1. `clock_skew`
        returns `unsupported` (design §6).
  - [x] 7.2 Failpoint allowlist generated from
        `include/raft/fault_injection.hpp`, with a check that fails the
        build on drift.
  - [x] 7.3 Append the schedule log before each call and update it after;
        snapshot `/elections` and `/log` before kill and restart.
  - [x] 7.4 `chaos_ctl replay [--repeat N]` and `chaos_ctl minimize`, both
        LLM-free.
  - [x] 7.5 Unit tests over the existing `CmdExecutor`/`HttpGet` stubs.

        Done October 24, 2026. `cmd/chaos_ctl/` holds the logic, and the
        MCP adapter (task 10) will only map tools onto it. Decisions made
        while building it:
        - One process per call, as the adapter will run it, so a run is a
          directory (`run_state.hpp`): `run.json` from `chaos_ctl init`,
          the schedule log, node snapshots, the workload's pid and its
          history. A `flock` serializes calls from several workers into
          one order.
        - The workload runs as its own `chaos_workload` process.
          `WORKLOAD_DURATION_S=0` now means "until SIGTERM", and SIGTERM
          stops it the way the deadline does, between operations. One
          workload per run, so process ids never restart at 0 within a
          history. `workload.start` writes its resolved seed into the
          logged arguments, so a replay runs the same workload.
        - The schedule log appends a second line for each outcome rather
          than rewriting the `started` line (`schedule_log.hpp`). The file
          stays append-only, a crashed call stays on record as `started`,
          and a reader folds the two lines by `seq`.
        - Every tool that changes the cluster runs `raft_safety` after it
          (design §3.3 step 4), including a call that failed part-way.
          Violations carry `schedule_seq`. Reads, `workload.*`, `check.run`
          and an `unsupported` call skip it.
        - 7.2: the names are not in `fault_injection.hpp`, which holds only
          the `fiu_do_on` macros. `scripts/gen-cairn-failpoints.py` reads
          every `fiu_do_on`/`fiu_fail` in `include/raft/` in the four
          families and keeps those `chaos_node` reaches:
          `file_persistence.hpp` and `test_state_machine.hpp`. A new
          header declaring such a name fails the check until it is
          classified. ci.yml's docs job runs `--check`. **Finding:** the
          `raft/network/*` names are evaluated only by
          `simulator_network.hpp`, and chaos_node talks over `tcp_rpc`, so
          they never fire in chaos_node. Requirement 3.2 lists that family,
          but offering names that do nothing would let an agent spend a
          run on a no-op, so the allowlist leaves them out. The same
          applies to `election_recovery_test.cpp`'s
          `election_after_fiu_network_isolation`, which arms them on
          chaos_node and so tests nothing beyond a quiet cluster (left for
          a separate fix; see doc/TODO.md). `raft/objstore/*` waits for an
          object-store target (M4), and `barrier_delay` reads a delay none
          of the three modes can set.
        - `fault.partition` takes the whole partition (`groups`, every node
          once) and replaces the one in place; `heal` removes it. A node
          that is down has no address, so it is refused rather than
          guessed. `fault.delay` and `fault.drop` each replace the node's
          netem qdisc, as `os_faults.hpp` builds them.
        - Replay runs the logged calls at their logged offsets from the
          first call, then stops the workload, heals and runs all three
          checkers, each attempt in its own directory under
          `<run>/replay/`. A log with no recorded violation counts any
          violation as a reproduction. It refuses an image whose id differs
          from the logged one unless `--allow-image-mismatch`.
        - `minimize` is ddmin over the calls that inject or undo a fault.
          Workload and check calls are always kept, and kept calls keep
          their original offsets. `--keep <seqs>` judges one subset an
          agent proposes.
        - The post-step check runs while the workload is still writing,
          and nodes are read one after another. The first CI run flagged
          `leader_completeness` twice under Podman: a follower read after
          the leader reported a commit newer than the leader's sample. It
          was a sampling artefact (the final check and the replay were
          clean), so `collect_node_states` now reads the leader of the
          highest term again after every other node.
        - Each post-step check reads every node's whole log. That is cheap
          now but grows with the run; an incremental read is the fix if
          long runs make it slow.

        `chaos_ctl_unit_test.cpp` (29 cases) covers the toolset, the log
        ordering, snapshots, partitions, the post-step check, the
        workload's lifecycle, `check.run`, ddmin and reproduction counting.
        `chaos_ctl_scenario_test.cpp` drives the binary end to end in
        `docker-chaos-tests` under both runtimes: a known-good schedule
        must leave every checker clean, and its replay must run clean. A
        local rootless Podman run passed every tool except `fault.delay`
        (the sandbox kernel has no netem) and reported `fault.pause` as
        `unsupported` (no cgroup freezer), as designed.

- [x] **8. Filtering proxy and fence.** (Requirements 3.4 and 7, design §3.2
  and §5.)
  - [x] 8.1 Extend `docker/container_api_proxy/` with the
        kill/start/pause/unpause/exec allowlist scoped to this run's
        label. Add a test that every other call gets `403`.
        Notes (October 24, 2026): `PROXY_MODE=chaos_ctl` with
        `PROXY_LABEL=key=value`; quorum mode is unchanged and still the
        default. It forwards `HEAD|GET /_ping`, inspect, `kill` (KILL or
        TERM), `stop?t=`, `start`, `pause`, `unpause`, exec create whose
        `Cmd` is one of the `os_faults.hpp` tc/iptables shapes (no
        `Privileged`, `User`, `Env`, `WorkingDir`, TTY or stdin), and
        exec start and inspect only for exec ids it created; exec start
        is relayed raw, as the CLI hijacks the connection. Each container
        call inspects the container first, refuses unless it carries the
        label, and goes upstream by its id, so a name cannot be
        re-pointed between check and call. Inspect is scoped too, since it
        returns a container's environment. `proxy_test.py` (stdlib
        `unittest`, run in the CI docs job) checks about 120 refused
        calls, that none of them reaches a fake API on a unix socket, and
        the exec round trip. The real `docker` CLI was driven through the
        proxy against rootless Podman's socket: every `os_faults.hpp`
        command, exit codes included, worked, and `ps`, `run`, `rm`,
        `logs`, `restart`, `exec sh`, `exec --privileged/-u/-e` and any
        call on an unlabelled container got 403.
  - [x] 8.2 `docker/cairn/cairn-compose.yml` with `internal: true`
        networks, an egress proxy allowlisting only the model hosts, and
        the panel on `127.0.0.1`. No `--privileged`, no host networking.
  - [x] 8.3 `scripts/cairn/kill.sh`, run under both runtimes.
  - [x] 8.4 Verify the fence from inside a worker container: the model
        hosts are reachable; a general host, package installs and the
        runtime socket are not.
  - Notes on 8.2-8.4 (October 24, 2026):
    - Networks: `raft_net` (nodes, tool server), `control_net` (tool
      server, container API proxy) and `cairn_net` (tool server, egress
      proxy, engine, workers) are `internal: true`. `egress_net` holds
      only the egress proxy. The engine also joins `panel_net`, an
      ordinary bridge, because neither runtime publishes a port from an
      internal network; the engine is not an agent (Requirement 7.2), and
      nothing else is on that network.
    - `docker/cairn/egress_proxy/` is a stdlib Python proxy:
      `CONNECT host:port` and absolute-form `http://` requests to the
      exact `host:port` pairs in `EGRESS_ALLOW`
      (default `api.anthropic.com:443`), 403 for everything else.
    - The tool server image (`docker/cairn/tool_server/`,
      `docker-cairn-tool-server-image`) is `kythira-chaos-node:dev` plus
      `chaos_ctl` and the static docker CLI 29.9.1, checked by sha256
      per architecture. The CLI speaks the same API to Docker and to
      Podman's compatible socket, so the image needs no runtime switch.
      It idles until task 10's MCP adapter.
    - chaos_ctl gained `--layout cairn`: nodes by compose service name
      on their container ports (`NodePorts::host`, persisted in
      `run.json`). `init --image-id` (or `$KYTHIRA_IMAGE_ID`) records the
      image, since the proxy refuses image inspect. `replay` and
      `minimize` refuse a layout the host cannot reach, and take
      `--layout chaos3 --compose docker/docker-compose.yml` to replay a
      fenced run on the host's published cluster, with the same image
      and settings. They also fall back to the `chaos_workload` next to
      them when the recorded one (a path in the tool server image) is
      missing.
    - The engine service is under the `engine` profile and needs
      `CAIRN_ENGINE_IMAGE` and `CAIRN_PANEL_CONTAINER_PORT` until task 1
      records the v0.2.1 digest and panel port. The fence does not
      depend on it.
    - `scripts/cairn/fence-check.sh` (`docker-cairn-fence-tests`, run in
      Container Scenario Tests on both runtimes) checks five things:
      - chaos_ctl in the tool server drives a schedule through the proxy
        with every checker clean.
      - The tool server is refused `ps`, `run`, `rm`, `exec sh`,
        `exec -u 0`, kill or inspect of an unlabelled container, and any
        internet access.
      - `fence-probe`, which sits where a worker sits, passes
        `fence_probe.py`: a verified TLS handshake to the model host
        through the egress proxy; 403 for example.com, pypi, the Alpine
        and Debian mirrors and GitHub; no direct route anywhere; no proxy,
        node or socket reachable; `pip download` and `apk add` both fail.
      - `kill.sh` leaves no container of the run.
      - The run replays on the host.
    - The probe stands in for a worker until task 10 builds the worker
      images, and task 10 reruns it inside each of them.
    - A local rootless Podman run passed all five steps. The model-host
      handshake was skipped there because the sandbox intercepts TLS; its
      `CONNECT` got through.

## M1: seeded bug

- [ ] **9. Seeded-bug image.** (Requirement 9.2, design §8.)
  - [ ] `-DKYTHIRA_CHAOS_SEEDED_BUG=vote_term`, off by default, with a
        configure warning banner, building
        `kythira-chaos-node:seeded-vote-term`. No failpoint enables it.
  - [ ] Show that `chaos_ctl replay` of a hand-written schedule catches it
        before any agent is involved.

- [ ] **10. Worker images, routing and projects.** (Design §5.)
  - [ ] Worker Dockerfiles FROM pinned upstream digests, adding only MCP
        client config.
  - [ ] Three-tier model routing per task 1b's answer.
  - [ ] One project per invariant under `docker/cairn/projects/`, with the
        origin facts and the doc's seed hints.

- [ ] **11. Five M1 runs against the seeded image.**
  - [ ] Success: found in at least 4 of 5 runs within the 2-hour limit.
  - [ ] Record per-role token use and cost per run here (Requirement 8.2),
        and set the run cadence from it.

## M2-M5

- [ ] **12. Reporter and `doc/chaos-findings.md`.** (Requirement 6.)
  - [ ] Replay file, draft `ChaosCluster` test and findings entry per
        finding. Nothing is committed automatically.

- [ ] **13. M2: first real hunt.** Every invariant project against current
  `main` in Docker. Triage and minimize anything found. Each confirmed
  finding's draft test fails before its fix and passes after. A clean
  result is recorded with its agent-hours.

- [ ] **14. M3: flaky Real Cloud Tests scenario.** Point a project at the
  failing scenario and try to make it fail on demand. M4 (EC2, Requirement
  9.5) and M5 (monthly routine) get their own follow-up spec once this is
  done.
