#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Build a copy of a vcpkg install prefix with Folly, and every installed port
# that depends on it, removed (.kiro/specs/ci-build-matrix-coverage/,
# Requirement 2.2, design section 3).
#
# Hiding Folly from find_package() alone is not enough for the no-folly CI
# job: vcpkg installs every port's headers into one shared include/
# directory, so a stray `#include <folly/...>` would still compile against
# the full prefix. The copy this script produces has no Folly headers,
# libraries or CMake package files at all, so both find_package() and the
# preprocessor fail on Folly.
#
# How:
#   1. Read <vcpkg-installed>/vcpkg/status, vcpkg's own record of what is
#      installed and what each port (and each installed feature of it)
#      depends on, and compute folly plus everything that depends on it,
#      transitively. Computing the set rather than naming proxygen, wangle
#      and fizz keeps this correct when a port gains or drops Folly. The
#      status file is read instead of running `vcpkg depend-info` because
#      on a warm actions/cache hit the CI job never bootstraps vcpkg.
#   2. `cp -al` the triplet directory to <out-dir>: hard links, so the copy
#      costs no disk and takes seconds.
#   3. Delete each removed port's files from the copy, using
#      <vcpkg-installed>/vcpkg/info/<port>_<version>_<triplet>.list as the
#      manifest. Deleting a hard link in the copy never touches the original
#      tree, so the vcpkg cache the job saves afterwards is unaffected.
#   4. Fail if any Folly header, library or package directory survived.
#
# Usage: scripts/make-folly-free-prefix.sh <vcpkg-installed-dir> <triplet> <out-dir>
#   e.g. scripts/make-folly-free-prefix.sh vcpkg_installed x64-linux "$RUNNER_TEMP/no-folly-prefix"
#
# Prints the removed ports, one per line, prefixed "removed: ".

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <vcpkg-installed-dir> <triplet> <out-dir>" >&2
    exit 2
fi

INSTALLED="$1"
TRIPLET="$2"
OUT="$3"

STATUS="$INSTALLED/vcpkg/status"
INFO_DIR="$INSTALLED/vcpkg/info"
SRC="$INSTALLED/$TRIPLET"

for path in "$STATUS" "$INFO_DIR" "$SRC"; do
    if [[ ! -e "$path" ]]; then
        echo "error: $path does not exist; is $INSTALLED a vcpkg install tree?" >&2
        exit 1
    fi
done
if [[ -e "$OUT" ]]; then
    echo "error: $OUT already exists; refusing to write into it" >&2
    exit 1
fi

# Step 1: folly and its transitive dependants, among ports installed for
# this triplet. A port counts as a dependant if its core paragraph or any
# installed feature paragraph depends on a removed port.
mapfile -t REMOVE < <(python3 - "$STATUS" "$TRIPLET" <<'PY'
import re
import sys

status_path, triplet = sys.argv[1], sys.argv[2]
paragraphs = open(status_path, encoding="utf-8").read().split("\n\n")
deps = {}  # port -> set of ports it (or an installed feature of it) needs
for para in paragraphs:
    fields = {}
    key = None
    for line in para.splitlines():
        if line[:1] in (" ", "\t") and key:
            fields[key] += " " + line.strip()
            continue
        if ":" in line:
            key, _, value = line.partition(":")
            key = key.strip()
            fields[key] = value.strip()
    if fields.get("Architecture") != triplet:
        continue
    if not fields.get("Status", "").endswith(" installed"):
        continue
    port = fields["Package"]
    entry = deps.setdefault(port, set())
    for dep in fields.get("Depends", "").split(","):
        dep = dep.strip()
        if not dep:
            continue
        # "name[feat1,feat2]:triplet" -> "name"; a dependency qualified with
        # another triplet is a host tool, not something linked into this one.
        name = re.sub(r"\[.*?\]", "", dep)
        name, _, dep_triplet = name.partition(":")
        if dep_triplet and dep_triplet != triplet:
            continue
        entry.add(name.strip())

if "folly" not in deps:
    sys.exit(f"error: folly is not installed for {triplet}; nothing to remove")

removed = {"folly"}
changed = True
while changed:
    changed = False
    for port, needs in deps.items():
        if port not in removed and needs & removed:
            removed.add(port)
            changed = True
print("\n".join(sorted(removed)))
PY
)

if [[ ${#REMOVE[@]} -eq 0 ]]; then
    echo "error: computed an empty removal set" >&2
    exit 1
fi

# Step 2: hard-linked copy.
mkdir -p "$(dirname "$OUT")"
cp -al "$SRC" "$OUT"

# Step 3: delete each removed port's files from the copy. A .list line is a
# path relative to the vcpkg_installed root, starting with the triplet;
# directory lines end in "/" and are left alone (other ports may share
# them).
for port in "${REMOVE[@]}"; do
    shopt -s nullglob
    lists=("$INFO_DIR/${port}_"*"_${TRIPLET}.list")
    shopt -u nullglob
    if [[ ${#lists[@]} -ne 1 ]]; then
        echo "error: expected one info list for $port, found ${#lists[@]}" >&2
        exit 1
    fi
    while IFS= read -r rel; do
        [[ -z "$rel" || "$rel" == */ ]] && continue
        [[ "$rel" == "$TRIPLET/"* ]] || continue
        rm -f "$OUT/${rel#"$TRIPLET"/}"
    done < "${lists[0]}"
    echo "removed: $port"
done

# Step 4: nothing of Folly's may survive, whatever the .list files said.
leftovers="$(
    {
        [[ -d "$OUT/include/folly" ]] && find "$OUT/include/folly" -type f
        find "$OUT" \( -name 'libfolly*' -o -path '*/share/folly/*' \) -type f
    } 2>/dev/null || true
)"
if [[ -n "$leftovers" ]]; then
    echo "error: Folly files survived the prune:" >&2
    echo "$leftovers" | head -20 >&2
    exit 1
fi
# Empty directories the deletions left behind (include/folly/...) would
# still satisfy nothing, but remove them so the assertion above stays exact.
find "$OUT" -depth -type d -empty -delete
if [[ -e "$OUT/include/folly" ]]; then
    echo "error: $OUT/include/folly still exists" >&2
    exit 1
fi
