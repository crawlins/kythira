# Design Document

## Overview

This document describes the design for verifying Kythira on 64-bit ARM Linux
by extending the existing CI/CD pipeline, not by introducing new
functional code. The approach is deliberately **native-runner-first**: GitHub
now provides hosted `ubuntu-24.04-arm` runners, so this design adds arm64 legs
to the existing job matrices rather than cross-compiling or running under
QEMU emulation. For a template-heavy C++23 codebase pulling in Folly, the AWS
SDK for C++, and Boost, native compilation avoids both a large speed penalty
and a class of "works under emulation, fails on real silicon" false
confidence.

The work has two halves:

1. **Remove x86_64 hardcoding** — the vcpkg triplet `x64-linux` is a literal
   string in `ci.yml`, `real-cloud-tests.yml`, four `CMakeLists.txt` files,
   and seven `Dockerfile`s. None of this is a deliberate x86_64-only design
   decision; it is unparameterized because arm64 was never exercised. This
   design introduces one computed value (`KYTHIRA_VCPKG_TRIPLET` in CMake,
   an equivalent shell expression in CI/Docker) and threads it through every
   site that currently hardcodes `x64-linux`.
2. **Add arm64 CI legs** — once the triplet is parameterized, add
   `ubuntu-24.04-arm` entries to the build-and-test matrix and to the
   real-cloud-tests AWS job, leaving the single-architecture format-check and
   coverage jobs alone (Requirement 8).

A pre-implementation spike (Requirement 10) is a hard gate on the whole design:
if `folly`, `aws-sdk-cpp`, or another dependency turns out to lack usable
`arm64-linux` vcpkg support at the pinned baseline, that has to be resolved
(baseline bump, overlay port, upstream fix) before any workflow file is
touched, or every subsequent CI job in this design fails for a
dependency-availability reason unrelated to Kythira's own code.

## Architecture

```
  ci.yml (after this spec)
  ─────────────────────────────────────────────────────────────────
  build-and-test:
    matrix:
      - { compiler: g++-13,    os: ubuntu-24.04     }  ← existing, x86_64
      - { compiler: clang++-18, os: ubuntu-24.04     }  ← existing, x86_64
      - { compiler: g++-13,    os: ubuntu-24.04-arm }  ← new, arm64 (native)
      - { compiler: clang++-18, os: ubuntu-24.04-arm }  ← new, arm64 (native)
        │
        ├── resolve KYTHIRA_VCPKG_TRIPLET  (x64-linux | arm64-linux)
        ├── cache key: vcpkg-${os}-${arch}-edhoc-${hash}   ← arch added
        ├── vcpkg install --triplet ${TRIPLET} --x-feature=edhoc
        ├── cmake -DCMAKE_PREFIX_PATH=vcpkg_installed/${TRIPLET}
        ├── build (native compiler for the runner's own arch)
        └── ctest -LE '^(slow|performance|verbose|benchmark|docker)$'

  coverage:        unchanged — x86_64 (ubuntu-24.04) only, Requirement 8
  format-check:     unchanged — x86_64 (g++-13 leg) only, Requirement 8
  docs:            unchanged — architecture-independent

  real-cloud-tests.yml
  ─────────────────────────────────────────────────────────────────
  aws:
    matrix:
      - { os: ubuntu-24.04,     ec2_arch: x86_64 }  ← existing
      - { os: ubuntu-24.04-arm, ec2_arch: arm64  }  ← new
        └── builds aws_quorum_manager_real_ec2_test natively for its own
            arch, so the __aarch64__ branch actually compiles+runs on the
            arm64 leg and provisions a Graviton (t4g.*) instance.
```

### Triplet resolution flow

```
Shell (workflows, Dockerfiles):
  ARCH=$(uname -m)                         # x86_64 | aarch64
  case "$ARCH" in
    x86_64)  TRIPLET=x64-linux ;;
    aarch64) TRIPLET=arm64-linux ;;
  esac

CMake (CMakeLists.txt, tests/*.CMakeLists.txt):
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(KYTHIRA_VCPKG_TRIPLET "arm64-linux")
  else()
      set(KYTHIRA_VCPKG_TRIPLET "x64-linux")
  endif()
  # every "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/..." literal becomes
  # "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/..."
```

