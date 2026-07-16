# Design Document

## Overview

This document describes the design for `otlp_metrics` and `otlp_logger`, two
new implementations of Kythira's existing `metrics`/`diagnostic_logger`
concepts that export via OTLP/HTTP with JSON encoding. Both classes are thin
encoders sitting in front of a shared, non-public batching exporter
(`detail::otlp_exporter`) that owns the actual background thread and HTTP
client.

```
                 ┌─────────────────┐        ┌─────────────────┐
 caller (raft.hpp) │  otlp_metrics   │        │   otlp_logger   │  caller (raft.hpp)
      ────────────▶│  (encode only)  │        │  (encode only)  │◀────────────
                 └────────┬────────┘        └────────┬────────┘
                          │  encoded Metric JSON               │  encoded LogRecord JSON
                          ▼                                     ▼
                 ┌──────────────────────────────────────────────────┐
                 │         detail::otlp_exporter (shared,            │
                 │         one per distinct (resource, endpoint))    │
                 │  ┌──────────────┐   ┌────────────────────────┐   │
                 │  │ bounded queue │──▶│ background thread:     │   │
                 │  │ (mutex + cv)  │   │  batch on size/interval,│   │
                 │  └──────────────┘   │  POST via httplib       │   │
                 │                     └───────────┬─────────────┘   │
                 └─────────────────────────────────┼─────────────────┘
                                                     ▼
                                       OTLP/HTTP JSON POST
                                       {endpoint}/v1/metrics or /v1/logs
                                                     ▼
                                       OpenTelemetry Collector
                                       (or any OTLP/HTTP-JSON-compatible
                                       ingest endpoint)
```

Neither `otlp_metrics` nor `otlp_logger` performs network I/O directly —
every public method does pure, allocation-light JSON construction and a
queue push. All I/O (HTTP connection, batching, retries-or-not) is confined
to `detail::otlp_exporter`, which is deliberately not part of the public API
surface (mirrors the `detail/`-style sharing already called for in
`.kiro/specs/dns-peer-discovery/tasks.md`'s Notes section for the
`rfc2136`-family TSIG helper — this is the second instance of that pattern
in this codebase, generalized here to a real shared header rather than a
"consider it" note).

## Architecture

### Sharing key: `(resource, endpoint)`

Per Requirement 6.6, exporter instances are shared by a process-wide
registry keyed on `(otlp_resource, endpoint)` equality — not by type
(`otlp_metrics` and `otlp_logger` deliberately do **not** share an exporter
with each other, since one exports `ResourceMetrics` bodies and the other
`ResourceLogs` bodies to different URL paths; sharing is only ever
metrics-with-metrics or logs-with-logs). The registry is a
function-local-static `std::map<registry_key, std::weak_ptr<otlp_exporter>>`
guarded by a mutex, using `weak_ptr` so the last `otlp_metrics`/`otlp_logger`
referencing a given exporter still controls its lifetime (shutdown flush,
Requirement 6.7) via a `shared_ptr` each holds — the registry does not keep
exporters alive past their last user.

```cpp
namespace kythira::detail {

struct otlp_exporter_key {
    otlp_resource resource;
    std::string endpoint;
    std::string path;  // "/v1/metrics" or "/v1/logs" — see below

    bool operator==(const otlp_exporter_key&) const = default;
};

}  // namespace kythira::detail

template<> struct std::hash<kythira::detail::otlp_exporter_key> { /* ... */ };
```

`path` is included in the key (rather than relying on type-based separation
alone) so the sharing rule is expressed once, structurally, instead of by
convention between two call sites — `otlp_metrics` always constructs its key
with `path = "/v1/metrics"`, `otlp_logger` always with `path = "/v1/logs"`,
and the registry naturally never conflates them even if a future third class
reused `detail::otlp_exporter` directly.

### Why a shared registry, not a shared-by-construction handle

