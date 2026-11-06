# Design Document — clang-tidy Result Cache and Shard Balancing

## Overview

Two independent changes to the `static-analysis` path, each switchable on its
own:

- **Component 1, the shard planner** (`scripts/tidy-shard.py`): replaces the
  round-robin `foreach` in `CMakeLists.txt` with a cost-weighted assignment
  read from `configs/tidy-file-costs.txt`. It changes which files a shard
  gets, never how they are analysed.
- **Component 2, the cache driver** (`scripts/tidy-cache.py`): replaces
  `run-clang-tidy` as the command `static-analysis` runs when
  `KYTHIRA_TIDY_CACHE` is not `off`. For each TU it computes a key, asks the
  backend, analyses the misses in a thread pool, and stores clean results.

A measurement (Component 0) comes first and can stop Component 2. Component 1
can land without it.

```
configure                                 static-analysis target
─────────                                 ──────────────────────
glob *.cpp ─▶ tidy-shard.py ─▶ tidy-sources.txt ─▶ KYTHIRA_TIDY_CACHE?
               ▲                                    │
   configs/tidy-file-costs.txt          off ────────┼──▶ run-clang-tidy (today)
                                                    │
                                   read/readwrite/local
                                                    ▼
                                       tidy-cache.py run
                                 ┌──────────┴───────────┐
                         per TU: key ─▶ HEAD clang-tidy/<k>
                                 │ hit            │ miss
                                 ▼                ▼
                          "cached: x.cpp"   clang-tidy x.cpp
                                              │ clean & readwrite
                                              ▼
                                        PUT clang-tidy/<k>
                                 └──────────┬───────────┘
                                            ▼
                     tidy-cache-stats.json + $GITHUB_STEP_SUMMARY
                     + timings for tidy-file-costs.txt
```

## Component 0: Measurement (task 1)

A throwaway branch with a draft PR and a throwaway workflow
(`tidy-cache-measure.yml`, dispatch only), closed once the numbers are in,
following the ccache, OCI build-cache and mold precedent. It uses Component
2's driver with the `local:` backend on a directory restored and saved with
`actions/cache` between steps of the one job, so it needs no bucket changes.

For each of the last 20 `main` commits, oldest first, on one 4-vCPU runner:

1. Check out the commit, configure with the CI clang-tidy flags and
   `KYTHIRA_TIDY_SHARD_COUNT=1`, build the tidy prerequisites.
2. Run the driver with the cache on. Record hits, misses, key seconds,
   analysis seconds.
3. For every hit, run clang-tidy fresh on that TU and compare (Requirement
   1.3). A disagreement is recorded with the TU, the commit and both outputs.
4. For the first and last commit only, run uncached over the whole tree and
   record per-TU file cost (the first gives Requirement 8's costs; the pair
   bounds drift).

Twenty full uncached runs would cost 20 x 2.5 hours, so steps 2-3 only
re-analyse hits, and the uncached baseline per commit is estimated as the
sum of file costs from step 4. The job will exceed GitHub's 6-hour job limit
if done serially; it is split into 4 jobs of 5 commits each, each job
seeding its cache from the previous one's saved directory.

## Component 1: Shard planner

`scripts/tidy-shard.py --count N --index I --costs configs/tidy-file-costs.txt
--out build/tidy-sources.txt <files...>`:

1. Read costs; any TU absent from the file gets the median of the listed
   costs (or 1.0 if the file is empty).
2. Sort TUs by `(-cost, path)`. Keep N running totals. Give each TU to the
   shard with the lowest total, ties to the lowest index.
3. Write shard I's files, sorted by path, one per line, to `--out`, and print
   the predicted seconds of every shard.

`CMakeLists.txt` calls it through `execute_process` at configure, reads the
written list back into `TIDY_SOURCES`, and keeps the existing range check on
`KYTHIRA_TIDY_SHARD_INDEX`. `configs/tidy-file-costs.txt` is added to
`CMAKE_CONFIGURE_DEPENDS` so a refreshed file reshuffles shards on the next
build. If Python is missing, configure falls back to today's round robin
with a warning; CI asserts the planner ran (the configure log line "tidy
shard plan: predicted max/min ..." is grepped by
`scripts/check-build-config.py`).

