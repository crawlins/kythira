# CI build caches

This repository keeps two caches, and they cache different things.

| | vcpkg binary cache | compiler cache |
|---|---|---|
| What one entry is | one **built port** — Folly, gRPC, OpenSSL … | one **object file** from this project's own source |
| Keyed by | vcpkg's ABI hash: port version, triplet, compiler, features, patches, and every dependency's hash | the preprocessed translation unit and the full compiler invocation |
| A hit saves | the port's whole build, minutes to tens of minutes | one compile, milliseconds to seconds |
| There are | a few hundred | tens of thousands |
| Backend | vcpkg's `x-aws` | sccache's `s3` |

Both live in one OCI Object Storage bucket, reached through OCI's Amazon S3
Compatibility API. The spec is `.kiro/specs/oci-build-cache/`; this document
is the operator's view of it.

There is a third cache that is not in the bucket and stays where it is: the
`vcpkg_installed/` **tree** cache, an `actions/cache` entry holding the whole
installed tree keyed on `hashFiles('vcpkg.json', 'vcpkg-overlays/**')`. It is
the L1 in front of the binary cache's L2 — a hit skips the vcpkg invocation
entirely.

## Layout

```
<bucket>/
  vcpkg/x64-linux/<abi-hash>.zip      lifecycle: delete after 90 days unmodified
  vcpkg/arm64-linux/<abi-hash>.zip
  sccache/<h>/<h>/<h>/<hash>          lifecycle: delete after 30 days unmodified
```

The triplets are kept apart by prefix for the same reason `runner.arch` is
part of the tree-cache keys: an arm64 job must not be able to restore an
x86_64 build.

There is deliberately **no** per-leg or per-compiler prefix under `sccache/`.
sccache's key already covers the compiler binary and every flag, so entries
from g++-13 and clang++-18, x64 and arm64, Release and coverage cannot
collide, and one prefix keeps the lifecycle rule and the audit simple.

The 30/90 asymmetry is the point of having two rules: a vcpkg archive is
minutes of build per object and there are few of them; an sccache object is
seconds and there are tens of thousands.

## Who may write

**Only a push to `main`.** Everything else — every pull request, every push to
every other branch — reads.

This is enforced twice, and the second one is the one that matters:

1. `.github/actions/oci-build-cache/` picks the read-write key for
   `push:main` and the read-only key for everything else. The match on `main`
   is exact; `main-experiment` gets the read-only key.
2. IAM refuses the rest. `kythira-build-cache-ro` holds `OBJECT_READ` and
   `OBJECT_INSPECT` and nothing else, so a bug in that case statement is
   answered with a 403 by the service rather than by poisoning a cache other
   builds trust.

`OBJECT_DELETE` is granted to **nobody**. Expiry is the lifecycle policy's
job, so a leaked key can at worst add to the cache.

### The trade-off, stated

A pull request that changes `vcpkg.json` or an overlay port builds its new
ports from source on **every one of its runs** until it merges, because it may
not write what it built. That is the cost of not letting a pull request poison
what `main` builds with, it is bounded by the tree-cache L1 (which PR runs do
still write), and it is exactly what such a PR does today whenever the Actions
cache has evicted its entry.

## Absence is a no-op

Nothing here is a build dependency. With the bucket deleted, the secrets
removed, or the endpoint unreachable:

- sccache's server fails to start, the leg's Configure step is passed no
  launcher, and the build compiles everything itself.
- vcpkg warns per port and builds those ports from source.
- The job goes green.

A fork's pull request has no secrets at all; the action reports
`enabled=false` and the run proceeds without either cache.

## What it is worth

Measured on this repository, not estimated —
`.kiro/specs/oci-build-cache/tasks.md` Task 1 has the full tables and the run
ids.

| | no cache | warm cache |
|---|---:|---:|
| `gcp-sdk-build` dependency install | 163.4 min | **6.5 min** |
| Other legs' dependency install | 69–99 min | **4–7 min** |
| `Full suite (stdexec)` Build | 170.3 min | **3.0 min** |
| `Full suite (boost)` Build | 80.9 min | **3.6 min** |
| `Build & Test` Builds | 36–60 min | **2.2–4.1 min** |
| sccache hit rate | — | **99.8–100%** |
| Ports built from source | all of them | **zero** |

Two caveats that matter more than the headline:

- **A cold cache costs nothing measurable.** Populating it moved build wall
  clocks by −2% to +6%, at 0.050 s average per cache write. You do not pay to
  fill it; you simply stop paying to rebuild.
- **The comparison is anchored.** `ion-serializer-build` is deliberately left
  without a binary cache, and it took 97.9, 97.8 and 92.2 minutes across the
  three runs the table is drawn from. When every cached leg drops by an order
  of magnitude and the uncached one does not move, the runners were not merely
  having a good day.

Every compile is cacheable: across 3,856 compiles spanning Release,
ThreadSanitizer and coverage builds, on two compilers and two architectures,
sccache reported **zero** non-cacheable calls. The `kythira_test_pch`
precompiled header does not defeat it, and neither does
`-fprofile-instr-generate` — the coverage leg cached 542 of 542 objects and
still passed its coverage floor, so cached instrumentation produces valid
coverage rather than merely fast builds.

