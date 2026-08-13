# VictoriaMetrics Metrics Backend

`victoriametrics_metrics` (`include/raft/victoriametrics_metrics.hpp`)
satisfies Kythira's `kythira::metrics` concept by sharing the Prometheus
backend's registry and text renderer (`include/raft/prometheus_metrics.hpp`)
and adding a background pusher that POSTs the rendered exposition to
VictoriaMetrics' `/api/v1/import/prometheus` endpoint every
`push_interval`. Requirements live in `doc/TODO.md`'s "Metrics Backends"
section; this document is the operator-facing reference.

## Scope

Push-based, in Prometheus text format — exactly the sharing the TODO entry
predicted ("likely shares most of the Prometheus implementation's wire
format"). This is not Prometheus remote-write (protobuf+snappy); VM's
import endpoint ingests the same text exposition a scraper would read,
which keeps the wire format human-debuggable (`curl` the same body by
hand) and the implementation a fraction of remote-write's size.

Failure semantics worth knowing: pushing the **cumulative** exposition
every interval means a failed push loses resolution, not data — the next
push carries the same monotonic totals. Failed pushes are therefore
counted (`failed_push_count()`) and never retried.

## Enabling it on `chaos_node`

| Variable | Default | Meaning |
|---|---|---|
| `VICTORIAMETRICS_ENDPOINT` | unset | VM base URL, e.g. `http://victoriametrics:8428`. Setting this is what turns the backend on. |

`chaos_node` attaches `job=<OTLP_SERVICE_NAME>` (default
`kythira-chaos-node`) and `instance=<NODE_ID>` as constant labels on every
series — pushed data has no scrape target to inherit identity from, so it
travels in the labels (`victoriametrics_config::constant_labels`).

One metrics backend per run: `OTLP_ENDPOINT` and
`PROMETHEUS_METRICS_PORT` take precedence (see `cmd/chaos_node/main.cpp`).

See `docker/victoriametrics-metrics-compose.yml` for a complete, runnable
single-node `chaos_node` + real VictoriaMetrics pair (the same compose
file `tests/docker_chaos/victoriametrics_metrics_scenario_test.cpp`
drives, via the `docker-victoriametrics-metrics-tests` CMake target).

## Wire shape

Identical to the Prometheus backend's exposition (counters with `_total`,
gauges, histograms; see `doc/prometheus_metrics_backend.md`), delivered as
a `text/plain` POST body instead of a scrape response. Query it back with
VM's Prometheus-compatible `/api/v1/query`.

## Composing with a scrape endpoint

`victoriametrics_metrics::registry()` exposes the shared
`prometheus_registry`, so a deployment can push to VM *and* expose a local
`prometheus_scrape_server` from the same aggregate state — useful while
migrating between pull and push.
