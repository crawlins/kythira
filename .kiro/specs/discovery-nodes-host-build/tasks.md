# Implementation Plan — Discovery Nodes Host Build

## Status: Not Started

**Last Updated**: July 17, 2026

## Overview

Extend `.kiro/specs/chaos-node-host-build/`'s "host build, Docker just
packages" pattern to `poco_discovery_node`, `dns_discovery_node`, and
`dns_sd_discovery_node`: rewrite each Dockerfile to a single runtime-only
stage, wire host-build-plus-staging into each image's existing CMake
target, and close the apt-package gap (`libavahi-client-dev`,
`libldns-dev`) that currently makes all three silently skip on every host
build.

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 1,
      "tasks": [1],
      "description": "Staging directories + .gitignore entries for all three targets — no dependencies, needed by everything else"
    },
    {
      "wave": 2,
      "tasks": [2],
      "description": "arm64-docker-smoke-test.yml apt-package fix — independent of wave 1, but required before wave 3's CMake guards will actually evaluate true"
    },
    {
      "wave": 3,
      "tasks": [3, 4, 5],
      "description": "CMake target rewrites for the three images (each depends on wave 1's staging paths) and each corresponding Dockerfile rewrite (depends on wave 1's staging path as its COPY source) — the three targets are independent of each other"
    },
    {
      "wave": 4,
      "tasks": [6],
      "description": "End-to-end verification via manual workflow dispatch — depends on all of waves 1-3"
    }
  ]
}
```

## Tasks

## Phase 1: Staging Plumbing (Task 1)

- [ ] 1. Add staging directories and ignore them
  - Create `docker/poco_discovery_node/dist/`, `docker/dns_discovery_node/dist/`,
    `docker/dns_sd_discovery_node/dist/` (empty is fine — the staging copy
    commands create them at build time too).
  - `.gitignore`: extend the comment block `.kiro/specs/chaos-node-host-build/`
    added for `docker/chaos_node/dist/` to cover all three new paths
    (design.md's Component 3 shows the combined form).
  - _Requirements: 1.3_

## Phase 2: CI Parity Fix (Task 2)

- [ ] 2. Add `libavahi-client-dev` and `libldns-dev` to
      `arm64-docker-smoke-test.yml`
  - `.github/workflows/arm64-docker-smoke-test.yml`: add both packages to
    the "Install system dependencies" step, alongside the `libfiu-dev`
    `.kiro/specs/chaos-node-host-build/` already adds there.
  - Done before Phase 3's tasks so that, once this workflow next runs
    (Task 6), `POCO_DNSSD_FOUND`/`LIBLDNS_FOUND` are true and Phase 3's
    CMake guards actually take the "build" branch, not the "unavailable"
    fallback.
  - _Requirements: 3.1_

## Phase 3: Build/Package Rewrites (Tasks 3-5)

- [ ] 3. `poco_discovery_node`: CMake wiring + Dockerfile rewrite
  - `tests/docker_chaos/CMakeLists.txt`: replace the existing unconditional
    `docker-poco-discovery-image` target with the
    `if(POCO_DNSSD_FOUND AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)`
    / `else()` pair from design.md's Component 1 — `DEPENDS
    poco_discovery_node`, staging `make_directory`/`copy` commands before
    the existing `docker build` command, "target unavailable" fallback.
  - `docker/poco_discovery_node/Dockerfile`: rewrite to the single
    runtime-only stage from design.md's Component 2 — no builder stage, no
    compiler/CMake/`libavahi-client-dev`; `COPY
    docker/poco_discovery_node/dist/poco_discovery_node
    /usr/local/bin/poco_discovery_node`; `entrypoint.sh` handling and
    `HEALTHCHECK` unchanged.
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

- [ ] 4. `dns_discovery_node`: CMake wiring + Dockerfile rewrite
  - `tests/docker_chaos/CMakeLists.txt`: same shape as Task 3, for
    `docker-dns-discovery-image` / `dns_discovery_node`, guarded on
    `if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)` (combined with
    Task 5's target in one `if`/`else` per design.md's Component 1, since
    both share the same guard condition).
  - `docker/dns_discovery_node/Dockerfile`: rewrite to single stage; no
    `entrypoint.sh` (this image `ENTRYPOINT`s directly at the binary today
    — unchanged).
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

- [ ] 5. `dns_sd_discovery_node`: CMake wiring + Dockerfile rewrite
  - Identical shape to Task 4, for `docker-dns-sd-discovery-image` /
    `dns_sd_discovery_node`.
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

## Phase 4: Verification (Task 6)

- [ ] 6. Manually dispatch `arm64-docker-smoke-test.yml` and confirm
  - Trigger the workflow on the branch carrying Tasks 1-5 (and, if not
    already merged, `.kiro/specs/chaos-node-host-build/`'s own changes —
    both land in the same workflow file).
  - Confirm `Run docker-poco-discovery-tests`, `Run
    docker-dns-discovery-tests`, and `Run docker-dns-sd-discovery-tests`
    all succeed — each image's host build, staging copy, `docker build`,
    and full scenario-test suite.
  - If green, update `doc/TODO.md` to note this follow-up complete
    alongside the entry `.kiro/specs/chaos-node-host-build/` will already
    have updated.
  - _Requirements: 5.1, 5.2_

## Notes

- Depends on `.kiro/specs/chaos-node-host-build/` having landed first (or
  being landed in the same change) — this spec's `.gitignore` and
  `arm64-docker-smoke-test.yml` edits are additive extensions of that
  spec's own edits to the same files/sections, not independent changes.
- No new `vcpkg.json` dependency, no new CMake option, no compose-file
  changes — this spec only touches `tests/docker_chaos/CMakeLists.txt`,
  the three Dockerfiles, `.gitignore`, and
  `.github/workflows/arm64-docker-smoke-test.yml`.
- `docker/bind9/Dockerfile`, every compose file, and `docker/chaos_node/`'s
  own files (covered by the other spec) are read-only references
  throughout — no task in this plan edits any of them.
