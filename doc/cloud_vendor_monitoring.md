# Cloud-Vendor Monitoring Configurations

The five cloud-vendor monitoring services (AWS CloudWatch, Azure Monitor,
GCP Cloud Monitoring, OCI Monitoring, Alibaba Cloud CloudMonitor) are
deliberately **configuration-only** integrations: Kythira ships no bespoke
`kythira::metrics`/`kythira::diagnostic_logger` implementation per vendor.
The node emits vendor-neutral telemetry — OTLP/HTTP JSON
(`doc/otlp_telemetry_backend.md`, enabled via `OTLP_ENDPOINT`) or a
Prometheus scrape endpoint (`doc/prometheus_metrics_backend.md`, enabled
via `PROMETHEUS_METRICS_PORT`) — and an OpenTelemetry Collector or the
vendor's own agent owns the vendor-specific ingestion. Writing five
vendor-SDK integrations inside Kythira would duplicate integration work
the Collector ecosystem already does well and tie Kythira's dependency
footprint to every vendor SDK; the rationale is recorded at the top of
`doc/TODO.md`'s Metrics Backends section.

The example configs live in [`docker/cloud-monitoring/`](../docker/cloud-monitoring/).
Each is deployable as-is after substituting the environment variables (or,
for OCI, the two marked properties) it declares.

| Vendor | Example config | Mechanism |
|---|---|---|
| AWS CloudWatch | `cloudwatch-collector-config.yaml` | Collector: `awsemf` (metrics as EMF → CloudWatch Logs → server-side extraction) + `awscloudwatchlogs` (logs) |
| Azure Monitor | `azure-monitor-collector-config.yaml` | Collector: `azuremonitor` (metrics + logs into an Application Insights resource) |
| GCP Cloud Monitoring | `gcp-cloud-monitoring-collector-config.yaml` | Collector: `googlecloud` (metrics via `timeSeries.create`, logs via Cloud Logging) |
| OCI Monitoring | `oci-management-agent-prometheus-emitter.properties` | Vendor agent: Management Agent PrometheusEmitter scrapes the node's Prometheus endpoint, posts via `PostMetricData` |
| Alibaba CloudMonitor | `alibaba-cloudmonitor-collector-config.yaml` | Collector: `prometheusremotewrite` into a CloudMonitor 2.0 Prometheus instance |

All Collector configs pin the same image/version the OTLP scenario test
uses (`otel/opentelemetry-collector-contrib:0.116.1`); they receive
whatever the node already emits on `OTLP_ENDPOINT` — deployment means
running a Collector next to your nodes with the config file and the
credentials environment, then pointing `OTLP_ENDPOINT` at it.

## Per-vendor notes

### AWS CloudWatch

Environment: ambient AWS credentials/region (SDK default resolution).
`KYTHIRA_CLOUDWATCH_ENDPOINT` overrides the CloudWatch Logs endpoint —
leave it unset/empty for real AWS; the LocalStack compose sets it. Metrics
arrive in the `Kythira/ChaosNode` namespace (extracted server-side from
the EMF documents in `/kythira/chaos-node/metrics`); logs arrive in
`/kythira/chaos-node/logs`. Pairs naturally with
`aws_ec2_quorum_manager`/`aws_asg_quorum_manager`.

### Azure Monitor

Environment: `APPLICATIONINSIGHTS_CONNECTION_STRING` — an Application
Insights resource's connection string (a secret: it authorizes ingestion).
Metrics land as `customMetrics`, logs as `traces` records, both queryable
with KQL. Ingestion latency is minutes, not seconds — size any alerting
accordingly.

### GCP Cloud Monitoring

