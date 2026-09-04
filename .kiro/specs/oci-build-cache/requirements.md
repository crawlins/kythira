# Requirements Document — OCI-Hosted Build Cache (vcpkg binary cache + sccache)

## Introduction

This document specifies moving this repository's two CI build caches — the
vcpkg **binary cache** (one archive per built port) and the **compiler
cache** for this project's own translation units (ccache today) — out of the
GitHub Actions cache and into one OCI Object Storage bucket, reached through
OCI's Amazon S3 Compatibility API by vcpkg's `x-aws` backend and by
[sccache](https://github.com/mozilla/sccache)'s `s3` backend respectively.

This is not a speculative optimization. The motivating measurement is already
in the tree, in `scripts/prune-actions-caches.sh`'s header:

> This repository sits at GitHub's 10 GB per-repository cache ceiling
> (measured 10.37 GB across 41 entries), so GitHub is continuously evicting
> entries LRU and the victim is arbitrary — a job that should have hit a warm
> ccache starts cold instead.

and the consequence is visible in every recent run. `.kiro/specs/ccache-adoption/`
measured a `build-and-test` leg at 29 m 07 s cold and 11 m 59 s warm. On
September 4, 2026, run 33834811218's four `build-and-test` legs built in
17 to 26 minutes, its "Restore ccache" steps took 1 to 3 seconds and its "Save
ccache" steps 2 to 12 seconds — the timings of an empty cache, not a 2 GB
one. The `Full suite (stdexec future backend)` leg, whose memory-pressure
failures `.github/workflows/ci.yml`'s Build step documents at length, is
triggered by exactly this: its own comment says the failures happen when
"how much real compilation happens at once when the cache is cold", and the
cache is cold every run. Run 33714319093's `gcp-sdk-build` job spent 2 h 33 m
rebuilding vcpkg ports after a binary-cache miss that a persistent cache
would not have had.

The vcpkg archives are the largest occupants of the Actions cache (4.49 GB of
the 10.37 GB, per the same header) and are deliberately never pruned, because
each is the only copy of an expensive build. Eight `ccache` families at 2 GB
each cannot fit beside them. No pruning policy fixes a cache that is
over-subscribed by design; a second home does.

`doc/sccache_dogfood_cost_estimate.md` pre-registers the cost of the
plain-object-storage version of this at **about $0.50 per month** on OCI: the
estimated 1 to 5 TB per month of runner traffic sits inside OCI's 10 TB free
egress allowance, ten gigabytes of standard storage is $0.26, and roughly
700,000 requests beyond the free 50,000 are about $0.24. The same document
records why AWS, Azure and GCP were not chosen: their internet egress alone
would be $100 to $450 per month for the same bytes.

### Why one bucket, two caches

- **The vcpkg move is the one that relieves the Actions cache.** Removing the
  4.49 GB of archives leaves the `vcpkg_installed/` tree caches and nothing
  else competing for 10 GB, so even a job that keeps `actions/cache` for its
  whole tree stops losing it to eviction.
- **The compiler-cache move is the one that makes warm builds real.** ccache
  cannot be pointed at object storage; sccache can, and its `s3` backend is
  the same wire protocol vcpkg's `x-aws` backend uses. One bucket, one set of
  credentials, one lifecycle policy, one cost line.
- **sccache also covers rustc**, which ccache cannot, so the `lakers` port's
  `cargo build` becomes cacheable for the first time. That is a small win on
  its own (`.kiro/specs/redis-compatible-kv/requirements.md` records it as
  the gap that motivated the Redis gateway) and comes for free here.

### What this spec does not cover

- **Dogfooding the Redis gateway** (`feat/redis-compatible-kv`,
  `.kiro/specs/redis-compatible-kv/`). sccache's `redis` backend against a
  Kythira cluster is a separate deployment with its own cost estimate
  (`doc/sccache_dogfood_cost_estimate.md`, about $55 per month) and its own
  reasons. This spec is the $0.50-per-month baseline that estimate is measured
  against, and nothing in it precludes pointing sccache at the gateway later:
  the launcher, the CI wiring and the read-only-on-PRs discipline carry over
  unchanged; only `SCCACHE_*` variables differ.
- **Self-hosted runners.** The runners stay GitHub-hosted; the cache moves,
  not the compute.
- **Replacing ccache for local developers.** ccache stays the default local
  launcher; sccache is added as an alternative, not imposed.

## Glossary

- **vcpkg binary cache**: vcpkg's per-port archive store. Each entry is one
  built port, keyed by vcpkg's ABI hash (port version, triplet, compiler,
  features, patches, and the hashes of everything the port depends on). A
  hit skips the port's whole build; a miss builds it from source and, with
  `readwrite`, uploads the result. Backends: `files`, `nuget`, `http`,
  `x-aws`, `x-gcs`, `x-azblob`, `x-cos`, `x-gha`, `x-script` (fetch-only).
