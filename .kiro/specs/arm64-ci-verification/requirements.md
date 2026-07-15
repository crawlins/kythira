# Requirements Document

## Introduction

This document specifies the requirements for verifying that Kythira builds and
its test suite passes on 64-bit ARM (`aarch64`/`arm64`) Linux, and for adding
the CI/CD jobs that enforce this on every push and pull request, alongside the
existing x86_64-only pipeline.

Today, every CI job in `.github/workflows/ci.yml` and
`.github/workflows/real-cloud-tests.yml` runs on `ubuntu-24.04` — a native
x86_64 GitHub-hosted runner — and the vcpkg triplet `x64-linux` is hardcoded
in more than a dozen places: both workflow files, four `CMakeLists.txt` files
(root, `tests/`, `tests/chaos/`, `tests/docker_chaos/`), and every
`docker/*/Dockerfile`. Separately, `.kiro/specs/aws-quorum-manager/` already
added arm64/Graviton (`t4g.micro`) awareness to the real-EC2 test
(`tests/aws_quorum_manager_real_ec2_test.cpp`), selected at compile time via
`#if defined(__aarch64__) || defined(__arm64__)` — but since that test is only
ever *built* on an x86_64 runner, that branch has never actually compiled or
run in CI.

GitHub now offers native `ubuntu-24.04-arm` hosted runners, which avoids the
alternative of cross-compiling or running under QEMU emulation — a meaningful
difference for a template-heavy C++23 codebase with several large
dependencies (Folly, AWS SDK for C++, Boost) where emulated builds would be
substantially slower and harder to debug. This spec's approach is therefore:
run the existing build-and-test pipeline *natively* on arm64 runners, fixing
every hardcoded x86_64 assumption that stands in the way.

**Scope**: 64-bit ARM Linux (`aarch64`, vcpkg triplet `arm64-linux`), matching
GitHub's hosted `ubuntu-24.04-arm` runners and AWS Graviton (`t4g`/`t3g`-class)
EC2 instances already referenced elsewhere in the project.

**Out of scope**: 32-bit ARM (`armv7`/`armhf`), ARM on macOS or Windows,
embedded/microcontroller ARM targets, self-hosted ARM runner hardware, and any
change to the actual Raft protocol logic (this is a build/CI verification
effort, not a functional change).

## Glossary

- **arm64 runner**: A GitHub-hosted `ubuntu-24.04-arm` virtual machine —
  native `aarch64` hardware, not emulated.
- **vcpkg triplet**: The `<arch>-<os>` identifier (`x64-linux`, `arm64-linux`)
  vcpkg uses to select which binaries/portfiles to build and where to install
  them (`vcpkg_installed/<triplet>/`).
- **Native build**: Compiling on hardware matching the target architecture, as
  opposed to cross-compiling or running under QEMU user-mode emulation.
- **Graviton**: AWS's arm64 EC2 processor family; `t4g.*` instance types are
  Graviton-based, already referenced as the arm64 fallback instance type in
  `tests/aws_quorum_manager_real_ec2_test.cpp`.
- **Multiarch tuple**: The Debian/Ubuntu library path suffix
  (`x86_64-linux-gnu`, `aarch64-linux-gnu`) used under `/usr/lib/<tuple>/` to
  let multiple architectures' shared libraries coexist.
- **`KYTHIRA_VCPKG_TRIPLET`**: A CMake variable this spec introduces to
  replace every hardcoded `x64-linux` path literal with one value computed
  from the host architecture.

## Requirements

### Requirement 1: Native ARM64 Build & Test CI Job

**User Story:** As a maintainer, I want `ci.yml`'s build-and-test job to run
natively on arm64 Linux runners with the same compiler matrix used on x86_64,
so that architecture-specific bugs (alignment, endianness assumptions,
triplet-path mistakes, toolchain differences) are caught before merge instead
of being discovered only when someone deploys to Graviton.

#### Acceptance Criteria