Both computations must agree: the shell-side value is what vcpkg is told to
install (`--triplet`) and what `-DCMAKE_PREFIX_PATH` points at; the CMake-side
value is what the manually-linked static archive paths (Boost, PocoDNSSD) and
the Docker build stages resolve to. They are two independent implementations
of the same architecture-to-triplet mapping (shell can't easily invoke CMake
just to ask, and CMake configure needs the value without shelling out to
`uname` inconsistently across the Linux-only environments this project
targets) — kept in sync by the small, fixed 2-entry mapping documented in one
place (this design doc and a comment at each definition site).

## Component Design

### 1. `ci.yml` — build-and-test matrix expansion

The matrix gains an `os` axis:

```yaml
strategy:
  fail-fast: false
  matrix:
    include:
      - compiler: g++-13
        os: ubuntu-24.04
        arch_label: x64
      - compiler: clang++-18
        os: ubuntu-24.04
        arch_label: x64
      - compiler: g++-13
        os: ubuntu-24.04-arm
        arch_label: arm64
      - compiler: clang++-18
        os: ubuntu-24.04-arm
        arch_label: arm64
runs-on: ${{ matrix.os }}
```

Every step that references `x64-linux` gains a `steps.triplet.outputs.value`
(or equivalent) computed once per job from `runner.arch`:

```yaml
- name: Resolve vcpkg triplet
  id: triplet
  run: |
    if [ "${{ runner.arch }}" = "ARM64" ]; then
      echo "value=arm64-linux" >> "$GITHUB_OUTPUT"
    else
      echo "value=x64-linux" >> "$GITHUB_OUTPUT"
    fi
```

`runner.arch` is used instead of `uname -m` in the workflow-level step because
it is available before any shell runs and matches GitHub's own runner
labeling (`ARM64`/`X64`), which also lets Requirement 4's cache key reuse the
same context expression directly (`${{ runner.arch }}`) without depending on
this step's output.

Job/artifact names become
`Unit Tests (${{ matrix.compiler }}, ${{ matrix.arch_label }})` and
`test-results-${{ matrix.compiler }}-${{ matrix.arch_label }}` (Requirement
1.3). The format-check steps keep their existing
`if: matrix.compiler == 'g++-13'` guard — since that guard doesn't check
`arch_label`, it must be tightened to
`if: matrix.compiler == 'g++-13' && matrix.arch_label == 'x64'` so format
checking still runs exactly once (Requirement 8.1), not once per architecture.

`timeout-minutes` for the arm64 leg is set from a value measured during
implementation rather than copied from the x86_64 job's `120` (Requirement
1.5); this design does not predict that number since GitHub's arm64 runner
performance characteristics for this dependency set are exactly what
Requirement 10's spike (and the first real arm64 CI runs) will establish.

### 2. `KYTHIRA_VCPKG_TRIPLET` (CMake)

Defined once near the top of the root `CMakeLists.txt`, before any
`find_package` or `EXISTS` check that references a triplet path:

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(KYTHIRA_VCPKG_TRIPLET "arm64-linux")
else()
    set(KYTHIRA_VCPKG_TRIPLET "x64-linux")
endif()
```

Every existing literal of the form
`"${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/..."` — in `CMakeLists.txt`
(the `POCO_DNSSD_FOUND` block), `tests/CMakeLists.txt` (the repeated
`libboost_json.a`/`libboost_context.a` `EXISTS` checks across ~10 targets),
`tests/chaos/CMakeLists.txt`, and `tests/docker_chaos/CMakeLists.txt` — is
rewritten to
`"${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/..."`. This is
a mechanical find-and-replace once the variable exists; no behavioral change
on x86_64, since `KYTHIRA_VCPKG_TRIPLET` resolves to the same
`"x64-linux"` string that was previously hardcoded (Requirement 2.4).

### 3. Avahi multiarch discovery (Requirement 3)

```cmake
if(NOT DEFINED CMAKE_LIBRARY_ARCHITECTURE OR CMAKE_LIBRARY_ARCHITECTURE STREQUAL "")
    # Fallback for configurations where CMake didn't populate this (rare on
    # the Ubuntu images this project targets, but cheap to guard).
    if(KYTHIRA_VCPKG_TRIPLET STREQUAL "arm64-linux")
        set(_multiarch_tuple "aarch64-linux-gnu")
    else()
        set(_multiarch_tuple "x86_64-linux-gnu")
    endif()
