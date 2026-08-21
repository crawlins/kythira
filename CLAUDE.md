# Kythira — steering directives for Claude Code

## Commit messages

All commit messages MUST follow the [Conventional Commits](https://www.conventionalcommits.org/) standard.

Format: `<type>(<optional scope>): <description>`

Permitted types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`, `revert`.

Rules:
- Description is lowercase, imperative mood, no trailing period.
- Body (if present) wraps at 72 characters.
- Breaking changes go in the footer as `BREAKING CHANGE: <description>`, or by appending `!` after the type/scope.
- **NEVER add a `Co-Authored-By:` trailer** to any commit message. No exceptions.

The body MUST be a detailed summary of the changes. Specifically:
- Explain **why** the change was made, not just what files were touched.
- Call out every non-obvious decision or trade-off.
- List each logical sub-change when a commit covers more than one concern.
- Include the root cause for bug fixes and the symptom that exposed it.
- A one-line body is only acceptable when the subject line is genuinely
  self-contained (e.g. a pure rename with no behavioural effect).

## Working branch commits

When committing work to a working/feature branch, reset to `origin/main`
and rebuild the commit(s) instead of layering incremental commits on top
of each other:

1. `git fetch origin main`, then check the branch's merge-base against
   `origin/main`. If `origin/main` has moved ahead of that merge-base,
   rebase the branch onto it first — never discard upstream commits.
2. `git reset --soft origin/main` to collapse the branch's own commits back
   into a single staged diff (safe only once step 1 confirms the branch's
   base already matches `origin/main`).
3. Recommit from that staged diff with fresh commit message(s) written from
   the accumulated changes and the actual work done — treat prior commit
   messages as scratch notes, not the final record. Follow the Commit
   messages rules above, including splitting into multiple commits when the
   diff covers more than one logical concern.
4. If the branch was already pushed (e.g. it backs an open PR), push with
   `git push --force-with-lease` to update it in place.

This keeps each working branch's history clean and reviewable — one (or a
few, logically-separated) well-described commits rather than an
accumulation of "wip" / "fix typo" / "address feedback" commits.

## Copyright headers

Every **new** file that can carry a comment MUST open with:

```
Copyright (c) <year> Clark Rawlins
SPDX-License-Identifier: Apache-2.0
```

followed by a blank line, using that file type's native comment syntax:

- `//` — C++, Rust, protobuf, BIND `named.conf`
- `#` — Python, shell, CMake/`CMakeLists.txt`, YAML, Dockerfiles, `Kconfig`
  and `*_defconfig`, systemd units, HCL, TOML, `.properties`, `.env.example`,
  Doxyfiles, and the `.gitignore` / `.dockerignore` / `.clang-format` /
  `.clang-tidy` dotfiles
- `;` — DNS zone files

Placement rules that override "put it at the top":

- On shell and Python files the header goes **after** the `#!` line, never
  before it. A header at line 1 displaces the shebang and silently turns the
  script into a non-executable file. Python encoding cookies (`# -*- coding:
  ... -*-`) are only honoured on the first two lines, so step over those too.
- On C++ headers the block goes **above** `#pragma once`.

Use the current year for genuinely new files. Do not bump the year on files
you merely edit — the notice records creation, and churning it adds diff
noise for no legal benefit.

### Exempt

Do not add a notice to:

- **Formats with no comment syntax** — JSON above all.
- **Third-party or vendored content** — `vcpkg-overlays/*` portfiles and
  patches, bundled upstream licenses, `doc/raft.txt` (the Ongaro & Ousterhout
  Raft paper). Claiming copyright over someone else's work is a false claim.
  Note `vcpkg-overlays/lakers/ffi/` is first-party Kythira code despite its
  location, and IS stamped.
- **Generated files** — anything rebuilt by the build, e.g.
  `generated/kythira/autoconf.hpp` from `genconfig.py`. The header would be
  discarded on the next run; add it to the generator's template instead.
- **Files parsed literally**, where a leading comment breaks them —
  `coverage_floor.txt` is read as a bare float by the coverage ratchet.
- **Binaries**, and Markdown documentation.

When in doubt, prefer adding the notice: the cost of a redundant header is
two lines, the cost of a missing one is an unlicensed file.

## Container runtime compatibility

Any test, compose file, or harness code that runs containers MUST work with both:

- **Docker** (rootful, the default CI runtime)
- **rootless Podman** (Podman ≥ 4.x with aardvark-dns)

### Rules that follow from this

1. **No static IP addresses in compose files.** `ipam.config.ipv4_address` is silently
   ignored by rootless Podman. Use compose service names for inter-container addressing
   and resolve them to IPs at runtime with `getaddrinfo` when the consumer requires a
   literal IP (e.g. ldns).

2. **No hardcoded `docker` in test harness code.** Use `container_runtime()` from
   `tests/docker_chaos/os_faults.hpp`, which reads `$KYTHIRA_CONTAINER_RUNTIME`
   (default `"docker"`). Use `compose_prefix()` for compose sub-commands, which also
   honours `$KYTHIRA_COMPOSE_COMMAND` for standalone `podman-compose`.

3. **CMake targets use detected runtime.** `tests/docker_chaos/CMakeLists.txt`
   auto-detects `docker` then `podman` via `find_program`, exposes
   `CONTAINER_RUNTIME` / `COMPOSE_COMMAND` cache variables for explicit override, and
   forwards both as env vars into every scenario-test invocation.

4. **Features that require root networking are forbidden.** Do not use `--privileged`,
   host networking, or kernel capabilities that rootless Podman cannot grant without
   explicit configuration.

5. **When adding a new compose file** that requires containers to address each other,
   wire them by service name and ensure the consuming binary resolves that name to an
   IP if needed — do not reintroduce static IPs.
