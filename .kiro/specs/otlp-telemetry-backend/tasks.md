# Implementation Plan — OTLP Telemetry Backend

## Status: Complete (not build-verified in this environment — see Notes)

**Last Updated**: July 17, 2026 (implementation pass)

## Overview

Add `otlp_metrics` (satisfying `kythira::metrics`) and `otlp_logger`
(satisfying `kythira::diagnostic_logger`), both emitting OTLP/HTTP JSON to an
operator-configured OpenTelemetry Collector via a shared, non-blocking,
batching export engine; wire both into `chaos_node` as an opt-in
`metrics_type`/`logger_type` pair; and prove the wire format against a real
Collector container in a new `docker_chaos` scenario test.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "otlp_resource + otlp_export_config — foundational data model, no dependents yet"
    },
    {
      "wave": 2,
      "tasks": [2],
      "description": "otlp_http_batch_exporter<Record> + real_http_poster, built against wave 1's config/resource types"
    },
    {
      "wave": 3,
      "tasks": [3, 4],
      "description": "otlp_metrics and otlp_logger, each composing wave 2's exporter for its own Record/signal path; independent of each other"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "chaos_node config.hpp OTLP_* env vars — only needs wave 1's config/resource shape"
    },
    {
      "wave": 5,
      "tasks": [6],
      "description": "chaos_node main.cpp wiring (tcp_raft_types_with_otlp + startup branch) — depends on wave 3's classes and wave 4's config"
    },
    {
      "wave": 6,
      "tasks": [7, 8],
      "description": "Example Collector config + documentation — depend on wave 5's final env-var surface; independent of each other"
    },
    {
      "wave": 7,
      "tasks": [9, 10],
      "description": "Unit tests for mapping correctness and exporter mechanics — depend on waves 2-3; independent of each other"
    },
    {
      "wave": 8,
      "tasks": [11],
      "description": "Docker scenario test + compose file + CMake wiring — depends on wave 5 (chaos_node OTLP support) and wave 6 (Collector config file)"
    }
  ]
}
```

## Tasks

## Phase 1: Shared Export Engine (Tasks 1-2)

- [x] 1. Add `otlp_resource` and `otlp_export_config`
  - New `include/raft/otlp_exporter.hpp`. `otlp_resource` with
    `service_name`/`service_instance_id`/`service_namespace`/
    `extra_attributes` and `to_json()` producing an OTLP `Resource` object's
    `attributes` array.
  - `otlp_export_config` with `endpoint_base_url`, `headers`,
    `max_batch_size`, `flush_interval`, `max_queue_size`, `http_timeout`,
    `max_retries`, `retry_backoff_base`, `histogram_bounds_ms`, each with the
    documented default from design.md.
  - `otlp_series_key` (metric name + sorted dimension pairs) with `operator==`
    and a `std::hash` specialization for use as an `unordered_map` key.
  - _Requirements: 4.1, 5.1_

- [x] 2. Add `otlp_http_batch_exporter<Record>` and `real_http_poster()`
  - Same file. `http_post_result`, `http_poster_fn`, `real_http_poster()`
    (a `cpp-httplib`-backed `httplib::Client`/`SSLClient` POST, selected by
    `endpoint_base_url`'s scheme).
  - `otlp_http_batch_exporter<Record>`: constructor takes
    `otlp_export_config`, `otlp_resource`, the target signal path, an
    `encode` callback (`(resource, span<const Record>) -> boost::json::object`),
    and a `Poster` (defaulted to `real_http_poster()`); `push(Record)`
    (mutex-protected bounded queue, drop-oldest overflow incrementing an
    atomic dropped-count); background thread with the batch-by-size-or-
    interval loop, retry-with-backoff POST, and RAII shutdown (stop signal +
    bounded final flush + join in the destructor); `dropped_record_count()`.
  - Move-constructible/assignable; copy deleted.
  - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

## Phase 2: Concept-Conforming Classes (Tasks 3-4)

- [x] 3. Add `otlp_metrics`
  - New `include/raft/otlp_metrics.hpp`. `pending_metric_record` (name,
    dimensions, one of `{sum_delta{count}, histogram{count, sum, bucket_counts},
    gauge{value}}`); `set_metric_name`/`add_dimension` accumulate into a
    mutex-protected `_pending`; `add_one`/`add_count`/`add_duration`/
    `add_value` each tag `_pending` with the corresponding shape (last call
    wins per Requirement 1.2); `emit()` looks up/updates
    `_series_start_time[key]` for Sum/Histogram (skipped for Gauge), builds
    the finished `pending_metric_record`, calls
    `_exporter.push(std::move(record))`, and resets `_pending` — WHEN no
    recording method was called since the last `set_metric_name`, `emit()`
    is a no-op (nothing pushed).
  - `encode` callback producing `ExportMetricsServiceRequest` JSON per
    design.md's worked example (Sum with `aggregationTemporality:
    AGGREGATION_TEMPORALITY_DELTA`/`isMonotonic: true`; Histogram with
    `explicitBounds`/`bucketCounts`/`count`/`sum`; Gauge with `asDouble`).
  - Histogram bucket placement: linear scan of `config.histogram_bounds_ms`
    (small, fixed-size list — no need for anything cleverer), duration
    converted from `std::chrono::nanoseconds` to milliseconds as a `double`.
  - `static_assert(metrics<otlp_metrics>, ...)`.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 4.2_

- [x] 4. Add `otlp_logger`
  - New `include/raft/otlp_logger.hpp`. `pending_log_record` (time,
    severity_number, severity_text, body, attributes); `log_level` →
    `(severity_number, severity_text)` per design.md's table; `log(level,
    message[, kv_pairs])` and the six convenience methods build a
    `pending_log_record` and call `_exporter.push(...)` directly (no
    multi-call accumulation state needed, unlike `otlp_metrics` — each log
    call is already a complete, self-contained record); `set_min_level`/
    `get_min_level` gate at the top of `log()`, mirroring `console_logger`.
  - `encode` callback producing `ExportLogsServiceRequest` JSON.
  - `static_assert(diagnostic_logger<otlp_logger>, ...)`.
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 4.2_

## Phase 3: `chaos_node` Wiring (Tasks 5-6)

- [x] 5. `chaos_node` OTLP environment variables
  - `cmd/chaos_node/config.hpp`: optional `OTLP_ENDPOINT`, `OTLP_HEADERS`
    (comma-separated `key=value`, parsed the same way `PEERS` is already
    tokenized in this file), `OTLP_SERVICE_NAME` (default
    `"kythira-chaos-node"`), read via the existing `get_opt`-style helper;
    surfaced on `node_config` as `std::optional<otlp_export_config>` +
    `otlp_resource` (or equivalent) ready to hand to the constructors from
    Tasks 3-4.
  - _Requirements: 5.2_

- [x] 6. Wire `otlp_metrics`/`otlp_logger` into `main.cpp`
  - `cmd/chaos_node/main.cpp`: `tcp_raft_types_with_otlp : tcp_raft_types`
    (same shape as the existing `tcp_raft_types_with_docker_qm`) setting
    `logger_type = otlp_logger`, `metrics_type = otlp_metrics`.
  - In `main()`: WHEN `cfg.otlp_endpoint` is set, construct the
    `otlp_export_config`/`otlp_resource` (service_instance_id defaulted to
    `cfg.node_id`, per Requirement 4.3), build `node_config<...>` with
    `otlp_logger`/`otlp_metrics` instances, and call
    `run_node<tcp_raft_types_with_otlp>(...)`; WHEN unset, keep today's
    `run_node<tcp_raft_types>(...)` path with `console_logger`/`noop_metrics`
    unchanged.
  - _Requirements: 5.3, 4.3_

## Phase 4: Example Configuration and Documentation (Tasks 7-8)

- [x] 7. Collector config and compose examples
  - New `docker/otel-collector/otel-collector-config.yaml`: OTLP/HTTP
    receiver on `4318`, `debug` and `file` exporters (`file` writing to a
    path under a mounted volume, for Task 11's scenario test to read).
  - `docker/docker-compose.yml` / `docker/docker-compose.quorum.yml`: add a
    commented-out `OTLP_ENDPOINT`/`OTLP_HEADERS`/`OTLP_SERVICE_NAME` example
    block on at least one node service.
  - _Requirements: 6.1, 5.4_

- [x] 8. Documentation and `doc/TODO.md` update
  - New `doc/otlp_telemetry_backend.md`: env vars (Task 5), wire shape
    (design.md's worked JSON examples), pointing `chaos_node` at a real
    Collector, and the explicit OTLP/HTTP-JSON-only scope note.
  - `doc/TODO.md`'s Metrics Backends section: new `[x]` entry for OTLP,
    describing the metrics+logging scope and the "reaches many backends via
    a suitably configured Collector, without marking those entries done"
    caveat from Requirement 6.3.
  - _Requirements: 6.2, 6.3_

## Phase 5: Tests (Tasks 9-11)

- [x] 9. `tests/otlp_metrics_test.cpp` / `tests/otlp_logger_test.cpp` (new files)
  - Injected stub `Poster` (Requirement 3.7): exact JSON assertions for each
    of `add_one`/`add_count`/`add_duration`/`add_value` and each
    `log_level`; two successive `emit()` calls on the same series confirming
    `start_time_unix_nano` chains correctly (Property 3); `static_assert`
    conformance checks (already present in the headers themselves, exercised
    here too via instantiation); no recording method before `emit()`
    produces no pushed record.
  - _Requirements: 1.2, 1.3, 1.4, 2.2, 2.3_

- [x] 10. `tests/otlp_exporter_unit_test.cpp` (new file)
  - Stub `Poster`: drop-oldest behavior at `max_queue_size`; batch triggers
    on both size and interval boundaries; retry count/backoff timing against
    a poster that fails a configured number of times then succeeds;
    non-retryable status dropped without retry; destructor performs a final
    flush attempt and joins cleanly with no detached thread.
  - _Requirements: 3.2, 3.3, 3.4, 3.5, 3.6_

- [x] 11. `tests/docker_chaos/otlp_collector_test.cpp` (new file, Docker scenario)
  - New `docker/otlp-collector-compose.yml`: `chaos_node` service(s) with
    `OTLP_ENDPOINT` pointed at a compose-service-name address (no static
    IPs, per CLAUDE.md), plus an `otel/opentelemetry-collector-contrib`
    service using Task 7's config file and a shared volume for its `file`
    exporter output.
  - Test: bring the cluster up (reusing `docker_chaos`'s existing
    compose-up/health-wait fixture pattern), drive real activity via the
    existing HTTP control plane, then read the Collector's exported file and
    assert a `resourceMetrics` entry with the expected
    `service.instance.id` and a `resourceLogs` entry with a recognizable
    body are present.
  - `tests/docker_chaos/CMakeLists.txt`: register via
    `add_docker_chaos_scenario_test`, add an `otel-collector`-pull step (no
    custom Dockerfile — pulled directly, per the `alpine:3.20` precedent) and
    a `docker-otlp-collector-tests` custom target following the existing
    `docker-dns-sd-discovery-tests` shape, forwarding
    `KYTHIRA_CONTAINER_RUNTIME`/`KYTHIRA_COMPOSE_COMMAND`; enabled by
    default (no opt-in gate), matching every other `docker_chaos` scenario
    test.
  - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

## Notes

- **Build verification caveat**: this implementation pass could not run
  `vcpkg install`/CMake configure-and-build in the environment it was
  written in (no pre-installed vcpkg dependencies, and bootstrapping
  folly/aws-sdk-cpp/etc. from scratch was judged too slow/heavy to attempt
  here) or the Docker-dependent scenario test. Every file was written and
  manually re-read against this project's established boost::json/httplib/
  concept-conformance idioms (cross-checked against
  `include/raft/ca_http_helpers.hpp`, `console_logger.hpp`, `metrics.hpp`,
  existing `tests/*_test.cpp`), but none of it has been compiled or run yet.
  Treat CI's first real build/test run on this branch as the actual
  first verification pass, not a formality.
- No new `vcpkg.json` dependency: `cpp-httplib` and `boost-json` (both
  already present) are sufficient for OTLP/HTTP JSON. OTLP/gRPC and
  protobuf-binary encoding are explicitly out of scope (design.md
  Non-Goals) — no task in this plan adds `grpc`/`protobuf`.
- `include/raft/raft.hpp`, `include/raft/metrics.hpp`, and
  `include/raft/logger.hpp` are read-only references throughout — no task
  edits any of them; `otlp_metrics`/`otlp_logger` satisfy the existing
  concepts unmodified, the same posture `.kiro/specs/ca-cluster-rpc-mtls/`
  took toward `tcp_rpc.hpp`.
- Wiring into binaries other than `chaos_node` (`ca_cluster_node`,
  `dns_discovery_node`, etc.) is out of scope for this plan — see design.md
  Non-Goals.
