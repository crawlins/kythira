# Requirements Document

## Introduction

`cmd/multi_raft_node` (multi-raft-host-binary) hosts one `multi_raft` per
process. Its `--persistence` axis accepts `memory`, `file-buffered` and
`file-barrier` (`cmd/multi_raft_node/config.cpp`, `parse_persistence()`), and
each group's store is built by `make_store()` in
`cmd/multi_raft_node/switchable_persistence.hpp`.

The tree already ships everything needed to persist a group in S3:
`object_store_persistence_engine` over `aws_s3_client`, with the batched
layout and leader group commit (object-store-batched-layout,
object-store-group-commit). What no binary offers is a way to *run* a node on
it. The engine is reachable only from C++, so every S3 number this project has
(commit rate, PUT latency) comes from the real-cloud test suite's single
engine, never from a Raft cluster serving client traffic.

This spec adds `--persistence s3` to `multi_raft_node`, with flags for the
bucket, prefix, region, endpoint, fencing, layouts and group commit, so that a
Tier C/D/E row can be taken with S3 as the durable log, and so that a cluster
can be exercised end to end on S3 against LocalStack or MinIO with no cloud
account.

### Dependency on object-backup-s3-endpoint

This spec builds on `.kiro/specs/object-backup-s3-endpoint/` (PR #599) and
must not be implemented before these parts of it have landed:

- **Its task 2**, `include/raft/aws_endpoint_override.hpp`.
  `validate_s3_endpoint()` and `ignored_endpoint_variable_warnings()` are
  reused here unchanged, so `--s3-endpoint` follows exactly the same rules,
  messages and `AWS_ENDPOINT_URL` stance as the backup tool's `--endpoint`.
  That header was named for the setting rather than the CLI for this reason.
- **Its task 1**, the measured list of variables the linked aws-sdk-cpp reads
  for the region. This spec's help text names them.
- **Its task 7.1**, which decides whether LocalStack 4.14.0 or a pinned MinIO
  is the CI S3 service (it turns on conditional-write support). This spec's
  CI step uses whichever that task chose.

The spec documents can merge independently of PR #599. Only the
implementation is gated.

### Non-goals

- **Production deployment.** `multi_raft_node` is a measurement host with no
  data-path authentication ("Do not deploy it", `node_usage()`). This makes
  S3 a persistence axis of that host. It does not make the host a product.
- **Other object stores.** Azure Blob, GCS, OCI and OSS persistence would each
  be another arm of the same shape. They are left out so the first arm can
  settle the shape. The flag names below are prefixed `--s3-` so later arms
  can take `--azure-blob-*` and so on without a rename.
- **Cross-group batching.** Each group is its own engine under its own
  prefix, and the object engine does not implement `group_storage`'s batched
  write extension, so N ready groups cost N flushes. Merging groups into one
  object stream is a different engine.
- **Credential flags.** Credentials come from the AWS SDK default chain only,
  as in `aws_s3_client` and the backup tool.
- **`--s3-takeover-epoch`.** The owner id is derived from the node id
  (Requirement 4.3), and a restart by the recorded owner already
  read-increments the epoch. An explicit takeover under a different identity
  is the backup tool's `restore-clone` path.
- **Deleting a merged-away group's objects.** `destroy_group_data()` has no
  object-store counterpart yet. Automatic split and merge are off in this host
  by default (`automatic_split_merge_enabled = false`), and a bucket lifecycle
  rule covers test prefixes.
- **Validating `--discovery-endpoint`.** It would benefit from the same
  validator. It is a separate, one-line follow-up.
- **A real-cloud run.** A Tier D row on real S3 is the natural next step. It
  needs Clark's go under the real-cloud rules, so it is not a task here.

## Glossary

- **S3 arm**: the `persistence_mode::s3` alternative of
  `switchable_persistence`.
- **Host prefix**: `<s3-prefix>/nodes/<node>`, where `<node>` is this host's
  node id made key-safe by `group_path_component()`.
- **Group prefix**: `group_key_prefix(host prefix, group)`, that is
  `<s3-prefix>/nodes/<node>/groups/<group>`. One engine per group prefix.
- **S3 service**: AWS S3, or an S3-compatible service reached through
  `--s3-endpoint`.

## Requirements

### Requirement 1: The `s3` persistence mode

**User Story:** As a maintainer taking a performance row, I want
`--persistence s3` so that S3 is a swept persistence axis that needs no
rebuild, like the three existing modes.

#### Acceptance Criteria

1. `--persistence` SHALL accept `s3`, in the command line and in a
   `--config` file, through the same applier as every other option.
2. WHEN `--persistence s3` is given THEN every group's store SHALL be an
   `object_store_persistence_engine` over `aws_s3_client` at that group's
   group prefix, built by `make_store()`.
3. Groups created after startup (lazy creation under `--capacity-role
   member`, a split under `--split-keys`) SHALL get an S3 store the same way,
   because they go through the same `store_factory`.
4. A host restarted with the same `--s3-bucket`, `--s3-prefix` and
   `--node-id` SHALL reload each group's term, vote, log and snapshot from
   its group prefix, so a cluster survives a full restart with no data loss.
5. `--data-dir` SHALL be neither required nor read under `s3`. Giving it
   SHALL be a configuration error, so that an operator never believes the log
   is on local disk.
6. `--persistence memory|file-buffered|file-barrier` SHALL behave exactly as
   before this spec, with no new requirement on the AWS SDK.

### Requirement 2: Location flags

**User Story:** As an operator, I want to choose the bucket and prefix, and
I want two hosts to be unable to share a prefix by mistake.

#### Acceptance Criteria

1. `--s3-bucket <name>` SHALL be required under `s3`. Its absence SHALL be a
   startup error naming the flag.
2. `--s3-prefix <path>` SHALL be required under `s3`, non-empty after
   trailing slashes are stripped. A run-scoped prefix (for example
   `runs/<run-tag>`) is the expected use.
3. THE host SHALL append `/nodes/<node>` itself to form the host prefix.
   Hosts of one cluster SHALL be able to share one `--s3-bucket` and one
   `--s3-prefix` and still never share a key.
4. THE host SHALL print its host prefix at startup, so a row's log records
   where its state lives.

### Requirement 3: Connection flags

**User Story:** As an operator, I want to target a specific AWS region, or a
LocalStack or MinIO, with the same rules the backup tool uses.

#### Acceptance Criteria

1. `--s3-region <name>` SHALL set `aws_client_config::region`. Absent, the
   region SHALL come from the SDK region chain. An empty value SHALL be an
   error. Its spelling SHALL NOT be checked locally (object-backup-s3-endpoint
   Requirement 3.5).
2. `--s3-endpoint <url>` SHALL set `aws_client_config::endpoint_override`,
   which implies path-style addressing and disables IMDS. It SHALL be
   validated by `validate_s3_endpoint()` before any network call. A shape
   error SHALL refuse startup with that function's message. Its warnings
   (plaintext to a non-loopback host) SHALL be printed on stderr.
3. WHEN `--s3-endpoint` is absent THEN the host SHALL print
   `ignored_endpoint_variable_warnings()` for `AWS_ENDPOINT_URL` and
   `AWS_ENDPOINT_URL_S3`, naming the variable and never its value.
4. `--s3-timeout <ms>` SHALL set `aws_client_config::api_timeout`, default
   30000. Zero SHALL be an error.
5. THE `--s3-*` connection flags SHALL be rejected under any other
   persistence mode, naming the flag and the mode, rather than ignored.

### Requirement 4: Fencing

**User Story:** As a maintainer, I want a second host started with the same
node id to be refused by the store, not to silently interleave two logs.

#### Acceptance Criteria

1. `--s3-fencing none|cas` SHALL choose between
   `object_store_persistence_engine<aws_s3_client, …, fencing_mode::none>`
   and `fenced_object_store_persistence_engine<aws_s3_client, …>` at run
   time. The default SHALL be `cas`.
2. Both instantiations SHALL be compiled into the binary, so the choice needs
   no rebuild (multi-raft-host-binary Requirement 1.2).
3. Under `cas`, `owner_id` SHALL be `host_node_id_text(node id)`. A restart
   by the same node id SHALL claim the next epoch with no flag.
4. WHEN a group's store throws `persistence_fenced_error` at construction
   THEN the host SHALL refuse to start, naming the group, the key and that
   another process holds the prefix. WHEN it is thrown at run time THEN the
   engine's latch already fails every further write; the host SHALL log it
   once per group and report the group as fenced on the control port's status
   surface, and SHALL NOT retry or try to clear the latch.

### Requirement 5: Engine options

**User Story:** As a maintainer, I want every `object_persistence_options`
knob that changes a measured number to be a flag, so a row can sweep it.

#### Acceptance Criteria

1. THE following flags SHALL map one to one onto `object_persistence_options`,
   each defaulting to that struct's own default except where 5.2 says
   otherwise:

   | Flag | Field | Values |
   |---|---|---|
   | `--s3-hard-state` | `hard_state` | `separate`, `combined` |
   | `--s3-log` | `log` | `per-entry`, `segmented` |
   | `--s3-max-segment-entries` | `max_segment_entries` | integer |
   | `--s3-max-segment-bytes` | `max_segment_bytes` | integer |
   | `--s3-write-behind` | `write_behind` | `0`, `1` |
   | `--s3-append-concurrency` | `append_concurrency` | integer |
   | `--s3-write-retries` | `write_retries` | integer |
   | `--s3-snapshot-retention` | `snapshot_retention` | integer |
   | `--s3-verify-checksums` | `verify_checksums` | `0`, `1` |
   | `--s3-max-object-bytes` | `max_object_bytes` | integer |

2. `--s3-hard-state` SHALL default to `combined`. It is the layout that makes
   every hard-state write one PUT, which halves the election-path cost, and
   this binary has no deployed buckets in the `separate` layout to stay
   compatible with. Every other flag SHALL default to the struct's default,
   including `write_behind` off, so a row that turns group commit on says so.
3. A value the engine rejects (for example `--s3-max-segment-entries 1`, or a
   non-default segment size under `per-entry`) SHALL refuse startup with the
   engine's own message, before any group is created. The engine's
   validation SHALL NOT be duplicated in the parser.
4. THE effective options SHALL be printed once at startup and reported on the
   control port's status surface beside the existing persistence mode, so a
   row's record shows the layout and group-commit setting it ran with. That
   surface SHALL report `"persistence":"s3"` and `"durable":true`
   (`control_server.hpp`'s `persistence_name()` and `durable()` today know
   only the three existing modes).

### Requirement 6: SDK lifetime

**User Story:** As a maintainer, I want the AWS SDK initialised exactly once
and shut down only after every S3 client is gone.

#### Acceptance Criteria

1. WHEN `--persistence s3` or `--discovery ec2-tag` is given THEN `main`
   SHALL call `Aws::InitAPI` once before either is used, and
   `Aws::ShutdownAPI` once after the host, and every store it owns, has been
   destroyed.
2. Discovery's own scoped init and shutdown
   (`resolve_peers_by_discovery()`) SHALL be replaced by that process-wide
   guard, so discovery and S3 persistence can be combined.
3. THE guard SHALL run on every exit path, including a startup error after
   some groups' stores were constructed.

### Requirement 7: Build gating

**User Story:** As a maintainer of a no-cloud build, I want this mode to cost
nothing when S3 persistence is off, and to be named when it is missing.

#### Acceptance Criteria

1. THE S3 arm SHALL be compiled only when `AWSSDK_FOUND` and Kconfig
   `AWS_S3_PERSISTENCE` (`_KYTHIRA_GATE_AWS_S3_PERSISTENCE`) both hold. CMake
   SHALL add the SDK's `s3` component to `multi_raft_node`'s link only then,
   and define a `KYTHIRA_NODE_HAS_S3_PERSISTENCE` macro.
2. WHEN the binary lacks the S3 arm THEN `--persistence s3` SHALL refuse
   startup naming the switch to turn on (`CONFIG_AWS_S3_PERSISTENCE=y`, and
   the AWS SDK must be found), not report an unknown mode. `--help` SHALL
   list `s3` either way and mark it unavailable when it is not compiled in.
3. THE SDK `s3` link SHALL NOT be added to any other `cmd/` target.

### Requirement 8: Timing guard

**User Story:** As a maintainer, I want to be told when the election timeout
is shorter than an S3 round trip, because a node that cannot persist its vote
inside the election cannot win it.

#### Acceptance Criteria

1. WHEN `--persistence s3` is given and `--election-timeout-min` is below
   1000 ms THEN the host SHALL print a warning naming the value, the floor,
   and `doc/cloud_object_persistence.md`'s "Election-timeout sizing" section.
2. THE warning SHALL NOT refuse startup: against a loopback LocalStack or
   MinIO a short timeout is legitimate, and CI tests rely on it.
3. THE 1000 ms floor SHALL be one named constant, with a comment citing the
   measured S3 PUT latencies it rests on.

### Requirement 9: Help text and documentation

#### Acceptance Criteria

1. `node_usage()` SHALL document `s3` under `--persistence`, and add an "S3
   persistence" section listing every `--s3-*` flag with its default.
2. `doc/cloud_object_persistence.md` SHALL gain a short "Running a cluster on
   S3" section with a three-host LocalStack or MinIO example and a real-S3
   example, and SHALL note that `multi_raft_node` is a measurement host.
3. `doc/CHANGELOG.md` SHALL gain an entry.

### Requirement 10: Testing

**User Story:** As a maintainer, I want a cluster on S3 proven to elect,
commit, survive a full restart and refuse a duplicate host, with no cloud
account.

#### Acceptance Criteria

1. Unit tests SHALL cover parsing of every `--s3-*` flag (present, absent,
   bad value), the required-flag errors, `--data-dir` being refused under
   `s3`, the `--s3-*` flags being refused under other modes, and the
   unavailable-arm message in a build without the S3 arm.
2. A stateful S3 HTTP mock SHALL be added under `tests/` (PUT, GET, DELETE,
   ListObjectsV2 with continuation, ETag = MD5 of the body, `If-Match` and
   `If-None-Match: *` answered with 412). It SHALL be served on the hostname
   `localhost`, for the reason object-backup-s3-endpoint Requirement 5.2
   gives. A unit test SHALL run the switchable store's S3 arm through it
   under both fencing modes.
3. A process-level test SHALL start three `multi_raft_node` hosts with
   `--persistence s3 --s3-endpoint http://localhost:<port>` against that
   mock, and SHALL: wait for readiness; write keys through the data port;
   stop all three; restart them; and read every key back. It SHALL run once
   with `--s3-write-behind 0` and once with `1`.
4. THE process test SHALL also start a fourth host with a node id already in
   use under `--s3-fencing cas` and assert it refuses to start with the
   fencing message, and that the first host's data is intact.
5. A CI step SHALL run the three-host write, restart and read cycle against
   the S3 service chosen by object-backup-s3-endpoint task 7.1, reusing that
   step's service container, once with `--s3-write-behind 0` and once with
   `1`. It SHALL fail, not skip, when the service is unhealthy, and follow
   CLAUDE.md's container rules (no static IPs, no privileged mode, Docker and
   rootless Podman).
