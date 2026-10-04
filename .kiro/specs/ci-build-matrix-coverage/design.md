# Design Document — CI Build-Matrix Coverage

## Overview

Five new jobs in `.github/workflows/ci.yml`, two new defconfigs, two
allowlist files, two new scripts and two small CMake glob edits. No existing
job changes its feature set, defconfig or cache key.

| Job | Closes | Needs vcpkg | Runs tests |
|---|---|---|---|
| `kconfig-check` | G4 (Req 4) | no | no |
| `alt-coap-backends` | G2 (Req 1) | yes, `edhoc` + `coap-cantcoap` + `coap-libnyoci` | `coap_*` |
| `no-folly` | G1 (Req 2) | yes, `edhoc`, then a Folly-free prefix | whole suite |
| `config-variants` | G5, G6 (Req 5, 6) | yes, `edhoc` | one target |
| `static-analysis` | G3 (Req 3) | yes, `edhoc` | no |

All four vcpkg jobs run on `ubuntu-24.04` with `clang++-18` (the fastest
compiler this workflow builds with, per the comment on `build-and-test`'s
Release configure), and all of them start in parallel with `build-and-test`.

## Architecture

```
ci.yml (push to main, every PR)
├── build-and-test ×5          (unchanged)
├── gcp-sdk-build              (unchanged)
├── ion-serializer-build       (unchanged)
├── coverage / tsan / future-backend-compat ×2  (unchanged)
├── kconfig-check              NEW  python only, ~1 min
├── alt-coap-backends          NEW  configs/ci_alt_coap_defconfig, strict
├── no-folly                   NEW  configs/ci_no_folly_defconfig, strict,
│                                   Folly-free prefix
├── config-variants            NEW  minimal_defconfig (strict),
│                                   no_cloud_defconfig (non-strict),
│                                   verify-optional-dependency-isolation.sh
└── static-analysis            NEW  ci_full_defconfig, clang-tidy-18
```

Every vcpkg job reuses the setup block the existing single-purpose jobs
(`gcp-sdk-build`, `ion-serializer-build`) already copy: drop unused apt
repos, install system dependencies, install kconfiglib, install Rust (for
`edhoc`), resolve the triplet, `./.github/actions/oci-build-cache`, the
`actions/cache` vcpkg tree keyed on the feature set, the pinned vcpkg
bootstrap, then sccache. Factoring that block into a composite action would
shrink `ci.yml`, but it touches every existing job and so is left out of
this spec; each new job copies the block, as `ion-serializer-build` did.

## Component Design

### 1. `kconfig-check` job (Requirement 4)

```yaml
kconfig-check:
  name: Kconfig defconfig check
  runs-on: ubuntu-24.04
  timeout-minutes: 10
  steps:
    - uses: actions/checkout@v4
    - run: pip3 install --user -r scripts/kconfig/requirements.txt
    - run: python3 scripts/kconfig/check_defconfigs.py Kconfig configs
    - run: python3 scripts/kconfig/check_defconfig_usage.py
```

It calls the script behind the `kconfig-check` target directly rather than
the target, because the target only exists after a CMake configure, and a
configure needs the vcpkg tree. The script is the target's whole body
(`CMakeLists.txt:2050-2056`), so the two cannot drift.

`check_defconfigs.py` fails on any Kconfiglib warning. Kconfiglib does not
warn about an assignment to an undefined symbol unless `warn_assign_undef`
is set, and it is off by default, so as first written the script passed a
defconfig naming a removed symbol. It now sets `warn_assign_undef`,
`warn_assign_override` and `warn_assign_redun`, which is what
kconfig-integration Req 5.4 asks for.

`check_defconfig_usage.py` (new, ~40 lines) lists `configs/*_defconfig`,
greps `.github/workflows/*.yml` for each file name, and fails for any file
with no match that is not in `configs/defconfig-usage-allowlist.txt`. A match
on a comment line does not count. The allowlist is a plain `name  # reason`
file; an entry with no reason, or for a defconfig that is now applied or no
longer exists, is also an error. After this spec it is empty: every
defconfig is applied by some job.

### 2. `alt-coap-backends` job (Requirement 1)

**Defconfig.** `configs/ci_alt_coap_defconfig` is `ci_full_defconfig` plus:

```
CONFIG_COAP_TRANSPORT_CANTCOAP=y
CONFIG_COAP_TRANSPORT_LIBNYOCI=y
```

Its header says to keep every other line in sync with `ci_full_defconfig`,
the same rule `ci_ion_defconfig` already states. The `ci_full_defconfig`
header comment that says neither feature "is installed by any CI job" is
updated to point at this job, the way its `ION_SERIALIZER` note points at
`ion-serializer-build`.

**vcpkg.** `vcpkg install --x-feature=edhoc --x-feature=coap-cantcoap
--x-feature=coap-libnyoci`. The libnyoci overlay needs autotools and
`autoconf-archive`; the system-dependency step already installs both for
gperf, so no new apt package is needed. The vcpkg binary cache shares every
package with the `edhoc` jobs, so a warm run builds only the two overlay
ports, and a fully warm run builds nothing.

