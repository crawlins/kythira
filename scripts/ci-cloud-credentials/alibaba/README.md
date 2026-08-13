# Alibaba Cloud credentials for CI

Today Alibaba Cloud has exactly one real-cloud test: the CloudMonitor
monitoring-config test (`scripts/real-cloud-monitoring/alibaba-cloudmonitor.sh`,
`alibaba-monitoring` job in `.github/workflows/real-cloud-tests.yml`), which
remote-writes one synthetic metric through the example config
`docker/cloud-monitoring/alibaba-cloudmonitor-collector-config.yaml` into a
CloudMonitor 2.0 Prometheus instance and queries it back. The quorum
manager / certificate provider suites are not implemented yet
(`doc/TODO.md`, Cloud Provider Support).

**No Alibaba Cloud account is provisioned for this project yet** — the test
stays fail-closed (its job errors naming each missing value) until an
operator completes the steps below.

## Deviation from the no-stored-credentials rule

Every other provider here authenticates via OIDC federation with no
long-lived key stored in GitHub. CloudMonitor's Prometheus remote-write
endpoint authenticates with a RAM AccessKey pair over HTTP Basic — there is
no OIDC-federated path to that ingestion endpoint (RAM role STS tokens
carry a security token that Basic auth cannot convey). So this provider
stores an AccessKey pair as repository secrets. Contain the blast radius:
create a dedicated RAM user whose only permission is CloudMonitor
read/write (`AliyunCloudMonitorFullAccess`, or a custom policy scoped to
the one instance), used for nothing else.

## One-time setup

1. In the CloudMonitor console, create (or pick) a CloudMonitor 2.0 /
   Managed Service for Prometheus instance and note, from its settings:
   - the **remote-write URL** (`.../api/v3/write`),
   - the **HTTP query API base URL** for the same instance.
2. Create the dedicated RAM user and an AccessKey pair for it (scope as
   above).
3. In the repository's `real-cloud-tests` environment, set:
   - secrets `ALIBABA_CLOUD_ACCESS_KEY_ID`, `ALIBABA_CLOUD_ACCESS_KEY_SECRET`
   - variables `ALIBABA_CMS_REMOTE_WRITE_URL`, `ALIBABA_CMS_PROM_QUERY_URL`
4. Enable via `REAL_CLOUD_TESTS_ALIBABA_MONITORING_ENABLED=true` (or the
   `alibaba_monitoring_enabled` dispatch input for a one-off run).

## Cost

CloudMonitor 2.0 Prometheus ingestion is billed per sample; one metric per
run is fractions of a cent. There is nothing to tear down after a run — the
test writes time-series data only.
