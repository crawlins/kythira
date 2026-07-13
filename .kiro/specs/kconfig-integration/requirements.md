# Requirements Document

## Introduction

Kythira's optional-dependency surface has grown organically: `folly`, `Boost`,
`cpp-httplib`, `OpenSSL`, `libcoap`, `libldns`, `lakers` (EDHOC), Poco DNSSD,
the AWS SDK (core + `acm-pca`), `libssh2`, and `libfiu` are each probed with
their own `find_package(... QUIET)` call in the root `CMakeLists.txt`, and
each gates its own ad hoc `target_compile_definitions` macro
(`KYTHIRA_HAS_LDNS`, `KYTHIRA_HAS_AWS_SDK`, `KYTHIRA_HAS_AWS_ACM_PCA`,
`LAKERS_AVAILABLE`, `LIBCOAP_AVAILABLE`, `CPPHTTPLIB_OPENSSL_SUPPORT`,
`CHAOS_TESTS_ENABLED`, `FIU_ENABLE`, ...) and its own chain of
`if(X_FOUND AND Y_FOUND)` guards across the root `CMakeLists.txt`,
`cmd/*/CMakeLists.txt`, and `tests/CMakeLists.txt`. There is no single place
to see the whole feature matrix, no way to express "`AWS_ACM_PCA` requires
`AWS_SDK`" as a declared constraint (today it's just two independent
`find_package` calls that happen to both need to succeed), and no way to ask
for a specific feature set and get a hard failure if it can't be satisfied —
today every optional dependency silently degrades to "feature disabled" if
`find_package` doesn't find it, which is convenient for opportunistic CI
builds but hides misconfiguration (e.g. a developer who believes they built
with EDHOC support because vcpkg's `edhoc` feature is listed in
`vcpkg.json`, but the Rust toolchain needed to build `lakers` from source was
missing, so `LAKERS_AVAILABLE` was silently never defined).

