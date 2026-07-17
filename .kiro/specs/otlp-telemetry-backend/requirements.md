# OTLP Telemetry Backend Requirements Document

## Introduction

Kythira defines two pluggable telemetry seams as C++20 concepts:
`kythira::metrics` (`include/raft/metrics.hpp`) and `kythira::diagnostic_logger`
(`include/raft/logger.hpp`). Both are compile-time template parameters on
`raft_types`/`tcp_raft_types` (`metrics_type`, `logger_type`), so a concrete
backend needs no change to `node<Types>` itself — only a new type satisfying
the concept. Today exactly one implementation of each exists:
`noop_metrics` (a zero-cost stub, `include/raft/metrics.hpp`) and
`console_logger` (structured stdout/stderr logging, `include/raft/console_logger.hpp`).
`doc/TODO.md`'s "Metrics Backends" section tracks a list of concrete metrics
backends to add (AWS CloudWatch, Azure Monitor, Prometheus, Telegraf, etc.),
none yet implemented, and its testing-requirement paragraph already names a
"real ... OTel Collector ... container" as one example of a self-provisioned
Docker target other backends could be tested against — but OTLP itself is not
one of the listed backends.

This document specifies two new classes — `otlp_metrics` (satisfying
`kythira::metrics`) and `otlp_logger` (satisfying `kythira::diagnostic_logger`)
— that emit their respective signal as OTLP (OpenTelemetry Protocol) data to a
collector endpoint. OTLP is deliberately vendor-neutral: a single
implementation, pointed at an OpenTelemetry Collector, reaches Prometheus,
Grafana, Honeycomb, Datadog, New Relic, Jaeger, or any other OTLP-speaking
backend the operator's Collector is configured to export to, without Kythira
needing a bespoke integration per vendor. This lets `otlp_metrics`/`otlp_logger`
serve as one implementation covering a large swath of `doc/TODO.md`'s Metrics
Backends wishlist, rather than a narrow, single-vendor entry in it.

Both classes share a single, non-blocking, batching HTTP export engine and are
transported over OTLP/HTTP with JSON encoding — the OTLP wire variant that
needs only an HTTP client and a JSON library, both of which
(`cpp-httplib`, `boost::json`) are already project dependencies. This spec
introduces no new `vcpkg.json` entries: no protobuf, no gRPC. OTLP/gRPC and
OTLP/HTTP protobuf-binary encoding are explicitly out of scope (see design.md
Non-Goals).

Per `doc/TODO.md`'s Metrics Backends section, OTLP is a self-hosted-agent
backend like Prometheus/Telegraf/NetData — it has no single vendor-managed
ingestion endpoint of its own, so per that section's carve-out ("self-hosted-
only agents ... have no such vendor-managed counterpart and so need only the
Docker-based test") this spec's testing requirement is satisfied by a single
Docker-based scenario test against a real, self-provisioned OTel Collector
container; no second, real-vendor-credentialed test is required.

This spec wires `otlp_metrics`/`otlp_logger` into one reference binary,
`chaos_node` (`cmd/chaos_node/`), as opt-in — mirroring the opt-in posture
`.kiro/specs/ca-cluster-rpc-mtls/` used for `tls_tcp_rpc_client`/`server`.
Wiring into other Raft binaries (`ca_cluster_node`, `dns_discovery_node`, etc.)
is out of scope (see design.md Non-Goals).

## Glossary

- **OTLP**: the OpenTelemetry Protocol — a vendor-neutral wire format for
  metrics, logs, and traces, typically sent to an OpenTelemetry Collector.
- **OTLP/HTTP (JSON)**: the OTLP transport used by this spec — `POST` to
  `<endpoint>/v1/metrics` / `<endpoint>/v1/logs` with
  `Content-Type: application/json`, body encoded using OTLP's proto3-JSON
  field mapping (e.g. `resourceMetrics`, `timeUnixNano` as a decimal string).
  Distinct from OTLP/gRPC and from OTLP/HTTP's protobuf-binary encoding,
  neither of which this spec implements.
- **Resource**: the set of attributes (`service.name`, `service.instance.id`,
  etc.) identifying the process emitting telemetry, attached once per export
  request and shared by every metric/log record in it.
