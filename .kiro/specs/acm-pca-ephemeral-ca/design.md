# ACM Private CA Ephemeral Test CA — Design

## Overview

```
aws_acm_pca_provider_real_test (KYTHIRA_ACM_PCA_REAL_TESTS=1)
  PreflightSkipFixture       exit 77 unless enabled; before Aws::InitAPI
  AwsSdkFixture              InitAPI / ShutdownAPI
  CostSummaryFixture         prints [aws-cost] at exit
  RealCaFixture : signal_cleanup_target
    ├── revocable  ephemeral_acm_pca(OCSP)   or supplied via $KYTHIRA_TEST_ACM_PCA_ARN
    └── bare       ephemeral_acm_pca(none)   or supplied
  test cases ──► aws_acm_pca_provider (unchanged) against revocable / bare
  teardown   ──► bare then revocable: DISABLE → DELETE(7 days)

aws_acm_pca_provider_localstack_test
  LocalStackCaFixture ──► the same ephemeral_acm_pca, no signal wiring
```

## `ephemeral_acm_pca` (tests/aws_acm_pca_test_support.hpp)

```cpp
enum class ca_revocation { ocsp, none };

class ephemeral_acm_pca {
public:
    struct options {
        Aws::Client::ClientConfiguration client;
        ca_revocation revocation{ca_revocation::ocsp};
        std::string common_name;
        std::vector<std::pair<std::string, std::string>> tags;
        std::chrono::seconds step_timeout{120};
        // Called with the ARN the moment CreateCertificateAuthority
        // returns, before bootstrap continues (Requirement 2.5).
        std::function<void(const std::string&)> on_created;
    };

    explicit ephemeral_acm_pca(options);      // throws ephemeral_ca_unavailable
    ~ephemeral_acm_pca();                      // calls teardown()

    [[nodiscard]] auto arn() const -> const std::string&;
    [[nodiscard]] auto created_at() const -> std::chrono::system_clock::time_point;
    [[nodiscard]] auto deleted_at() const -> std::optional<std::chrono::system_clock::time_point>;
    void teardown() noexcept;                  // idempotent; prints, never throws
};
```

The constructor throws `ephemeral_ca_unavailable` with the failing call and
AWS message. The real fixture turns that into teardown plus exit 77, and the
LocalStack fixture into exit 77, which is what it already does today. This
code is lifted from `LocalStackCaFixture::create_active_root_ca`,
`poll_until_ready` and `teardown` with three changes: the tags, the
`on_created` hook, and a status-aware teardown.

**Status-aware teardown.** `DescribeCertificateAuthority` first:

| Status | Action |
|---|---|
| `ACTIVE` | `UpdateCertificateAuthority(DISABLED)`, then `DeleteCertificateAuthority(7)` |
| `DISABLED`, `EXPIRED`, `FAILED` | `DeleteCertificateAuthority(7)` |
| `CREATING`, `PENDING_CERTIFICATE` | `DeleteCertificateAuthority` (AWS deletes immediately, ignoring the window) |
| `DELETED` | nothing |
| describe fails | still attempt DISABLE and DELETE; collect errors |

`std::atomic_flag` guards against a second run, either the destructor after
a signal-path teardown or the reverse.

## `RealCaFixture` (tests/aws_acm_pca_provider_real_test.cpp)

It owns up to two `ephemeral_acm_pca` instances as `std::optional`, plus an
optional supplied ARN.

1. Enabled check (Requirement 6.1) in `PreflightSkipFixture`.
2. If `$KYTHIRA_TEST_ACM_PCA_ARN` is set, describe it and classify it as
   revocable or bare. The other kind is created.
3. `install_aws_signal_handlers()`, then create the CA(s), passing
   `on_created = [this](auto&) { g_active_aws_fixture.store(this); }`.
   Registering before bootstrap completes means a crash during the slow
   steps still tears down.
4. `teardown()` (from `signal_cleanup_target`) calls each owned CA's
   `teardown()`, bare first, then adds the cost report.

The fixture is a function-local static behind a `BOOST_GLOBAL_FIXTURE`, the
same arrangement as `cas_pool()` in the GCP sibling. Boost destroys it after
the last case.

### Test cases (replacing the current three)

| Case | CA | Asserts |
|---|---|---|
| `root_certificate_pem_is_cached` | revocable | PEM, cache hit |
| `sign_csr_issues_certificate_chaining_to_root` | revocable | chain verifies against the root |
| `revocation_configured_distinguishes_cas` | both | `true` / `false` |
| `revoke_on_revocable_ca_succeeds` | revocable | no throw, `KEY_COMPROMISE` |
| `revoke_on_bare_ca_surfaces_aws_error` | bare | `std::runtime_error` carrying the AWS message (Req 10.7) |

