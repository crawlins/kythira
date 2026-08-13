# Telegraf Metrics Backend

`telegraf_metrics` (`include/raft/telegraf_metrics.hpp`) satisfies
Kythira's `kythira::metrics` concept by emitting InfluxDB line protocol
over UDP (default) or TCP to a Telegraf agent's `socket_listener` input.
The point of this backend is fan-out inheritance: once Telegraf parses the
lines, any output plugin the operator already runs (InfluxDB, Graphite,
Kafka, ...) receives Kythira's metrics without Kythira picking a
destination. Requirements live in `doc/TODO.md`'s "Metrics Backends"
section; this document is the operator-facing reference.

## Scope

InfluxDB line protocol rather than StatsD, deliberately: line protocol
carries dimensions as first-class tags with no extension syntax, and
Telegraf parses it natively (`data_format = "influx"`). Rendering happens
on `emit()`; the line is queued on a shared non-blocking exporter
(`include/raft/metrics_line_exporter.hpp` — bounded queue, background
sender thread, drop-oldest overflow) so the recording path never touches a
socket. Sends are fire-and-forget with no retry — metric datagrams are
inherently lossy, and `dropped_line_count()` makes the loss observable.

## Enabling it on `chaos_node`

| Variable | Default | Meaning |
|---|---|---|
| `TELEGRAF_ENDPOINT` | unset | `host:port` of the agent's socket_listener, e.g. `telegraf:8094`. Setting this is what turns the backend on. |
| `TELEGRAF_PROTOCOL` | `udp` | `udp` or `tcp`. |

One metrics backend per run: `OTLP_ENDPOINT`, `PROMETHEUS_METRICS_PORT`,
and `VICTORIAMETRICS_ENDPOINT` take precedence (see
`cmd/chaos_node/main.cpp`).

See `docker/telegraf/telegraf.conf` for an example agent config (swap its
`[[outputs.file]]` for your real output) and
`docker/telegraf-metrics-compose.yml` for a complete, runnable single-node
`chaos_node` + real Telegraf pair (the same compose file
`tests/docker_chaos/telegraf_metrics_scenario_test.cpp` drives, via the
`docker-telegraf-metrics-tests` CMake target).

## Wire shape

One line per `emit()`: `measurement[,tag=value...] field=value <ns>`.

- Measurement = the metric name (`,` and ` ` escaped, per the protocol).
- Dimensions → tags, in `add_dimension` order.
- `add_one()`/`add_count(n)` → `count=<n>i` (integer field; multiple calls
  before one `emit()` accumulate).
- `add_value(v)` → `value=<v>`.
- `add_duration(d)` → `duration_ms=<ms>`.
- Timestamp is nanoseconds since epoch, stamped at `emit()`.

Payloads pack multiple newline-separated lines up to
`metrics_line_exporter_config::max_payload_bytes` (default 1400, one safe
UDP datagram).
