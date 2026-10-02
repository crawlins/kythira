# Requirements Document — CI Build-Matrix Coverage

## Introduction

Several build configurations that this repository's own specs, Kconfig help
text and defconfig comments describe as supported have never been built by
CI. Each one is a claim that nothing checks, so it can rot silently: the
first person to find out is whoever next tries the configuration by hand.

This spec closes the gaps found by the 2026-10-02 parity audit (items F1 and
C1) and the spec gap analysis of the same day (Tier 3, items 26, 27, 30 and
part of 31). Every gap below was re-verified against `origin/main` at
`9c738d2`:

| Gap | Evidence on main |
|---|---|
| **G1. `CONFIG_FOLLY=n` is never built.** The `FOLLY` Kconfig help says deselecting it "is fully supported", and `certificate_authority` and `tests/` "are also now correctly buildable with CONFIG_FOLLY=n". | Every CMake-configuring job in `ci.yml` applies `ci_full_defconfig`, `ci_gcp_defconfig` or `ci_ion_defconfig`, all of which leave `CONFIG_FOLLY` at its default `y`. The `future-backend-compat` matrix changes only `KYTHIRA_DEFAULT_FUTURE_BACKEND`; Folly is still found and linked. `folly` is an unconditional `vcpkg.json` dependency, so its headers sit on every job's include path. |
| **G2. The cantcoap and libnyoci CoAP backends are never built.** | `configs/ci_full_defconfig` says of `COAP_TRANSPORT_LIBNYOCI`/`COAP_TRANSPORT_CANTCOAP`: "neither is installed by any CI job". Every `vcpkg install` in `.github/workflows/*.yml` passes only `edhoc`, `gcp` or `ion`. The 8 alternate-backend test files (113 test cases) therefore compile to their "backend unavailable" stub, and the cantcoap DTLS and EDHOC work in #383 and the libnyoci DTLS-RPK work in #380 ran only on a developer machine. |
| **G3. clang-tidy has no CI gate.** `.kiro/specs/clang-tidy/tasks.md` task 4 claims "zero findings" over the full tree. | No workflow installs `clang-tidy` or invokes `static-analysis`. `.clang-tidy` sets `WarningsAsErrors: "*"`, so the target would fail on a finding, but nothing runs it. `TIDY_SOURCES` globs `src/`, `tests/` and `examples/` only, so `cmd/` (the production executables) is never analysed even by hand; `FORMAT_SOURCES` omits `cmd/` the same way. |
| **G4. `kconfig-check` is not run in CI**, contrary to kconfig-integration Requirement 5.4. | The `kconfig-check` target exists (`CMakeLists.txt:2050`) and passes on main today, but no workflow invokes it or `scripts/kconfig/check_defconfigs.py`. |
| **G5. Two checked-in defconfigs are never configured.** | `configs/minimal_defconfig` ("smallest buildable configuration", kconfig-integration Req 5.2) and `configs/no_cloud_defconfig` (cloud-object-persistence Req 16.3) are referenced by no workflow. |
| **G6. Optional-dependency isolation is never checked.** | `scripts/verify-optional-dependency-isolation.sh` (stdexec-future-backend Property 5) is exposed only as a manual custom target; no workflow invokes it. |

**Scope**: CI jobs, defconfigs and the small CMake changes those jobs need.
Production code changes are in scope only where a newly-built configuration
fails to compile or a newly-run test fails, and only as far as needed to make
the configuration genuinely pass (see Requirement 8).

## Glossary

- **Alternate CoAP backends**: the cantcoap- and libnyoci-backed CoAP
  transports, enabled by the opt-in `coap-cantcoap` / `coap-libnyoci` vcpkg
  features and the `CONFIG_COAP_TRANSPORT_CANTCOAP` /
  `CONFIG_COAP_TRANSPORT_LIBNYOCI` Kconfig symbols.
- **Strict mode**: `-DKYTHIRA_KCONFIG_STRICT=ON`, under which a symbol set `y`
  whose dependency is not found is a configure `FATAL_ERROR`.
- **Folly-free prefix**: a copy of `vcpkg_installed/<triplet>` with Folly and
  every port that depends on it removed, so that neither `find_package` nor
  an `#include` can reach Folly.
- **Stub test case**: the `*_backend_unavailable_is_skipped`-style case an
  alternate-backend test file compiles to when its backend's
  `*_AVAILABLE` macro is undefined.
