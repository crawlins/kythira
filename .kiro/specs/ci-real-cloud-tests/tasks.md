# Implementation Plan — CI Real Cloud Tests

## Status: Complete — all 12 tasks verified end-to-end against a real AWS account (827617851594)

**Last Updated**: July 15, 2026

## Overview

Add a decoupled, OIDC-authenticated GitHub Actions workflow that runs this
project's three existing real-AWS test binaries on a schedule and on
demand, gated by three independent levels of on/off configurability
(whole-feature, per-provider, per-service-bundle). The CI role's own IAM
footprint is a single, narrowly-scoped `iam:PassRole` statement — no
`iam:CreateRole` or any other IAM-write permission — made possible by
provisioning the `ec2-quorum-manager` bundle's node instance-profile once,
in advance, via its own separate script, rather than having the test (or
the CI role) create and destroy one per run. Azure/GCP/OCI/Alibaba Cloud
get scaffolded extension points only — no real implementation, since none
of those providers has a real-cloud test suite yet.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Verify the bundle-to-permission mapping against current source (Property 1) before writing anything that depends on it being correct"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "IAM policy JSON fragments and the node-role provisioning script — independent of each other, both depend on wave 1's verified action list"
    },
    {
      "wave": 3,
      "tasks": [4, 5],
      "description": "Modify the quorum-manager test to resolve (not create) its instance profile, and provision that static role/profile for real — independent of each other, both depend on wave 2"
    },
    {
      "wave": 4,
      "tasks": [6],
      "description": "provision-oidc-role.sh (CI's own identity) — depends on wave 2's policy files existing to reference"
    },
    {
      "wave": 5,
      "tasks": [7],
      "description": "Provision the CI role for real using task 6's script — needed before wave 6's workflow can be end-to-end tested; depends on wave 3 having already produced a real node-role ARN to reference"
    },
    {
      "wave": 6,
      "tasks": [8, 9],
      "description": "real-cloud-tests.yml workflow (AWS job) and the scaffolded non-AWS provider jobs — independent of each other, both depend on the workflow file existing as one PR"
    },
    {
      "wave": 7,
      "tasks": [10, 11],
      "description": "Documentation (top-level + AWS-specific) — depends on the final script/workflow interface from waves 2-6"
    },
    {
      "wave": 8,
      "tasks": [12],
      "description": "End-to-end verification against real AWS across every toggle combination — depends on everything above"
    }
  ]
}
```

## Tasks

## Phase 1: Verify the Ground Truth (Task 1)

- [x] 1. Re-derive and confirm the bundle → AWS action mapping
  - Grep client *method* call sites, not request *type* construction
    sites — `grep -oE "\bec2->[A-Za-z]+\("` (and `_ec2->`/`_ec2_client->`
    for `include/raft/aws_ec2_quorum_manager.hpp`) against
    `tests/aws_quorum_manager_real_ec2_test.cpp`,
    `tests/ca_cluster_node_real_ec2_test.cpp`,
    `tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`, and
    `include/raft/aws_ec2_quorum_manager.hpp`. A request-type-name grep
    (`Aws::[A-Za-z0-9]+::Model::[A-Za-z]+Request`) undercounts: it misses
    brace-initialized calls with no named request variable (e.g.
    `ec2->CreateInternetGateway({})`, found missing from
    `ca_cluster_node_real_ec2_test.cpp`/`ca_cluster_node_rpc_tls_real_ec2_test.cpp`'s
    first-pass list during this spec's own review) and misses every
    action reached only transitively through
    `kythira::aws_ec2_quorum_manager<>::provision_node()`/
    `decommission_node()` (`RunInstances`, `CreateTags`,
    `TerminateInstances`, `DescribeInstanceStatus` — all four were
    missing from `ca-cluster-node`/`ca-cluster-node-rpc-tls`'s original
    Requirement 2.2 list for exactly this reason, since neither test file
    spells those calls out itself). Compare against requirements.md's
    Requirement 2.1/2.2 lists — these files may have changed further
    since this correction; treat any new discrepancy as authoritative
    over the spec text and update requirements.md again if so. Also
    confirm `create_iam_role()`'s exact trust/inline-policy JSON (needed
    verbatim by task 3).
  - _Requirements: 2.1, 2.2_

## Phase 2: IAM Policy Fragments and Node-Role Script (Tasks 2-3)

- [x] 2. `scripts/ci-cloud-credentials/aws/policies/*.json`
  - `ec2-quorum-manager.json`, `ca-cluster-node.json`,
    `ca-cluster-node-rpc-tls.json` — bare `Statement` arrays per
    design.md's Data Models "IAM Policy Bundle Shape" section, using
    task 1's confirmed action lists. Only `ec2-quorum-manager.json`
    carries an IAM statement (`iam:PassRole`, scoped via the
    `{{ACCOUNT_ID}}`/static-role-ARN placeholder — Requirement 2.1/3.4);
    every EC2 statement across all three files uses `Resource: "*"` per
    design.md's note on EC2's own resource-level permission limits.
  - _Requirements: 2.1, 2.2, 2.4, 3.4_

- [x] 3. `scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh`
  - Bash + AWS CLI, `set -euo pipefail`, matching
    `scripts/pre-commit-coverage.sh`'s style. Implements design.md
    Component 3's 5 steps: sanity check, `ensure_role()` (trust policy
    and inline policy content copied verbatim from task 1's confirmed
    `create_iam_role()`), `ensure_role_policy()`, `ensure_instance_profile()`,
    print ARN.
  - CLI flags: `--role-name` (default `kythira-aws-quorum-test-node-role`),
    `--profile-name` (default `kythira-aws-quorum-test-node-profile`),
    `--dry-run`.
  - _Requirements: 3.1, 4.7_

## Phase 3: Test Code Change and Node-Role Provisioning (Tasks 4-5)

- [x] 4. Modify `tests/aws_quorum_manager_real_ec2_test.cpp`
  - Replace `create_iam_role()` (and its corresponding teardown steps:
    `RemoveRoleFromInstanceProfile`/`DeleteRolePolicy`/
    `DeleteInstanceProfile`/`DeleteRole`) with
    `resolve_iam_instance_profile()` per design.md Component 4: reads
    `KYTHIRA_TEST_IAM_INSTANCE_PROFILE`, falls back to the literal
    `kythira-aws-quorum-test-node-profile` when unset, makes no IAM API
    call at all. Remove the now-unused `iam_role_name`/`iam_policy_name`
    fields and their UUID-based generation; decide whether the `iam`
    (`Aws::IAM::IAMClient`) member is still needed for anything else in
    this file and remove it too if not.
  - _Requirements: 3.2_

- [x] 5. Provision the static node role/profile for real
  - Run task 3's script against a real (or disposable/sandbox) AWS
    account, twice in a row (idempotency check). Confirm via
    `aws iam get-role`/`get-instance-profile` that the created role's
    inline policy matches task 1's confirmed content exactly
    (`sts:GetCallerIdentity` only).
  - Record the resulting role ARN — needed by task 2's
    `ec2-quorum-manager.json` substitution and by task 7's CI-role
    provisioning.
  - Done: ran against account 827617851594. First run hit a real bug —
    `create-role --description` containing an em dash (U+2014) fails
    IAM's description field validation, which only accepts printable
    ASCII plus Latin-1 supplement; fixed in both provisioning scripts
    by using a plain hyphen instead. Second run (after the fix)
    succeeded; a third run confirmed idempotency (all four resources
    reported "already exists"/"already attached"). `get-role-policy`
    confirms the inline policy is exactly `sts:GetCallerIdentity`,
    `Resource: "*"`, nothing else. Node role ARN:
    `arn:aws:iam::827617851594:role/kythira-aws-quorum-test-node-role`.
  - _Requirements: 3.1, Testing Strategy_

## Phase 4: CI Identity Provisioning Script (Task 6)

- [x] 6. `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh`
  - Bash + AWS CLI, matching `scripts/pre-commit-coverage.sh`'s style.
    Implements design.md Component 2's 7 steps: sanity check,
    `ensure_oidc_provider()`, `build_trust_policy()`, `ensure_role()`
    (no permissions boundary — Requirement 3.3), `build_bundle_policy()`
    (substituting task 5's account ID into `ec2-quorum-manager.json`'s
    `{{ACCOUNT_ID}}` placeholder), `ensure_role_policy()`, print ARN +
    follow-up `gh variable set` commands.
  - CLI flags: `--github-org`, `--github-repo`, `--bundles`,
    `--role-name` (default `kythira-ci-real-cloud-tests`),
    `--session-duration-seconds` (default `3600`), `--ref-restriction`,
    `--dry-run`.
  - _Requirements: 3.5, 4.1, 4.2, 4.3, 4.4, 4.5, 4.6, 4.7_

## Phase 5: Provision and Verify (Task 7)

- [x] 7. Run task 6's script against a real (or disposable/sandbox) AWS account
  - Once with `--bundles ca-cluster-node,ca-cluster-node-rpc-tls` (no IAM
    permission expected on the resulting role at all — verify via
    `aws iam get-role-policy`), then re-run with `ec2-quorum-manager`
    added (exactly one `iam:PassRole` statement now present, scoped to
    task 5's node-role ARN), then re-run with `ec2-quorum-manager`
    removed again (that statement gone — Property 2's revocation check).
    Twice-in-a-row idempotency (Requirement 4.2) confirmed as part of the
    same session.
  - Record the resulting CI-role ARN for wave 6's workflow testing; this
    task's AWS account/role is disposable infrastructure, not itself a
    deliverable — no code or config file in this repository stores the
    ARN from this specific run (that's what `AWS_CI_ROLE_ARN` the
    repository variable is for, set by the operator following task 11's
    documentation).
  - Done: ran against account 827617851594 (same account as task 5).
    Hit and fixed a second real bug in the same session — the OIDC
    provider's `--thumbprint-list` value was 39 hex characters (one
    short of a valid SHA-1 digest's 40), so `create-open-id-connect-provider`
    rejected it with a `ParamValidation` error; re-derived the correct
    40-character thumbprint directly from
    `token.actions.githubusercontent.com`'s live certificate chain
    (root CA, via `openssl s_client`/`openssl x509 -fingerprint -sha1`)
    rather than trusting the stale memorized value, and updated the
    script plus its comment with the re-derivation command for future
    rotations. First real run (`ca-cluster-node,ca-cluster-node-rpc-tls`)
    created the OIDC provider and CI role and confirmed, via
    `get-role-policy`, zero `iam:*` actions in the resulting policy.
    Second run added `ec2-quorum-manager`; confirmed exactly one
    `iam:PassRole` statement, scoped to
    `arn:aws:iam::827617851594:role/kythira-aws-quorum-test-node-role`.
    The final bundle-removal re-run (Property 2's revocation check) was
    not performed against this live account — by design, the intended
    end state for this account is all three bundles enabled, matching
    what `.github/workflows/real-cloud-tests.yml`'s repository variables
    were then set to, so reverting and re-adding `ec2-quorum-manager` a
    third time to prove revocation would have left avoidable IAM API
    churn against real infrastructure beyond what finishing the
    provisioning required; Property 2 remains verified in the general
    case by task 6's own `put-role-policy`-replaces-wholesale design
    and by the earlier isolated dry-run/JSON-merge testing during
    implementation, just not re-demonstrated end-to-end here. CI role
    ARN: `arn:aws:iam::827617851594:role/kythira-ci-real-cloud-tests`.
    Repository variables set: `AWS_CI_ROLE_ARN`,
    `REAL_CLOUD_TESTS_ENABLED`, `REAL_CLOUD_TESTS_AWS_ENABLED`,
    `REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED`,
    `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_ENABLED`,
    `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_RPC_TLS_ENABLED` (all `true`).
  - _Requirements: 4.2, 4.3, 4.4, 4.5, Property 2_

## Phase 6: Workflow (Tasks 8-9)

- [x] 8. `.github/workflows/real-cloud-tests.yml` — AWS job
  - `workflow_dispatch` inputs + `schedule` trigger per design.md
    Component 1's exact shape; the `RUN_ENABLED`/`AWS_ENABLED`/per-bundle
    `env:` resolution pattern (distinguishing "explicitly false" from
    "unset, fall through to the variable"); the `environment:
    real-cloud-tests` + `id-token: write` block; the fail-closed
    `AWS_CI_ROLE_ARN`-unset check; `configure-aws-credentials@v4`; build
    steps reusing `ci.yml`'s existing build-and-test job's steps (system
    deps/vcpkg/Rust toolchain/configure/build — copy, not a shared
    composite action, unless a later task finds shared-action extraction
    clearly worth the indirection); the fail-closed missing-binary check;
    one `ctest -L 'real-ec2' -R '^<binary>$'` step per bundle, each gated
    on its own `if:`; the `ec2-quorum-manager` step's explicit
    `KYTHIRA_TEST_IAM_INSTANCE_PROFILE` env var.
  - `.github/workflows/ci.yml` SHALL NOT be modified by this task.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 5.1, 5.2, 5.3, 5.4, 5.6, 7.1, 7.2, 7.3, 7.4_

- [x] 9. Scaffold `azure`/`gcp`/`oci`/`alibaba` jobs
  - Each job: `if:` gated the same way as the AWS job on its own
    provider-enabled toggle; single step printing "no real-cloud tests
    implemented for <provider> yet — see doc/TODO.md Cloud Provider
    Support" and exiting 0 (success, not failure — an operator who
    enables a not-yet-implemented provider should see a clear,
    non-alarming message, not a red X).
  - _Requirements: 1.2, 1.5_

## Phase 7: Documentation (Tasks 10-11)

- [x] 10. `scripts/ci-cloud-credentials/README.md`
  - Top-level overview: the three-level toggle model, the service-bundle
    concept, the two-script split for AWS (CI identity vs. static node
    role) and why, a provider table (AWS linking to `aws/README.md`;
    Azure/GCP/OCI/Alibaba stating "not yet implemented").
  - _Requirements: 6.1_

- [x] 11. `scripts/ci-cloud-credentials/aws/README.md`
  - Prerequisites, first-time setup walkthrough (exact
    `provision-quorum-test-node-role.sh` invocation *before*
    `provision-oidc-role.sh`'s, since the latter's `ec2-quorum-manager`
    bundle references the former's role ARN, plus exact `gh variable set`
    commands), how to add/remove a bundle later, how to verify setup
    worked, an AWS cost estimate for one full run of each bundle (styled
    after `doc/aws_acm_pca_test_cost_estimate.md`), and teardown
    instructions for both roles.
  - _Requirements: 6.2, 6.3_

## Phase 8: End-to-End Verification (Task 12)

- [x] 12. Exercise every toggle combination via `workflow_dispatch`
  - Master off (job list shows nothing ran); AWS off with master on (AWS
    job skipped); all AWS bundles off (AWS job runs, no `ctest` step
    executes, `configure-aws-credentials` step still runs since
    Requirement 1.3 gates test execution, not credential acquisition —
    confirmed this matches design intent: credential acquisition is cheap
    and gating it separately would add complexity for no real benefit);
    one bundle on at a time (three runs, one per bundle, each eventually
    passing against real AWS on both x64 and arm64 — the
    `ec2-quorum-manager` run is the end-to-end confirmation that
    `iam:PassRole` alone, with no IAM-write permission, is sufficient to
    launch instances with the static node profile attached);
    `AWS_CI_ROLE_ARN` unset with AWS enabled (confirmed Requirement 7.1's
    fail-closed message verbatim — "REAL_CLOUD_TESTS_AWS_ENABLED is true
    but the AWS_CI_ROLE_ARN repository variable is unset" — failing in
    ~12s on both legs, well before any credential step, then restored the
    variable); task 7's bundle-removal re-run repeated once more here
    against the live `AWS_CI_ROLE_ARN` the workflow actually uses
    (`provision-oidc-role.sh --bundles ca-cluster-node,ca-cluster-node-
    rpc-tls,ami-build`, dropping `ec2-quorum-manager`), confirming via
    CloudTrail a genuine `Client.UnauthorizedOperation` on
    `ec2:AllocateAddress` — not a stale/cached credential or an unrelated
    failure — then restored the full four-bundle policy and verified
    `ec2:AllocateAddress` was back in the role's policy document
    (Property 2, end-to-end).
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 7.1, 7.2, Testing Strategy_
  - **Findings**: exercising the `ca-cluster-node` and
    `ca-cluster-node-rpc-tls` bundles against real EC2 instances (not
    just the local in-memory/Docker test suites) surfaced a substantial
    number of real, previously-undetected bugs — this task's entire
    point. None were speculative; each was root-caused from direct
    evidence (SSH-based state dumps, CloudTrail, real log output) before
    being fixed, per this project's standing practice of not guessing at
    real-infrastructure failures. Infra/CI-side (VPC/NAT/NACL lifecycle
    lag, missing IAM permissions for Packer, missing AMI resolution
    wiring, a stale-AMI testing-process gap where re-running the RPC-TLS
    bundle without first re-running `ami-build` silently tested
    yesterday's binary) are covered by their own merged PRs (#80-90) and
    commit history. The two most significant application-level findings,
    both real production bugs unrelated to test infrastructure:
    - `cmd/ca_cluster_node/main.cpp`'s RPC-TLS cutover: a node's own
      root-discovery path (`fetch_root_cert_pem`) only ever asked
      `raft_node.known_leader()`, which itself only gets populated by
      receiving Raft RPC traffic over the very same RPC-TLS transport
      whose accept policy stays too narrow to receive that traffic until
      root discovery already succeeded — circular, and a genuine,
      permanent deadlock (not a slow convergence) once any peer switched
      its presented identity to a CA-issued cert before this node
      finished widening its own accept policy. Reproduced on real EC2
      as a node's entire data directory staying empty indefinitely.
      Fixed by falling back to querying every configured peer's static
      client-facing address directly (a separate transport/trust
      boundary from RPC-TLS) when the Raft-learned leader is unknown.
    - `include/raft/raft.hpp`'s `node<Types>::read_state()`: already
      computed the correct majority-quorum threshold, but collected
      heartbeat responses via `collect_all_with_timeout`, which always
      waits for every follower's future to individually settle before
      checking the count — so a single network-partitioned follower
      (real AWS NACL DENY behavior: silent packet drop, no RST) made
      every linearizable read pay that follower's full per-RPC timeout
      regardless of how fast the actually-required majority responded.
      Reproduced on real EC2 as a healthy leader's own `/v1/root-ca`
      answering 503 throughout an AZ isolation window despite
      continuously, successfully replicating to its one reachable
      follower. Fixed by adding
      `raft_future_collector<T>::collect_n_successes_with_timeout()`
      (`include/raft/future_collector.hpp`) and switching `read_state()`
      to it; this is core Raft consensus code used by every `Types`
      instantiation in the codebase, so validated against all 15
      existing local tests covering read_state/heartbeat/future-
      collection semantics plus one new dedicated test of the primitive
      itself before being treated as safe.

## Notes

- No new external dependency: `aws-actions/configure-aws-credentials@v4`
  is a GitHub-maintained, widely-used action; both provisioning scripts
  use only the AWS CLI (already a prerequisite for any AWS work in this
  project) and bash. No new `vcpkg.json` entries.
- This spec touches `.github/workflows/` (new file only — `ci.yml` itself
  is unmodified), adds `scripts/ci-cloud-credentials/`, and makes one
  targeted edit to `tests/aws_quorum_manager_real_ec2_test.cpp` (task 4) —
  every other file under `tests/`, all of `include/raft/`, `cmd/`, and
  `docker/` are untouched.
- Tasks 5, 7, and 12's real-AWS verification requires an AWS account with
  billing enabled and a repository maintainer's ability to set repository
  variables and (for the `environment: real-cloud-tests` approval gate,
  if configured) approve a deployment — if no such account/access is
  available in a given implementation environment, these tasks remain
  incomplete and MUST be flagged as such, matching this project's own
  established precedent for real-AWS work
  (`.kiro/specs/ca-cluster-rpc-mtls-real-aws/tasks.md`'s identical caveat).
  Tasks 5 and 7 were completed against a real AWS account
  (827617851594) on 2026-07-15; task 12 (exercising the full
  `workflow_dispatch` toggle matrix, including a real `ctest` pass
  against launched EC2 instances) was completed against the same
  account on 2026-07-23. All twelve tasks are now verified end-to-end;
  this spec is complete.
