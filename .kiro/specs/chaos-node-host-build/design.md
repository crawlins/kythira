# Chaos Node Host Build Design Document

## Overview

`docker/chaos_node/Dockerfile` today is the odd one out among its three
siblings (`poco_discovery_node`, `dns_discovery_node`, `dns_sd_discovery_node`):
those three already avoid re-bootstrapping vcpkg in-image by pointing
`CMAKE_PREFIX_PATH` at a `vcpkg_installed/<triplet>` tree `COPY . .` brings
in from the host, while still compiling their own source *inside* Docker.
`chaos_node`'s Dockerfile instead tries to reconstruct an equivalent
dependency set from a small, hand-maintained `apt-get install` list — one
that has drifted out of sync with what `cmd/chaos_node/CMakeLists.txt`
actually requires (`folly_FOUND AND Boost_FOUND AND httplib_FOUND`; folly
isn't apt-installable at all), so the `chaos_node` target is never even
defined in that build tree today.

This design goes past "reuse the host's vcpkg tree, still compile in
Docker" to "compile on the host entirely, Docker just packages the
result" — for `chaos_node` specifically. The binary is built once, using
the project's real, already-proven vcpkg-based CMake configuration (the
exact shape `ci.yml` already runs successfully), and
`docker/chaos_node/Dockerfile` shrinks to a single runtime-only stage that
`COPY`s it in. This is a strict simplification of the existing image-build
surface, not a new kind of machinery: Docker image builds already depend on
host state today (the other three Dockerfiles require a pre-populated
`vcpkg_installed/`; nothing about `docker build .` on a bare clone has ever
been fully self-contained for any of these four images) — this design just
extends that same pre-condition one step further, to "the binary itself,"
for the one image where source recompilation inside Docker was never
actually working anyway.

## Architecture

```
Before (current, broken):
  docker build -f docker/chaos_node/Dockerfile .
    │
    ▼
  [in-container] apt-get install cmake ninja clang-18 libboost-all-dev ...
    │  (no folly — not apt-installable)
    ▼
  [in-container] cmake -B build && cmake --build build --target chaos_node
    │
    ▼
  CMake configure: folly_FOUND=FALSE → cmd/chaos_node never add_subdirectory'd
    │
    ▼
  ninja: error: unknown target 'chaos_node'   ← current failure

After (this design):
  Host (CI runner or developer machine, vcpkg_installed/ already populated
        exactly as ci.yml already does — no new step needed for that part)
    │
    ▼
  cmake --build build --target chaos_node
    │  (real vcpkg tree: folly_FOUND=TRUE, same config ci.yml already proves)
    ▼
  build/chaos_node  (real, working binary)
    │
    ▼
  cmake -E copy build/chaos_node docker/chaos_node/dist/chaos_node
    │  (new CMake COMMAND, added to the existing docker-chaos-image target)
    ▼
  docker build -f docker/chaos_node/Dockerfile .
    │
    ▼
  [in-container] apt-get install libfiu0 fiu-utils iproute2 iptables curl
  [in-container] COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node
    │  (no compiler, no CMake, no builder stage at all)
    ▼
  kythira-chaos-node:dev   ← same tag, same ENTRYPOINT/EXPOSE/HEALTHCHECK
```

```
tests/docker_chaos/CMakeLists.txt        (extended)
  docker-chaos-image target gains:
    - DEPENDS chaos_node
    - a COMMAND copying $<TARGET_FILE:chaos_node> to docker/chaos_node/dist/
    - both wrapped in if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)

docker/chaos_node/Dockerfile             (rewritten — single stage)
docker/chaos_node/dist/                  (new — git-ignored staging dir)
.gitignore                               (+docker/chaos_node/dist/)
.github/workflows/arm64-docker-smoke-test.yml  (+libfiu-dev in apt list)
```

## Components and Interfaces

### 1. `tests/docker_chaos/CMakeLists.txt` — staging the binary

```cmake
# ── docker-chaos-image: stage the host-built chaos_node binary, then
#    package it into a runtime-only image (Requirement 1-3) ──────────────────
# Guarded exactly like cmd/chaos_node's own inclusion at the top level
# (folly_FOUND AND Boost_FOUND AND httplib_FOUND) — chaos_node is defined
# later in configure-time processing order (cmd/chaos_node is added after
# tests/ at the top level) so `if(TARGET chaos_node)` isn't usable here;
# these FOUND variables are the same condition, evaluated the same way
# cmd/chaos_node/CMakeLists.txt's own `if(NOT folly_FOUND OR ...) return()`
# guard already does.
if(folly_FOUND AND Boost_FOUND AND httplib_FOUND)
    add_custom_target(docker-chaos-image
        DEPENDS chaos_node
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/docker/chaos_node/dist"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:chaos_node>"
                "${CMAKE_SOURCE_DIR}/docker/chaos_node/dist/chaos_node"
        COMMAND ${_RUNTIME_EXE} build
                --file "${_CHAOS_DOCKERFILE}"
                --tag kythira-chaos-node:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Staging chaos_node binary and building kythira-chaos-node:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    add_custom_target(docker-chaos-image
        COMMAND ${CMAKE_COMMAND} -E echo
                "'docker-chaos-image' requires chaos_node to be buildable (folly + Boost + httplib)"
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()
```