An earlier alternative considered: have the caller construct one
`otlp_exporter` explicitly and pass a `shared_ptr` into both
`otlp_metrics`/`otlp_logger`'s constructors. Rejected because every other
pluggable Kythira component (`rfc6763_ldns_peer_discovery`,
`tls_tcp_rpc_client`, ...) is constructed from a single `config` value with
no cross-object wiring required from the caller — matching that convention
means `otlp_metrics{cfg}` and `otlp_logger{cfg}` each "just work" from their
own config, and the registry does the de-duplication invisibly. The
trade-off is the registry's own global-mutex-guarded map, touched once per
construction (not per `emit()`/`log()` call), which is an acceptable cost.

## Components and Interfaces

### 1. `include/raft/otlp_resource.hpp`

```cpp
namespace kythira {

struct otlp_resource {
    std::string service_name;                  // required, non-empty
    std::string service_instance_id;            // optional
    std::string service_namespace;              // optional
    std::vector<std::pair<std::string, std::string>> extra_attributes;

    bool operator==(const otlp_resource&) const = default;
};

// Throws std::invalid_argument if resource.service_name is empty.
void validate(const otlp_resource& resource);

// Encodes as the OTLP `Resource.attributes` JSON array (Requirement 5.3).
boost::json::array to_json_attributes(const otlp_resource& resource);

}  // namespace kythira
```

`operator==` is defaulted (structural equality over all four fields,
including vector order in `extra_attributes` — two resources differing only
in attribute *order* are treated as distinct keys; this is a deliberate
simplification over sorting/canonicalizing, since in practice a given
`config` value is constructed once per call site with a fixed attribute
order, not built dynamically in a way that would vary the order between
otherwise-identical resources).

### 2. `include/raft/detail/otlp_exporter.hpp` (non-public)

```cpp
namespace kythira::detail {

class otlp_exporter {
public:
    struct config {
        std::size_t max_batch_size{512};
        std::size_t max_queued_records{2048};       // must be >= max_batch_size
        std::chrono::milliseconds flush_interval{5000};
        std::chrono::milliseconds shutdown_flush_timeout{2000};
        std::chrono::milliseconds connection_timeout{5000};
        std::chrono::milliseconds read_timeout{5000};
        std::vector<std::pair<std::string, std::string>> extra_headers;
    };

    // Returns the shared instance for (resource, endpoint, path), creating
    // it on first use. `endpoint`/`path` are combined into the full POST URL.
    static auto get_or_create(otlp_resource resource, std::string endpoint,
                              std::string path, config cfg)
        -> std::shared_ptr<otlp_exporter>;

    // Enqueues one already-encoded record (a `Metric` or `LogRecord` JSON
    // value). Never blocks on network I/O; may drop the oldest queued
    // record under sustained backpressure (Requirement 6.4).
    void enqueue(boost::json::value record);

    [[nodiscard]] auto dropped_record_count() const -> std::uint64_t;

    ~otlp_exporter();  // stops the thread, best-effort final flush

private:
    otlp_exporter(otlp_resource resource, std::string url, config cfg);

    void run();                                   // background thread body
    void flush_locked(std::vector<boost::json::value> batch);
    auto build_request_body(const std::vector<boost::json::value>& batch) const
        -> boost::json::value;

    otlp_resource _resource;
    std::string _url;       // e.g. "http://otel-collector:4318/v1/metrics"
    std::string _scope_name{"kythira"};
    config _cfg;

    std::mutex _mu;
    std::condition_variable _cv;
    std::deque<boost::json::value> _queue;
    bool _running{true};
    std::thread _thread;
    std::atomic<std::uint64_t> _dropped{0};
    std::chrono::steady_clock::time_point _last_failure_log{};  // rate-limit
};

}  // namespace kythira::detail
```

