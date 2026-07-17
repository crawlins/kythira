# Discovery Nodes Host Build Design Document

## Overview

This is a direct extension of `.kiro/specs/chaos-node-host-build/`'s design
to three more images. The mechanism is identical in shape for all three
targets (`poco_discovery_node`, `dns_discovery_node`, `dns_sd_discovery_node`)
— they already share one Dockerfile pattern with each other (`FROM
ubuntu:24.04 AS builder` → apt-installs its one system dependency plus a
build toolchain → `CMAKE_PREFIX_PATH=/src/vcpkg_installed/$TRIPLET` →
compile → runtime stage `COPY --from=builder`), so the same rewrite applies
to each with only the specific package/binary/entrypoint names changing.

The one thing genuinely new here (not present in `chaos-node-host-build`,
whose `chaos_node` was already being compiled — if unsuccessfully — on
every relevant host today) is Requirement 3: none of these three targets
currently compile in *any* host build at all, because
`POCO_DNSSD_FOUND`/`LIBLDNS_FOUND` are apt-only detections
(`pkg_check_modules`/`find_package(Poco QUIET COMPONENTS DNSSD Foundation)`
depending on system `libavahi-client-dev`/`libldns-dev`, not vcpkg
packages) and neither `ci.yml` nor `arm64-docker-smoke-test.yml` installs
them. This design closes that gap, but *only* for the workflow that actually
needs it (Requirement 3.3) — `ci.yml` never invokes any `docker-*-image`
target, so it has no need for these packages regardless of this spec.

## Architecture

```
Per-target, identical shape (X = poco_discovery_node | dns_discovery_node |
dns_sd_discovery_node):

Before (current — works, but recompiles on every image rebuild):
  docker build -f docker/X/Dockerfile .
    │
    ▼
  [in-container] apt-get install cmake ninja g++/clang pkg-config <sys-dep>-dev
  [in-container] COPY . .   (brings in host's vcpkg_installed/, per .dockerignore)
  [in-container] cmake -B build -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/$TRIPLET
                 && cmake --build build --target X
    │
    ▼
  kythira-X:dev

After (this design):
  Host (vcpkg_installed/ already populated; <sys-dep>-dev now also
        installed there — Requirement 3)
    │
    ▼
  cmake --build build --target X
    │
    ▼
  build/X  (real, working binary)
    │
    ▼
  cmake -E copy build/X docker/X/dist/X
    │  (new CMake COMMAND, added to the existing docker-X-image target)
    ▼
  docker build -f docker/X/Dockerfile .
    │
    ▼
  [in-container] apt-get install <runtime-only packages, unchanged>
  [in-container] COPY docker/X/dist/X /usr/local/bin/X
    │  (no compiler, no CMake, no builder stage at all)
    ▼
  kythira-X:dev   ← same tag, same ENTRYPOINT/EXPOSE/HEALTHCHECK
```

```
tests/docker_chaos/CMakeLists.txt        (extended — 3 targets)
  docker-poco-discovery-image gains:
    DEPENDS poco_discovery_node + staging COMMAND
    guard: if(POCO_DNSSD_FOUND AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)
  docker-dns-discovery-image gains:
    DEPENDS dns_discovery_node + staging COMMAND
    guard: if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)
  docker-dns-sd-discovery-image gains:
    DEPENDS dns_sd_discovery_node + staging COMMAND
    guard: if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)

docker/poco_discovery_node/Dockerfile      (rewritten — single stage)
docker/dns_discovery_node/Dockerfile       (rewritten — single stage)
docker/dns_sd_discovery_node/Dockerfile    (rewritten — single stage)
docker/poco_discovery_node/dist/           (new — git-ignored)
docker/dns_discovery_node/dist/            (new — git-ignored)
docker/dns_sd_discovery_node/dist/         (new — git-ignored)
.gitignore                                 (+3 entries)
.github/workflows/arm64-docker-smoke-test.yml  (+libavahi-client-dev, +libldns-dev)
```

`docker/bind9/Dockerfile` is untouched — it builds a real BIND9 server
image from the stock `internetsystemsconsortium/bind9:9.18` (or similar)
base plus zone-file configuration, not project C++ source; nothing about
host-vs-in-container compilation applies to it.