`$<TARGET_FILE:chaos_node>` is a CMake generator expression resolving to the
exact built binary path regardless of build type or
`RUNTIME_OUTPUT_DIRECTORY` overrides — more robust than hardcoding
`build/chaos_node`, and correct even if that property ever changes.
`DEPENDS chaos_node` is a target-graph dependency, resolved at generate
time against the *whole* project (all subdirectories, regardless of
processing order) — unlike `if(TARGET chaos_node)`, which is a parse-time
check and would incorrectly evaluate false here since `cmd/chaos_node` is
processed after `tests/` in the top-level `CMakeLists.txt`. This is why the
guard uses the FOUND variables directly rather than `if(TARGET
chaos_node)`.

This replaces the existing (unconditional, no-`DEPENDS`) `docker-chaos-image`
definition in place — everything else in that file (the scenario-test
targets, `docker-chaos-tests`' aggregate target, the other three images'
targets) is untouched.

### 2. `docker/chaos_node/Dockerfile` — single, runtime-only stage

```dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libfiu0 fiu-utils \
        iproute2 \
        iptables \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/chaos_node/dist/chaos_node /usr/local/bin/chaos_node
COPY docker/chaos_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 7000 8080 9000

HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=10 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-8080}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
```

Identical runtime package list, `EXPOSE`, `HEALTHCHECK`, and `ENTRYPOINT` to
today's runtime stage (Requirement 5.1) — the only substantive change is
*where* `/usr/local/bin/chaos_node` comes from: a plain `COPY` from the
staging directory instead of `COPY --from=builder`, and there is no
`builder` stage left to reference. `docker build` invoked directly (not via
the CMake target) now requires `docker/chaos_node/dist/chaos_node` to
already exist — the same category of precondition
`poco_discovery_node`'s/etc.'s Dockerfiles already have today for
`vcpkg_installed/` (Requirement 3's rationale).

### 3. `.gitignore` / staging directory

```gitignore
# Host-built binary staged for docker/chaos_node/Dockerfile
# (.kiro/specs/chaos-node-host-build/) — not source, not committed.
docker/chaos_node/dist/
```

No `.dockerignore` change needed: `docker/chaos_node/dist/` doesn't match
any existing exclusion pattern there (Requirement 2.2).

### 4. `.github/workflows/arm64-docker-smoke-test.yml` — libfiu-dev parity

```yaml
      - name: Install system dependencies
        run: |
          sudo apt-get update -q
          sudo apt-get install -y --no-install-recommends \
            g++-13 \
            cmake \
            ninja-build \
            libfiu-dev
```

The single added line. `ci.yml`'s build-and-test job already installs
`libfiu-dev`; this workflow's own host build did not need it before
(nothing on the host compiled `chaos_node` there previously — that
happened entirely inside Docker, whose builder stage already had its own
`libfiu-dev`). Now that this workflow's host build *is* what produces the
image's binary, it needs the same package (Requirement 4.2).

## Correctness Properties

### Property 1: No second, divergent dependency list to drift out of sync

By construction, there is exactly one CMake configuration that determines
whether/how `chaos_node` builds — the project's own top-level
`CMakeLists.txt` plus `cmd/chaos_node/CMakeLists.txt`, run on the host with
the same vcpkg tree every other target uses. `docker/chaos_node/Dockerfile`
no longer contains any dependency-resolution logic of its own (no apt list
mirroring a subset of what CMake needs) — Requirement 1's bug class (a
Dockerfile's own package list silently drifting out of sync with what the
CMake configuration it invokes actually requires) cannot recur here, because
there is nothing left in the Dockerfile that resolves `chaos_node`'s
dependencies at all.

### Property 2: Runtime ABI compatibility

`chaos_node`'s vcpkg dependencies (folly, Boost, httplib, etc.) are
statically linked by default on this project's Linux triplets (`x64-linux`/
`arm64-linux` — see `CMakeLists.txt`'s `KYTHIRA_VCPKG_TRIPLET`; no
overriding `VCPKG_LIBRARY_LINKAGE` is set anywhere in
`vcpkg-configuration.json`/`vcpkg-overlays/`), so the compiled binary's
remaining dynamic dependencies are effectively just glibc/libstdc++/libgcc_s
and `libfiu` — already the only non-`curl`/`iproute2`/`iptables` runtime
package the image installs today (`libfiu0`). The host build environment
(GitHub-hosted `ubuntu-24.04`/`ubuntu-24.04-arm` runners) and the runtime
image's base (`ubuntu:24.04`) are the same OS release, so glibc symbol
versioning is not expected to be a problem — but this is an assumption
about ABI compatibility across two independently-updated Ubuntu 24.04
images (a GitHub Actions runner image vs. a Docker Hub base image), not a
guarantee. Requirement 6's real-container verification (start it, health-
check it, run the existing fault-injection-dependent scenario tests against
it) is what actually proves this holds — this design does not claim
certainty from static analysis alone, which is why that verification step
is a first-class Requirement rather than an afterthought.

