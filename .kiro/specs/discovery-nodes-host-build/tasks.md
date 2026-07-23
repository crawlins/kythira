# Implementation Plan — Discovery Nodes Host Build

## Status: Complete (6/6 tasks) — verified via a real `arm64-docker-smoke-test.yml` dispatch

**Last Updated**: July 23, 2026

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

- [x] 1. Add staging directories and ignore them
  - Directories not pre-created (empty dirs aren't tracked by git anyway) —
    each `make_directory` staging `COMMAND` creates its own at build time,
    confirmed working end to end by Task 6's dispatch.
  - `.gitignore`: extended the existing `docker/chaos_node/dist/` comment
    block to cover all three new paths in one combined block, per
    design.md's Component 3.
  - _Requirements: 1.3_

## Phase 2: CI Parity Fix (Task 2)

- [x] 2. Add `libavahi-client-dev` and `libldns-dev` to
      `arm64-docker-smoke-test.yml`
  - Added both packages to the "Install system dependencies" step,
    alongside the existing `libfiu-dev`.
  - _Requirements: 3.1_

## Phase 3: Build/Package Rewrites (Tasks 3-5)

- [x] 3. `poco_discovery_node`: CMake wiring + Dockerfile rewrite
  - `tests/docker_chaos/CMakeLists.txt`: `docker-poco-discovery-image` now
    `if(POCO_DNSSD_FOUND AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)`
    gated, `DEPENDS poco_discovery_node` + staging `make_directory`/`copy`
    before the existing `docker build` command; `else()` keeps the
    "unavailable" echo+false fallback.
  - `docker/poco_discovery_node/Dockerfile`: single runtime-only stage;
    `entrypoint.sh` handling and `HEALTHCHECK` unchanged.
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

- [x] 4. `dns_discovery_node`: CMake wiring + Dockerfile rewrite
  - Same shape as Task 3, guarded on
    `if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)`, combined with
    Task 5's target in one `if`/`else` since both share the guard.
  - `docker/dns_discovery_node/Dockerfile`: single runtime-only stage; no
    `entrypoint.sh` (unchanged — `ENTRYPOINT`s directly at the binary).
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

- [x] 5. `dns_sd_discovery_node`: CMake wiring + Dockerfile rewrite
  - Identical shape to Task 4, for `docker-dns-sd-discovery-image` /
    `dns_sd_discovery_node`.
  - _Requirements: 1.1, 1.2, 2.1, 2.2, 4.1_

## Phase 4: Verification (Task 6)

- [x] 6. Manually dispatch `arm64-docker-smoke-test.yml` and confirm
  - Dispatched on `feat/discovery-nodes-host-build` (PR #91) on
    2026-07-23: run
    [30006127341](https://github.com/crawlins/kythira/actions/runs/30006127341)
    completed successfully. `Run docker-poco-discovery-tests
    (poco_discovery_node image)`, `Run docker-dns-discovery-tests (bind9 +
    dns_discovery_node images)`, and `Run docker-dns-sd-discovery-tests
    (bind9 + dns_sd_discovery_node images)` all passed — each target's
    host build, staging copy, `docker build`, and full scenario-test suite
    against real arm64 hardware.
  - `doc/TODO.md` updated alongside this.
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
