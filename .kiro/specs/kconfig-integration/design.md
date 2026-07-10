# Design Document

## Overview

This document describes the design for layering Kconfig over Kythira's
existing `find_package`-driven optional-dependency matrix. The integration
follows the same pattern as the `clang-format`/`clang-tidy` specs — a
root-level config artifact, generated glue consumed by `CMakeLists.txt`, and
a handful of CMake custom targets — but the generated glue here spans two
languages: CMake (to gate `find_package` calls) and C++ (to preserve every
existing `#ifdef KYTHIRA_HAS_*` / `#ifdef LAKERS_AVAILABLE` call site
unchanged).

The core design decision is that **Kconfig expresses intent and constraints;
CMake still does detection.** Kconfig cannot know whether `folly` is
installed on a given machine — only `find_package` can determine that. What
Kconfig adds is: a single tree of symbols with declared `depends on`
relationships (so `AWS_ACM_PCA` cannot be selected without `AWS_SDK`), a
named, checked-in way to say "this build wants exactly this feature set,"
and a strict mode that turns "wanted but not found" from a silent skip into
a hard configure failure.

## Architecture

```
Developer (optional — bare `cmake -S . -B build` needs none of this)
──────────────────────────────────────────────────────────────────────
  python3 scripts/kconfig/menuconfig.py        interactive, writes build/.config
      or
  cp configs/ci_full_defconfig build/.config   non-interactive

Configure
─────────
cmake -S . -B build [-DKYTHIRA_KCONFIG=<path>] [-DKYTHIRA_KCONFIG_STRICT=ON]
    │
    ├── include(cmake/Kconfig.cmake)                         [first line of CMakeLists.txt
    │       │                                                  after project()]
    │       ├── locate python3 + kconfiglib
    │       │     found?      → run scripts/kconfig/genconfig.py
    │       │     not found?  → warn once, skip generation;
    │       │                   autoconf.cmake/.hpp fall back to
    │       │                   "no symbols defined" (== today's behavior,
    │       │                   since every find_package call below still
    │       │                   runs QUIET and self-guards on *_FOUND)
    │       │
    │       └── genconfig.py Kconfig <config-file>
    │               --cmake-out  build/generated/autoconf.cmake
    │               --header-out build/generated/kythira/autoconf.hpp
    │
    ├── include(build/generated/autoconf.cmake)   → KCONFIG_<SYMBOL> variables
    │
    ├── find_package(folly QUIET)                 (always — hard-required, no symbol)
    ├── find_package(Boost QUIET COMPONENTS ...)   (always — hard-required, no symbol)
    ├── kythira_find_optional(OPENSSL OpenSSL ...)          ── gated by KCONFIG_OPENSSL
    ├── kythira_find_optional(HTTP_TRANSPORT httplib ...)   ── gated by KCONFIG_HTTP_TRANSPORT
    ├── kythira_find_optional(COAP_TRANSPORT libcoap ...)   ── gated by KCONFIG_COAP_TRANSPORT
    ├── kythira_find_optional(EDHOC lakers ...)             ── gated by KCONFIG_EDHOC
    ├── kythira_find_optional(DNS_DISCOVERY PkgConfig:ldns) ── gated by KCONFIG_DNS_DISCOVERY
    ├── kythira_find_optional(POCO_DISCOVERY Poco::DNSSD)   ── gated by KCONFIG_POCO_DISCOVERY
    ├── kythira_find_optional(AWS_SDK AWSSDK ...)           ── gated by KCONFIG_AWS_SDK
    ├── kythira_find_optional(AWS_ACM_PCA AWSSDK:acm-pca)   ── gated by KCONFIG_AWS_ACM_PCA
    ├── kythira_find_optional(LIBSSH2_TESTS libssh2)        ── gated by KCONFIG_LIBSSH2_TESTS
    └── kythira_find_optional(CHAOS_TESTS libfiu)           ── gated by KCONFIG_CHAOS_TESTS

Build
─────
target_include_directories(network_simulator INTERFACE build/generated)
    → #include "kythira/autoconf.hpp" reachable; every existing
      #ifdef KYTHIRA_HAS_LDNS / #ifdef LAKERS_AVAILABLE / etc. site
      is unchanged and keeps working, whether autoconf.hpp came from
      Kconfig or (kconfiglib absent) is simply an empty generated stub.
```

## Component Design

### 1. Root `Kconfig` file

