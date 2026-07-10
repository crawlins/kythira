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
figures below are quoted per session, with monthly totals derived from
usage frequency further down.

## Assumption: the CA is created and deleted with the test session

The rest of this document assumed a pre-provisioned, long-lived CA (per
`.kiro/specs/certificate-authority/requirements.md` Requirement 10.2,
`aws_acm_pca_provider` "SHALL NOT create or delete a Private CA —
provisioning one is an out-of-band operator action"). **This section
instead models the alternative where a test session creates its own CA at
the start and deletes it at the end** — i.e. the CA becomes an ephemeral
fixture with the same lifetime as an EC2 test-cluster resource, rather than
a shared standing one. This does not match the current
`aws_acm_pca_provider` implementation (which only ever targets an existing
`certificate_authority_arn`); it would require the test harness itself to
call `CreateCertificateAuthority` / `DeleteCertificateAuthority` directly,
similar to how `RealEc2Fixture` provisions and tears down its VPC per test
case.

Per AWS's published pricing, CA operation charges have **no monthly
minimum** — they are billed purely by the hour the CA exists (prorated from
the monthly rate, i.e. monthly-rate ÷ ~730 hours/month), so a CA that lives
only for the duration of a test session is billed only for those hours, not
a full month. (There is also a one-time 30-day free trial on CA operation
charges for the first private CA created per account per region, not
modeled here since it only applies once.)

## AWS Private CA pricing (current, us-east-1)

| Mode | CA operation | Hourly equivalent | Per certificate |
|---|---|---|---|
| General-purpose | $400/month, prorated hourly, no minimum | ≈ $0.548/hr | $0.75 (first 1,000/mo), $0.35 (next 9,000), $0.10 above |
| Short-lived certificate (≤7-day validity) | $50/month, prorated hourly, no minimum | ≈ $0.069/hr | $0.058 flat |

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

## ACM Private CA cost per session

**Certificate issuance.** Each of the ~10-11 real-EC2 test cases bootstraps
a cluster (and several fault-injection cases provision replacement nodes),
each node needing a certificate; call it 15-40 `IssueCertificate` calls per
full session:

| Mode | 15 certs | 40 certs |
|---|---|---|
| Short-lived ($0.058/cert) | $0.87 | $2.32 |
| General-purpose ($0.75/cert, within first-1,000 tier) | $11.25 | $30.00 |

`GetCertificate`/`GetCertificateAuthorityCertificate` polling
(Requirement 10.3-10.4) and other read/describe API calls are not separately
billed.

**CA operation, prorated to the session's lifetime.** Since the CA is now
created at the start of the session and deleted at the end (see
"Assumption" above), its $400 or $50 monthly fee is billed only for the
hours it actually existed, with no minimum. Using the EC2 section's own
session-duration estimate — 6 cases at ~10 min + 3 cases at ~20 min + 1
nine-node case at ~25 min + the `ca_cluster_node` case at ~12 min ≈ 157 min
≈ 2.6 hours — the CA-creation charge for that one session is:

| Mode | Hourly rate | ≈ 2.6 hr session |
|---|---|---|
| Short-lived | $0.069/hr | ≈ $0.18 |
| General-purpose | $0.548/hr | ≈ $1.44 |

This is now a genuine per-session marginal cost (rather than a shared
standing fee), because each session pays for its own CA's lifetime instead
of sharing one that's already running.

## Combined estimate per test session

Summing EC2/NAT/EIP (≈ $0.10-0.30), the prorated CA-operation charge for
the session's own CA (≈ $0.18 / ≈ $1.44), and certificate issuance
(15-40 certs):

| Scenario | Per-session total |
|---|---|
| Short-lived CA mode, spot EC2 (recommended) | **≈ $1.15 - $2.80** |
| General-purpose CA mode | **≈ $12.80 - $31.75** |

There's no separate "fully loaded" figure to amortize anymore — because
the CA's own fee is now paid fresh by each session rather than shared
across sessions, the per-session total above already includes it in full.
Short-lived CA mode is still roughly 10-11x cheaper per session than
general-purpose, now driven almost entirely by the ~13x lower
per-certificate price (the CA-operation charge itself is a small fraction
of either total at this session length).

## Monthly cost, by usage frequency

Since these tests are opt-in (gated behind `KYTHIRA_AWS_REAL_EC2_TESTS`,
excluded from CI) rather than run on a fixed schedule, there's no single
"actual" monthly figure — it depends on how often developers actually run
them. Because the CA no longer persists between sessions, there is also no
fixed monthly base fee anymore: monthly cost is purely the per-session
total (≈ $1.15-$2.80 short-lived, ≈ $12.80-$31.75 general-purpose) times
the number of sessions run that month:

| Frequency | Sessions/month | **Total/month, short-lived** | **Total/month, general-purpose** |
|---|---|---|---|
| Occasional (~couple times/week) | 10 | **≈ $11.50 - $28.00** | **≈ $128 - $317.50** |
| Regular (~once/workday) | 20 | **≈ $23.00 - $56.00** | **≈ $256 - $635** |
| Heavy (daily, incl. weekends) | 30 | **≈ $34.50 - $84.00** | **≈ $384 - $952.50** |

Two things stand out, both a direct consequence of the CA no longer being a
shared fixture:

- **Both modes now scale linearly with usage** — there is no fixed $50 or
  $400 floor to dilute, so doubling the number of sessions run in a month
  roughly doubles the bill in either mode. This is the opposite of the
  pre-provisioned-CA scenario earlier in this document, where the base fee
  dominated and usage barely moved the short-lived-mode total.
- **Short-lived mode is cheaper at every frequency by roughly the same
  ~10-11x factor** seen in the per-session estimate, since (with the fixed
  base fee gone) the ratio between the two modes' monthly totals is now set
  almost entirely by the per-certificate price difference rather than by
  the base-fee difference.
- Creating and deleting a real Private CA per test session also carries an
  operational cost this document doesn't price: `CreateCertificateAuthority`
  is not instantaneous (the CA must reach the `PENDING_CERTIFICATE` state,
  and then be activated by installing a CA certificate before it can issue
  anything), so a from-scratch CA adds setup latency to every session on
  top of the dollar cost above — a further reason Requirement 10.2 treats
  provisioning the CA as a one-time, out-of-band operator action rather
  than something the test harness does itself.

## How this changes on other cloud providers

`doc/TODO.md`'s "Cloud Provider Support" section lists GCP (Certificate
Authority Service + Managed Instance Group) and Azure (Key Vault
Certificates + Virtual Machine Scale Set) as planned, not-yet-implemented
`certificate_provider`/quorum-manager backends. Neither has code in this
repo today, so the figures below are a pricing-only comparison, using the
same session shape as above (2.6 hours, 3-9 node cluster + bastion, 15-40
certificates issued) — not a measurement of anything Kythira currently
runs.