Environment: `GCP_PROJECT_ID` (may be empty — falls back to the
credentials' project) and Application Default Credentials
(`GOOGLE_APPLICATION_CREDENTIALS`, or ambient on GCE/GKE) with
`roles/monitoring.metricWriter` + `roles/logging.logWriter`. Metrics
arrive as `workload.googleapis.com/<name>`; logs under
`kythira-chaos-node` in Cloud Logging.

### OCI Monitoring

The one non-Collector integration: opentelemetry-collector-contrib (as of
v0.116) has **no OCI Monitoring exporter**, so the vendor's own Management
Agent is the routing mechanism — its PrometheusEmitter plugin scrapes the
node's existing Prometheus endpoint (`PROMETHEUS_METRICS_PORT`, the same
`prometheus_scrape_server` the Prometheus backend documents) and posts to
OCI Monitoring under the `kythira_chaos_node` namespace. Substitute `url`
and `compartmentId` in the `.properties` file, drop it into
`/opt/oracle/mgmt_agent/agent_inst/discovery/PrometheusEmitter/` on a host
running the agent, and grant the agents' dynamic group `use metrics` on
the compartment/namespace. OCI-side logging is out of scope for this
entry: the agent does not forward application logs, and OCI Logging
ingestion is a separate product/API — same doc-only reasoning as the
NetData logging leg (`doc/netdata_metrics_backend.md`).

### Alibaba Cloud CloudMonitor

CloudMonitor's legacy custom-metrics upload API was deprecated by Alibaba
in September 2024; the current ingestion path is CloudMonitor 2.0's
Prometheus-compatible remote write, which is what the example config uses
(`prometheusremotewrite` + HTTP Basic auth with a RAM AccessKey pair —
scope that key to CloudMonitor only). Environment:
`KYTHIRA_CMS_REMOTE_WRITE_URL` (the instance's `.../api/v3/write` URL),
`ALIBABA_CLOUD_ACCESS_KEY_ID`/`ALIBABA_CLOUD_ACCESS_KEY_SECRET`
(VPC-internal password-free writes can drop the auth block). Logs are out
of scope for this entry: CloudMonitor does not ingest logs — Alibaba's log
product is Log Service (SLS), reachable from the same Collector via the
`alibabacloudlogservice` exporter if needed.

## How these are tested

Per `doc/TODO.md`'s Metrics Backends testing requirement, each entry has
two tiers:

- **Docker tier (enabled by default, runs wherever a container runtime
  exists — e.g. the arm64 smoke workflow):**
  - *CloudWatch* is the one vendor with a self-hostable emulator of its
    ingestion API (LocalStack — the same emulator
    `aws_quorum_manager_localstack_test.cpp` uses), so it gets a full data
    round-trip: `tests/docker_chaos/cloudwatch_metrics_scenario_test.cpp`
    (CMake target `docker-cloudwatch-metrics-tests`) runs a real
    chaos_node → Collector (unmodified example config) → LocalStack, and
    reads the EMF documents and log records back out through the
    CloudWatch Logs API.
  - *The other four* have no emulator to round-trip against, so
    `tests/docker_chaos/cloud_monitoring_config_validation_test.cpp`
    (target `docker-cloud-monitoring-config-tests`) validates each
    Collector config with the Collector binary's own `validate` command
    (schema-level: a misspelled key or broken exporter config fails), and
    checks the OCI `.properties` for the vendor-documented required keys.
- **Real-cloud tier (disabled by default — real credentials, real cost):**
  one lightweight job per vendor in
  `.github/workflows/real-cloud-tests.yml` (`<provider>-monitoring`),
  gated by `REAL_CLOUD_TESTS_<PROVIDER>_MONITORING_ENABLED` plus the
  master switch. Each stands up the routing mechanism the example config
  describes — a real Collector running the unmodified file (OCI: the real
  Management Agent) — sends one synthetic probe (`kythira_ci_monitoring_probe`,
  the same OTLP/HTTP JSON wire shape `otlp_metrics` emits), and confirms
  arrival through the vendor's own query API. See
  `scripts/real-cloud-monitoring/` and
  `scripts/ci-cloud-credentials/README.md` for provisioning. As of August
  2026 the AWS/Azure/GCP jobs are runnable once their (documented)
  monitoring-specific variables are set; the OCI and Alibaba jobs are
  fully wired but have never run against the live services — their
  credentials/resources are not provisioned yet, and each fails closed
  naming exactly what is missing.
