# Cairn Chaos Testing — Implementation Plan

## Status: In progress (10/14 tasks: 1-10)

**Last Updated**: October 31, 2026. The spec was written from Clark's doc
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

- [x] **1. Probe Cairn before building against it.** No repo changes except
  the answers recorded here.
  - [x] 1a. Pull Cairn v0.2.1 (commit
        `e0ef2f850e5805f824815ee38f049e066deeb7d1`). Record the engine and
        worker image digests, and record how a project declares its tools
        (MCP or otherwise).
  - [x] 1b. Can any worker type point at a llama.cpp server's
        OpenAI-compatible endpoint without changing Cairn? The doc infers
        the Pi worker; test it. If none can, record that, and design §7's cost model
        is redone before task 11.
  - [x] 1c. Does the engine run unmodified under rootless Podman? Record
        what breaks, if anything.
  - [x] Record all answers with the date. An unrecorded probe gets re-run by
        the next person.
  - Findings (October 28, 2026, probed on tikhov with Podman 5.7.0;
    Docker is not installed there, so only the Podman path was
    exercised). Cairn checked out at `e0ef2f85` and confirmed as tag
    `v0.2.1`.
  - 1a — images and tool model.
    - There is no published "engine" image. `docker-compose.yaml` builds
      both `cairn-server` and `cairn-dispatcher` from the repo-root
      `Dockerfile`, whose base is
      `ghcr.io/astral-sh/uv@sha256:0a04b3cd4eb1797f131b6df7e13bb2add57e25945144ec4543a1fd873b3991e0`
      (`uv:python3.13-trixie`); the built image is ~1.22 GB.
    - Worker image `ghcr.io/oritera/cairn-worker-container:latest`,
      RepoDigest as pulled (`--platform=linux/amd64`)
      `sha256:d41bd6922951e7237f0c1c2cd4451b4bb7d38cf373e2258ae00b841a71fcdfc2`;
      the registry `latest` multi-arch index digest is
      `sha256:d90d40b7c634b8d5dc9464e04b863a185b9efe964ffd2b246f6e5cbc2a2ef053`.
      It is built `FROM kalilinux/kali-rolling:latest`
      (`container/Dockerfile`) with a security toolchain and three agent
      CLIs baked in: Codex 0.118.0, Claude Code 2.1.98, Pi 0.73.0.
    - A project declares NO tools, and Cairn uses no MCP. The server's
      project model (`cairn/src/cairn/server/models.py`) is only Facts,
      Intents, Hints and Settings (timeouts) — no tools field — and the
      only `mcp`/`tool` strings in the tree are vendored Tailwind JS.
      Tools are whatever the worker agent CLI has natively, run inside
      the worker container: the Pi adapter passes
      `--tools read,write,edit,bash,grep,find,ls` with
      `--no-skills --no-extensions`
      (`cairn/src/cairn/dispatcher/workers/adapters/pi.py`), Claude Code
      runs `claude -p --dangerously-skip-permissions` (`claudecode.py`),
      Codex runs `codex exec --dangerously-bypass-approvals-and-sandbox`
      (`codex.py`). Matches the README's "Zero MCP tools, zero RAG".
  - 1b — yes, the Pi worker, with no change to Cairn. `PiDriver`
    (`.../adapters/pi.py`, `_models_json`) writes a pi provider straight
    from env: `baseUrl=$PI_BASE_URL`, `api=$PI_PROVIDER_API`,
    `apiKey=$PI_API_KEY`, and the shipped `dispatch.yaml` already uses
    `PI_PROVIDER_API: "openai-completions"` — the OpenAI
    chat-completions wire API that llama.cpp's `llama-server` serves
    (`/v1/chat/completions`, `/v1/models`). To target llama.cpp, on a
    `type: pi` worker set `PI_BASE_URL=<server>/v1`,
    `PI_PROVIDER_API=openai-completions`, `PI_MODEL=<served model>`, any
    dummy `PI_API_KEY`, and optionally `PI_MODEL_CONTEXT_WINDOW`.
    Evidence: the exact healthcheck argv `PiDriver` builds, run inside
    the worker container with `--network host` pointed at a local stub
    OpenAI endpoint, produced a request at the stub
    (`POST /v1/chat/completions`, 1700 bytes) and pi reported
    `"api":"openai-completions","provider":"cairn","model":"stub"`. The
    other two backends do not fit llama.cpp as wired: Codex hardcodes
    `wire_api="responses"` (OpenAI Responses API) and Claude Code posts
    to `/v1/messages` (Anthropic Messages API); each accepts a custom
    base URL but expects a protocol llama.cpp does not serve, so either
    would need a translating proxy. Design §7's Pi-based cost model
    therefore stands; no redo needed.
  - 1c — the server runs unmodified; the dispatcher does not, as the
    compose file is shipped.
    - `cairn serve` started cleanly under rootless Podman and answered
      `GET /projects` with `[]`.
    - The dispatcher fails at construction: `ContainerManager.__init__`
      calls `docker.from_env()`
      (`cairn/src/cairn/dispatcher/runtime/containers.py:26`) and
      `docker-compose.yaml` bind-mounts `/var/run/docker.sock`, absent on
      a rootless Podman host. Exact error:
      `docker.errors.DockerException: Error while fetching server API`
      `version: ('Connection aborted.', FileNotFoundError(2, 'No such`
      `file or directory'))`.
    - Fixable WITHOUT touching Cairn source: enable Podman's
      Docker-compatible API (`systemctl --user start podman.socket`) and
      mount `$XDG_RUNTIME_DIR/podman/podman.sock` at
      `/var/run/docker.sock` in the dispatcher (or set `DOCKER_HOST`).
      With that it gets past `docker.from_env()` and proceeds into the
      startup healthcheck (worker-container create/exec over the Podman
      API). So M0 needs only a compose/socket adjustment, not a code
      patch; the stock `docker-compose.yaml` will not come up rootless.
    - Both images are amd64 (`--platform=linux/amd64`); tikhov is
      x86_64, so no emulation. Workers use `network_mode: host`, which
      rootless Podman honours.
  - How it was probed (all under `~/scratch/cairn-probe`, outside this
    checkout; every container, the stub, and the user Podman socket were
    stopped and removed afterward):
    - `git clone https://github.com/oritera/Cairn`,
      `git checkout e0ef2f85`, confirmed `git describe --tags` = v0.2.1.
    - `podman pull --platform=linux/amd64 ghcr.io/oritera/cairn-worker-container:latest`,
      digest from `podman image inspect --format '{{index .RepoDigests 0}}'`,
      cross-checked against the ghcr manifest API.
    - `podman build -t cairn-app -f Dockerfile .` for the engine image.
    - Server: `podman run --rm -p 8000:8000 cairn-app uv run cairn serve`
      `--host 0.0.0.0 --no-access-log`, then `curl 127.0.0.1:8000/projects`.
    - 1b: a ~20-line Python `http.server` stub answering
      `/v1/chat/completions` and `/v1/models`, then
      `podman run --rm --network host <worker-image> /bin/sh -lc`
      `<PiDriver script>` with `PI_BASE_URL` on the stub; watched the
      stub log.
    - 1c: `podman run --rm --network host -v <dispatch_mock.yaml>:/cairn/dispatch.yaml`
      `cairn-app uv run cairn dispatch --config dispatch.yaml`
      `--startup-healthcheck-only`, first with no socket (captured the
      error above), then with the Podman socket bind-mounted at
      `/var/run/docker.sock`.

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
          a separate fix; see doc/TODO.md). **Fixed 2026-10-28:**
          `tcp_rpc.hpp` and `tls_tcp_rpc.hpp` now evaluate the five
          `raft/network/send_*` names, both are classified as reached, and
          the allowlist carries them. `raft/objstore/*` waits for an
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

