# Design Document

## Overview

A fourth `persistence_mode`, `s3`, whose stores are object-store engines over
`aws_s3_client`. The parser gains one flag group, `switchable_persistence`
gains one alternative holding either fencing instantiation, `make_store()`
builds the group prefix, and `main` owns the AWS SDK for the whole process.
Neither the engine nor `aws_s3_client` changes.

```
parse_node_options(argv)                                   config.cpp
  ├─ --persistence s3           → _persistence = s3        (Req 1.1)
  ├─ --s3-* flags               → node_options::_s3        (Req 2, 3, 4, 5)
  └─ validation
        ├─ s3: bucket, prefix required; --data-dir refused (Req 1.5, 2.1, 2.2)
        ├─ not s3: any --s3-* flag refused                 (Req 3.5)
        ├─ s3 endpoint → validate_s3_endpoint()            (Req 3.2)   [#599]
        └─ s3 and election-timeout-min < 1000 ms → warning (Req 8)

main()                                                     main.cpp
  ├─ aws_sdk_scope (s3 or ec2-tag)  ─────────────┐         (Req 6)
  ├─ resolve_peers_by_discovery()                │
  ├─ preflight_s3_options()  → engine rejects?   │         (Req 5.3)
  ├─ run_host<Stack>(opt)                        │
  │    store_factory = make_store(opt, group)    │
  │       └─ s3: switchable_persistence{s3_location, s3_options}
  │              ├─ fencing none → unfenced engine
  │              └─ fencing cas  → fenced engine (owner_id = node id text)
  └─ host destroyed  →  ~aws_sdk_scope → Aws::ShutdownAPI ┘
```

### Key design decision: one binary, both fencing modes

Fencing is a template argument of the engine (it is a compile error on a
store that cannot express a precondition). `--s3-fencing` must still be a run
time choice, because multi-raft-host-binary Requirement 1.2 forbids an axis
that needs a rebuild. `aws_s3_client` is a `conditional_key_object_store`, so
both instantiations compile, and `switchable_state` holds one of them behind a
`unique_ptr` beside the existing memory and file alternatives. The cost is a
second instantiation of the engine in one translation unit, which is far
smaller than the transport templates `run_*.cpp` already split across files.

### Key design decision: the host appends `/nodes/<node>`

The file modes need a directory per host, and it is the operator's job to give
each host its own `--data-dir`. Object storage invites the opposite mistake:
one bucket and one prefix for the whole cluster is the natural thing to type,
and three hosts writing `<prefix>/groups/1/term` would interleave three logs.
Under `cas` fencing the second host would be refused, but under `none` it
would corrupt silently. Appending the node id makes the natural invocation
correct under both, and costs an operator who wanted a specific layout one
path level.

`group_path_component()` makes the node id key-safe. In a
`KYTHIRA_NODE_ID_KIND=text` build a node id can contain `/` or `:`, and these
become `_`.

### Key design decision: `combined` hard state by default

`object_persistence_options` defaults to `separate` because existing buckets
use it, and an in-place downgrade from `combined` is refused. That reason does
not apply to a new binary mode with no buckets. `combined` makes every
`save_current_term` / `save_voted_for` pair one PUT instead of up to two, and
those calls are on the election path. Every other knob keeps the struct's
default, in particular `write_behind` off, so a row only ever measures group
commit when it asks to.

### Key design decision: `cas` fencing by default

`cas` adds the owner object at construction and conditional headers on writes
the engine already makes. It adds no round trip to a steady-state write. What
it buys is that a second host started with a reused node id, or a host
restarted while its predecessor is still running, is refused by the store
instead of interleaving logs. For a measurement host that is often started
by scripts, that mistake is likely enough to make the safe choice the
default.

### Key design decision: reuse the backup tool's endpoint rules