## Reading the statistics

Every leg runs `sccache --show-stats` after its build with `if: always()`,
into the job summary. What to look at:

- **Cache hits over cacheable requests.** The threshold is 90% on a second
  consecutive `push` run to `main`. Below that, something is changing the
  compiler invocation between runs.
- **Non-cacheable requests, with sccache's own reason string.** A count equal
  to the number of `kythira_test_pch` users means the precompiled header is
  defeating the cache and the compiler cache is covering nothing that matters.
- **Cache write errors.** Expected and harmless on a pull request: that is the
  read-only key being refused. On a `push` to `main` they are a real problem.
- **Cache read/write bytes.** These are the egress figures the cost estimate
  in `doc/sccache_dogfood_cost_estimate.md` says to falsify first.

For vcpkg, the "Bootstrap vcpkg and install dependencies" step should show
zero ports built from source on a warm bucket. A port building from source
there means its ABI hash changed — a compiler bump, a new feature, an edited
overlay patch — or that the cache could not be reached, and the log says
which.

## Operating it

All three scripts live in `scripts/oci-build-cache/` and are run by an
operator, never by CI. They need the OCI CLI and `jq`.

```bash
# See what would be created; this is the default and it changes nothing.
scripts/oci-build-cache/provision.sh

# Create it. Prints the two customer secret keys ONCE, with the gh commands
# to set them. They are never written to disk.
scripts/oci-build-cache/provision.sh --apply

# What exists now, what it costs, and whether anything unexpected wears this
# spec's tag. Exit 1 means a leak, or a leak query that could not run.
scripts/oci-build-cache/audit.sh

# Prove the audit can fail before trusting it. Stub mode needs no tenancy.
scripts/oci-build-cache/test-audit.sh
scripts/oci-build-cache/test-audit.sh --live
```

### Rotating a key

```bash
scripts/oci-build-cache/provision.sh --rotate rw    # or: ro, both
```

It mints and prints the replacement **before** deleting the old key, so a
failure mid-rotation leaves a working credential rather than none. Set the new
values immediately — OCI returns a customer secret exactly once, and there is
no way to read it back:

```bash
gh secret set OCI_BUILD_CACHE_RW_ACCESS_KEY_ID     --body '...'
gh secret set OCI_BUILD_CACHE_RW_SECRET_ACCESS_KEY --body '...'
```

Rotating the **read-write** key between a push landing and the secret being
updated means `main` builds without a cache for those runs; nothing breaks.

### Repository configuration

| Kind | Name | Meaning |
|---|---|---|
| variable | `OCI_BUILD_CACHE_BUCKET` | bucket name |
| variable | `OCI_BUILD_CACHE_NAMESPACE` | Object Storage namespace, for the endpoint host |
| variable | `OCI_CI_REGION` | already exists; reused |
| secret | `OCI_BUILD_CACHE_RW_ACCESS_KEY_ID` / `_SECRET_ACCESS_KEY` | `kythira-build-cache-rw`'s customer secret key |
| secret | `OCI_BUILD_CACHE_RO_ACCESS_KEY_ID` / `_SECRET_ACCESS_KEY` | `kythira-build-cache-ro`'s customer secret key |

A missing **variable** fails the job at the action, with an `::error::` naming
it. Missing **secrets** are the fork case and disable the caches instead.

## Cost

Pre-registered at about **$0.50 per month**, in
`.kiro/specs/oci-build-cache/requirements.md` (Requirement 6.5): ten gigabytes
of standard storage is $0.26, roughly 700,000 requests beyond the free 50,000
is about $0.24, and the estimated 1 to 5 TB per month of runner traffic sits
inside OCI's 10 TB free egress allowance.

Read the citation carefully. That spec attributes those component figures to
`doc/sccache_dogfood_cost_estimate.md`, and **they are not in it** — that
document prices the *Redis-gateway* deployment (compute, block volume, egress;
about $55 per month) and says in as many words that Object Storage "this
deployment does not use". So the $0.50 is a pre-registration made in the spec,
not a figure carried over from a costed document, and it should be treated as
the weaker of the two claims until the first bill arrives. Closing that gap is
Requirement 8.3's cross-reference, owed by Task 11.

`audit.sh` prints the month-to-date figure so the estimate can be checked
before the bill rather than after it. For scale, one full population of the
cache is **397 vcpkg archives (5.06 GB)** and about **4,400 sccache objects
(2.04 GB)**; the lifecycle rules then hold it roughly steady. Over $5 in a month, or over 5 TB of
egress, is a falsified estimate to investigate — not a number to absorb.

## Local use

Nothing in this document is needed to build this project. A developer's
machine uses ccache, which `-DKYTHIRA_COMPILER_LAUNCHER=auto` picks by
default. `DEPENDENCIES.md` has the read-only recipe if you want to pull CI's
objects instead.