- **Data point**: one OTLP measurement. This spec emits three shapes: **Sum**
  (monotonic counter, from `add_one`/`add_count`), **Histogram** (single-
  sample latency observation, from `add_duration`), and **Gauge**
  (instantaneous value, from `add_value`).
- **Aggregation temporality**: whether a Sum/Histogram data point's value
  covers `[start_time, time]` (**delta**) or `[process_start, time]`
  (**cumulative**). This spec always uses **delta** — see Requirement 1.3.
- **Series**: the identity `(metric name, sorted dimension key/value pairs)`
  used to track each Sum/Histogram data point's `start_time_unix_nano` across
  successive `emit()` calls (Requirement 1.3).
- **SeverityNumber**: OTLP's 1-24 integer log-severity scale; this spec maps
  each `kythira::log_level` to the base value of its corresponding 4-wide
  band (Requirement 2.2).
- **Batch exporter**: the shared, internal, non-concept-facing component
  (`otlp_http_batch_exporter`) that queues records, batches them, and POSTs
  them with retry — used by both `otlp_metrics` and `otlp_logger`.

## Requirements

### Requirement 1: `otlp_metrics` satisfies `kythira::metrics`

**User Story:** As a Kythira operator, I want a `metrics_type` I can drop in
that forwards every counter/duration/gauge recording to an OTLP collector, so
I get real dashboards and alerting without writing a CloudWatch- or
Prometheus-specific integration first.

#### Acceptance Criteria

1. `otlp_metrics` SHALL be provided in a new `include/raft/otlp_metrics.hpp`
   and SHALL satisfy `kythira::metrics` (verified by `static_assert`):
   `set_metric_name`, `add_dimension`, `add_one`, `add_count`, `add_duration`,
   `add_value`, and `emit` SHALL all be non-blocking — none SHALL perform
   network I/O on the calling thread (Requirement 3 covers where the I/O
   actually happens).
2. `emit()` SHALL finalize the pending name/dimensions/recorded-value
   accumulated since the last `set_metric_name` call into exactly one OTLP
   data point, chosen by which recording method was last called before
   `emit()`: `add_one`/`add_count` → **Sum** (`isMonotonic: true`,
   `asInt: <count>`); `add_duration` → **Histogram** (`count: 1`,
   `sum: <duration in the configured unit>`, one bucket incremented per
   Requirement 1.4); `add_value` → **Gauge** (`asDouble: <value>`). WHEN no
   recording method was called before `emit()`, the record SHALL be dropped
   (not exported, not queued) rather than sent as a data-point-less metric.
3. Sum and Histogram data points SHALL use **delta** aggregation temporality.
   `otlp_metrics` SHALL track, per series (metric name + sorted dimension
   pairs), the `time_unix_nano` of that series' previous `emit()` (or
   construction time, for a series' first `emit()`) and SHALL use it as the
   next data point's `start_time_unix_nano`. Gauge data points SHALL carry no
   `start_time_unix_nano` (OTLP Gauges do not require one).
4. Histogram data points SHALL use a fixed, documented default set of bucket
   boundaries expressed in milliseconds (covering sub-millisecond to
   multi-second latencies), overridable via `otlp_metrics_config`
   (Requirement 5.1); `add_duration`'s value SHALL be converted to the same
   unit before bucket placement and before being written to `sum`.
5. `otlp_metrics` SHALL be safe to call concurrently from multiple threads
   without external synchronization — `node<Types>`'s own locking (see
   `include/raft/raft.hpp`, "All public methods ... are thread-safe") SHALL
   NOT be relied upon as the sole guarantee, matching `console_logger`'s
   existing choice to own its own internal mutex rather than assume a
   single-threaded caller.
6. Move-construction and move-assignment SHALL be supported (required to
   populate `node_config<Types>::metrics` by value, matching every existing
   `metrics_type`/`logger_type` usage in `cmd/chaos_node/main.cpp`); copy
   SHALL be deleted, matching `console_logger`'s existing convention.

### Requirement 2: `otlp_logger` satisfies `kythira::diagnostic_logger`

