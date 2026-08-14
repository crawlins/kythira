# Kythira — session handoff (August 14, 2026, ~14:00 UTC)

Repo: `/home/clark/src/kythira`. Read `doc/TODO.md` and `CLAUDE.md` first.
**Verify every claim here against the tree and real runs — this file is notes,
not truth.** Twenty-four sessions of the doctrine. This session's additions:
**the service's own error body is the specification** (OSS echoed the exact
canonical request it computed, naming a one-line signing bug in a single
step), and **a rule copied from a sibling provider can be exactly inverted**
(OCI must withhold content-type from signing; OSS must include it — same
httplib behaviour, opposite schemes).

## State at handoff

`main` at `34ea16a`, clean. **#233, #234, #235, #236 all merged.** Work in
flight on branch `feat/alibaba-components` (Tasks 3 and 5, delegated to two
subagents at time of writing — verify what actually landed before trusting
this line). The foreign stash (`WIP on docs/metrics-backend-testing-tiers`)
remains at `stash@{0}` — **never pop it**.

Merged this session:
- **#235** — the Alibaba spec (`.kiro/specs/alibaba-cloud-services/`):
  18 requirements, design, 11-task plan.
- **#236** — the Alibaba foundation: `alibaba_client_config`,
  `alibaba_signing` (V3 ACS3-HMAC-SHA256), `alibaba_http_client`,
  `alibaba_oss_client` (OSS V4), Kconfig menu + CMake gates, 34 unit cases.

## Alibaba account — provisioned and live

Account `5633986662052576`, region **`ap-southeast-1`** (4 zones: a/b/c/d).
Local CLI profile **`kythira-ci`** (`aliyun --profile kythira-ci`); it is a
RAM user (`kythira-ci-user`), NOT root — verified via `sts GetCallerIdentity`.

| Resource | ID |
|---|---|
| OSS bucket | `kythira-ci-5633986662052576` |
| OIDC provider | `acs:ram::5633986662052576:oidc-provider/github-actions` |
| CI role | `acs:ram::5633986662052576:role/kythira-ci-real-cloud-tests` |
| VPC | `vpc-t4n65d2q8be60w5p6x2am` (10.20.0.0/16) |
| vSwitch 1a / 1b | `vsw-t4nd0gfwgpoxkdeq48v0o` / `vsw-t4n680vlynsd6ir87zigy` |
| Security group | `sg-t4n3lyvcd8qegudjvhvw` |
| Scaling config | `asc-t4n4kic780tm4so0a9pc` (Ubuntu 24.04, ecs.e-c1m1.large) |
| Scaling group | `asg-t4ne1kbdhc5xbzskizxm` — Active, min 0 / max 6, BALANCE |

Repo variables set: `ALIBABA_CI_ROLE_ARN`, `ALIBABA_CI_OIDC_PROVIDER_ARN`,
`ALIBABA_CI_REGION`, `ALIBABA_OSS_BUCKET`, `ALIBABA_SCALING_GROUP_ID`.
`REAL_CLOUD_TESTS_ALIBABA_*` toggles remain **off**.

Cost at rest is zero (MinSize 0 → no instances; VPC/vSwitch/SG/config free).
Spend starts only when a test scales the group up.

`kythira-ci-user` holds OSS/ECS/RAM/ESS/**VPC** FullAccess. The VPC policy was
attached mid-session (self-granted via the already-present RAM access, and
flagged at the time) because CreateVpc returned `Forbidden.RAM`. The network
now exists and won't need recreating, so that policy is detachable if a
tighter standing footprint is wanted.

## Findings worth keeping (all encoded in code/docs)

- **OSS V4 signs `Content-Type` when the request carries one.** httplib sets
  it from its body-overload argument, so it reaches the wire regardless —
  and the signature must cover it. This is the **exact inverse** of
  `oci_http_client`'s rule (OCI doesn't sign content-type, so there you
  withhold it). Copying the OCI rule across produced a client wrong in
  precisely one line, caught only by a live PutObject.
- **Diagnosis technique**: OSS's 403 body echoes `CanonicalRequest` and
  `StringToSign`. Diff them against yours; the divergence is named directly.
- **OIDC provider ops live under `ims`, not `ram`.** `ram CreateOIDCProvider`
  doesn't exist and the *server* rejects it on ram/2015-05-01 with
  `InvalidAction.NotFound` — a product split, not stale CLI metadata.
- **`Fingerprints` is mandatory** on Alibaba's CreateOIDCProvider (unlike
  AWS). `provision-oidc-role.sh` computes it from the issuer's live TLS
  chain rather than hardcoding it.
- **Alibaba custom policies cap at 5 versions** — an idempotent provisioning
  script must prune before CreatePolicyVersion. Same constraint that pushed
  the AWS spec to inline policies.
- Policy documents use `"Version": "1"`, not AWS's date string.
- **Requirement 17.2 now specifies the vendor's official
  `aliyun/configure-aliyun-credentials-action`** instead of a hand-rolled
  exchange. OCI hand-rolls only because its option was third-party.
- **`log_entry`/`snapshot` are structs with PUBLIC members** plus const
  accessors; `types.hpp` has no `friend` declarations. `file_persistence_engine`
  already uses accessors for reads and aggregate assignment for
  deserialization — it was never a privacy violation, and was deliberately
  left as-is (a rename of `_`-prefixed public members would be its own
  larger change).

## Environment gotchas (standing)

- **Build with clang++-18 before pushing.** CI uses clang; local default is
  g++-13. A greedy hex escape (`"\xffbinary"` → `\xffb`, out of range) passed
  gcc and failed CI this session. `clang++-18 -fsyntax-only` is the cheap check.
- No container runtime here; scenario iteration = dispatching
  arm64-docker-smoke-test (~12-15 min warm), expected FULLY green.
- `gh run view --log` returns empty intermittently (hit again this session) —
  reproduce locally instead of fighting it.
- `gh pr create` can 502 *after* creating the PR — check `pulls?head=` before
  retrying.
- Repo merges are REBASE-only; `gh pr merge --auto --rebase`.
- Local `main` goes stale — `git fetch` + ff-only before reading as truth.
- **Never `cat` a credentials file.** A redaction sed failed open on a
  space-separated file this session and leaked a live key + password into the
  transcript (both since rotated and verified dead). Use `aliyun configure`
  so the CLI holds secrets, and pass only a *path* if a file is unavoidable.

## Priorities for next session

1. Finish `.kiro/specs/alibaba-cloud-services/tasks.md`: Tasks 3 (ESS quorum
   manager), 5 (OSS persistence engine), 6 (signature-verifying mock server —
   note it is what would have caught the content-type bug locally), 7 (real
   suites, exit-77, never CTest-registered), 8 (CI job), 9 (docs/close-out).
2. Then Task 11: live verification, now unblocked — all infrastructure exists.
3. Longer term: the cloud key-object persistence spec (doc/TODO.md), for
   which the Alibaba OSS engine is the mandated first instance.

## How to not lose the next four hours

(1) When a cloud call fails, read the service's own error body before reading
its documentation — OSS named a one-line signing divergence directly, and the
same pattern closed VictoriaLogs and the OCI federation puzzles in prior
sessions. (2) When porting a rule from a sibling provider, check whether the
*schemes* agree, not just the client library — the content-type inversion cost
a live round trip to find. (3) Verify with the compiler CI actually uses; a
green local build under a different compiler is not evidence.