- [x] **9. Seeded-bug image.** (Requirement 9.2, design §8.)
  - [x] `-DKYTHIRA_CHAOS_SEEDED_BUG=vote_term`, off by default, with a
        configure warning banner, building
        `kythira-chaos-node:seeded-vote-term`. No failpoint enables it.
  - [x] Show that `chaos_ctl replay` of a hand-written schedule catches it
        before any agent is involved.
  - Notes (October 28, 2026):
    - **Which check is skipped.** The bug drops the one-vote-per-term
      check (`raft.hpp`, RequestVote Rule 3), so a voter grants every
      candidate in a term and two can win it. Skipping Rule 1 (the stale
      term) instead would be invisible: the grant carries the voter's
      newer term, the candidate steps down, and no checker could ever
      see it. The define reaches only the `chaos_node` target; any other
      value of the option is a configure error.
    - **Image.** `docker-chaos-image` tags a seeded build
      `kythira-chaos-node:seeded-vote-term`, never `:dev`. The chaos3 and
      cairn compose files take `${KYTHIRA_CHAOS_NODE_IMAGE}` (default
      `:dev`), and `chaos_ctl` exports it from the run's recorded image on
      every call, so a seeded run replays on the seeded image. Nothing at
      run time says the binary is seeded, on purpose: the agents read
      `/status` and logs.
    - **Schedule.** `scripts/cairn/seeded-bug-check.sh`: every node's
      AppendEntries sends fail (elections run back to back) and 70% of
      RequestVote sends fail (a candidate reaches the third node but not
      its rival, which then campaigns in the same term). It needs no
      `tc`, so it runs under rootless Podman.
    - **Result (rootless Podman 4, this sandbox).** Seeded image: 3 of 3
      live runs reported `election_safety` (for example "2 nodes won
      term 5"), and replays reproduced it in 9 of 20 attempts, twice
      with log-matching, state-machine-safety and once linearizability
      violations as well. Control (same build without the define): 0
      violations in its live run and 0 of 10 replays. Replay is
      probabilistic here, as design §4 expects; M1's "found" bar
      (task 11) is the agent's, not this schedule's.

- [x] **10. Worker images, routing and projects.** (Design §3.1, §5, §7.)
  - [x] 10.1 The agents' way to the tools. The original wording here
        ("adding only MCP client config") assumed MCP, which task 1a found
        Cairn does not have. Instead `docker/cairn/tool_server/chaos_http.py`
        (stdlib) answers `GET /tools` and `POST /tool/<name>` on
        `cairn_net` and runs `chaos_ctl <name> -`, one call at a time,
        only for names `chaos_ctl tools` lists; the worker image's `chaos`
        command (`docker/cairn/worker/chaos`, Node, no dependencies) calls
        it. The tool server image gains python3 and runs it as its
        entrypoint.
  - [x] 10.2 Worker image `docker/cairn/worker/`: `node:22.21.1-bookworm-slim`
        by digest, Pi 0.73.0 with its tree pinned by `package-lock.json`
        and install scripts skipped, `ripgrep` and `fd-find` (Pi's grep
        and find tools otherwise download them at run time), `chaos`, and
        `PI_OFFLINE=1`. Not FROM Cairn's worker image: it is Kali with a
        security toolchain (Requirement 7.4), and its Codex and Claude Code
        cannot reach llama.cpp anyway (task 1b).
  - [x] 10.3 The dispatcher behind its own proxy. `container_api_proxy`
        gains `PROXY_MODE=cairn_dispatcher`, allowing exactly the calls
        Cairn's `ContainerManager` makes (docker-py 7.1): create only
        `cairn-dispatch-*` / `cairn-startup-healthcheck-*` of the worker
        image running `sleep infinity` on the run's `cairn_net` with no
        other host config (labelled by the proxy), list (filtered),
        inspect, start, stop, remove, `PUT archive`, and unprivileged
        exec with no TTY or stdin. Other containers answer 404.
  - [x] 10.4 Routing (`docker/cairn/dispatch.yaml`): one Pi worker on the
        local model, `max_running_projects: 1` since every project drives
        the one cluster. The API tiers are written out but commented: they
        need a spend-limited key (Requirement 8.1) and their CLI in the
        worker image, and M1 first measures the local tier alone.
  - [x] 10.5 Model server for M1 (added October 28, 2026: no machine here
        has a GPU that can serve a useful model, and Clark chose to rent
        one, capped at $15 for M1).
    - AWS g5.xlarge (A10G, 24 GB VRAM, 4 vCPU, 16 GiB RAM, about
      $1.006/hour on demand in us-east-1), Deep Learning Base AMI
      (Ubuntu 24.04, NVIDIA driver), 80 GB gp3 root volume. Security
      group: SSH only, from one address. Stopped between runs; the
      stopped volume costs about $6/month.
    - llama.cpp built from source with CUDA 13.2 (build 8642541b2).
      Model `unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF`, Q4_K_M,
      18.56 GB.
    - `llama-server --host 127.0.0.1 --port 8080 -ngl 99 -c 32768 -lm none`.
      `-lm none` is required: with mmap the 18.56 GB file thrashes the
      16 GiB of RAM during load. Load takes about 72 s; VRAM 21.0 of
      23.0 GiB; about 130 tokens/s generation.
    - Reached only through the `llm-tunnel` service (an SSH local
      forward to the server's loopback; `CAIRN_LLM_SSH`,
      `CAIRN_LLM_SSH_KEY`). Setup and smoke test cost about $0.72,
      including Cairn's `PiDriver` healthcheck over the tunnel.
  - [x] 10.6 Engine and wiring. `scripts/cairn/build-engine.sh` builds
        Cairn unmodified from the pinned commit (there is no published
        engine image). `cairn-compose.yml` gains the engine (on
        `engine_net` and `panel_net` only), the dispatcher (profile
        `dispatch`), `dispatch-api-proxy`, `llm-tunnel` (profile `llm`), a
        fixed name for `cairn_net`, and a `cairn_data` volume.
        `kill.sh` also removes the dispatcher's workers.
  - [x] 10.7 Projects: `docker/cairn/projects/*.json`, one per invariant
        (election safety, log matching, leader completeness, state-machine
        safety, linearizability, liveness), each with a goal that names
        the checker output that proves it and the doc's seed hints; the
        peer-to-peer and object-store hints are given only when
        `CAIRN_TARGET_FEATURES` says the target has them. `scripts/cairn/
        start-run.sh <project>` starts the stack, makes a fresh run
        directory current, waits for a leader, stops projects left active
        by earlier runs, creates the project with the cluster's
        `cluster.status` as its origin, and arms `kill.sh --after`.
  - Checked (October 28, 2026, rootless Podman 4 in the cloud sandbox,
    with a scripted stand-in for the model, since the GPU host stays
    stopped until task 11):
    - `start-run.sh election-safety` brought the stack up; Cairn's own
      dispatcher created the project's worker through the dispatch
      proxy; Pi, driven by the stand-in, ran `chaos cluster.status` with
      its bash tool; the call went through `chaos_http.py` and
      `chaos_ctl` into the run's `schedule.jsonl`, and its output became a
      fact on the board.
    - From inside a worker: `tool-server` and `llm-tunnel` answer;
      `node1`, `engine` and both API proxies do not resolve. The engine
      was on `cairn_net` in the first wiring and a worker could reach its
      API, which would let an agent write the board it is judged by; it
      now has its own network.
    - The engine's `cairn_data` volume outlives `kill.sh`, and Cairn
      resumed an earlier run's still-active project against the new
      cluster; `start-run.sh` now stops leftover projects first.
    - Under rootless Podman without systemd, healthchecks never run, so a
      `depends_on: service_healthy` on the engine waited forever; the
      dispatcher is started by `start-run.sh` once the engine answers.
    - `kill.sh` removed the worker and every container of the run.
    - Not exercised here: the real build of the worker image's apt step,
      the engine build from the upstream Dockerfile, and `llm-tunnel`
      (the sandbox's proxy refuses the Debian, Alpine, PyPI-mirror and
      ghcr blob hosts). Cairn's `uv.lock` pins every package to the
      mirrors.aliyun.com index, so `build-engine.sh` needs that host
      reachable; tikhov reached it in task 1. Task 11's first run is the
      check for all three.

- [ ] **11. Five M1 runs against the seeded image.** Two campaigns,
  October 29-30, 2026. The first found the bug 0 times in 5; the second,
  with Qwen3.8-27B and a corrected llama-server setup, found it 3 times
  in 5. The bar is 4, so M1 is not yet passed.
  - [ ] Success: found in at least 4 of 5 runs within the 2-hour limit.
        **0 of 5, then 3 of 5.**
  - [x] Record per-role token use and cost per run here (Requirement 8.2),
        and set the run cadence from it. Cost and tokens are recorded
        below; no cadence is set until a campaign passes.
  - First campaign. Setup: tikhov, rootless Podman, `start-run.sh election-safety --hours 2`
    five times in a row against `kythira-chaos-node:seeded-vote-term`, one
    worker tier only (`pi_local`): Qwen3-Coder-30B-A3B-Instruct Q4 in
    llama.cpp's `llama-server` (`-c 32768`) on a rented g5.xlarge, over
    the `llm-tunnel` SSH tunnel.

    | run | found | chaos calls | GPU minutes | what happened |
    |-----|-------|-------------|-------------|---------------|
    | 1 | no | ~288 | 120 | explored, never produced the double vote |
    | 2 | no | 2 | 120 | never left bootstrap |
    | 3 | no | 2 | 120 | never left bootstrap |
    | 4 | no | 2 | 120 | never left bootstrap |
    | 5 | no | 2 | 120 | never left bootstrap |

  - Cause: the harness worked (tunnel, dispatcher, dispatch proxy, tool
    server, `chaos` calls into the schedule log); the model did not. Cairn's
    bootstrap step needs a bare JSON object, and the dispatcher logged
    `bootstrap parse failed ... error=accepted must be true or false` again
    and again: the model answered in prose or with its event stream. This
    is the "local tier alone" measurement that task 10 set up. In
    hindsight the server, not only the model, was at fault: it ran without
    `--jinja`, so the model's chat and tool template was never applied (see
    the second campaign).
  - Cost: about $10 of GPU time for the five runs (10 hours of
    g5.xlarge); about $20.70 for the whole of M1 including setup and an
    idle stretch before the instance's self-stop watchdog was added.
  - Found on the way: the chaos_node image lacked `libldns3`, which
    chaos_node links when the build host has ldns; the nodes did not start
    until `docker/chaos_node/Dockerfile` installed it.
  - Next: a model that keeps to a strict JSON reply (a stronger local
    model, or llama-server's JSON grammar if Pi can ask for it, or one of
    the API tiers of Requirement 8.2 once a spend-limited key exists).
    A run whose bootstrap fails repeatedly should also be stopped early
    rather than left to its 2-hour limit: runs 2-5 spent 8 GPU hours
    stuck there.
  - Second campaign (October 30). Same project, image and host, with
    Qwen3.8-27B (dense, `unsloth/Qwen3.8-27B-GGUF`, `UD-Q4_K_M`, 16.5 GB)
    on a g5.xlarge: 17.4 GB of VRAM at a 32k context. The llama-server
    flags that matter are `--jinja --no-reasoning-preserve
    --reasoning-budget 0`:
    - Without `--jinja` the chat and tool template is not applied, and
      bootstrap failed as in the first campaign.
    - With `--jinja` alone, bootstrap passed in one run and failed in the
      next two: the model's reasoning text leaked into the reply.
    - `--reasoning-format none` must not be added: it leaves an empty
      `<think></think>` in the reply's content, which Cairn cannot parse.
    The run driver restarted a run's project after a bootstrap parse
    failure, up to 3 times, instead of leaving the run stuck.

    | run | found | minutes to violation | chaos calls | tokens | GPU minutes |
    |-----|-------|----------------------|-------------|--------|-------------|
    | 1 | yes | 29 | 737 | ~155k | 29 |
    | 2 | yes | 35 | 865 | ~273k | 35 |
    | 3 | yes | 72 | 2,748 | ~541k | 72 |
    | 4 | no | - | - | ~940k | 120 |
    | 5 | no | - | 2 | - | 21 |

    Tokens are prompt plus completion, taken from llama-server's
    cumulative log, so they are per campaign run rather than per role
    (Pi was the only role). Run 4 bootstrapped and explored for the full
    two hours without producing the double vote. Run 5 failed bootstrap
    three times, so the reasoning leak is rarer but not gone.
  - Replays: each found run's schedule was replayed three times with
    `chaos_ctl replay` and no model. Run 1: the violation reproduced 3 of
    3 times (one attempt also tripped log matching and state-machine
    safety, as two diverging leaders would). Run 2: reproduced 1 of 3
    (that attempt also tripped leader completeness, linearizability, log
    matching and state-machine safety); a low rate is expected, since
    task 9's hand-written schedule reproduced in 9 of 20 replays. Run 3:
    replay in progress (about 6 hours for 2,748 calls). Minimization was skipped: replays wait out each recorded
    fault, so ddmin would take many hours on tikhov.
  - Cost: about $9 for the second campaign; about $30 for all of M1,
    including an availability-zone move when us-east-1a had no g5
    capacity (a snapshot-restored volume reads cold blocks at about
    15-20 MB/s, slower than downloading the model again).
  - Next: the remaining failures are an occasional bootstrap reply that
    is not bare JSON, and timing luck. Constrained JSON decoding for the
    bootstrap reply only would address the first; a longer limit or a
    stronger model the second. A re-run is Clark's call.

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
