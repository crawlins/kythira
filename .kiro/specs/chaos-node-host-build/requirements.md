# Chaos Node Host Build Requirements Document

## Introduction

`docker/chaos_node/Dockerfile` currently compiles `chaos_node` *inside* its
own builder stage, using a small, hand-maintained `apt-get install` list
(`libboost-all-dev`, `nlohmann-json3-dev`, `libssl-dev`, `libfiu-dev`) instead
of this project's real dependency manager, vcpkg. That list has drifted out
of sync with what `chaos_node` actually needs to build: `cmd/chaos_node`'s
own `add_subdirectory()` call (`CMakeLists.txt`) — and `cmd/chaos_node`'s own
`CMakeLists.txt`, redundantly — both require `folly_FOUND AND Boost_FOUND
AND httplib_FOUND`, and folly is not installable via a plain `apt-get
install` on Ubuntu 24.04 at all. The result, discovered while validating
`.kiro/specs/otlp-telemetry-backend/`'s `docker-otlp-collector-tests` via a
manual `arm64-docker-smoke-test.yml` dispatch (see `doc/TODO.md`'s Minor
Enhancements entry for that investigation): `cmake --build build --target
chaos_node` inside the Dockerfile fails with `ninja: error: unknown target
'chaos_node'` — the `chaos_node` target is never even defined in that
build tree, so nothing about fixing the apt list alone (short of also
somehow apt-installing folly, which isn't possible) can fix it.

Three sibling images already avoid this exact problem a different way:
`docker/poco_discovery_node/Dockerfile`, `docker/dns_discovery_node/Dockerfile`,
and `docker/dns_sd_discovery_node/Dockerfile` each still *compile inside
Docker*, but point `CMAKE_PREFIX_PATH` at a `vcpkg_installed/<triplet>` tree
that `COPY . .` brings in from the host — i.e. they reuse the host's
already-vcpkg-built *dependencies* without re-bootstrapping vcpkg in-image
(`.dockerignore`'s own comment documents this convention explicitly).
`chaos_node`'s Dockerfile is the one holdout still trying to reconstruct an
equivalent dependency set from scratch via `apt-get`.

This spec goes one step further than that existing pattern, per an explicit
design discussion: rather than *also* recompiling `chaos_node`'s own source
inside Docker (reusing only the host's pre-built *dependencies*), build the
`chaos_node` *binary itself* on the host — using the exact same real,
already-proven vcpkg-based CMake configuration `ci.yml` already uses
successfully for every other target — and have `docker/chaos_node/Dockerfile`
do nothing but package that already-built binary into a minimal runtime
image. This eliminates the class of bug this spec starts from by
construction (there is no second, divergent build configuration to drift
out of sync with `cmd/chaos_node/CMakeLists.txt`'s real requirements), and
avoids recompiling `chaos_node`'s source a second time for image packaging
when a CI job's own host build already compiled it once.

## Glossary

- **Host build**: compiling `chaos_node` on the machine invoking `docker
  build` (a GitHub Actions runner, or a developer's own machine), using the
  project's normal, real CMake configuration — as opposed to compiling it a
  second time inside a Docker builder stage.
- **Staging directory**: `docker/chaos_node/dist/`, a new, git-ignored
  directory holding the host-built `chaos_node` binary, positioned so it is
  inside the Docker build context (repository root) but outside every
  existing `.dockerignore` exclusion pattern.
- **Runtime-only Dockerfile**: a single-stage `docker/chaos_node/Dockerfile`
  that only installs runtime packages and `COPY`s in an already-built
  binary — no compiler, no CMake, no builder stage.
- **`docker-chaos-image`**: the existing `tests/docker_chaos/CMakeLists.txt`
  custom target that builds the `kythira-chaos-node:dev` image; every
  `docker_chaos` scenario test (and now `docker-otlp-collector-tests`)
  depends on it.

## Requirements

### Requirement 1: Host build reuses the project's real CMake configuration

**User Story:** As a maintainer, I want `chaos_node`'s Docker image build to
use the exact same dependency resolution `ci.yml` already proves works, so
this Dockerfile can never again drift out of sync with `cmd/chaos_node`'s
actual requirements the way its apt-only builder stage did.

#### Acceptance Criteria

1. `chaos_node` SHALL be built via the project's ordinary top-level CMake
   configuration (`cmake -B build ... -DCMAKE_PREFIX_PATH=<vcpkg_installed
   tree> && cmake --build build --target chaos_node`) — the same shape
   `ci.yml`'s build-and-test job and `poco_discovery_node`'s/
   `dns_discovery_node`'s/`dns_sd_discovery_node`'s Dockerfiles already use,
   not a new, third build configuration.
2. `docker/chaos_node/Dockerfile` SHALL NOT install a compiler, CMake,
   Ninja, or any `-dev` package for `chaos_node`'s own dependencies
   (Boost, OpenSSL, nlohmann-json, libfiu-dev's headers) — none of that
   SHALL be needed once compilation moves to the host.
3. WHEN the host environment cannot satisfy `folly_FOUND AND Boost_FOUND AND
   httplib_FOUND` (i.e. `chaos_node` is not itself buildable there), the
   host build step SHALL fail with the same clear, existing CMake/ninja
   diagnostics a plain `cmake --build build --target chaos_node` already
   produces — no new, harder-to-diagnose failure mode SHALL be introduced.

### Requirement 2: `chaos_node`'s Dockerfile becomes runtime-only

**User Story:** As a maintainer, I want `docker/chaos_node/Dockerfile` to do
nothing but package an already-built binary, so an image rebuild after a
source change is fast (no in-container recompilation) and the Dockerfile
itself is trivial to review — no build toolchain, no dependency list to
maintain in two places.

#### Acceptance Criteria

1. `docker/chaos_node/Dockerfile` SHALL become a single stage: `FROM
   ubuntu:24.04`, install only the runtime packages already listed in its
   current runtime stage (`libfiu0`, `fiu-utils`, `iproute2`, `iptables`,
   `curl` — unchanged from today), `COPY` in the pre-built binary and
   `entrypoint.sh`, and keep the existing `EXPOSE`/`HEALTHCHECK`/
   `ENTRYPOINT` unchanged.
2. The binary SHALL be copied from the staging directory
   (`docker/chaos_node/dist/chaos_node`), not from a host `build/` directory
   directly — `.dockerignore`'s existing `build/` exclusion (there for a
   real, unrelated reason: preventing a stale host `CMakeCache.txt` from
   corrupting an in-container reconfigure for the *other* three Dockerfiles
   that still do compile in-container) SHALL NOT need to change.
3. `docker/chaos_node/dist/` SHALL be added to `.gitignore` — it holds a
   build artifact, not source.

### Requirement 3: Build orchestration stages the binary automatically

**User Story:** As a developer or CI job running `cmake --build build
--target docker-chaos-tests` (or `docker-chaos-image` directly) exactly as
today, I want the host build and staging copy to happen automatically as
part of that same command, so no new manual step or separate script
invocation is required to keep using this target the way it's used today.

#### Acceptance Criteria

1. The existing `docker-chaos-image` custom target
   (`tests/docker_chaos/CMakeLists.txt`) SHALL gain a dependency on the
   `chaos_node` CMake target and a `COMMAND` copying its built output
   (`$<TARGET_FILE:chaos_node>`) to `docker/chaos_node/dist/chaos_node`
   *before* its existing `docker build` command — added to that target
   directly (not a new, separate intermediate target), since "build the
   image" already conceptually includes "have something to put in it."
2. This addition SHALL be guarded consistently with this file's existing
   defensive style (e.g. `if(TARGET Folly::folly)`, `if(TARGET
   httplib::httplib)` elsewhere in the same file): wrapped in `if(folly_FOUND
   AND Boost_FOUND AND httplib_FOUND)` (the same condition
   `cmd/chaos_node`'s own inclusion is gated on at the top level), so an
   environment where `chaos_node` genuinely cannot be built produces a
   clear "target unavailable" outcome rather than an opaque CMake
   dependency-resolution error.
3. No changes SHALL be required to `.github/workflows/arm64-docker-smoke-test.yml`'s
   invocation (`cmake --build build --target docker-chaos-tests`) or to any
   `docker-compose*.yml` file — both already treat `kythira-chaos-node:dev`
   as an opaque, externally-built image tag, and that workflow's existing
   "Configure" step already sets `CMAKE_PREFIX_PATH` at the vcpkg tree it
   bootstraps, satisfying Requirement 1.1 with no workflow-file change.

### Requirement 4: Fault-injection support is preserved, not silently dropped

**User Story:** As a maintainer relying on `chaos_node`'s libfiu-based fault
injection for the existing chaos scenario tests, I want the switch to a host
build to not silently produce a binary with fault injection compiled out,
so `persistence_faults_test`, `docker_chaos_safety_assertions_test`, and
every other fault-injection-dependent scenario test continue to mean what
they say.

#### Acceptance Criteria

1. Fault-injection support (`CHAOS_TESTS_ENABLED`, set by
   `pkg_check_modules(FIU QUIET libfiu)` finding `libfiu-dev` at host
   configure time — see `CMakeLists.txt`) SHALL be present in the
   host-built `chaos_node` binary used for the Docker image, matching
   today's in-container build (whose builder stage already `apt-get
   install`s `libfiu-dev`).
2. `.github/workflows/arm64-docker-smoke-test.yml`'s "Install system
   dependencies" step SHALL gain `libfiu-dev`, since that workflow's host
   build currently does not install it (unlike `ci.yml`'s build-and-test
   job, which already does) — without this, Requirement 3's host build
   would silently produce a `chaos_node` with fault injection compiled out,
   a real regression from today's in-container-built image.
3. The existing `-DKYTHIRA_FAULT_INJECTION=ON` flag the current Dockerfile
   passes to its in-container `cmake -B build` invocation SHALL NOT be
   carried over — it is dead: `cmd/chaos_node/CMakeLists.txt` gates fault
   injection on `CHAOS_TESTS_ENABLED` (an auto-detected variable), not on a
   `KYTHIRA_FAULT_INJECTION` CMake cache variable, and CMake's own "Manually-
   specified variables were not used by the project: KYTHIRA_FAULT_INJECTION"
   warning already confirms this flag has never done anything.

### Requirement 5: No change to the image's external contract

**User Story:** As an operator or test author using `kythira-chaos-node:dev`
today (compose files, `docker_chaos` scenario tests, manual `docker run`),
I want this to be purely a build-mechanism change — same image tag, same
entrypoint, same ports, same environment variables — so nothing downstream
needs to change.

#### Acceptance Criteria

1. The image tag (`kythira-chaos-node:dev`), `ENTRYPOINT`
   (`/entrypoint.sh`), `EXPOSE`d ports (7000/8080/9000), `HEALTHCHECK`, and
   every environment variable `entrypoint.sh`/`config.hpp` reads SHALL be
   unchanged.
2. `docker/docker-compose.yml`, `docker/docker-compose.quorum.yml`, and
   `docker/otlp-collector-compose.yml` SHALL require no changes — none of
   them have a compose-level `build:` section (confirmed: they only
   reference `image: kythira-chaos-node:dev`, always built externally via
   `docker-chaos-image` or by hand), so nothing about how they *consume*
   the image is affected.

### Requirement 6: Verification

**User Story:** As a maintainer, I want direct evidence this actually works
end to end — a real container that starts, passes its health check, and
still exercises fault injection correctly — not just "the Dockerfile
parses."

#### Acceptance Criteria

1. The existing `docker_chaos` scenario test suite (which already starts
   real `kythira-chaos-node:dev` containers and polls `/health` before
   proceeding) SHALL be run against the rebuilt image and SHALL pass,
   including at least one fault-injection-dependent scenario test
   (`docker_chaos_persistence_faults_test` or
   `docker_chaos_safety_assertions_test`) — this is direct evidence that
   Requirement 4's libfiu-dev fix actually took effect, not just that the
   image builds.
2. This SHALL be exercised via the same `arm64-docker-smoke-test.yml`
   manual dispatch already used to discover the original bug (Requirement
   1's motivating investigation) — no new workflow SHALL be needed for
   this spec's own verification.