Leaf issuance count is 2: one on each CA, because the bare-CA case needs its
own certificate to revoke. With the two self-issued roots, that is 4
certificates per run.

## Ordering and quotas

ACM Private CA counts CAs in `DELETED` status against the per-region CA
quota (200 by default) until the restore window ends. At 2 CAs per run and
a 7-day window, the quota allows about 14 runs a day sustained. The weekly
schedule, plus occasional manual runs, uses well under 1% of it. The leak
script's `audit` ignores `DELETED` CAs so they never read as leaks.

Billing: a CA in `DELETED` status is understood not to accrue the hourly
charge during the restore window. This is inferred from AWS's pricing page,
not measured. Task 7 verifies it in the billing console after the first run
and records the result in the cost doc. It also confirms whether the root's
self-issued `IssueCertificate` is billed as a certificate. That is the one
unknown in the 2026-10-03 manual run's roughly $0.18.

## IAM bundle (`acm-pca.json`)

Three statements. The condition-key support for each action must be checked
against the service authorization reference (Requirement 5.2), since ACM
Private CA does not support `aws:ResourceTag` uniformly.

```json
[
  {"Sid": "AcmPcaCreateTagged",
   "Action": ["acm-pca:CreateCertificateAuthority", "acm-pca:TagCertificateAuthority"],
   "Resource": "*",
   "Condition": {"StringEquals": {"aws:RequestTag/kythira:suite": "acm-pca-real-test"}}},
  {"Sid": "AcmPcaOperateOwnedOnly",
   "Action": ["acm-pca:GetCertificateAuthorityCsr", "acm-pca:IssueCertificate",
              "acm-pca:GetCertificate", "acm-pca:ImportCertificateAuthorityCertificate",
              "acm-pca:GetCertificateAuthorityCertificate",
              "acm-pca:DescribeCertificateAuthority", "acm-pca:RevokeCertificate",
              "acm-pca:UpdateCertificateAuthority", "acm-pca:DeleteCertificateAuthority"],
   "Resource": "arn:aws:acm-pca:*:*:certificate-authority/*",
   "Condition": {"StringEquals": {"aws:ResourceTag/kythira:suite": "acm-pca-real-test"}}},
  {"Sid": "AcmPcaList",
   "Action": ["acm-pca:ListCertificateAuthorities", "acm-pca:ListTags"],
   "Resource": "*"}
]
```

`IssueCertificate` additionally scopes by template ARN through
`acm-pca:TemplateArn`. Restricting it to `EndEntityCertificate/V1` and
`RootCACertificate/V1` stops the role minting a subordinate CA certificate.

## Leak script

`scripts/aws-acm-pca-leaks.sh audit|sweep` follows `aws-asg-leaks.sh`'s
structure: tag-based discovery, an audit grace period, and a sweep that runs
only after the audit so detection stays loud. ACM Private CA has no
server-side tag filter on `ListCertificateAuthorities`, so the script lists
all CAs, calls `list-tags` on each non-DELETED one, and keeps those with
`kythira:suite=acm-pca-real-test`.

## Workflow

A new step in the existing AWS job runs when `BUNDLE_ACM_PCA` is true:

```yaml
BUNDLE_ACM_PCA: ${{ vars.REAL_CLOUD_TESTS_AWS_ACM_PCA_ENABLED == 'true' }}
```

There is deliberately no dispatch input (25-input cap). A manual run sets
the variable, or runs locally with `KYTHIRA_ACM_PCA_REAL_TESTS=1`. The step
runs ctest for the one test, then the audit with `if: always()`, then the
sweep on audit failure. The job's zero-bundle guard and its bundle list in
the error message gain `REAL_CLOUD_TESTS_AWS_ACM_PCA_ENABLED`.

## Alternatives considered

- **A standing CA with `$KYTHIRA_TEST_ACM_PCA_ARN` only (today).** Costs
  about $50/month in short-lived mode whether or not the suite runs, and
  an operator has to set it up per account. It is kept as an option
  (Requirement 1.4) for anyone who already has one.
- **One CA, toggling OCSP between cases with `UpdateCertificateAuthority`.**
  Halves CA-hours, but the revocation configuration change is eventually
  consistent. That would make the bare-CA case racy, and it saves about
  $0.01 a run.
- **General-purpose mode.** About 8x the hourly rate, and it allows longer
  leaf validity the suite does not need.
