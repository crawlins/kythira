# Requirements Document

## Introduction

Kythira defines two pluggable observability concepts —
[`kythira::metrics`](../../include/raft/metrics.hpp) and
[`kythira::diagnostic_logger`](../../include/raft/logger.hpp) — but ships only
zero-cost no-op/console implementations (`noop_metrics`, `console_logger`).
`doc/TODO.md`'s "Metrics Backends" section lists several vendor-specific
backends (CloudWatch, Azure Monitor, GCP Cloud Monitoring, Prometheus, ...) as
outstanding work, but none of them is vendor-neutral: each ties a Kythira
deployment to one specific observability product.

This spec defines two new implementations that instead speak the
[OpenTelemetry Protocol (OTLP)](https://opentelemetry.io/docs/specs/otlp/) —
the CNCF-standardized wire protocol understood by the OpenTelemetry
Collector and, through it (or natively), every major observability backend
(Prometheus, Grafana, Datadog, Honeycomb, CloudWatch, Azure Monitor, GCP
Cloud Monitoring, and others). Implementing OTLP once obsoletes the need for
Kythira to ever implement a per-vendor metrics or logging backend again — an
operator points Kythira at an OpenTelemetry Collector (or a vendor's
OTLP-native ingest endpoint) and re-routes from there using the Collector's
own exporter configuration, entirely outside Kythira.

Two new classes are introduced:

- **`otlp_metrics`** (`include/raft/otlp_metrics.hpp`) — satisfies the
  `kythira::metrics` concept; encodes each recorded metric as an OTLP
  `Metric` and exports it via OTLP/HTTP.
- **`otlp_logger`** (`include/raft/otlp_logger.hpp`) — satisfies the
  `kythira::diagnostic_logger` concept; encodes each log call as an OTLP
  `LogRecord` and exports it via OTLP/HTTP.

Both share a common export transport (background batching, HTTP POST,
retry-free best-effort delivery) and a common OTLP `Resource` (the
attributes identifying *this process* to the backend — `service.name`,
`service.instance.id`, etc.), factored into shared, non-public
implementation detail so the two classes do not duplicate wire-format or
transport code.

This spec covers **encoding and exporting** logs and metrics via OTLP/HTTP.
It does not cover OTLP traces/spans (Kythira has no tracing concept today —
see "Out of Scope" below), does not change the `metrics`/`diagnostic_logger`
concepts themselves (`include/raft/metrics.hpp`, `include/raft/logger.hpp`),
and does not wire either class into any `cmd/*` binary's default
`raft_types`/`tcp_raft_types` specialization (operators opt in by supplying
`otlp_metrics`/`otlp_logger` as their own `metrics_type`/`logger_type` — see
the design doc's "Adoption" section).

## Glossary

- **OTLP**: OpenTelemetry Protocol — the wire format and semantics this spec
  implements. Defined by the
  [OTLP specification](https://opentelemetry.io/docs/specs/otlp/) and the
  [`opentelemetry-proto`](https://github.com/open-telemetry/opentelemetry-proto)
  schema.
- **OTLP/HTTP**: The HTTP transport binding of OTLP — `POST` requests to
  `{endpoint}/v1/metrics` and `{endpoint}/v1/logs`, in either Protobuf or
  JSON encoding. This spec uses **JSON encoding only** (see Requirement 2);
  Protobuf encoding is explicitly out of scope (see "Out of Scope").
- **Resource**: The OTLP data-model concept identifying the entity
  producing telemetry — a flat set of key/value attributes
  (`service.name`, `service.instance.id`, ...) attached once per export
  request, shared by every metric/log record in that request.
- **Instrumentation Scope**: The OTLP data-model concept identifying the
  library that produced the telemetry (distinct from the `Resource`, which
  identifies the *application*) — a `{name, version}` pair.
- **`ResourceMetrics` / `ResourceLogs`**: The top-level OTLP/HTTP request
  body shape — one `Resource`, containing one or more `ScopeMetrics`/
  `ScopeLogs`, each containing one `InstrumentationScope` and a list of
  `Metric`/`LogRecord` entries.
- **`Sum`**: The OTLP metric type for monotonic counters — a list of
  `NumberDataPoint`s plus an `aggregationTemporality` and `isMonotonic` flag.
- **`Gauge`**: The OTLP metric type for instantaneous, non-cumulative
  values — a list of `NumberDataPoint`s with no temporality/monotonicity
  concept.
- **`NumberDataPoint`**: One OTLP metric sample — `attributes`,
  `timeUnixNano`, and exactly one of `asInt`/`asDouble`.
- **`LogRecord`**: One OTLP log entry — `timeUnixNano`, `severityNumber`,
  `severityText`, `body` (an `AnyValue`), and `attributes`.
- **`AnyValue`**: The OTLP data-model's tagged-union value type
  (`stringValue`, `intValue`, `boolValue`, `doubleValue`, `arrayValue`,
  `kvlistValue`, `bytesValue`). This spec only ever produces `stringValue`
  (see Requirement 5.4 — the `diagnostic_logger` concept is string-typed).
- **Protobuf JSON mapping**: The
  [canonical JSON encoding rules for Protobuf messages](https://protobuf.dev/programming-guides/json/)
  that OTLP/HTTP's JSON variant inherits — most relevantly here, that
  64-bit integer fields (`int64`, `uint64`, `fixed64`, and therefore every
  OTLP `*UnixNano` timestamp and every `Sum` data point's `asInt`) are
  encoded as JSON **strings**, not JSON numbers, to avoid precision loss in
  JSON parsers that use IEEE-754 doubles (e.g. JavaScript).
- **Batching exporter**: The shared background component (non-public,
  `include/raft/detail/otlp_exporter.hpp`) that queues encoded OTLP records
  and periodically POSTs them as a single batched request, so
  `otlp_metrics`/`otlp_logger` call sites never block on network I/O.

## Requirements

### Requirement 1: `otlp_metrics`

**User Story:** As an operator running a Kythira cluster, I want Raft's
built-in metric emission points (command counters, replication latency,
election events, ...) exported as OTLP metrics, so I can feed them into
whatever OpenTelemetry-compatible backend my organization already uses,
without Kythira needing a bespoke integration for that backend.

#### Acceptance Criteria

1. `otlp_metrics` SHALL be provided in `include/raft/otlp_metrics.hpp` and
   SHALL satisfy `kythira::metrics<otlp_metrics>`
   (`include/raft/metrics.hpp`), verified by a `static_assert` in that
   header.
2. `otlp_metrics` SHALL be default-constructible (required by the `metrics`
   concept's implicit usage as a `raft_types`/`tcp_raft_types` member) when
   constructed with a default-initialized `config`; a default-constructed
   instance with an empty `endpoint` SHALL behave as a no-op (Requirement
   8.4).
3. `otlp_metrics` SHALL be cheaply copyable, and every copy SHALL share the
   same underlying batching exporter and export queue (Requirement 6) —
   Raft's implementation captures copies of its `metrics_type` member into
   asynchronous continuations (`include/raft/raft.hpp`, e.g. the
   `command_completed`/`command_timeout` closures), so two copies recording
   concurrently from different threads MUST both reach the same export
   pipeline without data races.
4. `set_metric_name(name)` SHALL begin building a new metric record,
   discarding any dimensions accumulated by a prior `set_metric_name` call
   on the same instance that was never `emit()`-ted (mirrors
   `console_logger`'s pattern of one accumulate-then-flush cycle per call
   site, and matches every existing `_metrics.set_metric_name(...); ...;
   _metrics.emit();` call site in `include/raft/raft.hpp`).
5. `add_dimension(name, value)` SHALL attach one OTLP attribute
   (`{"key": name, "value": {"stringValue": value}}`) to the in-progress
   metric's data point. Multiple calls SHALL accumulate multiple
   attributes.
6. Exactly one of `add_one()`, `add_count(n)`, `add_duration(d)`, or
   `add_value(v)` SHALL be called per record before `emit()`; calling more
   than one before `emit()` SHALL retain only the last call's encoding
   (last-write-wins, not an error — see Requirement 9.3 for why this isn't
   validated at runtime).
7. `add_one()` SHALL encode as an OTLP `Sum` metric with one
   `NumberDataPoint` (`asInt: "1"`), `aggregationTemporality:
   AGGREGATION_TEMPORALITY_DELTA`, `isMonotonic: true`. `add_count(n)` SHALL
   encode identically with `asInt: "n"`.
8. `add_duration(d)` SHALL encode as an OTLP `Gauge` metric with one
   `NumberDataPoint` (`asInt: "<d in whole nanoseconds>"`, `unit: "ns"`) —
   a `Gauge`, not a `Histogram`, because a single `add_duration()` call
   carries exactly one sample with no client-side bucketing scheme; mapping
   to `Histogram` would require inventing arbitrary bucket boundaries this
   class has no basis for choosing. Downstream aggregation into a
   histogram/percentile view is the Collector or backend's job, not this
   exporter's (see "Out of Scope").
9. `add_value(v)` SHALL encode as an OTLP `Gauge` metric with one
   `NumberDataPoint` (`asDouble: v`, unit taken from `config.default_unit`,
   default `"1"`).
10. `emit()` SHALL enqueue the fully-built `Metric` (name + data point) onto
    the shared batching exporter (Requirement 6) and return without
    blocking on network I/O, then reset the in-progress record so the next
    `set_metric_name()` starts clean.
11. The build SHALL succeed on every configuration (no new optional
    dependency gate — Requirement 10).

### Requirement 2: `otlp_logger`

**User Story:** As an operator, I want Kythira's structured diagnostic log
lines exported as OTLP log records alongside the metrics from Requirement
1, so logs and metrics from the same process correlate in my observability
backend via a shared `Resource` (matching `service.instance.id`/node ID).

#### Acceptance Criteria

1. `otlp_logger` SHALL be provided in `include/raft/otlp_logger.hpp` and
   SHALL satisfy `kythira::diagnostic_logger<otlp_logger>`
   (`include/raft/logger.hpp`), verified by a `static_assert` in that
   header.
2. `otlp_logger` SHALL be cheaply copyable with the same shared-exporter
   semantics as Requirement 1.3 (`include/raft/raft.hpp` captures
   `logger_type` copies into asynchronous continuations identically to
   `metrics_type`).
3. `log(level, message)` and `log(level, message, key_value_pairs)` SHALL
   encode one OTLP `LogRecord`: `timeUnixNano` (current wall-clock time,
   Requirement 4.3), `severityNumber`/`severityText` (Requirement 3),
   `body: {"stringValue": message}`, and one `attributes` entry per
   key-value pair (`{"key": k, "value": {"stringValue": v}}`).
4. `trace(message)`, `debug(message)`, `info(message)`, `warning(message)`,
   `error(message)`, `critical(message)` (each with and without a
   `key_value_pairs` argument) SHALL delegate to `log()` with the
   corresponding `log_level`, matching `console_logger`'s existing method
   set exactly.
5. `log()`/every convenience method SHALL enqueue the encoded `LogRecord`
   onto the shared batching exporter (Requirement 6) and return without
   blocking on network I/O.
6. There is no per-instance minimum-level filter requirement (unlike
   `console_logger`'s constructor `min_level` parameter) — `otlp_logger`'s
   `config` MAY carry an optional `min_level` that, when set, SHALL cause
   `log()` calls below that level to be dropped before encoding (cheap
   short-circuit, avoids wasted allocation for filtered-out trace/debug
   volume); when absent, all levels are exported and filtering is left to
   the Collector/backend's own pipeline.
7. The build SHALL succeed on every configuration (no new optional
   dependency gate — Requirement 10).

### Requirement 3: Log severity mapping

**User Story:** As an operator viewing exported logs in an OTLP-compatible
backend, I want Kythira's severity levels to show up as the correct,
backend-recognized OTLP severity, so filtering/alerting on "error and
above" behaves the same as it would for any other OTLP-instrumented
service.

#### Acceptance Criteria

1. `kythira::log_level` SHALL map to OTLP `SeverityNumber`/`severityText`
   exactly as follows (per the
   [OTLP log data model's severity table](https://opentelemetry.io/docs/specs/otel/logs/data-model/#field-severitynumber)):

   | `log_level`  | `severityNumber` | `severityText` |
   |--------------|-------------------|-----------------|
   | `trace`      | 1                 | `"TRACE"`       |
   | `debug`      | 5                 | `"DEBUG"`       |
   | `info`       | 9                 | `"INFO"`        |
   | `warning`    | 13                | `"WARN"`        |
   | `error`      | 17                | `"ERROR"`       |
   | `critical`   | 21                | `"FATAL"`       |

   Note `severityText` uses OTLP's own vocabulary (`"WARN"`/`"FATAL"`), which
   differs from `console_logger`'s human-readable strings (`"WARNING"`/
   `"CRITICAL"`) — this is intentional; `severityText` is consumed by
   OTLP-aware tooling, not printed for humans.
2. `severityNumber` SHALL be encoded as a JSON integer (not a string) —
   unlike 64-bit integer fields, `SeverityNumber` is a Protobuf `enum`
   (32-bit), which the Protobuf JSON mapping encodes as a plain number (or
   the enum's string name; this implementation SHALL use the numeric form
   for simplicity, which every conformant OTLP/HTTP JSON receiver accepts
   per the Protobuf JSON mapping's "either representation" rule).

### Requirement 4: OTLP `Resource` and `InstrumentationScope`

**User Story:** As an operator running multiple Kythira nodes across one or
more clusters, I want every exported log and metric tagged with which node
and cluster produced it, so I can filter/group by node in my observability
backend without parsing it out of a message string.

#### Acceptance Criteria

1. A shared `otlp_resource` struct (`include/raft/otlp_resource.hpp`) SHALL
   carry: `service_name` (required, no default — constructing with an empty
   `service_name` SHALL throw `std::invalid_argument`, since an unnamed
   service defeats the purpose of the `Resource`), `service_instance_id`
   (optional; the natural value is the Raft node's own ID, but this struct
   has no dependency on `raft::types` — the caller supplies whatever string
   it wants), `service_namespace` (optional), and `extra_attributes`
   (`std::vector<std::pair<std::string, std::string>>`, for anything not
   covered by the three named fields, e.g. `deployment.environment`).
2. Both `otlp_metrics::config` and `otlp_logger::config` SHALL embed an
   `otlp_resource` field. Two `otlp_metrics`/`otlp_logger` instances
   constructed with resources that compare unequal (by value) SHALL NOT
   share a batching exporter (Requirement 6.6) — each distinct `Resource`
   gets its own export queue/thread/batch, since OTLP's wire format
   attaches exactly one `Resource` per `ResourceMetrics`/`ResourceLogs`
   entry and batching records from different resources together would
   require either duplicating the resource per record (wasteful) or
   silently picking one (incorrect).
3. The exported request body SHALL additionally include
   `instrumentationScope: {"name": "kythira", "version": <config-supplied
   or empty string>}` on every `ScopeMetrics`/`ScopeLogs` entry.
4. `LogRecord.timeUnixNano` and every `NumberDataPoint.timeUnixNano` SHALL
   be the wall-clock time (`std::chrono::system_clock::now()`) at the
   moment `log()`/`emit()` was called (not the time the batch was actually
   sent), encoded per Requirement 3.2's sibling rule for 64-bit fields —
   as a JSON string of whole nanoseconds since the Unix epoch.

### Requirement 5: OTLP/HTTP JSON wire format

**User Story:** As an operator pointing an OpenTelemetry Collector's
`otlphttp` receiver at Kythira's exporter, I want the request bodies to be
valid, spec-conformant OTLP/HTTP JSON, so the Collector accepts them
without a custom unmarshaling workaround.

#### Acceptance Criteria

1. Metrics SHALL be POSTed to `{config.endpoint}/v1/metrics` with
   `Content-Type: application/json` and a body shaped
   `{"resourceMetrics": [{"resource": {...}, "scopeMetrics": [{"scope":
   {...}, "metrics": [...]}]}]}`.
2. Logs SHALL be POSTed to `{config.endpoint}/v1/logs` with `Content-Type:
   application/json` and a body shaped `{"resourceLogs": [{"resource":
   {...}, "scopeLogs": [{"scope": {...}, "logRecords": [...]}]}]}`.
3. `Resource.attributes` and every `NumberDataPoint`/`LogRecord`'s
   `attributes` SHALL each be a JSON array of `{"key": <string>, "value":
   {"stringValue": <string>}}` objects (the `KeyValue`/`AnyValue` wire
   shape), never a JSON object/map — matching `opentelemetry-proto`'s
   `repeated KeyValue` field, which the JSON mapping renders as an array,
   not an object.
4. Every `AnyValue` produced by this implementation SHALL use only the
   `stringValue` variant — `otlp_resource`, `add_dimension`, and
   `key_value_pairs` are all string-typed at the API surface
   (`std::string_view` throughout both concepts), so there is no source of
   a non-string attribute value to encode.
5. JSON construction SHALL use `boost::json` (already a required project
   dependency — `include/raft/*.hpp` already uses it extensively, e.g.
   `acme_jws.hpp`, `ca_state_machine.hpp`), not a new JSON library
   dependency.
6. HTTP export SHALL use `httplib::Client` (already a required project
   dependency), matching the existing `make_client()`-style pattern in
   `include/raft/acme_certificate_provider_impl.hpp` (connection/read
   timeouts, transparent HTTPS when the endpoint scheme is `https://` and
   the build has `CPPHTTPLIB_OPENSSL_SUPPORT`).

### Requirement 6: Shared batching exporter

**User Story:** As an operator running a busy Raft cluster emitting many
metrics/log lines per second, I want them batched into a small number of
HTTP requests rather than one request per call, so the exporter doesn't
become a request-volume problem for the Collector or a latency problem for
the calling thread.

#### Acceptance Criteria

1. A non-public `otlp_exporter` component SHALL be added at
   `include/raft/detail/otlp_exporter.hpp` (not a public API — mirrors the
   existing `detail/`-namespace convention this project's own Notes
   sections have called for, e.g. `.kiro/specs/dns-peer-discovery/tasks.md`'s
   note on sharing the `rfc2136`-family TSIG/TCP-send helper).
2. `otlp_exporter` SHALL own: a bounded queue of pre-encoded JSON record
   values (one per `Metric`/`LogRecord`), a background thread, a mutex +
   condition variable (mirrors the existing fresher-thread pattern in
   `include/raft/rfc2136_dns_sd_discovery.hpp`), and the `httplib::Client`
   used to POST batches.
3. The background thread SHALL flush (POST a batch) when EITHER the queue
   reaches `config.max_batch_size` (default 512) OR `config.flush_interval`
   (default 5 seconds) has elapsed since the last flush, whichever comes
   first — an empty queue at the flush-interval tick SHALL NOT send an
   empty-bodied request.
4. A record enqueued when the queue is already at `config.max_queued_records`
   (default 2048, must be `>= max_batch_size`) SHALL cause the *oldest*
   queued record to be dropped to make room (favors export recency over
   completeness under sustained backpressure) and SHALL increment an
   internal dropped-record counter, readable via
   `otlp_exporter::dropped_record_count()` for diagnostics and test
   assertions — this counter is a plain accessor, not itself re-exported as
   an OTLP metric (doing so would recursively re-enter the same exporter
   this counter is reporting on).
5. A failed export attempt (connection failure, non-2xx response) SHALL be
   logged to `stderr` (a single line, rate-limited to at most once per
   `config.flush_interval` to avoid log-spamming stderr when the collector
   is down for an extended period) and the batch SHALL be dropped — no
   retry queue, no exponential backoff. This is a deliberate scope
   reduction (see "Out of Scope"); OTLP export is inherently best-effort
   telemetry, and Kythira already has a `retry_policy`/`execute_with_retry`
   mechanism (`include/raft/error_handler.hpp`) built around
   `kythira::Future`-returning RPC operations — it does not fit a
   background thread doing synchronous batched POSTs, and retrying stale
   telemetry batches after an outage has limited value versus just resuming
   forward with fresh data once the collector recovers.
6. Two `otlp_metrics`/`otlp_logger` instances (or copies thereof)
   constructed with `config`s whose `otlp_resource` and `endpoint` compare
   equal SHALL share one `otlp_exporter` instance (one background
   thread/queue per distinct `(resource, endpoint)` pair, not one per
   constructed object) — a process running a Raft node plus, say, an HTTP
   transport that both emit metrics with the same `service.name`/
   `service.instance.id` SHALL NOT spawn two redundant background threads
   and two independent HTTP connections to the same Collector.
7. The destructor of the last surviving `otlp_metrics`/`otlp_logger`
   instance referencing a given `otlp_exporter` SHALL stop the background
   thread and attempt one final best-effort flush of any remaining queued
   records (bounded by `config.shutdown_flush_timeout`, default 2 seconds)
   before the process exits — swallowing any exception, per this project's
   established best-effort-destructor idiom (e.g.
   `rfc2136_ldns_discovery::~rfc2136_ldns_discovery()`).

### Requirement 7: Fault injection

**User Story:** As a developer testing Raft's behavior when the
observability backend is unreachable, I want to simulate OTLP export
failures deterministically, consistent with this project's existing
`libfiu`-based chaos-testing conventions.

#### Acceptance Criteria

1. `otlp_exporter`'s flush path SHALL carry fault-injection points
   `raft/otlp/exporter/flush/fail` (throws, exercising the failed-export
   path in Requirement 6.5) and `raft/otlp/exporter/flush/noop` (silently
   skips the real HTTP call, succeeding without network I/O) — naming
   mirrors the existing convention (`raft/dns/rfc2136/send_update`,
   `raft/dns/rfc6763_ldns/send_update`).
2. These fault points SHALL compile to zero-cost no-ops outside chaos test
   builds, via the existing `fiu_do_on`/`raft/fault_injection.hpp`
   mechanism (no new fault-injection plumbing).

### Requirement 8: Configuration

**User Story:** As an operator, I want to point both classes at an
OpenTelemetry Collector (or any OTLP/HTTP-JSON-compatible ingest endpoint)
with a single, small configuration struct per class, following the same
`config`-struct convention every other pluggable Kythira component uses.

#### Acceptance Criteria

1. `otlp_metrics::config` SHALL contain: `otlp_resource resource`,
   `std::string endpoint` (base URL, e.g. `"http://otel-collector:4318"` —
   no trailing `/v1/metrics`, appended internally per Requirement 5.1),
   `std::vector<std::pair<std::string, std::string>> extra_headers`
   (applied to every export request — the mechanism for bearer tokens/API
   keys many OTLP-compatible ingest endpoints require), `std::string
   scope_version` (default `""`), `std::string default_unit` (default
   `"1"`, used by `add_value`, Requirement 1.9), and the batching knobs
   from Requirement 6 (`max_batch_size`, `max_queued_records`,
   `flush_interval`, `shutdown_flush_timeout`, plus
   `connection_timeout`/`read_timeout` for the underlying
   `httplib::Client`, each defaulting to 5 seconds).
2. `otlp_logger::config` SHALL contain the same fields as
   `otlp_metrics::config` (Requirement 8.1) plus `std::optional<log_level>
   min_level` (Requirement 2.6, default `std::nullopt`).
3. Every batching/timeout knob SHALL have a documented default (values given
   in Requirement 6/8.1) so a minimal `config{.resource = {...}, .endpoint =
   "..."}` is usable without tuning anything else.
4. A `config` with an empty `endpoint` SHALL construct successfully and
   behave as a complete no-op (every `add_*`/`log*`/`emit()` call is a
   cheap early-return, no queue, no background thread, no HTTP client) —
   this lets `otlp_metrics{}`/`otlp_logger{}` (default-`config`) serve as
   the same kind of always-safe default that `noop_metrics` is today,
   satisfying Requirement 1.2, without a caller needing a separate no-op
   type.

### Requirement 9: Concept satisfaction and non-goals

**User Story:** As a developer, I want compile-time verification that both
new classes satisfy their respective concepts, and a clear record of what
this spec deliberately does not attempt, so a future contributor doesn't
mistake a scope boundary for an oversight.

#### Acceptance Criteria

1. `static_assert(kythira::metrics<otlp_metrics>)` SHALL appear at the
   bottom of `include/raft/otlp_metrics.hpp`.
2. `static_assert(kythira::diagnostic_logger<otlp_logger>)` SHALL appear at
   the bottom of `include/raft/otlp_logger.hpp`.
3. Neither concept's method signatures return a status/error code or
   `kythira::Future` (both concepts model synchronous, always-succeeding
   calls per their existing definitions in `metrics.hpp`/`logger.hpp`), so
   this spec's classes have no channel to reject a malformed call (e.g.
   Requirement 1.6's "more than one recording method before `emit()`") —
   this is a pre-existing constraint of the concepts themselves, not a gap
   introduced by this spec, and is out of scope to change (would be a
   breaking change to two public concepts every existing implementation,
   including `noop_metrics`/`console_logger`, would need to follow).

### Requirement 10: Build integration

**User Story:** As a developer building Kythira without network access to
an OTLP collector (e.g. CI, an offline dev environment), I want the build
to succeed unconditionally, since this feature adds no new required
dependency.

#### Acceptance Criteria

1. `otlp_metrics.hpp`, `otlp_logger.hpp`, and
   `detail/otlp_exporter.hpp` SHALL depend only on already-required project
   dependencies (`boost::json`, `httplib`, standard library `<thread>`/
   `<mutex>`/`<condition_variable>`) — no `#ifdef KYTHIRA_HAS_*` build guard
   is needed or added, unlike the libldns-/Poco-DNSSD-gated peer-discovery
   classes in `.kiro/specs/dns-peer-discovery/`.
2. No changes to `CMakeLists.txt`'s dependency-detection section are
   required; only new source files and new test-target registration in
   `tests/CMakeLists.txt` (ungated, like any other unconditionally-built
   unit test target).

## Out of Scope

- **OTLP traces/spans.** Kythira has no tracing concept
  (`kythira::tracer`/similar) to implement against; adding one is a
  separate, much larger spec of its own.
- **OTLP/HTTP Protobuf encoding.** Deliberately deferred rather than
  independently justified: Protobuf is not otherwise a project dependency
  today, and introducing it (plus generated code from
  `opentelemetry-proto`'s `.proto` files) solely for this spec would mean
  owning a new, unrelated dependency just for telemetry export, versus this
  spec's JSON-only, `boost::json`-based approach (already a project
  dependency). If Kythira ever adopts Protobuf for some other reason (a
  binary RPC wire format alongside `json_serializer.hpp`, or as a
  prerequisite of the OTLP/gRPC item below), OTLP/Protobuf becomes a much
  cheaper addition to this spec's classes at that point — reusing an
  already-present dependency instead of introducing one — and SHOULD be
  revisited then rather than before. Until then, a future spec MAY still
  choose to add it standalone, behind its own build guard, analogous to the
  libldns/Poco-DNSSD pattern.
- **OTLP/gRPC.** Deliberately deferred rather than independently justified:
  gRPC (and the Protobuf it depends on) is not otherwise a project
  dependency today, and introducing it solely for this spec would mean
  owning a second, unrelated heavy dependency (and its own
  `.proto`-generated code) just for telemetry export. Kythira's RPC
  transports (HTTP, CoAP, TCP) are themselves candidates for a future gRPC
  transport (see `doc/TODO.md`'s "New Transport Implementations"-style
  entries and `.kiro/specs/docker-chaos/design.md`'s passing consideration
  of gRPC as a control-plane transport); if/when that work adds gRPC (and
  therefore Protobuf) as a project dependency for RPC, OTLP/gRPC export
  becomes a much cheaper addition to this spec's classes (same
  dependencies, reused rather than newly introduced) and SHOULD be
  revisited then rather than before.
- **Client-side histogram aggregation for `add_duration`.** See
  Requirement 1.8 — encoded as a `Gauge`, not a `Histogram`.
- **Retry/backoff on export failure.** See Requirement 6.5.
- **Wiring `otlp_metrics`/`otlp_logger` into any `cmd/*` binary's default
  types.** Operators opt in explicitly (design doc's "Adoption" section);
  no existing binary's default `metrics_type`/`logger_type` changes.
- **A real OpenTelemetry Collector Docker chaos test is a stretch task**
  (tasks.md marks it optional/last-wave) — the primary test strategy is a
  lightweight in-process mock OTLP/HTTP JSON collector
  (`tests/otlp_test_collector.hpp`, mirroring the existing
  `tests/acme_test_server.hpp` pattern), which does not require a
  container runtime and so runs in every environment this project's other
  unit tests already run in.
