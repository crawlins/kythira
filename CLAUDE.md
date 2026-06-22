# Kythira — steering directives for Claude Code

## Commit messages

All commit messages MUST follow the [Conventional Commits](https://www.conventionalcommits.org/) standard.

Format: `<type>(<optional scope>): <description>`

Permitted types: `feat`, `fix`, `docs`, `style`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`, `revert`.

Rules:
- Description is lowercase, imperative mood, no trailing period.
- Body (if present) wraps at 72 characters.
- Breaking changes go in the footer as `BREAKING CHANGE: <description>`, or by appending `!` after the type/scope.

The body MUST be a detailed summary of the changes. Specifically:
- Explain **why** the change was made, not just what files were touched.
- Call out every non-obvious decision or trade-off.
- List each logical sub-change when a commit covers more than one concern.
- Include the root cause for bug fixes and the symptom that exposed it.
- A one-line body is only acceptable when the subject line is genuinely
  self-contained (e.g. a pure rename with no behavioural effect).

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
