# Implementation Plan

Each task is one PR unless noted. Tasks 4-8 can go in any order once task 3
is in.

- [x] 1. Service module interface and registry
  - `service_module`, `caller`, group space and `module_registry` in core
    public headers; build-time registrar; unit tests with two fake modules
  - Done (2026-11-14): `include/raft/service_module.hpp` (the interface,
    `module_numbers`, `group_space`, `caller`, health and metrics) and
    `include/raft/module_registry.hpp` (`module_registry` of typed
    in-process interfaces, `module_catalog` and `module_registrar`, and
    `plan_modules`, which turns `KYTHIRA_SERVICES` into start order or a
    startup error); `tests/service_module_test.cpp`.
  - _Requirements: 1.1-1.3, 7.1_

- [x] 2. Node core
  - `KYTHIRA_NODE_*` configuration, identity and TLS, peer list, logger with
    a `service` field, shared metrics and health endpoint, signals,
    dependency-ordered start and reverse stop, drain before Raft stops
  - Done (2026-11-14): `include/raft/node_core.hpp` (`node_config_from_env`,
    `with_aliases` for a stand-alone daemon's legacy names, `module_log`,
    `node_runner`, health JSON and Prometheus text, stop signals) and
    `include/raft/node_endpoint.hpp` (`/healthz` and `/metrics` on core's
    Beast server); `tests/node_core_test.cpp`.
  - _Requirements: 2.2, 2.4, 2.5, 8.1, 8.3_

- [x] 3. `kythira_node` and group spaces
  - The composite daemon over node core; `KYTHIRA_SERVICES`; module numbers
    and partitioned group ids on the shared listener, a host per module;
    split/merge hooks scoped per module; overlap check
  - Done (2026-11-14): `include/raft/group_range_mux.hpp` (one Raft server
    shared by several multi-raft hosts, routed by group id range, refusing
    overlaps), `include/raft/node_host.hpp` (what a module runs on: node
    id, the shared client, `server_for(space)`),
    `include/raft/kythira_node_host.hpp` (the node's Beast/CBOR transport
    with mutual TLS, and `kythira_node_host`), `cmd/kythira_node/`
    (modules join through the `KYTHIRA_NODE_MODULES` property and link
    whole-archive); `tests/group_range_mux_test.cpp` and
    `tests/kythira_node_host_test.cpp` (two modules with different state
    machines committing over one listener). The module namespace is now
    `kythira::service_node`: `kythira::node` is Raft's node class.
    Requirement 2.2 and 4.2 now say a host per module on one listener,
    rather than one host (design.md "One listener, a host per module").
  - _Requirements: 2.1, 2.3, 4.1-4.4_

- [x] 4. Ledger as a module
  - Adapter over `ledger_host`; `ledger_node` rebuilt as a one-module node
    with its variables as aliases; existing tests unchanged
  - Done (2026-11-14): `services/ledger/cmd/ledger_node/ledger_module.hpp`
    (`ledger_module<Host>`, what `run()` did, behind `service_module`);
    `ledger_node` runs it as a one-module node on its own JSON Beast
    transport in a stand-alone group space, so ids, data directory,
    variables, ports and wire are as before; `kythira_node` links it through
    `kythira_ledger_module`, where `composite_env` fills the ledger's
    node-wide variables from `KYTHIRA_NODE_*` and `ledger_host`'s new
    `group_base` puts its groups at `4 << 56`. `ledger_node_cluster_test`
    passes unchanged; `kythira_node_offers_ledger` checks the module is
    linked; `doc/verifiable_ledger.md` covers running it in
    `kythira_node`.
  - _Requirements: 3.1, 3.2, 3.4, 8.2_

- [x] 5. Kinesis as a module
  - Adapter over the Kinesis gateway; `kinesis_gateway_node` rebuilt;
    publishes `kinesis.put_records`
  - Done (2026-11-14): `services/kinesis/cmd/kinesis_gateway_node/kinesis_module.hpp`
    (`kinesis_module<Host>`, what `run()` did, behind `service_module`);
    `kinesis_gateway_node` runs it as a one-module node on its own CBOR
    Beast transport in a stand-alone group space, so ids, data directory,
    variables, ports and wire are as before; `kythira_node` links it through
    `kythira_kinesis_module`, where `composite_env` fills the node-wide
    variables from `KYTHIRA_NODE_*` and the initial groups sit at
    `3 << 56`. `kinesis.put_records` is `include/raft/module_interfaces.hpp`'s
    `aws_json_endpoint`: a signed PutRecords, PutRecord or
    DescribeStreamSummary goes to the same front end as over the network,
    so the signature and the signer's policies decide
    (`kinesis_in_process.hpp`, `kinesis_in_process_test`); the composite
    settings are checked by `kinesis_node_config_test`.
  - _Requirements: 3.1, 3.2, 3.4, 7.1, 8.2_