`build_request_body` wraps the batch's already-encoded per-record JSON
values into the `resourceMetrics`/`resourceLogs` envelope (Requirement 5.1/
5.2) — the envelope shape is identical for both metrics and logs except for
the field names (`"metrics"` vs `"logRecords"`, `"scopeMetrics"` vs
`"scopeLogs"`, `"resourceMetrics"` vs `"resourceLogs"`), so
`otlp_exporter` is parameterized by which of the two shapes to use (a
`bool is_metrics` flag set at construction from which `path` it was created
with, rather than a second templated variant — the two shapes differ only
in three string literals, not in any logic).

### 3. `include/raft/otlp_metrics.hpp`

```cpp
namespace kythira {

class otlp_metrics {
public:
    struct config {
        otlp_resource resource;
        std::string endpoint;  // "" = no-op instance, Requirement 8.4
        std::vector<std::pair<std::string, std::string>> extra_headers;
        std::string scope_version;
        std::string default_unit{"1"};
        std::size_t max_batch_size{512};
        std::size_t max_queued_records{2048};
        std::chrono::milliseconds flush_interval{5000};
        std::chrono::milliseconds shutdown_flush_timeout{2000};
        std::chrono::milliseconds connection_timeout{5000};
        std::chrono::milliseconds read_timeout{5000};
    };

    otlp_metrics() = default;                 // no-op instance
    explicit otlp_metrics(config cfg);

    void set_metric_name(std::string_view name);
    void add_dimension(std::string_view dimension_name, std::string_view dimension_value);
    void add_one();
    void add_count(std::int64_t count);
    void add_duration(std::chrono::nanoseconds duration);
    void add_value(double value);
    void emit();

private:
    struct in_progress_metric {
        std::string name;
        boost::json::array attributes;
        // Exactly one of the following is set by the last add_* call
        // (Requirement 1.6); an enum discriminant tracks which.
        enum class kind { unset, sum_int, gauge_int, gauge_double } kind{kind::unset};
        std::int64_t int_value{0};
        double double_value{0.0};
        std::string_view unit;
    };

    config _cfg;
    std::shared_ptr<detail::otlp_exporter> _exporter;  // null for no-op instances
    in_progress_metric _current;
};

static_assert(kythira::metrics<otlp_metrics>);

}  // namespace kythira
```

`_exporter` is `nullptr` exactly when `_cfg.endpoint.empty()` — every public
method's body starts by checking `_current`/`_exporter` state and returning
immediately in the no-op case, so a default-constructed `otlp_metrics{}`
compiles down to a handful of cheap early-returns (Requirement 8.4), not a
distinct code path duplicated from `noop_metrics`.

`emit()`'s encoding, expanded:

```cpp
void otlp_metrics::emit() {
    if (!_exporter || _current.kind == in_progress_metric::kind::unset) {
        _current = {};
        return;
    }
    boost::json::object data_point;
    data_point["attributes"] = _current.attributes;
    data_point["timeUnixNano"] = std::to_string(epoch_nanos_now());
    boost::json::object metric_shape;  // "sum" or "gauge" object
    switch (_current.kind) {
        case in_progress_metric::kind::sum_int:
            data_point["asInt"] = std::to_string(_current.int_value);
            metric_shape["dataPoints"] = boost::json::array{data_point};
            metric_shape["aggregationTemporality"] = "AGGREGATION_TEMPORALITY_DELTA";
            metric_shape["isMonotonic"] = true;
            break;
        case in_progress_metric::kind::gauge_int:
            data_point["asInt"] = std::to_string(_current.int_value);
            metric_shape["dataPoints"] = boost::json::array{data_point};
            break;
        case in_progress_metric::kind::gauge_double:
            data_point["asDouble"] = _current.double_value;
            metric_shape["dataPoints"] = boost::json::array{data_point};
            break;
        case in_progress_metric::kind::unset:
            std::unreachable();  // guarded above
    }
    boost::json::object metric;
    metric["name"] = _current.name;
    metric["unit"] = std::string(_current.unit);
    metric[_current.kind == in_progress_metric::kind::sum_int ? "sum" : "gauge"] =
        std::move(metric_shape);

    _exporter->enqueue(std::move(metric));
    _current = {};
}
```

