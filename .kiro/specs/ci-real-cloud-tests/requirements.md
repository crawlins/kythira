# Requirements Document

## Introduction

This project already has three real-AWS integration test binaries —
`tests/aws_quorum_manager_real_ec2_test.cpp`,
`tests/ca_cluster_node_real_ec2_test.cpp`, and
`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp` — all gated behind
`KYTHIRA_AWS_REAL_EC2_TESTS`/`LIBSSH2_FOUND`, labeled `real-ec2;slow`, and
excluded from every existing CTest run via `.github/workflows/ci.yml`'s
`-LE '^(slow|performance|verbose|benchmark|docker)$'` filter. They have
never been run against real AWS in this project's CI — every completion
note attached to them says "compile-verified only, no AWS access in this
environment." (LocalStack-backed tests, e.g.
`tests/ca_cluster_node_localstack_test.cpp`, are a separate, already-working
tier gated by `KYTHIRA_AWS_LOCALSTACK_TESTS` and out of scope here — they
need no cloud credentials at all.)

This document specifies the CI/CD infrastructure to actually run those
tests (and equivalents for other cloud providers, as they're implemented)
on a schedule and on demand, authenticated via short-lived, OIDC-federated
credentials — no long-lived cloud access keys stored as GitHub secrets —
with three independent levels of on/off configurability:

1. **Whole-feature switch**: real-cloud testing entirely on or off.
2. **Per-cloud-provider switch**: AWS, Azure, GCP, OCI, Alibaba Cloud
   (matching `doc/TODO.md`'s "Cloud Provider Support" list) independently
   on or off.
3. **Per-service-bundle switch**: within an enabled provider, only the
   specific test suites (and therefore only the specific cloud-service
   permissions those suites need) that are switched on actually run —
   turning off a bundle means the CI role never even holds the
   permissions that bundle's tests would have used.

Only AWS has real-cloud test coverage today (three binaries, described
above). This spec designs the framework generically — the workflow
structure, the three-level toggle model, the credential-provisioning
scripts' contract, and the documentation format — so that adding Azure,
GCP, OCI, or Alibaba Cloud support later (once those providers have their
own real-cloud test suites, per `doc/TODO.md`'s "Cloud Provider Support"
section) means writing that provider's own provisioning script(s) and one
new documentation page following an established pattern, not redesigning
the workflow. Concrete, working implementation is scoped to AWS only; the
other four providers get a documented extension point and are explicitly
out of scope for this spec's own deliverable (see design.md's Non-Goals).

## Glossary

- **Real-cloud test**: an integration test that provisions and exercises
  actual cloud-provider resources (as opposed to a unit test, or an
  integration test against a local emulator like LocalStack). Distinguished
  in this codebase by the `real-ec2` CTest label (AWS) or its future
  per-provider equivalents.
- **Service bundle**: a named group of one or more real-cloud test
  binaries/CTest labels that share the same cloud-service permission
  requirements — e.g. AWS's `ec2-quorum-manager` bundle
  (`aws_quorum_manager_real_ec2_test`, needing EC2 + IAM + STS) versus its
  `ca-cluster-node` bundle (`ca_cluster_node_real_ec2_test`, needing only
  EC2 + STS). Enabling a bundle is what drives both "which tests run" and
  "which permissions the CI role needs."
- **OIDC federation**: exchanging a GitHub Actions-issued OpenID Connect
  token for short-lived cloud credentials via each provider's native
  workload-identity mechanism (AWS IAM OIDC identity provider +
  `sts:AssumeRoleWithWebIdentity`; Azure federated credentials; GCP
  Workload Identity Federation; etc.) — no cloud access key or secret is
  ever stored in GitHub.
- **Provisioning script**: the one-time (or re-run-on-change), operator-run
  script per cloud provider that creates the IAM role (or provider
  equivalent) CI will assume, its trust policy scoped to this repository,
  and its permissions scoped to exactly the enabled service bundles.

## Requirements

### Requirement 1: Three-level on/off configurability

**User Story:** As a maintainer, I want real-cloud testing controllable at
the whole-feature level, the per-provider level, and the per-service-bundle
level, so that I can run only what I've actually provisioned credentials
for, and so that a CI role never holds more permission than the tests it's
actually allowed to run need.

#### Acceptance Criteria

1. A repository variable `REAL_CLOUD_TESTS_ENABLED` (`true`/`false`,
   default absent/`false`) SHALL gate the entire feature: WHEN it is not
   `true`, the real-cloud-tests workflow SHALL exit successfully without
   attempting to acquire any cloud credential or run any real-cloud test,
   on both its scheduled and manually-dispatched triggers.
2. One repository variable per cloud provider (`REAL_CLOUD_TESTS_AWS_ENABLED`,
   `REAL_CLOUD_TESTS_AZURE_ENABLED`, `REAL_CLOUD_TESTS_GCP_ENABLED`,
   `REAL_CLOUD_TESTS_OCI_ENABLED`, `REAL_CLOUD_TESTS_ALIBABA_ENABLED`,
   default absent/`false`) SHALL gate that provider's portion of the
   workflow independently — WHEN Requirement 1.1's master switch is `true`
   but a given provider's variable is not `true`, that provider's job SHALL
   be skipped, and every other enabled provider's job SHALL still run.
3. One repository variable per service bundle within an enabled provider
   (e.g. `REAL_CLOUD_TESTS_AWS_EC2_QUORUM_ENABLED`,
   `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_ENABLED`,
   `REAL_CLOUD_TESTS_AWS_CA_CLUSTER_RPC_TLS_ENABLED`, default absent/`false`)
   SHALL gate whether that bundle's CTest label subset runs at all. A
   provider job with every one of its own bundles disabled SHALL skip
   without acquiring cloud credentials.
4. `workflow_dispatch` SHALL expose boolean inputs mirroring every
   variable in Requirements 1.1–1.3, each defaulting to that variable's
   current value (`${{ inputs.x || vars.X }}` pattern) — an operator
   triggering a manual run SHALL be able to override any subset of
   toggles for that one run without changing the persisted repository
   variables.
5. `doc/TODO.md`'s "Cloud Provider Support" section SHALL remain the single
   source of truth for which providers have real-cloud test coverage at
   all — this workflow SHALL NOT reference a provider whose variable
   toggle exists but which has zero real-cloud tests to run (i.e., Azure/
   GCP/OCI/Alibaba's variables and job skeletons MAY exist as documented
   extension points per this spec's design, but their jobs SHALL no-op
   with a clear "no real-cloud tests implemented for this provider yet"
   message rather than failing, until a corresponding spec adds their
   test suites).

### Requirement 2: AWS service-bundle-to-permission mapping

**User Story:** As a security-conscious maintainer, I want the CI role's
AWS permissions scoped to exactly the service bundles currently enabled,
so that turning off a bundle (e.g. because I don't want CI creating IAM
roles) actually removes that risk, not just skips the tests that would
have used it.

#### Acceptance Criteria

1. The `ec2-quorum-manager` bundle (`tests/aws_quorum_manager_real_ec2_test.cpp`,
   CTest name `aws_quorum_manager_real_ec2_test`) SHALL map to: the full
   EC2 lifecycle action set that file and `include/raft/aws_ec2_quorum_manager.hpp`
   exercise (VPC/subnet/security-group/route-table/internet-gateway/NAT-
   gateway/NACL/Elastic-IP/key-pair create-and-delete, `RunInstances`,
   `TerminateInstances`, `StopInstances`, `StartInstances`,
   `DescribeInstances`, `DescribeInstanceStatus`, `DescribeInstanceTypes`,
   `DescribeSpotPriceHistory`, `DescribeImages`, `GetConsoleOutput`,
   `CreateTags`) plus `sts:GetCallerIdentity` plus exactly one IAM action,
   `iam:PassRole`, scoped to the single, static, pre-provisioned node role
   ARN Requirement 3 creates — AWS enforces this permission implicitly on
   `ec2:RunInstances` whenever an `IamInstanceProfileSpecification` is
   attached (`tests/aws_quorum_manager_real_ec2_test.cpp`'s own
   `launch_bastion()`/quorum-manager-driven node launches both do this),
   checked against the specific role ARN being passed. This bundle SHALL
   NOT include any IAM role/policy/instance-profile *lifecycle* action
   (`CreateRole`, `PutRolePolicy`, `CreateInstanceProfile`,
   `AddRoleToInstanceProfile`, or their delete/remove counterparts) — see
   Requirement 3 for why the CI role never needs them at all, not merely
   why they'd be risky to grant.
2. The `ca-cluster-node` bundle (`tests/ca_cluster_node_real_ec2_test.cpp`)
   and the `ca-cluster-node-rpc-tls` bundle
   (`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`) SHALL each map to
   the EC2 action subset those files actually use (VPC/subnet/security-
   group/route-table/internet-gateway/key-pair create-and-delete,
   `RunInstances`, `TerminateInstances`, `DescribeInstances`, plus — for
   `ca-cluster-node-rpc-tls` only — the deny-all-NACL action subset:
   `CreateNetworkAcl`, `CreateNetworkAclEntry`, `DescribeNetworkAcls`,
   `ReplaceNetworkAclAssociation`, `DeleteNetworkAcl`) plus
   `sts:GetCallerIdentity`. Neither bundle SHALL include any IAM action at
   all, not even `iam:PassRole` — those two test files never attach an
   instance profile to a launched node.
3. Between the three bundles, the CI role's total IAM footprint is exactly
   one action (`iam:PassRole`), scoped to exactly one resource (the static
   node role's ARN) — no bundle, individually or in combination, ever
   grants the CI role the ability to create, modify, or delete any IAM
   principal. This SHALL be treated as the measure of this design's
   success, not merely a nice-to-have: a compromised or over-broad CI
   credential with `iam:CreateRole`/`iam:PutRolePolicy` could grant itself
   (or a newly-created role) arbitrary further permissions; a credential
   holding only a narrowly-scoped `iam:PassRole` cannot, since it has no
   way to change what the role it's permitted to pass is allowed to do.
4. The provisioning script (Requirement 4) SHALL accept a list of enabled
   bundle names and SHALL attach only the corresponding permission
   statements — an operator who only ever enables `ca-cluster-node`/
   `ca-cluster-node-rpc-tls` and never `ec2-quorum-manager` SHALL end up
   with a CI role that holds no `iam:PassRole` permission either, since
   only the `ec2-quorum-manager` bundle's tests ever launch an instance
   with an attached instance profile.
5. Each real-cloud test binary's CTest registration already carries labels
   identifying its bundle unambiguously (`aws_quorum_manager_real_ec2_test`
   → label `quorum` combined with `real-ec2`; `ca_cluster_node_real_ec2_test`
   → label `ca_cluster_node` combined with `real-ec2` and NOT `rpc_tls`;
   `ca_cluster_node_rpc_tls_real_ec2_test` → labels `ca_cluster_node` AND
   `rpc_tls` combined with `real-ec2`) — the workflow's `ctest -L`/`-LE`
   invocation for a given bundle SHALL use these existing labels, adding
   no new ones, so a bundle's test selection and its permission scope stay
   verifiably in sync with the same source of truth used elsewhere in this
   project's test suite.

### Requirement 3: Static, pre-provisioned quorum-manager test node role

**User Story:** As a security reviewer, I want the IAM role/instance-profile
that `aws_quorum_manager_real_ec2_test.cpp` attaches to the EC2 instances it
launches to be created once, in advance, by an operator with full IAM
rights — not dynamically created and destroyed by the CI role itself on
every run — so the CI role's own IAM footprint can be a single, narrowly-
scoped `iam:PassRole` rather than the full role/policy/instance-profile
lifecycle.

`include/raft/aws_ec2_quorum_manager.hpp` (the production code this bundle
tests) never calls an IAM API at all — `aws_ec2_quorum_manager_config::iam_instance_profile`
is a plain `std::string`, referenced by name on `RunInstances` and passed
straight through with no opinion about how that name came to exist. The
dynamic `CreateRole`/`PutRolePolicy`/`CreateInstanceProfile`/
`AddRoleToInstanceProfile` sequence in
`tests/aws_quorum_manager_real_ec2_test.cpp`'s `create_iam_role()` today
exists purely to give the test *something* to pass in — it is test-fixture
setup, not something the test's own assertions require to happen fresh
per run, and not a validation of `aws_ec2_quorum_manager`'s own behavior
(which has no role/policy-creation behavior to validate in the first
place).

#### Acceptance Criteria

1. A new script, `scripts/ci-cloud-credentials/aws/provision-quorum-test-node-role.sh`,
   SHALL create — once, idempotently, run by an operator with full IAM
   rights, separately from Requirement 4's CI-identity provisioning script
   — a single, well-known-named IAM role (`kythira-aws-quorum-test-node-role`)
   and instance profile (`kythira-aws-quorum-test-node-profile`), with the
   role's own inline policy granting only `sts:GetCallerIdentity` —
   identical in shape and scope to what
   `tests/aws_quorum_manager_real_ec2_test.cpp`'s `create_iam_role()`
   creates today, just created once instead of once per test run.
2. `tests/aws_quorum_manager_real_ec2_test.cpp` SHALL be changed to resolve
   the instance-profile name to attach from the `KYTHIRA_TEST_IAM_INSTANCE_PROFILE`
   environment variable when set, and to the well-known default
   `kythira-aws-quorum-test-node-profile` (Requirement 3.1) when unset —
   in both cases referencing an existing profile by name, never calling
   `iam:CreateRole`/`iam:PutRolePolicy`/`iam:CreateInstanceProfile`/
   `iam:AddRoleToInstanceProfile`, and never tearing one down at the end
   of a run either. This is the one piece of this spec that touches test
   code rather than pure CI/CD infrastructure — necessary because the
   permission simplification in Requirement 2.1/2.3 depends on the test
   itself no longer performing IAM lifecycle calls, not just on the CI
   role being denied them.
3. WHEN the referenced instance profile does not actually exist in the AWS
   account (e.g. an operator enabled the `ec2-quorum-manager` bundle
   without first running Requirement 3.1's script), `RunInstances` SHALL
   fail with AWS's own clear "instance profile not found"-class error —
   this is an acceptable, self-explanatory fail-closed outcome and this
   spec introduces no separate pre-flight check duplicating it.
4. The CI role's `iam:PassRole` statement (Requirement 2.1) SHALL be scoped
   to exactly this one role's ARN (`arn:aws:iam::<account>:role/kythira-aws-quorum-test-node-role`),
   not a wildcard pattern — the CI role can pass this one, deliberately
   near-powerless role and no other, and since it holds no
   `iam:PutRolePolicy` permission on that role (or any role), it cannot
   expand what passing it would grant even if compromised.
5. The CI role's session duration (`--session-duration-seconds` on
   Requirement 4's provisioning script, capped by the role's own
   `MaxSessionDuration`) SHALL be no more than 3600 seconds (one hour) by
   default — real-cloud test runs in this project's existing design
   (`tests/ca_cluster_node_rpc_tls_real_ec2_test.cpp`'s
   `TIMEOUT 1800`-class budgets) comfortably fit within that window with
   margin, and a short session limits the blast radius of a leaked
   short-lived credential far more tightly than IAM user access keys ever
   could (those have no expiry at all without manual rotation) — this
   remains a defense-in-depth measure even though Requirement 3.4's tight
   `iam:PassRole` scoping already removes the escalation path a broader
   IAM footprint would have created.

### Requirement 4: AWS OIDC role provisioning script

**User Story:** As an operator setting up real-AWS CI testing for the
first time (or adding a newly-enabled bundle later), I want a single
script that creates everything CI needs to authenticate — the GitHub OIDC
identity provider if it doesn't already exist, the IAM role with a trust
policy scoped to this exact repository, and a least-privilege permission
policy scoped to the bundles I tell it to enable — so I don't hand-craft
IAM JSON or, worse, generate a long-lived access key and paste it into a
GitHub secret.

#### Acceptance Criteria

1. `scripts/ci-cloud-credentials/aws/provision-oidc-role.sh` SHALL accept,
   at minimum: `--github-org <org>`, `--github-repo <repo>`,
   `--bundles <comma-separated bundle names>`, and SHALL default
   `--role-name` to `kythira-ci-real-cloud-tests` and
   `--session-duration-seconds` to `3600` (one hour) if not given.
2. The script SHALL be idempotent: running it twice with the same
   arguments SHALL succeed both times and leave AWS in the same end
   state (`aws iam get-role`/`get-open-id-connect-provider`-style
   existence checks before every create call, updating in place — e.g.
   `put-role-policy` — where AWS's API is naturally idempotent, rather
   than failing on "already exists").
3. The script SHALL check for an existing IAM OIDC identity provider for
   `token.actions.githubusercontent.com` (`aws iam list-open-id-connect-providers`)
   and create one only if absent — since a single AWS account can only
   have one identity provider per issuer URL, and other repositories or
   projects in the same account may have already created it.
4. The created (or updated) IAM role's trust policy SHALL restrict
   `sts:AssumeRoleWithWebIdentity` to the `token.actions.githubusercontent.com`
   provider with an audience condition of `sts.amazonaws.com` and a
   subject condition scoped to `repo:<github-org>/<github-repo>:*` at
   minimum — the script SHALL support an optional `--ref-restriction`
   flag narrowing the subject condition further (e.g. to
   `repo:<org>/<repo>:ref:refs/heads/main` plus
   `repo:<org>/<repo>:environment:real-cloud-tests`, the latter for
   manually-dispatched runs gated by Requirement 5's GitHub Environment).
5. The script SHALL attach exactly the permission statements corresponding
   to `--bundles` (Requirement 2.4) as an inline or managed policy on the
   role — re-running the script with a different `--bundles` value SHALL
   replace the previous policy content, not merely add to it (so
   *disabling* a bundle and re-running actually revokes that bundle's
   permissions, including its `iam:PassRole` statement per Requirement 2.4).
6. The script SHALL print the created role's ARN on success and SHALL NOT
   print, log, or persist any credential material — the role ARN is not a
   secret (the trust policy is what protects it) and is meant to be
   copy-pasted into a repository variable (`AWS_CI_ROLE_ARN`), not a
   repository secret.
7. The script SHALL be plain bash using the AWS CLI (`aws iam ...`,
   `aws sts get-caller-identity`), matching this project's existing
   `scripts/*.sh` convention (`scripts/pre-commit-coverage.sh`,
   `scripts/install-hooks.sh`) — no new language runtime or dependency.
   This applies equally to Requirement 3.1's node-role provisioning
   script.

### Requirement 5: Workflow structure and scheduling

**User Story:** As a maintainer, I want real-cloud tests to run on a
predictable schedule and on demand, decoupled from the existing per-push/
per-PR CI pipeline, so that every ordinary commit's CI run stays fast and
free, and a real-cloud run is always a deliberate, reviewable action.

#### Acceptance Criteria

1. A new workflow file, `.github/workflows/real-cloud-tests.yml`, SHALL be
   added — `.github/workflows/ci.yml` SHALL NOT be modified to add
   real-cloud test execution; its existing `-LE` label filter (already
   excluding `real-ec2`-labeled tests via the `slow` label) SHALL continue
   to guarantee ordinary push/PR runs never attempt a real-cloud test.
2. The new workflow SHALL trigger on `workflow_dispatch` (manual,
   Requirement 1.4's inputs) and on a `schedule` (a weekly cadence by
   default, e.g. `cron: '0 6 * * 1'` — Monday mornings UTC — chosen to
   balance "catches real-infrastructure drift before it compounds" against
   "real-EC2 spend is not incurred needlessly"; the exact cadence SHALL be
   easily adjustable by editing one `cron` line).
3. WHEN triggered by `schedule`, the workflow SHALL use only repository
   variables (Requirement 1) for every toggle — a scheduled run performs
   no manual input resolution.
4. The workflow SHALL use a GitHub Environment named `real-cloud-tests`
   for every provider's job (`environment: real-cloud-tests`), with
   `id-token: write` permission scoped to that job — this both satisfies
   OIDC's requirement that the token-requesting job declare
   `permissions: id-token: write` and gives the repository owner the
   option (configured in GitHub's own repository settings, not in this
   workflow file) to require manual approval before a real-cloud job runs,
   independent of this spec's own on/off toggles.
5. Each enabled provider's job SHALL run independently (not serialized
   behind another provider's job) so that, for example, AWS real-cloud
   tests are unaffected by a hypothetical future Azure job's failure or
   slowness.
6. Within the AWS job, each enabled bundle's test binary SHALL run via a
   `ctest -L` invocation selecting exactly that bundle's labels
   (Requirement 2.5) — bundles SHALL run sequentially within the job (not
   parallel matrix legs), matching this project's own precedent of
   avoiding concurrent real-EC2 test execution contention
   (`ca_cluster_node_test`'s `PROCESSORS 4`/no-co-scheduling rationale,
   `scripts/pre-commit-coverage.sh`), since real-EC2 tests already budget
   many minutes each and total real-cloud spend is not meaningfully
   affected by sequential-vs-parallel execution.
7. The workflow SHALL reuse `tests/aws_real_ec2_test_support.hpp`'s
   existing cost-tracking and signal-driven-cleanup apparatus unmodified —
   this spec adds no new cost-reporting or cleanup mechanism, since the
   real-EC2 test binaries themselves already emit a per-run cost estimate
   and already tear down every AWS resource they create (including on a
   killed/canceled workflow run, via that apparatus's signal handlers) —
   GitHub Actions' own SIGTERM-on-cancel behavior is exactly the signal
   class those handlers already cover.

### Requirement 6: Per-cloud-provider setup documentation

**User Story:** As an operator setting up this feature for the first time,
I want a clear, provider-specific document walking through prerequisites,
running the provisioning script, configuring the resulting repository
variables, and tearing the feature down — so I don't have to reverse-
engineer the workflow YAML or the provisioning script's source to figure
out what to actually do.

#### Acceptance Criteria

1. `scripts/ci-cloud-credentials/README.md` SHALL be the top-level entry
   point: the three-level toggle model (Requirement 1), the service-bundle
   concept (Requirement 2), and a table linking to each provider's own
   subdirectory — with AWS's row linking to a real document and every
   other provider's row stating "not yet implemented; see `doc/TODO.md`
   Cloud Provider Support" rather than linking to a stub.
2. `scripts/ci-cloud-credentials/aws/README.md` SHALL document, at
   minimum: prerequisites (an AWS account, `aws` CLI configured with
   sufficient IAM-admin permissions to run the provisioning script once,
   which repository variables to set and how to find their values after
   running the script), the exact provisioning script invocation for a
   first-time setup and for adding/removing a bundle later, how to verify
   the setup worked (a dry-run or a manual `workflow_dispatch` with only
   the cheapest bundle enabled), the AWS-side cost this incurs per run (a
   worked estimate, in the style of `doc/aws_acm_pca_test_cost_estimate.md`'s
   existing precedent — real EC2 instances/EBS/data transfer for the
   duration of one test run), and how to tear the feature down (which IAM
   resources to delete, and that deleting them is safe at any time since
   the workflow will simply fail closed — Requirement 7 — rather than
   degrade unsafely).
3. Both documents SHALL be added under `scripts/ci-cloud-credentials/`,
   colocated with the scripts they document — matching this project's
   existing `docker/<feature>/README.md` convention of documentation
   living next to the artifact it describes, not in a separate top-level
   `doc/` entry (`doc/aws_acm_pca_test_cost_estimate.md` is a narrower,
   single-topic exception to that convention already existing in this
   project; this spec's broader, multi-provider "how do I set this up"
   documentation is a better fit for the `docker/`-style pattern).

### Requirement 7: Fail-closed behavior

**User Story:** As a maintainer, I want a misconfigured or partially-set-up
real-cloud-tests feature to fail loudly and safely rather than silently
skip coverage or silently run with more access than intended, so a broken
setup is caught immediately rather than discovered much later as a gap.

#### Acceptance Criteria

1. WHEN a provider is enabled (Requirement 1.2) but the corresponding
   repository variable holding its CI role ARN (e.g. `AWS_CI_ROLE_ARN`)
   is unset, that provider's job SHALL fail with a clear error message
   naming the missing variable — it SHALL NOT silently skip.
2. WHEN a bundle is enabled (Requirement 1.3) but its corresponding test
   binary is not present in the build (e.g. `LIBSSH2_FOUND` was false at
   configure time), the job SHALL fail with a clear error message, not
   silently report success with zero tests run.
3. WHEN OIDC credential exchange itself fails (e.g. the trust policy
   doesn't match, or the role has been deleted), the job SHALL fail with
   whatever error `aws-actions/configure-aws-credentials` (or the
   equivalent future per-provider action) surfaces — no fallback to any
   other credential source (e.g. an ambient AWS profile on the runner)
   SHALL be attempted, since GitHub-hosted runners have no such ambient
   credentials by design and a fallback would defeat the purpose of using
   OIDC in the first place.
4. A test failure within an enabled bundle SHALL fail that provider's job
   (matching every other CTest invocation in this project's CI) — it
   SHALL NOT be treated as advisory/non-blocking, since a real-cloud test
   failure is exactly the class of regression (see
   `.kiro/specs/ca-cluster-rpc-mtls-real-aws/`'s own motivating incident)
   this workflow exists to catch.