```kconfig
mainmenu "Kythira Build Configuration"

config COVERAGE
	bool "Source-based coverage instrumentation"
	default n
	help
	  Mirrors the existing ENABLE_COVERAGE CMake option. Requires Clang;
	  forces CMAKE_BUILD_TYPE=Debug. See cmake/Kconfig.cmake for how this
	  symbol continues to set the legacy ENABLE_COVERAGE cache variable
	  so `cmake --build build --target coverage` keeps working unchanged.

menu "Transports"

config OPENSSL
	bool "OpenSSL (TLS/HTTPS support)"
	default y
	help
	  find_package(OpenSSL). Backs KYTHIRA_HAS_OPENSSL and, combined with
	  HTTP_TRANSPORT, CPPHTTPLIB_OPENSSL_SUPPORT. vcpkg package: openssl.

config HTTP_TRANSPORT
	bool "HTTP transport (cpp-httplib)"
	default y
	help
	  find_package(httplib). vcpkg package: cpp-httplib.

config HTTP_TRANSPORT_TLS
	bool "HTTPS support for HTTP transport"
	depends on HTTP_TRANSPORT && OPENSSL
	default y
	help
	  Defines CPPHTTPLIB_OPENSSL_SUPPORT, enabling httplib::SSLServer/
	  SSLClient and ca_service --serve's --tls-cert/--tls-key mode.

config COAP_TRANSPORT
	bool "CoAP transport (libcoap, DTLS)"
	default y
	help
	  find_package(libcoap CONFIG) or PkgConfig fallback (libcoap-3).
	  Backs LIBCOAP_AVAILABLE. vcpkg package: libcoap (feature: dtls).

config EDHOC
	bool "EDHOC-to-OSCORE credential bootstrap (lakers)"
	depends on COAP_TRANSPORT
	default n
	help
	  Backs LAKERS_AVAILABLE, enabling oscore_bootstrap::edhoc (see
	  coap-transport-security spec, Requirement 5). Requires a Rust
	  toolchain to build the lakers vcpkg overlay port from source — see
	  vcpkg-overlays/lakers/README.md. vcpkg feature: edhoc. Defaults to
	  n because the Rust toolchain is not assumed to be present; strict
	  mode will fail configure if you select y without it.

endmenu # Transports

menu "Peer Discovery"

config DNS_DISCOVERY
	bool "RFC 1035/2136 DNS peer discovery (libldns)"
	default y
	help
	  PkgConfig module `ldns`. Backs KYTHIRA_HAS_LDNS.

config POCO_DISCOVERY
	bool "Poco DNS-SD peer discovery"
	default y
	help
	  Poco::DNSSD, either from the manually-built vcpkg_installed tree
	  (with avahi-client/avahi-common) or system CMake config/pkg-config.
	  Backs KYTHIRA_HAS_POCO_DNSSD.

endmenu # Peer Discovery

menu "AWS Integration"

config AWS_SDK
	bool "AWS SDK (EC2/ASG/IAM/S3/STS quorum managers)"
	default y
	help
	  find_package(AWSSDK COMPONENTS core ec2 autoscaling iam s3 sts).
	  Backs KYTHIRA_HAS_AWS_SDK.

config AWS_ACM_PCA
	bool "AWS ACM Private CA provider"
	depends on AWS_SDK
	default y
	help
	  find_package(AWSSDK COMPONENTS acm-pca) — independently found even
	  though it depends on AWS_SDK, because AWSSDKConfig.cmake only
	  defines the acm-pca imported target on a component-scoped call.
	  Backs KYTHIRA_HAS_AWS_ACM_PCA.

endmenu # AWS Integration

menu "Test-Only Dependencies"

config LIBSSH2_TESTS
	bool "libssh2 (real-EC2 integration tests)"
	default y
	help
	  find_package(libssh2), CMake config preferred over pkg-config.

config CHAOS_TESTS
	bool "libfiu fault injection / chaos tests"
	default y
	help
	  PkgConfig module libfiu. Backs CHAOS_TESTS_ENABLED and FIU_ENABLE.

endmenu # Test-Only Dependencies

choice
	prompt "Default kythira::Future backend"
	default DEFAULT_FUTURE_BACKEND_FOLLY
	help
	  Selects the KYTHIRA_DEFAULT_FUTURE_BACKEND alias target introduced
	  by the stdexec-future-backend spec (Requirement 11.3). Only affects
	  non-templated call sites using kythira::future_default; templated
	  core code accepts either backend regardless of this choice.

config DEFAULT_FUTURE_BACKEND_FOLLY
	bool "folly"

config DEFAULT_FUTURE_BACKEND_STDEXEC
	bool "stdexec"
	depends on STDEXEC_BACKEND

endchoice

config STDEXEC_BACKEND
	bool "stdexec-backed Future/Promise/Executor backend"
	default n
	help
	  Backs the second Future backend from the stdexec-future-backend
	  spec. vcpkg package: stdexec.

endmenu # (mainmenu close is implicit at EOF)
```

