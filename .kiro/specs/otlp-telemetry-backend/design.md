# OTLP Telemetry Backend Design Document

## Overview

`kythira::metrics` and `kythira::diagnostic_logger` are call-site-facing
concepts: a `node<Types>` holds one long-lived `_metrics`/`_logger` instance
and calls a short sequence of methods on it per event (`set_metric_name` →
`add_dimension`* → one of `add_one`/`add_count`/`add_duration`/`add_value` →
`emit()`; or `log(level, message[, kv_pairs])`). Both concepts' contracts
require every method to be non-blocking, deferring I/O to a background
emitter — this is load-bearing, not a style preference: `_metrics`/`_logger`
calls happen inline on the Raft node's own locked critical sections (see
`include/raft/raft.hpp` call sites around `submit_command`/`append_log_entry`),
so any blocking I/O here would directly stall consensus.

This design adds two thin, concept-facing classes (`otlp_metrics`,
`otlp_logger`) that translate each call into an OTLP data shape and hand it to
a single shared, internal batching/HTTP engine (`otlp_http_batch_exporter`).
Both signal types reuse identical queueing, batching, retry, and shutdown
logic — the only difference between the two is *what* gets JSON-encoded and
*which* OTLP path (`/v1/metrics` vs `/v1/logs`) it's posted to.

OTLP/HTTP with JSON encoding is used rather than OTLP/gRPC or OTLP/HTTP with
protobuf-binary encoding: the project already depends on `cpp-httplib` (HTTP
client) and `boost::json`, so this needs no new `vcpkg.json` entry, no
protobuf compiler step, and no gRPC runtime — a real tradeoff against
generated-message type safety and wire compactness, made deliberately in
favor of zero new build-system surface (see Non-Goals).

## Architecture

```
Recording call sites (include/raft/raft.hpp, under node<Types>::_mutex)
  │
  │  _metrics.set_metric_name(...); .add_dimension(...); .add_one(); .emit();
  │  _logger.info(message, {{"key","value"}, ...});
  ▼
otlp_metrics / otlp_logger              (include/raft/otlp_metrics.hpp,
  - accumulate pending record            include/raft/otlp_logger.hpp)
  - map to OTLP data shape on emit()/log()  — Requirements 1, 2
  - own mutex; non-blocking throughout
  │
  │  push finalized record
  ▼
otlp_http_batch_exporter<Poster>        (include/raft/otlp_exporter.hpp)
  - bounded, mutex-protected queue (drop-oldest on overflow) — Requirement 3
  - background thread: batch by size/interval, JSON-encode, POST, retry
  │
  │  HTTP POST <endpoint>/v1/metrics  or  <endpoint>/v1/logs
  │  Content-Type: application/json
  ▼
OpenTelemetry Collector (operator-run, e.g. docker/otel-collector/)
  │
  ▼
Prometheus / Grafana / Honeycomb / Datadog / ... (Collector's own exporters)
```

```
include/raft/otlp_exporter.hpp                (new)
  ├── otlp_resource                 — Requirement 4: shared Resource attrs
  ├── otlp_export_config            — Requirement 3/5: batching/retry knobs
  ├── otlp_series_key               — (metric name, sorted dimensions) for
  │                                    delta-temporality start-time tracking
  └── otlp_http_batch_exporter<Poster = real_http_poster>
        - push(record)              — Requirement 3.2
        - background thread loop    — Requirement 3.4
        - dropped_record_count()    — Requirement 3.3, test/ops visibility

include/raft/otlp_metrics.hpp                 (new)
  └── otlp_metrics                  — satisfies kythira::metrics
        composes an otlp_http_batch_exporter<...> targeting /v1/metrics

include/raft/otlp_logger.hpp                  (new)
  └── otlp_logger                   — satisfies kythira::diagnostic_logger
        composes an otlp_http_batch_exporter<...> targeting /v1/logs

cmd/chaos_node/config.hpp / main.cpp          (extended)
  - OTLP_ENDPOINT / OTLP_HEADERS / OTLP_SERVICE_NAME env vars — Requirement 5
  - tcp_raft_types_with_otlp : tcp_raft_types  (sibling Types struct,
    same shape as the existing tcp_raft_types_with_docker_qm) selected at
    startup when OTLP_ENDPOINT is set

docker/otel-collector/otel-collector-config.yaml   (new) — Requirement 6
docker/otlp-collector-compose.yml                  (new) — Requirement 7
tests/docker_chaos/otlp_collector_test.cpp         (new) — Requirement 7
```

