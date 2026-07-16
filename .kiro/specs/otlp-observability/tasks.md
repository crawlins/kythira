# Implementation Plan — OTLP Metrics & Logging

## Status: Not Started

**Last Updated**: July 16, 2026

## Overview

Implement two vendor-neutral observability backends — `otlp_metrics`
(satisfies `kythira::metrics`) and `otlp_logger` (satisfies
`kythira::diagnostic_logger`) — that export via OTLP/HTTP with JSON
encoding, sharing a common non-public batching exporter
(`detail::otlp_exporter`) and a common `otlp_resource` type. See
`requirements.md` for the full acceptance criteria and `design.md` for class
sketches, wire-format examples, and correctness properties.

Reference material to read before starting:
- `include/raft/metrics.hpp` / `include/raft/logger.hpp` — the two concepts
  being implemented against; do not change either.
- `include/raft/rfc2136_dns_sd_discovery.hpp` — the existing
  background-thread + mutex + condition-variable "fresher thread" pattern
  `detail::otlp_exporter`'s background flush thread follows.
- `include/raft/acme_certificate_provider_impl.hpp`'s `make_client()` — the
  existing `httplib::Client` construction/timeout pattern to reuse.
- `tests/acme_test_server.hpp` — the existing mock-HTTP-server test pattern
  `tests/otlp_test_collector.hpp` follows.