- **`x-gha`**: the backend this repository uses today, storing archives in
  the GitHub Actions cache through `ACTIONS_CACHE_URL` and
  `ACTIONS_RUNTIME_TOKEN`, which `ci.yml` re-exports for `run:` steps.
- **`x-aws`**: the backend that shells out to the AWS CLI (`aws s3 cp`). It
  reaches any S3-compatible endpoint the CLI can: the CLI honours
  `AWS_ENDPOINT_URL`, and OCI's compatibility endpoint is
  `https://<namespace>.compat.objectstorage.<region>.oraclecloud.com`.
- **`vcpkg_installed/` tree cache**: the *other* vcpkg cache, an
  `actions/cache` entry holding the whole installed tree, keyed by
  `hashFiles('vcpkg.json', 'vcpkg-overlays/**')`. A hit skips the vcpkg
  invocation entirely; it is the L1 in front of the binary cache's L2. This
  spec keeps it.
- **sccache**: a compiler cache with pluggable remote storage. Wrapped in
  front of the compiler via `CMAKE_C/CXX_COMPILER_LAUNCHER` (and in front of
  rustc via `RUSTC_WRAPPER`), it hashes the preprocessed translation unit and
  the invocation, and reads or writes the object through its backend. Unlike
  ccache it has no direct mode: every lookup preprocesses. Unlike ccache it
  has an `s3` backend and a rustc backend.
- **Customer secret key**: OCI's name for the S3-style access-key/secret-key
  pair an IAM user holds for the compatibility API. Static credentials; the
  keyless Workload Identity Federation the `oci` job in
  `real-cloud-tests.yml` uses does not produce them.
- **PAR (pre-authenticated request)**: an OCI URL that grants unauthenticated
  `GET`/`PUT` under a bucket prefix for a fixed period. vcpkg's `http`
  backend can use one; sccache cannot. Named here as the fallback if the
  `x-aws` path is ever unavailable on a runner, not as the design.
- **Cache poisoning**: writing a wrong object under a key that other builds
  will trust. For sccache the key is content-derived, so a poisoned entry is
  a wrong object file for a correct source; for vcpkg the ABI hash is not
  verified against the archive's contents, so a poisoned archive is a wrong
  library. The only writer this spec allows is a push to `main`.
- **Leg**: one job or matrix entry in `ci.yml` that compiles this project's
  own code. Eight restore a compiler cache today: the four `build-and-test`
  entries (g++-13 and clang++-18, x64 and arm64), `gcp-sdk-build`,
  `coverage`, `tsan`, and `future-backend-compat`'s two backends.

## Requirements

### Requirement 1: Measure before wiring

**User Story:** As the maintainer who has already had one compiler cache
provide 0% benefit for five days because a directory did not match, I want
the numbers this spec promises taken on a throwaway branch before a line of
permanent wiring exists, so that the acceptance criteria below are calibrated
against what sccache and `x-aws` actually do to this tree.

#### Acceptance Criteria

