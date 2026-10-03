# Implementation Plan — ACM Private CA Ephemeral Test CA

## Status: Not started (0/7 tasks)

**Last Updated**: October 3, 2026

## Overview

Make `aws_acm_pca_provider_real_test` create and delete its own ACM Private
CAs per run instead of requiring `$KYTHIRA_TEST_ACM_PCA_ARN`. The tasks add
leak detection, cost reporting, a tag-scoped IAM bundle and an opt-in Real
Cloud Tests step. See `requirements.md` and `design.md`.

Background: on 2026-10-03 the suite's first real run needed a CA created and
deleted by hand (personal profile, us-west-2), because the account had
none. The CA lived 8 min 45 s. With three certificates (one root and two
leaves), the run cost about $0.18, assuming the root's self-issued
certificate is billed like a leaf; task 7 confirms that. The run also found
a PEM-join bug in `sign_csr()`, fixed alongside this spec.

## Task Dependency Graph

```json
{
  "waves": [
    {"wave": 1, "tasks": [1], "description": "Shared ephemeral_acm_pca class; everything else builds on it"},
    {"wave": 2, "tasks": [2, 3, 4], "description": "Real fixture and cases, LocalStack port, IAM bundle: independent of each other"},
    {"wave": 3, "tasks": [5], "description": "Leak script needs the tags from task 2"},
    {"wave": 4, "tasks": [6], "description": "Workflow step needs the suite, the bundle and the script"},
    {"wave": 5, "tasks": [7], "description": "First real runs and cost doc, with Clark's go-ahead"}
  ]
}
```

## Tasks

- [ ] 1. Add `ephemeral_acm_pca` to `tests/aws_acm_pca_test_support.hpp`
  - Lift `create_active_root_ca`, `poll_until_ready` and `teardown` out of
    `LocalStackCaFixture` into the class from `design.md`: options struct,
    `on_created` hook, `ephemeral_ca_unavailable` exception, and an
    idempotent, never-throwing teardown guarded by `std::atomic_flag`.
  - Add status-aware teardown per the design table: ACTIVE goes to DISABLE
    then DELETE(7), and CREATING/PENDING_CERTIFICATE go straight to DELETE.
  - Set `UsageMode = SHORT_LIVED_CERTIFICATE`. Pass tags in
    `CreateCertificateAuthority`'s own `Tags` field.
  - Set root validity per Requirement 1.2. The 2026-10-03 manual run's
    short-lived root CA accepted a 5-year root, so 1 year is well inside
    the limit. Create-to-ACTIVE took about a minute, under the 120 s
    per-step bound.
  - Verify: the header compiles in both test translation units.
  - _Requirements: 1.1-1.3, 2.1-2.3, 2.5, 3.1, 7.1_

- [ ] 2. Rewrite `aws_acm_pca_provider_real_test.cpp` around `RealCaFixture`
  - Enabled check: `KYTHIRA_ACM_PCA_REAL_TESTS=1` or
    `$KYTHIRA_TEST_ACM_PCA_ARN`, otherwise exit 77 before `Aws::InitAPI()`.
  - When an ARN is supplied, classify it with `revocation_configured()` and
    create only the other kind. Never modify the supplied CA.
  - `signal_cleanup_target`, `install_aws_signal_handlers()`, registration
    from `on_created`, and teardown plus exit 77 on
    `ephemeral_ca_unavailable`.
  - Add the five cases from the design table, replacing the current three.
    The bare-CA revoke asserts the AWS error (certificate-authority Req 10.7).
  - Add a `TestCostReport` (CA lifetimes at $50/730 per hour, certificates
    at $0.058 each) and `CostSummaryFixture`.
  - Link `aws_real_ec2_test_support.hpp`'s needs in `tests/CMakeLists.txt`
    if any are missing. Keep labels `slow;real-acm-pca;aws`.
  - Verify: compiles; with neither variable set it exits 77 without
    touching AWS.
  - _Requirements: 1.4, 1.5, 2.4-2.6, 4.1, 4.2, 6.1, 6.2_

- [ ] 3. Port `aws_acm_pca_provider_localstack_test.cpp` to `ephemeral_acm_pca`
  - Keep the skip-on-unsupported behaviour and add no signal wiring.
  - Verify: compiles; behaviour unchanged on community LocalStack (exit 77).
  - _Requirements: 7.2, 7.3_

- [ ] 4. Add the `acm-pca` IAM bundle
  - `scripts/ci-cloud-credentials/aws/policies/acm-pca.json` per the
    design, after checking each action's supported condition keys in the
    ACM Private CA service authorization reference. Document any action
    that cannot be tag-scoped in its `Sid` and the README.
  - Scope `IssueCertificate` to the two templates the suite uses through
    `acm-pca:TemplateArn`.
  - Accept the bundle in `provision-oidc-role.sh` and document it in the
    AWS README.
  - Verify: `aws iam simulate-custom-policy` (no account changes): allowed
    on a tagged CA ARN, denied on an untagged one, and create denied
    without the request tag.
  - Applying the bundle to the CI role needs Clark's go-ahead.
  - _Requirements: 5.1-5.3_

- [ ] 5. Add `scripts/aws-acm-pca-leaks.sh audit|sweep`
  - Model it on `aws-asg-leaks.sh`. List CAs, read tags with `list-tags`,
    skip `DELETED`, and apply a grace period
    (`KYTHIRA_ACM_PCA_AUDIT_GRACE_SECONDS`, default 300).
  - `audit` exits 1 on a leak or when it cannot list. `sweep` uses the
    teardown order from Requirement 2.1 and exits 1 if anything survives.
  - Put the copyright header after the shebang. Pass shellcheck.
  - Verify: shellcheck clean. A SIGKILL'd local run leaves a tagged CA,
    `audit` exits 1 naming it, and `sweep` removes it (needs Clark's
    go-ahead, as in the ASG task 9 check).
  - _Requirements: 3.2, 3.3_

- [ ] 6. Wire the step into `real-cloud-tests.yml`
  - Add `BUNDLE_ACM_PCA` from `vars.REAL_CLOUD_TESTS_AWS_ACM_PCA_ENABLED`
    only; add no `workflow_dispatch` input (25-input cap).
  - Steps: ctest for the one test, then audit (`if: always()`), then sweep
    on audit failure while the job still fails.
  - Count the bundle in the zero-bundle guard and its error message.
  - Verify: actionlint clean and the `workflow-input-limits` check passes.
  - _Requirements: 6.3, 6.4_

- [ ] 7. First real runs and cost doc
  - Run locally with `KYTHIRA_ACM_PCA_REAL_TESTS=1`, then once in Actions
    after the bundle is on the CI role. Both need Clark's go-ahead per the
    real-cloud cost rules.
  - Record the measured `[aws-cost]` total in
    `doc/aws_acm_pca_test_cost_estimate.md`, and check in the billing
    console that DELETED CAs stop accruing (design, "Ordering and quotas").
  - Run `audit` after each run and confirm it is clean.
  - _Requirements: 4.3_