This specification defines requirements for introducing
[Kconfig](https://www.kernel.org/doc/html/latest/kbuild/kconfig-language.html)
— the same configuration language used by the Linux kernel, Zephyr, Buildroot,
and coreboot — as a declarative front end over this existing dependency
matrix, using [Kconfiglib](https://github.com/ulfalizer/Kconfiglib) (a
pure-Python Kconfig implementation with no C toolchain dependency) as the
parser/`menuconfig` engine. Kconfig does not replace `find_package` —
CMake still does the actual probing of what's installed — but it becomes the
single source of truth for (a) the tree of available features and their
`depends on` relationships, (b) whether a given build wants a feature at all,
and (c) whether "wanted but not found" should degrade silently (today's
behavior, kept as the default) or fail the configure step loudly (an opt-in
mode for reproducible/CI builds).

This is a build-tooling integration in the same family as the existing
`clang-format`, `clang-tidy`, and `code-coverage` specs: a root-level config
artifact, generated CMake/C++ glue, and custom build targets, laid out as a
spec here before implementation.

## Glossary

- **Kconfig**: The configuration language and root file (`Kconfig` at the
  repository root) declaring `config` symbols, `menu`s, `depends on` and
  `select` relationships, and `choice` groups.
- **Kconfiglib**: The pure-Python library (`pip install kconfiglib`) used to
  parse `Kconfig`, load/save `.config` files, and drive `menuconfig`/
  `guiconfig`/`savedefconfig` — used instead of the kernel's C
  `kconfig-frontends` (`mconf`/`nconf`) so no new C toolchain dependency is
  introduced.
- **`.config`**: The generated file recording the resolved value of every
  Kconfig symbol for one configuration (analogous to a CMake cache, but
  Kconfig-scoped). Not committed; lives under `build/`.
- **defconfig**: A minimal, checked-in `.config` fragment (only symbols that
  differ from their Kconfig-declared default) used to reproduce a named
  configuration non-interactively, e.g. `configs/ci_full_defconfig`.
- **CONFIG symbol**: A single Kconfig-declared option, e.g. `CONFIG_AWS_SDK`,
  with a boolean, tristate, string, or choice value.
- **Feature macro**: The existing C++ preprocessor macro a CONFIG symbol
  ultimately controls (e.g. `CONFIG_AWS_SDK` → `KYTHIRA_HAS_AWS_SDK`). Kconfig
  integration preserves every existing macro name so no call site changes.
- **Autoconf artifacts**: The two generated files produced from `.config` on
  every CMake configure: `build/generated/autoconf.cmake` (CMake `set()`
  statements) and `build/generated/kythira/autoconf.hpp` (C++ `#define`s).
- **Strict mode**: The build mode (`KYTHIRA_KCONFIG_STRICT=ON`) in which a
  CONFIG symbol set to `y` makes the corresponding `find_package` call
  `REQUIRED`, failing configure if the dependency isn't actually installed —
  as opposed to the default **autodetect mode**, which preserves today's
  silent-degrade behavior.

## Requirements

### Requirement 1

**User Story:** As a developer, I want a single root `Kconfig` file
enumerating every optional dependency and feature currently scattered across
`CMakeLists.txt`, so that the full feature matrix and its dependency
constraints are visible in one place.

#### Acceptance Criteria

1. WHEN the root `Kconfig` file is read THEN it SHALL declare one `config`
   symbol for every optional dependency currently gated by a `find_package(...
   QUIET)` call in the root `CMakeLists.txt`: OpenSSL, HTTP transport
   (`cpp-httplib`), CoAP transport (`libcoap`), EDHOC (`lakers`), DNS peer
   discovery (`libldns`), Poco DNS-SD discovery, the AWS SDK core component
   group, AWS ACM Private CA, `libssh2` (real-EC2 tests), and `libfiu`
   (chaos tests).
2. WHEN a feature's current CMake guard already expresses a prerequisite
   (e.g. `CPPHTTPLIB_OPENSSL_SUPPORT` requires both `httplib_FOUND` and
   `OpenSSL_FOUND`; `KYTHIRA_HAS_AWS_ACM_PCA` requires the AWS SDK core
   component) THEN the corresponding Kconfig symbol SHALL declare that
   prerequisite with `depends on`, so an inconsistent combination cannot be
   selected in `menuconfig` in the first place.
3. WHEN `ENABLE_COVERAGE` is considered THEN it SHALL also become a Kconfig
   symbol (`CONFIG_COVERAGE`) rather than remaining a bare CMake `option()`,
   so it appears in the same feature tree as every other build toggle.
4. WHEN the default future backend selection introduced by the
   `stdexec-future-backend` spec (`KYTHIRA_DEFAULT_FUTURE_BACKEND`,
   values `folly`/`stdexec`) is considered THEN it SHALL be expressed as a
   Kconfig `choice` (`CONFIG_DEFAULT_FUTURE_BACKEND_FOLLY` /
   `CONFIG_DEFAULT_FUTURE_BACKEND_STDEXEC`) rather than a raw CMake cache
   string, so the two specs compose instead of defining two separate
   selection mechanisms.
5. WHEN a symbol's `help` text is read THEN it SHALL name the concrete
   library, the CMake/vcpkg name used to find it, and which macro it
   ultimately controls, so `menuconfig`'s help pane is sufficient
   documentation without cross-referencing `CMakeLists.txt`.

### Requirement 2

**User Story:** As a developer, I want to run an interactive `menuconfig` (or
a non-interactive `defconfig`) without installing any new C toolchain, so
that configuring the feature set is as easy as running the existing
`clang-format`/`clang-tidy` tooling.

#### Acceptance Criteria

1. WHEN `menuconfig` tooling is chosen THEN the system SHALL use Kconfiglib
   (pure Python) rather than the kernel's C `mconf`/`nconf`, so no new
   compiled dependency is added to the build machine.
2. WHEN `cmake --build build --target menuconfig` is invoked THEN the system
   SHALL launch Kconfiglib's terminal `menuconfig` against the root `Kconfig`
   file and the current (or default) `.config`, writing the result to
   `build/.config`.
3. WHEN `cmake --build build --target guiconfig` is invoked AND a Tk display
   is available THEN the system SHALL launch Kconfiglib's Tkinter-based
   `guiconfig` as an alternative to the terminal UI.
4. WHEN Python 3 or the `kconfiglib` package is not found on the build
   machine THEN the `menuconfig`/`guiconfig`/`savedefconfig` targets SHALL
   print an actionable install instruction (`pip install kconfiglib`) and
   fail only those targets — they SHALL NOT block ordinary configure/build.
5. WHEN `cmake --build build --target savedefconfig` is invoked THEN the
   system SHALL write the minimal defconfig (only symbols differing from
   their declared default) for the current `build/.config` to
   `build/defconfig`, ready to be copied into `configs/`.

### Requirement 3

**User Story:** As a build system maintainer, I want `.config` to be
translated into both CMake variables and a C++ header on every configure, so
that CMake's `find_package` guards and the existing `#ifdef` call sites both
consume Kconfig's decisions without duplicating them.

#### Acceptance Criteria

1. WHEN CMake configures THEN it SHALL, before any `find_package` call in the
   root `CMakeLists.txt`, `include(cmake/Kconfig.cmake)`, which SHALL resolve
   a `.config` (from `-DKYTHIRA_KCONFIG=<path>`, or `build/.config` if
   present, or the `Kconfig`-declared defaults if neither exists) and
   generate `build/generated/autoconf.cmake` and
   `build/generated/kythira/autoconf.hpp` from it via a Kconfiglib-based
   script.
2. WHEN `build/generated/autoconf.cmake` is generated THEN it SHALL define
   one CMake variable per CONFIG symbol (e.g. `KCONFIG_AWS_SDK`) that
   `CMakeLists.txt` reads to decide whether to attempt/require the matching
   `find_package` call.
3. WHEN `build/generated/kythira/autoconf.hpp` is generated THEN it SHALL
   `#define` exactly the existing feature-macro names
   (`KYTHIRA_HAS_LDNS`, `KYTHIRA_HAS_AWS_SDK`, `KYTHIRA_HAS_AWS_ACM_PCA`,
   `LAKERS_AVAILABLE`, `LIBCOAP_AVAILABLE`, `CPPHTTPLIB_OPENSSL_SUPPORT`,
   `KYTHIRA_HAS_OPENSSL`, `KYTHIRA_HAS_POCO_DNSSD`, `FIU_ENABLE`) so that no
   existing `#ifdef` site in `include/`, `src/`, `tests/`, or `cmd/` needs to
   change.
4. WHEN a CONFIG symbol ultimately resolves to "off" (either by explicit
   `.config` value or because the underlying library was not found in
   autodetect mode) THEN the corresponding macro SHALL simply be absent from
   `autoconf.hpp`, matching today's `#ifdef`-not-defined behavior exactly.
5. WHEN `network_simulator` and other library targets are built THEN
   `build/generated` SHALL be on their include path so `autoconf.hpp` is
   reachable, but this SHALL NOT change the public installed include layout
   under `include/` — `autoconf.hpp` is a build artifact, not an installed
   header.

### Requirement 4

**User Story:** As a developer building for CI or a reproducible deployment,
I want an explicit "strict" mode in which requesting a feature that isn't
actually installed fails the build loudly, so that misconfiguration is caught
at configure time instead of discovered later as a silently-disabled feature.

#### Acceptance Criteria

1. WHEN `KYTHIRA_KCONFIG_STRICT` is unset or `OFF` (the default) THEN a
   CONFIG symbol set to `y` SHALL cause CMake to attempt the corresponding
   `find_package` as `QUIET` and, if not found, print the existing
   `message(WARNING ...)` and leave the feature disabled — i.e. today's
   behavior is preserved byte-for-byte when Kconfig is not engaged.
2. WHEN `KYTHIRA_KCONFIG_STRICT=ON` THEN a CONFIG symbol set to `y` SHALL
   cause CMake to invoke the corresponding `find_package` as `REQUIRED`,
   aborting configure with CMake's standard not-found error if the
   dependency is missing.
3. WHEN a CONFIG symbol is `n` (or absent) THEN the corresponding
   `find_package` call SHALL be skipped entirely regardless of strict mode,
   even if the library happens to be installed on the host — Kconfig `n`
   means "do not link this in," not "don't require it."
4. WHEN no `.config` is supplied at all (bare `cmake -S . -B build`, no prior
   `menuconfig`, no `-DKYTHIRA_KCONFIG=...`) THEN every CONFIG symbol SHALL
   default to a value that reproduces today's zero-configuration behavior —
   concretely, every optional-dependency symbol defaults to `y` in
   autodetect mode, so "try to find everything, silently skip what's
   missing" remains the out-of-the-box experience.

### Requirement 5

**User Story:** As a project maintainer, I want checked-in defconfigs for the
configurations we actually build in practice, so that "the full dev build"
and "the minimal build" are named, reproducible, and validated in CI rather
than tribal knowledge.

#### Acceptance Criteria

1. WHEN the repository is cloned THEN `configs/ci_full_defconfig` SHALL exist,
   selecting every optional feature (`y`) with `KYTHIRA_KCONFIG_STRICT`
   implied `ON` for the CI job that uses it, so CI fails fast if a dependency
   silently stops being installable in the CI image.
2. WHEN the repository is cloned THEN `configs/minimal_defconfig` SHALL
   exist, selecting only the hard-required core (folly, Boost, HTTP
   transport) with every optional feature `n`, demonstrating the smallest
   buildable configuration.
3. WHEN a defconfig is applied (`cmake -S . -B build
   -DKYTHIRA_KCONFIG=configs/ci_full_defconfig`) THEN the system SHALL
   produce the same `autoconf.cmake`/`autoconf.hpp` output as manually
   running `menuconfig` and saving the same selections.
4. WHEN CI runs THEN a `kconfig-check` step SHALL load every file under
   `configs/` with Kconfiglib and confirm it parses against the current
   `Kconfig` and produces no "symbol referenced in defconfig no longer
   exists" warnings, catching drift between `Kconfig` edits and stale
   defconfigs.

### Requirement 6

**User Story:** As a contributor to the container-based chaos tests, I want
Kconfig to explicitly not govern container-runtime selection, so that the
project's Docker/Podman portability rules aren't accidentally baked into a
static, checked-in config file.

#### Acceptance Criteria

1. WHEN the Kconfig integration is designed THEN `CONTAINER_RUNTIME` and
   `COMPOSE_COMMAND` (the existing `tests/docker_chaos/CMakeLists.txt` cache
   variables, and their `KYTHIRA_CONTAINER_RUNTIME`/`KYTHIRA_COMPOSE_COMMAND`
   environment-variable counterparts) SHALL remain plain CMake cache
   variables resolved by `find_program` at configure time, and SHALL NOT be
   modeled as Kconfig symbols.
2. WHEN a CONFIG symbol enables the chaos-test executables generally (e.g.
   whether `CHAOS_TESTS_ENABLED` is derived from `CONFIG_CHAOS_TESTS`) THEN
   it SHALL only gate whether the fault-injection (`libfiu`) code path
   compiles, not which container runtime the Docker-based scenario tests use.
3. WHEN documentation for this feature is written THEN it SHALL state
   explicitly that no checked-in defconfig may set a static container IP,
   force a specific runtime as the only option, or otherwise reintroduce the
   host-portability problem the container-runtime rules in `CLAUDE.md`
   exist to prevent.

### Requirement 7

**User Story:** As a developer relying on vcpkg for dependency installation,
I want the relationship between vcpkg features and Kconfig symbols documented
and kept in sync, so that enabling a Kconfig symbol without the matching
vcpkg feature installed produces a clear, actionable error rather than
confusion about which layer is responsible.

#### Acceptance Criteria

1. WHEN a Kconfig symbol corresponds to a vcpkg optional feature (currently
   only `edhoc`, backing `CONFIG_EDHOC`) THEN the symbol's `help` text SHALL
   name the vcpkg feature and reference `vcpkg-overlays/lakers/README.md` for
   installation.
2. WHEN `KYTHIRA_KCONFIG_STRICT=ON` and `CONFIG_EDHOC=y` but `lakers` is not
   found THEN the resulting `find_package(lakers CONFIG REQUIRED)` failure
   message SHALL be left as CMake's standard diagnostic (which already names
   the missing package) rather than being swallowed or replaced — Kconfig
   adds gating, not new error-reporting machinery.
