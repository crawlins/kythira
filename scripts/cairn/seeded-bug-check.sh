#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Shows the cairn M1 seeded bug is catchable before any agent is involved
# (.kiro/specs/cairn-chaos-testing/ task 9): a hand-written schedule run
# through chaos_ctl on the seeded image, then `chaos_ctl replay --repeat N`
# of the log it wrote.
#
# The bug (-DKYTHIRA_CHAOS_SEEDED_BUG=vote_term) grants a vote without
# checking whether the term already has one. The schedule makes two
# candidates in one term likely: every node's AppendEntries sends fail, so
# no leader can hold its followers and elections run back to back, and a
# share of RequestVote sends fail, so a candidate's request can reach the
# third node but not its rival, which then campaigns in the same term and
# asks the same voter. A correct voter refuses the second request; the
# seeded one grants it, and raft_safety reports election_safety.
#
# Usage: seeded-bug-check.sh [image] [repeat] [hold-seconds]
#   image   default kythira-chaos-node:seeded-vote-term. Pass
#           kythira-chaos-node:dev for the control, which must find nothing.
#   repeat  replay attempts, default 5.
#   hold    seconds the faults stay armed, default 40.
#
# Needs the image (cmake -DKYTHIRA_CHAOS_SEEDED_BUG=vote_term, then
# --target docker-chaos-image) and a host chaos_ctl: $KYTHIRA_CHAOS_CTL, or
# build/chaos_ctl. Uses $KYTHIRA_CONTAINER_RUNTIME and
# $KYTHIRA_COMPOSE_COMMAND as chaos_ctl does. Prints the replay's JSON last.
# Exits 0 when the outcome matches the image: the seeded image's live run
# and at least one replay show election_safety, and the control shows no
# violation at all.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
image="${1:-kythira-chaos-node:seeded-vote-term}"
repeat="${2:-5}"
hold="${3:-40}"
ctl_bin="${KYTHIRA_CHAOS_CTL:-$repo/build/chaos_ctl}"
run_dir="${KYTHIRA_CHAOS_RUN_DIR:-$(mktemp -d)/seeded-bug-check}"
seeded=0
[ "$image" = "kythira-chaos-node:seeded-vote-term" ] && seeded=1

fail() { echo "FAIL: $*" >&2; exit 1; }
[ -x "$ctl_bin" ] || fail "no chaos_ctl at $ctl_bin; set KYTHIRA_CHAOS_CTL"

# One tool call; prints its last line, the JSON result.
call() { "$ctl_bin" "$@" --run-dir "$run_dir" | tail -n 1; }
on_all() {
    local n
    for n in 1 2 3; do
        call fault.failpoint "{\"node\":$n,$1}" > /dev/null
    done
}

cd "$repo"
call init --layout chaos3 --compose docker/docker-compose.yml --image "$image" --seed 9 \
    --start > /dev/null
echo "run dir: $run_dir"

call workload.start '{"seed":9,"concurrency":2,"rate":20}' > /dev/null
on_all '"name":"raft/network/send_request_vote","mode":"random","probability":0.7'
on_all '"name":"raft/network/send_append_entries","mode":"always"'
sleep "$hold"
on_all '"name":"raft/network/send_append_entries","mode":"off"'
on_all '"name":"raft/network/send_request_vote","mode":"off"'
sleep 3
call workload.stop > /dev/null
live="$(call check.run '{"checks":["raft_safety","linearizability"]}')"
echo "live run: $live"

# Exit status 1 from replay means "reproduced", which is not an error here.
replay="$("$ctl_bin" replay --run-dir "$run_dir" --repeat "$repeat" | tail -n 1)" || true
echo "$replay"

if [ "$seeded" -eq 1 ]; then
    grep -q '"election_safety"' <<< "$live" || fail "the live run did not catch the seeded bug"
    grep -q '"reproduced":0,' <<< "$replay" && fail "no replay caught the seeded bug"
else
    grep -q '"violations":\[\]' <<< "$live" || fail "the control's live run found a violation"
    grep -q '"reproduced":0,' <<< "$replay" || fail "a control replay found a violation"
fi
echo "OK"