### 4. `include/raft/otlp_logger.hpp`

```cpp
namespace kythira {

class otlp_logger {
public:
    struct config {
        otlp_resource resource;
        std::string endpoint;  // "" = no-op instance
        std::vector<std::pair<std::string, std::string>> extra_headers;
        std::string scope_version;
        std::optional<log_level> min_level;
        std::size_t max_batch_size{512};
        std::size_t max_queued_records{2048};
        std::chrono::milliseconds flush_interval{5000};
        std::chrono::milliseconds shutdown_flush_timeout{2000};
        std::chrono::milliseconds connection_timeout{5000};
        std::chrono::milliseconds read_timeout{5000};
    };

    otlp_logger() = default;
    explicit otlp_logger(config cfg);

    void log(log_level level, std::string_view message);
    void log(log_level level, std::string_view message,
             const std::vector<std::pair<std::string_view, std::string_view>>& key_value_pairs);

    void trace(std::string_view message);
    void trace(std::string_view message,
              const std::vector<std::pair<std::string_view, std::string_view>>& kvs);
    // ... debug/info/warning/error/critical, each overload pair, all
    // delegating to log() (Requirement 2.4) ...

private:
    void log_impl(log_level level, std::string_view message,
                 const std::vector<std::pair<std::string_view, std::string_view>>* kvs);

    config _cfg;
    std::shared_ptr<detail::otlp_exporter> _exporter;  // null for no-op instances
};

static_assert(kythira::diagnostic_logger<otlp_logger>);

}  // namespace kythira
```

`log_impl` is the single encoding path every convenience method and both
`log()` overloads funnel through: checks `_cfg.min_level` (Requirement 2.6),
builds the `LogRecord` JSON object (severity mapping per Requirement 3,
`body`/`attributes` per Requirement 2.3), and enqueues.

### Severity mapping table (Requirement 3.1)

```cpp
namespace kythira::detail {

struct otlp_severity { int number; std::string_view text; };

constexpr auto otlp_severity_for(log_level level) -> otlp_severity {
    switch (level) {
        case log_level::trace:    return {1, "TRACE"};
        case log_level::debug:    return {5, "DEBUG"};
        case log_level::info:     return {9, "INFO"};
        case log_level::warning:  return {13, "WARN"};
        case log_level::error:    return {17, "ERROR"};
        case log_level::critical: return {21, "FATAL"};
    }
    std::unreachable();
}

}  // namespace kythira::detail
```

## Data Models

### Metrics export body

`otlp_metrics` recording `_metrics.set_metric_name("command_received");
_metrics.add_dimension("node_id", "node1"); _metrics.add_one();
_metrics.emit();` against a resource `{service_name: "kythira-raft",
service_instance_id: "node1"}` produces:

```json
{
  "resourceMetrics": [
    {
      "resource": {
        "attributes": [
          {"key": "service.name", "value": {"stringValue": "kythira-raft"}},
          {"key": "service.instance.id", "value": {"stringValue": "node1"}}
        ]
      },
      "scopeMetrics": [
        {
          "scope": {"name": "kythira", "version": ""},
          "metrics": [
            {
              "name": "command_received",
              "unit": "1",
              "sum": {
                "dataPoints": [
                  {
                    "attributes": [
                      {"key": "node_id", "value": {"stringValue": "node1"}}
                    ],
                    "timeUnixNano": "1737033600000000000",
                    "asInt": "1"
                  }
                ],
                "aggregationTemporality": "AGGREGATION_TEMPORALITY_DELTA",
                "isMonotonic": true
              }
            }
          ]
        }
      ]
    }
  ]
}
```

A batch of `N` metrics recorded before the next flush appends `N` entries to
the same `"metrics"` array within one `ScopeMetrics` — the `Resource`/scope
wrapper is written once per POST, not once per metric.

