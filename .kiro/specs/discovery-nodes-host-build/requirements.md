# Discovery Nodes Host Build Requirements Document

## Introduction

`.kiro/specs/chaos-node-host-build/` fixes `docker/chaos_node/Dockerfile` —
which could not build `chaos_node` at all (folly, a hard requirement, isn't
apt-installable) — by building the binary on the host, using the project's
real vcpkg-based CMake configuration, and having the Dockerfile do nothing
but package the already-built result into a minimal runtime image.

`docker/poco_discovery_node/Dockerfile`, `docker/dns_discovery_node/Dockerfile`,
and `docker/dns_sd_discovery_node/Dockerfile` are not broken the same way —
each already avoids re-bootstrapping vcpkg in-image by pointing
`CMAKE_PREFIX_PATH` at a `vcpkg_installed/<triplet>` tree `COPY . .` brings
in from the host, and each still successfully compiles its own target
*inside* Docker today. But per an explicit follow-up discussion after
`chaos-node-host-build`'s design ("is this something that makes sense to
extend to the discovery targets as well?"): yes, for the same reasons that
motivated going one step further for `chaos_node` — no in-container
recompilation on every image rebuild, one fewer Dockerfile-embedded
dependency list to ever drift out of sync with what `cmd/<target>/CMakeLists.txt`
actually requires — and it turns out to be *simpler* here than it was for
`chaos_node`: none of these three targets have chaos_node's libfiu/
fault-injection-parity wrinkle.

There is a real, analogous parity gap to close first, though: each of these
three targets requires an apt-only system library
(`POCO_DNSSD_FOUND`/`libavahi-client-dev` for `poco_discovery_node`;
`LIBLDNS_FOUND`/`libldns-dev` for `dns_discovery_node` and
`dns_sd_discovery_node`) that today is installed *only* inside each
Dockerfile's own builder stage — neither `ci.yml`'s build-and-test job nor
`.github/workflows/arm64-docker-smoke-test.yml`'s host build installs
`libavahi-client-dev` or `libldns-dev` at all, so **all three targets are
currently silently skipped in every host build** (confirmed by this
project's own `message(STATUS "... skipped (requires ...)")` diagnostics in
`cmd/poco_discovery_node/CMakeLists.txt`, `cmd/dns_discovery_node/CMakeLists.txt`,
`cmd/dns_sd_discovery_node/CMakeLists.txt`). Moving compilation to the host
without also closing this gap would silently break these three images
exactly the way `chaos-node-host-build`'s Requirement 4 identified (and
fixed) for `chaos_node`'s libfiu-dev/fault-injection case.

`docker/bind9/Dockerfile` (used by the DNS/DNS-SD discovery scenario tests
as a real BIND9 server, not project C++ code) is unaffected by any of this
and out of scope — see Non-Goals.

## Glossary

- **Discovery node**: this spec's collective term for
  `poco_discovery_node`, `dns_discovery_node`, and `dns_sd_discovery_node` —
  the three peer-discovery mechanism binaries this spec covers.
- **Host build**, **Staging directory**, **Runtime-only Dockerfile**: as
  defined in `.kiro/specs/chaos-node-host-build/requirements.md`'s
  Glossary — this spec applies the identical concepts to three more images.

## Requirements

### Requirement 1: Each discovery node's Dockerfile becomes runtime-only

**User Story:** As a maintainer, I want
`poco_discovery_node`'s/`dns_discovery_node`'s/`dns_sd_discovery_node`'s
Dockerfiles to package an already-built binary rather than recompiling
in-container on every image rebuild, for the same reason
`chaos-node-host-build` did this for `chaos_node`.

#### Acceptance Criteria

1. Each of the three Dockerfiles SHALL become a single stage: `FROM
   ubuntu:24.04`, install only the runtime packages already listed in that
   image's current runtime stage (`poco_discovery_node`: `libavahi-client3`,
   `curl`; `dns_discovery_node`/`dns_sd_discovery_node`: `libldns3`, `curl`
   — all unchanged from today), `COPY` in the pre-built binary (and, for
   `poco_discovery_node`, `entrypoint.sh` — the other two `ENTRYPOINT`
   directly at the binary already, with no wrapper script, and SHALL
   continue to do so), and keep each image's existing `EXPOSE`/
   `HEALTHCHECK`/`ENTRYPOINT` unchanged.
2. None of the three Dockerfiles SHALL install a compiler, CMake, Ninja,
   `pkg-config`, or any `-dev` package (`libavahi-client-dev`,
   `libldns-dev`) once compilation moves to the host — none of that SHALL
   be needed there anymore.
