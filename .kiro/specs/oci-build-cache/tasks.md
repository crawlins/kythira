# Implementation Plan — OCI-Hosted Build Cache

## Status: 2/12 tasks complete

**Last Updated**: September 6, 2026. Task 2 is done: **the bucket, the IAM and
the credentials exist**, and `audit.sh` reads them back clean. Task 4 (the
CMake launcher) is done and verified locally; it is the only task in the graph
with no edge into it, and it needed neither the bucket nor a CI run.

With Task 2 applied, Tasks 1, 3, 6 and 7 are unblocked: the bucket Task 1's
measurement needs now exists, and so do the repository variables and secrets
the composite action reads.

Tasks 2, 3 and 5 have their **artifacts written and exercised as far as a
machine with no OCI tenancy can exercise them** — the provisioning and audit
scripts with their two-direction test, the composite action with its selection
logic run once per event state, and the documentation — and each records below
exactly what is still owed. They stay unchecked because what is owed is the
half that touches the tenancy and CI.

**Nothing is wired into a workflow.** The composite action exists and no job
references it; Tasks 6 and 7 are where that changes, and they are correctly
gated on Tasks 1 and 2. Task 1's measurement is what everything after it is
calibrated against, and it needs a bucket and a runner.

**A premise of the Introduction has changed since it was written, and it
strengthens the case rather than weakening it.** `.github/workflows/ci.yml`
still exports `VCPKG_BINARY_SOURCES='clear;x-gha,readwrite'`, and vcpkg now
answers `warning: The 'x-gha' binary caching backend has been removed`
(observed in every job of run 33997439418, September 5, 2026). The layer this
spec proposes to replace is therefore not degraded by eviction; it is **gone**,
and has been since whichever vcpkg release dropped it. The whole-tree
`actions/cache` L1 in front of it has no `restore-keys`, so a change to
`vcpkg.json` or any overlay misses in every job with nothing to fall back on:
that run's `Build & Test (g++-13, x64)` spent **70 m 30 s** in "Bootstrap
vcpkg and install dependencies", building 117 of the 131 ports it handled,
against 18 m 48 s building kythira and 8 m 24 s running its tests. Requirement
1's measurement should be read against that, not against a working `x-gha`.

## Overview

Move the vcpkg binary cache and the compiler cache out of the 10 GB GitHub
Actions cache, which `scripts/prune-actions-caches.sh` measured at 10.37 GB
and continuously evicting, into one OCI Object Storage bucket reached through
the S3 Compatibility API — vcpkg by `x-aws`, this project's own compiles by
sccache's `s3` backend — so that warm builds happen on every run rather than
on the runs the Actions cache happened to keep. Pre-registered cost: about
$0.50 per month (`doc/sccache_dogfood_cost_estimate.md`).

Reference material to read before starting:
- `.kiro/specs/oci-build-cache/design.md` — the composite action's exact
  logic, the CMake launcher block, the per-leg step sequence, the IAM
  policies and the bucket layout.
- `.kiro/specs/ccache-adoption/` — the spec this supersedes in CI; its
  Task 7 is the reason Requirement 7 exists.
- `scripts/prune-actions-caches.sh` header — the measurement that motivates
  this.
- `.github/workflows/ci.yml` — every `VCPKG_BINARY_SOURCES` line and every
  "Restore ccache" / "Save ccache" pair (six jobs), plus
  `arm64-docker-smoke-test.yml` and `real-cloud-tests.yml`'s `aws` and `oci`
  jobs.
- `scripts/perf-cloud/audit-aws-leaks.sh` and
  `scripts/perf-cloud/test-audit-aws-leaks.sh` — the audit pattern, and the
  test-in-the-failing-direction pattern.
- `docker/sccache_runner/run.sh` on `feat/redis-compatible-kv` — the guarded
  `sccache --start-server` pattern.

## Task Dependency Graph

```json
{
  "waves": [
    { "tasks": [1, 2] },
    { "tasks": [3, 4, 5] },
    { "tasks": [6, 7] },
    { "tasks": [8, 9, 10] },
    { "tasks": [11, 12] }
  ],
  "edges": [
    [1, 5], [1, 7], [2, 3], [2, 6], [2, 7],
    [3, 6], [3, 7], [4, 7], [5, 7],
    [6, 8], [7, 8], [7, 9], [8, 10], [9, 10], [10, 11], [10, 12]
  ]
}
```