## Components and Interfaces

### 1. `otlp_resource` and `otlp_export_config`

```cpp
namespace kythira {

// Requirement 4.1: attributes attached once per export request, shared by
// every data point / LogRecord in it.
struct otlp_resource {
    std::string service_name;
    std::string service_instance_id;
    std::optional<std::string> service_namespace;
    std::vector<std::pair<std::string, std::string>> extra_attributes;

    [[nodiscard]] auto to_json() const -> boost::json::object;
};

// Requirement 5.1: batching/retry/histogram-bucket knobs, all defaulted.
struct otlp_export_config {
    std::string endpoint_base_url;                      // e.g. "http://otel-collector:4318"
    std::vector<std::pair<std::string, std::string>> headers;

    std::size_t max_batch_size = 512;
    std::chrono::milliseconds flush_interval{5000};
    std::size_t max_queue_size = 4096;
    std::chrono::milliseconds http_timeout{5000};
    unsigned max_retries = 3;
    std::chrono::milliseconds retry_backoff_base{200};

    // Requirement 1.4: milliseconds; ~exponential spread covering sub-ms to
    // multi-second Raft RPC/consensus latencies.
    std::vector<double> histogram_bounds_ms{
        1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000};
};

}  // namespace kythira
```

### 2. `otlp_http_batch_exporter<Poster>`

```cpp
namespace kythira {

// Requirement 3.7: injectable POST seam, mirroring tests/docker_chaos's
// existing CmdExecutor/HttpGet/HttpPost stub pattern — default implementation
// is real_http_poster (cpp-httplib), tests substitute a recording stub.
struct http_post_result {
    bool ok = false;
    int status = 0;
};
using http_poster_fn =
    std::function<http_post_result(std::string_view url,
                                   const std::vector<std::pair<std::string, std::string>>& headers,
                                   std::string_view json_body,
                                   std::chrono::milliseconds timeout)>;

[[nodiscard]] auto real_http_poster() -> http_poster_fn;  // cpp-httplib-backed

// One instance per signal type (metrics or logs); owns its own queue and
// background thread. `Encode` turns a batch of `Record` into the OTLP JSON
// request body (ExportMetricsServiceRequest / ExportLogsServiceRequest).
template<typename Record>
class otlp_http_batch_exporter {
public:
    otlp_http_batch_exporter(otlp_export_config config, otlp_resource resource,
                             std::string_view signal_path,  // "/v1/metrics" | "/v1/logs"
                             std::function<boost::json::object(const otlp_resource&,
                                                                std::span<const Record>)> encode,
                             http_poster_fn poster = real_http_poster());

    otlp_http_batch_exporter(otlp_http_batch_exporter&&) noexcept;
    auto operator=(otlp_http_batch_exporter&&) noexcept -> otlp_http_batch_exporter&;
    otlp_http_batch_exporter(const otlp_http_batch_exporter&) = delete;

    ~otlp_http_batch_exporter();  // Requirement 3.6: signal stop, final flush, join

    // Requirement 3.2: the only concept-facing entry point; never blocks on I/O.
    auto push(Record record) -> void;

    [[nodiscard]] auto dropped_record_count() const -> std::uint64_t;  // Requirement 3.3

private:
    // background_loop(): wait up to flush_interval or until max_batch_size
    // records queued; drain under lock; encode(); POST with retry
    // (Requirement 3.5); on exhausted retries, increment dropped count and
    // move on — never re-enters this exporter or any otlp_logger.
};

}  // namespace kythira
```