`--s3-endpoint` calls `validate_s3_endpoint()` from
`include/raft/aws_endpoint_override.hpp` (object-backup-s3-endpoint task 2),
and the "unset endpoint" path calls `ignored_endpoint_variable_warnings()`.
Writing a second validator would let the two binaries disagree about what a
valid endpoint is, and the `AWS_ENDPOINT_URL` reasoning (CI exports it for
Oracle's build cache) applies to this binary exactly as to the backup tool.
This is the reason this spec depends on #599.

### Key design decision: validate options before creating any group

`object_store_persistence_engine` rejects bad options at construction, with
precise messages. The parser does not repeat those rules (Requirement 5.3).
Instead `main` builds the `object_persistence_options` once and constructs a
throwaway engine over a store that answers every request with an empty
listing, before the transport starts. A rejected option therefore refuses
startup with the engine's own message, rather than surfacing inside
`create_group` for group 1 after the servers are already bound.

The throwaway store is a ten-line `key_object_store` in the host's own code,
not `tests/mock_object_store.hpp`, which is test-only.

### Key design decision: a timing warning, not a refusal

`doc/cloud_object_persistence.md` says to size election timeouts against a
measurement from where the nodes run. Measured S3 PUT p50 is roughly 35 ms
(arm64) to 160 ms (x64) from GitHub runners (task-10 run 39727417123), with a
longer p99, so the default 150 to 300 ms timeout is about one round trip. A
refusal would break the loopback LocalStack and MinIO runs this spec's own
tests depend on, where a round trip is a millisecond. A warning at a 1000 ms
floor tells the operator taking a real-cloud row, and stays out of the way on
loopback.

## Components and Interfaces

### Options (`cmd/multi_raft_node/config.hpp`)

```cpp
enum class persistence_mode : std::uint8_t {
    memory = 0, file_buffered = 1, file_barrier = 2,
    s3 = 3,
};

enum class s3_fencing : std::uint8_t { none = 0, cas = 1 };

struct s3_persistence_options {
    std::string bucket;
    std::string prefix;            // trailing '/' stripped
    std::string region;            // empty: SDK region chain
    std::string endpoint;          // empty: AWS; validated, normalised
    std::chrono::milliseconds timeout{30000};
    s3_fencing fencing{s3_fencing::cas};
    kythira::object_persistence_options engine{};  // hard_state = combined
    /// Which --s3-* flags were given, so Req 3.5 can name them under
    /// another persistence mode.
    std::vector<std::string> flags_seen;
};

struct node_options {
    // ... existing fields ...
    s3_persistence_options _s3{};
};
```

`config.hpp` includes `raft/object_store_persistence.hpp` only for
`object_persistence_options` and its enums, which need no SDK. The parser
therefore compiles, and the unavailable-arm test runs, in a build without the
S3 arm.

### Parser (`cmd/multi_raft_node/config.cpp`)

The `--s3-*` branches go into the shared `apply` lambda, so `--config` files
accept them too. Each records its flag name in `flags_seen`. The enum flags
take the spellings in Requirement 5.1 (`per-entry`, not `per_entry`), the
`0|1` flags use `to_u64` like `--capacity-dry-run`, and integer flags use
`to_u64`.

Validation, after the existing checks:

| Condition | Result |
|---|---|
| `s3` and no `--s3-bucket` / `--s3-prefix` | error naming the flag |
| `s3` and `--data-dir` given | error: the log is not on local disk |
| not `s3` and `flags_seen` non-empty | error naming the first flag and the mode |
| `s3` and `--s3-endpoint` fails `validate_s3_endpoint()` | error, that message |
| `s3` and `--s3-timeout 0` | error |
| `s3` and `election_timeout_min < k_s3_min_election_timeout` | warning |
| `s3` and no endpoint, `AWS_ENDPOINT_URL[_S3]` set | warning naming it |

`k_s3_min_election_timeout` is `std::chrono::milliseconds{1000}`.

`parse_persistence()` accepts `s3` in every build. Whether the arm exists is
checked in `main`, so the parser stays SDK-free and its unit test runs in
every configuration.

### Store (`cmd/multi_raft_node/switchable_persistence.hpp`)

```cpp
#ifdef KYTHIRA_NODE_HAS_S3_PERSISTENCE
using switchable_s3_engine = kythira::object_store_persistence_engine<
    kythira::aws_s3_client, kythira::host_node_id, std::uint64_t, std::uint64_t>;
using switchable_fenced_s3_engine = kythira::fenced_object_store_persistence_engine<
    kythira::aws_s3_client, kythira::host_node_id, std::uint64_t, std::uint64_t>;
#endif

struct switchable_state {
    std::optional<switchable_memory_engine> _memory{};
    std::unique_ptr<switchable_file_engine> _file{};
#ifdef KYTHIRA_NODE_HAS_S3_PERSISTENCE
    std::unique_ptr<switchable_s3_engine> _s3{};
    std::unique_ptr<switchable_fenced_s3_engine> _s3_fenced{};
#endif
};
```

`dispatch` gains two branches. A third constructor takes
`(aws_client_config, bucket, group_prefix, object_persistence_options,
s3_fencing)`, and under `cas` sets `owner_id` from the node id before
constructing.

`durability()` and `durable_through()` must forward to the S3 engines. The
engine answers `synchronous` with `write_behind` off and `barrier` with it on,
and `node` keys its lock release on that answer, so a wrapper that answered
`none` would make the engine unsafe and one that answered `synchronous` would
make `--s3-write-behind 1` stop batching without an error. The barrier
methods (`append_log_entry_sequenced`, `barrier_through`) already go through
`dispatch` and need no change beyond the new branches.

Each group gets its own `aws_s3_client`. The client holds a
`shared_ptr<S3Client>`, and one SDK client per group is simple and keeps
groups independent. Sharing one client across groups is a possible later
optimisation, not part of this spec.

`make_store()`:

```cpp
if (opt._persistence == persistence_mode::s3) {
    const auto host_prefix = opt._s3.prefix + "/nodes/" +
                             kythira::group_path_component(host_node_id_text(opt._node_id));
    return switchable_persistence{s3_client_config(opt._s3), opt._s3.bucket,
                                  kythira::group_key_prefix(host_prefix, group),
                                  opt._s3.engine, opt._s3.fencing, opt._node_id};
}
```

A `persistence_fenced_error` from the constructor propagates out of
`store_factory`. At startup that is inside `create_group` in `run_host`, and
`main` catches it with the message Requirement 4.4 asks for.

### SDK lifetime (`cmd/multi_raft_node/main.cpp`)

```cpp
#ifdef KYTHIRA_HAS_AWS_SDK
struct aws_sdk_scope {
    Aws::SDKOptions options;
    explicit aws_sdk_scope() { Aws::InitAPI(options); }
    ~aws_sdk_scope() { Aws::ShutdownAPI(options); }
};
#endif
```

`main` constructs one in an `std::optional` when `--persistence s3` or
`--discovery ec2-tag` is set, before discovery, and declares it before
anything that owns a store, so it is destroyed last.
`resolve_peers_by_discovery()` loses its own init, shutdown and
`sdk_guard`.

`main` also checks the arm: with `--persistence s3` and no
`KYTHIRA_NODE_HAS_S3_PERSISTENCE`, it refuses with the Requirement 7.2
message before building anything.

### Status surface (`cmd/multi_raft_node/control_server.hpp`)

`persistence_name()` returns `"s3"`, `durable()` is true for `s3`, and the
status JSON gains an `"s3"` object: bucket, host prefix, endpoint (or
`null`), fencing, hard-state, log layout, segment limits, write-behind,
append concurrency, and `flushes`, the sum of `flushes_issued()` over this
host's groups (Requirement 11.7). It also gains a `"fenced_groups"` array (Requirement
4.4). It does not report credentials or the region chain's result.