### Logs export body

`_logger.warning("Quorum lost — no autonomous provisioning attempted", {})`
against the same resource:

```json
{
  "resourceLogs": [
    {
      "resource": { "attributes": [ /* same as above */ ] },
      "scopeLogs": [
        {
          "scope": {"name": "kythira", "version": ""},
          "logRecords": [
            {
              "timeUnixNano": "1737033600000000000",
              "severityNumber": 13,
              "severityText": "WARN",
              "body": {"stringValue": "Quorum lost — no autonomous provisioning attempted"},
              "attributes": []
            }
          ]
        }
      ]
    }
  ]
}
```

## Correctness Properties

### Property 1: No public method blocks on network I/O
**Validates: Requirements 1.10, 2.5, 6.2**

Every public method on `otlp_metrics`/`otlp_logger` either returns
immediately (no-op instance) or ends in `detail::otlp_exporter::enqueue()`,
whose only blocking operation is acquiring `_mu` (a queue-local mutex held
only for a `push_back` + optional oldest-drop, never across any HTTP call).
The HTTP POST happens exclusively on the background thread (`run()`).

### Property 2: Copies of the same logical instance share one export pipeline
**Validates: Requirements 1.3, 2.2, 6.6**

`otlp_metrics`/`otlp_logger` hold `std::shared_ptr<detail::otlp_exporter>`,
obtained from `otlp_exporter::get_or_create()`'s process-wide registry keyed
on `(resource, endpoint, path)`. Copy-constructing either class copies the
`shared_ptr` (referencing the same exporter, same queue, same background
thread), not the exporter itself. Any two instances/copies constructed from
`config`s with equal `resource`/`endpoint` therefore always resolve to the
same exporter regardless of which object's constructor ran first.

### Property 3: Distinct resources never share a queue
**Validates: Requirement 4.2**

The registry key includes `resource` by value (via `otlp_resource`'s
defaulted `operator==`), so two configs differing in even one attribute
resolve to two different registry entries and thus two different
exporters/threads/queues — no code path ever appends one resource's records
into a batch tagged with a different resource's `Resource` object.

### Property 4: Backpressure drops oldest, never blocks the caller
**Validates: Requirement 6.4**

`enqueue()` is `push_back` + `if (queue.size() > max_queued_records)
pop_front()` under the same lock — bounded, O(1) amortized, and never waits
on the background thread or the network. The caller-visible cost of
sustained backpressure is silent data loss (tracked via
`dropped_record_count()`), not latency.

### Property 5: Shutdown flush is best-effort and bounded
**Validates: Requirement 6.7**

The destructor signals `_running = false`, notifies `_cv`, and joins the
thread; the thread's final iteration attempts one more flush bounded by
`shutdown_flush_timeout` via the same connection/read timeouts already
configured on the `httplib::Client` (not a separate timeout mechanism) — a
collector that is completely unreachable at shutdown delays process exit by
at most `connection_timeout + read_timeout`, never indefinitely.

## Error Handling

- **Collector unreachable / non-2xx response**: the batch is dropped after
  the attempt; a single rate-limited `stderr` line is emitted (Requirement
  6.5). No exception escapes `enqueue()` or the background thread — a
  `try`/`catch (...)` wraps the entire flush body, matching this project's
  established "swallow and log, never crash the caller over telemetry"
  posture (no existing Kythira component treats a metrics/logging failure
  as fatal).
- **Malformed `config` (empty `otlp_resource.service_name`)**: throws
  `std::invalid_argument` from the constructor, synchronously — this is a
  programmer error (a missing required field), not a runtime condition, so
  it fails loud and immediately rather than silently degrading to a no-op
  (contrast with `config.endpoint` being empty, which is the *documented*
  way to request a no-op instance, Requirement 8.4).
