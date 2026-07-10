# Cost estimate: real AWS Certificate Manager (Private CA) test session

This estimates the AWS bill for one run of the "real AWS" test suites that
exercise `aws_acm_pca_provider` (`include/raft/aws_acm_pca_provider.hpp`)
against an actual ACM Private CA and an actual EC2 test cluster, as opposed
to the LocalStack (`*_localstack_test.cpp`) or mocked/fault-injection
(`tests/aws_acm_pca_provider_unit_test.cpp`) variants, which cost nothing.

These tests are gated behind `KYTHIRA_AWS_REAL_EC2_TESTS` /
`KYTHIRA_AWS_LOCALSTACK_TESTS`, labeled `slow`/`real-ec2`, and excluded from
CI (`ctest -LE '^(slow|performance|verbose|benchmark|docker)$'` in
`.github/workflows/ci.yml`). They only run when a developer opts in locally
with real AWS credentials, so there is no fixed "sessions per day/month" —
figures below are quoted per session, with an optional amortized view.

## Why the Private CA itself isn't a per-session cost

Per `.kiro/specs/certificate-authority/requirements.md` (Requirement 10.2),
`aws_acm_pca_provider_config` takes a pre-existing `certificate_authority_arn`
and the provider "SHALL NOT create or delete a Private CA — provisioning one
is an out-of-band operator action, given its ongoing per-CA cost." So a test
session's marginal AWS spend is issuance/API calls against an already-running
CA, not the CA's own monthly operation fee. That fee is a standing cost of
the shared fixture, optionally amortized below.

## AWS Private CA pricing (current, us-east-1)

| Mode | CA operation | Per certificate |
|---|---|---|
| General-purpose | $400/month (prorated hourly, ≈ $0.548/hr) | $0.75 (first 1,000/mo), $0.35 (next 9,000), $0.10 above |
| Short-lived certificate (≤7-day validity) | $50/month (≈ $0.069/hr) | $0.058 flat |

Test certificates are short-lived by nature, so short-lived mode is the
relevant comparison; general-purpose is included to show the cost of
defaulting to it by mistake.

## EC2 test-cluster cost (already instrumented in-repo)

`tests/aws_quorum_manager_real_ec2_test.cpp` already tracks its own AWS
spend via a `TestCostReport`/`CostAccumulator` (on-demand rate table +
$0.045/hr NAT Gateway + $0.005/hr EIP, printed as `[aws-cost]` at teardown).
`BOOST_FIXTURE_TEST_SUITE(real_ec2, RealEc2Fixture)` re-provisions a fresh
VPC/subnets/NAT gateway/EIP/bastion/cluster for *each* of the 10 test cases
(6 at a 600s timeout, 3 at 1200s, plus the 9-node `nine_node_full_cluster_healthy`
case at 1200s), and `ca_cluster_node_real_ec2_test.cpp` adds one more
(3-node CA cluster, 900s timeout). Using the repo's own on-demand rates as a
ceiling, assuming actual runtimes average 40-60% of each case's timeout
budget:

| Item | Assumption | Cost (on-demand ceiling) |
|---|---|---|
| 6 cases, 3-node cluster + bastion, ~10 min each | $0.0104/hr instances | ≈ $0.14 |
| 3 cases, 3-node cluster + bastion, ~20 min each | $0.0104/hr instances | ≈ $0.09 |
| 1 nine-node case, ~25 min | $0.0104/hr instances | ≈ $0.06 |
| `ca_cluster_node_real_ec2_test`, 3-node, ~12 min | $0.0104/hr instances | ≈ $0.02 |
| **EC2/NAT/EIP subtotal** | | **≈ $0.30** (on-demand) |

The suite selects the cheapest available spot instance type via
`DescribeSpotPriceHistory` (`ec2_hourly_rate()` in the test file is only the
on-demand ceiling used for `spot_price_cluster`/`spot_price_bastion` when no
type is pinned), so actual spend is typically 40-70% lower —
**≈ $0.10-0.20 per full session** in practice. NAT Gateway and EIP hourly
charges are not spot-discountable and dominate this subtotal.