### Build (`cmd/multi_raft_node/CMakeLists.txt`)

```cmake
if(AWSSDK_FOUND AND _KYTHIRA_GATE_AWS_S3_PERSISTENCE)
    find_package(AWSSDK QUIET COMPONENTS core s3)
    target_compile_definitions(multi_raft_node PRIVATE KYTHIRA_NODE_HAS_S3_PERSISTENCE)
    target_link_libraries(multi_raft_node PRIVATE ${AWSSDK_LINK_LIBRARIES})
endif()
```

It sits beside the existing EC2 block. When both are on, `AWSSDK_LINK_LIBRARIES`
must cover `ec2` and `s3`, so the two `find_package` calls are merged into one
component list built from what is enabled. Task 1 verifies the variable
behaves that way rather than assuming it. `KYTHIRA_HAS_AWS_SDK` is defined in
either case, since `aws_sdk_scope` needs it.

The S3 engines are instantiated in `main.cpp` and in each `run_*.cpp` that
instantiates `switchable_persistence`. If that measurably raises a
`run_*.cpp`'s peak compile memory (they sit at about 5.4 GiB under the
`heavy_tu` pool), the engine members move behind an out-of-line factory in
one new translation unit. Task 4 measures this.

## Error Handling

| Condition | Where | Result |
|---|---|---|
| Missing `--s3-bucket` / `--s3-prefix` | parser | exit, flag named |
| `--data-dir` with `s3`; `--s3-*` with another mode | parser | exit, flag named |
| Bad endpoint shape | parser | exit, validator's message |
| Engine rejects an option | `main` preflight | exit, engine's message |
| Binary built without the S3 arm | `main` | exit, names `CONFIG_AWS_S3_PERSISTENCE` |
| Bucket missing, credentials refused, unreachable | first group's construction | exit, SDK message and group prefix |
| Prefix owned by another host (`cas`) | group construction | exit, fencing message |
| Fenced at run time | engine latch | group writes fail; logged once; `fenced_groups` |
| Election timeout below 1000 ms | parser | warning |

## Testing Strategy

### Unit: options (`tests/multi_raft_node_s3_options_test.cpp`, new)