1. A throwaway PR SHALL run every leg in Requirement 5.1's table with sccache
   in place of ccache and SHALL record, per leg, from `sccache --show-stats`:
   compile requests, cacheable requests, non-cacheable requests with the
   reason sccache gives, cache hits, cache misses, and cache read/write
   bytes, on a cold run and on the warm run immediately after it.
2. The throwaway PR SHALL run at least one job with vcpkg's `x-aws` backend
   against the bucket, first with an empty bucket and then with a populated
   one, and SHALL record the port count uploaded, the bytes uploaded, and
   the wall clock of the "Bootstrap vcpkg and install dependencies" step on
   each.
3. The precompiled header `kythira_test_pch` (`target_precompile_headers`)
   SHALL be called out specifically in the measurement: sccache's PCH support
   differs from ccache's, and a non-cacheable count equal to the number of
   PCH users would mean the compiler cache covers nothing that matters.
   Whether the coverage leg's `-fprofile-instr-generate` objects are
   cacheable SHALL be recorded the same way.
4. The measurement SHALL be committed to `tasks.md` under Task 1 with the
   run ids, before any task after it is started, and the wall-clock and
   hit-rate thresholds in Requirement 6 SHALL be re-derived from it if they
   disagree with the estimates written here.
5. The throwaway PR SHALL be closed without merging, in the pattern of
   PR #49.

### Requirement 2: One bucket, provisioned by script, in the existing tenancy

**User Story:** As the operator of the OCI tenancy this repository already
tests against, I want the cache bucket created and audited by the same kind
of script as `scripts/perf-cloud/`, so that a leaked or wrongly-sized
resource is caught by a check, not a bill.

#### Acceptance Criteria

1. One bucket SHALL hold both caches, in the compartment named by the
   `OCI_CI_COMPARTMENT_ID` repository variable and the region named by
   `OCI_CI_REGION`, with the vcpkg archives under `vcpkg/<triplet>/` and the
   sccache objects under `sccache/`. Standard tier; no versioning; no
   replication.
2. The bucket SHALL carry two lifecycle rules: delete `sccache/` objects
   unmodified for 30 days and `vcpkg/` objects unmodified for 90 days. The
   asymmetry is deliberate: a vcpkg archive is minutes of build per object
   and there are a few hundred; an sccache object is seconds and there are
   tens of thousands.
3. Two IAM users in their own group SHALL exist for this bucket and nothing
   else: `kythira-build-cache-rw`, whose policy allows `OBJECT_READ`,
   `OBJECT_CREATE`, `OBJECT_OVERWRITE` and `OBJECT_INSPECT` on this bucket
   only, and `kythira-build-cache-ro`, whose policy allows `OBJECT_READ` and
   `OBJECT_INSPECT` on this bucket only. Each SHALL hold exactly one customer
   secret key.
4. The bucket SHALL NOT be public. Reads are authenticated with the read-only
   key, so that the read path is the same code path as the write path and a
   public-ACL mistake cannot later turn into a write path.
5. `scripts/oci-build-cache/provision.sh` SHALL create all of the above
   idempotently through the OCI CLI, default to a dry run that prints what
   it would create, and print the customer secret keys once, to stdout, with
   instructions for the repository secrets in Requirement 4. It SHALL NOT
   write them to any file.
6. `scripts/oci-build-cache/audit.sh` SHALL list the bucket's object count
   and total bytes per prefix, the lifecycle rules, the two users' keys, and
   the tenancy's month-to-date egress for the bucket, and SHALL exit
   non-zero if any resource exists in the compartment with this spec's tag
   that this spec does not name, in the pattern of
   `scripts/perf-cloud/audit-aws-leaks.sh`. It SHALL be exercised in the
   failing direction once before it is trusted (`.kiro/specs/multi-machine-placement/`
   task 1 is the precedent).

### Requirement 3: vcpkg binary cache moves to the bucket

**User Story:** As a CI job whose dependency tree is Folly, Boost, OpenSSL,
gRPC, Proxygen and two cloud SDKs, I want a binary-cache miss to cost one
download rather than forty minutes of building, on every run and not only on
the runs the Actions cache happens to have kept.