- [x] 6. DynamoDB as a module
  - Adapter over the gateway; bootstrap groups and per-voter id ranges
    inside the module's range; `dynamodb_gateway_node` rebuilt; Kinesis
    destination uses the in-process path when Kinesis is co-located
  - Done: `dynamodb_module.hpp` runs the gateway as `dynamodb_module`
    (local, memory or RocksDB serving, started after `kinesis`).
    `run_cluster.hpp` is templated on the host: `cluster_node` maps the
    bootstrap ids and every split's allocated ids through the module's
    group space (`2 << 56` in `kythira_node`), and the strong host keeps
    its own listener. `dynamodb_gateway_node` is the module on a
    `standalone_host` under `node_runner`, with the same variables, ports
    and data layout as before. With Kinesis co-located and no
    `KYTHIRA_DYNAMODB_KINESIS_ENDPOINT`, a streaming destination's signed
    PutRecords go through `kinesis.put_records`. `composite_env.hpp` fills
    the node-wide settings, checked by `dynamodb_node_config_test`;
    `dynamodb_gateway_cluster_test` and `dynamodb_strong_cluster_test`
    run over the templated cluster.
  - _Requirements: 3.1, 3.2, 3.4, 4.1, 7.3, 7.4, 8.2_

- [x] 7. Redis gateway as a module
  - Adapter over the Redis gateway and build cache; `redis_gateway_node`
    rebuilt; embedded IAM replica replaced by the co-located IAM module
    when present, `KYTHIRA_IAM_*` kept for stand-alone
  - Done: `redis_module.hpp` runs the gateway as `redis_module` (number
    1), with the serving side (`redis_serving.hpp`'s `host_serving`)
    picked by `KYTHIRA_CACHE_PERSISTENCE` from one factory per storage,
    each still a `multi_raft` stack in its own translation unit. The
    initial tiling is `space.to_host(1..n)` and the snapshot-isolation
    oracle keeps the stand-alone id's low 56 bits. `redis_gateway_node`
    is the module on its own Beast transport (`standalone_node.hpp`),
    with the same variables, ports, wire and data layout; `composite_env`
    fills the node-wide settings, checked by
    `redis_gateway_node_config_unit_test`. The IAM half waits on task 8:
    until IAM is a module there is nothing co-located to use, so a
    `CONFIG_REDIS_GATEWAY_IAM` build keeps its embedded replica in both
    forms, and task 8 swaps in `iam.evaluator` when it is published.
  - _Requirements: 3.1, 3.2, 3.4, 7.3, 8.2_

- [ ] 8. IAM on the multi-raft host, as a module
  - IAM state machine as a host group; `iam_node --convert-data-dir`;
    publishes `iam.evaluator`; co-located modules authorize through it,
    the Redis module's embedded replica included (task 7)
  - [x] 8a. The module in `kythira_node`: IAM's group on the node's host
    at `5 << 56`, served by `iam_node`'s serving side, now
    `iam_service.hpp` over a Raft node it does not own; `iam.evaluator`
    published, with the action registry locked and merging tables for
    tables added while IAM serves; Kinesis, DynamoDB and Redis take it
    when IAM is co-located instead of an embedded replica;
    `kythira_node_iam_test` (three processes, Kinesis authorizing
    in-process, state kept across a restart)
  - [ ] 8b. `iam_node` on the multi-raft host with
    `--convert-data-dir` for an existing TCP-RPC data directory
  - [ ] 8c. Read replicas as host learners, so stand-alone gateways'
    embedded replicas join a host-based IAM group
  - _Requirements: 5.1-5.3, 7.2_

- [x] 9. Resource isolation
  - Per-module executors and thread counts, disk floors and memory budgets;
    saturation test showing no elections in other modules
  - Each module already had its own host (executor stripes, tick thread)
    and listener; added each listener's connection cap
    (`KINESIS_MAX_CONNECTIONS`, `KYTHIRA_DYNAMODB_MAX_CONNECTIONS`,
    `KYTHIRA_LEDGER_MAX_CONNECTIONS`, `KYTHIRA_IAM_MAX_CONNECTIONS`;
    Redis has its I/O and worker thread counts), the bare Raft tuning
    names read under each module's prefix first in `kythira_node`
    (`module_tuning`, `node_core.hpp`; `KYTHIRA_SHARD_CUTS` prefixed only),
    and `kythira_<module>_raft_term_sum` on `/metrics`. The saturation
    case in `kythira_node_iam_test` floods Kinesis (IAM's evaluator is the
    other module there; DynamoDB joins the node test in task 11) and
    checks neither group's terms move. `doc/colocation.md` lists what is
    and is not isolated.
  - _Requirements: 6.1-6.4_

- [ ] 10. Packaging, compose and docker-chaos
  - `kythira_node` image; three-node DynamoDB + Kinesis + IAM compose file
    by service name; docker-chaos scenario under Docker and rootless Podman
  - _Requirements: 9.1, 9.2, 10.3_

- [ ] 11. Test matrix and docs
  - All-modules in-process cluster test; build legs per module alone and
    all together; `doc/colocation.md` covering what is and is not isolated
    and how to move a service between layouts
  - _Requirements: 3.3, 6.4, 10.1, 10.2, 10.4_