Compiles `cmd/multi_raft_node/config.cpp` directly, the way
`multi_raft_driver_agreement_test` reaches the binary's headers by path.
Covers each `--s3-*` flag present and absent, every spelling, bad values,
required flags, `--data-dir` refusal, flags under another mode, the same
flags through `--config`, and the timing warning. Registered in every build,
because the parser is SDK-free.

### Unit: unavailable arm

In a build without `KYTHIRA_NODE_HAS_S3_PERSISTENCE`, runs the binary with
`--persistence s3` and asserts the message names the Kconfig symbol.
Registered only in such a build (the `no_cloud` defconfig CI job).

### Stateful S3 mock (`tests/s3_stateful_mock.hpp`, new)

A cpp-httplib server with an in-memory map. It handles PUT (stores the body,
ETag = quoted MD5 hex, honours `If-None-Match: *` and `If-Match` with 412
`PreconditionFailed`, and checks `Content-MD5` with 400 `BadDigest`), GET
(404 `NoSuchKey`), DELETE, and ListObjectsV2 with `prefix`, `max-keys` and
`continuation-token`. It also has a PUT delay knob, a fail-next-N rule by key prefix, and per-prefix
PUT counters, used by the write-behind tests below. It does not verify
signatures, for the reason
`aws_s3_client_unit_test`'s recording mock gives. It binds to `localhost`. It
is a separate header from that recording mock, which programs one answer per
request and cannot hold a cluster's state.

A unit case runs `switchable_persistence`'s S3 arm through the mock under
both fencing modes: write, reload in a fresh instance, read back. Under `cas`
a second instance at the same prefix with a different owner is refused.

### Process: three-host cluster (`tests/multi_raft_node_s3_process_test.cpp`, new)

Starts the mock and three `multi_raft_node` processes on loopback with
`--persistence s3 --s3-endpoint http://localhost:<port> --s3-bucket b
--s3-prefix run`, two groups each, and dummy static AWS credentials in a
scrubbed environment (empty `HOME`, so no `~/.aws` and no IMDS).

1. Wait for every control port to report ready. Write 200 keys through the
   data ports.
2. Stop all three hosts with SIGTERM. Assert the mock holds keys under
   `run/nodes/<id>/groups/<g>/` for every host and group.
3. Restart all three with the same flags. Wait for ready. Read all 200 keys.
4. Start a fourth process with node id 1's id, its own ports and the same
   S3 flags. Assert it exits non-zero with the fencing message, and that
   node 1 still serves reads.

The whole case runs twice, with `--s3-write-behind 0` and `1`. The
write-behind run also reads each host's status surface and asserts it
reports `"write_behind":true`.

### Write-behind tests (Requirement 11)

Group commit is where this binary can go wrong without any error. If
`switchable_persistence` answered the wrong `durability()`, `node` would
either hold its lock through every flush (batching silently off) or treat
staged entries as durable (acknowledged writes lost on a crash). The engine's
own suite cannot catch either, because neither involves the engine. These
tests therefore run through the wrapper and through the binary.

**Mock controls.** `s3_stateful_mock` gains two test knobs, both off by
default: a per-request delay applied to PUT only (so listing and recovery
stay fast), and a fail-next-N rule matching a key prefix that answers 500
`InternalError`. It also counts PUTs per key prefix and records the
concurrent-request high-water mark.

**Unit (wrapper).** In `tests/multi_raft_node_s3_store_test.cpp`, beside the
fencing cases:

- `durability()` over {write-behind off, on} x {none, cas} x {per-entry,
  segmented}: `synchronous` off, `barrier` on.
- Write-behind on, `segmented`: 10 `append_log_entry_sequenced` calls send
  no log PUT; `barrier_through(last)` sends exactly one; `durable_through()`
  is below `last` before and equal to it after; a fresh store reloads 10
  entries.
- The same under `per-entry` with `--s3-append-concurrency 4`: at most 3
  request rounds, read from the mock's high-water mark and PUT count.

**Process (batching).** Three hosts, one group, `--s3-write-behind 1
--s3-log segmented`, mock delay 50 ms on PUT, 16 client threads each writing
distinct keys for a fixed count. The leader is found from the control port.
B = leader commits / leader log PUTs. The assertion is B > 2: with 16
writers and a 50 ms flush, B should be near the writer count, and the margin
keeps a slow CI runner from failing it. The baseline run
(`--s3-write-behind 0 --s3-log per-entry`) asserts B = 1. The test also
asserts the status surface's `flushes` equals the mock's leader log PUT count
under `segmented`, since a flush of at most 16 entries fits one segment at
the default of 100.

