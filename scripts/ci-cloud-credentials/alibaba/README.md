# Alibaba Cloud credentials for CI

Alibaba Cloud has three real-cloud tests, in two different jobs.

The `alibaba` job runs two **bundles**, both authenticated by RAM
`AssumeRoleWithOIDC` through the vendor's own action — no stored key:

| Bundle | Test binary | Toggle | Dispatch input | Extra variables |
|---|---|---|---|---|
| `ess-quorum-manager` | `alibaba_quorum_manager_real_test` | `REAL_CLOUD_TESTS_ALIBABA_ESS_QUORUM_ENABLED` | `alibaba_bundle_ess_quorum` | `ALIBABA_SCALING_GROUP_ID` |
| `oss-persistence` | `alibaba_oss_persistence_real_test` | `REAL_CLOUD_TESTS_ALIBABA_OSS_PERSISTENCE_ENABLED` | `alibaba_bundle_oss_persistence` | `ALIBABA_OSS_BUCKET` |

Neither binary is CTest-registered — they need real credentials and spend
real money — so the job builds them as named targets and runs them directly.
Both sit under `REAL_CLOUD_TESTS_ALIBABA_ENABLED` and the master
`REAL_CLOUD_TESTS_ENABLED`. Provision the role and its per-bundle policies
with:

```sh
scripts/ci-cloud-credentials/alibaba/provision-oidc-role.sh \
    --github-org ORG --github-repo REPO \
    --bundles ess-quorum-manager,oss-persistence \
    --bucket kythira-ci-5633986662052576
```

`oss-persistence` is Alibaba's member of the object-persistence family
described in [`../README.md`](../README.md); its policy
(`policies/oss-persistence.json`) is object get/put/delete/list scoped to the
one bucket, with no bucket administration. **It runs four of the shared five
checks, and the missing one is a finding rather than a gap:** OSS cannot
express an overwrite compare-and-swap (`400 NotImplemented`), so the fenced
suite is *uninstantiable* over `alibaba_oss_client` rather than skipped — it
would not compile. See the spec's Requirement 9.8.

Separately, the `alibaba-monitoring` job runs the CloudMonitor
monitoring-config test (`scripts/real-cloud-monitoring/alibaba-cloudmonitor.sh`),
which remote-writes one synthetic metric through the example config
`docker/cloud-monitoring/alibaba-cloudmonitor-collector-config.yaml` into a
CloudMonitor 2.0 Prometheus instance and queries it back. It has its own
toggle and no bundle layer, and it is the one path here that stores a
credential — see the next section for why.

No certificate provider is implemented for this provider (`doc/TODO.md`,
Cloud Provider Support).

## Deviation from the no-stored-credentials rule — the monitoring test only

This applies to the `alibaba-monitoring` job and **not** to the two bundles
above, which federate like every other provider here.

CloudMonitor's Prometheus remote-write
endpoint authenticates with a RAM AccessKey pair over HTTP Basic — there is
no OIDC-federated path to that ingestion endpoint (RAM role STS tokens
carry a security token that Basic auth cannot convey). So this provider
stores an AccessKey pair as repository secrets. Contain the blast radius:
create a dedicated RAM user whose only permission is CloudMonitor
read/write (`AliyunCloudMonitorFullAccess`, or a custom policy scoped to
the one instance), used for nothing else.

## One-time setup (monitoring test)

The two bundles' setup is `provision-oidc-role.sh`, above. What follows is
only for the `alibaba-monitoring` job.

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

The `oss-persistence` bundle is comparably cheap: a few hundred object
operations and a few hundred kilobytes held for the length of the run, against
a bucket that is otherwise empty. `ess-quorum-manager` is the expensive one —
it launches a real ECS instance, and the job's post-run audit fails the build
if the scaling group does not come back to zero capacity.

**One measurement in this suite is not what it looks like.** The OSS bucket is
in `ap-southeast-1` while every other provider's is in the US, so its ~1.5 s
`save_current_term` p50 is a **cross-ocean distance measurement, not a
statement about OSS**. Do not quote it as a provider comparison.