#### Acceptance Criteria

1. Every workflow that sets `VCPKG_BINARY_SOURCES` — the six jobs in
   `ci.yml`, `arm64-docker-smoke-test.yml`, and the `aws` and `oci` jobs in
   `real-cloud-tests.yml` — SHALL set it to
   `clear;x-aws,s3://<bucket>/vcpkg/<triplet>/,<mode>` where `<mode>` is
   Requirement 4's read-only or read-write selection.
2. The AWS CLI on the runner SHALL be pointed at the OCI compatibility
   endpoint through `AWS_ENDPOINT_URL`, `AWS_REGION` set to the OCI region,
   and the customer secret key in `AWS_ACCESS_KEY_ID`/`AWS_SECRET_ACCESS_KEY`,
   all exported by one shared step (Requirement 4.5) so that no job carries
   its own copy.
3. `x-gha` SHALL be removed from every `VCPKG_BINARY_SOURCES`, together with
   the "Export Actions cache credentials for vcpkg" step that exists only to
   serve it. The `vcpkg_installed/` tree caches (`actions/cache`, keyed on
   `hashFiles`) SHALL be kept as the L1.
4. `scripts/prune-actions-caches.sh` and its header SHALL be updated: the
   "vcpkg caches are never touched" rule stays for the tree caches, and the
   header's accounting of what occupies the Actions cache SHALL be
   re-measured after this move and rewritten with the new figures.
5. WHEN the bucket is unreachable or the credentials are absent THEN vcpkg
   SHALL build the affected ports from source and the job SHALL still
   succeed; a cache failure is a warning in the log, never a red job. This
   SHALL be verified once by running a job with a deliberately wrong
   endpoint (Requirement 7.3).
6. The triplets SHALL be kept apart by prefix (`x64-linux`, `arm64-linux`),
   mirroring the `runner.arch` component of the tree-cache keys and for the
   same reason `ci.yml` gives there.

### Requirement 4: Writers are pushes to `main`; everything else reads

**User Story:** As the person who will trust these caches, I want the only
thing that can write a cache entry other builds consume to be a commit that
is already on `main`, so that a pull request cannot poison what `main`
builds with.

#### Acceptance Criteria

1. Jobs triggered by `push` to `main` SHALL use the `kythira-build-cache-rw`
   key and `readwrite` for vcpkg and normal read-write mode for sccache.
2. Jobs triggered by `pull_request` SHALL use the `kythira-build-cache-ro`
   key and `read` for vcpkg, and SHALL set `SCCACHE_S3_NO_CREDENTIALS`
   unset but run sccache against the read-only key, so that a write attempt
   is refused by IAM and counted by sccache rather than succeeding.
3. WHEN neither key is available (a pull request from a fork, where GitHub
   withholds secrets) THEN both caches SHALL be disabled for that run and
   the build SHALL proceed without them. The mechanism is Requirement 5.4:
   sccache's server is started explicitly and the launcher exported only if
   that succeeded; vcpkg's `x-aws` failure is a warning.
4. The trade-off SHALL be recorded in `design.md` and accepted: a pull
   request that changes `vcpkg.json` or an overlay port builds its new ports
   from source on every one of its runs until it merges, exactly as such a
   PR does today whenever the Actions cache has evicted its entry. That is
   the cost of not letting a PR write, and it is bounded by the tree-cache
   L1, which PR runs still write.
5. One composite action, `.github/actions/oci-build-cache/`, SHALL perform
   the event-to-key selection and the environment export for both caches,
   so the policy lives in one file. It SHALL take the repository secrets
   `OCI_BUILD_CACHE_RW_ACCESS_KEY_ID`, `OCI_BUILD_CACHE_RW_SECRET_ACCESS_KEY`,
   `OCI_BUILD_CACHE_RO_ACCESS_KEY_ID`, `OCI_BUILD_CACHE_RO_SECRET_ACCESS_KEY`
   and the repository variables `OCI_BUILD_CACHE_BUCKET`,
   `OCI_BUILD_CACHE_NAMESPACE` and `OCI_CI_REGION`, and SHALL fail the job
   with a `::error::` naming the missing variable if a *variable* is unset,
   while treating missing *secrets* as Requirement 4.3's no-credentials case.