### Compute + NAT + IP (the EC2-equivalent side)

| Provider | Small on-demand instance | NAT egress | Static/external IP |
|---|---|---|---|
| AWS (this doc, above) | t3.micro $0.0104/hr | NAT Gateway, flat $0.045/hr | EIP $0.005/hr |
| GCP | e2-micro ≈ $0.0084/hr | Cloud NAT ≈ $0.0014/hr *per VM using it* (capped at $0.044/hr at 32+ VMs) | external IP ≈ $0.004/hr |
| Azure | B1s ≈ $0.0104/hr | NAT Gateway, flat $0.045/hr | Standard Public IP ≈ $0.005/hr |

Azure's rates are close enough to AWS's (same instance price, same flat
NAT-gateway price, same IP price) that its EC2-equivalent subtotal is
essentially the same **≈ $0.10-0.30 per session**. GCP is somewhat cheaper
at this cluster size because Cloud NAT bills per attached VM instead of a
flat gateway rate — for a 4-10 instance cluster that's roughly
$0.006-$0.014/hr versus a flat $0.045/hr — putting GCP's subtotal at
roughly **≈ $0.07-$0.21 per session**. All three are small next to the
certificate-issuance cost below, so this isn't where provider choice
matters most.

### Private CA equivalents

| Provider | Product | Monthly base fee | Hourly equivalent | Per-certificate (low-volume tier) |
|---|---|---|---|---|
| AWS | ACM Private CA, short-lived mode | $50 | ≈ $0.069/hr | $0.058 |
| AWS | ACM Private CA, general-purpose mode | $400 | ≈ $0.548/hr | $0.75 (first 1,000/mo) |
| GCP | Certificate Authority Service, DevOps tier | $20 | ≈ $0.027/hr | $0.30 (first 50,000/mo) |
| GCP | Certificate Authority Service, Enterprise tier | $200 | ≈ $0.274/hr | $0.50 (first 50,000/mo) |
| Azure | *(no first-party equivalent — see below)* | — | — | — |

