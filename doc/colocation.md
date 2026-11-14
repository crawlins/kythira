# Running services together in `kythira_node`

`kythira_node` runs any of Kythira's services in one process, each as a
module (`.kiro/specs/service-colocation/`). `KYTHIRA_SERVICES` names them;
`kythira_node --help` lists the ones compiled in. Each stand-alone daemon
(`redis_gateway_node`, `dynamodb_gateway_node`, `kinesis_gateway_node`,
`ledger_node`, `iam_node`) is still built and runs the same module code on
its own.

This page covers resource isolation (task 9). Packaging and moving a
service between layouts follow with tasks 10 and 11.

## What each module has to itself

Every module keeps its own:

- **Raft host.** Each module runs its own `multi_raft` host, with its own
  executor stripes, tick thread, placement driver and Raft logs, in its
  own group-id range (`m << 56`), behind the node's one Raft listener. One
  module's backlog of proposals does not queue behind another's.
- **Request threads.** Each module serves its API on its own listener,
  one thread per connection, up to its own connection cap. A flood that
  fills one module's cap is refused at that module's listener and holds no
  other module's threads.
- **Disk floor and memory limits**, enforced as the stand-alone daemon
  enforces them, against the module's own data directory under
  `KYTHIRA_NODE_DATA_DIR`.

| Module | Request threads | Raft tuning prefix | Disk and memory |
|---|---|---|---|
| Redis (`redis`) | `KYTHIRA_REDIS_IO_THREADS`, `_WORKER_THREADS` | `KYTHIRA_REDIS_` | `KYTHIRA_CACHE_MAX_DISK_BYTES`, `KYTHIRA_REDIS_MAX_VALUE_BYTES` |
| DynamoDB (`dynamodb`) | `KYTHIRA_DYNAMODB_MAX_CONNECTIONS` (256) | none read | `KYTHIRA_DYNAMODB_DISK_FLOOR_BYTES` |
| Kinesis (`kinesis`) | `KINESIS_MAX_CONNECTIONS` (256) | `KINESIS_` | `KINESIS_DISK_FLOOR_BYTES`, `KINESIS_MAX_BODY_BYTES` |
| Ledger (`ledger`) | `KYTHIRA_LEDGER_MAX_CONNECTIONS` (512) | `KYTHIRA_LEDGER_` | `KYTHIRA_LEDGER_SNAPSHOT_DISK_MARGIN_BYTES`, `KYTHIRA_LEDGER_MAX_BODY_BYTES` |
| IAM (`iam`) | `KYTHIRA_IAM_MAX_CONNECTIONS` (256) | none read | none |

The stand-alone daemons read their Raft tuning under bare names
(`KYTHIRA_EXECUTOR_STRIPES`, `KYTHIRA_ELECTION_TIMEOUT_MIN_MS`,
`_MAX_MS`, `KYTHIRA_HEARTBEAT_INTERVAL_MS`, `KYTHIRA_RPC_TIMEOUT_MS`,
`KYTHIRA_TICK_INTERVAL_MS`, `KYTHIRA_POLICY_INTERVAL_MS`,
`KYTHIRA_LOG_LEVEL`). In `kythira_node` each module looks first for the
name under its own prefix (`KINESIS_EXECUTOR_STRIPES`,
`KYTHIRA_REDIS_HEARTBEAT_INTERVAL_MS`) and then for the bare name, which
so stays a node-wide default. `KYTHIRA_SHARD_CUTS` names one service's
keys, so in `kythira_node` only the prefixed form is read
(`KINESIS_SHARD_CUTS`, `KYTHIRA_REDIS_SHARD_CUTS`).

Every module reports `kythira_<module>_raft_term_sum` on the node's
`/metrics`: the sum of the current terms of its groups on that member. It
rises with every election one of them starts, so an alert on its rate
catches a module whose heartbeats stall. `kythira_node_iam_test` holds
every Kinesis request thread busy for twenty seconds and checks that
neither Kinesis's nor IAM's sum moves on any member (Requirement 6.3).

## What co-location does not isolate

Modules in one process share that process. Choose stand-alone daemons
when any of this matters:

- **Faults.** A crash, an abort or memory corruption in one module takes
  every module on the node down with it. Raft keeps each service
  available through the loss of one member, but every service loses that
  member at once.
- **Secrets.** Any module can read any other module's memory: IAM's
  unsealing key and decrypted secrets, a gateway's forwarding secret, the
  ledger's signing key, and (once it exists) the virtual HSM's key
  material (the virtual HSM's Requirement 2.5). In-process calls between
  modules are authorized like network requests, but nothing stops code in
  one module from reading another's memory.
- **CPU and memory.** Threads and executors are per module, but the cores,
  the memory allocator and the page cache are the node's. A module that
  spends every core, or exhausts memory, slows or kills the others; the
  caps above bound a module's request threads, not its CPU time. Container
  or cgroup limits apply to the node as a whole.
- **Disk bandwidth.** Each module's disk floor protects its own free
  space check, but every module writes to the same volume unless
  `KYTHIRA_NODE_DATA_DIR`'s subdirectories are mounted separately.
- **Upgrades.** Every module on a node restarts together, on one binary.