The costs file format, `<seconds> <path>` with a `#` comment header carrying
the copyright notice and the run it was measured from, keeps it diffable and
reviewable. `scripts/tidy-shard.py --refresh <stats.json>...` merges timing
output from shards into a new file, replacing costs only for TUs that were
analysed (not cache hits).

Why longest-processing-time-first: it is deterministic, within 4/3 of
optimal in the worst case and much closer for 772 items with this spread, and
needs no solver.

## Component 2: Cache driver

### Invocation

```
scripts/tidy-cache.py run -p build --jobs N \
    --mode read|readwrite|local:<dir> \
    --clang-tidy clang-tidy-18 --compiler clang++-18 \
    --costs-out build/tidy-cache-stats.json  <files...>
```

`static-analysis` runs this instead of `run-clang-tidy` when
`KYTHIRA_TIDY_CACHE` is not `off`. One driver process with a thread pool,
rather than a per-TU wrapper under `run-clang-tidy -clang-tidy-binary`, so
file hashes are memoised in memory, HTTP connections are reused, and the
counters live in one place.

### Key computation (Requirement 2)

Per TU, in this order, each component length-prefixed into one SHA-256:

| Part | Source | Memoised per |
|---|---|---|
| `"tidy-cache-v1"` | constant | — |
| `clang-tidy --version` + SHA-256 of the binary | run once | run |
| `clang-tidy --dump-config <tu>` | run once per directory | directory |
| normalised compile command | `compile_commands.json` | TU |
| sorted `(relpath, sha256)` of every dependency | `-M` scan + file reads | file |

Normalisation replaces `${workspace}` with `@SRC@` and `${build}` with
`@BUILD@`, drops the launcher (`sccache`) and `-o`, `-MD`, `-MF x`, `-MT x`,
`-MQ x`, and keeps everything else in order.

The dependency scan runs the compile command with `-M -MF -` (stdout) in
place of `-c -o`. For CMake's clang PCH flags
(`-Xclang -include-pch -Xclang <x.pch> -Xclang -include -Xclang <cmake_pch.hxx>`)
the scan drops the `-include-pch` pair and turns the remaining
`-Xclang -include -Xclang <hxx>` into `-include <hxx>`, so the PCH's headers
appear in the dependency list by their own content. The scan is a
preprocessor run, roughly 5-15% of a clang-tidy run's cost per TU; task 1
measures the real figure.

Dependency paths are made relative to `@SRC@`, `@BUILD@` or kept absolute
for system headers. Hashes of files are cached by `(path, size, mtime_ns)`
for the life of the driver.

### Backend

- `read` / `readwrite`: the OCI S3-compatible endpoint, from the
  `AWS_ENDPOINT_URL`, `AWS_REGION`, `AWS_ACCESS_KEY_ID`,
  `AWS_SECRET_ACCESS_KEY` and `SCCACHE_BUCKET` variables the build-cache
  action already exports. Requests are signed with SigV4 using the Python
  standard library (`hmac`, `hashlib`, `http.client`), so there is no new
  dependency. Lookup is `HEAD clang-tidy/<key>`; store is
  `PUT clang-tidy/<key>` with a small JSON body.
- `local:<dir>`: a file `<dir>/<key[0:2]>/<key>`, written via rename for
  atomicity.

The entry body, kept under 1 KiB:

```json
{"v":1,"tu":"tests/foo_test.cpp","tidy":"18.1.3","commit":"<sha>","at":"<utc>"}
```

The body is never read back as output; it exists for auditing.

### Analysis and storing (Requirement 3)