`push()`'s drop-oldest overflow policy (Requirement 3.3) and the background
loop's retry policy (Requirement 3.5) are the two places back-pressure from a
struggling collector is absorbed — deliberately, in favor of bounded memory
and a live Raft node, over "never lose a sample."

### 3. `otlp_metrics`

```cpp
namespace kythira {

class otlp_metrics {
public:
    otlp_metrics(otlp_export_config config, otlp_resource resource);

    auto set_metric_name(std::string_view name) -> void;
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void;
    auto add_one() -> void;
    auto add_count(std::int64_t count) -> void;
    auto add_duration(std::chrono::nanoseconds duration) -> void;
    auto add_value(double value) -> void;
    auto emit() -> void;   // Requirement 1.2: finalizes + pushes to the exporter

private:
    struct pending_metric_record { /* name, dimensions, one of {sum_delta, histogram, gauge} */ };

    std::mutex _mutex;                          // Requirement 1.5
    pending_metric_record _pending;              // reset on every emit()
    std::unordered_map<otlp_series_key, std::chrono::nanoseconds> _series_start_time;  // Req 1.3
    otlp_http_batch_exporter<pending_metric_record> _exporter;
};

static_assert(metrics<otlp_metrics>, "otlp_metrics must satisfy metrics concept");

}  // namespace kythira
```

`emit()`'s Sum/Histogram/Gauge branch (Requirement 1.2) reads the last-called
recording method's tag on `_pending`, builds the OTLP data point, looks up (and
updates) `_series_start_time[key]` for Sum/Histogram (Requirement 1.3), and
pushes the finished record — then resets `_pending` for the next
`set_metric_name`. An example encoded Sum data point (one `add_one()` call on
metric `"command_received"` with dimension `node_id=1`):

```json
{
  "resourceMetrics": [{
    "resource": {"attributes": [
      {"key": "service.name", "value": {"stringValue": "kythira-chaos-node"}},
      {"key": "service.instance.id", "value": {"stringValue": "1"}}
    ]},
    "scopeMetrics": [{
      "metrics": [{
        "name": "command_received",
        "sum": {
          "aggregationTemporality": "AGGREGATION_TEMPORALITY_DELTA",
          "isMonotonic": true,
          "dataPoints": [{
            "attributes": [{"key": "node_id", "value": {"stringValue": "1"}}],
            "startTimeUnixNano": "1752739200000000000",
            "timeUnixNano": "1752739205000000000",
            "asInt": "1"
          }]
        }
      }]
    }]
  }]
}
```

### 4. `otlp_logger`

```cpp
namespace kythira {

class otlp_logger {
public:
    explicit otlp_logger(otlp_export_config config, otlp_resource resource,
                         log_level min_level = log_level::trace);

    auto log(log_level level, std::string_view message) -> void;
    auto log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs)
        -> void;
    auto trace(std::string_view message) -> void;
    auto debug(std::string_view message) -> void;
    auto info(std::string_view message) -> void;
    auto warning(std::string_view message) -> void;
    auto error(std::string_view message) -> void;
    auto critical(std::string_view message) -> void;
    // + the six key_value_pairs overloads required by diagnostic_logger

    auto set_min_level(log_level level) -> void;
    [[nodiscard]] auto get_min_level() const -> log_level;

private:
    struct pending_log_record { /* time, severity_number, severity_text, body, attributes */ };

    std::mutex _mutex;                           // Requirement 2.4
    log_level _min_level;
    otlp_http_batch_exporter<pending_log_record> _exporter;
};

static_assert(diagnostic_logger<otlp_logger>,
              "otlp_logger must satisfy diagnostic_logger concept");

}  // namespace kythira
```