- `include/raft/raft.hpp`'s `_metrics.set_metric_name(...)`/`_logger.info(...)`
  call sites — confirms the exact usage shape (single reused member,
  copies captured into async continuations) both new classes must support.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1, 2],
      "description": "Foundational, independent building blocks: shared Resource type and the mock OTLP collector test double"
    },
    {
      "wave": 2,
      "tasks": [3],
      "description": "Shared batching exporter — depends on task 1 (otlp_resource)"
    },
    {
      "wave": 3,
      "tasks": [4, 5],
      "description": "otlp_metrics and otlp_logger — both depend on task 3, independent of each other"
    },
    {
      "wave": 4,
      "tasks": [6, 7],
      "description": "Unit tests for each class — depend on tasks 2 (mock collector) and 4/5 respectively"
    },
    {
      "wave": 5,
      "tasks": [8],
      "description": "Fault injection points (added directly to task 3's exporter) and chaos tests — depends on tasks 3, 6, 7"
    },
    {
      "wave": 6,
      "tasks": [9, 10],
      "description": "Adoption documentation and the optional Docker Collector scenario test — depend on everything above"
    }
  ]
}
```

## Tasks

- [ ] 1. Implement `otlp_resource` in `include/raft/otlp_resource.hpp`
  - Define the struct (`service_name`, `service_instance_id`,
    `service_namespace`, `extra_attributes`) with a defaulted `operator==`
  - `validate(const otlp_resource&)`: throws `std::invalid_argument` when
    `service_name` is empty
  - `to_json_attributes(const otlp_resource&) -> boost::json::array`:
    encodes `service.name`/`service.instance.id`/`service.namespace` (only
    the non-empty ones) plus every `extra_attributes` entry, each as
    `{"key": ..., "value": {"stringValue": ...}}`
  - Add `std::hash<kythira::otlp_resource>` (needed by the exporter
    registry's key type, task 3) — combine hashes of all four fields
  - Verify: unit test constructs valid/invalid resources, asserts the exact
    JSON array shape and key ordering
  - _Requirements: 4.1, 4.2, 5.3_

- [ ] 2. Implement `tests/otlp_test_collector.hpp` mock OTLP/HTTP collector
  - Mirrors `tests/acme_test_server.hpp`'s "construct it and go" shape: an
    `httplib::Server` on an ephemeral port, started on a background thread
    in the constructor, stopped in the destructor
  - Registers `POST /v1/metrics` and `POST /v1/logs` handlers that parse the
    body as `boost::json::value` and append it to two separate
    `std::vector<boost::json::value>` (one per path), guarded by a mutex
  - Exposes `received_metrics()`/`received_logs()` (copies, thread-safe),
    `clear()`, and `base_url()` (`"http://127.0.0.1:<port>"`)
  - Optional: a `respond_with_error(int status)` toggle so tests can
    exercise the failed-export path (Requirement 6.5) without needing the
    fault-injection points from task 8
  - Verify: a throwaway test POSTs a hand-built JSON body and asserts it's
    captured verbatim
  - _Requirements: none directly (test infrastructure only); supports
    verification of 5.1, 5.2, 6.5_

- [ ] 3. Implement `detail::otlp_exporter` in
  `include/raft/detail/otlp_exporter.hpp`
  - `config` struct with the six batching/timeout fields and their defaults
    (design doc §"2. `include/raft/detail/otlp_exporter.hpp`")
  - `get_or_create(resource, endpoint, path, cfg) -> shared_ptr<otlp_exporter>`:
    process-wide registry (`static` `std::mutex` + `std::map<key,
    weak_ptr<otlp_exporter>>`), keyed on `(resource, endpoint, path)`
  - `enqueue(boost::json::value record)`: bounded `std::deque` under a
    mutex; drop-oldest + increment `_dropped` when at
    `max_queued_records`; notify the condition variable
  - Background thread (`run()`): wait on the condition variable for
    `flush_interval` or a queue-size wakeup, drain up to `max_batch_size`
    records, build the `resourceMetrics`/`resourceLogs` envelope (shape
    selected by the `is_metrics` flag derived from `path` at construction),
    POST via `httplib::Client` (constructed once, reused — connection/read
    timeouts from `config`, `extra_headers` applied via
    `Client::set_default_headers` or per-request headers)
  - Failed POST (connection error or non-2xx): drop the batch, emit one
    rate-limited `stderr` line (throttled to at most once per
    `flush_interval`)
  - Destructor: signal shutdown, join thread, one bounded final flush
    attempt (`shutdown_flush_timeout`), swallow all exceptions
  - `dropped_record_count()` accessor
  - Verify: unit test exercises batching-by-size, batching-by-interval,
    backpressure-drops-oldest, and shared-instance identity (two
    `get_or_create()` calls with equal keys return the same `shared_ptr`)
    directly against `otlp_test_collector` from task 2
  - _Requirements: 6.1–6.7_

- [ ] 4. Implement `otlp_metrics` in `include/raft/otlp_metrics.hpp`
  - `config` struct (design doc §"3. `include/raft/otlp_metrics.hpp`")
  - Default ctor → no-op instance (`_exporter == nullptr`); `explicit
    otlp_metrics(config)` → calls `validate(cfg.resource)`, then, if
    `cfg.endpoint` is non-empty, obtains the shared exporter via
    `detail::otlp_exporter::get_or_create(cfg.resource, cfg.endpoint,
    "/v1/metrics", ...)`
  - `set_metric_name`/`add_dimension`/`add_one`/`add_count`/`add_duration`/
    `add_value`: accumulate into the `in_progress_metric` member; every
    method is a no-op when `_exporter == nullptr`
  - `emit()`: encodes per the design doc's expanded `emit()` listing (`sum`
    for `add_one`/`add_count`, `gauge` for `add_duration`/`add_value`),
    calls `_exporter->enqueue(...)`, resets `_current`
  - `static_assert(kythira::metrics<otlp_metrics>)` at the bottom of the
    header
  - Verify: `cmake --build build` succeeds (no build guard, per Requirement
    10)
  - _Requirements: 1.1–1.11, 4.3, 4.4, 5.1, 5.3–5.6, 8.1, 8.3, 8.4, 9.1_

- [ ] 5. Implement `otlp_logger` in `include/raft/otlp_logger.hpp`
  - `config` struct (design doc §"4. `include/raft/otlp_logger.hpp`")
  - Default ctor → no-op instance; `explicit otlp_logger(config)` → calls
    `validate(cfg.resource)`, obtains the shared exporter via
    `get_or_create(cfg.resource, cfg.endpoint, "/v1/logs", ...)` when
    `cfg.endpoint` is non-empty
  - `log_impl(level, message, kvs)`: checks `cfg.min_level` short-circuit,
    builds the `LogRecord` object (severity via
    `detail::otlp_severity_for(level)`, `body`, `attributes`), enqueues
  - Both `log()` overloads and all six convenience methods (each with and
    without `key_value_pairs`) delegate to `log_impl`
  - `static_assert(kythira::diagnostic_logger<otlp_logger>)` at the bottom
    of the header
  - Verify: `cmake --build build` succeeds (no build guard)
  - _Requirements: 2.1–2.7, 3.1, 3.2, 4.3, 4.4, 5.2, 5.3–5.6, 8.2, 8.3, 8.4,
    9.2_

- [ ] 6. Unit tests for `otlp_metrics`
    (`tests/otlp_metrics_unit_test.cpp`)
  - Concept `static_assert` compiles (redundant with the header's own, but
    exercised at the test TU too, matching this project's convention of a
    `concept_satisfied` test case per discovery/provider class)
  - No-op instance: every method callable, mock collector receives nothing
  - `add_one()`/`add_count(n)` → asserts the exact `sum` JSON shape
    (Data Models section) including `AGGREGATION_TEMPORALITY_DELTA`/
    `isMonotonic: true`
  - `add_duration(d)` → asserts `gauge` shape, `asInt` in nanoseconds,
    `unit: "ns"`
  - `add_value(v)` → asserts `gauge` shape, `asDouble`, `unit` from
    `default_unit`
  - Multiple `add_dimension()` calls accumulate; a fresh
    `set_metric_name()` after a non-`emit()`-ted record discards prior
    dimensions (Requirement 1.4)
  - Batching: N `emit()` calls under `max_batch_size` before
    `flush_interval` elapses arrive as one POST with N array entries
  - Backpressure: exceeding `max_queued_records` increments
    `dropped_record_count()`
  - Two `otlp_metrics` instances constructed with equal `resource`/
    `endpoint` interleave records into the same batch (Property 2)
  - _Requirements: 1.1–1.11 (verification), 9.1_

- [ ] 7. Unit tests for `otlp_logger` (`tests/otlp_logger_unit_test.cpp`)
  - Concept `static_assert`; no-op instance behavior; `log()` +
    `key_value_pairs` → exact `LogRecord` JSON shape (Data Models section)
  - One test case per `log_level` asserting the severity-mapping table
    (Requirement 3.1) exactly
  - `min_level` filtering: a configured `min_level` drops lower-severity
    calls before they reach the mock collector
  - Batching/backpressure/shared-instance-identity mirrors task 6's cases
    against the `/v1/logs` path
  - _Requirements: 2.1–2.7 (verification), 3.1, 3.2, 9.2_

- [ ] 8. Fault injection points and chaos tests
  - Add `fiu_do_on("raft/otlp/exporter/flush/fail", ...)` /
    `"raft/otlp/exporter/flush/noop"` to `detail::otlp_exporter`'s flush
    path (in the header from task 3 — this task wires the fault points
    into existing code, it does not restructure it)
  - `tests/chaos/otlp_exporter_chaos_test.cpp`: `flush/fail` asserts
    `dropped_record_count()` increments and no exception reaches the
    `emit()`/`log()` caller; `flush/noop` asserts the mock collector
    receives nothing while calls still return normally; a
    fault-cleared-mid-run case (mirrors the existing DNS discovery chaos
    suites' `fault_disable_mid_run_restores_normal` pattern) confirms
    normal delivery resumes once the fault is disabled
  - _Requirements: 7.1, 7.2_

- [ ] 9. Adoption documentation
  - `include/raft/otlp_metrics.hpp`/`otlp_logger.hpp` file-level Doxygen
    comments include the "Adoption" design-doc snippet (custom
    `raft_types`/`tcp_raft_types` specialization) as a usage example
  - `doc/TODO.md`'s "Metrics Backends" section gains an entry for this spec
    once implemented (out of scope for the spec-authoring PR itself — left
    as this task so the implementation PR doesn't skip it, matching this
    project's established convention of updating `doc/TODO.md` alongside
    the code that completes a listed item)
  - _Requirements: none directly — documentation of Requirements 1, 2, 8's
    resulting public API_

- [ ] 10. Docker OpenTelemetry Collector scenario test (stretch/optional)
  - `docker/otlp-observability-compose.yml`: an `otel/opentelemetry-collector`
    (or `-contrib`) service configured with a `debug` exporter (or `file`
    exporter writing to a shared volume the test reads back), plus a small
    demo binary/container emitting a handful of known metrics/logs via
    `otlp_metrics`/`otlp_logger`
  - Service-name addressing only (no static IPs), `container_runtime()`/
    `compose_prefix()` from `tests/docker_chaos/os_faults.hpp`, no
    `--privileged`/host networking — per `CLAUDE.md`'s container-runtime-
    compatibility rules
  - `tests/docker_chaos/otlp_observability_test.cpp`: brings the compose
    stack up, waits on the collector's healthcheck, asserts the demo
    binary's known metrics/logs appear in the collector's debug/file
    output
  - Verify: `docker compose -f docker/otlp-observability-compose.yml up
    --build` succeeds locally with Docker or rootless Podman
  - _Requirements: none directly — end-to-end validation beyond what the
    in-process mock (task 2) already covers_

## Notes

- Tasks 1 and 2 are independent and can be implemented in parallel; task 3
  depends only on task 1 (task 2's mock collector is test infrastructure
  used starting at task 3's own verification step, not a compile-time
  dependency of any production header).
- Tasks 4 and 5 both depend on task 3 but not on each other — they can
  proceed in parallel once the shared exporter exists.
- Task 8 modifies the header written in task 3 (adds fault points) rather
  than introducing a new file — sequenced after tasks 6/7 only so the
  fault-point chaos tests have the full, already-unit-tested classes to
  exercise, not because the fault points themselves have a code dependency
  on the unit tests.
- Task 10 is explicitly optional per `requirements.md`'s "Out of Scope"
  section — the spec's correctness properties are fully verifiable via the
  in-process mock collector (task 2) without it.
- No task in this plan modifies `include/raft/metrics.hpp`,
  `include/raft/logger.hpp`, or any `cmd/*` binary's default
  `raft_types`/`tcp_raft_types` specialization — see `requirements.md`'s
  "Out of Scope" section.
