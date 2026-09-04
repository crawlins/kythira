# Design Document — OCI-Hosted Build Cache (vcpkg binary cache + sccache)

## Overview

This design moves two caches into one OCI Object Storage bucket and changes
nothing about what CI builds. It has three moving parts that decide whether
it works:

1. **A composite action** (`.github/actions/oci-build-cache/`) that picks the
   credential for the triggering event and exports one environment that
   both vcpkg's `x-aws` backend and sccache's `s3` backend read. The writer
   policy lives here and nowhere else.
2. **A CMake launcher switch** (`KYTHIRA_COMPILER_LAUNCHER`) that replaces
   the ccache-only auto-detection with an explicit choice, so CI can say
   `sccache` and a developer's machine keeps saying `ccache`.
3. **A guarded start** in every leg — `sccache --start-server` first, the
   launcher exported only if that worked — so the bucket is an accelerant
   and never a dependency.

Everything else is provisioning, measurement and documentation in support of
those three.

## Architecture

```
GitHub-hosted runner (x64 or arm64)
┌────────────────────────────────────────────────────────────────────────┐
│ .github/actions/oci-build-cache  (Requirement 4)                        │
│   event == push && ref == main  → RW key, VCPKG mode=readwrite           │
│   event == pull_request         → RO key, VCPKG mode=read                │
│   no key available (fork PR)    → export nothing, caches off             │
│   exports: AWS_ENDPOINT_URL, AWS_REGION, AWS_ACCESS_KEY_ID,              │
│            AWS_SECRET_ACCESS_KEY, VCPKG_BINARY_SOURCES,                  │
│            SCCACHE_BUCKET, SCCACHE_ENDPOINT, SCCACHE_REGION,             │
│            SCCACHE_S3_KEY_PREFIX, SCCACHE_S3_USE_SSL, RUSTC_WRAPPER,     │
│            VCPKG_KEEP_ENV_VARS                                           │
└──────────────┬───────────────────────────────────────┬─────────────────┘
               │                                       │
   vcpkg install (only on a                 sccache --start-server
   vcpkg_installed/ L1 miss)                  ok → cmake -DKYTHIRA_COMPILER_LAUNCHER=sccache
      │  x-aws  →  aws s3 cp                  fail → cmake (no launcher), warning
      ▼                                                │
┌──────────────────────────────────────────────────────▼─────────────────┐
│ OCI Object Storage bucket  <OCI_BUILD_CACHE_BUCKET>                     │
│   vcpkg/x64-linux/<abi-hash>.zip        lifecycle: 90 days              │
│   vcpkg/arm64-linux/<abi-hash>.zip                                      │
│   sccache/<h>/<h>/<h>/<hash>            lifecycle: 30 days              │
│   S3 Compatibility API endpoint:                                        │
│   https://<namespace>.compat.objectstorage.<region>.oraclecloud.com     │
│   IAM: kythira-build-cache-rw (read+write), kythira-build-cache-ro (read)│
└────────────────────────────────────────────────────────────────────────┘
```

What stays: the `vcpkg_installed/` tree cache in `actions/cache` (the L1),
the `heavy_tu` pool, `-j3` and the swapfile on the stdexec leg, and ccache
for local developers.

What goes: `x-gha`, the "Export Actions cache credentials for vcpkg" step,
and every "Restore ccache" / "ccache size limit" / "Save ccache" triple on a
leg that moves.

## Components and Interfaces

### 1. Provisioning — `scripts/oci-build-cache/provision.sh` (Requirement 2)

OCI CLI, idempotent, dry-run by default, `--apply` to create. In order:

1. Resolve the Object Storage namespace (`oci os ns get`) and print it; it
   becomes the `OCI_BUILD_CACHE_NAMESPACE` repository variable.
2. Create the bucket in `OCI_CI_COMPARTMENT_ID` with `--public-access-type
   NoPublicAccess`, `--storage-tier Standard`, versioning disabled, and the
   freeform tag `kythira-spec=oci-build-cache` that the audit keys on.