1. WHEN a push or pull request targets `main` THEN `ci.yml`'s `build-and-test`
   job SHALL run on `ubuntu-24.04-arm` for the same `{g++-13, clang++-18}`
   matrix already used on `ubuntu-24.04`, in addition to (not instead of) the
   existing x86_64 runs.
2. WHEN the arm64 leg runs THEN it SHALL execute the same steps as the x86_64
   leg — system dependency install, Rust toolchain install, vcpkg bootstrap,
   CMake configure, build, and `ctest` with the same
   `-LE '^(slow|performance|verbose|benchmark|docker)$'` label exclusion —
   except where Requirement 2 requires architecture-derived parameterization.
3. WHEN the arm64 leg's test results are published THEN the job/artifact names
   (test-reporter `name:`, `actions/upload-artifact` `name:`) SHALL be
   architecture-qualified (e.g. `Unit Tests (g++-13, arm64)`,
   `test-results-g++-13-arm64`) so x86_64 and arm64 results never collide or
   overwrite each other in the Checks UI or artifact list.
4. WHEN the arm64 leg's tests fail THEN the workflow SHALL fail exactly as an
   x86_64 leg failure does today (the existing "Fail if tests failed" step,
   generalized to run for every matrix entry regardless of architecture).
5. WHEN the arm64 leg is added THEN its `timeout-minutes` SHALL be set from a
   value measured during implementation (native arm64 build+test wall time),
   not silently inherited from the x86_64 job's `120` without verification.

### Requirement 2: vcpkg Triplet Parameterization

**User Story:** As a developer, I want the vcpkg triplet and installed-tree
path derived from the host architecture instead of hardcoded to `x64-linux`,
so the same CMake and CI code path works unmodified on x86_64 and arm64.

#### Acceptance Criteria