### Requirement 5: sccache replaces ccache in CI, and joins it locally

**User Story:** As a CI leg, I want my compiler cache to survive the runner
being destroyed, which the Actions cache has not been able to promise; as a
local developer, I want nothing to change unless I ask for it.

#### Acceptance Criteria

1. The following legs SHALL use sccache as their compiler launcher, each
   with `SCCACHE_S3_KEY_PREFIX=sccache/` and no per-leg prefix, since
   sccache's key already includes the compiler and every flag:
   `build-and-test` (four entries), `gcp-sdk-build`, `coverage`, `tsan`,
   `future-backend-compat` (both backends), and `real-cloud-tests.yml`'s
   `aws` and `oci` jobs. Any leg whose Requirement 1.1 measurement shows
   fewer than 50% of its compile requests cacheable SHALL instead keep ccache
   with its existing `actions/cache` persistence, and the reason SHALL be
   written beside its Configure step.
2. The root `CMakeLists.txt`'s `KYTHIRA_ENABLE_CCACHE` option SHALL become a
   `KYTHIRA_COMPILER_LAUNCHER` cache variable taking `auto`, `ccache`,
   `sccache` or `none`, default `auto`. `auto` SHALL prefer ccache if found,
   then sccache if found, then none — so a developer's machine behaves as it
   does today. An explicit value that names a launcher not on `PATH` SHALL
   be a configure error, not a silent fallback (the ccache spec's
   `CCACHE_DIR` incident is the reason: a launcher that quietly does nothing
   is worse than one that refuses). `KYTHIRA_ENABLE_CCACHE=OFF` SHALL keep
   working as an alias for `none` for one release, with a deprecation
   message.
3. CI SHALL install a pinned sccache release binary by version and SHA-256
   (there is no apt package), into the same "Install system dependencies"
   step family, for both `X64` and `ARM64` runners.
4. Each leg SHALL start sccache explicitly (`sccache --start-server`) after
   the composite action has exported the environment, and SHALL export
   `CMAKE_C_COMPILER_LAUNCHER`/`CMAKE_CXX_COMPILER_LAUNCHER` (through
   `-DKYTHIRA_COMPILER_LAUNCHER=sccache`) only if that succeeded. The pattern
   is `docker/sccache_runner/run.sh` on `feat/redis-compatible-kv`: an
   unreachable cache is never a build dependency.
5. Each leg SHALL run `sccache --show-stats` after its build step, with
   `if: always()`, and SHALL write the compile/cacheable/hit/miss counts and
   the read/write bytes to the job summary. These are the numbers
   Requirement 6 is checked against and the ones
   `doc/sccache_dogfood_cost_estimate.md` says to falsify first.
6. The "Restore ccache", "ccache size limit" and "Save ccache" steps SHALL be
   removed from every leg that moved, along with `CCACHE_DIR` from its `env`.
7. `RUSTC_WRAPPER=sccache` SHALL be exported alongside the C++ launcher, and
   `VCPKG_KEEP_ENV_VARS` SHALL pass `RUSTC_WRAPPER`, `SCCACHE_*` and `AWS_*`
   through to port builds, so the `lakers` port's `cargo build` is cached on
   a binary-cache miss. This is the rustc coverage the Redis-gateway spec
   named as ccache's gap.

### Requirement 6: Measured effect, with the numbers to beat written down

**User Story:** As the reviewer of the PR that lands this, I want a pass/fail
that is a number, not "faster".

#### Acceptance Criteria

