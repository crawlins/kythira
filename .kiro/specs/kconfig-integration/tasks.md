# Implementation Plan - Kconfig Integration

## Status: Not Started

## Overview

Integrate Kconfig (via Kconfiglib) as a declarative front end over Kythira's
existing `find_package`-driven optional-dependency matrix. Work is divided
into five phases: the root `Kconfig` file, the Kconfiglib-based generator
script, the CMake glue and custom targets, checked-in defconfigs plus CI
validation, and documentation.

---

## Phase 1: Root `Kconfig` File (Tasks 1–3)

- [ ] 1. Write `Kconfig` at the repository root
  - Transcribe every optional dependency listed in Requirement 1.1 into a
    `config` symbol, grouped into `menu`s (`Transports`, `Peer Discovery`,
    `AWS Integration`, `Test-Only Dependencies`) as laid out in the design
    doc
  - Add `depends on` for every prerequisite currently expressed as a
    compound CMake `if()` (`HTTP_TRANSPORT_TLS` needs `HTTP_TRANSPORT` +
    `OPENSSL`; `EDHOC` needs `COAP_TRANSPORT`; `AWS_ACM_PCA` needs
    `AWS_SDK`)
  - Add `CONFIG_COVERAGE` and the `DEFAULT_FUTURE_BACKEND` choice
  - Every symbol gets `help` text naming the CMake package and, where
    applicable, the vcpkg feature
  - _Requirements: 1.1–1.5_

- [ ] 2. Validate the `Kconfig` file with Kconfiglib directly (no CMake yet)
  - `pip install kconfiglib`; `python3 -c "import kconfiglib;
    kconfiglib.Kconfig('Kconfig')"` — must not raise
  - `python3 -m kconfiglib.menuconfig Kconfig` — walk the full menu tree by
    hand, confirm `depends on` correctly greys out `AWS_ACM_PCA` when
    `AWS_SDK` is deselected, and `EDHOC` when `COAP_TRANSPORT` is deselected
  - _Requirements: 1.2_

- [ ] 3. Write `configs/ci_full_defconfig` and `configs/minimal_defconfig`
  - `ci_full_defconfig`: every optional symbol `y`
  - `minimal_defconfig`: every optional symbol `n` (core `folly`/`Boost`
    stay hard-required outside Kconfig, per design)
  - Generate both non-interactively:
    `python3 -m kconfiglib.alldefconfig` / hand-edit, then
    `python3 -m kconfiglib.savedefconfig`
  - _Requirements: 5.1, 5.2_

---

## Phase 2: Generator Script (Tasks 4–6)

- [ ] 4. Write `scripts/kconfig/genconfig.py`
  - Implement per the design doc: `KCONFIG_<NAME>` CMake `set()` lines for
    every bool/tristate symbol, plus the `MACRO_MAP` translation table for
    the subset of symbols that back an existing C++ macro
  - `--cmake-out` / `--header-out` CLI flags; accept an optional positional
    `.config`/defconfig path, falling back to `Kconfig`-declared defaults
    when omitted
  - _Requirements: 3.2, 3.3, 3.4_

- [ ] 5. Pin the Kconfiglib dependency
  - Add `scripts/kconfig/requirements.txt` with a pinned `kconfiglib==`
    version
  - _Requirements: 8.2_

- [ ] 6. Unit-verify genconfig.py against each defconfig from Task 3
  - Run against `configs/ci_full_defconfig`: every `MACRO_MAP` macro present
    in the generated header, every `KCONFIG_*` var `ON` in the generated
    CMake file
  - Run against `configs/minimal_defconfig`: no `MACRO_MAP` macros present,
    every optional `KCONFIG_*` var `OFF`
  - Run with no config file at all (bare `Kconfig` defaults): confirm output
    matches `ci_full_defconfig`'s output (every optional symbol defaults to
    `y` per Requirement 4.4 — only `EDHOC`/`STDEXEC_BACKEND` default to `n`,
    since their vcpkg features are opt-in)
  - _Requirements: 3.1, 4.4_

---

## Phase 3: CMake Glue and Custom Targets (Tasks 7–12)

- [ ] 7. Write `cmake/Kconfig.cmake`
  - `KYTHIRA_KCONFIG` and `KYTHIRA_KCONFIG_STRICT` cache variables
  - `find_package(Python3 COMPONENTS Interpreter)` +
    `import kconfiglib` probe, with the graceful-degradation fallback path
    from the design doc when either is missing
  - `include()` at the very top of the root `CMakeLists.txt`, immediately
    after `project()`, before any existing `find_package` call
  - _Requirements: 3.1, 2.4_

- [ ] 8. Define the `kythira_find_optional()` macro
  - Implement per the design doc: skip when `KCONFIG_<SYMBOL>` is `OFF`,
    `REQUIRED` vs `QUIET` per `KYTHIRA_KCONFIG_STRICT`
  - _Requirements: 4.1, 4.2, 4.3_