Every symbol's `help` text names the CMake package name and, where
applicable, the vcpkg package/feature — this is the spec's answer to
Requirement 1.5 and Requirement 7.1.

### 2. `scripts/kconfig/genconfig.py`

A thin wrapper around Kconfiglib's public API — not a fork or vendor of
Kconfiglib itself (installed via `pip install kconfiglib`, pinned in
`scripts/kconfig/requirements.txt`):

```python
#!/usr/bin/env python3
import argparse, kconfiglib

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kconfig_file")
    ap.add_argument("config_file", nargs="?")
    ap.add_argument("--cmake-out", required=True)
    ap.add_argument("--header-out", required=True)
    args = ap.parse_args()

    kconf = kconfiglib.Kconfig(args.kconfig_file)
    if args.config_file:
        kconf.load_config(args.config_file)   # falls back to declared
                                               # defaults for anything absent

    # CMake side: one KCONFIG_<NAME> per bool/tristate symbol.
    with open(args.cmake_out, "w") as f:
        for sym in kconf.unique_defined_syms:
            if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
                f.write(f"set(KCONFIG_{sym.name} "
                        f"{'ON' if sym.tri_value else 'OFF'})\n")

    # C++ side: preserve the *existing* macro names, not CONFIG_<NAME>.
    MACRO_MAP = {
        "OPENSSL": "KYTHIRA_HAS_OPENSSL",
        "HTTP_TRANSPORT_TLS": "CPPHTTPLIB_OPENSSL_SUPPORT",
        "COAP_TRANSPORT": "LIBCOAP_AVAILABLE",
        "EDHOC": "LAKERS_AVAILABLE",
        "DNS_DISCOVERY": "KYTHIRA_HAS_LDNS",
        "POCO_DISCOVERY": "KYTHIRA_HAS_POCO_DNSSD",
        "AWS_SDK": "KYTHIRA_HAS_AWS_SDK",
        "AWS_ACM_PCA": "KYTHIRA_HAS_AWS_ACM_PCA",
        "CHAOS_TESTS": "FIU_ENABLE",
    }
    with open(args.header_out, "w") as f:
        f.write("#pragma once\n// Generated by scripts/kconfig/genconfig.py — do not edit.\n")
        for sym_name, macro in MACRO_MAP.items():
            sym = kconf.syms.get(sym_name)
            if sym and sym.tri_value:
                f.write(f"#define {macro} 1\n")

if __name__ == "__main__":
    main()
```

Symbols with no `MACRO_MAP` entry (`COVERAGE`, `HTTP_TRANSPORT`,
`LIBSSH2_TESTS`, the future-backend choice) only ever needed to reach CMake,
not C++, so they're covered by the `KCONFIG_*` loop alone — `COVERAGE`
continues to be consumed as `ENABLE_COVERAGE` via a one-line alias in
`cmake/Kconfig.cmake` (`set(ENABLE_COVERAGE ${KCONFIG_COVERAGE})`) so the
existing coverage-target machinery in `CMakeLists.txt` needs no changes.

### 3. `cmake/Kconfig.cmake`