- **Fault injection** (`raft/otlp/exporter/flush/fail` /
  `.../flush/noop`, Requirement 7): identical shape to every existing DNS
  discovery class's `send_update`/`send_update_rr` fault points — `fail`
  throws before the real HTTP call, `noop` returns success before it.

## Testing Strategy

- **Unit tests** (`tests/otlp_metrics_unit_test.cpp`,
  `tests/otlp_logger_unit_test.cpp`): construct against a
  `tests/otlp_test_collector.hpp` mock (an `httplib::Server` capturing every
  POST body it receives into a `std::vector<boost::json::value>`, mirroring
  `tests/acme_test_server.hpp`'s "construct it and go" shape). Assert:
  concept `static_assert`s compile; a no-op instance (empty `endpoint`)
  never touches the mock; `set_metric_name`/`add_dimension`/`add_one`/
  `emit()` produces the exact JSON shape from the Data Models section;
  `add_duration`/`add_value` produce a `gauge`, `add_one`/`add_count` a
  `sum`; severity mapping (Requirement 3.1's table, one test case per
  `log_level`); batching (N records under `max_batch_size` flush together
  on one POST after `flush_interval`); backpressure (`dropped_record_count()`
  increments once `max_queued_records` is exceeded); shared-exporter
  identity (two `config`s with equal `resource`/`endpoint` — assert the
  mock receives interleaved records from both objects in one shared batch,
  not two separate connections).
- **Chaos tests** (`tests/chaos/otlp_exporter_chaos_test.cpp`): the two
  fault points from Requirement 7 — `flush/fail` asserts
  `dropped_record_count()` increments and no exception propagates to the
  caller; `flush/noop` asserts the mock collector receives nothing while
  `emit()`/`log()` still return normally.
- **Docker scenario test — stretch/optional** (tasks.md's final wave): a
  real OpenTelemetry Collector container (`otel/opentelemetry-collector`
  image) configured with a `debug` exporter, plus a small demo binary
  emitting known metrics/logs, verifying end-to-end delivery through an
  actual Collector rather than the in-process mock. Follows this project's
  existing container-runtime-compatibility rules (`CLAUDE.md`): no static
  IPs (service-name addressing, e.g. `http://otlp-test-collector:4318`),
  `container_runtime()`/`compose_prefix()` from
  `tests/docker_chaos/os_faults.hpp`, no `--privileged`/host networking.
  Marked optional because the in-process mock already exercises every wire-
  format and batching behavior this spec defines; the Collector container
  only additionally validates that a *real* Collector's `otlphttp` receiver
  accepts what this exporter sends, which is valuable but not required to
  validate this spec's own correctness properties.

## Dependencies

```
boost::json   (already required)   OTLP/HTTP JSON body construction
httplib       (already required)   OTLP/HTTP POST transport, TLS when
                                    CPPHTTPLIB_OPENSSL_SUPPORT is enabled
                                    and the endpoint scheme is https://
```

No new dependency, no new CMake optional-dependency gate (Requirement 10).

## Adoption

Neither class is wired into any `cmd/*` binary's default `raft_types`/
`tcp_raft_types` specialization (Requirement's "Out of Scope"). An operator
opts in by defining their own types specialization, e.g.:

```cpp
struct my_raft_types : kythira::tcp_raft_types<...> {
    using metrics_type = kythira::otlp_metrics;
    using logger_type = kythira::otlp_logger;
};

kythira::otlp_resource resource{
    .service_name = "my-kythira-cluster",
    .service_instance_id = node_id_to_string(my_node_id),
};
kythira::otlp_metrics metrics{{.resource = resource, .endpoint = "http://otel-collector:4318"}};
kythira::otlp_logger logger{{.resource = resource, .endpoint = "http://otel-collector:4318"}};
```

This mirrors how `rfc6763_ldns_peer_discovery`/`poco_peer_discovery` are
adopted today — a library-level building block an operator's own binary
wires in, not a change to any existing binary's defaults.
