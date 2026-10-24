#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Stops everything the cairn chaos tests start, in one command
# (.kiro/specs/cairn-chaos-testing/ Requirement 7.6, design §5): the engine
# and its workers, the tool server, both proxies and the cluster in
# docker/cairn/cairn-compose.yml, plus the host-side clusters that
# `chaos_ctl replay` and `minimize` start (docker/docker-compose.yml,
# docker/chaos-5-compose.yml) and any chaos_workload a host-side run left
# running. It is also what a run's wall-clock limit fires:
#
#   scripts/cairn/kill.sh               stop now
#   scripts/cairn/kill.sh --after 7200  stop in two hours (run it with &)
#
# Run directories are kept: they are in the cairn stack's `runs` volume or
# on the host, and a run's schedule log is its artifact. Remove the volume
# by hand once a run is triaged.
#
# Uses $KYTHIRA_CONTAINER_RUNTIME (default docker) and
# $KYTHIRA_COMPOSE_COMMAND (default "<runtime> compose"), as
# tests/docker_chaos/os_faults.hpp does, so it works under Docker and
# rootless Podman alike. Exits non-zero if any stack failed to come down.

set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runtime="${KYTHIRA_CONTAINER_RUNTIME:-docker}"
if [ -n "${KYTHIRA_COMPOSE_COMMAND:-}" ]; then
    # shellcheck disable=SC2206  # a command and its words, e.g. "podman-compose"
    compose=(${KYTHIRA_COMPOSE_COMMAND})
else
    compose=("$runtime" compose)
fi

case "${1:-}" in
    "") ;;
    --after)
        [[ "${2:-}" =~ ^[0-9]+$ ]] || { echo "usage: $0 [--after SECONDS]" >&2; exit 2; }
        echo "cairn kill: stopping everything in ${2}s"
        sleep "$2"
        ;;
    -h|--help)
        sed -n '/^# Stops everything/,/^# rootless Podman alike/p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) echo "usage: $0 [--after SECONDS]" >&2; exit 2 ;;
esac

status=0

down() {
    local file="$1"
    shift
    echo "cairn kill: ${compose[*]} -f ${file#"$repo"/} down"
    if ! "${compose[@]}" -f "$file" "$@" down --remove-orphans --timeout 5; then
        echo "cairn kill: ${file#"$repo"/} did not come down" >&2
        status=1
    fi
}

# Every profile, so the engine and the probe come down with the rest.
down "$repo/docker/cairn/cairn-compose.yml" --profile engine --profile probe
down "$repo/docker/docker-compose.yml"
down "$repo/docker/chaos-5-compose.yml"

if pkill -x chaos_workload 2>/dev/null; then
    echo "cairn kill: stopped host chaos_workload processes"
fi

exit "$status"