```cmake
set(KYTHIRA_KCONFIG "" CACHE FILEPATH
    "Path to a Kconfig .config/defconfig. Empty = build/.config if present, else Kconfig defaults.")
set(KYTHIRA_KCONFIG_STRICT OFF CACHE BOOL
    "If ON, a CONFIG symbol set to y makes its find_package() REQUIRED instead of QUIET-and-degrade.")

find_package(Python3 QUIET COMPONENTS Interpreter)
set(_KCONFIG_OK FALSE)
if(Python3_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import kconfiglib"
        RESULT_VARIABLE _KCONFIGLIB_MISSING
        OUTPUT_QUIET ERROR_QUIET)
    if(_KCONFIGLIB_MISSING EQUAL 0)
        set(_KCONFIG_OK TRUE)
    endif()
endif()

set(_CFG "${KYTHIRA_KCONFIG}")
if(NOT _CFG AND EXISTS "${CMAKE_BINARY_DIR}/.config")
    set(_CFG "${CMAKE_BINARY_DIR}/.config")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated/kythira")
set(_CMAKE_OUT "${CMAKE_BINARY_DIR}/generated/autoconf.cmake")
set(_HEADER_OUT "${CMAKE_BINARY_DIR}/generated/kythira/autoconf.hpp")

if(_KCONFIG_OK)
    execute_process(
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_SOURCE_DIR}/scripts/kconfig/genconfig.py"
                "${CMAKE_SOURCE_DIR}/Kconfig" ${_CFG}
                --cmake-out "${_CMAKE_OUT}" --header-out "${_HEADER_OUT}"
        RESULT_VARIABLE _GENCONFIG_FAILED)
    if(_GENCONFIG_FAILED)
        message(FATAL_ERROR "Kconfig: genconfig.py failed — see output above")
    endif()
else()
    message(STATUS "Kconfig: python3/kconfiglib not found — "
                    "menuconfig/defconfig unavailable, all find_package() "
                    "calls fall back to unconditional QUIET probing "
                    "(equivalent to every optional symbol defaulting to y, "
                    "autodetect mode)")
    file(WRITE "${_CMAKE_OUT}" "")   # no KCONFIG_* vars defined
    file(WRITE "${_HEADER_OUT}" "#pragma once\n")
endif()

include("${_CMAKE_OUT}")

# kythira_find_optional(<SYMBOL> <find_package-style call>)
# Wraps one optional dependency: skipped entirely if KCONFIG_<SYMBOL> is
# explicitly OFF; QUIET-and-degrade unless KYTHIRA_KCONFIG_STRICT is ON,
# in which case not-found is a hard configure error (Requirement 4).
macro(kythira_find_optional SYMBOL)
    if(NOT DEFINED KCONFIG_${SYMBOL} OR KCONFIG_${SYMBOL})
        if(KYTHIRA_KCONFIG_STRICT AND DEFINED KCONFIG_${SYMBOL})
            set(_req REQUIRED)
        else()
            set(_req QUIET)
        endif()
        find_package(${ARGN} ${_req})
    endif()
endmacro()
```

`kythira_find_optional` is a thin macro around the existing `find_package`
call shape, not a replacement for it — `AWS_ACM_PCA`'s two-step
`unset(AWSSDK_FOUND)` re-`find_package` dance and the Poco DNSSD
manually-built-libs fallback keep their existing hand-written logic; they
just get wrapped in `if(NOT DEFINED KCONFIG_AWS_ACM_PCA OR
KCONFIG_AWS_ACM_PCA)` instead of running unconditionally.

### 4. CMake custom targets

| Target | Behavior |
|---|---|
| `menuconfig` | `${Python3_EXECUTABLE} -m kconfiglib.menuconfig Kconfig`, `KCONFIG_CONFIG=build/.config` in the environment |
| `guiconfig` | Same, via `kconfiglib.guiconfig` (Tk) |
| `savedefconfig` | `${Python3_EXECUTABLE} -m kconfiglib.savedefconfig --out build/defconfig`, printed with a reminder to `cp build/defconfig configs/<name>_defconfig` |
| `kconfig-check` | Loads every `configs/*_defconfig` with Kconfiglib against the current `Kconfig`, non-zero exit on any parse warning (Requirement 5.4) — wired into CI, not into the normal build |

All four follow the existing pattern used for `format`/`format-check` and
`static-analysis`/`static-analysis-fix`: a stub target that prints an
install hint and fails when the underlying tool (`python3`/`kconfiglib`) is
absent, so `cmake --build build --target help` always lists them.

### 5. Symbol → macro → CMake variable migration table