## Components and Interfaces

### 1. `tests/docker_chaos/CMakeLists.txt` — staging each binary

```cmake
if(POCO_DNSSD_FOUND AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)
    add_custom_target(docker-poco-discovery-image
        DEPENDS poco_discovery_node
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/dist"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:poco_discovery_node>"
                "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/dist/poco_discovery_node"
        COMMAND ${_RUNTIME_EXE} build
                --file "${_POCO_DISCOVERY_DOCKERFILE}"
                --tag kythira-poco-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Staging poco_discovery_node binary and building kythira-poco-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    add_custom_target(docker-poco-discovery-image
        COMMAND ${CMAKE_COMMAND} -E echo
                "'docker-poco-discovery-image' requires poco_discovery_node to be buildable (Poco DNSSD + folly + Boost + httplib)"
        COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)
    add_custom_target(docker-dns-discovery-image
        DEPENDS dns_discovery_node
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/docker/dns_discovery_node/dist"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:dns_discovery_node>"
                "${CMAKE_SOURCE_DIR}/docker/dns_discovery_node/dist/dns_discovery_node"
        COMMAND ${_RUNTIME_EXE} build
                --file "${_DNS_DISCOVERY_DOCKERFILE}"
                --tag kythira-dns-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Staging dns_discovery_node binary and building kythira-dns-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    add_custom_target(docker-dns-sd-discovery-image
        DEPENDS dns_sd_discovery_node
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_SOURCE_DIR}/docker/dns_sd_discovery_node/dist"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:dns_sd_discovery_node>"
                "${CMAKE_SOURCE_DIR}/docker/dns_sd_discovery_node/dist/dns_sd_discovery_node"
        COMMAND ${_RUNTIME_EXE} build
                --file "${_DNS_SD_DISCOVERY_DOCKERFILE}"
                --tag kythira-dns-sd-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Staging dns_sd_discovery_node binary and building kythira-dns-sd-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
else()
    foreach(_t docker-dns-discovery-image docker-dns-sd-discovery-image)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'${_t}' requires the relevant node to be buildable (libldns + folly + httplib)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
```

Same rationale as `chaos-node-host-build`'s Component 1 for every choice
here: `$<TARGET_FILE:...>` generator expressions (robust to build-type/
output-dir changes), `DEPENDS` on the CMake target rather than `if(TARGET
...)` (which would incorrectly read `FALSE` here regardless of actual
buildability, since `cmd/poco_discovery_node`/`cmd/dns_discovery_node`/
`cmd/dns_sd_discovery_node` are all processed after `tests/` at the top
level — same ordering fact `chaos-node-host-build`'s design.md documents
for `cmd/chaos_node`), and the FOUND-variable guard mirroring each target's
own `cmd/.../CMakeLists.txt` `return()` condition exactly.

### 2. Each Dockerfile — single, runtime-only stage

```dockerfile
# docker/poco_discovery_node/Dockerfile
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libavahi-client3 \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/poco_discovery_node/dist/poco_discovery_node /usr/local/bin/poco_discovery_node
COPY docker/poco_discovery_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

HEALTHCHECK --interval=2s --timeout=3s --start-period=15s --retries=20 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-9011}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
```

```dockerfile
# docker/dns_discovery_node/Dockerfile  (dns_sd_discovery_node identical
# apart from the binary name / default port 9031 / no entrypoint.sh —
# both ENTRYPOINT directly at the binary today, unchanged by this spec)
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libldns3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY docker/dns_discovery_node/dist/dns_discovery_node /usr/local/bin/dns_discovery_node

HEALTHCHECK --interval=2s --timeout=3s --retries=30 \
    CMD curl -sf http://localhost:${HTTP_PORT:-9021}/health || exit 1

ENTRYPOINT ["/usr/local/bin/dns_discovery_node"]
```

Each identical to its current runtime stage apart from the `COPY` source
(Requirement 1.1/1.2).

### 3. `.gitignore` additions

```gitignore
# Host-built binaries staged for docker/*/Dockerfile
# (.kiro/specs/chaos-node-host-build/, .kiro/specs/discovery-nodes-host-build/)
# — not source, not committed.
docker/chaos_node/dist/
docker/poco_discovery_node/dist/
docker/dns_discovery_node/dist/
docker/dns_sd_discovery_node/dist/
```

