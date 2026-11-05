#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Runs M2 (.kiro/specs/cairn-chaos-testing/ task 13): every invariant
# project once, in turn, against a chaos_node built from current main, and
# counts a violation of any checker as a find. It is m1-campaign.sh with
# the project list and find rule changed, so bootstrap restarts, early
# abort, the --deadline cost guard and the per-run logs are the same.
#
# Usage: m2-hunt.sh --out DIR [--hours H] [--deadline EPOCH]
#                   [--projects P1,P2,...] [--keep-host]
#   --projects  default: all six under docker/cairn/projects/, cheapest
#               to check first (election-safety ... liveness)
#
# Environment: as m1-campaign.sh. KYTHIRA_CHAOS_NODE_IMAGE defaults to
# kythira-chaos-node:dev, which must be built from the commit under test
# with KYTHIRA_COMMIT set so the schedule log names it.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KYTHIRA_CHAOS_NODE_IMAGE="${KYTHIRA_CHAOS_NODE_IMAGE:-kythira-chaos-node:dev}"
all=election-safety,log-matching,leader-completeness,state-machine-safety,linearizability,liveness

args=() projects="$all"
while [ $# -gt 0 ]; do
    case "$1" in
        --projects) projects="$2"; shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
exec "$here/m1-campaign.sh" "${args[@]}" --projects "$projects" --violation any