**User Story:** As a Kythira operator, I want a `logger_type` that forwards
structured log records to the same OTLP collector already receiving my
metrics, so logs and metrics from the same node correlate in one backend
instead of living in two disconnected systems.

#### Acceptance Criteria

1. `otlp_logger` SHALL be provided in a new `include/raft/otlp_logger.hpp`
   and SHALL satisfy `kythira::diagnostic_logger` (verified by
   `static_assert`): `log(level, message)`, `log(level, message,
   key_value_pairs)`, and the six convenience methods
   (`trace`/`debug`/`info`/`warning`/`error`/`critical`) SHALL all be
   non-blocking.
2. Each `log(...)` call SHALL produce one OTLP LogRecord with
   `time_unix_nano` set to the call's wall-clock time, `body` set to
   `message` (a string `AnyValue`), and `severity_number`/`severity_text` set
   per `kythira::log_level`, using the base value of that level's 4-wide OTLP
   band: `trace`→1 (`"TRACE"`), `debug`→5 (`"DEBUG"`), `info`→9 (`"INFO"`),
   `warning`→13 (`"WARN"`), `error`→17 (`"ERROR"`), `critical`→21
   (`"FATAL"`).
3. The structured overload's `key_value_pairs` SHALL be mapped to OTLP
   `attributes` — one `KeyValue` per pair, each value a string `AnyValue` —
   preserving input order.