6. Tests 10.1 to 10.4 and Requirement 11's tests SHALL need no cloud account
   and SHALL be registered only when the S3 arm is compiled in, except the
   unavailable-arm case, which is registered only when it is not.

### Requirement 11: Write-behind tests

**User Story:** As a maintainer, I want `--s3-write-behind 1` proven to
batch, to stay durable, and to fail safely through this binary, not only in
the engine's own suite, because the switchable wrapper and the host are
exactly where group commit can be lost silently.

#### Acceptance Criteria

1. **Durability class forwarding.** A unit test SHALL assert that the
   switchable store's S3 arm answers `durability_class::synchronous` with
   write-behind off and `durability_class::barrier` with it on, under both
   fencing modes and both log layouts, and that `durable_through()` follows
   the engine's value rather than a constant.
2. **Staging and flush through the wrapper.** A unit test SHALL append N
   entries through `append_log_entry_sequenced` with write-behind on and
   assert the mock received no log PUT; then call `barrier_through` on the
   last sequence and assert all N entries are on the mock, in one PUT under
   `segmented` and in at most `ceil(N / append_concurrency)` request rounds
   under `per-entry`; and that a fresh store reloads all N.
3. **Batching in a running cluster.** The stateful mock SHALL support an
   injected per-request delay. A process test SHALL run the three-host
   cluster with `--s3-write-behind 1 --s3-log segmented`, the mock delaying
   every PUT by 50 ms, and at least 16 concurrent client writers. It SHALL
   compute B as the leader's committed entries divided by its log PUTs
   (counted by the mock under the leader's host prefix) and SHALL assert
   B > 2. The same run with `--s3-write-behind 0` SHALL assert B = 1 under
   `per-entry`, as the baseline proving the measurement can tell the two
   apart. Both B values SHALL be printed so a regression shows its size.
4. **Acknowledged writes survive a crash.** A process test SHALL run
   concurrent writers against a write-behind cluster, SIGKILL the leader
   mid-load, restart it, and assert that every key a client got a success
   response for is readable afterwards. Keys whose request failed or timed
   out MAY be absent; acknowledged ones SHALL NOT be.
5. **A failed flush never acknowledges.** The stateful mock SHALL support
   failing chosen PUTs with 500. A process test SHALL fail a leader log PUT
   during a write-behind flush and assert: the clients covered by that flush
   get an error or a retried success, never a success for an entry that is
   not on the mock; the next flush succeeds; and after a full restart every
   acknowledged key is readable.
6. **Fencing with write-behind.** The duplicate-host case of 10.4 SHALL also
   run with `--s3-write-behind 1`, and SHALL assert the refused host never
   leaves an object under the first host's prefix.
7. **Status reports group commit.** The control port's status `"s3"` object
   SHALL report `write_behind` and the sum of the engine's `flushes_issued()`
   across this host's groups. The batching test in 11.3 SHALL cross-check
   its mock-side PUT count against that figure. Reading the engine's own
   counter adds no work to the measured path, so it does not conflict with
   multi-raft-performance Requirement 8.2.