`log_level` → OTLP `SeverityNumber`/`SeverityText` (Requirement 2.2):

| `log_level` | `SeverityNumber` | `SeverityText` |
|---|---|---|
| `trace`     | 1  | `"TRACE"` |
| `debug`     | 5  | `"DEBUG"` |
| `info`      | 9  | `"INFO"`  |
| `warning`   | 13 | `"WARN"`  |
| `error`     | 17 | `"ERROR"` |
| `critical`  | 21 | `"FATAL"` |

### 5. `chaos_node` wiring

```cpp
// cmd/chaos_node/main.cpp — Requirement 5.3, same shape as the existing
// tcp_raft_types_with_docker_qm sibling-Types pattern already in this file.
struct tcp_raft_types_with_otlp : kythira::tcp_raft_types {
    using logger_type = kythira::otlp_logger;
    using metrics_type = kythira::otlp_metrics;
};

// in main(): branch on cfg.otlp_endpoint.has_value(), instantiate the
// resource/config once, construct node_config<...>::logger/metrics from it,
// and call run_node<tcp_raft_types_with_otlp>(...) instead of
// run_node<tcp_raft_types>(...) — mirrors the existing docker_quorum_manager
// branch immediately above it.
```

## Correctness Properties

### Property 1: The recording hot path never performs I/O

Every `otlp_metrics`/`otlp_logger` public method either (a) mutates
in-process, mutex-protected accumulation state, or (b) pushes a finalized
record onto `otlp_http_batch_exporter`'s bounded queue under the same kind of
lock. Neither path touches a socket. Verified by: the `Poster` template
parameter is only ever invoked from the background thread's loop, never from
`push()`/`emit()`/`log()` — a code-level invariant checked by Requirement
3.7's injectable-poster unit tests asserting zero poster calls happen
synchronously within a `push()`/`emit()` call.

### Property 2: Bounded memory under a sustained collector outage

`otlp_http_batch_exporter`'s queue has a fixed capacity
(`max_queue_size`); `push()`'s drop-oldest policy (Requirement 3.3) guarantees
the queue never exceeds that capacity regardless of how long the collector
stays unreachable — memory use for queued-but-unsent records is bounded by
`max_queue_size × sizeof(Record)`, independent of outage duration.

### Property 3: Delta temporality's start-time is always the previous emit's time

For a given series `(name, dimensions)`, the `start_time_unix_nano` used for
its `k`-th Sum/Histogram data point is exactly the `time_unix_nano` used for
its `(k-1)`-th (or construction time, for `k=1`) — by construction, since
`_series_start_time[key]` is read then immediately overwritten with the
current call's timestamp inside the same mutex-held section (Requirement
1.3). This is the property an OTLP-consuming backend needs to correctly
attribute each delta Sum/Histogram value to a specific, non-overlapping time
window.

### Property 4: An export failure cannot cause an unbounded retry loop

Requirement 3.5 bounds retries per batch to `max_retries` with a doubling
backoff, and an exhausted batch is dropped, not requeued — so a persistently
failing collector produces a bounded amount of retry traffic per
`flush_interval`, not an ever-growing one. Requirement 3.5 also forbids
routing export-failure diagnostics back through `otlp_logger` itself,
eliminating the obvious way this property could otherwise be violated
(a logging call whose own transport is failing, logging that failure,
generating another failing logging call).

## Error Handling

- **Collector unreachable at push time**: absorbed entirely by the queue;
  `push()` still succeeds (in the sense of not blocking/throwing) whether or
  not the eventual POST does.
- **Queue full**: drop-oldest (Requirement 3.3); no exception, no blocking.
- **POST transport failure / retryable HTTP status**: retried per
  Requirement 3.5; batch dropped and counted after `max_retries`.
- **POST non-retryable HTTP status** (e.g. 400 from a malformed request —
  which would indicate a bug in this spec's JSON encoding, not a transient
  collector condition): not retried, counted as dropped immediately, so a
  systematic encoding bug fails fast (visible via `dropped_record_count()`)
  rather than being masked by endless retries.