3. Put the lifecycle policy: two `DELETE` rules, `sccache/` at 30 days and
   `vcpkg/` at 90, both on `object-name-filter` prefixes.
4. Create the group `kythira-build-cache`, the users
   `kythira-build-cache-rw` and `kythira-build-cache-ro` in it, and two
   policies scoped with `where target.bucket.name = '<bucket>'`:

   ```
   Allow group kythira-build-cache to read buckets in compartment id <c>
     where target.bucket.name = '<bucket>'
   Allow user kythira-build-cache-rw to manage objects in compartment id <c>
     where all { target.bucket.name = '<bucket>',
                 any { request.permission = 'OBJECT_READ',
                       request.permission = 'OBJECT_INSPECT',
                       request.permission = 'OBJECT_CREATE',
                       request.permission = 'OBJECT_OVERWRITE' } }
   Allow user kythira-build-cache-ro to read objects in compartment id <c>
     where target.bucket.name = '<bucket>'
   ```

   `OBJECT_DELETE` is granted to nobody; expiry is the lifecycle policy's
   job, and a leaked key that cannot delete cannot empty the cache.
5. Create one customer secret key per user and print both pairs once, with
   the four `gh secret set` commands to run. The script never writes them
   to disk, and re-running it does not mint new keys unless `--rotate` is
   passed, which deletes the old key after printing the new one.

`scripts/oci-build-cache/audit.sh` lists, per prefix, object count and
bytes (`oci os object list --all` summed, since the compatibility API's
`ListObjects` does the same work), the lifecycle rules, the two users'
customer secret keys with their creation dates, the month-to-date egress
for the bucket from the usage API, and finally every resource in the
compartment carrying the spec tag, failing if one is not on the list above.
Task 2 runs it in the failing direction first by tagging a scratch bucket.

### 2. Composite action — `.github/actions/oci-build-cache/action.yml` (Requirements 3, 4)

Inputs are the secrets and variables named in Requirement 4.5, passed
explicitly by the caller (composite actions do not see `secrets.*`). Logic:

```bash
case "${GITHUB_EVENT_NAME}:${GITHUB_REF_NAME}" in
  push:main)  key_id="$RW_ID"; secret="$RW_SECRET"; mode=readwrite ;;
  *)          key_id="$RO_ID"; secret="$RO_SECRET"; mode=read ;;
esac
for v in OCI_BUILD_CACHE_BUCKET OCI_BUILD_CACHE_NAMESPACE OCI_CI_REGION; do
  [ -n "${!v}" ] || { echo "::error::$v repository variable is unset"; exit 1; }
done
if [ -z "$key_id" ] || [ -z "$secret" ]; then
  echo "::warning::no OCI build-cache credential for event ${GITHUB_EVENT_NAME}; both caches disabled"
  echo "enabled=false" >> "$GITHUB_OUTPUT"; exit 0
fi
endpoint="https://${OCI_BUILD_CACHE_NAMESPACE}.compat.objectstorage.${OCI_CI_REGION}.oraclecloud.com"
{
  echo "AWS_ENDPOINT_URL=$endpoint"
  echo "AWS_REGION=$OCI_CI_REGION"
  echo "AWS_ACCESS_KEY_ID=$key_id"
  echo "AWS_SECRET_ACCESS_KEY=$secret"
  echo "VCPKG_BINARY_SOURCES=clear;x-aws,s3://${OCI_BUILD_CACHE_BUCKET}/vcpkg/${TRIPLET}/,${mode}"
  echo "SCCACHE_BUCKET=$OCI_BUILD_CACHE_BUCKET"
  echo "SCCACHE_ENDPOINT=$endpoint"
  echo "SCCACHE_REGION=$OCI_CI_REGION"
  echo "SCCACHE_S3_KEY_PREFIX=sccache/"
  echo "SCCACHE_S3_USE_SSL=true"
  echo "RUSTC_WRAPPER=sccache"
  echo "VCPKG_KEEP_ENV_VARS=RUSTC_WRAPPER;SCCACHE_BUCKET;SCCACHE_ENDPOINT;SCCACHE_REGION;SCCACHE_S3_KEY_PREFIX;SCCACHE_S3_USE_SSL;AWS_ENDPOINT_URL;AWS_REGION;AWS_ACCESS_KEY_ID;AWS_SECRET_ACCESS_KEY"
} >> "$GITHUB_ENV"
echo "enabled=true" >> "$GITHUB_OUTPUT"
```