**Build and test.** Build only what the test filter needs:

```
cmake --build build --target $(ctest-derived list of coap_* test targets)
ctest --test-dir build -R '^coap_' -LE '^(slow|performance|verbose|benchmark|docker)$'
scripts/check-test-run.sh --build-dir build --log ctest.log --floor <N> -- -R '^coap_'
```

Restricting the build to the `coap_*` targets keeps the job to the delta the
alternate backends add. Running every `coap_*` test, not only the
`coap_cantcoap_*` and `coap_libnyoci_*` ones, covers Req 1.4: shared CoAP
headers compiled with `CANTCOAP_AVAILABLE`/`LIBNYOCI_AVAILABLE` defined take
code paths the default build does not.

**Proving the real suites ran (Req 1.3).** Three checks, cheapest first:

1. After configure, assert `generated/kythira/autoconf.hpp` defines both
   `CONFIG_COAP_TRANSPORT_CANTCOAP` and `CONFIG_COAP_TRANSPORT_LIBNYOCI`.
   Strict mode already guarantees the dependency was found; this guards
   against the defconfig not being applied at all.
2. `check-test-run.sh --floor` with the count of `coap_*` tests at the time
   the job lands.
3. Run each alternate-backend binary with `--list_content` and fail if the
   only case it lists is its stub case. This is the check that catches a
   backend silently compiling to its stub, which the other two do not.

### 3. `no-folly` job (Requirement 2)

**Defconfig.** `configs/ci_no_folly_defconfig`:

```
# CONFIG_FOLLY is not set
# CONFIG_PROXYGEN_TRANSPORT is not set     (depends on FOLLY; stated anyway)
CONFIG_BOOST_FUTURE_BACKEND=y
CONFIG_DEFAULT_FUTURE_BACKEND_BOOST=y
... every other ci_full_defconfig line ...
```

Boost rather than stdexec, because the `future-backend-compat` comment
records the stdexec leg as materially heavier to build, and because Boost
is a hard dependency that is always present. The choice of default backend
is not what this job tests; the absence of Folly is.

**Folly-free prefix (Req 2.2).** After the normal `vcpkg install`, a new
script `scripts/make-folly-free-prefix.sh <triplet> <out-dir>`:

1. Runs `vcpkg depend-info` over the manifest and computes the set of
   installed ports that are Folly or depend on it, transitively (today
   proxygen, wangle, fizz and their dependants). Computing the set, rather
   than hard-coding it, keeps the script correct when a port gains or loses
   a Folly dependency.
2. Copies `vcpkg_installed/<triplet>` to `<out-dir>` with `cp -al`
   (hard links, so no disk cost), then deletes from the copy each removed
   port's files, using `vcpkg_installed/vcpkg/info/<port>_*.list` as the
   file manifest.
3. Asserts `<out-dir>/include/folly` and every `libfolly*` are gone.

The job configures with `-DCMAKE_PREFIX_PATH=<out-dir>`. The original
`vcpkg_installed/` is never modified, so the vcpkg cache save step is
unaffected.

Hard links mean deleting from the copy cannot touch the original, but an
in-place write into the copy would. Nothing writes into a prefix during a
configure or build, and step 3's assertions run against the copy only.

**Assertions (Req 2.4).**

- `grep '^folly_FOUND:' build/CMakeCache.txt` is absent or `FALSE`.
- `grep -E 'libfolly|/folly/' build/build.ninja` is empty.

**Disabled-target report (Req 2.5).** The job runs `ctest -N` and writes the
test names to a file. A second step (in the same job, so no extra build)
runs `ctest -N` against a configure-only `ci_full_defconfig` build directory
(configure takes seconds, nothing is compiled) and diffs the two lists. Every
test present only in the full list must appear in
`configs/no-folly-test-allowlist.txt` with a reason; anything else fails the
job. The diff is also written to the job summary.

**Tests.** Full `ctest` with `build-and-test`'s label exclusion, then
`check-test-run.sh --floor`.

**As built (Tasks 6-7).** Three details changed on contact with the real
tree:

- The script reads `vcpkg_installed/vcpkg/status` (vcpkg's own record of
  each installed port's and feature's dependencies) instead of running
  `vcpkg depend-info`, because on a warm `actions/cache` hit the job never
  bootstraps vcpkg. Its usage is
  `make-folly-free-prefix.sh <vcpkg-installed-dir> <triplet> <out-dir>`.
- `folly_FOUND` is a plain variable, never a cache entry, so it cannot be
  grepped out of `CMakeCache.txt`. The job asserts instead that `folly_DIR`
  is absent or `-NOTFOUND`, which is what a successful `find_package(folly)`
  would leave behind.