else()
    set(_multiarch_tuple "${CMAKE_LIBRARY_ARCHITECTURE}")
endif()

find_library(AVAHI_CLIENT_LIB
    NAMES avahi-client
    PATHS /tmp/avahi-dev/usr/lib/${_multiarch_tuple}
          /usr/lib/${_multiarch_tuple}
          /usr/lib
    NO_DEFAULT_PATH
)
# AVAHI_COMMON_LIB analogous
```

`CMAKE_LIBRARY_ARCHITECTURE` is CMake's own multiarch-tuple detection
(populated from the compiler on Debian/Ubuntu-family systems); the
`KYTHIRA_VCPKG_TRIPLET`-derived fallback exists only for the unlikely case it
is empty, so the search never silently degrades to only the x86_64 path on an
arm64 host. `POCO_DNSSD_FOUND` keeps its existing `EXISTS`-gated
fail-open behavior (Requirement 3.2) — nothing about that control flow
changes, only the paths it searches.

### 4. Cache key architecture isolation (Requirement 4)

```yaml
- name: Cache vcpkg packages
  uses: actions/cache@v4
  id: vcpkg-cache
  with:
    path: vcpkg_installed
    key: vcpkg-${{ runner.os }}-${{ runner.arch }}-edhoc-${{ hashFiles('vcpkg.json', 'vcpkg-overlays/**') }}
