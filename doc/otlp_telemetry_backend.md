# OTLP Telemetry Backend

`otlp_metrics` (`include/raft/otlp_metrics.hpp`) and `otlp_logger`
(`include/raft/otlp_logger.hpp`) satisfy Kythira's `kythira::metrics`
(`include/raft/metrics.hpp`) and `kythira::diagnostic_logger`
(`include/raft/logger.hpp`) concepts by emitting OTLP (OpenTelemetry
Protocol) data to a collector. Design rationale, requirements, and the full
implementation plan live at `.kiro/specs/otlp-telemetry-backend/`; this
document is the operator-facing "how do I turn this on" reference.

## Scope

This implements **OTLP/HTTP with JSON encoding only** — `POST` to
`<endpoint>/v1/metrics` and `<endpoint>/v1/logs` with
`Content-Type: application/json`, body encoded using OTLP's proto3-JSON
field mapping. **OTLP/gRPC and OTLP/HTTP protobuf-binary encoding are not
implemented** (see the spec's design.md Non-Goals) — every OpenTelemetry
Collector accepts OTLP/HTTP JSON on the same terms as the other two, so this
is a scope choice to avoid a new protobuf/gRPC build dependency, not a
missing feature relative to what Kythira needs.

## Enabling it on `chaos_node`

Set these environment variables (all optional; unset `OTLP_ENDPOINT` keeps
`chaos_node`'s existing `console_logger`/`noop_metrics` default):

| Variable | Default | Meaning |
|---|---|---|
| `OTLP_ENDPOINT` | unset | Collector base URL, e.g. `http://otel-collector:4318`. Setting this is what turns OTLP on. |
| `OTLP_SERVICE_NAME` | `kythira-chaos-node` | OTLP `service.name` resource attribute. |
| `OTLP_HEADERS` | unset | Comma-separated `key=value` pairs sent as extra HTTP headers on every export (e.g. a collector-required API key: `x-api-key=...`). |

`service.instance.id` is always the node's own `NODE_ID` — no configuration
needed to distinguish nodes in a shared collector's data.

See `docker/docker-compose.yml` for a commented-out example, and
`docker/otlp-collector-compose.yml` for a complete, runnable single-node
`chaos_node` + real OpenTelemetry Collector pair (the same compose file
`tests/docker_chaos/otlp_collector_test.cpp` drives).

## Wire shape

Both classes batch individually-recorded events (never locally
pre-aggregated) and POST them with delta aggregation temporality — see
`.kiro/specs/otlp-telemetry-backend/design.md` for the full component
breakdown and worked JSON examples. Summary:

- `metrics_type::add_one()`/`add_count(n)` → OTLP **Sum**, monotonic, delta
  temporality.
- `metrics_type::add_duration(d)` → OTLP **Histogram**, one sample per data
  point (`count: 1`), bucketed against a fixed default boundary set
  (`otlp_export_config::histogram_bounds_ms`, overridable).
- `metrics_type::add_value(v)` → OTLP **Gauge**.
- `diagnostic_logger`'s six levels map to OTLP `SeverityNumber` as the base
  value of each level's 4-wide band: `trace`→1, `debug`→5, `info`→9,
  `warning`→13, `error`→17, `critical`→21.

## Using a real collector

Point `OTLP_ENDPOINT` at any OpenTelemetry Collector with an `otlp` receiver
listening on its HTTP port (`4318` by default) — self-hosted
(`docker/otel-collector/otel-collector-config.yaml` is a minimal working
example) or a vendor's hosted OTLP ingest endpoint that accepts
OTLP/HTTP JSON. From there, the Collector's own exporters reach Prometheus,
Grafana, Honeycomb, Datadog, New Relic, or any other backend it's configured
for — Kythira itself never needs a vendor-specific integration.