3. Each binary SHALL be copied from a staging directory
   (`docker/poco_discovery_node/dist/poco_discovery_node`,
   `docker/dns_discovery_node/dist/dns_discovery_node`,
   `docker/dns_sd_discovery_node/dist/dns_sd_discovery_node`), each added to
   `.gitignore` — matching `chaos-node-host-build`'s Requirement 2.2/2.3
   exactly, for the same reason (avoiding any interaction with
   `.dockerignore`'s existing, unrelated `build/` exclusion).

### Requirement 2: Build orchestration stages each binary automatically

**User Story:** As a developer or CI job running
`cmake --build build --target docker-poco-discovery-tests` (or
`docker-dns-discovery-tests`/`docker-dns-sd-discovery-tests`/the individual
`docker-*-image` targets directly) exactly as today, I want the host build
and staging copy to happen automatically as part of that same command.

#### Acceptance Criteria

1. `docker-poco-discovery-image`, `docker-dns-discovery-image`, and
   `docker-dns-sd-discovery-image` (`tests/docker_chaos/CMakeLists.txt`)
   SHALL each gain a `DEPENDS` on their respective CMake target
   (`poco_discovery_node`, `dns_discovery_node`, `dns_sd_discovery_node`)
   and a `COMMAND` copying `$<TARGET_FILE:...>` to that target's staging
   directory, added *before* each target's existing `docker build`
   command — matching `chaos-node-host-build`'s Requirement 3.1 exactly.
2. Each addition SHALL be guarded on the same condition that target's own
   `cmd/.../CMakeLists.txt` gates its `add_executable()` on:
   `poco_discovery_node` → `if(POCO_DNSSD_FOUND AND folly_FOUND AND
   Boost_FOUND AND httplib_FOUND)`; `dns_discovery_node`/
   `dns_sd_discovery_node` → `if(LIBLDNS_FOUND AND folly_FOUND AND
   httplib_FOUND)` — the same "use the FOUND variables directly, not
   `if(TARGET ...)`" reasoning `chaos-node-host-build`'s Requirement 3.2
   documents (`cmd/poco_discovery_node`, `cmd/dns_discovery_node`, and
   `cmd/dns_sd_discovery_node` are all, like `cmd/chaos_node`, added to the
   top-level `CMakeLists.txt` *after* `tests/`).
3. `docker-bind9-image`, `docker-poco-discovery-tests`,
   `docker-dns-discovery-tests`, `docker-dns-sd-discovery-tests`, and every
   `docker-compose*.yml`/scenario-test file SHALL require no changes — all
   already treat these image tags as opaque, externally-built artifacts.

### Requirement 3: Discovery-node builds actually run on the relevant hosts

**User Story:** As a maintainer, I want the host environments that invoke
these `docker-*-image` targets to actually be able to build
`poco_discovery_node`/`dns_discovery_node`/`dns_sd_discovery_node`, not
silently skip them the way every host build does today.

#### Acceptance Criteria

1. `.github/workflows/arm64-docker-smoke-test.yml`'s "Install system
   dependencies" step SHALL gain `libavahi-client-dev` and `libldns-dev`
   (alongside `libfiu-dev`, already added by `chaos-node-host-build`) —
   without this, Requirement 2's host build would silently produce nothing
   to stage, exactly as `cmd/poco_discovery_node/CMakeLists.txt`'s/
   `cmd/dns_discovery_node/CMakeLists.txt`'s/
   `cmd/dns_sd_discovery_node/CMakeLists.txt`'s own `return()`-with-message
   guards already document happens today whenever
   `POCO_DNSSD_FOUND`/`LIBLDNS_FOUND` is false.
2. WHEN `POCO_DNSSD_FOUND`/`LIBLDNS_FOUND` is still false for a reason
   unrelated to this spec (e.g. a future runner image lacking a package
   this spec adds), the corresponding `docker-*-image` target's guard
   (Requirement 2.2) SHALL produce the same "target unavailable" outcome
   `chaos-node-host-build`'s Error Handling section already establishes —
   not a new, harder-to-diagnose failure mode.
3. `ci.yml`'s build-and-test job SHALL NOT be changed to install
   `libavahi-client-dev`/`libldns-dev` as part of this spec — it never
   invokes any `docker-*-image` target, so it has no need for these
   packages; making `poco_discovery_node`/`dns_discovery_node`/
   `dns_sd_discovery_node` build (and therefore get compiled and exercised)
   as part of the *default per-PR gate* is a related but separate concern,
   out of scope here (see Non-Goals).

### Requirement 4: No change to any image's external contract

**User Story:** As an operator or test author using
`kythira-poco-discovery:dev`/`kythira-dns-discovery:dev`/
`kythira-dns-sd-discovery:dev` today, I want this to be purely a
build-mechanism change.

#### Acceptance Criteria

1. Each image's tag, `ENTRYPOINT`, `EXPOSE`d ports, `HEALTHCHECK`, and every
   environment variable each binary reads SHALL be unchanged — matching
   `chaos-node-host-build`'s Requirement 5.1.
2. `docker/poco-discovery-compose.yml`, `docker/dns-discovery-compose.yml`,
   and `docker/dns-sd-discovery-compose.yml` SHALL require no changes —
   none has a compose-level `build:` section (confirmed by inspection, the
   same way `chaos-node-host-build`'s Requirement 5.2 confirmed it for
   `chaos_node`'s compose files).

### Requirement 5: Verification

**User Story:** As a maintainer, I want direct evidence all three rebuilt
images actually work — not just that each Dockerfile parses.

#### Acceptance Criteria

1. `docker-poco-discovery-tests`, `docker-dns-discovery-tests`, and
   `docker-dns-sd-discovery-tests` SHALL all pass when run against the
   rebuilt images, via the same `arm64-docker-smoke-test.yml` manual
   dispatch `chaos-node-host-build`'s Requirement 6.2 already uses — one
   dispatch covers verification for both specs' changes together, since
   they land in the same workflow.
2. Because none of these three targets have a fault-injection analog to
   verify, ordinary scenario-test passage (which already starts real
   containers and confirms real peer discovery over the network, per each
   test's own existing assertions) is sufficient evidence — no additional,
   narrower spot-check is needed the way `chaos-node-host-build`'s
   Requirement 6.1 needed one specifically for libfiu parity.