Tasks 1 and 2 are independent and both gate the wiring: nothing permanent
is written to a workflow until the throwaway measurement exists and the
bucket does.

## Tasks

- [ ] 1. Throwaway measurement — **in flight; two attempts, both instructive**
  - Branch `measure/oci-build-cache-task1`, draft PR
    [#320](https://github.com/crawlins/kythira/pull/320), marked DO NOT MERGE
    per Requirement 1.5. It wires the composite action, `x-aws` and sccache
    into the nine legs of Requirement 5.1's table and leaves
    `ion-serializer-build` on its original wiring as an unmodified control.
    The action gains a throwaway arm selecting the read-write key for this
    branch, because a warm run is meaningless unless the cold one populated
    the cache and only `main` may otherwise write.
  - **Attempt 1 (run 34030385117) measured nothing, and was cancelled.**
    `sccache --start-server` failed on every leg, so every Configure received
    `KYTHIRA_COMPILER_LAUNCHER=none` and the statistics step skipped itself.
    The run reported success throughout: the guarded start is designed to let
    a build proceed without a cache, so "no cache at all" and "healthy" look
    identical from the outside. **Anything that reads a green run as evidence
    the cache works is reading nothing.** The cause was the empty IAM groups
    recorded under Task 2, not the wiring.
  - **Attempt 1 also could not have measured Requirement 1.2 even had sccache
    worked.** Every measured leg logged `Bootstrap vcpkg and install
    dependencies: skipped`: the `vcpkg_installed/` tree cache (the L1) is
    keyed on `hashFiles('vcpkg.json', 'vcpkg-overlays/**')`, it hit, and a hit
    there skips the vcpkg invocation entirely — so `x-aws` never ran. The
    fix is a comment appended to an overlay README, which moves that hash
    without touching any portfile, forcing an L1 miss without changing what is
    built. Worth keeping: **the vcpkg binary cache can only be measured on an
    L1 miss**, and nothing in the workflow says so.
  - **Attempt 2 (run 34087833341) is running with both halves live.** Before
    launching it, the `x-aws` call was probed by hand — which found a fourth
    blocker that would have wasted the run: AWS CLI ≥2.23 sends `PutObject`
    as an aws-chunked stream and OCI answers `NotImplemented: AWS chunked
    encoding not supported`, so every port upload would have failed. With
    `AWS_REQUEST_CHECKSUM_CALCULATION=when_required` exported by the action it
    round-trips byte-identically, and the run has since uploaded **315 port
    archives, 788 MiB** (x64 211 / 479 MiB, arm64 104 / 308 MiB) into the
    correct per-triplet prefixes. That is Requirement 1.2's empty-bucket
    column, generated.
  - **Attempt 2 finished green, 15/15 jobs, and its vcpkg half is complete.**
    Requirement 1.2's *empty bucket* column, one full matrix with `x-aws`
    writing into an empty bucket (run 34087833341):

    | Leg | vcpkg install | Build |
    | --- | ---: | ---: |
    | `Build & Test (clang++-18, arm64)` | 69.4 min | 34.8 min |
    | `Build & Test (g++-13, arm64)` | 70.2 min | — |
    | `Coverage (clang++-18)` | 74.3 min | — |
    | `Build & Test (g++-13, x64)` | 75.9 min | — |
    | `Build & Test (g++-14, x64)` | 83.6 min | — |
    | `Full suite (boost)` | 96.6 min | 80.9 min |
    | `Full suite (stdexec)` | 98.9 min | **170.3 min** |
    | `gcp-sdk-build` | **163.4 min** | — |
    | `ion-serializer-build` (control, no binary cache) | 97.9 min | 0.7 min |

    Uploaded: **397 archives, 5.06 GB** (x64 266 / 3.69 GB, arm64 131 /
    1.34 GB) into the correct per-triplet prefixes.

    Two rows carry the argument for the whole spec. **`gcp-sdk-build` at
    163.4 minutes** reproduces the 2 h 33 m the Introduction cites, because an
    empty bucket is that same situation; `ci.yml` claims "the gcp tree is the
    edhoc tree plus google-cloud-cpp, so only the delta is ever built", which
    has never been true *across* runs for want of a persistent binary cache,
    and the populated column is the test of it. **`Full suite (stdexec)`'s
    Build at 170.3 minutes** is worse than the 1 h 51 m – 2 h 09 m
    Requirement 6.2 quotes, and 6.2 asks for ≤45 min once the compiler cache
    is warm — so that threshold is now measured against 170.3, not 129.
  - **Attempt 2's sccache half is void on every leg, for a new reason:**
    `sccache: error: Server startup failed: Address in use`, then
    `-DKYTHIRA_COMPILER_LAUNCHER=none`. The action exports `RUSTC_WRAPPER` and
    keeps it through `VCPKG_KEEP_ENV_VARS`, so the `lakers` port's cargo build
    starts an sccache server **during the vcpkg install** — which is
    Requirement 5.7 working as designed. The guard then reads an
    already-running server as failure. **design.md Component 4 has the same
    bug**: `if sccache --start-server; then` asks "did I start one" when the
    question is "is one reachable". It needs
    `sccache --start-server || sccache --show-stats >/dev/null`.
  - **The canary meant to catch exactly this was itself broken.** It checked
    the `Start sccache` step's conclusion, which is `success` in both branches
    because the `else` only echoes a warning. It reported green while every
    leg ran uncached. A step that cannot fail is not a signal.
  - **Unintended but real: the rustc half is proven.** `sccache/` ended the
    run with **98 objects, 40 MiB**, and since the C++ launcher was `none`
    everywhere those can only be rustc artifacts from the cargo build. That is
    the coverage `.kiro/specs/redis-compatible-kv/` names as ccache's gap,
    working against OCI.
  - **A constraint discovered late: a cold sccache column cannot be re-taken
    in the same prefix.** `OBJECT_DELETE` is granted to nobody, deliberately,
    so the 98 objects cannot be removed before 30 days of lifecycle expiry.
    Attempt 3 must use a fresh `SCCACHE_S3_KEY_PREFIX` (e.g.
    `sccache-measure-3/`) for a virgin prefix. This applies to every future
    re-measurement and is a consequence of the no-delete rule, not a fault in
    it.
  - **Still owed**: the per-leg sccache table (Requirement 1.1) from a run
    where the launcher is actually sccache,
    the same table from a warm re-run, the `kythira_test_pch` and
    `-fprofile-instr-generate` answers Requirement 1.3 names, and the
    populated-bucket install timings. The warm round needs the L1 entries this
    run saves to be deleted first, or its vcpkg install is skipped exactly as
    in attempt 1.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [x] 2. Provision the bucket, users, policies and keys — **applied against
      the real tenancy September 6, 2026**
  - `provision.sh --apply` ran green end to end. What exists now, read back by
    `audit.sh` (exit 0, no UNKNOWNs): bucket `kythira-build-cache` in
    `us-phoenix-1`, namespace **`axunmw4f0mln`**, NoPublicAccess, Standard,
    versioning disabled; both lifecycle rules (`sccache/` 30 days, `vcpkg/`
    90 days, enabled); three groups, two users, one policy of four
    statements; and one ACTIVE customer secret key per user, created
    **2026-09-06T10:25:52Z** (rw) and **10:25:56Z** (ro). Keys themselves are
    not recorded here, by design.
  - Repository configuration set: variables `OCI_BUILD_CACHE_BUCKET` and
    `OCI_BUILD_CACHE_NAMESPACE`, secrets `OCI_BUILD_CACHE_R{W,O}_ACCESS_KEY_ID`
    and `_SECRET_ACCESS_KEY` (2026-09-06T10:26Z).
  - **Five defects, each found by the service refusing the call rather than by
    review.** The dry run cannot find any of them, which is the point worth
    keeping: a plan that prints correctly is not a plan that works.
    1. **The lifecycle payload shape was wrong twice over.** `--items` takes a
       bare JSON *array* in *camelCase*; the design's `{"items": [...]}` with
       kebab-case keys is refused as `InvalidJSON: Could not parse body as
       valid ObjectLifecycleDetails`, which names neither the key nor the
       shape. `--generate-param-json-input items` is the authority.
    2. **Lifecycle rules need a policy for the Object Storage service
       principal**, which design.md Component 1 does not mention:
       `InsufficientServicePermissions` until
       `Allow service objectstorage-us-phoenix-1 to manage object-family …`
       exists. The lifecycle step now runs *after* the policy step and
       retries, because IAM is eventually consistent — it failed on attempt 1
       and succeeded on attempt 2, ten seconds later, on the very first run.
    3. **`user create` needs `--email` on an identity-domain tenancy**
       (`error.identity.user.primaryEmailNotSpecified`). Added as an `--email`
       flag defaulting to the address of the OCI CLI user running the script.
    4. **OCI policy statements have no `user` subject.** The grammar admits
       `any-user`, `group`, `dynamic-group` and `service` only, so the
       user-scoped rw/ro split in design.md cannot be written at all —
       `Failed to parse policy due to an issue with token: user at character:
       6`. It is now one group per role (`kythira-build-cache-rw`,
       `kythira-build-cache-ro`) holding one user each, with the umbrella
       group keeping the single grant both share (`read buckets`).
    5. **The key-minting guard read "no keys" as "has keys".**
       `oci iam customer-secret-key list` on a user with none prints
       **nothing** and exits 0 — not `{"data": []}` — so `length(data)` gave
       an empty string, which compares unequal to `"0"`, and the script
       declined to mint the keys it exists to create while reporting
       "already holds  key(s)". Empty and `null` now normalise to 0.
  - One more, in `audit.sh`: the Usage API rejects any bound that is not
    exactly midnight UTC ("hours, minutes, seconds, and second fractions must
    be 0"), so asking for usage "up to now" always failed. The end bound is
    tomorrow's date.
  - **Requirement 2.6's live direction: the query is proven, the latency
    test is not.** `test-audit.sh --live` creates a throwaway tagged bucket
    and waits for the audit to flag it. It did not appear within five minutes,
    nor within twenty on a second run, so that check reports INCONCLUSIVE —
    a slow search index is not a broken auditor, and reporting it as a
    failure would be exactly the unearned red the test exists to prevent.

    **What the live run does establish is the thing that matters**: the
    audit's section 5 finds **all seven** real resources — the bucket, three
    groups, two users and the policy — through the identical
    `structured-search` query, in a tenancy where they were created by
    `provision.sh` and tagged by it. So the query, the tag and the
    expected-versus-unexpected classification are all verified against live
    data; the stub covers the classification of an *unexpected* one. The only
    unverified link is how long OCI takes to index a brand-new resource,
    which is a property of the service. For reference: the real bucket was
    created at 10:16Z and was listed by the audit at 10:26Z, so indexing does
    happen — it appears to run on a sweep longer than the 20-minute window,
    not continuously.
  - **The bucket was provisioned but unusable for a further three hours, and
    the reason was neither IAM nor OCI (September 6, 2026).** Everything below
    was found by the service refusing a call; none of it is visible in a dry
    run, and the first CI matrix that tried to use the cache reported success
    while caching nothing.
    1. **The three IAM groups were empty, and this script said they were
       not.** This is the root cause of every `NoSuchBucket` in the chain, and
       it is the empty-versus-zero trap for the *second* time in this file:
       `oci iam group list-users --query "length(data[?id=='…'])"` prints
       nothing and exits 0 for a non-member, so `member=""`, `[ "$member" !=
       "0" ]` is true, and the script printed "kythira-build-cache-rw is
       already in kythira-build-cache-rw" six times while adding nobody. A
       policy that grants to an empty group grants nothing, and the S3 API
       reports that as a bucket which does not exist. Both counts now go
       through one `count_of()` helper.
    2. **The S3 Compatibility API cannot see a bucket outside its designated
       compartment.** It resolves a bucket *name* in exactly one compartment
       per namespace (`default-s3-compartment-id`, the tenancy root here), and
       design.md says to create the bucket in `OCI_CI_COMPARTMENT_ID`. The
       script now reads that compartment, creates the bucket there, moves one
       that is elsewhere, and scopes the policy to follow it — `in tenancy`,
       since `in compartment id <tenancy-ocid>` is not accepted for the root.
    3. `oci iam policy update` refuses `--statements` without
       `--version-date`; empty is the documented "current service behaviour"
       value. Hence `--update-policy`, off by default.
  - **A negative result worth keeping.** The `read buckets` statement was
    relaxed (its `target.bucket.name` condition dropped) on the theory that a
    condition cannot be evaluated before a bucket name is resolved. That
    experiment ran while the groups were still empty, so it could not have
    shown anything; re-run after the real fix, the tightened statement works
    unchanged. The condition is back, all four statements are bucket-scoped,
    and the hypothesis is recorded as **refuted** rather than left standing.
  - **Proof the cache works, taken after the fix rather than assumed:** both
    keys start sccache against the bucket; a fresh source compiles cold, then
    hits from a server restarted with no local state — a hit that can only
    have come from OCI — at 100% hit rate with 0 cache and 0 write errors;
    the bucket goes from empty to 3 objects / 3753 bytes; `audit.sh` exits 0
    with no UNKNOWNs.
  - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6_

- [ ] 3. Composite action `.github/actions/oci-build-cache/` — **written and
      exercised September 5, 2026; not yet run by any workflow**
  - `action.yml` implements design.md Component 2: the inputs of Requirement
    4.5, the `::error::` on a missing repository *variable*, the
    no-credentials `enabled=false` path for missing *secrets*, the AWS CLI
    assertion, the environment export, and the pinned sccache install for
    `X64` and `ARM64`.
  - **The selection logic was run, not reviewed.** The step's script was
    extracted from the YAML and executed with a stub `aws`, a temporary
    `GITHUB_ENV` and `GITHUB_OUTPUT`, once per state:

    | Event and ref | Key | vcpkg mode | `enabled` | Exit |
    | --- | --- | --- | --- | --- |
    | `push:main` | read-write | `readwrite` | true | 0 |
    | `push:main-experiment` | read-only | `read` | true | 0 |
    | `pull_request:318/merge` | read-only | `read` | true | 0 |
    | `push:main`, RO secrets absent | read-write | `readwrite` | true | 0 |
    | `pull_request`, RO secrets absent | none | — | **false**, with a warning | 0 |
    | `push:main`, `BUCKET` unset | — | — | — | **1**, `::error::BUCKET is empty` |

    The second row is the one worth having: Task 3 asks specifically that the
    `push:main` case be exact, and `main-experiment` takes the read-only key.
  - **sccache is pinned by version and by checksum, and the checksums were
    measured rather than copied.** Both `linux-musl` assets of v0.17.0 were
    downloaded and hashed on this box; each agrees with the `.sha256`
    published beside it (x86_64 `67c4a96d…`, aarch64 `821a8634…`), and the
    x86_64 binary runs and reports `sccache 0.17.0`. Both guards were run:
    an unrecognised `RUNNER_ARCH` exits 1, and bumping `sccache-version`
    without bumping the checksums exits 1 rather than installing an
    unverified binary. A deliberately wrong hash was fed to the same
    `sha256sum -c` line to confirm it fails.
  - **`actionlint` cannot check this file.** 1.7.7 parses any `.yml` under
    `.github/` as a *workflow*, so it reports a missing `on:` and `jobs:`
    section for a composite action's metadata and never looks at the
    contents; a deliberately typo'd copy produced the identical output, so it
    was not checking either way. Verified instead by parsing the YAML
    (`runs.using == composite`, every step declares `shell: bash`) and
    running `shellcheck` 0.10.0 over each extracted `run:` block — clean.
  - Secrets reach the script through `env:` rather than `${{ }}` splicing in
    the body, so a secret containing shell metacharacters cannot be executed.
  - **Still to do**: the two verification runs Task 3 names, which need a
    scratch workflow and the bucket — and Tasks 6 and 7, which are what
    actually reference this action.
  - _Requirements: 3.2, 4.1, 4.2, 4.3, 4.5, 5.3_

- [x] 4. CMake `KYTHIRA_COMPILER_LAUNCHER` — **done September 5, 2026**
  - The `KYTHIRA_ENABLE_CCACHE` block in the root `CMakeLists.txt` is replaced
    by design.md Component 3, with the deprecation alias.
  - **All seven states verified**, by extracting the block verbatim into a
    scratch project so each state is a separate configure rather than a
    reconfigure of one tree (`CMAKE_IGNORE_PATH` hides this box's
    `/usr/bin/ccache` for the "absent" cases, with `CMAKE_MAKE_PROGRAM`
    pinned so that ignoring `/usr/bin` does not also hide `make`):

    | State | `CMAKE_CXX_COMPILER_LAUNCHER` | Exit |
    | --- | --- | --- |
    | `auto`, ccache present | `/home/clark/.local/bin/ccache` | 0 |
    | `auto`, only sccache present | the sccache found | 0 |
    | `auto`, neither present | empty, "compiler launcher: none" | 0 |
    | `sccache` explicit, present | the sccache found | 0 |
    | `sccache` explicit, absent | — | **1**, `FATAL_ERROR` naming the launcher |
    | `none` | empty | 0 |
    | `KYTHIRA_ENABLE_CCACHE=OFF` | empty, after a deprecation warning | 0 |

  - **Requirement 9.1 (target-list diff): identical.** Two full configures of
    `ci_full_defconfig` in scratch trees, `-DKYTHIRA_COMPILER_LAUNCHER=auto`
    against `=none`, **3128 targets each**, and `diff` of `ninja -t targets
    all` is empty once the build-directory path is normalised (the only
    textual difference was each tree's own absolute path). The launcher is
    demonstrably wired in the `auto` tree — 561 `ccache` occurrences in
    `build.ninja`, `LAUNCHER = /home/clark/.local/bin/ccache` — and absent in
    the `none` tree at 0.
  - **Two deviations from design.md Component 3, both needed to make it work
    across reconfigures**, since CI legs and developers will change this
    variable on an existing tree:
    1. `find_program(_launcher NAMES ccache sccache)` as written pins its
       result in a cache entry, and `find_program` skips the search when its
       result variable is already cached — so a tree first configured with
       ccache would keep launching ccache after a later
       `-DKYTHIRA_COMPILER_LAUNCHER=sccache`, silently. The block now uses one
       cache entry per launcher name (`KYTHIRA_CCACHE_PROGRAM`,
       `KYTHIRA_SCCACHE_PROGRAM`, and `KYTHIRA_<NAME>_PROGRAM` on the explicit
       path). Verified: `auto` → explicit `sccache` on the same tree switches.
    2. The `else()` branch now writes empty `CMAKE_C/CXX_COMPILER_LAUNCHER`
       cache entries rather than leaving them alone. Both are written with
       `FORCE`, so without this a tree configured with a launcher and then
       reconfigured to `none` kept launching. Verified: the entry goes from
       `/home/clark/.local/bin/ccache` to empty.
  - `message(DEPRECATION)` is kept as designed rather than downgraded to
    `WARNING`: checked on CMake 3.31.6 that it prints by default, with and
    without `CMAKE_WARN_DEPRECATED`.
  - The two sccache states were re-run against the **real** sccache 0.17.0
    binary (the one Task 3 pins), not only the stub they were first taken
    with, and select it identically. What is still not verified here is that
    sccache *caches* anything — that is Task 1's measurement, on a runner.
  - _Requirements: 5.2, 9.1_

- [ ] 5. `DEPENDENCIES.md` and `doc/ci_build_cache.md` — **written September 5,
      2026; the measured figures Tasks 10 and 12 add are still missing**
  - `DEPENDENCIES.md` gains an sccache entry beside ccache: what it is, why CI
    wants it and a local machine does not, the read-only recipe (`ro` key,
    `AWS_ENDPOINT_URL`, `-DKYTHIRA_COMPILER_LAUNCHER=sccache`), and that
    refused uploads on that key are the intended behaviour rather than a
    misconfiguration. The ccache entry's Status line is rewritten for the new
    launcher variable, including the deprecated alias.
  - `doc/ci_build_cache.md` covers Requirement 8.2: what each cache holds and
    how its key is formed, the bucket layout and why there is no per-leg
    sccache prefix, the 30/90 lifecycle asymmetry, the writer policy enforced
    twice (action, then IAM) with the PR trade-off stated, absence as a no-op,
    how to read the job-summary statistics and what each number would mean,
    the three scripts, key rotation, and the repository configuration table.
  - Not ingested by Doxygen: `Doxyfile`'s `INPUT` is `include README.md`, so a
    new file under `doc/` cannot break the docs gate. Checked rather than
    assumed, because that gate is `WARN_AS_ERROR = FAIL_ON_WARNINGS`.
  - **A discrepancy found while writing it, and written into the document.**
    `requirements.md` attributes the $0.50-per-month pre-registration to
    `doc/sccache_dogfood_cost_estimate.md` — "ten gigabytes of standard
    storage is $0.26, roughly 700,000 requests … about $0.24". Those figures
    are **not in that document**. It prices the Redis-gateway deployment
    (compute, block volume, egress, about $55 per month), has no Object
    Storage price line at all, and says explicitly that Object Storage is
    "not included … this deployment does not use". The $0.50 is therefore a
    pre-registration made in the spec, not a figure carried from a costed
    document, and `doc/ci_build_cache.md` says so at the point it quotes it.
    Requirement 8.3's cross-reference (Task 11) is where the two are
    reconciled; whoever does it should add the Object Storage line to the
    estimate rather than quietly re-citing it.
  - _Requirements: 8.1, 8.2_

- [ ] 6. vcpkg binary cache to the bucket, every workflow
  - In `ci.yml` (six jobs), `arm64-docker-smoke-test.yml`, and
    `real-cloud-tests.yml` (`aws`, `oci`): add the composite action after
    "Resolve vcpkg triplet", delete the `export VCPKG_BINARY_SOURCES=...x-gha`
    line (the env now carries it), delete the "Export Actions cache
    credentials for vcpkg" step. Keep the `vcpkg_installed/` tree caches.
  - Verify with a `push` to `main` whose `vcpkg.json` hash is new (or by
    deleting the tree cache once): the install step uploads, the audit shows
    `vcpkg/x64-linux/` and `vcpkg/arm64-linux/` populated, and the next run
    with the tree cache deleted again downloads instead of building.
    Record run ids and port counts.
  - _Requirements: 3.1, 3.2, 3.3, 3.6, 4.1_

- [ ] 7. sccache replaces ccache on every moved leg
  - Per design.md Component 4, on each leg Task 1 cleared: add "Start
    sccache" (guarded), pass `-DKYTHIRA_COMPILER_LAUNCHER=` from its output,
    add "sccache statistics" with `if: always()` writing to the job summary,
    delete "Restore ccache" / "ccache size limit" / "Save ccache" and
    `CCACHE_DIR`. Any leg Task 1 kept on ccache gets a comment beside its
    Configure step saying why, with the measured cacheable fraction.
  - `RUSTC_WRAPPER` and `VCPKG_KEEP_ENV_VARS` come from the action; verify
    once, on a tree-cache miss, that the `lakers` port's cargo build reports
    sccache requests in the statistics.
  - Verify on a `push` to `main`: statistics in every summary, bucket
    `sccache/` object count rises. Record run id and per-leg counts.
  - _Requirements: 5.1, 5.4, 5.5, 5.6, 5.7_

- [ ] 8. Actions-cache accounting, re-measured
  - After Tasks 6 and 7 have run on `main` for two days, re-run the
    measurement in `scripts/prune-actions-caches.sh`'s header (total bytes,
    entry count, bytes per family) and rewrite that header's accounting
    paragraph with the new figures. The rules do not change.
  - Record before/after here.
  - _Requirements: 3.4_

- [ ] 9. Three-state credential verification and bad-endpoint run
  - Real runs: a `push` to `main` writes (object count rises); a
    `pull_request` from a branch of this repository reads with hits and
    reports sccache write errors, object count unchanged; a run with the
    four secrets temporarily renamed builds green with `enabled=false`.
  - One run with `OCI_BUILD_CACHE_NAMESPACE` pointed at a non-existent
    namespace: green, ports built from source, sccache start warning.
  - Record all four run ids here.
  - _Requirements: 3.5, 4.3, 7.2, 7.3, 9.4_

- [ ] 10. Second-run thresholds
  - On the second consecutive `push` run to `main` after Task 7, read from
    the job summaries: each `build-and-test` Build step ≤ 15 min; stdexec
    leg Build ≤ 45 min with swap under 4 GiB in "Report headroom after
    build"; every leg ≥ 90% hits over cacheable; zero ports built from
    source. A following `pull_request` run shows the same hit rates.
  - Record the table here, against the pre-registered figures. A miss on
    any threshold is a finding to investigate in this task, not a threshold
    to move.
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [ ] 11. Month-one audit and the cost cross-reference
  - Run `scripts/oci-build-cache/audit.sh` one month after Task 7: bytes per
    prefix, request count and egress from the usage API, and the bill line.
    Paste it here beside the $0.50 and 1 to 5 TB pre-registered.
  - Add the one-paragraph cross-reference to
    `doc/sccache_dogfood_cost_estimate.md` with the measured figure.
  - _Requirements: 6.5, 7.4, 8.3_

- [ ] 12. TODO row and close-out
  - Update `doc/TODO.md`'s row for this spec from 0/12 with what each task
    found, in the manner of the table's other closed rows, and move it off
    "Not Started".
  - Confirm every task above cites a run id (Requirement 7.1).
  - _Requirements: 7.1, 8.4_

## Deferred

- sccache inside vcpkg port builds via a custom triplet (design.md,
  "Deferred").
- Pointing sccache at the Redis gateway; that is
  `doc/sccache_dogfood_cost_estimate.md`'s deployment, and this spec is its
  baseline.