3. WHEN this feature is documented THEN it SHALL state explicitly that
   Kconfig and vcpkg are two independent layers — vcpkg controls what is
   *built and available* on disk (`vcpkg install`), Kconfig controls what is
   *searched for and linked into* the `network_simulator` target — and that
   this spec does not add any mechanism for Kconfig to invoke vcpkg.

### Requirement 8

**User Story:** As a new contributor, I want the Kconfig workflow documented
alongside the existing build instructions, so that I can discover and use it
without reading the CMake internals.

#### Acceptance Criteria

1. WHEN `README.md` is read THEN it SHALL contain a "Build Configuration
   (Kconfig)" section covering: the default zero-config behavior (Requirement
   4.4), how to run `menuconfig`, how to apply a defconfig, and what strict
   mode changes.
2. WHEN `DEPENDENCIES.md` is read THEN it SHALL list `kconfiglib` as an
   optional, Python-based *tooling* dependency (needed only for
   `menuconfig`/`guiconfig`/`savedefconfig`/regenerating autoconf artifacts
   from a changed `.config`), distinct from the required/optional C++
   library dependencies it already documents.
3. WHEN `doc/TODO.md` is read after this feature ships THEN the Kconfig
   integration item SHALL be marked complete with a dated changelog entry,
   consistent with how the `clang-tidy` and `code-coverage` specs recorded
   their completion.
