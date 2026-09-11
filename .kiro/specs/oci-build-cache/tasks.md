# Implementation Plan — OCI-Hosted Build Cache

## Status: 11/12 tasks complete (1, 1a, 2, 3, 4, 5, 6, 7, 7a, 8, 9, 10, 12)

**Last Updated**: September 9, 2026. **Both caches are live on `main`** and
verified there by run
[34296062792](https://github.com/crawlins/kythira/actions/runs/34296062792),
15 of 15 jobs green: vcpkg's archives through `x-aws` and this project's
compiles through sccache, in one OCI bucket, with 397 archives / 5.06 GB
banked and zero ports built from source. The `Full suite (stdexec)` leg —
the one that fought hardest — built in **4.1 minutes against 175 cold**, at
561 hits out of 561 requests.

**Task 10 is closed and every Requirement 6 threshold is met with room to
spare**, read off run
[34353211972](https://github.com/crawlins/kythira/actions/runs/34353211972):
`build-and-test` Builds of 2.23–4.05 minutes against a 15-minute ceiling, the
stdexec leg at 4.01 against 45, hit rates of 99.82–100%, and zero
non-cacheable calls in 4,494 requests. Nothing needed investigating and no
threshold was moved.

**Task 9 is closed, and it found the only things that were actually broken.**
A job pointed at a deliberately wrong endpoint built **red** instead of
degrading to no compiler cache — twice, for two independent reasons, both
violating Requirement 3.5. sccache validates its storage backend lazily, on
the first request, so no exit status could distinguish a healthy server that
was already listening from a dead one (PR
[#326](https://github.com/crawlins/kythira/pull/326)); and `RUSTC_WRAPPER` was
exported before the vcpkg install, where the lakers port's cargo build uses it
and where no per-job guard can reach (PR
[#328](https://github.com/crawlins/kythira/pull/328)). Neither had ever fired,
because the `vcpkg_installed/` tree cache had been hitting on every run. The
re-run against both fixes is **15/15 green** (run
[34358385332](https://github.com/crawlins/kythira/actions/runs/34358385332))
and, as a by-product, is the first controlled measurement of what the vcpkg
cache is worth: ten legs installing in **4.41–6.51 minutes** beside one at
**95.01**, same commit, same L1 miss, cache reachable or not.

**Task 11** is all that is left, and what remains of it is one full month of
cadence, not a missing bill. **The bill is readable and always was**: the
September 9 run reported "no Object Storage line yet this month" because
`audit.sh` asked the Usage API for an ungrouped total and then filtered it by
`service`, a field that API only populates when asked to group. Fixed
September 10, 2026, and month-to-date then read **$0.051963** — of which
requests are **100%**, storage and egress both zero under the 10 GB
always-free and 10 TB egress allowances. Against the **$0.50/month**
pre-registered that is comfortable, and for a reason the pre-registration did
not contain: this bill scales with round trips, not with bytes. Storage the
same day was **7.8 GiB** (sccache 4,721 objects / 2,781 MiB, up from 4,535 /
2,282 the day before; vcpkg unchanged), leak check clean, both lifecycle rules
enabled, both keys ACTIVE.

That makes **four** separate defects in this spec with one shape — something
that could not answer being read as an answer — the earlier three being PRs
[#326](https://github.com/crawlins/kythira/pull/326),
[#328](https://github.com/crawlins/kythira/pull/328) and
[#329](https://github.com/crawlins/kythira/pull/329), the last of which was
also found by an `audit.sh` run and also reported a false negative about a
credential CI was authenticating with at that moment. The rule now written at
the site: **when a query comes back empty, say whether the QUERY failed or the
SUBJECT is absent, and never let those two print the same sentence.**

What is still owed is the October 9 re-run against the **1 to 5 TB** egress
band, which 130 GB over an eleven-day window that was neither a full month nor
a representative one cannot settle.

**What this spec got wrong, kept here because it is the useful part.** Two
premises did not survive contact. `x-gha` — the backend every workflow was
asking for — had already been removed by vcpkg, so the layer being replaced
was not degraded, it was absent (see below). And moving the compiler cache
out of the Actions cache **did not relieve the 10 GB ceiling**: ccache was
0.60 GiB of it, not the eight 2 GB families assumed, and 94% is the
`vcpkg_installed/` trees Requirement 3.3 deliberately keeps. The real result
is better and different — eviction stopped being expensive, because a miss
went from 69–163 minutes of rebuilding to a 4–7 minute download.

**Last Updated (previous)**: September 7, 2026. **Task 1's measurement is done**, and it
answers the question the whole spec turns on. The vcpkg binary cache takes
installs from 69–163 minutes to 4–7; the compiler cache takes the stdexec
leg's Build from **170.3 minutes to 3.0**, at a 100% hit rate over 561
translation units. Every threshold in Requirement 6 is met with an order of
magnitude to spare, and none needs relaxing. The two questions Requirement 1.3
raises — whether the precompiled header and the coverage leg's instrumented
objects are cacheable — are both answered **yes**, with zero non-cacheable
calls across 3,856 compiles.

**Last Updated (two before)**: September 6, 2026. Task 2 is done: **the bucket, the IAM and
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

**Nothing is wired into a workflow** (true when written on September 6; Tasks
6 and 7 landed on September 9). The composite action exists and no job
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

- [x] 1. Throwaway measurement — **complete September 8, 2026, on the third
      attempt; the first two measured nothing and are kept below because
      each one failed in a way a green run could not distinguish from
      success**
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
  - **Attempt 3 (run 34133234910) is the measurement.** Nine of ten measured
    legs green; the tenth died on a transient sccache download (see below).
    Both halves recorded.

    **Requirement 1.2 — vcpkg install, empty bucket → populated:**

    | Leg | empty | populated | factor |
    | --- | ---: | ---: | ---: |
    | `gcp-sdk-build` | 163.4 min | **6.5 min** | **25×** |
    | `Full suite (stdexec)` | 98.9 min | 6.4 min | 15× |
    | `Full suite (boost)` | 96.6 min | 5.3 min | 18× |
    | `Build & Test (g++-14, x64)` | 83.6 min | 6.1 min | 14× |
    | `Build & Test (g++-13, x64)` | 75.9 min | 6.1 min | 12× |
    | `Coverage (clang++-18)` | 74.3 min | 5.9 min | 13× |
    | `Build & Test (clang++-18, arm64)` | 69.4 min | 5.1 min | 14× |
    | `Build & Test (clang++-18, x64)` | — | 4.0 min | — |
    | `ThreadSanitizer` | — | 4.2 min | — |
    | **`ion-serializer-build` (control, no binary cache)** | **97.9 min** | **97.8 min** | **1.00×** |

    **The control is the load-bearing row.** It is untouched, still asking for
    the removed `x-gha`, and it reproduced itself to within six seconds across
    two runs hours apart while every cached leg fell by an order of magnitude.
    That is what rules out "the runners were faster today", which is the
    objection a single-run measurement always invites.

    Requirement 6.3's criterion is met on a real run: **zero ports built from
    source** (checked on `g++-13 x64`; all 131 packages came from OCI). Attempt
    2 uploaded 397 archives / 5.06 GB to produce this.

    **Requirement 1.1 — sccache, cold column** (virgin prefix
    `sccache-measure-3/`, so 0% hits is the correct result, not a
    disappointment):

    | Leg | requests | non-cacheable | misses | write errors | Build |
    | --- | ---: | ---: | ---: | ---: | ---: |
    | `Build & Test (clang++-18, arm64)` | 561 | **0** | 561 | 0 | 36.8 min (cold-cache 34.8) |
    | `Build & Test (clang++-18, x64)` | 561 | **0** | 561 | 0 | 54.1 min |
    | `Build & Test (g++-13, x64)` | 561 | **0** | 561 | 0 | 54.7 min |
    | `Build & Test (g++-14, x64)` | 561 | **0** | 561 | 0 | 59.6 min |
    | `Coverage (clang++-18)` | 542 | **0** | 542 | 0 | 43.3 min |
    | `Full suite (boost)` | 527 | **0** | 527 | 0 | 79.3 min (was 80.9) |
    | `Full suite (stdexec)` | 527 | **0** | 527 | 0 | 166.6 min (was 170.3) |
    | `ThreadSanitizer` | 9 | **0** | 9 | 0 | 4.5 min |
    | `gcp-sdk-build` | 7 | **0** | 7 | 0 | 0.9 min |

  - **Requirement 1.3 is answered, and the answer is yes to both.** Across
    **3,856 compiles** spanning Release, ThreadSanitizer and coverage builds,
    on two compilers and two architectures, there is **not one non-cacheable
    call, not one unsupported-compiler call, and not one write error**. The
    `kythira_test_pch` precompiled header does not defeat sccache, and the
    coverage leg's `-fprofile-instr-generate` objects are fully cacheable —
    542 of 542. Requirement 5.1's clause about keeping a leg on ccache when
    fewer than 50% of its requests are cacheable therefore has **no
    candidates**: every leg is at 100%.
  - **The cold-cache penalty is noise.** Build wall clocks moved by −2% to
    +6% while populating the cache (boost 80.9 → 79.3, stdexec 170.3 → 166.6,
    clang arm64 34.8 → 36.8). Average cache write is 0.050 s, so ~28 s spread
    over a whole build. Populating costs nothing measurable; the entire saving
    so far is the vcpkg half.
  - **Still owed: the WARM sccache column.** Every leg above is a first pass
    against a virgin prefix. A re-run against the same prefix is what produces
    Requirement 6.1's ≤15 min `build-and-test` Build, 6.2's ≤45 min stdexec
    Build, and 6.3's ≥90% hit rate — and it is cheap now that installs are
    4–7 minutes. **Note 6.2's threshold should be read against 170.3 min, not
    the 1 h 51 m – 2 h 09 m the requirement quotes.**
  - **One leg failed, and it exposed a real brittleness in the action.**
    `Build & Test (g++-13, arm64)` died 60 seconds in, inside the pinned
    sccache download; the other arm64 leg passed the identical step, so it was
    transient. But the action **failed the whole job over an unavailable
    cache**, which contradicts its own stated contract and the guarded-start
    discipline used everywhere else. A checksum *mismatch* must stay fatal — a
    wrong binary on the compile path is not a warning — but a failed download
    should degrade to no compiler cache. Fix before Task 7.
  - **The WARM round (run 34133234910, attempt 2) closes Requirement 1.1 and
    every threshold in Requirement 6.** Same commit re-run, so cache state was
    the only variable; the four L1 tree caches were deleted first so vcpkg ran
    again rather than being skipped.

    | Leg | Build: no cache → cold sccache → **warm** | hit rate | install |
    | --- | --- | ---: | ---: |
    | `Full suite (stdexec)` | 170.3 → 166.6 → **3.0 min** | **100%** (561/561) | 3.7 min |
    | `Full suite (boost)` | 80.9 → 79.3 → **3.6 min** | **100%** (561/561) | 4.4 min |
    | `Build & Test (g++-14, x64)` | — → 59.6 → **4.1 min** | **100%** (561/561) | 4.8 min |
    | `Build & Test (g++-13, x64)` | — → 54.7 → **3.4 min** | **100%** (561/561) | 5.8 min |
    | `Build & Test (clang++-18, x64)` | — → 54.1 → **3.1 min** | 99.82% (560/561) | 5.9 min |
    | `Coverage (clang++-18)` | — → 43.3 → **5.8 min** | 99.82% | 4.7 min |
    | `Build & Test (clang++-18, arm64)` | 34.8 → 36.8 → **2.2 min** | 99.82% | 5.4 min |
    | `ThreadSanitizer` | — → 4.5 → **0.2 min** | **100%** (9/9) | 3.7 min |
    | `gcp-sdk-build` | — → 0.9 → **0.0 min** | **100%** (7/7) | 5.0 min |
    | `Build & Test (g++-13, arm64)` | — → — → 40.1 min | **0%** (0/561) | 5.6 min |

    **Requirement 6, every criterion, measured:**

    | Criterion | Target | Measured |
    | --- | --- | --- |
    | 6.1 `build-and-test` Build | ≤ 15 min | **2.2 – 4.1 min** |
    | 6.2 stdexec Build | ≤ 45 min | **3.0 min** |
    | 6.2 swap use | < 4 GiB | leg passed; the build is now 3 minutes of linking |
    | 6.3 hit rate over cacheable | ≥ 90% | **99.82 – 100%** |
    | 6.3 ports built from source | zero | **zero** |

  - **The last row of the warm table is the one that proves the mechanism.**
    `g++-13 arm64` shows 0% and 561 misses — because it is the leg that
    *failed* in attempt 3 on a transient sccache download, so it never wrote
    its objects. Nothing to hit. Hits come from what a previous run wrote, and
    the one leg that wrote nothing is the one leg that hit nothing. A
    uniformly perfect table would have been less informative.
  - **A 100% hit rate across 561 translation units is the surprising part.**
    It means every compiler invocation hashed identically to the previous
    run — no timestamps, absolute paths or run-specific values leaking into a
    command line. That property, not the wall clock, is what makes the cache
    usable at all.
  - **One real miss, and it is not noise.** Exactly one translation unit misses
    on all three **clang** legs (99.82% = 560/561) and on neither gcc leg. Same
    count, same compiler, both architectures: one TU whose clang command line
    genuinely varies between runs, not flakiness. Worth identifying before
    Task 10 quotes a hit rate.
  - **Correctness, not just speed:** the coverage leg passed every step
    including its floor gate, so cached `-fprofile-instr-generate` objects
    still produce valid coverage; and both `Full suite` legs ran the whole
    test suite green off cached objects (8.0 and 7.8 min).
  - **Task 1 is complete** apart from the write-up of Requirement 1.4's
    threshold re-derivation, which the table above supplies: no threshold in
    Requirement 6 needs relaxing, and 6.2's quoted cold baseline of
    1 h 51 m – 2 h 09 m should be corrected to the **170.3 min** measured here.
  - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

- [x] 1a. Throwaway closed and deleted (Requirement 1.5) — **September 8, 2026**
  - PR [#320](https://github.com/crawlins/kythira/pull/320) closed **unmerged**,
    and the branch deleted from the remote.
  - **The read-write arm never reached `main`.** It existed only on the
    throwaway branch, which is why deleting the branch — not editing the
    action — is what removed it. Verified after the fact: `origin/main` and
    `fix/oci-build-cache-cli-prompt` both contain zero references to
    `measure/`, and the branch no longer exists on the remote. The writer
    policy is back to `push:main` alone, with IAM enforcing it independently.
  - The permanent wiring (Tasks 6 and 7) should be written fresh against the
    corrected `design.md`, not by resurrecting that branch: its ci.yml carries
    an L1-busting comment in an overlay README and a measurement-only key
    prefix, neither of which belongs on `main`.


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

- [x] 3. Composite action `.github/actions/oci-build-cache/` — **done
      September 8, 2026, verified by three real runs rather than a scratch
      workflow**
  - `action.yml` implements design.md Component 2 as corrected: event-to-key
    selection, `::error::` on a missing repository *variable*, the
    no-credentials `enabled=false` path for missing *secrets*, the AWS CLI
    assertion, the environment export, and the pinned sccache install for
    `X64` and `ARM64`.
  - **Task 3's own verification list is discharged by Task 1's runs**
    (Requirement 7.1: runs **34030385117**, **34087833341** and
    **34133234910**, every `Build & Test` and `Full suite` job's
    "Select credential and export the cache environment" step), which
    exercised this action on ten legs across two architectures, three times:
    - the RW key is selected only for `push:main` and (on the throwaway
      branch, now deleted) an explicit branch arm; every other ref took the
      read-only key. Confirmed in job logs: `build cache: read-write … key`
      versus `read-only`.
    - an unset variable fails the job at the action — the `::error::` path was
      exercised locally with each of `BUCKET`, `NAMESPACE`, `REGION` and
      `TRIPLET` empty in turn.
    - the AWS CLI assertion passed on both `ubuntu-24.04` and
      `ubuntu-24.04-arm` (`aws-cli/2.36.35` on the runners), so the header's
      claim that it is present is now measured rather than assumed.
    - the pinned sccache installed and reported `sccache 0.17.0` on every leg,
      both architectures.
  - **Two corrections the runs forced, both now in the action:**
    1. `AWS_REQUEST_CHECKSUM_CALCULATION` / `AWS_RESPONSE_CHECKSUM_VALIDATION`
       set to `when_required`. Without them OCI answers every upload with
       `NotImplemented: AWS chunked encoding not supported`, because AWS CLI
       ≥2.23 sends `PutObject` as an aws-chunked stream. vcpkg's `x-aws`
       backend *is* that CLI call, so this was the difference between 397
       archives uploaded and none.
    2. **A failed sccache download now degrades to no compiler cache instead
       of failing the job**, while a checksum mismatch stays fatal. The
       asymmetry is the point: GitHub being unreachable for twenty seconds is
       not a reason to kill a two-hour build, and Task 1 lost
       `Build & Test (g++-13, arm64)` exactly that way while the other arm64
       leg passed the identical step — which contradicted this action's own
       header promise that the cache is never a build dependency. A wrong
       binary on the compile path is a different matter and still stops
       everything. Both paths were exercised by breaking each in turn: an
       unreachable URL warns and exits 0, a wrong checksum exits 1.
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

- [x] 5. `DEPENDENCIES.md` and `doc/ci_build_cache.md` — **done September 8,
      2026, now with measured figures rather than the design's estimates**
  - Both documents were written from the design in the first pass
    (Requirement 8.1, 8.2: what each cache holds, bucket layout, the 30/90
    lifecycle asymmetry, the writer policy and its trade-off, absence as a
    no-op, statistics, the scripts, key rotation, repository configuration).
  - `doc/ci_build_cache.md` gains a **"What it is worth"** section carrying
    Task 1's numbers: installs 69–163 min → 4–7, the stdexec Build 170.3 → 3.0,
    hit rates 99.8–100%, zero ports built from source — with the two caveats
    that matter more than the headline. That a cold cache costs nothing
    measurable (−2% to +6%), and that the comparison is **anchored** by the
    deliberately uncached `ion` control at 97.9 / 97.8 / 92.2 minutes across
    the same three runs, which is what makes it a measurement rather than a
    good afternoon on the runners.
  - It also records that every compile is cacheable (0 non-cacheable across
    3,856), that coverage instrumentation caches *and still passes its floor
    gate*, and the cache's steady-state size (397 archives / 5.06 GB, ~4,400
    sccache objects / 2.04 GB) so the cost line can be sanity-checked.
  - `DEPENDENCIES.md`'s sccache entry gains the headline measurement and a
    pointer to the operator document.
  - Requirement 8.3's cross-reference in
    `doc/sccache_dogfood_cost_estimate.md` stays with Task 11, which is where
    the first month's real bill lands — and note the discrepancy already
    recorded there: the $0.50 figure is a pre-registration made in this spec,
    not a figure carried from that document.
  - _Requirements: 8.1, 8.2_

- [x] 6. vcpkg binary cache to the bucket, every workflow — **done and
      verified on `main`, September 9, 2026**
  - Merged as PR [#322](https://github.com/crawlins/kythira/pull/322).
    Fifteen jobs across **five** workflows take `VCPKG_BINARY_SOURCES` from
    the composite action as `x-aws`; the `x-gha` exports and the
    "Export Actions cache credentials for vcpkg" steps that existed only to
    serve them are deleted. The `vcpkg_installed/` tree caches stay
    (Requirement 3.3).
  - **Requirement 3.1's enumeration was stale and the rule was followed
    instead.** It names `ci.yml`'s six jobs, `arm64-docker-smoke-test.yml`, and
    `real-cloud-tests.yml`'s `aws` and `oci` jobs. In fact `perf-cloud.yml` and
    `coap-flake-measure.yml` also set it, and `real-cloud-tests.yml` sets it in
    **six** jobs, not two — `alibaba`, `ami-build`, `aws`, `azure`, `gcp`,
    `oci`. Ten sites, not eight.
  - Verified on run **34296062792**, a real `push` to `main`: **15 of 15 jobs
    green**, the read-write key selected (`build cache: read-write key`), and
    the bucket at 397 archives / 5.06 GB with zero ports built from source.
  - _Requirements: 3.1, 3.2, 3.3, 3.6, 4.1_

- [x] 7. sccache replaces ccache on every moved leg — **done and verified on
      `main`, September 9, 2026**
  - Every job that compiles this project starts sccache against the bucket,
    passes `-DKYTHIRA_COMPILER_LAUNCHER`, and writes `sccache --show-stats`
    into the job summary. The `Restore ccache` / `ccache size limit` /
    `Save ccache` triples and their `CCACHE_DIR` entries are gone.
  - Requirement 5.1's list was stale the same way: `real-cloud-tests.yml` had
    ccache in five jobs, not two. `ami-build` gets the binary cache but no
    sccache — it builds an AMI rather than compiling this project.
  - **The invariant, checked mechanically rather than by reading:** every job
    that starts sccache also passes the launcher and reports statistics. It
    found two jobs whose configure steps do not share the common shape
    (`perf-cloud`'s prefix path is its last flag; `alibaba`'s block is indented
    differently). Violating that invariant is what made two of Task 1's runs
    measure nothing while reporting success.
  - `actionlint` caught a genuine bug: the launcher flag on
    `Configure (format-check)`, which runs *before* `Start sccache` — a forward
    reference. That tree compiles nothing and now has no launcher.
  - **Verified on `main` run 34296062792, 15/15 green**, with the
    `Full suite (stdexec)` leg — the hardest case — at **561 requests, 561
    hits, 100%, 0 write errors, 0 non-cacheable**, Build **4.1 min against 175
    cold**.
  - _Requirements: 5.1, 5.4, 5.5, 5.6, 5.7_

- [x] 7a. **Two findings from getting there, each worth its own change** —
      both written down September 9, 2026; neither *fixed* here, on purpose.
  - **A structural gap in the writer policy.** A leg that only ever succeeds on
    a `pull_request` can never warm its own cache, because PR runs hold the
    read-only key. `Full suite (stdexec)` hit exactly that: it passed once on a
    PR (banking nothing) and died on every `main` attempt, and because the
    runner is killed at VM level, sccache's pending uploads died with it — so
    failed attempts banked nothing either. A deadlock, not flakiness. It was
    broken by copying that leg's objects from Task 1's
    `sccache-measure-3/` prefix into `sccache/` (read + create only; no delete
    rights needed), after which the leg built in 4.1 minutes at a 100% hit
    rate. **design.md should record this beside the PR trade-off it already
    documents**, because it applies to any leg fragile enough to fail on
    `main` while passing on PRs.
  - **A latent CI landmine the cache now hides.** That leg died five times at
    objects 226, 236, 226, 226 (`-j3`) and 227 (`-j2`) of 1115 — a death point
    that stable across a changed parallelism setting is one translation unit,
    not aggregate memory pressure. The cluster is `multi_raft_scale_test` /
    `multi_raft_driver_agreement_test`, which sit **outside** the `heavy_tu`
    pool `ci.yml` built for precisely this failure mode. Any genuinely cold
    build — a compiler bump, a new `-D`, anything that moves the hash — will
    hit it again. **The fix is adding those TUs to the pool, as its own change
    with its own measurement**, not folded into the caching work.
    **CORRECTION, September 9, 2026, from the measurement this bullet asked
    for: the two targets named here are the wrong ones.** Ninja's `[N/M]`
    counter on a non-smart-terminal is a *completion* index — it buffers an
    edge's output and prints the status line when the edge finishes — so 226
    and 227 are the last TUs to FINISH before the machine stopped, not the
    ones that stopped it. That also explains the stability across `-j2` and
    `-j3` that this bullet read as evidence for a single TU: what finishes
    last is fixed by declaration order, not by parallelism. Differencing every
    `tests/` object against the ones job `102095938462` actually completed
    leaves exactly two — `multi_raft_http_benchmark_test.cpp.o` and
    `multi_raft_performance_report.cpp.o`, which never completed at all, and
    which peak at **21,114 and 16,640 MiB** under stdexec — 36.9 GiB together,
    against a runner's 16 GiB plus the 24 GiB swapfile that leg adds. All four are now in the
    pool; see `doc/TODO.md` and the top-level `CMakeLists.txt`.
  - The `-j2` experiment that led here was **refuted** and its PR
    ([#323](https://github.com/crawlins/kythira/pull/323)) closed unmerged
    rather than landed, so `main` keeps `-j3`.
  - **Both are now recorded where the next person will meet them**, which is
    all this task claims: the writer-policy deadlock is in `design.md`
    beside the PR trade-off it qualifies, and the `heavy_tu` gap is a
    "Known Follow-ups" entry in `doc/TODO.md` carrying the five death
    points, the `-j2` refutation, and the reason it is deliberately not
    folded into this spec. Filing rather than fixing is the point: the pool
    change needs its own cold-build measurement, and a cold build is
    precisely what this spec spent three weeks eliminating.

- [x] 8. Actions-cache accounting, re-measured — **done September 9, 2026, and
      it refutes a premise rather than confirming one**

    | | Aug 2026 (header) | 8 Sep (before) | 9 Sep (after) |
    | --- | ---: | ---: | ---: |
    | total | 10.37 GB / 41 | 10.00 GiB / 12 | **8.67 GiB / 11** |
    | `vcpkg` trees (kept) | 4.49 GB | 9.40 GiB / 7 | **8.17 GiB / 6** |
    | `ccache` families (removed) | — | 0.60 GiB / 5 | **0.50 GiB / 5** |

  - **Moving the compiler cache out did not relieve the ceiling.** ccache was
    **0.60 GiB**, not the "eight families at 2 GB each" the Introduction
    assumes — small precisely *because* it was being evicted constantly, which
    is the spec's own "cold every run" observation seen from the other end.
    Most of the 1.33 GiB drop is one vcpkg tree entry expiring, not the move.
  - **The ccache entries are still listed even though nothing writes them.**
    GitHub keeps a cache until eviction or its retention window elapses, so
    they age out rather than vanish; any reading taken within a week of the
    move is transitional by construction. The task text says "after two days"
    — that is too soon to see them gone, and this reading is honest about
    being early rather than waiting to look tidier.
  - **What changed is that eviction stopped mattering.** A tree-cache miss cost
    69–163 minutes of rebuilding and now costs a 4–7 minute download. The
    repository still sits near the ceiling; being evicted is simply no longer
    expensive. That is a better outcome than the one argued for, and a
    different one — and `doc/TODO.md` and the close-out should say so rather
    than repeat the original story.
  - `scripts/prune-actions-caches.sh`'s header is rewritten with the measured
    figures and the two misreadings spelled out. **The rules are unchanged** —
    verified mechanically: of 56 changed lines, **zero are non-comment**, and
    `shellcheck` reports the identical three pre-existing findings before and
    after. The `--superseded` rule is now mostly historical, since the run-id
    keys it targets left `ci.yml` with Task 7; it stays because any future
    `actions/cache` key carrying a run id recreates the pattern, and because
    `--closed-prs` is about refs rather than families.
  - _Requirements: 3.4_

  - After Tasks 6 and 7 have run on `main` for two days, re-run the
    measurement in `scripts/prune-actions-caches.sh`'s header (total bytes,
    entry count, bytes per family) and rewrite that header's accounting
    paragraph with the new figures. The rules do not change.
  - Record before/after here.
  - _Requirements: 3.4_

- [x] 9. Three-state credential verification and bad-endpoint run — **all four
      states verified September 9, 2026. The fourth took two attempts: the
      first found two real Requirement 3.5 violations, and the re-run against
      the fixes is green.**

  | State | Run | Result |
  | --- | --- | --- |
  | `push` to `main` writes | [34353211972](https://github.com/crawlins/kythira/actions/runs/34353211972) | 11 legs took the read-write key; 3 objects written; **0 write errors** |
  | `pull_request` reads, cannot write | [34349573736](https://github.com/crawlins/kythira/actions/runs/34349573736) | every leg read-only; **write errors == misses, exactly**; count unchanged |
  | secrets withheld | [34350454484](https://github.com/crawlins/kythira/actions/runs/34350454484), `Coverage` | `enabled=false`, `Start sccache` **skipped**, job **green** |
  | bad endpoint, take one | [34350454484](https://github.com/crawlins/kythira/actions/runs/34350454484), `ThreadSanitizer` | **FAILED — see below.** Two violations found, fixed by PRs [#326](https://github.com/crawlins/kythira/pull/326) and [#328](https://github.com/crawlins/kythira/pull/328) |
  | bad endpoint, take two | [34358385332](https://github.com/crawlins/kythira/actions/runs/34358385332), `ThreadSanitizer` | **15/15 green**; 117 ports built from source; both probes warned; no compiler cache |

  - **The writer policy is proven by a contrast, not by a count.** The same
    translation unit — the one clang TU whose command line genuinely varies
    between runs — misses on all three clang legs in both runs. On the
    `pull_request` it produced a write error each time and the bucket did not
    move. On the `push` to `main` it produced an object each time. Reading the
    bucket by creation time attributes every write in the window with nothing
    left over: three objects of 10.6–11.2 MB at 12:50:02, 12:51:13 and
    12:56:39 for run 34353211972's three clang misses, then two of **555 and
    509 bytes** at 12:57:35 and 12:57:50 — the new probe's own object, one per
    architecture — and three more large ones for run 34354004247. 4,521 →
    4,529 across both runs, fully accounted for. IAM refuses the read-only
    write independently of the case statement in the action, so the policy is
    enforced from both sides.
  - **The withheld-secrets state is visible in step conclusions alone**, which
    is the point of making it a distinct state: `OCI build cache` **succeeds**
    with `enabled=false` rather than failing, `Start sccache` is **skipped** by
    its `enabled == 'true'` guard, `Configure` falls back to
    `KYTHIRA_COMPILER_LAUNCHER=none`, and the leg builds and tests green with
    no compiler cache at all. Requirement 9.4 holds: a fork PR, which GitHub
    denies secrets, still builds.

  - **The bad-endpoint run did not go green, and finding that is what this
    task was for.** `Build (ThreadSanitizer)` failed **one second** after it
    started, 12:24:55 to 12:24:56, with `Configure` green ahead of it. The
    first compile in the job — the precompiled header — died:

    ```
    [1/17] Building CXX object tests/CMakeFiles/kythira_test_pch.dir/cmake_pch.hxx.gch
    FAILED: [code=2] ...
    /usr/local/bin/sccache /usr/bin/g++-13 ...
    sccache: error: Server startup failed: cache storage failed to read: ConfigInvalid (permanent)
       uri: https://nonexistent-namespace-for-task-9.../.sccache_check
       path: .sccache_check
    ninja: build stopped: subcommand failed.
    ```

    Requirement 3.5 says a cache failure is a warning in the log, never a red
    job. It was a red job. **Root cause: sccache validates its storage backend
    lazily**, on the first request, with that `.sccache_check` read — not at
    server startup — so neither subcommand the guard used could see a broken
    backend. Reproduced locally against the pinned sccache 0.17.0 rather than
    inferred from the timing:

    | case | `--start-server` | `--show-stats` | probe compile |
    | --- | ---: | ---: | ---: |
    | healthy, no server yet | 0 | 0 | 0 |
    | healthy, already listening | 2 | 0 | 0 |
    | backend unreachable | 2 | **0** | **2** |

    The `|| sccache --show-stats` fallback existed for the middle row — the
    lakers port's cargo build leaves a healthy server listening, so
    `--start-server` exits 2 with "Address in use", and reading *that* as
    failure cost Task 1 an entire run. But `--show-stats` returns 0 for the
    bottom row too. The guard could not tell a live server from a dead one,
    chose sccache over a dead one, and every compile then exited 2.
  - **Fixed in PR [#326](https://github.com/crawlins/kythira/pull/326)**
    (merged to `main` as `cc11a5b`): the decision is now a probe compile of a
    five-byte C file, the only check that separates all three rows, followed by
    `sccache --zero-stats` so the probe stays out of the figures every other
    task in this spec quotes. The guard was duplicated verbatim at **twelve
    sites across three workflows** and is now one script,
    `scripts/oci-build-cache/start-sccache.sh`, which exits 0 in every case —
    it can only ever downgrade a job to no compiler cache, never fail one. A
    fourth direction was verified too: with sccache **not installed at all**
    (the composite action's transient-download-failure path) it warns and
    degrades identically. The healthy path is confirmed on `main` in run
    [34354004247](https://github.com/crawlins/kythira/actions/runs/34354004247).
  - **A second violation, on the same requirement, that the first fix
    structurally could not catch.** `RUSTC_WRAPPER=sccache` was exported
    unconditionally, and the lakers port's cargo build runs under it *during*
    `Bootstrap vcpkg and install dependencies` — which happens before any
    per-job guard runs. Measured: `sccache rustc --version` exits 2 against an
    unreachable endpoint, so cargo fails, the port fails, the install fails,
    and the job goes red. It had never fired only because the tree cache had
    been hitting on every run, and a hit skips the install entirely. Found by
    predicting it before spending a run on it, and fixed in PR
    [#328](https://github.com/crawlins/kythira/pull/328) by probing inside the
    action and gating the export.

  - **Take two (run
    [34358385332](https://github.com/crawlins/kythira/actions/runs/34358385332)):
    15 of 15 green, including the deliberately broken leg.** Requirement 3.5
    and Requirement 7.3 both satisfied, and the run doubles as the controlled
    comparison this spec had never managed — every leg took the same L1 miss
    on the same commit, and the only variable was whether the binary cache was
    reachable:

    | Leg | vcpkg install |
    | --- | ---: |
    | `Full suite (stdexec)` | 4.41 min |
    | `Build & Test (g++-13, x64)` | 4.66 min |
    | `GCP SDK Build` | 4.71 min |
    | `Build & Test (clang++-18, x64)` | 4.96 min |
    | `Build & Test (clang++-18, arm64)` | 5.11 min |
    | `Build & Test (g++-13, arm64)` | 5.46 min |
    | `Ion Serializer Build` | 5.90 min |
    | `Coverage (clang++-18)` | 6.00 min |
    | `Build & Test (g++-14, x64)` | 6.28 min |
    | `Full suite (boost)` | 6.51 min |
    | **`ThreadSanitizer`, endpoint broken** | **95.01 min**, **117 ports from source** |

    Every earlier figure for this ratio compared different runs on different
    days. This is one run, and the ratio is **15 to 21×**. The 117 ports match
    the 117 of 131 that run 33997439418 built cold in 70 m 30 s, so the
    from-source path is the same one the project had before any of this.
  - **Both probes fired, an hour and a half apart**, which is the two-layer
    design working rather than redundancy:

    ```
    13:44:31  ##[warning]sccache could not serve a compile ...   <- the action's probe (#328)
    15:19:33  ##[warning]sccache could not serve a compile ...   <- the job's Start sccache (#326)
    ```

    The first is what let the 95-minute install run at all: it gated
    `RUSTC_WRAPPER` off before vcpkg started, so lakers' cargo build ran
    unwrapped. The second set `KYTHIRA_COMPILER_LAUNCHER=none`, after which
    `Build (ThreadSanitizer)` took 4.8 minutes uncached, the `sccache
    statistics` step skipped itself, and the tests passed.
  - **What made take two possible was busting the L1 tree cache.** On take one
    the `Bootstrap vcpkg` step was *skipped* because `vcpkg_installed/` hit, so
    `x-aws` never ran and Requirement 7.3's "ports built from source" half was
    never exercised at all — the identical trap that cost Task 1 its second
    attempt. A comment appended to an overlay README moves
    `hashFiles('vcpkg.json', 'vcpkg-overlays/**')` without touching a portfile.
    **This is now twice that a verification has silently measured nothing
    because of that L1 hit.** Any future test of the vcpkg binary cache has to
    start by forcing the miss.
  - PR [#325](https://github.com/crawlins/kythira/pull/325) closed **unmerged**
    and its branch deleted, per Requirement 1.5. Nothing from it reaches `main`.
  - _Requirements: 3.5, 4.3, 7.2, 7.3, 9.4_

- [x] 10. Second-run thresholds — **every threshold met, September 9, 2026,
      on run [34353211972](https://github.com/crawlins/kythira/actions/runs/34353211972)
      (15/15 green), the second consecutive `push` to `main` after Task 7's
      populating run 34296062792**

    | Requirement | Threshold | Cold baseline | Measured | Margin |
    | --- | --- | --- | --- | --- |
    | 6.1 `build-and-test` Build | ≤ 15 min | 17–26 min | **2.23 – 4.05 min** | 3.7 – 6.7× |
    | 6.2 stdexec Build | ≤ 45 min | 170.3 min | **4.01 min** | 11× |
    | 6.2 stdexec swap after build | < 4 GiB | — | **88 KiB** | 5 orders |
    | 6.3 hit rate over cacheable | ≥ 90% | — | **99.82 – 100%** | — |
    | 6.3 non-cacheable calls | — | — | **0 of 4,494** | — |
    | 6.4 following `pull_request` | same as 6.3 | — | **99.82 – 100%** | — |

    Per leg, all eleven that compile anything:

    | Leg | Build | requests | hits | rate |
    | --- | ---: | ---: | ---: | ---: |
    | `Build & Test (clang++-18, arm64)` | 2.23 min | 561 | 560 | 99.82% |
    | `Build & Test (g++-13, arm64)` | 2.66 min | 561 | 561 | 100% |
    | `Build & Test (clang++-18, x64)` | 3.08 min | 561 | 560 | 99.82% |
    | `Build & Test (g++-13, x64)` | 3.36 min | 561 | 561 | 100% |
    | `Build & Test (g++-14, x64)` | 4.05 min | 561 | 561 | 100% |
    | `Full suite (boost)` | 3.68 min | 561 | 561 | 100% |
    | `Full suite (stdexec)` | 4.01 min | 561 | 561 | 100% |
    | `Coverage (clang++-18)` | 6.18 min | 542 | 541 | 99.82% |
    | `ThreadSanitizer` | 0.36 min | 9 | 9 | 100% |
    | `GCP SDK Build` | 0.08 min | 7 | 7 | 100% |
    | `Ion Serializer Build` | 0.08 min | 9 | 9 | 100% |

  - **Nothing needed investigating and no threshold was moved**, which is
    worth stating explicitly because this task's own text reserves the right
    to do neither. The closest any figure came to its ceiling was 6.1 at 27%
    of it.
  - **The 6.2 swap threshold is now measuring the wrong thing, and that is a
    result rather than a problem.** It was written against a leg that
    compiled 1115 objects and died doing it; the same leg now links 561
    cached objects and finishes with **88 KiB** of a 23 GiB swap file
    touched — the second `Full suite` leg used 8.0 KiB. The threshold was
    guarding memory pressure during compilation, and there is no longer any
    compilation to guard. It is kept as a regression tripwire: if it ever
    rises, the cache has stopped working, and that is exactly when the
    number becomes meaningful again.
  - **6.3's "zero ports built from source" is satisfied vacuously here, and
    the honest reading matters.** `Bootstrap vcpkg and install dependencies`
    was **skipped on all eleven legs** — the `vcpkg_installed/` L1 tree cache
    hit everywhere — so the vcpkg binary cache was not exercised on this run
    at all. Zero ports were built because vcpkg never ran, not because it ran
    and found everything. The requirement's own "when it runs at all" clause
    anticipates this. The demonstration that vcpkg's cache works on a real L1
    miss is Task 6's, on run 34296062792: 397 archives, zero from source.
  - **6.4 is satisfied by run
    [34349573736](https://github.com/crawlins/kythira/actions/runs/34349573736)**,
    a `pull_request` against an unchanged `vcpkg.json`, showing the same
    99.82–100% rates from the read-only key. The three clang legs' single
    genuine miss is the only difference between the two runs, and it is a
    property of that translation unit rather than of the key.
  - _Requirements: 6.1, 6.2, 6.3, 6.4_

- [ ] 11. Month-one audit and the cost cross-reference — **the cross-reference
      is done and the bill is READABLE; only the full-month window is still
      owed.** "The bill genuinely has to wait for a month to exist", which is
      what this line said until September 10, was **half wrong, and wrong for
      this spec's signature reason**: the bill existed and the audit could not
      see it. See "The fourth variant" below.
  - **September 9, 2026 baseline**, `audit.sh` against the real tenancy,
    exit 0:

    | | |
    | --- | ---: |
    | `vcpkg/x64-linux/` | 267 objects, 3,691 MiB |
    | `vcpkg/arm64-linux/` | 131 objects, 1,373 MiB |
    | `sccache/` | 4,535 objects, 2,282 MiB |
    | total | **~7.2 GiB** |

    Both lifecycle rules present and enabled (sccache/ 30 days, vcpkg/ 90).
    Both customer secret keys ACTIVE, created 2026-09-06T10:25:52Z (rw) and
    10:25:56Z (ro) — matching what Task 2 recorded. Leak check clean.
  - ~~**The usage API has no line yet**: "no Object Storage line yet this
    month". That is the honest month-one answer on September 9 and the reason
    this task stays open rather than being closed on a guess.~~
    **FALSE, and found on September 10, 2026.** The reasoning was sound and
    the input was wrong: there was an Object Storage line, and `audit.sh`
    could not see it. Run the same window two ways and the API answers
    differently —

        ungrouped:            1 item,  service null,      $0.052629
        --group-by service:   6 items, Object Storage     $0.051963

    The Usage API returns **one aggregate row for the whole tenancy with
    `service` absent** unless the caller groups by it, and the audit filtered
    on `select((.service // "") | test("Object Storage"))`. So the row came
    back, the filter dropped it, and `length == 0` printed as
    "no Object Storage line yet this month". Same credentials, same window,
    same API — the only variable was `--group-by`.
  - **That run also found the audit lying.** It reported
    `kythira-build-cache-rw: does not exist` about the credential CI was
    authenticating with at that moment, and answered the usage query with a
    404 that reads like a missing IAM policy. One cause: `try()` folded stderr
    into the captured value, and `oci iam compartment get --query` on the
    tenancy root exits 0 while writing "Query returned empty result, no output
    to show." to stderr — so that sentence became `tenancy_id` and was passed
    on as an OCID. **This is the third variant of one failure mode in this
    spec**: the first two were empty read as zero, this one a diagnostic read
    as a value. The UNKNOWN tally was separately dead — every `try` call site
    is a command substitution, so the counter incremented a subshell copy and
    the "N informational queries could not be read" summary could never fire,
    for the whole life of the script. Fixed in PR
    [#329](https://github.com/crawlins/kythira/pull/329), with three checks
    added to `test-audit.sh` that fail against the previous version. The stub
    had to be tightened first: its `iam user list` answered with a valid user
    id whatever compartment it was handed, so it could not reproduce the bug
    at all.
  - **September 10, 2026, month-to-date (September 1–11), after the fix.**
    `audit.sh` exit 0, leak check clean:

    | Object Storage line | quantity | cost |
    | --- | ---: | ---: |
    | Requests | 202,831 | **$0.051963** |
    | Outbound Data Transfer Zone 1 | 130.11 GB | $0.000000 |
    | Storage | 0.902 GB-months | $0.000000 |
    | | | **$0.051963** |

    Bucket the same day: `vcpkg/` 267 + 131 objects at 3,691 + 1,373 MiB
    unchanged, `sccache/` **4,721 objects / 2,781 MiB**, up from 4,535 /
    2,282 on September 9. Both lifecycle rules enabled, both keys ACTIVE and
    unmoved.

    **Against the $0.50/month pre-registered: comfortable, and for a reason
    the pre-registration did not anticipate.** Requests are **100% of the
    cost**; storage and egress both round to zero (10 GB always-free, 10 TB
    egress). The bill scales with how many round trips sccache makes, not
    with bytes stored or moved — so the 30-day lifecycle rule, which bounds
    storage, does not touch the only line that costs anything.

    **The storage figure is self-checking**: 0.902 GB-months against ~7.5 GB
    held for roughly 3.6 of 30 days is 0.90. The independent agreement is
    what says the reading is the right quantity and not a coincidence.

  - **Still owed, and it is now only this:** re-run on or after **October 9,
    2026** for a *full month* at steady cadence, and set the egress against
    the **1 to 5 TB** pre-registered. 130 GB in eleven days is not 1/30th of a
    month — the caches went live on `main` on September 9, and September 10
    alone carried six pull requests at ~15 jobs each. The window is short and
    unrepresentative in both directions; it neither confirms nor refutes the
    band.
  - ~~Add the one-paragraph cross-reference to
    `doc/sccache_dogfood_cost_estimate.md` with the measured figure.~~
    **Done September 10, 2026**, as "Measured against this estimate". It
    leads with the fact that the document prices a three-node
    `redis_gateway_node` deployment that was never built, so its ~$55/month
    headline is not falsified by anything here; what is comparable is the
    workload. The estimate's central thesis — that egress is the expensive
    part and OCI's allowance zeroes it — **holds**: 130 GB billed at zero,
    which on AWS is $11–16 for this partial window alone. What the estimate
    lacks is any request-count row, which is the entire bill.

  - **THE FOURTH VARIANT of this spec's one failure mode**, and the most
    expensive, because it is the one that held this task open. The pattern in
    all four is *something that could not answer being read as an answer*:

    | | read as |
    | --- | --- |
    | 1. `--show-stats` exits 0 over a dead server | a healthy server ([#326](https://github.com/crawlins/kythira/pull/326)) |
    | 2. `RUSTC_WRAPPER` set before any guard could run | a guarded wrapper ([#328](https://github.com/crawlins/kythira/pull/328)) |
    | 3. a stderr diagnostic captured as stdout | an OCID ([#329](https://github.com/crawlins/kythira/pull/329)) |
    | 4. an **ungrouped aggregate** with no `service` field | "no Object Storage line" |

    The rule that would have caught all four, now written at the site: **when
    a query comes back empty, say whether the QUERY failed or the SUBJECT is
    absent, and never let those two print the same sentence.** `audit.sh` now
    distinguishes three outcomes — no rows at all (UNKNOWN, and explicitly
    "not the same as a zero bill"), rows for other services but not this one
    (genuinely absent, and it says how many others came back), and a real
    line.

    **Four checks added to `test-audit.sh`, and all four fail against the
    previous `audit.sh`** — verified by running the new suite against
    `git show HEAD:scripts/oci-build-cache/audit.sh`. The stub had to be
    fixed first, the same way task 9's did: it answered with a
    service-labelled row whether or not the caller grouped, so it could not
    reproduce the bug at all. It now models `--group-by` as the real API
    does. *A stub more permissive than the service it stands in for tests
    nothing* — that sentence was already in this file, about `iam user list`,
    and it was true a second time.
  - _Requirements: 6.5, 7.4, 8.3_

- [x] 12. TODO row and close-out — **September 9, 2026** (the row and the
      7.1 audit; Tasks 10 and 11 will amend the row when they land, which is
      why the spec reads 9/12 rather than 12/12)
  - **`doc/TODO.md`'s row rewritten and moved to "Partially Implemented"**,
    with a paragraph after the "Not Started" table recording the move in the
    manner that table uses for every other spec that has left it. The row
    deliberately leads with what was *measured* — installs 69–163 → 4–7 min,
    `Full suite (stdexec)` Build 170.3 → 4.1 min at a 100% hit rate over 561
    TUs, zero non-cacheable calls across 3,856 compiles — and then with the
    two premises that did not survive, because a row that only records the
    win teaches nothing about how the estimate was wrong.
  - **The correction that matters is the ceiling one.** The spec argued for
    moving the compiler cache out of GitHub's 10 GB Actions cache to relieve
    that ceiling; ccache turned out to be 0.60 GiB of it, and 94% is the
    `vcpkg_installed/` trees Requirement 3.3 keeps on purpose. The ceiling
    was never the problem. What changed is that **eviction stopped being
    expensive** — a tree-cache miss cost 69–163 minutes of rebuilding and now
    costs a 4–7 minute download. Both the row and Task 8 say so plainly
    rather than quietly restating the benefit as if it had been the claim.
  - **Requirement 7.1 audited task by task**, and it holds for every task
    whose claim a CI run can demonstrate:

    | Task | Run(s) cited | Job / evidence |
    | --- | --- | --- |
    | 1 | 34030385117, 34087833341, 34133234910 | the nine measured legs plus the unmodified `ion-serializer-build` control |
    | 3 | the same three | each leg's "Select credential and export the cache environment" step (citation added by this task) |
    | 6 | 34296062792 | 15/15 jobs on a `push` to `main`; `build cache: read-write key`; 397 archives / 5.06 GB, zero ports from source |
    | 7 | 34296062792 | `Full suite (stdexec)`: 561 requests, 561 hits, 0 write errors, 0 non-cacheable |
    | 9 | pending | PR [#325](https://github.com/crawlins/kythira/pull/325), DO NOT MERGE |
    | 10 | pending | the second consecutive `push` to `main` |

  - **Four tasks cite no run id and correctly cannot.** 1a is a PR closed and
    a branch deleted (verified against `origin/main` and the remote, not a
    run); 2 is provisioning read back by `audit.sh` against the real tenancy;
    4 is CMake behaviour across seven states, each a separate configure in a
    scratch tree on this box — a runner would tell you *less*, since it has
    one launcher installed; 5 and 8 are documents and a `gh cache list`
    measurement. Requirement 7.1 exists because ccache once cached to a
    directory nothing restored, and a green run hid it; naming a run id for a
    claim no run demonstrates would be that same failure wearing a citation.
  - _Requirements: 7.1, 8.4_

## Deferred

- sccache inside vcpkg port builds via a custom triplet (design.md,
  "Deferred").
- Pointing sccache at the Redis gateway; that is
  `doc/sccache_dogfood_cost_estimate.md`'s deployment, and this spec is its
  baseline.
