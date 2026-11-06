#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Split the clang-tidy file list into shards of about equal measured cost.

.kiro/specs/clang-tidy-result-cache/ Requirement 8. CMakeLists.txt calls the
`plan` mode at configure time in place of the old round-robin deal, which in
run 42113857996 gave one shard 27 minutes and another 66.

plan    Longest-processing-time-first: sort the files by cost descending
        (ties by path) and give each to the shard with the smallest running
        load (ties to the lowest index). With --jobs J, a shard's first and
        longest file counts J times, because run-clang-tidy starts files in
        no fixed order and the longest may start last. A file missing from
        the costs file gets the median listed cost (1.0 if the file lists
        none). The result is a pure function of the file list, the costs,
        the shard count and J.
        Writes shard INDEX's files, sorted, one per line, to --out, and
        prints one "tidy shard plan:" line with the predicted spread.

refresh Merge per-TU timings from tidy-cache.py --stats JSON files into a
        costs file. Only analysed TUs (not cache hits) carry a timing; TUs
        with none keep their old cost, and TUs no longer in --files are
        dropped when --files is given.

Costs file format: `<seconds> <repository-relative path>` per line, sorted
by path; lines starting with `#` are comments.
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
from pathlib import Path


def read_costs(path: Path | None) -> dict[str, float]:
    costs: dict[str, float] = {}
    if path is None or not path.exists():
        return costs
    for n, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise SystemExit(f"{path}:{n}: expected '<seconds> <path>'")
        costs[parts[1]] = float(parts[0])
    return costs


def write_costs(path: Path, costs: dict[str, float], header: list[str]) -> None:
    lines = [f"# {h}" if h else "#" for h in header]
    lines += [f"{costs[p]:.2f} {p}" for p in sorted(costs)]
    path.write_text("\n".join(lines) + "\n")


def relpath(f: str, root: Path) -> str:
    p = Path(f)
    if p.is_absolute():
        try:
            return p.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            return p.as_posix()
    return p.as_posix()


def plan(files: list[str], costs: dict[str, float], count: int,
         jobs: int = 1) -> tuple[list[list[str]], list[float]]:
    """LPT assignment of files (repository-relative) to `count` shards.

    Each shard runs its files `jobs` at a time in an order we do not
    control (run-clang-tidy walks a Python set), so its worst-case wall
    time is (total + (jobs - 1) * longest) / jobs: the longest file may
    start last. Because files arrive longest first, a shard's first file
    is its longest, so it is charged `jobs` times its cost. With jobs=1
    this is plain LPT on CPU-seconds. Returned totals are those charged
    loads, in CPU-seconds.
    """
    median = statistics.median(costs.values()) if costs else 1.0
    weighted = sorted(((costs.get(f, median), f) for f in set(files)),
                      key=lambda cf: (-cf[0], cf[1]))
    shards: list[list[str]] = [[] for _ in range(count)]
    totals = [0.0] * count
    for cost, f in weighted:
        i = min(range(count), key=lambda k: (totals[k], k))
        totals[i] += cost * (jobs if not shards[i] else 1)
        shards[i].append(f)
    return [sorted(s) for s in shards], totals


def cmd_plan(args) -> int:
    if args.jobs < 1:
        print(f"tidy-shard: --jobs {args.jobs} must be at least 1", file=sys.stderr)
        return 2
    if args.count < 1 or not 0 <= args.index < args.count:
        print(f"tidy-shard: index {args.index} is outside 0..{args.count - 1}", file=sys.stderr)
        return 2
    root = Path(args.root)
    rel = {relpath(f, root): f for f in args.files}
    costs = read_costs(Path(args.costs) if args.costs else None)
    shards, totals = plan(list(rel), costs, args.count, args.jobs)
    chosen = [rel[f] for f in shards[args.index]]
    Path(args.out).write_text("".join(f"{f}\n" for f in chosen))
    lo, hi = min(totals), max(totals)
    known = sum(1 for f in rel if f in costs)
    j = args.jobs
    median = statistics.median(costs.values()) if costs else 1.0
    cpu = sum(costs.get(f, median) for f in shards[args.index])
    print(f"tidy shard plan: {len(rel)} files ({known} with a measured cost) in "
          f"{args.count} shards of {j} jobs; worst-case wall s per shard min "
          f"{lo / j:.0f} max {hi / j:.0f} (max/min {hi / lo if lo else float('inf'):.2f}); "
          f"shard {args.index} has {len(chosen)} files, {cpu:.0f} CPU-s, worst case "
          f"{totals[args.index] / j:.0f} s")
    return 0


def cmd_refresh(args) -> int:
    old = read_costs(Path(args.costs))
    new = dict(old)
    for stats_path in args.stats:
        stats = json.loads(Path(stats_path).read_text())
        for rec in stats.get("per_tu", []):
            secs = rec.get("seconds")
            if rec.get("hit") or not secs:
                continue
            tu = rec["tu"]
            for token in ("@SRC@/",):
                if tu.startswith(token):
                    tu = tu[len(token):]
            new[tu] = float(secs)
    if args.files:
        keep = {relpath(f, Path(args.root)) for f in args.files}
        new = {p: c for p, c in new.items() if p in keep}
    header = [
        "Copyright (c) 2026 Clark Rawlins",
        "SPDX-License-Identifier: Apache-2.0",
        "",
        "Measured clang-tidy seconds per translation unit on a 4-vCPU",
        "ubuntu-24.04 runner, for scripts/tidy-shard.py (clang-tidy-result-cache",
        "Requirement 8). Regenerate with `scripts/tidy-shard.py refresh`.",
    ] + ([f"Source: {args.source}"] if args.source else [])
    write_costs(Path(args.out or args.costs), new, header)
    changed = sum(1 for p in new if old.get(p) != new[p])
    print(f"tidy-shard: {len(new)} costs, {changed} new or changed")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--count", type=int, required=True)
    p.add_argument("--index", type=int, required=True)
    p.add_argument("--costs")
    p.add_argument("--jobs", type=int, default=1,
                   help="files each shard analyses at once (run-clang-tidy -j)")
    p.add_argument("--out", required=True)
    p.add_argument("--root", default=os.getcwd(), help="repository root for relative paths")
    p.add_argument("--files-from", help="read the file list from this file")
    p.add_argument("files", nargs="*")
    r = sub.add_parser("refresh")
    r.add_argument("--costs", required=True)
    r.add_argument("--out", help="write here instead of overwriting --costs")
    r.add_argument("--root", default=os.getcwd())
    r.add_argument("--source", default="", help="run that produced the timings")
    r.add_argument("--files-from")
    r.add_argument("--stats", nargs="+", required=True)
    r.add_argument("files", nargs="*")
    args = ap.parse_args()
    if args.files_from:
        args.files += [l.strip() for l in Path(args.files_from).read_text().splitlines() if l.strip()]
    return cmd_plan(args) if args.cmd == "plan" else cmd_refresh(args)


if __name__ == "__main__":
    sys.exit(main())