**Process (crash).** Same cluster, write-behind on, 50 ms delay. Writers
record each key whose request returned success. After about 2 seconds the
test SIGKILLs the leader, waits for a new leader, restarts the killed host,
waits for all three to be ready, and reads every recorded key. SIGKILL rather
than SIGTERM, because a graceful stop could flush the staged run and hide the
bug this exists to catch.

**Process (failed flush).** Write-behind on. The mock fails the next leader
log PUT once. Writers record successes. The test asserts that every success
is on the mock at the end (by listing the leader's prefix and reading the
keys back), that later writes succeed, and that every success survives a full
restart. A covered client may see an error; the engine reports a failed
flush to every caller it was to cover, and the test accepts that.

**Process (fencing).** The duplicate-host case also runs with write-behind
on. The refused host fails at owner claim during construction, before any
append, so the test compares a mock-side listing of the first host's prefix
before and after and asserts it is unchanged apart from the first host's own
writes.

### CI: real S3-compatible service

A step beside the backup tool's LocalStack (or MinIO) step from
object-backup-s3-endpoint task 7, reusing its service container and bucket
creation. It runs the process test's steps 1 to 3 against
`http://localhost:<port>` instead of the mock, once with write-behind off and
once on, and fails when the service is unhealthy. This is the check that the mock and a real implementation agree on
the conditional-write and listing behaviour the engine depends on.

## Implementation notes (2026-10-23)

What the implementation changed from the design above, and why.

### The CAS owner is `<node-id>@<own Raft URL>`

Requirement 4.3 asked for `owner_id = host_node_id_text(node id)`. That
cannot meet Requirements 4.4 and 10.4. The engine treats a recorded owner
equal to its own `owner_id` as a restart and claims the next epoch, because a
crash-restart and a duplicate are indistinguishable at construction
(`object_store_persistence.hpp`, "The owner object"). With the bare node id,
a second host under a reused id would start, and the fence would fall later
on whichever host wrote second, which can be the original.

The owner is therefore the node id plus this host's own entry in its peer
map, the URL peers dial, which is unique per live process. A restart with the
same flags is the same owner and claims the next epoch (Requirement 1.4), and
a second process under the same id at another address is a different owner,
refused at construction with the engine's message. The process test checks
both, and that the refused host wrote nothing. The cost: a host that comes
back at another address under the same prefix is refused too and needs a
fresh prefix, which run-scoped prefixes already give. The preflight runs
before discovery, when this host may not be in its own peer map, and falls
back to the bare id there; it only needs a non-empty owner.

### The S3 arm is behind an interface from the start

`switchable_persistence` holds a `std::shared_ptr<s3_group_store>`, an
abstract class whose two implementations (one per fencing mode) live in
`cmd/multi_raft_node/s3_store.cpp`, compiled only with the arm. The
`run_*.cpp` units therefore parse no SDK header and instantiate no S3 engine.
Task 4.4's measurement, `run_httplib_cbor.cpp` with g++-13 at `-O1` on the
sandbox: 1,898,032 KiB peak RSS on main before the change and 1,845,832 KiB
after, i.e. no rise. A virtual call per persistence operation is noise beside
the round trip behind it. The shared pointer also lets a process-wide
registry (`s3_store_registry`) hold every live group store weakly, which is
how the status surface sums `flushes_issued()` and lists fenced groups.

### `object_persistence_options` has its own header

`config.hpp` needs the struct and its two layout enums. Including
`object_store_persistence.hpp` for them pulls in `persistence.hpp` and, through
it, the future backend, which would have made the parser and its unit test
need Folly. They moved to `include/raft/object_persistence_options.hpp`,
which the engine header includes, so no existing user changes.

### Smaller points

- `--s3-timeout` is milliseconds like every other duration flag, and
  `aws_client_config::api_timeout` is whole seconds, so it is rounded up.
- `--s3-endpoint` reuses `validate_s3_endpoint()` unchanged; its messages say
  `--endpoint` and the parser rewrites that to `--s3-endpoint`.
- `0|1` flags reject any other number rather than reading it as on.
- B in the batching test is the leader's log growth on the mock (last index
  after minus before, from segment bodies or singleton keys) over its log
  PUTs in the window. Measured locally: 4.3 with write-behind and `segmented`
  (320 entries, 74 PUTs, 74 flushes), exactly 1.0 without and `per-entry`.
- The CI step's script ran locally against LocalStack 4.14.0 under both
  Docker and rootless Podman (task 9.2).
- The status object is on `/describe`, beside the existing `persistence` and
  `durable` fields, which is where this host's status surface is.