Both AWS and GCP publish per-hour-prorated pricing for their private-CA
services, so "create the CA at session start, delete it at session end"
works the same way on both. **Azure has no directly comparable first-party
managed private-CA product** — Key Vault stores and rotates certificates
but isn't itself a CA; issuing from a private hierarchy on Azure means
either standing up your own CA (e.g. Active Directory Certificate
Services) on a VM, or using a third-party marketplace SaaS such as
Keytos EZCA ($200/month per CA, flat, with certificate issuance included
at no additional per-certificate charge). Neither is quite the same shape
as AWS/GCP's metered PaaS offerings, so the Azure numbers below carry more
uncertainty than the AWS/GCP ones.

### Per-session and monthly cost, by provider (15-40 certificates/session)

| Provider / mode | Per session | 10 sessions/mo | 20 sessions/mo | 30 sessions/mo |
|---|---|---|---|---|
| AWS short-lived (recommended baseline) | ≈ $1.15 - $2.80 | ≈ $11.50 - $28 | ≈ $23 - $56 | ≈ $34.50 - $84 |
| GCP DevOps tier | ≈ $4.67 - $12.32 | ≈ $47 - $123 | ≈ $93 - $246 | ≈ $140 - $370 |
| GCP Enterprise tier | ≈ $8.31 - $20.96 | ≈ $83 - $210 | ≈ $166 - $419 | ≈ $249 - $629 |
| AWS general-purpose | ≈ $12.80 - $31.75 | ≈ $128 - $317.50 | ≈ $256 - $635 | ≈ $384 - $952.50 |

GCP's DevOps tier lands *between* AWS's two modes rather than undercutting
AWS short-lived mode, and that's driven entirely by the per-certificate
price, not the base fee: DevOps tier's $20/month base fee is 60% lower than
AWS short-lived's $50, but its $0.30/certificate rate is over 5x AWS
short-lived's $0.058 — and at only 15-40 certificates/session, the
per-certificate charge matters more than the base fee either way. AWS
short-lived mode remains the cheapest option of the three at this
certificate volume; GCP Enterprise tier is a close second to AWS
general-purpose mode ($8-21 vs. $12.80-31.75/session), being slightly
cheaper on both its base fee ($200 vs $400) and per-cert rate ($0.50 vs
$0.75).

**Azure, self-hosted (AD CS on a VM):** no CA-specific fee at all — cost is
just the hosting VM (a small Windows VM, since AD CS requires Windows
Server; roughly $0.02-$0.06/hr including the Windows Server license
component, on top of the EC2-equivalent side above) and zero per-certificate
charge. That undercuts every managed option above in raw dollar terms
(order of $0.10-$0.30/session), but it's not a managed service — the test
harness would own CA setup, patching, and availability itself, which none
of the other rows require and which this document doesn't price.

