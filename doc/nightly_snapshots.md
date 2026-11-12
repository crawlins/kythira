# Nightly snapshot releases

`.github/workflows/nightly.yml` publishes a GitHub **prerelease** of `main`
every night at 07:17 UTC, if `main` has moved since the previous snapshot.

## What you get

Each snapshot is tagged `nightly-YYYYMMDD-<sha7>` and carries:

| Asset | Contents |
| --- | --- |
| `kythira-nightly-…-linux-x86_64.tar.gz` | stripped server binaries, `LICENSE`, `README.md`, `BUILDINFO.txt` |
| `kythira-nightly-…-linux-aarch64.tar.gz` | the same for arm64 |
| `SHA256SUMS` | checksums of both archives |

The binaries are `multi_raft_node`, `ca_service`, `ca_cluster_node`,
`raft_object_backup`, `oci_heartbeat_writer_node`, `redis_gateway_node`,
`iam_node`, `kythira-iam`, `ledger_node`, `ledger_ctl`,
`kinesis_gateway_node` and `dynamodb_gateway_node` (the workflow's
`RELEASE_TARGETS` list). They are built from `configs/nightly_defconfig`
(the union of `ci_full_defconfig` and `ci_ion_defconfig`, so every server CI
builds) with g++-14 in Release mode, linked with mold. vcpkg dependencies are static; the
binaries need only glibc 2.39+ and libstdc++ from GCC 14, i.e. Ubuntu 24.04 or
newer. `BUILDINFO.txt` lists each binary's shared-library needs.

A snapshot is `main`'s head at build time. It is **not** gated on CI being
green and the workflow does not run the test suite; the release notes link the
commit's CI checks so you can see its state before using it.

## Retention

The newest 14 snapshots are kept (`NIGHTLY_KEEP` in the workflow). Each
successful publish deletes older `nightly-*` releases and their tags.
Snapshots never become the repository's "Latest release".

## Running it by hand

Actions → **Nightly snapshot** → *Run workflow* on `main`. Tick **force** to
publish even when `main` has not changed; a forced run on the same day and
commit replaces that day's snapshot. Dispatching from any other branch fails
in the gate job rather than publishing a branch as a `main` snapshot.

A pull request that edits the workflow runs the two build legs (no release is
written), so a change to the build is proven before it merges.

## Caches

Scheduled and dispatched runs are on `main`, so they use the read-write OCI
build-cache key (see `doc/ci_build_cache.md`). The x64 leg uses exactly the
Ion Serializer Build job's vcpkg features, compiler and cache key, so it starts
warm; the arm64 leg has no CI counterpart and is cold on its first night only.