- **Malformed/empty `otlp_export_config::endpoint_base_url`**: `otlp_metrics`/
  `otlp_logger`'s constructor throws `std::invalid_argument` — fail at
  startup, matching the project's existing "fail closed at construction, not
  silently at first use" convention (e.g. `ca_cluster_node_config` flag
  validation).
- **Destruction with a non-empty queue**: Requirement 3.6's bounded final
  flush attempt is best-effort — if the collector is down at shutdown, queued
  records are lost, logged nowhere (there is nowhere left to log to that
  wouldn't itself need the now-shutting-down exporter), and the process exits
  normally. This is an accepted tradeoff, not a defect (see Non-Goals).

## Testing Strategy

- **Unit — mapping correctness** (`tests/otlp_metrics_test.cpp`,
  `tests/otlp_logger_test.cpp`): using the injectable `Poster` seam
  (Requirement 3.7), assert the exact JSON shape produced for each of
  `add_one`/`add_count`/`add_duration`/`add_value` and each `log_level`,
  including delta `start_time_unix_nano` correctness across two successive
  `emit()` calls on the same series (Property 3), and `static_assert`s
  confirming concept conformance.
- **Unit — exporter mechanics** (`tests/otlp_exporter_unit_test.cpp`):
  drop-oldest overflow behavior at `max_queue_size`, batch-by-size vs.
  batch-by-interval triggering, retry count/backoff timing against a stub
  poster that fails N times then succeeds, and clean shutdown (destructor
  joins, attempts final flush) — no real sockets, matching the existing
  `docker_chaos_harness_unit_tests`/`docker_chaos_fault_control_unit_tests`
  convention of injected-stub-only unit tests registered directly in CTest.
- **Docker scenario** (`tests/docker_chaos/otlp_collector_test.cpp`,
  Requirement 7): real `chaos_node` containers, real
  `otel/opentelemetry-collector-contrib`, real HTTP, real OTLP parsing on the
  receiving end — the end-to-end proof no unit test (which only checks what
  *we* believe the schema means) can provide.

## Non-Goals

- **OTLP/gRPC.** Would require adding `grpc`/`protobuf` to `vcpkg.json` and a
  code-generation build step for comparatively little benefit over
  OTLP/HTTP JSON at Kythira's telemetry volume; every OpenTelemetry Collector
  accepts OTLP/HTTP JSON on the same terms as gRPC. Revisit only if a real
  deployment hits a throughput ceiling this spec's batching can't absorb.
- **OTLP/HTTP protobuf-binary encoding.** Slightly more compact and slightly
  faster to encode than JSON, but requires the protobuf compiler/runtime this
  spec deliberately avoids; JSON is a fully spec-compliant OTLP/HTTP encoding,
  not a workaround.
- **Local pre-aggregation, exemplars, or trace/span emission.** This spec
  forwards every individual recorded event as its own delta data point/
  LogRecord and lets the Collector or downstream backend aggregate; it does
  not implement OTLP traces at all (`kythira::metrics`/`diagnostic_logger`
  have no span/trace concept to translate from).
- **Wiring into `ca_cluster_node`, `dns_discovery_node`, or any binary other
  than `chaos_node`.** `otlp_metrics`/`otlp_logger` are general-purpose
  concept implementations usable anywhere `metrics_type`/`logger_type` is a
  free template parameter; this spec proves the integration once, in one
  reference binary, the same way `.kiro/specs/ca-cluster-rpc-mtls/` scoped
  its own transport change to `ca_cluster_node` alone.
- **A real-vendor-credentialed second test.** Per Requirement 7.5 and
  `doc/TODO.md`'s own self-hosted-agent carve-out, OTLP has no single
  vendor-managed endpoint to test against; the Docker Collector test is this
  backend's complete testing requirement.