- The test-list diff is `scripts/check-disabled-tests.py`, reading
  `ctest --show-only=json-v1` from both trees. It also rejects allowlist
  entries without a reason and stale entries (a test that is registered
  after all, or that the reference tree no longer has), so a fixed gate
  cannot leave a line behind that hides the next regression.

### 4. `config-variants` job (Requirements 5 and 6)

One vcpkg install (`edhoc`), then three independent steps, each with its own
build directory:

| Step | Configure | Build | Run |
|---|---|---|---|
| minimal | `configs/minimal_defconfig`, strict | all | none |
| no-cloud | `configs/no_cloud_defconfig`, **not** strict | `object_store_persistence_unit_test` | that test, `check-test-run.sh --floor 1` |
| isolation | the script's own configure | the script's own target | the script |

`minimal_defconfig` is built but not tested. Its tests are a subset of
`ci_full_defconfig`'s, which five legs already run; what nobody has checked
is that the configuration compiles and links. Building everything rather than
a sample is what Req 5.1 asks for and is what "smallest buildable
configuration" claims. sccache makes the overlap with the full build cheap.

`verify-optional-dependency-isolation.sh` hard-codes
`$REPO_ROOT/vcpkg_installed/$VCPKG_TRIPLET` as its prefix. That is where CI's
manifest-mode install puts the tree, so it works unmodified; Req 6.2 is met
by honouring an optional `KYTHIRA_PREFIX_PATH` override, defaulting to the
current value, so the script never needs editing to move.

### 5. `static-analysis` job (Requirement 3)

**Tooling.** `apt-get install clang-tidy-18` (Ubuntu 24.04's
`clang-tidy-18` package ships `run-clang-tidy-18`, which `CMakeLists.txt`
already searches for). The major version matches the `18.1.3` recorded at
the top of `.clang-tidy`.

**Generated files (Req 3.2).** clang-tidy reads `compile_commands.json`, but
the commands in it include generated headers. The job builds the
code-generation targets before analysis. Task 1 lists them (expected: the
Protobuf/gRPC `*.pb.h` generation targets and `autoconf.hpp`, which configure
already writes). If isolating them proves brittle, the fallback is a full
`cmake --build`, which sccache makes cheap on a warm run.

**Source set (Req 3.3).** `cmd/*.cpp` is added to `TIDY_SOURCES` and
`cmd/*.cpp`/`cmd/*.hpp` to `FORMAT_SOURCES`. The formatting churn and any
new findings land in the same change.

**Time budget (Req 3.5).** Unknown until measured; Task 1 measures a full
run. Expected shape: ~400 translation units, many template-heavy, on a
4-core runner. If a full run fits in 60 minutes it runs on every event. If
not, pull requests pass the changed `.cpp` files plus every `.cpp` whose
compile command depends on a changed header (from `ninja -t deps` after the
codegen build) to a new `static-analysis-files` target, and pushes to `main`
run the full tree. The scoped path is only built if the measurement calls
for it.

## Trade-offs

- **Separate jobs, not new matrix legs.** The `build-and-test` matrix shares
  one defconfig and one feature set by design; giving one leg a different
  defconfig would break the "every leg runs the same suite" property its
  comments rely on. Separate jobs also let each new gate fail with a name
  that says what broke.
- **Pruned prefix, not a second vcpkg manifest.** A Folly-free manifest
  would need either a second `vcpkg.json` or turning `folly` and `proxygen`
  into opt-in features, which changes every existing job's install line.
  Pruning a hard-linked copy costs seconds and touches nothing else. Its
  weakness is that it depends on vcpkg's `info/*.list` format; the script's
  own assertions catch that breaking.
- **x64 only.** None of these gaps is architecture-specific, and arm64
  runners are the scarcer resource.
- **No Folly-free `examples/` or `cmd/`.** The `FOLLY` Kconfig help already
  scopes them out; the job builds what the configuration enables and the
  allowlist records the rest.

## Error Handling

- A strict-mode configure that cannot satisfy a symbol fails with the
  existing `FATAL_ERROR`; no new handling.
- `make-folly-free-prefix.sh` uses `set -euo pipefail` and fails if
  `depend-info` returns nothing, if a `.list` file is missing for a port it
  meant to remove, or if any Folly file survives.
- `check_defconfig_usage.py` prints every offending defconfig, not just the
  first.

## Testing Strategy

Each job is validated the way this repository validates CI changes: the
draft PR's own run. In addition:

- **Negative checks, run once and recorded in the PR**, not checked in:
  add `#include <folly/Unit.h>` to a backend-independent header and confirm
  `no-folly` fails; rename a symbol in `Kconfig` and confirm `kconfig-check`
  fails; add an unused `configs/x_defconfig` and confirm the usage check
  fails; drop `--x-feature=coap-libnyoci` and confirm the stub-case check
  fails; introduce a `bugprone-*` finding and confirm `static-analysis`
  fails.
- `actionlint` and `shellcheck` (both pip-installable) on every changed
  workflow and script before pushing.