1. On the second consecutive `push` run to `main` after the wiring lands
   (the first populates), each `build-and-test` leg's Build step SHALL
   complete in at most 15 minutes, against 17 to 26 minutes cold today and
   the ccache spec's 11 m 59 s warm figure. If Requirement 1's measurement
   shows sccache's preprocess-every-lookup costs more than ccache's direct
   mode did, this threshold SHALL be re-derived from that measurement, not
   relaxed by hand.
2. On the same run, `Full suite (stdexec future backend)`'s Build step SHALL
   complete in at most 45 minutes, against 1 h 51 m to 2 h 09 m cold today,
   and its "Report headroom after build" step SHALL show swap use under
   4 GiB.
3. On the same run, every leg's sccache hit rate over cacheable requests
   SHALL be at least 90%, and the `Bootstrap vcpkg and install dependencies`
   step, when it runs at all, SHALL show zero ports built from source.
4. A `pull_request` run against an unchanged `vcpkg.json` SHALL show the
   same hit rates as 6.3, demonstrating that read-only access reads.
5. The month's OCI bill for the bucket SHALL be under $5 for the first three
   months, against the $0.50 pre-registered, and the audit script's egress
   figure SHALL be under 5 TB per month. Exceeding either SHALL be treated
   as a falsified estimate and investigated, not absorbed.

### Requirement 7: Verify the real wiring, not just the plan

**User Story:** As the maintainer who found that ccache had cached to a
directory nothing restored, I want every claim in this spec checked against
a real run before the task that makes it is marked done.

#### Acceptance Criteria

1. Every task in `tasks.md` SHALL cite the run id and the job whose log or
   summary demonstrates it, in the manner of `.kiro/specs/ccache-adoption/tasks.md`.
2. Requirement 4's selection SHALL be verified in all three states with real
   runs: a `push` to `main` writes (bucket object count rises), a
   `pull_request` from a branch in this repository reads and cannot write
   (sccache reports write errors, bucket count does not rise), and a run
   with the secrets deliberately withheld builds green with both caches
   disabled.
3. Requirement 3.5 SHALL be verified with a run whose `AWS_ENDPOINT_URL`
   points at a non-existent host: the job goes green and the log shows the
   ports built from source.
4. The audit script SHALL be run after the first month and its output
   committed to `tasks.md` beside the pre-registered figures.

### Requirement 8: Documentation

#### Acceptance Criteria

1. `DEPENDENCIES.md` SHALL gain an sccache entry beside the ccache one,
   including how a developer points a local build at the bucket read-only
   (the `ro` key, the `AWS_ENDPOINT_URL`, and `-DKYTHIRA_COMPILER_LAUNCHER=sccache`),
   and the note that a local build never needs it.
2. `doc/ci_build_cache.md` SHALL describe the two caches, the bucket layout,
   the lifecycle rules, the writer policy and its trade-off, how to rotate a
   key, how to read the job-summary statistics, and how to run the audit.
3. `doc/sccache_dogfood_cost_estimate.md` SHALL gain a one-paragraph
   cross-reference recording that this spec is the baseline it compares
   against, with the measured monthly cost once Requirement 7.4 has it.
4. `doc/TODO.md`'s "Not Started" table SHALL carry this spec from the day
   its documents are written, so that it cannot drift the way that table
   documents other specs drifting.

### Requirement 9: Explicit non-goals

#### Acceptance Criteria

1. This spec SHALL NOT change what any leg builds, tests, or asserts. A leg
   with sccache and a leg without it produce the same targets and the same
   test results; the optional-dependency-isolation check from the ccache
   spec (diff the target lists with the launcher on and off) SHALL be run
   once and recorded.
2. This spec SHALL NOT introduce sccache's distributed compilation
   (`sccache-dist`), its local disk cache tiering, or any second storage
   backend.
3. This spec SHALL NOT touch `Full suite`'s `-j3`, the `heavy_tu` job pool,
   or the swapfile. Those bound a cold build's memory; this spec makes cold
   builds rare. Both are needed.
4. This spec SHALL NOT make sccache or the bucket a build dependency
   anywhere: every job that ran green before this spec SHALL run green with
   the bucket deleted.