4. `otlp_logger` SHALL own its own internal synchronization (mirroring
   `console_logger`'s existing `std::mutex`) and SHALL support an optional
   minimum-severity filter (`set_min_level`/`get_min_level`, matching
   `console_logger`'s existing surface) so operators can suppress `trace`/
   `debug` volume without touching call sites.
5. Move-construction/assignment SHALL be supported; copy SHALL be deleted —
   same rationale as Requirement 1.6.

### Requirement 3: Shared OTLP/HTTP JSON batch-export engine

**User Story:** As a Kythira operator, I want metrics/log recording to never
stall Raft consensus even if my collector is slow, unreachable, or down for
an extended period, so a telemetry-pipeline outage cannot become a
consensus-availability incident.

#### Acceptance Criteria

1. `otlp_http_batch_exporter` SHALL be provided in a new
   `include/raft/otlp_exporter.hpp`, used internally by both `otlp_metrics`
   and `otlp_logger` (composition, not inheritance) so the queueing/
   batching/HTTP-POST/retry logic is implemented exactly once.
2. `emit()`/`log()` (the concept-facing calls) SHALL only ever push a
   finalized record onto an in-process, mutex-protected, bounded queue — no
   call SHALL block on network I/O, a DNS lookup, or a TLS handshake.
3. The queue SHALL have a configurable maximum size
   (`otlp_export_config::max_queue_size`); WHEN full, the oldest queued
   record SHALL be dropped to make room for the new one (drop-oldest), and an
   internal dropped-record counter SHALL be incremented and exposed via an
   accessor for tests/operational visibility — bounding memory growth during
   a sustained collector outage SHALL take priority over not losing any
   individual sample.
4. A single background thread per exporter instance SHALL drain the queue,
   batching up to `otlp_export_config::max_batch_size` records or waiting up
   to `otlp_export_config::flush_interval`, whichever comes first, then
   JSON-encode the batch as an OTLP `ExportMetricsServiceRequest` or
   `ExportLogsServiceRequest` (per Requirement 1/2's mapping) and `POST` it
   to `<endpoint_base_url>/v1/metrics` or `<endpoint_base_url>/v1/logs`
   respectively.
5. A POST that fails at the transport level, or returns HTTP 429, 502, 503,
   or 504, SHALL be retried with exponential backoff
   (`otlp_export_config::retry_backoff_base`, doubling each attempt) up to
   `otlp_export_config::max_retries` times before the batch is dropped and a
   failure is recorded (surfaced via the same dropped-record counter as
   Requirement 3.3, not re-emitted through the exporter itself — an OTLP
   export failure logging itself via `otlp_logger` would risk an
   unbounded retry loop).
6. On destruction, the exporter SHALL signal its background thread to stop,
   attempt one final best-effort flush of any queued records within a
   bounded shutdown timeout (`otlp_export_config::http_timeout`), and then
   join the thread — no thread SHALL be left detached or leaked.
7. The HTTP POST mechanism SHALL be injectable (a template parameter or
   `std::function`-style seam), defaulting to a real `cpp-httplib`-based
   poster, so unit tests can substitute a stub — mirroring the existing
   injectable `CmdExecutor`/`HttpGet`/`HttpPost` pattern already established
   in `tests/docker_chaos/harness.hpp`/`os_faults.hpp` for the same reason
   (deterministic tests with no real network I/O).

### Requirement 4: OTLP Resource attribution

**User Story:** As an operator running a multi-node Kythira cluster pointed
at one shared collector, I want every metric and log record tagged with which
node produced it, so I can filter/group by node in my dashboards instead of
seeing an undifferentiated stream.

#### Acceptance Criteria

1. `otlp_resource` SHALL be provided in `include/raft/otlp_exporter.hpp`,
   carrying `service_name`, `service_instance_id`, an optional
   `service_namespace`, and a list of additional free-form
   attribute key/value pairs.
2. Both `otlp_metrics` and `otlp_logger` SHALL accept an `otlp_resource` at
   construction and SHALL attach it as the single `Resource` covering every
   data point/LogRecord in each export request they send (OTLP's
   `resourceMetrics[].resource` / `resourceLogs[].resource`).
3. WHEN wired into `chaos_node` (Requirement 5), `service_instance_id`
   SHALL default to the node's own `node_id` (stringified) so records from
   different nodes in the same cluster are distinguishable without any
   operator-supplied configuration beyond the node ID it already has.

### Requirement 5: Configuration and `chaos_node` wiring

**User Story:** As an operator running `chaos_node` in Docker (or otherwise),
I want to point it at an OTLP collector using the same environment-variable
configuration convention the rest of `chaos_node` already uses, without OTLP
becoming a hard requirement for nodes that don't want it.

#### Acceptance Criteria

1. `otlp_export_config` (`include/raft/otlp_exporter.hpp`) SHALL carry
   `endpoint_base_url`, an ordered list of extra HTTP headers (for
   collector-required auth, e.g. an API-key header), `max_batch_size`,
   `flush_interval`, `max_queue_size`, `http_timeout`, `max_retries`,
   `retry_backoff_base`, and the Histogram bucket-boundary override from
   Requirement 1.4 — all with documented defaults requiring only
   `endpoint_base_url` to be set.
2. `chaos_node`'s `node_config` (`cmd/chaos_node/config.hpp`) SHALL gain
   optional environment variables — `OTLP_ENDPOINT`, `OTLP_HEADERS` (a
   comma-separated `key=value` list), `OTLP_SERVICE_NAME` (default
   `"kythira-chaos-node"`) — read with the same `get_opt`-style helper
   already used for `chaos_node`'s other optional settings.
3. WHEN `OTLP_ENDPOINT` is set, `cmd/chaos_node/main.cpp` SHALL construct and
   use `otlp_metrics`/`otlp_logger` as `metrics_type`/`logger_type` (via a
   sibling `Types` struct selected at startup, following the exact pattern
   `.kiro/specs/ca-cluster-rpc-mtls/tasks.md` Task 5 and this file's existing
   `tcp_raft_types_with_docker_qm` already establish for "compile-time type
   alias, runtime-selected via a startup branch into `run_node<Types>`").
   WHEN unset, `chaos_node` SHALL fall back to today's `console_logger`/
   `noop_metrics` — OTLP support SHALL be strictly additive and optional.
4. `docker/docker-compose.yml` and `docker/docker-compose.quorum.yml` SHALL
   gain a documented (commented-out, since OTLP is optional) example of the
   new environment variables on at least one node service.

### Requirement 6: Example configuration and documentation

**User Story:** As an operator adopting this backend, I want a working
example collector configuration and documentation showing how to point
`chaos_node` at it, so I'm not reverse-engineering the wire format from
source code — following the same convention every other Cloud Provider
Support/Metrics Backend entry in `doc/TODO.md` already commits to.

#### Acceptance Criteria

1. A new `docker/otel-collector/otel-collector-config.yaml` SHALL be
   provided: an OTLP/HTTP receiver on `4318`, and at minimum a `debug`
   (or `file`) exporter so the collector's received data is directly
   observable without standing up a second downstream backend — the same
   minimal-machinery spirit as `docker/ca-provisioning-compose.yml`'s use of
   stock `alpine:3.20` rather than a purpose-built image.
2. A new `doc/otlp_telemetry_backend.md` (or an addition to an existing,
   clearly-related doc) SHALL document: the environment variables from
   Requirement 5.2, the OTLP/HTTP JSON wire shape this implementation sends
   (Requirements 1-2), how to point `chaos_node` at a real OpenTelemetry
   Collector, and an explicit note on scope — this implements OTLP/HTTP JSON
   only, not OTLP/gRPC or protobuf-binary (design.md Non-Goals).
3. `doc/TODO.md`'s Metrics Backends section SHALL gain a new `[x]` entry for
   OTLP once this spec's tasks are complete, described as covering both
   metrics and logging and noting it satisfies a large share of the section's
   vendor-specific entries indirectly (an operator can reach CloudWatch,
   Prometheus, etc. via a suitably configured Collector) without those
   entries themselves being considered done.

### Requirement 7: Docker-based OTel Collector scenario test

**User Story:** As a maintainer, I want automated proof that `otlp_metrics`/
`otlp_logger` produce data a real OpenTelemetry Collector accepts and
correctly parses — not just that the JSON we construct matches what we
believe the schema to be — so a wire-format regression is caught in CI, not
by an operator's collector rejecting every request in production.

#### Acceptance Criteria

1. A new Docker scenario test SHALL be added at
   `tests/docker_chaos/otlp_collector_test.cpp`, following the existing
   `add_docker_chaos_scenario_test` convention in
   `tests/docker_chaos/CMakeLists.txt` (its own Boost.Test executable, not
   registered directly in CTest, run via a dedicated custom target — see
   Requirement 7.3).
2. A new compose file `docker/otlp-collector-compose.yml` SHALL start one (or
   more) `chaos_node` instances configured via `OTLP_ENDPOINT` (Requirement
   5.2) alongside a real `otel/opentelemetry-collector-contrib` container
   (pulled directly, per the `alpine:3.20` precedent in
   `docker/ca-provisioning-compose.yml` — no custom Dockerfile needed) using
   `docker/otel-collector/otel-collector-config.yaml` (Requirement 6.1)
   configured with a `file` exporter writing to a bind-mounted volume the
   test can read. Per CLAUDE.md's container-runtime-compatibility rules, all
   inter-container addressing SHALL use compose service names, no static
   IPs.
3. The test SHALL drive the running cluster (e.g. submit a command via the
   existing HTTP control plane) to generate real metric/log activity, then
   assert the Collector's exported file contains at least one `resourceMetrics`
   entry with the expected `service.instance.id` and at least one
   `resourceLogs` entry with a recognizable message body — proving the full
   path (recording call → batch exporter → real HTTP POST → real Collector
   OTLP receiver → real Collector parse) works end to end.
4. A `docker-otlp-collector-tests` custom target SHALL be added to
   `tests/docker_chaos/CMakeLists.txt`, following the exact structure of the
   existing `docker-dns-sd-discovery-tests`/`docker-poco-discovery-tests`
   targets (image build dependency, `KYTHIRA_CONTAINER_RUNTIME`/
   `KYTHIRA_COMPOSE_COMMAND` env forwarding), and SHALL be enabled by
   default — it requires only a container runtime, matching every other
   `docker_chaos` scenario test and satisfying `doc/TODO.md`'s blanket
   testing requirement that such tests run in every environment with a
   container runtime available, CI included.
5. Per `doc/TODO.md`'s carve-out for self-hosted-only agents, no second,
   real-vendor-credentialed test SHALL be required for this spec — OTLP has
   no single vendor-managed ingestion endpoint of its own; the Docker-based
   Collector test above is this backend's complete testing requirement.
