# Implementation Plan — Chaos Node Host Build

## Status: Not Started

**Last Updated**: July 17, 2026

## Overview

Rewrite `docker/chaos_node/Dockerfile` to a single, runtime-only stage that
packages an already-built `chaos_node` binary rather than recompiling
(unsuccessfully — folly isn't apt-installable) inside Docker; wire the host
build and staging copy into the existing `docker-chaos-image` CMake target;
restore fault-injection parity by installing `libfiu-dev` wherever the host
build now runs; verify end to end via the same `arm64-docker-smoke-test.yml`
manual dispatch that surfaced the original bug.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Staging directory + .gitignore entry — no dependencies, needed by everything else"
    },
    {
      "wave": 2,
      "tasks": [2, 3],
      "description": "docker-chaos-image CMake target rewrite (depends on wave 1's staging path) and the Dockerfile rewrite (depends on wave 1's staging path as its COPY source) — independent of each other"
    },
    {
      "wave": 3,
      "tasks": [4],
      "description": "arm64-docker-smoke-test.yml libfiu-dev fix — independent of waves 1-2, but needed before wave 4's verification will show correct fault-injection behavior"
    },
    {
      "wave": 4,
      "tasks": [5],
      "description": "End-to-end verification via manual workflow dispatch — depends on all of waves 1-3"
    }
  ]
}
```

## Tasks

## Phase 1: Staging Plumbing (Task 1)

- [ ] 1. Add the staging directory and ignore it
  - Create `docker/chaos_node/dist/` (an empty directory is fine; the
    staging copy step creates it at build time too via `${CMAKE_COMMAND} -E
    make_directory`, so this task just needs the `.gitignore` entry to
    exist — no placeholder file needed).
  - `.gitignore`: add `docker/chaos_node/dist/`.
  - Confirm (by inspection, not a new test) that this path matches no
    existing `.dockerignore` pattern — Requirement 2.2.
  - _Requirements: 2.2, 2.3_

## Phase 2: Build/Package Rewrite (Tasks 2-3)

- [ ] 2. Wire the host build + staging copy into `docker-chaos-image`
  - `tests/docker_chaos/CMakeLists.txt`: replace the existing unconditional
    `docker-chaos-image` custom target with the
    `if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)` / `else()` pair from
    design.md's Component 1 — `DEPENDS chaos_node`, a `make_directory` +
    `copy $<TARGET_FILE:chaos_node>` command pair before the existing
    `docker build` command, and the "target unavailable" fallback in the
    `else()` branch matching this file's existing style for missing
    prerequisites.
  - Every other target in this file (scenario tests, `docker-chaos-tests`,
    the other three images' targets) is untouched.
  - _Requirements: 1.3, 3.1, 3.2_

- [ ] 3. Rewrite `docker/chaos_node/Dockerfile` to a single runtime-only stage
  - Replace the two-stage Dockerfile with design.md's Component 2: no
    builder stage, no compiler/CMake/Ninja/dev-package installs; same
    runtime `apt-get install` list, `EXPOSE`, `HEALTHCHECK`, `ENTRYPOINT` as
    today; `COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node`
    instead of `COPY --from=builder ...`.
  - _Requirements: 1.1, 1.2, 2.1, 5.1_

## Phase 3: CI Parity Fix (Task 4)

- [ ] 4. Add `libfiu-dev` to `arm64-docker-smoke-test.yml`'s host install step
  - `.github/workflows/arm64-docker-smoke-test.yml`: add `libfiu-dev` to the
    "Install system dependencies" step's package list (alongside the
    existing `g++-13`/`cmake`/`ninja-build`), matching `ci.yml`'s
    build-and-test job.
  - Remove the now-pointless `-DKYTHIRA_FAULT_INJECTION=ON` flag along with
    the rest of the old in-container `cmake` invocation deleted in Task 3 —
    nothing carries it forward; fault injection is determined solely by
    `libfiu-dev`'s presence at host configure time (`CHAOS_TESTS_ENABLED`).
  - _Requirements: 4.1, 4.2, 4.3_

## Phase 4: Verification (Task 5)

- [ ] 5. Manually dispatch `arm64-docker-smoke-test.yml` and confirm
  - Trigger the workflow on the branch carrying Tasks 1-4.
  - Confirm the `docker-chaos-image` build step (inside `Run
    docker-chaos-tests`) succeeds — the host `chaos_node` compile, the
    staging copy, and `docker build` all complete.
  - Confirm all 7 existing chaos scenario tests pass against the rebuilt
    image, including `docker_chaos_persistence_faults_test` or
    `docker_chaos_safety_assertions_test` specifically (direct evidence
    Task 4's `libfiu-dev` fix restored fault-injection support, not just
    that the container starts).
  - If green, update `doc/TODO.md`'s Minor Enhancements entry for this bug
    (added while completing `.kiro/specs/otlp-telemetry-backend/`) from
    `[ ]` to `[x]`.
  - _Requirements: 6.1, 6.2_

## Notes

- No new `vcpkg.json` dependency, no new CMake option, no compose-file
  changes — this spec only touches
  `tests/docker_chaos/CMakeLists.txt`, `docker/chaos_node/Dockerfile`,
  `.gitignore`, and `.github/workflows/arm64-docker-smoke-test.yml`.
- `poco_discovery_node`'s/`dns_discovery_node`'s/`dns_sd_discovery_node`'s
  Dockerfiles are read-only references throughout (their existing
  `vcpkg_installed/`-reuse pattern is what first suggested host-side reuse
  was viable here at all) — no task in this plan edits any of them; see
  design.md's Non-Goals for why applying this same pattern to them is
  explicitly out of scope for this spec.
