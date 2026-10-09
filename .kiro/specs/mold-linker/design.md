# Design Document — mold Linker Adoption

## Overview

The change has one moving part that decides whether mold is used at all, a
`KYTHIRA_LINKER` block in the root `CMakeLists.txt` (Component 1), and one
that decides whether CI really benefits, explicit `-DKYTHIRA_LINKER=mold`
plus a post-build `.comment` assertion in every linking job (Component 3).
A measurement (Component 0) comes first and can stop the rest.

```
              developer machine                       CI job
              ─────────────────                       ──────
          mold installed? (optional)         apt-get install ... mold
                    │                                   │
                    ▼                                   ▼
   cmake -DKYTHIRA_LINKER=auto (default)   cmake -DKYTHIRA_LINKER=mold
                    │                                   │
                    └──────────────┬────────────────────┘
                                   ▼
             CMakeLists.txt: probe -fuse-ld=<linker> once per
             (linker, path); on success add_link_options(-fuse-ld=...)
                 auto + no mold  → no flag, today's build
                 mold + no mold  → FATAL_ERROR
                                   │
                                   ▼
                     cmake --build (optional link pool)
                                   │
                                   ▼
             CI only: readelf -p .comment <binary> | grep mold
```

The vcpkg install step is upstream of all of this and never sees the flag.

## Component 0: Measurement (task 1)