## ACM Private CA marginal cost per session

Each of the ~10-11 real-EC2 test cases bootstraps a cluster (and several
fault-injection cases provision replacement nodes), each node needing a
certificate; call it 15-40 `IssueCertificate` calls per full session:

| Mode | 15 certs | 40 certs |
|---|---|---|
| Short-lived ($0.058/cert) | $0.87 | $2.32 |
| General-purpose ($0.75/cert, within first-1,000 tier) | $11.25 | $30.00 |

`GetCertificate`/`GetCertificateAuthorityCertificate` polling
(Requirement 10.3-10.4) and other read/describe API calls are not separately
billed.

## Combined estimate per test session

| Scenario | Marginal cost (CA already running) | Fully loaded (CA's own fee amortized over ~10 sessions/month) |
|---|---|---|
| Short-lived CA mode, spot EC2 (recommended) | **≈ $1.00 - $2.50** | **≈ $6 - $7.50** |
| General-purpose CA mode | **≈ $11 - $30** | **≈ $51 - $70** |

The "fully loaded" column assumes a shared CA used for ~10 real-AWS test
sessions/month (occasional opt-in developer runs); it drops close to the
marginal figure if the CA also serves other long-lived purposes, and rises
if sessions are rarer. Short-lived CA mode is roughly 8-10x cheaper per
session than general-purpose across every scenario, both because of the
smaller monthly fee and the ~13x lower per-certificate price, so it is the
right mode for `aws_acm_pca_provider_config.template_arn`/CA selection when
the CA backing these tests is dedicated to testing.

## Monthly cost, by usage frequency

Since these tests are opt-in (gated behind `KYTHIRA_AWS_REAL_EC2_TESTS`,
excluded from CI) rather than run on a fixed schedule, there's no single
"actual" monthly figure — it depends on how often developers actually run
them. The table below takes the per-session marginal range from the
combined estimate above ($1.00-$2.50/session short-lived, $11-$30/session
general-purpose) times three illustrative frequencies, plus the standing
monthly CA operation fee ($50 short-lived, $400 general-purpose):

| Frequency | Sessions/month | Marginal cost, short-lived | Marginal cost, general-purpose | **Total/month, short-lived** | **Total/month, general-purpose** |
|---|---|---|---|---|---|
| Occasional (~couple times/week) | 10 | $10 - $25 | $110 - $300 | **≈ $60 - $75** | **≈ $510 - $700** |
| Regular (~once/workday) | 20 | $20 - $50 | $220 - $600 | **≈ $70 - $100** | **≈ $620 - $1,000** |
| Heavy (daily, incl. weekends) | 30 | $30 - $75 | $330 - $900 | **≈ $80 - $125** | **≈ $730 - $1,300** |

Two things stand out:

- **In short-lived mode, the $50/month base fee dominates and usage barely
  moves the total** (≈$60-75 → ≈$80-125 across a 3x increase in sessions),
  because per-session marginal cost is small (~$1-2.50).
- **In general-purpose mode, per-certificate issuance dominates and scales
  linearly with usage** — the $400 base fee is a smaller share of the bill
  than the $0.75/certificate charge once test volume is nontrivial, so
  running the suite more often costs far more in general-purpose mode than
  in short-lived mode (an extra 10 sessions/month adds ~$10-25 in
  short-lived mode vs. ~$110-300 in general-purpose mode). Short-lived
  mode is 6-10x cheaper per month at every frequency shown.
- If the same Private CA also backs other long-running uses (e.g. a
  staging `ca_service --provider aws-acm-pca` deployment per
  `docker/ca_service/`), its $50/$400 monthly fee is already being paid
  regardless of test usage, and only the marginal per-session cert-issuance
  cost (≈$0.87-2.32/session, short-lived mode) should be attributed to
  testing.
