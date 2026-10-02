#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0
"""Summarise KYTHIRA_COAP_SEND_PROBE lines per sweep cell and per group.

The CoAP row of the multi-Raft performance matrix reports time spent waiting
on the libcoap client's `_mutex` as a first-class figure
(`.kiro/specs/multi-raft-performance/` Requirement 17a.3), and takes it from
the probe the client already has rather than from a second instrumentation
scheme (17a.4, and `.kiro/specs/coap-transport-multi-raft/` task 14). With
`KYTHIRA_COAP_SEND_PROBE=1` every send prints one line:

    [stall-probe] send_rpc token=... target=2 lock_wait_ms=0 ... group=17
        path=/raft/append_entries lock_wait_us=12 send_path_us=48

This reads a log of `multi_raft_http_benchmark_test`'s
`write_latency_by_group_count` case, splits it at the case's own cell headers
("  coap, 8 group(s), 2 ms tick:"), and prints, per cell, the distribution of
`lock_wait_us` and `send_path_us` over all sends and then per group.

The client's I/O thread writes to the same stream once a second
(`.kiro/specs/coap-client-event-driven-io/` Requirement 2.6):

    [stall-probe] io_passes mode=readiness window_ms=1000 passes=812
        steps=1290 budget_hits=0 max_steps=9 steps_hist=1:520,2:201,3-4:80,...

Those are summed per cell into the drain-steps-per-pass distribution.

Usage:
    KYTHIRA_COAP_SEND_PROBE=1 build/tests/multi_raft_http_benchmark_test \\
        --run_test=multi_raft_http_benchmark/write_latency_by_group_count \\
        --log_level=message > sweep.log 2>&1
    scripts/coap-send-probe-summary.py sweep.log [--per-group]
"""

import argparse
import re
import sys
from collections import defaultdict

CELL = re.compile(r"^\s+(?P<label>.+), (?P<groups>\d+) group\(s\), (?P<tick>\d+) ms tick:\s*$")
# Boost.Test colours its messages on a terminal and sometimes off one.
ANSI = re.compile(r"\x1b\[[0-9;]*m")
PROBE = re.compile(
    r"\[stall-probe\] send_rpc .*?(?: group=(?P<group>\d+))? path=(?P<path>\S+)"
    r" lock_wait_us=(?P<lock>\d+) send_path_us=(?P<send>\d+)"
)
IO_PASSES = re.compile(
    r"\[stall-probe\] io_passes mode=(?P<mode>\S+) window_ms=\d+ passes=(?P<passes>\d+)"
    r" steps=(?P<steps>\d+) budget_hits=(?P<hits>\d+) max_steps=(?P<max>\d+)"
    r" steps_hist=(?P<hist>\S+)"
)


def quantile(sorted_values, q):
    """The same nearest-rank-below rule `latency_sample_set::quantile` uses."""
    if not sorted_values:
        return None
    return sorted_values[int(q * (len(sorted_values) - 1))]


def describe(values):
    values = sorted(values)
    if not values:
        return "n=0"
    p99 = quantile(values, 0.99) if len(values) >= 100 else None
    return "n={} p50={} p95={} p99={} max={} mean={:.1f}".format(
        len(values),
        quantile(values, 0.50),
        quantile(values, 0.95),
        "unavailable" if p99 is None else p99,
        values[-1],
        sum(values) / len(values),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("log", help="log of the write_latency_by_group_count case")
    parser.add_argument("--per-group", action="store_true", help="also print one line per group")
    args = parser.parse_args()

    cells = []  # (header, {"lock": [...], "send": [...], "groups": {g: [...]}})
    current = None
    with open(args.log, errors="replace") as log:
        for raw in log:
            line = ANSI.sub("", raw)
            cell = CELL.match(line)
            if cell:
                current = ("{label}, {groups} group(s), {tick} ms tick".format(**cell.groupdict()),
                           {"lock": [], "send": [], "groups": defaultdict(list),
                            "io": {"modes": set(), "passes": 0, "steps": 0, "hits": 0,
                                   "max": 0, "hist": {}}})
                cells.append(current)
                continue
            io = IO_PASSES.search(line)
            if io and current is not None:
                acc = current[1]["io"]
                acc["modes"].add(io.group("mode"))
                acc["passes"] += int(io.group("passes"))
                acc["steps"] += int(io.group("steps"))
                acc["hits"] += int(io.group("hits"))
                acc["max"] = max(acc["max"], int(io.group("max")))
                for bucket in io.group("hist").split(","):
                    label, count = bucket.split(":")
                    acc["hist"][label] = acc["hist"].get(label, 0) + int(count)
                continue
            probe = PROBE.search(line)
            if probe and current is not None:
                lock = int(probe.group("lock"))
                current[1]["lock"].append(lock)
                current[1]["send"].append(int(probe.group("send")))
                if probe.group("group") is not None:
                    current[1]["groups"][int(probe.group("group"))].append(lock)

    printed = False
    for header, data in cells:
        if not data["lock"]:
            continue  # an HTTP cell: no probe lines, and nothing to report
        printed = True
        print(header)
        print("  lock_wait_us  " + describe(data["lock"]))
        print("  send_path_us  " + describe(data["send"]))
        per_group_p95 = {g: quantile(sorted(v), 0.95) for g, v in data["groups"].items()}
        if per_group_p95:
            worst = max(per_group_p95, key=per_group_p95.get)
            best = min(per_group_p95, key=per_group_p95.get)
            print("  lock_wait_us p95 by group: {} (group {}) to {} (group {}) over {} groups".format(
                per_group_p95[best], best, per_group_p95[worst], worst, len(per_group_p95)))
        io = data["io"]
        if io["passes"]:
            print("  io passes     mode={} passes={} steps/pass mean={:.2f} max={} "
                  "budget reached={} ({:.2%})".format(
                      "/".join(sorted(io["modes"])), io["passes"], io["steps"] / io["passes"],
                      io["max"], io["hits"], io["hits"] / io["passes"]))
            print("  steps/pass    " + " ".join(
                "{}:{:.1%}".format(label, count / io["passes"])
                for label, count in io["hist"].items()))
        if args.per_group:
            for group in sorted(data["groups"]):
                print("    group {}: {}".format(group, describe(data["groups"][group])))
    if not printed:
        print("no [stall-probe] send_rpc lines with lock_wait_us found; "
              "was KYTHIRA_COAP_SEND_PROBE=1 set?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