| Existing gate | Kconfig symbol | Generated CMake var | Generated C++ macro |
|---|---|---|---|
| `option(ENABLE_COVERAGE ...)` | `CONFIG_COVERAGE` | `KCONFIG_COVERAGE` → aliased to `ENABLE_COVERAGE` | *(none — CMake-only)* |
| `find_package(OpenSSL QUIET)` | `CONFIG_OPENSSL` | `KCONFIG_OPENSSL` | `KYTHIRA_HAS_OPENSSL` |
| `find_package(httplib QUIET)` | `CONFIG_HTTP_TRANSPORT` | `KCONFIG_HTTP_TRANSPORT` | *(none — presence implied by target existing)* |
| `httplib_FOUND AND OpenSSL_FOUND` | `CONFIG_HTTP_TRANSPORT_TLS` | `KCONFIG_HTTP_TRANSPORT_TLS` | `CPPHTTPLIB_OPENSSL_SUPPORT` |
| `find_package(libcoap CONFIG)` + PkgConfig fallback | `CONFIG_COAP_TRANSPORT` | `KCONFIG_COAP_TRANSPORT` | `LIBCOAP_AVAILABLE` |
| `find_package(lakers CONFIG QUIET)` | `CONFIG_EDHOC` | `KCONFIG_EDHOC` | `LAKERS_AVAILABLE` |
| `pkg_check_modules(LIBLDNS QUIET ldns)` | `CONFIG_DNS_DISCOVERY` | `KCONFIG_DNS_DISCOVERY` | `KYTHIRA_HAS_LDNS` |
| Poco DNSSD detection block | `CONFIG_POCO_DISCOVERY` | `KCONFIG_POCO_DISCOVERY` | `KYTHIRA_HAS_POCO_DNSSD` |
| `find_package(AWSSDK COMPONENTS core ec2 ...)` | `CONFIG_AWS_SDK` | `KCONFIG_AWS_SDK` | `KYTHIRA_HAS_AWS_SDK` |
| `find_package(AWSSDK COMPONENTS acm-pca)` | `CONFIG_AWS_ACM_PCA` | `KCONFIG_AWS_ACM_PCA` | `KYTHIRA_HAS_AWS_ACM_PCA` |
| `find_package(libssh2 QUIET)` | `CONFIG_LIBSSH2_TESTS` | `KCONFIG_LIBSSH2_TESTS` | *(none — test targets check `LIBSSH2_FOUND` directly)* |
| `pkg_check_modules(FIU QUIET libfiu)` | `CONFIG_CHAOS_TESTS` | `KCONFIG_CHAOS_TESTS` | `FIU_ENABLE` (+ `CHAOS_TESTS_ENABLED` CMake var) |
| *(new, from stdexec-future-backend spec)* | `CONFIG_STDEXEC_BACKEND` / `choice DEFAULT_FUTURE_BACKEND_*` | `KCONFIG_STDEXEC_BACKEND` / `KCONFIG_DEFAULT_FUTURE_BACKEND_STDEXEC` | *(none — feeds `KYTHIRA_DEFAULT_FUTURE_BACKEND`)* |

## Implementation Notes

- **`kconfiglib` is tooling, not a build requirement.** As Requirement 4.4
  and the `cmake/Kconfig.cmake` fallback branch establish, a machine with
  neither Python 3 nor `kconfiglib` builds exactly as it does today —
  `autoconf.cmake` is empty, every `kythira_find_optional` call behaves as
  an unconditional `QUIET` probe. This is deliberate: it means adopting
  Kconfig cannot regress the zero-setup `cmake -S . -B build` path that
  `README.md`'s Quick Start currently documents.
- **Strict mode is a CMake cache variable, not a Kconfig symbol**, because it
  changes how CMake *reacts* to symbol values rather than being a feature
  itself; modeling it inside `Kconfig` would let a checked-in defconfig
  silently flip it, which would surprise a developer who applies
  `configs/ci_full_defconfig` locally expecting the same lenient behavior CI
  gets by passing `-DKYTHIRA_KCONFIG_STRICT=ON` on the command line.
- **`AWS_ACM_PCA`'s existing `unset(AWSSDK_FOUND)` re-`find_package` trick is
  preserved verbatim** inside its `kythira_find_optional` block; Kconfig only
  adds the outer `depends on AWS_SDK` gate and the strict/quiet toggle, it
  does not re-implement AWS SDK component resolution.
- **Container runtime detection is explicitly out of scope** (Requirement 6):
  `tests/docker_chaos/CMakeLists.txt`'s `CONTAINER_RUNTIME`/
  `COMPOSE_COMMAND` cache variables and their env-var overrides stay exactly
  as they are. A defconfig is a portable, checked-in artifact; baking a
  specific container runtime into one would violate the same
  host-portability principle that already rules out static IPs in compose
  files per `CLAUDE.md`.
- **Generated files are gitignored.** `build/generated/`, `build/.config`,
  and `build/defconfig` are build artifacts; only `Kconfig` itself and the
  named files under `configs/` are checked in.
- **macro-name inconsistency is preserved on purpose.** `LAKERS_AVAILABLE`
  breaks the `KYTHIRA_HAS_*` naming convention every other macro follows;
  this spec does not rename it, since doing so would require touching every
  existing `#ifdef LAKERS_AVAILABLE` call site for a purely cosmetic reason
  unrelated to Kconfig integration.