- **Gate**: a CI job whose failure fails the workflow run, as opposed to an
  informational or `continue-on-error` step.

## Requirements

### Requirement 1: Build and test the alternate CoAP backends

**User Story:** As a maintainer, I want every pull request to build and run
the cantcoap and libnyoci backends' real test suites, so that a change to
shared CoAP code cannot silently break a backend nobody builds.

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL
   install the `coap-cantcoap` and `coap-libnyoci` vcpkg features (together
   with `edhoc`) and configure with both
   `CONFIG_COAP_TRANSPORT_CANTCOAP=y` and `CONFIG_COAP_TRANSPORT_LIBNYOCI=y`
   under strict mode.
2. The job's defconfig SHALL be checked in under `configs/` and SHALL differ
   from `ci_full_defconfig` only in the alternate-backend symbols, so that a
   failure in this job is attributable to the alternate backends.
3. WHEN the job's tests run THEN every `coap_cantcoap_*` and
   `coap_libnyoci_*` test target SHALL run, and the job SHALL fail if any of
   them reports only its stub test case or is skipped.
4. The job SHALL also run the backend-independent CoAP test targets whose
   behaviour changes when an alternate backend is compiled in (any test that
   instantiates more than one backend, such as concept-conformance or
   cross-backend suites), so that the alternates are tested against the
   shared code they consume.
5. The job SHALL use the OCI build cache (vcpkg binary cache and sccache) the
   same way the existing vcpkg jobs do, so that a warm run costs only the
   alternate-backend delta.
6. The alternate-backend features SHALL NOT be added to the five
   `build-and-test` legs, so that those legs' vcpkg feature set, cache key
   and runtime do not change.

### Requirement 2: Build and test with `CONFIG_FOLLY=n`

**User Story:** As a maintainer, I want CI to prove the "Folly is optional"
claim, so that a stray `#include <folly/...>` or a `folly::` call in
backend-independent code fails a pull request instead of a user's build.

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL
   configure with `CONFIG_FOLLY=n`, `CONFIG_PROXYGEN_TRANSPORT=n` and a
   non-Folly default future backend under strict mode, from a checked-in
   defconfig.
2. The job SHALL configure against a Folly-free prefix, so that Folly's
   headers and libraries are unreachable to both `find_package` and the
   preprocessor. Hiding Folly from `find_package` alone is not sufficient,
   because vcpkg's shared include directory would still satisfy
   `#include <folly/...>`.
3. The job SHALL build every target the configuration enables and SHALL run
   the resulting test suite with the same label exclusions as
   `build-and-test`.
4. The job SHALL fail if `folly_FOUND` is true in `CMakeCache.txt`, or if any
   generated link command references a Folly library, so that the job cannot
   silently regress into a Folly build.
5. The job SHALL publish the list of targets and tests the configuration
   disabled (for example the Folly-only concept-wrapper tests and the
   proxygen transport), and the job SHALL fail if a test target registered
   under `ci_full_defconfig` disappears without being on a checked-in
   allowlist that names why it requires Folly.

### Requirement 3: clang-tidy gate

**User Story:** As a maintainer, I want clang-tidy to run in CI, so that the
"zero findings" baseline from the clang-tidy spec is enforced rather than
asserted.

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL run
   the `static-analysis` target with the clang-tidy major version named in
   `.clang-tidy`'s header, against a compilation database produced by a
   `ci_full_defconfig` configure.
2. Generated sources and headers (Protobuf, Kconfig's `autoconf.hpp`, and any
   other build-time outputs a translation unit includes) SHALL exist before
   clang-tidy runs, so that no finding is an artefact of a missing header.
3. `TIDY_SOURCES` SHALL include `cmd/**/*.cpp`, and `FORMAT_SOURCES` SHALL
   include `cmd/**/*.cpp` and `cmd/**/*.hpp`, so that the production
   executables are held to the same rules as `src/`, `tests/` and
   `examples/`. Any reformatting this causes SHALL land in the same change.
4. The job SHALL fail on any finding. Findings present on `main` when the
   gate is introduced SHALL be fixed or suppressed with a justified `NOLINT`
   in the same change, never baselined by disabling the gate.
5. IF a full-tree run exceeds the job's time budget (Requirement 7) THEN pull
   request runs MAY analyse only translation units affected by the diff,
   provided pushes to `main` still analyse the full tree and the scoping rule
   is documented in the job.

### Requirement 4: `kconfig-check` in CI