- [ ] 9. Rewire the root `CMakeLists.txt`'s optional `find_package` calls
  through `kythira_find_optional()`
  - One at a time, in this order (least to most entangled with existing
    hand-written logic): `OpenSSL`, `httplib`, `libcoap`, `lakers`,
    `libldns` (PkgConfig), `AWSSDK` core, `libssh2`, `libfiu`
  - After each: full local build with the corresponding defconfig symbol
    both `y` and `n`, confirm identical target/macro output to pre-change
    behavior when `y` and full absence when `n`
  - _Requirements: 3.1–3.4_

- [ ] 10. Rewire the Poco DNSSD block and the `AWS_ACM_PCA`
  `unset(AWSSDK_FOUND)` block
  - These keep their existing hand-written detection logic; only wrap the
    outer `if(...)` in the Kconfig gate, per the design doc's note that
    `kythira_find_optional` does not replace bespoke detection
  - _Requirements: 3.1, 1.2_

- [ ] 11. Wire `CONFIG_COVERAGE` to `ENABLE_COVERAGE`
  - `set(ENABLE_COVERAGE ${KCONFIG_COVERAGE})` immediately after the
    `include(build/generated/autoconf.cmake)` line, before the existing
    `option(ENABLE_COVERAGE ...)` — CMake's `option()` leaves an
    already-`set` cache variable untouched, so no coverage-target code
    changes
  - Verify `cmake -S . -B build-coverage -DKYTHIRA_KCONFIG=... ` (symbol
    `y`) and `-DENABLE_COVERAGE=ON` (legacy, no Kconfig involved) produce
    identical `ENABLE_COVERAGE` cache state
  - _Requirements: 1.3_

- [ ] 12. Add `menuconfig`, `guiconfig`, `savedefconfig`, `kconfig-check`
  custom targets
  - Follow the stub-target-when-tool-missing pattern from `format`/
    `static-analysis`
  - `kconfig-check` loads every `configs/*_defconfig` and fails on any
    Kconfiglib parse warning
  - _Requirements: 2.2, 2.3, 2.5, 5.4_

---

## Phase 4: Validation Matrix (Tasks 13–16)

- [ ] 13. Zero-config regression check
  - `rm -rf build && cmake -S . -B build && cmake --build build` with no
    `.config`, no `-DKYTHIRA_KCONFIG`, on a machine both with and without
    `kconfiglib` installed
  - Confirm identical set of enabled features/targets to the pre-Kconfig
    baseline in both cases (Requirement 4.4)
  - _Requirements: 4.4, 2.4_

- [ ] 14. Strict-mode failure check
  - `cmake -S . -B build -DKYTHIRA_KCONFIG=configs/ci_full_defconfig
    -DKYTHIRA_KCONFIG_STRICT=ON` on a machine missing one optional
    dependency (e.g. no `libfiu`) — confirm configure fails with CMake's
    standard `find_package REQUIRED` error naming `libfiu`, not a generic
    Kconfig error
  - _Requirements: 4.2_

- [ ] 15. Minimal-config check
  - `cmake -S . -B build-min -DKYTHIRA_KCONFIG=configs/minimal_defconfig`
    on a machine with every optional dependency installed — confirm none of
    them are linked in despite being available (Requirement 4.3), and that
    `cmd/dns_discovery_node`, `cmd/poco_discovery_node`, `ca_cluster_node`,
    etc. subdirectories are correctly skipped
  - _Requirements: 4.3_

- [ ] 16. `menuconfig` round-trip check
  - `cmake --build build --target menuconfig`, deselect `POCO_DISCOVERY`,
    save, reconfigure — confirm `KYTHIRA_HAS_POCO_DNSSD` disappears from
    `autoconf.hpp` and `cmd/poco_discovery_node` is skipped
  - `cmake --build build --target savedefconfig`, diff against
    `configs/ci_full_defconfig` with the one symbol flipped — confirm the
    defconfig is minimal (only the changed symbol appears)
  - _Requirements: 2.2, 2.5_

---

## Phase 5: Documentation (Tasks 17–19)

- [ ] 17. Add "Build Configuration (Kconfig)" section to `README.md`
  - Cover: default zero-config behavior, `menuconfig`, applying a
    defconfig, `KYTHIRA_KCONFIG_STRICT`
  - _Requirements: 8.1_

- [ ] 18. Update `DEPENDENCIES.md`
  - Add `kconfiglib` under a new "Optional Tooling" subsection, distinct
    from required/optional C++ libraries
  - _Requirements: 8.2_

- [ ] 19. Update `doc/TODO.md`
  - Mark the Kconfig integration item `[x]` with a dated "What Changed"
    entry once Phases 1–4 are complete
  - _Requirements: 8.3_

---

## Summary

| Phase | Tasks | Status |
|-------|-------|--------|
| 1 | 1–3   | Not Started |
| 2 | 4–6   | Not Started |
| 3 | 7–12  | Not Started |
| 4 | 13–16 | Not Started |
| 5 | 17–19 | Not Started |

**Total**: 19 tasks