The secret values are masked automatically because they arrive as secrets.
`TRIPLET` is an input the caller resolves the way `ci.yml`'s "Resolve vcpkg
triplet" step does today. The action also downloads the pinned sccache
release for `runner.arch`, verifies its SHA-256 against a table in the
action, and installs it to `/usr/local/bin`.

Read-only on `pull_request` means sccache's write attempts fail with 403 and
are counted as `cache write errors`; the compile result is still returned
(`sccache/src/server.rs`, the same property `.kiro/specs/redis-compatible-kv/`
relies on). vcpkg's `read` mode never tries.

### 3. CMake — `KYTHIRA_COMPILER_LAUNCHER` (Requirement 5.2)

Replaces the `KYTHIRA_ENABLE_CCACHE` block in the root `CMakeLists.txt`:

```cmake
set(KYTHIRA_COMPILER_LAUNCHER "auto" CACHE STRING
    "Compiler launcher: auto (ccache, then sccache, then none), ccache, sccache or none")
set_property(CACHE KYTHIRA_COMPILER_LAUNCHER PROPERTY STRINGS auto ccache sccache none)
if(DEFINED KYTHIRA_ENABLE_CCACHE AND NOT KYTHIRA_ENABLE_CCACHE)
    message(DEPRECATION "KYTHIRA_ENABLE_CCACHE=OFF is now KYTHIRA_COMPILER_LAUNCHER=none")
    set(KYTHIRA_COMPILER_LAUNCHER "none")
endif()
set(_launcher "")
if(KYTHIRA_COMPILER_LAUNCHER STREQUAL "auto")
    find_program(_launcher NAMES ccache sccache)
elseif(NOT KYTHIRA_COMPILER_LAUNCHER STREQUAL "none")
    find_program(_launcher NAMES ${KYTHIRA_COMPILER_LAUNCHER})
    if(NOT _launcher)
        message(FATAL_ERROR "KYTHIRA_COMPILER_LAUNCHER=${KYTHIRA_COMPILER_LAUNCHER} but it is not on PATH")
    endif()
endif()
if(_launcher)
    set(CMAKE_C_COMPILER_LAUNCHER "${_launcher}" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_launcher}" CACHE STRING "" FORCE)
    message(STATUS "compiler launcher: ${_launcher}")
else()
    message(STATUS "compiler launcher: none")
endif()
```

`auto` preferring ccache is what keeps a developer's machine unchanged. The
`FATAL_ERROR` on an explicit name is the lesson of the `CCACHE_DIR`
incident: CI asking for sccache and silently getting nothing is the failure
this whole spec exists to end.

### 4. CI wiring per leg (Requirements 3, 5)

Each moved leg's step sequence becomes:

```yaml
- name: Resolve vcpkg triplet          # unchanged
- name: OCI build cache
  id: cache
  uses: ./.github/actions/oci-build-cache
  with:
    triplet: ${{ steps.triplet.outputs.value }}
    rw-access-key-id: ${{ secrets.OCI_BUILD_CACHE_RW_ACCESS_KEY_ID }}
    rw-secret-access-key: ${{ secrets.OCI_BUILD_CACHE_RW_SECRET_ACCESS_KEY }}
    ro-access-key-id: ${{ secrets.OCI_BUILD_CACHE_RO_ACCESS_KEY_ID }}
    ro-secret-access-key: ${{ secrets.OCI_BUILD_CACHE_RO_SECRET_ACCESS_KEY }}
    bucket: ${{ vars.OCI_BUILD_CACHE_BUCKET }}
    namespace: ${{ vars.OCI_BUILD_CACHE_NAMESPACE }}
    region: ${{ vars.OCI_CI_REGION }}
- name: Cache vcpkg packages           # unchanged (the L1)
- name: Bootstrap vcpkg and install dependencies
  if: steps.vcpkg-cache.outputs.cache-hit != 'true'
  run: |                               # VCPKG_BINARY_SOURCES now comes from the env
    git clone ... && vcpkg install ...
- name: Start sccache
  id: sccache
  if: steps.cache.outputs.enabled == 'true'
  run: |
    if sccache --start-server; then echo "launcher=sccache" >> "$GITHUB_OUTPUT";
    else echo "::warning::sccache could not reach the bucket; building without a compiler cache"; fi
- name: Configure
  run: cmake -B build -G Ninja ... -DKYTHIRA_COMPILER_LAUNCHER=${{ steps.sccache.outputs.launcher || 'none' }}
- name: Build
- name: sccache statistics
  if: always()
  run: sccache --show-stats | tee -a "$GITHUB_STEP_SUMMARY"
```

The "Restore ccache", "ccache size limit", "Save ccache" steps and the
`CCACHE_DIR` env entry are deleted on every moved leg. The `coverage` leg's
"Free disk space" step is unaffected. The `x-gha` credential export step is
deleted everywhere.

`gcp-sdk-build` keeps its distinct tree-cache key; its binary-cache prefix
is the same `vcpkg/x64-linux/` as every other x64 job, which is the point:
"the gcp tree is the edhoc tree plus google-cloud-cpp, so only the delta is
ever built" (`ci.yml`) is finally true across runs and not only within one.

### 5. `scripts/prune-actions-caches.sh` (Requirement 3.4)

No logic change. The header's accounting paragraph is rewritten after Task
8's re-measurement with the post-move figures: what remains in the Actions
cache is the tree caches and, for any leg that Requirement 5.1 kept on
ccache, its families.

## Data Models

### Bucket layout