**User Story:** As a maintainer, I want CI to validate every defconfig against
`Kconfig`, so that renaming or removing a symbol cannot leave a stale
defconfig behind (kconfig-integration Requirement 5.4).

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL run
   `scripts/kconfig/check_defconfigs.py` over every `configs/*_defconfig` and
   fail on any error or warning it reports.
2. The job SHALL NOT install vcpkg or configure CMake, so that it reports in
   about a minute.
3. The job SHALL fail if any `configs/*_defconfig` is neither applied by some
   workflow nor listed, with a one-line reason, in a checked-in allowlist, so
   that a new defconfig cannot be added and then forgotten.

### Requirement 5: Configure and build the remaining defconfigs

**User Story:** As a maintainer, I want the documented minimal and no-cloud
configurations to be built, so that their headers' claims stay true.

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL
   configure `configs/minimal_defconfig` and build every target it enables.
2. The same job SHALL configure `configs/no_cloud_defconfig` without strict
   mode (as its own header requires), build
   `object_store_persistence_unit_test`, and run it, as the header's own
   usage block describes.
3. Each configuration SHALL use its own build directory, so that cached
   CMake state from one cannot leak into the other.

### Requirement 6: Optional-dependency isolation in CI

**User Story:** As a maintainer, I want stdexec-future-backend Property 5
checked automatically, so that a Folly-only build cannot start depending on
stdexec.

#### Acceptance Criteria

1. WHEN CI runs on a push to `main` or on a pull request THEN a job SHALL run
   `scripts/verify-optional-dependency-isolation.sh` against the job's
   installed vcpkg tree and fail if the script fails.
2. The script SHALL be runnable from CI without modification to the vcpkg
   tree; if it needs a different prefix path, that SHALL be passed in, not
   hard-coded.

### Requirement 7: Cost and placement

**User Story:** As a maintainer, I want the new coverage to cost as little CI
time as possible, so that it does not slow every pull request.

#### Acceptance Criteria

1. Every new job SHALL run on `ubuntu-24.04` (x64) only. None of the gaps is
   architecture-specific, and the arm64 legs already cover the default
   configuration.
2. New jobs SHALL run in parallel with `build-and-test`, not after it.
3. Every new vcpkg-installing job SHALL use the OCI build cache action and
   sccache, and SHALL set a `timeout-minutes` with a comment recording the
   warm and cold durations it was sized from.
4. Requirements 5 and 6 SHALL share one job, so that they share one vcpkg
   install.
5. Every new ctest invocation SHALL be followed by `scripts/check-test-run.sh`
   with a `--floor`, so that a filter that matches nothing cannot pass.

### Requirement 8: Fixing what the new jobs find

**User Story:** As a maintainer, I want each new gate to land green, so that
it protects `main` from the day it merges.

#### Acceptance Criteria

1. WHEN a newly-built configuration fails to compile or a newly-run test fails
   THEN the failure SHALL be root-caused and fixed in the change that adds the
   job, OR, when the fix is large or belongs to another spec, the job SHALL
   exclude exactly that target or test with a comment naming the follow-up,
   and `doc/TODO.md` SHALL record it.
2. No gate SHALL be introduced with `continue-on-error`.
3. Stale claims the new jobs disprove (Kconfig help text, defconfig comments,
   spec status lines, the root `CMakeLists.txt` comment above
   `kythira_find_optional(FOLLY folly)` that still says Folly is required
   transitively) SHALL be corrected in the same change.

### Requirement 9: Non-goals

1. DNS peer discovery and ACME dns-01 (`CONFIG_DNS_DISCOVERY`, libldns) are
   out of scope: no CI image installs libldns, and adding it is a separate
   dependency decision (spec gap analysis item 28).
2. Running the Docker chaos, elastic-capacity and DNS scenario tests per pull
   request, and adding a rootless Podman job, are out of scope (gap analysis
   item 29).
3. Persistent compiler caches for `coap-flake-measure.yml` and
   `arm64-docker-smoke-test.yml`, and running `future_backend_benchmark_test`,
   are out of scope (gap analysis item 31).
4. Decoupling the HTTP and CoAP transports from `folly::Future` at the header
   level is out of scope. Requirement 2 proves what is already claimed; it
   does not widen the claim.
5. Fixing the alternate-backend functional gaps the parity audit lists
   (C2 to C12) is out of scope. Requirement 1 makes their tests run; it does
   not add the missing behaviour.
