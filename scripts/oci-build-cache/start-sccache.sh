#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
#
# Start sccache and decide whether this job may use it as a compiler
# launcher (.kiro/specs/oci-build-cache/ Requirements 3.5 and 5.1).
#
# Writes `launcher=sccache` to $GITHUB_OUTPUT when, and only when, a compile
# really does round-trip through sccache. Writes nothing and warns otherwise,
# so the caller's `${{ steps.sccache.outputs.launcher || 'none' }}` falls back
# to no compiler cache and the job still builds. A cache failure must never be
# a red job; that is Requirement 3.5, and this script is where it is enforced.
#
# WHY A PROBE AND NOT AN EXIT STATUS. Neither sccache subcommand can answer
# the question. `--start-server` fails on a healthy server that is already
# listening -- the lakers port's cargo build runs under RUSTC_WRAPPER during
# the vcpkg install, so one usually is -- and reading that as failure cost
# Task 1 an entire run. The guard that replaced it fell back to
# `--show-stats`, which returns 0 whether or not a server is alive. Measured
# on sccache 0.17.0:
#
#   | case                        | --start-server | --show-stats | probe |
#   | --------------------------- | -------------- | ------------ | ----- |
#   | healthy, no server yet      |              0 |            0 |     0 |
#   | healthy, already listening  |              2 |            0 |     0 |
#   | backend unreachable         |              2 |            0 |     2 |
#
# The middle and last rows are indistinguishable to both subcommands, so the
# old guard selected sccache over a dead server and every compile then exited
# 2. Task 9's bad-endpoint run died that way: ninja stopped one second into
# `Build (ThreadSanitizer)` having compiled nothing, which is precisely the
# red build Requirement 3.5 forbids. sccache validates its storage backend
# lazily -- on the first request, with a `.sccache_check` read -- so the only
# check that means anything is a request.
#
# The probe is therefore a real compile. It costs one `cc` invocation on a
# five-byte file, and it is exactly the operation the build is about to
# perform 561 times.
#
# Usage: start-sccache.sh

set -uo pipefail

probe_dir=$(mktemp -d)
trap 'rm -rf "$probe_dir"' EXIT

# Best effort: on a job where the vcpkg install already started a server this
# exits non-zero with "Address in use", which is not a failure. The probe
# below is what decides.
sccache --start-server >/dev/null 2>&1 || true

printf 'int main(void){return 0;}\n' > "${probe_dir}/probe.c"
if sccache cc -c "${probe_dir}/probe.c" -o "${probe_dir}/probe.o" \
        >"${probe_dir}/probe.log" 2>&1; then
    echo "sccache: probe compile succeeded; using sccache as the compiler launcher"
    echo "launcher=sccache" >> "${GITHUB_OUTPUT:-/dev/null}"
    # Make the probe invisible to the statistics this job reports. Without
    # this every recorded figure gains one request and one miss, which would
    # silently break comparison against every measurement already recorded in
    # this spec. It also drops the rust compiles the lakers port makes during
    # a cold vcpkg install, so the reported numbers mean "this job's own
    # build" whether or not the vcpkg tree cache happened to hit -- which the
    # figures quoted in tasks.md have always implicitly assumed.
    sccache --zero-stats >/dev/null 2>&1 || true
else
    echo "::warning::sccache could not serve a compile; building with no compiler cache"
    # First lines only: the diagnosis is in the error and the URI it
    # tried, and the rest is twenty lines of response headers.
    head -8 "${probe_dir}/probe.log" | sed 's/^/  /'
fi
