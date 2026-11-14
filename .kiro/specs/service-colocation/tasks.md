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

- [ ] 4. Ledger as a module
  - Adapter over `ledger_host`; `ledger_node` rebuilt as a one-module node
    with its variables as aliases; existing tests unchanged
  - _Requirements: 3.1, 3.2, 3.4, 8.2_

- [ ] 5. Kinesis as a module
  - Adapter over the Kinesis gateway; `kinesis_gateway_node` rebuilt;
    publishes `kinesis.put_records`
  - _Requirements: 3.1, 3.2, 3.4, 7.1, 8.2_

- [ ] 6. DynamoDB as a module
  - Adapter over the gateway; bootstrap groups and per-voter id ranges
    inside the module's range; `dynamodb_gateway_node` rebuilt; Kinesis
    destination uses the in-process path when Kinesis is co-located
  - _Requirements: 3.1, 3.2, 3.4, 4.1, 7.3, 7.4, 8.2_

- [ ] 7. Redis gateway as a module
  - Adapter over the Redis gateway and build cache; `redis_gateway_node`
    rebuilt; embedded IAM replica replaced by the co-located IAM module
    when present, `KYTHIRA_IAM_*` kept for stand-alone
  - _Requirements: 3.1, 3.2, 3.4, 7.3, 8.2_

- [ ] 8. IAM on the multi-raft host, as a module
  - IAM state machine as a host group; `iam_node --convert-data-dir`;
    publishes `iam.evaluator`; co-located modules authorize through it
  - _Requirements: 5.1-5.3, 7.2_

- [ ] 9. Resource isolation
  - Per-module executors and thread counts, disk floors and memory budgets;
    saturation test showing no elections in other modules
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