1. WHEN vcpkg is bootstrapped in `ci.yml`, `real-cloud-tests.yml`, and every
   `docker/*/Dockerfile` THEN the triplet SHALL resolve to `x64-linux` on
   x86_64 hosts and `arm64-linux` on aarch64 hosts, computed from the host
   architecture (`uname -m` or the workflow's `runner.arch` context) rather
   than being a hardcoded literal.
2. WHEN `CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/chaos/CMakeLists.txt`,
   and `tests/docker_chaos/CMakeLists.txt` reference a path under
   `vcpkg_installed/x64-linux/...` (the manually-linked `libboost_json.a` /
   `libboost_context.a` static archives, and the PocoDNSSD static libraries)
   THEN the literal `x64-linux` path segment SHALL be replaced by a single
   CMake variable, `KYTHIRA_VCPKG_TRIPLET`, computed once (from
   `CMAKE_SYSTEM_PROCESSOR` or an equivalent detection) and reused everywhere,
   so no second hardcoded triplet literal remains in the CMake tree.
3. WHEN `-DCMAKE_PREFIX_PATH=...` is passed to `cmake -B build` in any
   workflow step THEN it SHALL point at
   `vcpkg_installed/<resolved-triplet>` rather than a hardcoded
   `vcpkg_installed/x64-linux`.
4. WHEN `KYTHIRA_VCPKG_TRIPLET` is introduced THEN the existing x86_64 CI
   behavior SHALL be unchanged (same triplet resolves to `x64-linux`, same
   paths are produced) — this requirement is a refactor for the x86_64 leg,
   not a behavior change.

### Requirement 3: Multi-Architecture Library Discovery (Avahi / PocoDNSSD)

**User Story:** As a maintainer, I want the `find_library` search for Avahi
(used by `poco_peer_discovery`'s optional DNSSD backend) to work on arm64
too, so that backend isn't silently unavailable on ARM for a path-hardcoding
reason unrelated to whether the actual library is installed.

#### Acceptance Criteria

1. WHEN CMake searches for `avahi-client`/`avahi-common`
   (`CMakeLists.txt` around lines 250–262) THEN the search paths SHALL include
   the arm64 Debian multiarch directory (`/usr/lib/aarch64-linux-gnu`) in
   addition to the existing `/usr/lib/x86_64-linux-gnu` path, or SHALL derive
   the multiarch tuple programmatically (e.g. via `CMAKE_LIBRARY_ARCHITECTURE`
   or `dpkg-architecture -qDEB_HOST_MULTIARCH`) instead of hardcoding either
   literal.
2. WHEN the manually-built PocoDNSSD static libraries (`libPocoDNSSD.a`,
   `libPocoDNSSDAvahi.a`) are absent for the resolved
   `KYTHIRA_VCPKG_TRIPLET` THEN CMake configuration SHALL continue to
   fall through to `POCO_DNSSD_FOUND FALSE` — exactly as it already does on
   x86_64 hosts that lack the prebuilt archive — rather than failing
   configuration. arm64 hosts without a prebuilt PocoDNSSD archive SHALL
   build successfully with the DNSSD backend disabled.
3. WHEN the arm64 CI job's configure step runs THEN it SHALL print the same
   `libldns not found` / DNSSD-availability status messages CMake already
   prints on x86_64, so a disabled-optional-feature state is visible in logs
   rather than silent.

### Requirement 4: vcpkg Binary Cache Isolation by Architecture

**User Story:** As a maintainer, I want the GitHub Actions vcpkg cache key to
include the runner architecture, so that adding an arm64 leg cannot restore an
x86_64-built cache entry onto an arm64 runner (or vice versa) and corrupt a
build with mismatched-architecture binaries.

#### Acceptance Criteria

1. WHEN the `actions/cache` key is computed in `ci.yml` and
   `real-cloud-tests.yml` (currently
   `vcpkg-${{ runner.os }}-edhoc-${{ hashFiles('vcpkg.json', 'vcpkg-overlays/**') }}`)
   THEN the key SHALL also include `${{ runner.arch }}`, e.g.
   `vcpkg-${{ runner.os }}-${{ runner.arch }}-edhoc-${{ hashFiles(...) }}`.
2. WHEN the arm64 leg runs for the first time after this change THEN it SHALL
   populate its own cache entry, verified by the cache key differing from the
   x86_64 key and no cache-hit being reported on the arm64 leg's first run.
3. WHEN existing x86_64 cache entries are inspected after this change THEN
   they SHALL remain addressable (the new key is additive — it still contains
   everything the old key did, plus `runner.arch` — so the very first
   post-change x86_64 run is a cache miss that repopulates under the new key,
   not a permanent cache loss).

### Requirement 5: Rust Toolchain and `lakers`/EDHOC on arm64

**User Story:** As a developer, I want the optional `edhoc` vcpkg feature
(which builds the Rust-based `lakers` overlay port used by
coap-transport-security's EDHOC bootstrap) to build on arm64 the same way it
does on x86_64, so that code path isn't x86_64-only.

#### Acceptance Criteria

1. WHEN `dtolnay/rust-toolchain@stable` runs on an `ubuntu-24.04-arm` runner
   THEN it SHALL install a working native `aarch64-unknown-linux-gnu` Rust
   toolchain.
2. WHEN `vcpkg install --triplet arm64-linux --x-feature=edhoc` runs THEN
   `vcpkg-overlays/lakers/portfile.cmake`'s `cargo build --release` step
   SHALL build natively for the host and produce
   `liblakers_kythira_ffi.a` under the `arm64-linux`-suffixed build tree,
   with no modification to `portfile.cmake` required (its build directory is
   already parameterized by `${TARGET_TRIPLET}`).
3. WHEN the arm64 CI leg configures with `--x-feature=edhoc` enabled THEN
   `LAKERS_AVAILABLE` SHALL be defined and any test exercising
   `oscore_bootstrap::edhoc` SHALL pass identically to the x86_64 leg.

### Requirement 6: Docker Chaos and Discovery Images on arm64

**User Story:** As a maintainer, I want the Docker chaos/discovery test images
(`chaos_node`, `poco_discovery_node`, `dns_discovery_node`,
`dns_sd_discovery_node`, `bind9`, `ca_cluster_node`, `ca_service`) to build and
run natively on arm64 hosts, so the container-based test suites aren't
silently x86_64-only once arm64 CI exists.

#### Acceptance Criteria

1. WHEN any `docker/*/Dockerfile` build stage runs
   `cmake ... -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux` THEN the
   hardcoded triplet path SHALL be replaced with a value derived at build time
   (e.g. `uname -m` inside the build stage, or a Docker build-arg) so the same
   Dockerfile produces a correct image whether the build host — and thus the
   `ubuntu:24.04` base image pulled — is x86_64 or arm64.
2. WHEN the `docker-chaos-image`, `docker-poco-discovery-image`,
   `docker-bind9-image`, `docker-dns-discovery-image`, and
   `docker-dns-sd-discovery-image` CMake targets run on an arm64 host with a
   native `docker` or `podman` installation THEN they SHALL each produce a
   runnable `linux/arm64` image without requiring QEMU emulation.
3. WHEN the docker-chaos and docker-discovery scenario tests (excluded from
   the default `ctest` run via the `docker` CTest label, on both
   architectures) are run explicitly on an arm64 host THEN they SHALL pass
   with the same assertions as on x86_64. This requirement covers build/run
   correctness on arm64; it does not add these `docker`-labeled tests to the
   default per-PR CI gate on either architecture, matching current practice.

### Requirement 7: Real Cloud Tests — Exercise the Graviton (arm64) EC2 Path

**User Story:** As a maintainer, I want `real-cloud-tests.yml`'s AWS job to
actually exercise the arm64/Graviton instance-selection branch already
present in `tests/aws_quorum_manager_real_ec2_test.cpp`, instead of leaving it
dead code that has never compiled because the workflow only ever builds on an
x86_64 runner.

#### Acceptance Criteria

1. WHEN `real-cloud-tests.yml`'s `aws` job builds
   `aws_quorum_manager_real_ec2_test` THEN it SHALL do so on both an
   `ubuntu-24.04` (x86_64) runner and an `ubuntu-24.04-arm` (arm64) runner, so
   the `#if defined(__aarch64__) || defined(__arm64__)` branch
   (`tests/aws_quorum_manager_real_ec2_test.cpp` lines ~403–417, and the
   `target_arch` selection at lines ~460–464) is actually compiled and
   executed by at least one CI leg.
2. WHEN the arm64 leg of the AWS real-cloud-tests job runs with the
   `ec2-quorum-manager` bundle enabled THEN it SHALL provision and validate
   against a Graviton EC2 instance (default `t4g.micro`, or the configured
   `AWS_TEST_INSTANCE_TYPE`), using the `arm64` Amazon Linux 2023 AMI selected
   by the existing `DescribeImages` filter logic.
3. WHEN either architecture leg of the real-cloud-tests `aws` job fails for a
   reason unrelated to real-infrastructure availability THEN the failure
   SHALL be attributable to a specific architecture (job/step naming
   disambiguates x86_64 vs. arm64 results), consistent with Requirement 1.3.

### Requirement 8: Non-Goals — Jobs That Deliberately Stay x86_64-Only

**User Story:** As a maintainer, I want the format-check and coverage jobs to
explicitly remain x86_64-only, so that adding arm64 CI doesn't silently double
unrelated CI cost or create two competing sources of truth for a single
architecture-independent measurement.

#### Acceptance Criteria

1. WHEN `ci.yml`'s format-check steps (currently gated on
   `matrix.compiler == 'g++-13'`) run on the arm64 matrix leg THEN they SHALL
   continue to be skipped there (format-check runs exactly once per workflow
   run — on x86_64/`g++-13` — regardless of how many arm64 legs exist), since
   `clang-format` output does not vary by target architecture.
2. WHEN the `coverage` job in `ci.yml` is evaluated THEN it SHALL continue to
   run only on `ubuntu-24.04` (x86_64) and SHALL NOT be duplicated for arm64:
   line/function coverage percentages are not expected to vary by
   architecture, and the job's own comments already document it as
   disk/time-constrained on a single architecture.
3. WHEN this spec is implemented THEN `clang-tidy` (a local/pre-commit-only
   static-analysis target per `.kiro/specs/clang-tidy/`, never invoked in CI)
   SHALL remain unaffected and out of scope — it is source-level analysis
   independent of target architecture.

### Requirement 9: Documentation

**User Story:** As a contributor, I want the project's ARM support status,
verification method, and known limitations documented, so I know precisely
what "Kythira works on ARM" means and what remains unverified or
intentionally unsupported.

#### Acceptance Criteria

1. WHEN this spec is implemented THEN `README.md` SHALL gain a section (near
   the existing CI/build documentation) stating that Kythira is built and
   tested natively on 64-bit ARM (`aarch64`, vcpkg triplet `arm64-linux`)
   Linux via GitHub-hosted `ubuntu-24.04-arm` runners, alongside the existing
   x86_64 matrix.
2. WHEN this documentation is written THEN it SHALL explicitly list the known
   ARM limitations discovered during implementation, at minimum: PocoDNSSD's
   manually-built static libraries are not provided for `arm64-linux` by this
   repository (the DNSSD discovery backend degrades to disabled — same
   behavior as any host missing the archive, per Requirement 3.2); 32-bit ARM
   (`armv7`/`armhf`) and non-Linux ARM (macOS, Windows) are explicitly out of
   scope.
3. WHEN `doc/TODO.md` is updated THEN it SHALL mark the ARM verification item
   done, following the pattern used for other completed spec entries (e.g.
   the `ca-cluster-rpc-mtls` entry dated July 14, 2026): what CI jobs were
   added, any real bugs found and fixed during implementation, and any
   explicitly deferred follow-up work.

### Requirement 10: Pre-Implementation Spike — vcpkg `arm64-linux` Dependency Availability

**User Story:** As a developer, I want to verify — before implementing the CI
changes in Requirements 1–7 — that every one of Kythira's vcpkg dependencies
actually builds for the `arm64-linux` triplet at the project's pinned
`builtin-baseline`, so this spec's design isn't built on an unverified
assumption about third-party ARM support.

#### Acceptance Criteria

1. WHEN the spike runs `vcpkg install --triplet arm64-linux` for the full
   current `vcpkg.json` dependency set (`boost-algorithm`, `boost-asio`,
   `boost-json`, `boost-system`, `boost-test`, `boost-thread`, `cpp-httplib`,
   `folly`, `libcoap[dtls]`, `openssl`, `poco[net]`,
   `aws-sdk-cpp[acm-pca,autoscaling,ec2,iam,s3,sts]`, `libssh2`,
   `stdexec[tbb]`) on an arm64 host (or arm64 emulation, for the spike only)
   THEN it SHALL record, per dependency, whether a working portfile/binary
   exists for `arm64-linux` at the pinned baseline, and any build failures
   encountered.
2. IF any dependency lacks `arm64-linux` support at the pinned baseline THEN
   the spike SHALL document the gap (upgrade the baseline, patch/add an
   overlay port, or file an upstream issue) as a blocking follow-up to be
   resolved before Requirement 1's CI job is enabled — a red arm64 CI job
   SHALL NOT be merged as the deliverable of this spec.
3. WHEN the spike concludes THEN its findings SHALL be recorded in
   `.kiro/specs/arm64-ci-verification/spike-notes.md`, mirroring
   `.kiro/specs/stdexec-future-backend/spike-notes.md`, so subsequent
   implementation tasks reference verified facts rather than assumptions.