| Prefix | Written by | Key | Lifecycle |
|---|---|---|---|
| `vcpkg/x64-linux/` | vcpkg `x-aws` from x64 `push` runs | `<abi-hash>.zip` | 90 days unmodified |
| `vcpkg/arm64-linux/` | vcpkg `x-aws` from arm64 `push` runs | `<abi-hash>.zip` | 90 days unmodified |
| `sccache/` | sccache from `push` runs | `<h>/<h>/<h>/<sha>` (sccache's own layout) | 30 days unmodified |

There is no per-leg or per-compiler prefix under `sccache/`; sccache's hash
covers the compiler binary, every flag, and the preprocessed input, so
entries from g++-13 and clang++-18, x64 and arm64, Release and coverage
cannot collide. One prefix keeps the lifecycle rule and the audit simple.

### Repository configuration

| Kind | Name | Meaning |
|---|---|---|
| variable | `OCI_BUILD_CACHE_BUCKET` | bucket name |
| variable | `OCI_BUILD_CACHE_NAMESPACE` | Object Storage namespace, for the endpoint host |
| variable | `OCI_CI_REGION` | already exists; reused |
| secret | `OCI_BUILD_CACHE_RW_ACCESS_KEY_ID` / `_SECRET_ACCESS_KEY` | `kythira-build-cache-rw`'s customer secret key |
| secret | `OCI_BUILD_CACHE_RO_ACCESS_KEY_ID` / `_SECRET_ACCESS_KEY` | `kythira-build-cache-ro`'s customer secret key |

## Correctness Properties

### Property 1: Absence is a no-op

With the bucket deleted, the secrets removed, or the endpoint unreachable,
every job builds exactly what it builds today and goes green. Verified by
Requirement 7.2's withheld-secrets run and 7.3's bad-endpoint run.

### Property 2: Only `main` writes

No object in the bucket was written by a run that was not a `push` to
`main`. Enforced by IAM (the `ro` user cannot create objects), not by the
workflow's choice of mode alone; a workflow bug that selected `readwrite`
on a PR would be refused by the service. Verified by Requirement 7.2.

### Property 3: The compiler cache never changes a build's output

sccache returns an object only when the preprocessed source and the full
invocation hash match; a hit is byte-identical to what the compiler would
produce. The target-list diff of Requirement 9.1 shows the launcher does not
change what is built.

### Property 4: The estimate is falsifiable, and says where

The pre-registered $0.50 per month and 1 to 5 TB egress are checked by the
audit script against the bill (Requirement 6.5). The per-leg bytes that the
estimate is least sure of are printed on every run (Requirement 5.5).

## Error Handling

| Failure | Behaviour |
|---|---|
| Bucket unreachable at `sccache --start-server` | warning; leg builds with no launcher |
| Bucket unreachable during `vcpkg install` | vcpkg warns per port and builds from source |
| Read-only key used for a write | 403 from OCI; sccache counts a write error and returns the compile; vcpkg `read` mode never writes |
| Repository variable unset | `::error::` naming it; job fails at the action, before any build time is spent |
| Secrets absent (fork PR) | action reports `enabled=false`; both caches off; build proceeds |
| sccache binary checksum mismatch | action fails the job: a wrong binary on the compile path is not something to warn about |
| Object expired between lookup and read | a miss; sccache compiles |
| Egress past 10 TB in a month | $0.0085/GB; the audit's month-to-date figure is the early warning |

## Testing Strategy

1. **Measurement first** (Requirement 1): the throwaway PR, its numbers into
   `tasks.md` Task 1.
2. **Audit in the failing direction** before it is trusted (Requirement 2.6).
3. **Three-state credential test** (Requirement 7.2) with real runs.
4. **Bad-endpoint test** (Requirement 7.3).
5. **Target-list diff** launcher on versus off (Requirement 9.1).
6. **Second-run thresholds** (Requirement 6) read from job summaries, not
   from the wall clock of a run that also populated the cache.
7. **Month-one audit** (Requirement 7.4).

## Dependencies

- sccache, a pinned release (0.10 or later; the S3 backend and
  `SCCACHE_ENDPOINT` behaviour this design relies on are documented for
  that line), installed by the composite action.
- AWS CLI v2 on the runner image, for vcpkg's `x-aws` (present on
  `ubuntu-24.04` and `ubuntu-24.04-arm`; the action asserts it).
- OCI CLI for the provisioning and audit scripts, run by an operator, not by
  CI.
- The existing OCI tenancy, compartment and region already used by
  `real-cloud-tests.yml`'s `oci` job.

## Deferred, with the design recorded

- **sccache inside vcpkg port builds.** Ports that build with CMake honour
  `CMAKE_CXX_COMPILER_LAUNCHER` from a triplet file; a custom triplet under
  `triplets/` setting it to sccache would cache the compiles of a cold
  Folly or AWS-SDK rebuild. Second-order once the binary cache persists,
  and it changes every port's ABI hash, so it is a separate change with its
  own measurement.
- **Pointing sccache at the Redis gateway.** `SCCACHE_REDIS_ENDPOINT`
  replaces the `SCCACHE_BUCKET`/`SCCACHE_ENDPOINT` pair in the composite
  action; the launcher, the guarded start and the writer policy are
  unchanged. That is the dogfood deployment in
  `doc/sccache_dogfood_cost_estimate.md`, and this spec is the floor it is
  measured against.
- **A PAR-based `http` backend for vcpkg** if a runner image ever ships
  without the AWS CLI. Same bucket, no credentials on the runner, the URL is
  the secret.
