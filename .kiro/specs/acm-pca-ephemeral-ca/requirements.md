# ACM Private CA Ephemeral Test CA — Requirements Document

## Introduction

`tests/aws_acm_pca_provider_real_test.cpp` (certificate-authority task 11)
needs an ACTIVE ACM Private CA, and today an operator has to provide one
through `$KYTHIRA_TEST_ACM_PCA_ARN`. Without that variable the suite exits
77 and runs nothing. Its first real run on 2026-10-03 showed the cost of
that arrangement: the account had no CA, so one was created and deleted by
hand to run three test cases. A standing CA would avoid that, but it bills
around the clock (short-lived mode is about $50/month, general-purpose
about $400/month) to serve a suite that runs for minutes.

This spec moves CA creation and deletion into the test suite: the CA
becomes a per-run fixture with the same lifetime as an EC2 test cluster.
`doc/aws_acm_pca_test_cost_estimate.md` ("Assumption: the CA is created and
deleted with the test session") already costed this model. The pattern
already exists in this repo: `tests/gcp_privateca_provider_real_test.cpp`
creates and tears down a CAS pool and root CA per run (gcp-cloud-services
Requirement 23 ACs 15-17), and `tests/aws_acm_pca_provider_localstack_test.cpp`
bootstraps root CAs against LocalStack.

certificate-authority Requirement 10.2 ("this component SHALL NOT create or
delete a Private CA") still holds. It constrains `aws_acm_pca_provider`, the
production component. The test fixture creates the CA through
`Aws::ACMPCA::ACMPCAClient` directly, and `aws_acm_pca_provider` does not
change.

## Glossary

- **Owned CA**: a CA this suite created in the current run, which it must
  delete.
- **Supplied CA**: a CA named by `$KYTHIRA_TEST_ACM_PCA_ARN`. The suite uses
  it as it is and never modifies or deletes it.
- **Bootstrap**: the steps that make a new root CA ACTIVE:
  `CreateCertificateAuthority`, `GetCertificateAuthorityCsr`, then
  `IssueCertificate` with the `RootCACertificate/V1` template, then
  `GetCertificate`, then `ImportCertificateAuthorityCertificate`.
- **Run id**: a per-process identifier (`run-<pid>-<epoch seconds>`) that
  tags every resource the run creates.

## Requirements

### Requirement 1: Owned CAs by default

**User Story:** As a developer, I want the real ACM Private CA suite to
create the CAs it needs, so I can run it in any account without first
provisioning a standing CA.

#### Acceptance Criteria

1. WHEN `$KYTHIRA_TEST_ACM_PCA_ARN` is unset and the suite is enabled
   (Requirement 6), the fixture SHALL create and bootstrap two owned root
   CAs in the region given by `$AWS_REGION` (default `us-west-2`):
   - a **revocable** CA with `OcspConfiguration.Enabled = true` and no CRL
     (a CRL needs an S3 bucket and bucket policy; OCSP needs neither);
   - a **bare** CA with no revocation configuration.
2. Both CAs SHALL use `UsageMode = SHORT_LIVED_CERTIFICATE`, `KeyAlgorithm =
   RSA_2048`, `SigningAlgorithm = SHA256WITHRSA`, and a root certificate
   validity of 1 year. Short-lived mode is about one-eighth of the
   general-purpose hourly rate, and the suite's leaf certificates already
   request 1 day (under the 7-day cap).
3. Bootstrap SHALL poll each step that ACM Private CA answers with
   `RequestInProgressException`, with backoff bounded by 120 s per step, and
   SHALL wait for `Status = ACTIVE` before any test case runs.
4. WHEN `$KYTHIRA_TEST_ACM_PCA_ARN` is set, the fixture SHALL use that CA as
   the revocable or bare CA according to its `DescribeCertificateAuthority`
   revocation configuration. It SHALL create only the other CA, and SHALL
   never update, disable or delete the supplied one.
5. With both CAs present, the suite SHALL assert both Requirement 10.7 paths
   from certificate-authority: revoke succeeds on the revocable CA, and on
   the bare CA it rejects with the AWS error. `revocation_configured()`
   SHALL return `true` for the revocable CA and `false` for the bare one.
   This replaces today's single case that branches on whichever kind of CA
   it was given.

### Requirement 2: Teardown

**User Story:** As the account owner, I want every CA the suite creates to
be deleted by the end of the run, whatever happens, so a test cannot leave
a CA billing indefinitely.

#### Acceptance Criteria

1. Teardown SHALL, for each owned CA: call `UpdateCertificateAuthority` with
   `Status = DISABLED` when the CA is ACTIVE, then
   `DeleteCertificateAuthority` with `PermanentDeletionTimeInDays = 7` (the
   minimum). A CA still in `CREATING` or `PENDING_CERTIFICATE` SHALL be
   deleted directly, since AWS deletes those immediately.
2. Teardown SHALL run every step even when an earlier step fails. It SHALL
   collect errors, print them to stderr with each CA's ARN and a line
   telling the operator to delete it manually, and SHALL never throw. A
   teardown failure SHALL NOT fail the test run, matching
   `CasPoolFixture::teardown()`.
3. Teardown SHALL run at most once per process. It SHALL be idempotent
   against a CA already `DELETED`.
4. The fixture SHALL derive from `signal_cleanup_target` and register in
   `g_active_aws_fixture` from `tests/aws_real_ec2_test_support.hpp`, with
   `install_aws_signal_handlers()` installed. SIGINT, SIGTERM, SIGHUP,
   SIGQUIT, SIGABRT, SIGSEGV and SIGBUS during the run (a ctest TIMEOUT
   included) SHALL then still delete owned CAs.
5. Registration SHALL happen as soon as the first `CreateCertificateAuthority`
   returns an ARN, before bootstrap continues. A CA that fails mid-bootstrap
   is still owned and still deleted.
6. A provisioning failure (an access-denied error, a quota error, a
   bootstrap step that times out) SHALL tear down whatever was created and
   then exit 77, so ctest reports "Not Run". A missing permission is the
   same class of problem as missing credentials, but it must not leak.

### Requirement 3: Tagging and leak detection

**User Story:** As the account owner, I want leaked CAs to be easy to find
and remove, even after a SIGKILL that no handler can catch.

#### Acceptance Criteria

1. Each owned CA SHALL be created with the tags
   `kythira:suite=acm-pca-real-test` and `kythira:run-id=<run id>`.
   `CreateCertificateAuthority` SHALL pass them in its `Tags` parameter, so
   the CA is never untagged, even briefly.
2. A script, `scripts/aws-acm-pca-leaks.sh audit|sweep`, modelled on
   `scripts/aws-asg-leaks.sh`, SHALL find CAs by the `kythira:suite` tag
   and ignore any CA already in `DELETED` status.
   - `audit` SHALL exit 1 on any non-DELETED tagged CA older than
     `KYTHIRA_ACM_PCA_AUDIT_GRACE_SECONDS` (default 300). It SHALL also exit 1
     when it cannot list CAs.
   - `sweep` SHALL disable and delete those CAs as Requirement 2.1 describes,
     and SHALL exit 1 if any survives.
3. The script SHALL never touch a CA without the suite tag, including the
   CA named by `$KYTHIRA_TEST_ACM_PCA_ARN`.

### Requirement 4: Cost reporting

**User Story:** As the account owner, I want each run to report what it
cost, as the real-EC2 suites already do.

#### Acceptance Criteria

1. The suite SHALL add a `TestCostReport` to `g_cost_accumulator` and register
   `CostSummaryFixture`, so the run prints the `[aws-cost]` block from
   `tests/aws_real_ec2_test_support.hpp`.
2. The report SHALL list each owned CA as a `BilledResource`, with its
   lifetime from `CreateCertificateAuthority` returning to
   `DeleteCertificateAuthority` returning, at the short-lived hourly rate
   ($50/730 per hour, us-east-1 list price). It SHALL also list each
   issued certificate at $0.058, counting the two self-issued root
   certificates and every leaf.
3. `doc/aws_acm_pca_test_cost_estimate.md` SHALL be updated to state the
   per-run figure this model produces. Two CAs for about 10 minutes plus 4
   certificates comes to roughly $0.25, and the doc SHALL replace that
   estimate with the first measured value.

### Requirement 5: IAM

**User Story:** As the account owner, I want the CI role allowed to create
and delete only the CAs this suite owns.

#### Acceptance Criteria

1. A new policy bundle, `scripts/ci-cloud-credentials/aws/policies/acm-pca.json`,
   SHALL grant `acm-pca:CreateCertificateAuthority` and `acm-pca:TagCertificateAuthority`
   conditioned on `aws:RequestTag/kythira:suite = acm-pca-real-test`.
   - It SHALL grant `GetCertificateAuthorityCsr`, `IssueCertificate`,
     `GetCertificate`, `ImportCertificateAuthorityCertificate`,
     `GetCertificateAuthorityCertificate`, `DescribeCertificateAuthority`,
     `RevokeCertificate`, `UpdateCertificateAuthority` and
     `DeleteCertificateAuthority` conditioned on
     `aws:ResourceTag/kythira:suite = acm-pca-real-test`.
   - It SHALL grant `ListCertificateAuthorities` and `ListTags` unconditioned,
     since the leak script needs to list CAs.
   - `IssueCertificate` and the other per-CA actions SHALL NOT be granted on
     untagged CAs, so the role cannot issue from a production CA in the
     same account.
2. Each action in the bundle SHALL be checked against the ACM Private CA
   service authorization reference for the condition keys it actually
   supports. An action that does not support `aws:ResourceTag` SHALL be
   documented in the bundle's `Sid` and README entry, never silently
   widened.
3. `provision-oidc-role.sh` SHALL accept `acm-pca` as a bundle name, and
   `scripts/ci-cloud-credentials/aws/README.md` SHALL document it. Applying
   the bundle to the live CI role is an operator step that needs Clark's
   go-ahead, as with the ASG bundle.

### Requirement 6: Enabling the suite and CI

**User Story:** As a maintainer, I want the suite to be opt-in, and
schedulable in Real Cloud Tests without breaking the workflow's input cap.

#### Acceptance Criteria

1. The suite SHALL run only when `KYTHIRA_ACM_PCA_REAL_TESTS=1`, or when
   `$KYTHIRA_TEST_ACM_PCA_ARN` is set (today's opt-in, kept). Otherwise it
   SHALL exit 77 before `Aws::InitAPI()`. Creating billable resources stays
   behind an explicit switch.
2. CTest labels SHALL stay `slow`, `real-acm-pca` and `aws`, so the default
   CI run never builds a CA.
3. `.github/workflows/real-cloud-tests.yml` SHALL gain an ACM Private CA
   bundle in the AWS job, enabled only by the repository variable
   `REAL_CLOUD_TESTS_AWS_ACM_PCA_ENABLED`. It SHALL NOT add a
   `workflow_dispatch` input, because the workflow is at GitHub's
   25-input cap.
   - The step SHALL run the suite and then `aws-acm-pca-leaks.sh audit`. On
     audit failure it SHALL run `sweep` and still fail the job.
4. The AWS job's "every bundle disabled" guard SHALL count the new bundle.

### Requirement 7: Shared fixture code

**User Story:** As a maintainer, I want one implementation of the CA
bootstrap, so the LocalStack and real suites cannot drift apart.

#### Acceptance Criteria

1. The bootstrap, teardown and polling code SHALL live in
   `tests/aws_acm_pca_test_support.hpp` as a class, `ephemeral_acm_pca`, that
   both `aws_acm_pca_provider_localstack_test.cpp` and
   `aws_acm_pca_provider_real_test.cpp` use.
   - Its constructor SHALL take a client configuration, a revocation choice
     (OCSP or none) and the tags.
   - It SHALL expose `arn()` and `teardown()`.
2. The LocalStack suite's existing skip-on-unsupported behaviour SHALL be
   preserved: LocalStack bootstrap failures stay a skip, never a failure.
3. Signal registration (Requirement 2.4) SHALL be done by the real suite's
   fixture, not by `ephemeral_acm_pca`. LocalStack resources are throwaway,
   and the real-EC2 support header's globals should not be pulled into the
   LocalStack binary.
