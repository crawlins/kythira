#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Assert that a build tree's binaries were linked by the linker CI asked for.
#
# Usage: check-linker.sh <linker> <build-dir-or-binary>...
#
# For each argument that is a directory, the first executable named *_test
# under it is checked, or failing that the first ELF executable outside
# CMakeFiles/ (CMake's compiler-identification and try_compile() binaries
# live there and are linked without the project's link options). A binary
# can also be named directly. The check reads the ELF .comment section, which
# mold, lld and gold stamp with their own name and version; ld.bfd stamps
# nothing there.
#
# This is the guard against the silent no-op .kiro/specs/mold-linker/
# Requirement 5.3 describes: a job that passes -DKYTHIRA_LINKER=mold but,
# through a cache entry, a stale tree or a compiler that quietly ignored the
# flag, links with ld.bfd and stays green. Configure already fails when mold
# is missing; this catches every way past that.
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "usage: $0 <linker> <build-dir-or-binary>..." >&2
  exit 2
fi
linker=$1
shift

status=0
for target in "$@"; do
  if [ -d "$target" ]; then
    # sed rather than head: head exits after one line, and with pipefail
    # sort's SIGPIPE on a tree with hundreds of tests fails the script.
    bin=$(find "$target" -maxdepth 3 -type f -perm -u+x -name '*_test' | sort | sed -n 1p)
    if [ -z "$bin" ]; then
      while IFS= read -r f; do
        if [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ]; then
          bin=$f
          break
        fi
      done < <(find "$target" -maxdepth 4 -path '*/CMakeFiles' -prune -o \
                 -type f -perm -u+x -print | sort)
    fi
    if [ -z "$bin" ]; then
      echo "::error::no executable under $target to check the linker of"
      status=1
      continue
    fi
  else
    bin=$target
  fi
  if [ ! -f "$bin" ]; then
    echo "::error::$bin does not exist"
    status=1
    continue
  fi
  comment=$(readelf -p .comment "$bin" 2>&1 || true)
  if grep -q "$linker" <<<"$comment"; then
    echo "$bin: linked by $(grep -o "$linker [0-9][0-9.]*" <<<"$comment" | sed -n 1p)"
  else
    echo "::error::$bin was not linked by $linker (KYTHIRA_LINKER=$linker had no effect)"
    echo "$comment"
    status=1
  fi
done
exit $status