A throwaway branch `measure/mold-linker` with a draft PR that is closed once
the numbers are in, following the ccache (PR #49) and OCI build-cache task 1
precedent. It needs no CMake change: the measuring workflow edit appends
`-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold` (acceptable for a throwaway because
the vcpkg step is separate and the measurement is about link time, not
probe hygiene) and installs `mold`.

Run order per leg, same commit, sccache warm:

1. Default-linker build. Save `build/.ninja_log`.
2. mold build in a fresh build directory. Save `build/.ninja_log`.
3. In the mold tree, delete the slowest test binary and relink it alone under
   `/usr/bin/time -v`; repeat in the default-linker tree. Record maximum
   resident set size.

Summing link time from `.ninja_log` (format v5/v6: `start_ms end_ms mtime
output hash`): an edge is a link step when its output has no `.o` suffix and
is not under `CMakeFiles/`. A small script in the measurement branch prints
the total, the count, and the top five. Its output, not the script, goes into
`tasks.md`.

Legs: `Build & Test (clang++-18, x64)`, `Build & Test (g++-13, arm64)`,
`Coverage (clang++-18)`. Add `Full suite (stdexec future backend)` if
Requirement 1.5 triggers, because that is the leg with the tightest memory
history.

## Component 1: `KYTHIRA_LINKER` in `CMakeLists.txt`

Placed directly after the `KYTHIRA_COMPILER_LAUNCHER` block, before the
`ENABLE_COVERAGE` block and every `find_package()` call.

```cmake
# Linker. mold links in parallel; ld.bfd, which every Ubuntu toolchain uses
# by default, does not. Link steps are about half of all build edges and no
# compiler cache can skip them (.kiro/specs/mold-linker/ has the measured
# effect on this tree).
#
# add_link_options(), not CMAKE_*_LINKER_FLAGS: the latter propagate into
# every try_compile(), so switching linkers could change what find_package()
# and the check_* probes detect. vcpkg packages are built by a separate
# `vcpkg install` step and never see this.
#
# `auto` uses mold if the compiler accepts -fuse-ld=mold and otherwise
# changes nothing. It does not fall back to lld: a machine without mold gets
# the build it always had, not a third linker nobody tests. CI asks for mold
# by name so a missing package fails here instead of linking with ld.bfd.
#
# Revert with -DKYTHIRA_LINKER=default.
set(KYTHIRA_LINKER "auto" CACHE STRING
    "Linker: auto (mold if usable, else the compiler default), mold, lld or default")
set_property(CACHE KYTHIRA_LINKER PROPERTY STRINGS auto mold lld default)

include(CheckLinkerFlag)

# Probe -fuse-ld=<name>, re-probing when the linker binary on PATH changes.
# check_linker_flag() caches its result, so without the path check a tree
# first configured before mold was installed would stay on ld.bfd forever:
# the silent no-op the launcher block above also guards against.
function(kythira_probe_linker name out)
    string(TOUPPER "${name}" upper)
    find_program(KYTHIRA_${upper}_PROGRAM NAMES "${name}" "ld.${name}")
    if(NOT KYTHIRA_${upper}_PROGRAM)
        set(${out} FALSE PARENT_SCOPE)
        return()
    endif()
    if(NOT "${KYTHIRA_${upper}_PROGRAM}" STREQUAL "${_KYTHIRA_${upper}_PROBED_PATH}")
        unset(KYTHIRA_LINKER_ACCEPTS_${upper} CACHE)
        set(_KYTHIRA_${upper}_PROBED_PATH "${KYTHIRA_${upper}_PROGRAM}"
            CACHE INTERNAL "")
    endif()
    check_linker_flag(CXX "-fuse-ld=${name}" KYTHIRA_LINKER_ACCEPTS_${upper})
    set(${out} "${KYTHIRA_LINKER_ACCEPTS_${upper}}" PARENT_SCOPE)
endfunction()

set(_kythira_linker "")
if(KYTHIRA_LINKER STREQUAL "auto")
    kythira_probe_linker(mold _ok)
    if(_ok)
        set(_kythira_linker mold)
    endif()
elseif(KYTHIRA_LINKER STREQUAL "mold" OR KYTHIRA_LINKER STREQUAL "lld")
    kythira_probe_linker(${KYTHIRA_LINKER} _ok)
    if(NOT _ok)
        message(FATAL_ERROR
            "KYTHIRA_LINKER=${KYTHIRA_LINKER} but ${CMAKE_CXX_COMPILER} "
            "does not accept -fuse-ld=${KYTHIRA_LINKER}")
    endif()
    set(_kythira_linker ${KYTHIRA_LINKER})
elseif(NOT KYTHIRA_LINKER STREQUAL "default")
    message(FATAL_ERROR "KYTHIRA_LINKER must be auto, mold, lld or default, "
                        "got \"${KYTHIRA_LINKER}\"")
endif()

if(_kythira_linker)
    add_link_options("-fuse-ld=${_kythira_linker}")
    string(TOUPPER "${_kythira_linker}" _u)
    execute_process(COMMAND "${KYTHIRA_${_u}_PROGRAM}" --version
                    OUTPUT_VARIABLE _v OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    message(STATUS "linker: ${_v} (${KYTHIRA_${_u}_PROGRAM})")
else()
    message(STATUS "linker: default")
endif()
unset(_kythira_linker)
```

Notes on the choices:

- **Why not `CMAKE_LINKER_TYPE`.** It is the clean answer and needs CMake
  3.29; CI runs Ubuntu 24.04's 3.28.3. Requirement 8.4 records when to switch.
- **Directory scope.** `add_link_options()` applies to targets created after
  it in this directory and below, which is every target this project defines
  because the block precedes all `add_subdirectory()` calls. It does not
  apply to `try_compile()` (Requirement 3.1) or to imported targets.
- **Reconfigure.** Nothing is written with `FORCE`, and the flag lives only in
  the directory property, so switching to `default` stops adding it on the
  next configure. Switching compiler needs a fresh tree anyway.
- **`KYTHIRA_MOLD_PROGRAM` is only for the message and the re-probe key.** The
  compiler driver finds `ld.mold` itself; if the two ever disagree, the CI
  `.comment` check (Component 3) is what catches it.
- **Coverage and sanitizers.** Their flags stay in `CMAKE_EXE_LINKER_FLAGS`.
  The driver passes `-fprofile-instr-generate` and `-fsanitize=*` runtime
  libraries to whichever linker it runs, so no change there.

### `KYTHIRA_LINK_JOBS` (Requirement 6)

Placed next to the existing `heavy_tu` pool declaration:

```cmake
set(KYTHIRA_LINK_JOBS "" CACHE STRING
    "Maximum concurrent link steps (Ninja only); empty means unlimited")
if(KYTHIRA_LINK_JOBS)
    if(NOT KYTHIRA_LINK_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "KYTHIRA_LINK_JOBS must be a positive integer")
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS
                 kythira_link=${KYTHIRA_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK kythira_link)
endif()
```

`CMAKE_JOB_POOL_LINK` must be set before the targets it should cover are
created, so this goes with the linker block near the top, while the
`heavy_tu` declaration can stay where it is. mold itself also reads
`MOLD_JOBS`; the Ninja pool is preferred because it works the same for any
linker and is visible in `build.ninja`.

## Component 2: Documentation

A `### mold — faster linking` entry in `DEPENDENCIES.md`, placed after the
`sccache` entry so the three build accelerators sit together:

- Status: optional, build-time only.
- Purpose: parallel linker for this project's own binaries.
- Installation: `apt install mold` (Ubuntu 24.04's package is what CI uses;
  older distributions may ship a 1.x release, which task 1 does not cover).
- Notes: absence changes nothing; the measured figure from task 1 and the leg
  and conditions it was measured on; `-DKYTHIRA_LINKER=default` to opt out.

## Component 3: CI wiring

For each linking job:

1. Add `mold` to the `apt-get install` package list (inside the existing
   three-attempt retry loop, not a new step).
2. Add `-DKYTHIRA_LINKER=mold` beside the existing
   `-DKYTHIRA_COMPILER_LAUNCHER=...` line in every `cmake -B` invocation in
   that job (some jobs configure more than one tree, e.g. `config-variants`).
3. After the Build step:

```yaml
      - name: Assert binaries were linked by mold
        run: |
          bin=$(find build -maxdepth 3 -type f -perm -u+x -name '*_test' | head -1)
          if [ -z "$bin" ]; then
            echo "::error::no test executable found to check the linker of"; exit 1
          fi
          if ! readelf -p .comment "$bin" | grep -q mold; then
            echo "::error::$bin was not linked by mold (KYTHIRA_LINKER=mold had no effect)"
            readelf -p .comment "$bin"; exit 1
          fi
```

   Jobs that build only named targets (e.g. a `--target ca_cluster_node`
   build) check that target's binary instead of a `*_test`.

`real-cloud-tests.yml` jobs are `workflow_dispatch`/scheduled and cost money
to run, so they get the same edits but are verified by one dispatched run of
the cheapest AWS leg, not all of them; the others pick the change up on their
next scheduled run.

Rollback for any one job is deleting its `-DKYTHIRA_LINKER=mold` line, which
falls back to `auto`; with mold still installed that is still mold, so a true
rollback is `-DKYTHIRA_LINKER=default`.

## Correctness Properties

1. **No change without mold.** With `KYTHIRA_LINKER=auto` on a machine
   without mold, or `KYTHIRA_LINKER=default` anywhere, `build.ninja` contains
   no `-fuse-ld` and is otherwise identical to `main`'s.
2. **No probe drift.** `CMakeCache.txt` from a `mold` and a `default`
   configure of the same commit differ only in `KYTHIRA_LINKER`,
   `KYTHIRA_MOLD_PROGRAM`, `KYTHIRA_LINKER_ACCEPTS_MOLD` and
   `_KYTHIRA_MOLD_PROBED_PATH`.
3. **No silent no-op.** In CI, every linking job either fails at configure
   (mold missing) or produces binaries whose `.comment` names mold.
4. **Equivalent binaries.** Requirement 4.3's ELF properties match;
   `ctest` and coverage results match.

## Testing Strategy

- **Configure matrix (local, no vcpkg needed):** the block lifted into a
  scratch CMake project with one `add_executable()`, run with each `KYTHIRA_LINKER` value with and
  without mold on `PATH`, checking the status line and `build.ninja`. Also:
  configure with mold hidden, install it, reconfigure, and confirm the tree
  switches to mold (Requirement 2.6).
- **Equivalence (CI, once):** the task 1 measurement branch already produces
  both trees on the same commit; extend it to diff `ninja -t targets all`,
  run `readelf -d -l -n` on the Requirement 4.3 sample, and compare `ctest`
  JUnit outputs and coverage totals. Recording this in `tasks.md` is the
  deliverable; it is not a permanent test.
- **Ongoing:** the `.comment` assertion in every job, plus the existing suite.

## Risks

- **Archive semantics.** mold, like lld, does not resolve static archives
  in exactly ld.bfd's single left-to-right pass. A link that only works, or
  only picks a particular duplicate definition, because of archive order can
  change silently. Requirement 4.4 and 4.6 name the two places in this tree
  known to depend on it; the full `ctest` comparison covers the rest.
- **Memory and thread oversubscription.** mold uses every core per link, and
  Ninja may run several links at once beside `-j3` compiles. Requirement 6
  bounds it if measurement shows a need.
- **Ubuntu package lag.** The Ubuntu 24.04 package is a 2.x release that will
  not track upstream. That is acceptable for a linker whose job is to
  reproduce ld.bfd's output faster; a downloaded release would need its own
  pin-and-checksum machinery for little gain.
- **Debuggers and tools.** gdb, `llvm-cov`, `addr2line` and the sanitizer
  symbolizers read standard DWARF and symbol tables, which mold emits. The
  coverage comparison and sanitizer runs in Requirement 4 are the check.