A miss runs `clang-tidy -p build --quiet <tu>` with the same flags
`run-clang-tidy` passes today, captures stdout/stderr, and prints them under
a `== <tu>` header as `run-clang-tidy` does. Clean means exit 0 and no line
matching clang's diagnostic format (`<file>:<line>:<col>: (warning|error):`).
Only clean TUs in `readwrite` or `local:` mode are stored. The driver exits
non-zero if any TU was not clean, so `static-analysis` fails as today.

### Failure handling (Requirement 5)

Each request: 10 s timeout, 2 retries with 1 s / 3 s backoff. A failed
lookup is a miss; a failed store is a warning. A shared counter of
consecutive backend errors disables the backend for the rest of the run at
20 and says so once.

### Counters (Requirement 6)

`tidy-cache-stats.json` holds totals plus a per-TU list of
`{tu, hit, seconds}`. The driver appends a Markdown table to
`$GITHUB_STEP_SUMMARY` when set. With the cache requested and zero successful
lookups, it exits 2 with "tidy cache requested but made no successful
lookups (silent no-op)".

## CI wiring

In the `static-analysis` job of `ci.yml`:

- Configure adds `-DKYTHIRA_TIDY_CACHE=<mode>`, where mode is `off` if
  `vars.TIDY_CACHE_DISABLED == 'true'` or the build-cache action reports
  `enabled != 'true'`, else `readwrite` on `refs/heads/main` and `read`
  otherwise.
- A step after `static-analysis`, `if: always()`, uploads
  `build/tidy-cache-stats.json`.
- `scripts/check-build-config.py` gains `--expect KYTHIRA_TIDY_CACHE=<mode>`.

A new scheduled workflow, `tidy-audit.yml` (weekly, Sunday 06:00 UTC, and
`workflow_dispatch`), reuses the same job definition with the cache forced
`off` on `main`'s head, uploads the timing stats, and runs
`tidy-shard.py --refresh` to publish a refreshed costs file as an artifact.
If it fails, its summary looks up the most recent `ci.yml` run on the same
SHA and, if that passed, names the result a false clean (Requirement 7.4).

## Storage estimate (Requirement 9.3)

One entry per clean TU per distinct key. A cold cache writes 772 entries.
Afterwards, a `main` push writes one entry per TU whose key changed: a
`.cpp`-only change writes a few, a widely included header change writes up
to all 772. At the October-November rate of 10-15 `main` pushes a day, a
30-day lifecycle bounds the prefix at roughly 772 x 15 x 30 = 350,000
objects in the very worst case, under 350 MB, and far less in practice. At
OCI's $0.0255/GB-month, that is under one cent a month.

## Alternatives considered

- **ctcache** (`clang-tidy-cache`, an existing open-source wrapper): keys on
  preprocessed output by default. Unless comments are kept, that drops
  them, so a removed `NOLINT` could keep serving a stale clean (Requirement
  2.2); task 1 would have to prove its configuration safe either way. Its
  remote backends also need its own server or extra Python packages. Writing a ~400-line driver whose key this
  project controls is cheaper than patching it.
- **Diff-scoped analysis** (only TUs touched by the PR's diff): misses TUs
  that include a changed header, and `ci.yml` already rejected it for that
  reason. The cache gets the same speed-up without that hole.
- **A planner job that computes keys once and fans out only the misses**:
  fewer runner slots on a mostly-warm run, but adds a job, a dynamic matrix
  and an artifact hand-off. Worth revisiting if the six shards' fixed setup
  (about 3-5 minutes each) becomes the floor.
- **Fewer, larger self-hosted shards**: see the compute-sizing analysis; it
  costs money and the cache removes most of the work anyway.

## Risks

- **False clean from an input the key misses.** Mitigated by hashing raw
  bytes of every dependency, the unit tests in Requirement 2.7, task 1's
  fresh re-check of every hit, and the weekly uncached audit.
- **Dependency-scan cost.** If the `-M` scan is a large fraction of a tidy
  run on the heaviest TUs, warm-run savings shrink. Task 1 measures it.
- **Runner-specific absolute paths in system headers.** Ubuntu runner images
  update clang and libstdc++ packages; those headers are content-hashed, so
  an image update causes misses, not false cleans.
