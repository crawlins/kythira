# Requirements Document — mold Linker Adoption

## Introduction

This document specifies linking this project's own binaries with
[mold](https://github.com/rui314/mold), a multithreaded drop-in replacement
for GNU ld, instead of whatever linker the compiler driver picks by default
(GNU ld.bfd on every Ubuntu runner and in every Docker builder this project
uses today).

The case for it comes from this repository's own measurements, not from
mold's published benchmarks:

- The ccache experiment (`.kiro/specs/ccache-adoption/`, draft PR #49) found
  that **212 of 399 compiler-launcher invocations (53%) were link steps**, and
  that no compiler cache can touch them. The OCI-hosted sccache that replaced
  ccache in CI (`.kiro/specs/oci-build-cache/`) has the same blind spot. Once
  the compile cache is warm, linking is most of what a "nothing changed"
  rebuild still spends its time on.
- Every test binary statically links Folly, Boost, OpenSSL and, on the cloud
  legs, the AWS/Azure/GCP SDKs from `vcpkg_installed/`. The root
  `CMakeLists.txt` memory-budget comment for the beast pool calls out
  "whichever static link of a Folly/Boost/OpenSSL/AWS-SDK test binary happens
  to be in the third slot" as part of what a `-j3` runner has to fit, and notes
  that "link steps are in no pool and were never in this table".
- The project defines 279 `add_executable()` targets. ld.bfd links each one
  single-threaded; mold parallelises symbol resolution, relocation and output
  writing within a single link.

Nothing above says *how much* mold saves here. Requirement 1 measures that on
this repository's own CI before anything is merged, the same way the ccache
and OCI build-cache specs did, and the adoption is gated on the result.

### Current state (verified October 9, 2026, on `main`)

- No `-fuse-ld`, `CMAKE_LINKER`, `CMAKE_LINKER_TYPE` or `LINKER_TYPE` appears
  anywhere in the repository's CMake, workflows or Dockerfiles. The only
  explicit linker option is `LINKER:--as-needed` on
  `oci_heartbeat_writer_node` (`cmd/oci_heartbeat_writer/CMakeLists.txt`).
- `cmake_minimum_required(VERSION 3.20)`. CI installs Ubuntu 24.04's `cmake`
  package, which is 3.28.3. `CMAKE_LINKER_TYPE` needs 3.29, so it is not
  available on CI's CMake.
- No LTO (`INTERPROCEDURAL_OPTIMIZATION`, `-flto`) and no `--gc-sections` are
  used anywhere.
- CI compilers: `g++-13`, `g++-14`, `clang++-18`, on `ubuntu-24.04` (x64) and
  `ubuntu-24.04-arm` (arm64). Docker builder stages are `ubuntu:24.04`.
- Link flags the build already adds: `-fprofile-instr-generate -latomic`
  (coverage), `-fsanitize=thread` / `-fsanitize=address` (sanitizers).

## Glossary

- **mold**: The linker. Invoked through the compiler driver with
  `-fuse-ld=mold` (GCC 12.1+ and Clang 12+ accept it; every CI compiler
  qualifies). It stamps its name and version into the output's `.comment`
  section, which is how this spec proves it was really used.
- **default linker**: Whatever the compiler driver uses with no `-fuse-ld`:
  GNU ld.bfd on every host in scope.
- **link step**: A Ninja edge whose output is an executable or shared library.
  `.ninja_log` records each edge's start and end time, so link time can be
  summed from a finished build without instrumenting anything.
- **silent no-op**: The failure mode the `KYTHIRA_COMPILER_LAUNCHER` block was
  written to prevent (the ccache spec's `CCACHE_DIR` incident: CI asked for a
  cache, got nothing, and nobody noticed for five days). Here: CI asks for
  mold, links with ld.bfd, and stays green.

## Requirements

### Requirement 1: Measure before adopting

**User Story:** As the maintainer, I want mold's effect on this repository's
own CI measured before it is wired in, so the decision rests on numbers from
these builds and not on mold's marketing benchmarks.

#### Acceptance Criteria

1. A throwaway branch and draft PR SHALL build the same commit twice on at
   least the `Build & Test (clang++-18, x64)`, `Build & Test (g++-13, arm64)`
   and `Coverage (clang++-18)` legs: once with the default linker and once
   with `-fuse-ld=mold`, with the sccache cache warm for both so compile time
   is held constant.
2. For each run the measurement SHALL record: the Build step's wall clock;
   the summed duration of all link steps from `.ninja_log`; the five slowest
   individual link steps with their durations; and the peak resident memory
   of the slowest link (`/usr/bin/time -v` around that one target, rebuilt in
   isolation after the full build).
3. The results SHALL be recorded in `tasks.md` against task 1, including any
   leg where mold was slower or used more memory.
4. WHEN mold does not reduce summed link time by at least 30% on the
   clang++-18 x64 leg THEN Requirements 2-8 SHALL NOT be implemented and this
   spec SHALL be closed with the numbers as its record. The threshold is
   deliberately modest: the point is to rule out a change that costs
   reviewer attention for noise-level gains.

   *Revision, October 15, 2026.* Task 1 measured a 27.1% reduction on the
   clang++-18 x64 leg, and only after Requirement 9's change; before it,
   mold could not link that leg at all. The maintainer waived the threshold
   and chose adoption over lld (49.5% on the same leg) and over closing the
   spec. The numbers and the decision are recorded against task 1.
5. WHEN mold's peak link memory exceeds ld.bfd's by more than 25% on any leg
   THEN task 1 SHALL also measure that leg with Ninja's link pool limited to
   one concurrent link (Requirement 6) before the go/no-go decision.

---

### Requirement 2: One CMake knob, defaulting to mold when present

**User Story:** As a developer, I want mold used automatically when it is
installed and to have no effect when it is not, and I want CI to be able to
demand it so a missing package fails loudly.

#### Acceptance Criteria

1. The root `CMakeLists.txt` SHALL define a cache variable
   `KYTHIRA_LINKER` with the values `auto` (default), `mold`, `lld` and
   `default`, placed immediately after the `KYTHIRA_COMPILER_LAUNCHER` block
   and before the first `find_package()` call, following that block's shape.
2. WHEN `KYTHIRA_LINKER=auto` AND the compiler accepts `-fuse-ld=mold` THEN
   every executable, shared library and module this project defines SHALL be
   linked with mold, and configure SHALL print
   `linker: mold <version> (<path>)`.
3. WHEN `KYTHIRA_LINKER=auto` AND mold is not usable THEN the build SHALL be
   identical to today's (no `-fuse-ld` anywhere) and configure SHALL print
   `linker: default`. This SHALL NOT be an error. `auto` SHALL NOT fall back
   to lld: a machine without mold gets today's build, not a third linker.
4. WHEN `KYTHIRA_LINKER` is `mold` or `lld` AND the compiler does not accept
   the corresponding `-fuse-ld=` flag THEN configure SHALL fail with a
   `FATAL_ERROR` naming the linker, matching the explicit-launcher behaviour
   and for the same reason (the silent no-op).
5. WHEN `KYTHIRA_LINKER=default` THEN no `-fuse-ld` flag SHALL be added.
6. Changing `KYTHIRA_LINKER` on an existing build tree SHALL take effect on
   reconfigure in both directions. A cached probe result SHALL NOT pin the
   first linker the tree was configured with (the launcher block's two
   separate `find_program` entries exist for exactly this reason).
7. `lld` is accepted as an explicit value only, as an escape hatch for a
   developer whose mold misbehaves; it is not tested in CI by this spec.

---

### Requirement 3: The linker choice must not leak into dependency builds or probes

**User Story:** As the maintainer of the vcpkg binary cache, I need the
linker change to leave every vcpkg package's ABI hash and every
`find_package`/`check_*` result unchanged, so adopting mold does not
invalidate the OCI-hosted vcpkg cache or change what the build detects.

#### Acceptance Criteria

1. The linker flag SHALL be applied with `add_link_options()` (or an
   equivalent per-target mechanism), not through `CMAKE_*_LINKER_FLAGS`,
   `LDFLAGS`, the vcpkg triplet or `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`.
   `CMAKE_*_LINKER_FLAGS` propagate into `try_compile()`; `add_link_options()`
   does not.
2. No workflow SHALL pass a linker choice (`-fuse-ld=...`) to the
   `vcpkg install` step. vcpkg packages continue to be linked with the
   default linker. The one `LDFLAGS` a workflow exports there is
   Requirement 9.3's `-latomic` for clang-built ports, which adds a library
   and leaves the linker alone.
3. Verification SHALL show the vcpkg cache keys and the restored
   `vcpkg_installed/` tree are unchanged between a mold and a default-linker
   configure of the same commit (no vcpkg rebuild is triggered). The key
   depends on the job's compiler (Requirement 9), never on its linker.
4. Verification SHALL show `CMakeCache.txt` differs between the two
   configures only in `KYTHIRA_LINKER` and the probe result entries this spec
   adds.

---

### Requirement 4: Equivalent output

**User Story:** As someone who ships these binaries and reads their coverage
numbers, I need proof that switching the linker changes link time and
nothing else that matters.

#### Acceptance Criteria

1. For one full build of the same commit with each linker, the set of built
   targets (`ninja -t targets all`) SHALL be identical.
2. The full `ctest` run SHALL produce the same pass/fail set with either
   linker on the clang++-18 x64 and g++-13 arm64 legs.
3. For a representative sample (at least `multi_raft_node`, `ca_cluster_node`,
   `oci_heartbeat_writer_node` and the slowest test binary from Requirement
   1.2), the following SHALL match between the two linkers: `DT_NEEDED`
   entries; PIE; RELRO and BIND_NOW; non-executable stack (`GNU_STACK`
   flags); presence of a build-id note. Differences in section layout, size
   or symbol order are expected and acceptable.
4. `oci_heartbeat_writer_node` SHALL still have no `libatomic.so.1` in its
   `DT_NEEDED` entries under mold. Its `--as-needed` ordering trick
   (`cmd/oci_heartbeat_writer/CMakeLists.txt`) is the one link in this tree
   that depends on precise archive/link-line semantics, and it exists because
   getting it wrong broke a real cloud deployment.
5. The coverage leg's reported line and region coverage SHALL be identical
   with either linker for the same commit, and `coverage_floor.txt`
   enforcement SHALL behave the same.
6. Duplicate-symbol behaviour SHALL be checked explicitly for the
   libcoap/libnyoci pair, which both export `coap_insert_option` and coexist
   only because the linker pulls archive members on demand (root
   `CMakeLists.txt`, libnyoci block). The `alt-coap-backends` leg SHALL link
   and pass with mold.
7. ThreadSanitizer and AddressSanitizer builds SHALL link and run their
   existing suites with mold.

---

### Requirement 5: CI uses mold by name and proves it

**User Story:** As a CI job, I want mold installed and requested explicitly,
and I want the job to fail if the binaries were not actually linked by mold.

#### Acceptance Criteria

1. Every CI job that runs `cmake --build` for this project (in `ci.yml`,
   `real-cloud-tests.yml`, `container-tests.yml`, `perf-cloud.yml`,
   `coap-flake-measure.yml` and `arm64-docker-smoke-test.yml`) SHALL add
   `mold` to its `apt-get install` list and pass `-DKYTHIRA_LINKER=mold` to
   its configure step. Jobs that do not link (`static-analysis`,
   `kconfig-check`, `docs`, `packer-*`, `workflow-input-limits`,
   `aws-ci-policies`) SHALL NOT be touched.
2. CI SHALL request mold by name rather than relying on `auto`, so a missing
   package fails at configure (Requirement 2.4) instead of quietly linking
   with ld.bfd. This is the same choice CI makes for sccache.
3. Each modified job SHALL, after its Build step, assert that at least one
   built executable's `.comment` section contains `mold`
   (`readelf -p .comment`), and fail with an `::error::` annotation otherwise.
   This is the silent-no-op guard; it is cheap and needs no extra build.
4. The Ubuntu `mold` package SHALL be used, not a downloaded release binary,
   so no new pinned-download or checksum machinery is introduced. If task 1
   finds the Ubuntu 24.04 package unusable, the spec SHALL be revised before
   any alternative is adopted.

---

### Requirement 6: Bounded link concurrency where memory requires it

**User Story:** As the maintainer of jobs that already run at `-j3` because of
memory pressure, I need mold's threads and memory to fit inside the budget
those jobs were tuned for.

#### Acceptance Criteria

1. The root `CMakeLists.txt` SHALL accept `KYTHIRA_LINK_JOBS` (default empty,
   meaning unlimited, which is today's behaviour). WHEN it is set to a
   positive integer THEN a Ninja job pool of that size SHALL be assigned to
   `CMAKE_JOB_POOL_LINK`.
2. WHEN task 1 shows that concurrent mold links push a memory-constrained
   leg (at minimum `future-backend-compat`'s stdexec leg and `coverage`) into
   swap or OOM THEN that leg SHALL set `KYTHIRA_LINK_JOBS` to the value task
   1 measured as fitting, with a workflow comment citing the run.
3. WHEN no leg needs it THEN `KYTHIRA_LINK_JOBS` SHALL still be implemented
   but left unset everywhere, so the knob exists before the next memory
   incident rather than being invented during one.

---

### Requirement 7: Documentation

#### Acceptance Criteria

1. `DEPENDENCIES.md`'s "Optional Dependencies" section SHALL gain a `mold`
   entry in the existing format (Status, Purpose, Installation, Notes). The
   installation line is `apt install mold` only: this project targets Linux,
   and mold's macOS port is a separate product. The Notes line SHALL state
   that its absence changes nothing about the build and cite task 1's
   measured figure with the conditions it was measured under.
2. The new `CMakeLists.txt` block SHALL carry a comment explaining why
   `add_link_options` rather than `CMAKE_*_LINKER_FLAGS` (Requirement 3.1),
   why `auto` does not fall back to lld (Requirement 2.3), and how to revert
   (`-DKYTHIRA_LINKER=default`).

---

### Requirement 8: Explicit non-goals

#### Acceptance Criteria

1. The `docker/ca_cluster_node` and `docker/ca_service` builder stages SHALL
   NOT install mold in this spec. They produce the binaries that ship on the
   CA AMI; changing their linker waits until mold has been the CI linker for
   at least two weeks with no linker-attributed failure, and is a follow-up.
   Because those builders do not install mold, `KYTHIRA_LINKER=auto` already
   leaves them on ld.bfd with no change to their Dockerfiles.
2. vcpkg ports SHALL NOT be relinked with mold (Requirement 3).
3. LTO, `--gc-sections`, `--icf` and split DWARF SHALL NOT be introduced.
   They interact with the linker choice and are each worth measuring, but
   bundling them would make it impossible to attribute any change in time,
   size or behaviour to mold.
4. `CMAKE_LINKER_TYPE` SHALL NOT be used while CI's CMake is older than 3.29.
   When CI's CMake reaches 3.29 the block may be simplified to it in a
   separate change.
5. No Kconfig symbol SHALL be added. The linker, like the compiler launcher,
   is a toolchain choice and not a feature of the built system, so it stays
   out of the defconfigs and the `config-variants` job.

---

### Requirement 9: Clang jobs build their vcpkg ports with clang

*Added October 15, 2026, after task 1.*

**User Story:** As a clang++-18 CI job linking with mold, I need my vcpkg
static libraries built by the same compiler as my own objects, because mold
rejects the mixture that ld.bfd tolerates.

GCC places the VTT of a class with virtual bases (glog's
`LogMessage::LogStream`, among others) inside the vtable's COMDAT group;
clang emits no VTT for it. When a clang-built object's vtable group wins,
the GCC-built archive's only VTT definition is discarded while its debug
information (`.debug_loclists`, or `.debug_addr` under split DWARF) still
refers to it. ld.bfd and lld write a tombstone value and carry on; mold
stops with "refers to a discarded COMDAT section probably due to an ODR
violation". mold has no option to downgrade this one error
(`--ignore-odr-violations` does not exist in 2.30, 2.42.1 or 3.0.0), and
`--gdb-index` and `-gsplit-dwarf` do not help. `--strip-debug` hides it at
the cost of debug info and was rejected for that reason.

#### Acceptance Criteria

1. Every job that configures with `-DCMAKE_CXX_COMPILER=clang++-18` and
   links with mold SHALL build its vcpkg ports with `CC=clang-18` and
   `CXX=clang++-18`.
2. Each such job's whole-tree `actions/cache` key SHALL include the
   compiler (`-clang++-18-`), so a clang-built tree is never restored on a
   GCC job or the other way round. Jobs with the same feature set and
   architecture SHALL share one key, as the GCC jobs already do.
3. Such jobs SHALL export `LDFLAGS="-Wl,--push-state,--no-as-needed -latomic
   -Wl,--pop-state"` for the `vcpkg install` step only. clang lowers
   `std::atomic<T>::is_lock_free()` to a libatomic call that GCC inlines, and
   proxygen's sample servers fail to link without it.
4. GCC jobs SHALL keep the runner's default compiler for vcpkg (or the
   compiler they already set, as the g++-14 legs do) and SHALL keep their
   existing cache keys, so their trees stay valid.
5. The per-package OCI binary cache already keys on vcpkg's ABI hash, which
   includes the compiler, so it holds clang and GCC packages side by side
   with no change. That roughly doubles what it stores for the shared ports.
   The GitHub Actions cache gains one whole tree per clang feature set and
   architecture; if that pushes the repository past its Actions cache quota,
   the quota is raised in the repository settings, not worked around here.

