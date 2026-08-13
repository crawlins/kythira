# Kythira — session handoff (August 13, 2026, ~15:00 UTC)

Repo: `/home/clark/src/kythira`. Read `doc/TODO.md` and `CLAUDE.md` first.
**Verify every claim here against the tree and real runs — this file is notes,
not truth.** Twenty-three sessions of the doctrine. This session's additions:
**a vendor's image tag scheme can encode its license** (LocalStack's
date-versioned 2026.x tags are the paid distribution and exit 55 without an
auth token; the free line stayed at 4.x — the container's own log named it),
and **the same metric has different wire names on different paths** (OTLP
carries `raft_heartbeat_sent`; `_total` is Prometheus-exposition rendering —
an assertion copied across backends failed on exactly that).

## State at handoff

**#233 MERGED at 14:07:04 UTC and #234 (live-verification docs) MERGED at ~14:55 UTC** (both verified, rebase). Checkout on `main`
at `75dd734`, fast-forwarded, clean. RESUME.md is the only untracked file;
the foreign stash (`WIP on docs/metrics-backend-testing-tiers`) remains at
`stash@{0}` — **never pop it**. Nothing in flight.

Merged this session:
- **#233** — the five cloud-vendor monitoring entries (CloudWatch / Azure
  Monitor / GCP / OCI / Alibaba CloudMonitor), executed as the config-only
  integrations the TODO prescribes. Example configs in
  `docker/cloud-monitoring/`, operator doc `doc/cloud_vendor_monitoring.md`.
  Docker tier: `docker-cloudwatch-metrics-tests` (chaos_node → Collector
  running the unmodified example config → LocalStack round-trip) +
  `docker-cloud-monitoring-config-tests` (Collector `validate` on the other
  four configs, required-key check for OCI's `.properties`); smoke run
  31704919261 = fully green dispatch, both new steps included. Real-cloud
  tier: five `<provider>-monitoring` jobs + `scripts/real-cloud-monitoring/`,
  all disabled by default behind `REAL_CLOUD_TESTS_<P>_MONITORING_ENABLED`.

Findings worth keeping (all encoded in code/docs, listed for recall):
- LocalStack `2026.x` = licensed distribution, needs LOCALSTACK_AUTH_TOKEN
  (exit 55); community line is `4.x` (pinned 4.14.0 in
  `docker/cloudwatch-localstack-compose.yml`, comment records the run).
- Routing substitutions recorded in TODO/doc: Alibaba's custom-metrics
  upload API was deprecated Sept 2024 → `prometheusremotewrite` into
  CloudMonitor 2.0; collector-contrib (v0.116) has NO OCI exporter → OCI
  Management Agent PrometheusEmitter scraping `PROMETHEUS_METRICS_PORT`.
- The real-cloud monitoring jobs need no C++ build — host Collector binary
  + one synthetic OTLP probe + the vendor's query API as oracle.

## Honest-status ledger (real-cloud monitoring tier)

- **AWS**: **VERIFIED LIVE** (August 13, ~14:38 UTC, dispatch run
  31711151464) — bundle provisioned via provision-oidc-role.sh with the
  full five-bundle list (before/after get-role-policy diff = pure
  addition), probe metric extracted into Kythira/ChaosNode by real
  CloudWatch in 9 s, log record confirmed at 10 s. PR #234 records this
  in doc/TODO/CHANGELOG. REAL_CLOUD_TESTS_AWS_MONITORING_ENABLED=true
  set at user request (~15:10 UTC) — the job now runs in the weekly
  Monday 06:00 UTC scheduled run alongside the other enabled suites
  (~$0.002/run, log groups deleted each run).
- **Azure**: needs an App Insights resource + the two values in
  `azure/README.md` (Monitoring section). Not yet dispatched live.
- **GCP**: needs three extra roles on the CI SA (`gcp/README.md`). Not yet
  dispatched live.
- **OCI**: never run live — needs `OCI_MGMT_AGENT_INSTALL_KEY` secret +
  `OCI_MGMT_AGENT_INSTALLER_URL` var; the script's response-file keys must
  be checked against the installer ZIP's `input.rsp.example` on first use
  (flagged in script + README).
- **Alibaba**: never run live — no Alibaba account exists; stored-AK
  deviation documented in `alibaba/README.md`.

## Environment gotchas (standing)

- No container runtime on this machine; scenario iteration = dispatching
  arm64-docker-smoke-test (~12-15 min warm). It is expected FULLY green —
  a red step is a finding (poco/dns keep documented continue-on-error).
- `gh run view --json` shows conclusion "success" for continue-on-error
  steps that failed — read logs, not conclusions, for masked steps.
- The metrics-scenario fixture's failure dump (compose ps + all container
  logs + last evidence) made both of this session's failures one-line
  diagnoses. Keep using it for any new scenario test.
- Coverage gate = FUNCTION coverage vs human-written coverage_floor.txt
  (tolerance 0.50). This session's PR changed no library code, so the
  local hook's coverage step was skipped (SKIP_COVERAGE_CHECK=1) and the
  PR's own coverage job stayed the authority — it passed.
- Repo merges are REBASE-only (merge commits and squash disabled);
  `gh pr merge --auto --rebase`.
- Local `main` goes stale while working on branches — `git fetch` +
  ff-only before reading the tree as truth.
- Local AWS auth: the `default`/`clark` profiles hold a dead static key
  (InvalidClientTokenId); the `personal` profile reaches account
  827617851594 (the kythira account — as root, so treat with care).
  Use `AWS_PROFILE=personal` for provisioning scripts, per the user's
  explicit instruction this session.
- workflow_dispatch materializes EVERY unprovided boolean input as
  'false' — the inputs-null → repo-variable fallback in
  real-cloud-tests.yml's `if:` expressions only applies to scheduled
  runs. A manual dispatch must pass run_real_cloud_tests=true or every
  job skips silently while the run reports itself completed (cost one
  dispatch to learn; recorded in CHANGELOG).
- GitHub had an API wobble ~14:40 UTC (500s on dispatch, 502s on
  GraphQL, SSH auth flaps). A 502'd `gh pr create` can still create the
  PR server-side — check `pulls?head=` before retrying.

## Priorities for next session

Nothing carried over as in-flight. Open surface in doc/TODO.md:
1. **Cloud Provider Support**: Alibaba Cloud (quorum manager + cert
   provider + OSS persistence engine), and the cloud key-object
   persistence-engine spec (S3/Azure Blob/GCS/OCI Object Storage; confront
   the Raft synchronous-flush requirement head-on).
2. Optionally: light up the remaining real-cloud monitoring legs — Azure
   (needs an App Insights resource) and GCP (three SA roles) are the
   cheap ones; AWS is already verified live (ledger above).
3. Minor: memory usage profiling; OSCORE leftovers; proxygen ingress
   timeout still "reduced, not root-caused".

## How to not lose the next four hours

This session's loop was again CI-only-reproducible failures, and again the
counter-measures compounded: (1) the failure-dump fixture turned both
defects into one-line diagnoses — LocalStack's own log named its license
requirement, and the evidence dump showed the EMF documents present under
the unsuffixed metric name; (2) when copying an assertion from a sibling
test, check what the *wire* actually carries on the new path, not what the
other backend renders; (3) the working-branch rebuild rule (soft-reset to
origin/main, recommit with saved messages) kept the PR at four clean
commits across three CI iterations — save the messages to files BEFORE the
reset.
