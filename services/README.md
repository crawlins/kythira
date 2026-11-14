# Service modules

A service is something built on Kythira rather than part of it: the Redis
gateway and the build cache it serves, the IAM and OIDC issuer, the verifiable
ledger. Each one lives in its own directory under `services/`, behind its own
Kconfig switch, and may use only what any outside consumer of Kythira could
use. A CI check enforces that, so a module stays buildable on its own and a
later move to a separate repository is a mechanical step, not a rewrite.

This file is the pattern every module follows. It comes from
`.kiro/specs/redis-gateway-module/` (Requirements 1-5 and 9.2), and the
ledger (`.kiro/specs/verifiable-ledger/`) and IAM
(`.kiro/specs/iam-compatible-auth/`) modules use it as is.

## Layout

```
services/<m>/
├── README.md               what the module is, the core targets and variables
│                           it uses, and what a split would still need
├── Kconfig                 the module's symbols, `source`d from the top level
├── CMakeLists.txt          entered only when the module's switch is on
├── boundary-allow.txt      core *_impl.hpp includes the module may make, with
│                           reasons (optional; see "Boundary rules")
├── include/kythira/<m>/    library headers
├── cmd/<daemon>/           the daemon: the composition root
├── tests/                  unit and in-process integration tests
├── tests/docker/           container scenarios
└── docker/                 images and compose files
```

Headers are included as `<kythira/<m>/<name>.hpp>`. A file moved in from
`include/raft/` keeps its basename, so `git log --follow` and a search for
the old name still find it. Namespaces stay `kythira::`.

## Kconfig

The top-level `Kconfig` has one line per module:

```
source "services/<m>/Kconfig"
```

Kconfiglib resolves `source` against the source root, which is where
`cmake/Kconfig.cmake` and `scripts/kconfig/genconfig.py` run. A module's
symbols follow the usual rules (`depends on` for hard requirements, `help`
naming what the switch builds). New modules default to `n`. The Redis gateway
stays `y` because it was on before it became a module.

## CMake

The top level enters a module only when its switch is on, after the core
targets, `tests/` and `tests/docker_chaos/`, so every target and helper the
module uses already exists:

```cmake
if(KYTHIRA_BUILD_<M>)
    add_subdirectory(services/<m>)
endif()
```

With the switch off, nothing under `services/<m>/` is configured, and the
core's targets and tests are those of a module-on build minus the module's.

A module reaches core only through exported targets and variables, never
through `${CMAKE_SOURCE_DIR}`:

| From core | What it carries |
|---|---|
| `kythira_core_headers` | `include/` and `generated/` (the Kconfig autoconf header) |
| `kythira_test_support` | the in-process test fabric's include path (`tests/`, `tests/docker_chaos/`); tests only |
| `cmake/kythira_test_helpers.cmake` | `add_raft_completion_test`, `add_docker_chaos_scenario_test`, the image-build helpers, `kythira_scale_test_timeouts()` (call it at the end of a directory that registers tests) and `KYTHIRA_IMAGE_CONTEXT`, the build context and working directory for images and scenario targets |

Paths inside the module use `${CMAKE_CURRENT_SOURCE_DIR}`. A module's
`README.md` lists the core targets and variables it uses, which is the list a
split would have to export from a Kythira package.

The only places core CMake may name a module or its targets are the
`add_subdirectory(services/<m>)` line and deliberate hooks (an install rule,
a target a workflow builds by name). Mark each hook with a trailing
`# module-hook` comment saying what it is for.

## Boundary rules

`scripts/check_module_boundaries.py` checks every `services/*/` directory on
every pull request (`ci.yml`, `kconfig-check` job). It needs no build: it
reads `#include` lines and CMake files. Its rules, by the name it reports:

| Rule | What it forbids |
|---|---|
| `other-module` | including another module's header. Share the piece as a core concept and wire the two together in a composition root instead |
| `impl-header` | including a core `*_impl.hpp`, except from a file listed in `boundary-allow.txt` |
| `core-tests` | including anything under the top-level `tests/`, except the test fabric from the module's own `tests/`, by name, through `kythira_test_support` (never by a relative path) |
| `core-cmd` | including anything under the top-level `cmd/` |
| `core-includes-module` | a core file (`include/`, `src/`, `cmd/`, `tests/`) including a module header |
| `cmake-source-dir` | `${CMAKE_SOURCE_DIR}` or `${PROJECT_SOURCE_DIR}` pointing outside the module |
| `cmake-escape` | `${CMAKE_CURRENT_SOURCE_DIR}/..` resolving outside the module |
| `core-cmake-target` | core CMake naming a module target, other than the hooks above |
| `allow-entry` | a malformed, misplaced or stale `boundary-allow.txt` line |

`boundary-allow.txt` lists the core internals a module may include, one per
line:

```
# file                                   allowed core *_impl.hpp       why
cmd/redis_gateway_node/redis_serving.hpp multi_raft_impl.hpp           instantiates the multi-raft host
```

Only the composition root (`cmd/`) and module tests that stand up an
in-process cluster may be listed, since both instantiate core templates the
way an application would. Library headers under `include/kythira/<m>/` never
may. Every line needs a reason, and a line whose file no longer includes the
header is an error, so the list cannot outlive what it records.

Run it locally with:

```
python3 scripts/check_module_boundaries.py
python3 scripts/tests/test_check_module_boundaries.py   # its fixture tests
```

Cross-module dependencies are not allowed. When one module needs something
from another, the shared piece becomes a core concept (a header under
`include/raft/` with no module types in it), each module depends on core, and
the daemon that needs both wires them together, with a `boundary-allow.txt`
entry for the adapter.

## CI

`ci.yml` builds every module that is on in its defconfig and runs its ctest
on every pull request. That is the gate.

A module may also have its own workflow, `.github/workflows/<m>-module.yml`,
filtered by path to the module, the core headers it depends on and the
workflow itself. It runs the boundary check, a module-off build and ctest
(`configs/ci_no_<m>_defconfig`), the module's tests, and its container
scenarios under both Docker and rootless Podman (see CLAUDE.md, "Container
runtime compatibility").

Such a workflow is **not** a required check: GitHub reports nothing for a
workflow its `paths` filter skips, so a required check from it would hang
every unrelated pull request. Auto-merge fires on required checks only, so
whoever owns a pull request that touches a module must wait for the module
workflow to pass before merging.
