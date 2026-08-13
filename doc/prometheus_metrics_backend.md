# Prometheus Metrics Backend

`prometheus_metrics` (`include/raft/prometheus_metrics.hpp`) satisfies
Kythira's `kythira::metrics` concept (`include/raft/metrics.hpp`) by
aggregating into a shared in-process registry that a bundled HTTP server
(`prometheus_scrape_server`) exposes as `GET /metrics` in Prometheus text
exposition format (version 0.0.4). Requirements live in `doc/TODO.md`'s
"Metrics Backends" section; this document is the operator-facing reference.

## Scope

Pull-based only: the node exposes an endpoint and Prometheus scrapes it.
There is no remote-write push — for push semantics against a
Prometheus-compatible store, use the VictoriaMetrics backend
(`doc/victoriametrics_metrics_backend.md`), which shares this backend's
registry and renderer.

Unlike the OTLP backend, the recording path performs no network I/O at all
(one mutex, arithmetic, return), so there is no batching exporter, queue,
or drop accounting to configure.

## Enabling it on `chaos_node`

| Variable | Default | Meaning |
|---|---|---|
| `PROMETHEUS_METRICS_PORT` | unset | Port for the `/metrics` scrape endpoint (bound on `0.0.0.0`). Setting this is what turns the backend on. |

One metrics backend per run: `OTLP_ENDPOINT` takes precedence over this,
and this takes precedence over `VICTORIAMETRICS_ENDPOINT` /
`TELEGRAF_ENDPOINT` / `NETDATA_STATSD_ENDPOINT` (see `cmd/chaos_node/main.cpp`).

See `docker/prometheus/prometheus.yml` for an example scrape config and
`docker/prometheus-metrics-compose.yml` for a complete, runnable
single-node `chaos_node` + real Prometheus pair (the same compose file
`tests/docker_chaos/prometheus_metrics_scenario_test.cpp` drives, via the
`docker-prometheus-metrics-tests` CMake target).

## Wire shape

- `add_one()`/`add_count(n)` → **counter**, cumulative; the family name
  gets the idiomatic `_total` suffix at render time unless the recorded
  name already ends with it (`http.client.request.sent` scrapes as
  `http_client_request_sent_total`).
- `add_value(v)` → **gauge**, last value wins.
- `add_duration(d)` → **histogram** with cumulative `le` buckets, `_sum`,
  and `_count`; bounds are `prometheus_metrics_config::histogram_bounds_ms`
  (default 1 ms – 10 s, the same spread as the OTLP backend).
- Dimensions become labels, sorted so `add_dimension` call order never
  splits a series; invalid characters in names sanitize to `_`, label
  values escape `\`, `"`, and newline.
- A metric name must be used with exactly one recording shape — two shapes
  under one name would render conflicting `# TYPE` lines, which Prometheus
  rejects at scrape time.

## Embedding outside chaos_node

Construct one `std::shared_ptr<prometheus_registry>`, hand
`prometheus_metrics{registry}` to the node/transport (the handle is
copyable; all copies share the registry — the copy-per-emission idiom the
HTTP transports use is the intended usage), and keep a
`prometheus_scrape_server{registry, bind_address, port}` alive for the
process lifetime. Port 0 binds an ephemeral port, reported by `.port()`.