```

Applied identically in `ci.yml` (both jobs that cache vcpkg) and
`real-cloud-tests.yml`. `runner.arch` renders as `X64` or `ARM64`, so the
post-change x86_64 key becomes `vcpkg-Linux-X64-edhoc-<hash>` — a cache miss
against the pre-change `vcpkg-Linux-edhoc-<hash>` key, which just means one
extra full vcpkg build on the first run after this change lands, not data
loss (Requirement 4.3). The arm64 key, `vcpkg-Linux-ARM64-edhoc-<hash>`, has
never existed, so its first run is a guaranteed miss followed by a
freshly-populated, architecture-isolated cache entry (Requirement 4.2).

### 5. Rust/`lakers` on arm64 (Requirement 5)

No code change is required in `vcpkg-overlays/lakers/portfile.cmake` — it
already parameterizes its cargo build-target directory by
`${TARGET_TRIPLET}` (`${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/cargo-target`),
and `cargo build --release` with no explicit `--target` builds for the host
triple by default, which is exactly `aarch64-unknown-linux-gnu` when running
natively on an arm64 runner. The only actual dependency is that
`dtolnay/rust-toolchain@stable` installs correctly on `ubuntu-24.04-arm`
(Requirement 5.1) — verified as part of the spike (Requirement 10) alongside
the vcpkg dependency check, since a missing/broken Rust toolchain on arm64
would block the `edhoc` feature the same way a missing vcpkg port would.

### 6. Docker image triplet parameterization (Requirement 6)

Every `docker/*/Dockerfile` builder stage currently hardcodes:

```dockerfile
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux && \
    cmake --build build --target <binary>
```

This becomes a build-arg-driven value, resolved from `uname -m` inside the
build stage so no build-time flag is required from the caller (keeping
`docker build` / `docker compose up` invocations in
`tests/docker_chaos/os_faults.hpp`'s `compose_prefix()`/`container_runtime()`
unchanged):

```dockerfile
RUN ARCH="$(uname -m)"; \
    case "$ARCH" in \
        aarch64) TRIPLET=arm64-linux ;; \
        *)       TRIPLET=x64-linux ;; \
    esac; \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/${TRIPLET} && \
    cmake --build build --target <binary>
```

Since these images are always built from a `COPY . .` of the checked-out
source tree (never a cross-platform image pull followed by a foreign-arch
`vcpkg_installed`), the base `ubuntu:24.04` image itself resolves to the
correct architecture automatically when Docker/Podman selects it for the
host's platform — no `--platform` flag or QEMU registration is introduced,
consistent with the native-build-only scope of this spec (Requirement 6.2).

### 7. Real Cloud Tests arm64 leg (Requirement 7)

```yaml
jobs:
  aws:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-24.04
            arch_label: x64
          - os: ubuntu-24.04-arm
            arch_label: arm64
    runs-on: ${{ matrix.os }}
    name: Real Cloud Tests (AWS, ${{ matrix.arch_label }})
```

The rest of the job is unchanged apart from the triplet/cache-key
parameterization from Requirements 2 and 4. Building
`aws_quorum_manager_real_ec2_test` on the arm64 leg means the
`#if defined(__aarch64__) || defined(__arm64__)` branch in that file
(instance-type fallback, AMI architecture filter, `target_arch` selection)
compiles for the first time in CI and, when the `ec2-quorum-manager` bundle
runs, provisions a real Graviton instance end-to-end — closing the gap
described in this spec's Introduction where that code has existed since the
`aws-quorum-manager` spec but was never actually exercised.

## Trade-offs

| Decision | Chosen | Alternative | Reason |
|---|---|---|---|
| Build strategy | Native `ubuntu-24.04-arm` runners | Cross-compile from x86_64, or QEMU-emulated arm64 | Native avoids emulation slowdown (significant for Folly/AWS-SDK/Boost-heavy builds) and avoids "passes under emulation, breaks on real hardware" false confidence; GitHub now provides these runners at no extra setup cost |
| Triplet detection (CI/Docker) | `runner.arch` context / `uname -m` shell check | A single hardcoded `--platform`-driven cross-build matrix | Keeps every job self-determining its own triplet from the runner it's actually on, matching the native-build decision above |
| Triplet detection (CMake) | New `KYTHIRA_VCPKG_TRIPLET` variable from `CMAKE_SYSTEM_PROCESSOR` | Pass the triplet in from CI via `-D` on every invocation | CMake can determine its own host architecture without relying on every caller (CI, local dev, Docker) remembering to pass a flag; reduces the number of places a mistake could hardcode the wrong value again |
| Coverage job | x86_64-only, not duplicated | Run coverage on both architectures | Line/function coverage % is not expected to vary by architecture; the job is already documented as disk/time-constrained on one architecture — duplicating it doubles cost for no signal (Requirement 8.2) |
| Format-check job | x86_64-only, not duplicated | Run format-check on both architectures | `clang-format` output is architecture-independent; running it twice can only ever agree with itself |
| Docker image cross-arch builds | Not supported (native-only) | `docker buildx` + QEMU multi-platform images | Out of scope per this spec's scope statement; the project has no existing multi-platform image publishing requirement, only "does it build and run on the host it's built on" |
| Real-cloud-tests arm64 leg | Add as a matrix entry to the existing `aws` job | Leave `__aarch64__` branch permanently uncompiled/manually tested | The whole point of Requirement 7 is that dead-in-CI code exercising real AWS provisioning is exactly the kind of gap this spec exists to close |
| Spike before implementation | Dedicated Requirement 10 spike, gating Requirements 1–7 | Discover vcpkg arm64 gaps mid-implementation via a red CI job | Mirrors this project's existing `stdexec-future-backend/spike-notes.md` precedent; a red arm64 job merged as "in progress" is worse signal than a documented pre-check |

## Components and Interfaces

| Component | Location | Interface / Change |
|---|---|---|
| Build-and-test matrix | `.github/workflows/ci.yml` | Adds `ubuntu-24.04-arm` × `{g++-13, clang++-18}`; triplet-resolution step; arch-qualified names |
| Coverage job | `.github/workflows/ci.yml` | Unchanged — stays x86_64-only |
| Format-check guard | `.github/workflows/ci.yml` | Guard tightened to also require the x86_64 matrix entry |
| Real Cloud Tests AWS job | `.github/workflows/real-cloud-tests.yml` | Adds `ubuntu-24.04-arm` matrix entry; triplet/cache-key parameterization |
| `KYTHIRA_VCPKG_TRIPLET` | `CMakeLists.txt` | New variable, computed from `CMAKE_SYSTEM_PROCESSOR`; replaces all `x64-linux` literals |
| Avahi discovery | `CMakeLists.txt` (~line 250) | `find_library` PATHS gain multiarch-tuple derivation |
| PocoDNSSD fallback | `CMakeLists.txt` (~line 242) | Unchanged fail-open logic; only the searched path changes |
| Boost static-lib fallback | `tests/CMakeLists.txt`, `tests/chaos/CMakeLists.txt`, `tests/docker_chaos/CMakeLists.txt` | `x64-linux` literal → `${KYTHIRA_VCPKG_TRIPLET}` |
| `lakers` overlay port | `vcpkg-overlays/lakers/portfile.cmake` | No change — already triplet-parameterized via `${TARGET_TRIPLET}` |
| Docker builder stages | `docker/*/Dockerfile` (7 files) | `uname -m`-derived `TRIPLET` shell variable replaces the hardcoded `-DCMAKE_PREFIX_PATH=.../x64-linux` |
| Spike findings | `.kiro/specs/arm64-ci-verification/spike-notes.md` | New file; per-dependency arm64-linux availability record |
| README | `README.md` | New "ARM (arm64) Support" section |
| TODO | `doc/TODO.md` | Marks this spec's item done |

## Error Handling

| Failure | Response |
|---|---|
| A vcpkg dependency has no `arm64-linux` port/binary at the pinned baseline | Caught by the Requirement 10 spike *before* implementation; documented as a blocking follow-up (baseline bump / overlay port / upstream fix), not discovered as a red CI job after merge |
| `dtolnay/rust-toolchain@stable` fails to install on `ubuntu-24.04-arm` | Caught by the spike; if it fails, the `edhoc` feature is dropped from the arm64 leg's `vcpkg install` invocation (documented as a known limitation, Requirement 9.2) rather than blocking the entire arm64 job |
| PocoDNSSD static libs absent on arm64 (expected — Requirement 3.2) | Not an error: `POCO_DNSSD_FOUND` stays `FALSE`, configuration succeeds, `poco_discovery_node`'s DNSSD backend is compiled out, exactly like any x86_64 host missing the archive today |
| arm64 leg exceeds its measured `timeout-minutes` | Job fails visibly (no silent `continue-on-error`); implementation task re-measures and adjusts the timeout, or investigates a genuine performance regression |
| Cache-key change causes a one-time cache miss on x86_64 | Expected and harmless (Requirement 4.3) — one extra full vcpkg build, then the new key is warm for all subsequent runs |
| Docker image build fails only on arm64 (e.g. an apt package genuinely missing an arm64 build) | Investigated per-image; if a hard blocker is found for one of the seven Dockerfiles, that image's arm64 support is documented as a specific, named limitation rather than silently skipped |

## Testing Strategy

Because this spec's deliverable *is* CI/CD configuration, its own tests are
the CI runs it adds:

- **Spike verification** (Requirement 10): a one-off, manually-run
  `vcpkg install --triplet arm64-linux` (on a real or emulated arm64 host,
  used only for this pre-check) against the full current dependency set,
  results recorded in `spike-notes.md`. This gates everything else.
- **Build-and-test arm64 legs** (Requirement 1): the existing `ctest` suite,
  run natively on `ubuntu-24.04-arm`, is itself the correctness test for
  Requirements 1–5 — a green run across both compilers on arm64 demonstrates
  the triplet parameterization, cache isolation, and Rust/`lakers` build all
  work together, exactly as the equivalent x86_64 legs already demonstrate
  today.
- **Docker image builds on arm64** (Requirement 6): each `docker-*-image`
  CMake target is built once on a native arm64 host as part of implementation
  verification; the docker-labeled scenario tests (`ctest -L docker`) are run
  against the resulting images to confirm parity with x86_64 behavior. These
  remain excluded from the default per-PR gate on both architectures,
  unchanged from current practice.
- **Real Cloud Tests arm64 leg** (Requirement 7): exercised via
  `workflow_dispatch` with the AWS master switch and `ec2-quorum-manager`
  bundle enabled, verifying an actual Graviton instance is provisioned,
  reachable, and torn down — the same acceptance bar already applied to the
  existing x86_64 leg.
- **Non-goal jobs** (Requirement 8): verified by inspection of the workflow
  YAML (grep for `matrix.arch_label` guards) plus a runtime check that
  format-check and coverage each appear exactly once in a completed workflow
  run's job list, regardless of how many arm64 legs exist.