**Azure, marketplace SaaS (EZCA):** $200/month flat per CA with certificate
issuance included, which — *if* prorated to the hour the way AWS/GCP's own
services are (unconfirmed for this third-party product; it may instead be
a fixed monthly/annual commitment regardless of how long the CA exists) —
would land around $0.71 for a 2.6-hour session plus $0 in per-certificate
charges, i.e. comparable to or cheaper than GCP's Enterprise tier at this
certificate volume. If it's actually billed as a standing monthly
subscription rather than prorated hourly, creating and deleting the CA per
session provides no savings, and the effective monthly cost would just be
$200 regardless of how many sessions ran that month. Confirm the vendor's
actual billing granularity before relying on either number.

### OCI and Alibaba Cloud

`doc/TODO.md` also lists OCI (Instance Pool + OCI Certificates Service) and
Alibaba Cloud (Auto Scaling Group + Alibaba Cloud SSL Certificates Service)
as further planned, not-yet-implemented backends.

**OCI is structurally the cheapest option researched for this session
shape**, but not because of aggressive per-unit pricing — because its
Certificates service and a slice of its compute/network are given away in
the Always Free tier rather than metered per-hour like AWS/GCP/Azure:

- **OCI Certificates Service** (which includes private CAs) is included in
  every tenancy's Always Free allowance, up to **5 private CAs and 150
  private certificates**, with no additional charge within that. A single
  test session (1 CA, 15-40 certificates) fits entirely inside those limits,
  so the CA-side cost is **$0**, not merely low. (If certificate volume or
  CA count grew past the free allowance, OCI would presumably bill through
  the same Vault/KMS consumption model referenced below, but nothing in the
  documentation surfaced usage-based pricing beyond that free allowance.)
- **NAT Gateway has no hourly fee at all** on OCI — only data egress beyond
  the account's 10 TB/month free allowance is billed, unlike AWS's/Azure's
  flat $0.045/hr or GCP's per-VM Cloud NAT charge.
- **Compute**: 2 `VM.Standard.E2.1.Micro` instances are Always Free per
  tenancy; a 3-9 node cluster plus bastion would need to pay standard
  on-demand rates for the instances beyond those two (comparable in order
  of magnitude to AWS t3.micro/GCP e2-micro, though this document didn't
  pin down an exact figure).
- **Vault/KMS**: if the private CA's key is software-protected (the
  default), there's no separate Vault charge; HSM-protected keys add a
  per-key-version monthly fee this document didn't quantify. A test CA has
  no reason to require HSM protection, so this likely doesn't apply.

Net effect: for exactly this session shape (one ephemeral CA, a handful of
nodes, 15-40 certs), OCI's bill would plausibly be **at or near $0** for
everything except the compute beyond the 2 free instances — categorically
different from AWS/GCP/Azure's per-hour-metered model, rather than just a
lower rate on the same model.

**Alibaba Cloud's Private Certificate Authority (PCA)** is a real,
documented product — a private CA purchased as a subscription instance
(RSA, SM, or ECC algorithm; Alibaba unified pricing across algorithms in
an October 2024 price cut) — but this document could **not** obtain actual
dollar/yuan figures for it: Alibaba's pricing pages render the price table
dynamically rather than as indexed text, and repeated searches turned up
only the billing model description, not numbers. Two things are known with
reasonable confidence: it's sold as an annual (not hourly-prorated)
subscription per CA instance, which — like the Azure EZCA caveat above —
would mean creating and deleting a CA per test session captures none of the
proration benefit AWS/GCP/OCI offer; and it's priced separately from
Alibaba's SSL Certificates Service (which sells commercial DV/OV
certificates, not a private CA). Anyone who needs a real number should
check the "Purchase and enable a private CA" console page directly rather
than rely on this document.