(Shown together with `chaos_node`'s existing entry from the prior spec —
implementation SHOULD add these three alongside it, one comment block
covering all four, rather than four separate near-duplicate comments.)

### 4. `.github/workflows/arm64-docker-smoke-test.yml` — closing the skip gap

```yaml
      - name: Install system dependencies
        run: |
          sudo apt-get update -q
          sudo apt-get install -y --no-install-recommends \
            g++-13 \
            cmake \
            ninja-build \
            libfiu-dev \
            libavahi-client-dev \
            libldns-dev
```

Two added lines (building on `chaos-node-host-build`'s `libfiu-dev`
addition). This is what actually makes `POCO_DNSSD_FOUND`/`LIBLDNS_FOUND`
true on this workflow's host for the first time — without it, the
Component 1 targets' `DEPENDS poco_discovery_node`/etc. would still resolve
(the targets exist in the CMake graph, gated by their own `return()`), but
those dependency targets would never have been *defined* at all, since
`cmd/poco_discovery_node`'s/etc.'s own `if(NOT POCO_DNSSD_FOUND OR ...)
return()` guard would still trip — i.e. this package installation is a
precondition for Requirement 2's guard evaluating true in the first place,
not just a nice-to-have.

## Correctness Properties

Properties 1 ("no second, divergent dependency list") and 3 ("no
compilation-time regression for the common case") from
`chaos-node-host-build`'s design.md apply identically here, per-target —
not restated in full; see that document.

### Property 2 (extended): Runtime ABI compatibility, plus one new wrinkle

Same reasoning as `chaos-node-host-build`'s Property 2 (vcpkg's default
static linkage on this project's Linux triplets; GitHub-hosted
`ubuntu-24.04`/`ubuntu-24.04-arm` runners vs. `ubuntu:24.04` base image).
The one addition here: `libavahi-client3`/`libldns3` are *apt*, not vcpkg,
packages — dynamically linked regardless of where compilation happens, and
already present in each image's runtime stage today (unchanged by this
spec) precisely because that's already true of the *current*,
working, in-container-compiled images. Moving compilation to the host does
not change which shared libraries the binary needs at runtime, only where
the compilation step itself executes — the same point
`chaos-node-host-build`'s design.md makes for `libfiu0`.

## Error Handling

Identical to `chaos-node-host-build`'s Error Handling section, per target:
host can't build → existing "target unavailable" `echo`+`false` fallback;
staging copy failure → ordinary CMake custom-command failure, `cmake
--build` stops before `docker build`; `docker build` invoked directly
without staging having run first → Docker's standard "file not found in
build context" error at the `COPY` instruction.

## Testing Strategy

- **Manual dispatch of `arm64-docker-smoke-test.yml`** (Requirement 5.1):
  the same dispatch already used for `chaos-node-host-build`'s own
  verification — both specs' changes land in the same workflow, so one
  green run of `Run docker-poco-discovery-tests`, `Run
  docker-dns-discovery-tests`, and `Run docker-dns-sd-discovery-tests`
  covers this spec end to end.
- No fault-injection-style spot-check is needed (Requirement 5.2) — these
  three targets have no libfiu analog; ordinary scenario-test passage
  (which already exercises real peer discovery over a real network between
  real containers) is the complete correctness signal.
- No new test type is introduced, for the same reason
  `chaos-node-host-build`'s Testing Strategy gives: the existing scenario
  suites already provide exactly the coverage this change needs.

## Non-Goals

- **Installing `libavahi-client-dev`/`libldns-dev` in `ci.yml`** so
  `poco_discovery_node`/`dns_discovery_node`/`dns_sd_discovery_node` build
  (and get exercised) as part of the default per-PR gate. Real and
  arguably worth doing, but a test-coverage concern independent of *how*
  Docker images get built — out of scope for a spec about build mechanism
  (Requirement 3.3).
- **`docker/bind9/Dockerfile`.** Not project C++ code; nothing about this
  spec's mechanism applies.
- **Cross-compiling or multi-arch image production**, and **changing image
  tags/versioning/registry publishing** — same Non-Goals as
  `chaos-node-host-build`, for the same reasons, per target.