### Property 3: No compilation-time regression for the common case

`ci.yml`'s build-and-test job already compiles `chaos_node` as part of its
ordinary `cmake --build build -j$(nproc)` (no `--target` filter — builds
everything the configured dependency set makes available, `chaos_node`
included). `arm64-docker-smoke-test.yml`, by contrast, previously never
compiled `chaos_node` on the host at all (its own "Configure" step
configures only; compilation happened exclusively inside Docker via
`docker-chaos-tests`' dependency chain). This design adds exactly one host
compilation of `chaos_node` where previously there was zero — not a second,
*duplicate* compilation on top of an existing one — while removing the
(currently broken) in-container compilation entirely. Net compilation count
for `arm64-docker-smoke-test.yml` goes from "one broken attempt inside
Docker" to "one working attempt on the host."

## Error Handling

- **Host cannot build `chaos_node`** (folly/Boost/httplib unavailable):
  `docker-chaos-image` (and everything depending on it) becomes the
  existing "target unavailable, requires X" `echo` + `false` fallback
  pattern already used elsewhere in this file for missing container
  runtimes — no attempt is made to fall back to in-container compilation,
  since that path is being removed entirely, not kept as a fallback.
- **Staging copy fails** (e.g. disk full, permissions): surfaces as an
  ordinary CMake custom-command failure — `cmake --build ... --target
  docker-chaos-image` stops before attempting `docker build`, with the
  normal `cmake -E copy` error message; no new failure mode introduced.
- **`docker build` invoked directly without the staging step having run
  first**: fails at the `COPY docker/chaos_node/dist/chaos_node ...`
  instruction with Docker's standard "file not found in build context"
  error — a clear, immediate failure, not a confusing partial build.

## Testing Strategy

- **Manual dispatch of `arm64-docker-smoke-test.yml`** (Requirement 6.2):
  the same workflow already used to discover the original bug. A green run
  of its `Run docker-chaos-tests (chaos_node image + 7 scenario tests)` step
  is direct, end-to-end evidence — real image build, real containers, real
  health checks, real Raft consensus exercised by the existing 7 chaos
  scenario tests.
- **Fault-injection parity spot-check** (Requirement 6.1): confirm
  `docker_chaos_persistence_faults_test` or
  `docker_chaos_safety_assertions_test` specifically passes — both exercise
  `chaos_node`'s libfiu-gated fault-injection code paths, so a pass there is
  concrete evidence Requirement 4's `libfiu-dev` workflow fix actually took
  effect, not just that the container starts.
- No new test *type* is introduced — the existing `docker_chaos` scenario
  test suite already provides exactly the coverage this change needs
  (start a real container from this image, confirm it behaves correctly),
  and duplicating that coverage in a new, narrower test would only test the
  packaging mechanism in isolation from the thing that actually matters
  (a working cluster).

## Non-Goals

- **Applying this same "host build, Docker just packages" pattern to
  `poco_discovery_node`, `dns_discovery_node`, or `dns_sd_discovery_node`.**
  Those three Dockerfiles already work (their `CMAKE_PREFIX_PATH`-at-
  `vcpkg_installed/` pattern is functioning, unlike `chaos_node`'s), so this
  spec's motivating bug doesn't apply to them. Converting them too would be
  a reasonable, cheap follow-up given how similar the mechanism is, but is
  out of scope for a spec specifically about *fixing* `chaos_node`'s Docker
  image.
- **Cross-compiling** (building an arm64 binary on an x64 host or vice
  versa) or producing multi-arch images from a single host build. Both
  `ci.yml` and `arm64-docker-smoke-test.yml` already run natively on
  matching-architecture runners for each leg; this design preserves that,
  it doesn't change it.
- **Changing the image tag, versioning scheme, or publishing images to a
  registry.** Purely a local build-mechanism change (Requirement 5).
- **Fixing the dead `-DKYTHIRA_FAULT_INJECTION=ON` flag's underlying naming
  inconsistency** (i.e. renaming `CHAOS_TESTS_ENABLED` to something an
  external caller could plausibly override, or making it independently
  settable rather than purely auto-detected from `libfiu-dev`'s presence).
  Requirement 4.3 only removes the now-pointless flag from the Dockerfile
  path being rewritten; it does not redesign how fault-injection detection
  itself works project-wide.
