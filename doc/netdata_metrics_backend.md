# NetData Metrics Backend

`netdata_metrics` (`include/raft/netdata_metrics.hpp`) satisfies Kythira's
`kythira::metrics` concept by emitting StatsD datagrams over UDP to
NetData's built-in StatsD collector (port 8125 by default), for operators
already running NetData for host-level monitoring who want Kythira's
Raft/RPC metrics on the same dashboard. Requirements live in
`doc/TODO.md`'s "Metrics Backends" section; this document is the
operator-facing reference.

## Scope — and an honest caveat

StatsD aggregates by metric NAME: NetData renders one chart per name, and
dimensions ride along as DataDog-style tags (`|#key:value,...`), which
NetData accepts and surfaces as chart labels — **not** as separate time
series, and older NetData versions drop them entirely when re-exporting
(netdata/netdata#3813). If you need per-dimension series, use the
Prometheus or Telegraf backend; this one exists for the same-dashboard
convenience its TODO entry describes. `netdata_metrics_config::include_tags
= false` strips tags from the wire entirely.

Transport is the same shared non-blocking exporter as the Telegraf backend
(`include/raft/metrics_line_exporter.hpp`): bounded queue, background UDP
sender, drop-oldest overflow, no retry, `dropped_line_count()` for
observability.

## Enabling it on `chaos_node`

| Variable | Default | Meaning |
|---|---|---|
| `NETDATA_STATSD_ENDPOINT` | unset | `host:port` of NetData's StatsD listener, e.g. `netdata:8125`. Setting this is what turns the backend on. |

One metrics backend per run: every other backend's env var takes
precedence over this one (see `cmd/chaos_node/main.cpp`).

See `docker/netdata/netdata.conf` for an example NetData config (pinning
the StatsD collector on explicitly) and
`docker/netdata-metrics-compose.yml` for a complete, runnable single-node
`chaos_node` + real NetData pair (the same compose file
`tests/docker_chaos/netdata_metrics_scenario_test.cpp` drives, via the
`docker-netdata-metrics-tests` CMake target).

## Wire shape

- `add_one()`/`add_count(n)` → `name:<n>|c` (counter).
- `add_value(v)` → `name:<v>|g` (gauge).
- `add_duration(d)` → `name:<ms>|ms` (timer — NetData renders
  min/max/avg/percentile dimensions per chart).
- `:`, `|`, newline, and space in names (and `,` in tags) are protocol
  delimiters with no escape syntax; they are replaced with `_`.
- Multiple metrics pack into one datagram newline-separated, up to
  `metrics_line_exporter_config::max_payload_bytes` (default 1400).
