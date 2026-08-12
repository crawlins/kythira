#!/bin/bash
# Static cloud-init user_data for the kythira CI pool's instance
# configuration (Requirement 4.4 on-instance heartbeat).
#
# ASCII ONLY, deliberately: cloud-init 26.1 on Ubuntu 24.04 was observed
# to silently drop a bare (non-MIME) user-data shellscript containing
# non-ASCII bytes -- valid UTF-8, "#!" first line, classified fine by the
# same cloud-init's UserDataProcessor when tested in isolation -- while
# the byte-identical script wrapped as a MIME part executed, and a pure
# ASCII bare script executed. Established by paired instance boots, not
# by reading cloud-init (whose upstream source disagrees with the
# observed behavior). Do not reintroduce fancy punctuation in comments.
#
# STATIC is the load-bearing property: an instance configuration is
# immutable and every pool launch shares it, so nothing per-run can live
# here. The per-run artifact URL arrives through a side channel this
# script polls for instead: a "kythira-artifact-url" freeform tag written
# onto the instance by whoever wants the heartbeat running (the real-cloud
# test suite, via its own UpdateInstance path), read back through the
# metadata service, which reflects freeform tags. No tag within the
# window: exit 0, and the instance is exactly what it was before this
# script existed -- every pre-heartbeat test keeps passing on this image.
#
# The metadata service (link-local, v2, "Bearer Oracle" mandatory) is the
# only dependency; the artifact fetch is a pre-authenticated request URL
# so it needs no auth at all, and it transits the VCN's service gateway --
# this subnet has no internet route, by design.
#
# python3 for the JSON, not jq: Canonical's cloud images ship python3 and
# do not ship jq, and an apt-get here would need internet the subnet does
# not have.

set -u
# Every milestone goes to BOTH the log file and the serial console: the
# instance is unreachable from outside (private subnet, no SSH keys), so
# "oci compute console-history capture" is the ONLY external read, and
# the first on-instance run failed with nothing observable anywhere --
# the bootstrap log died with the instance. An instrument you cannot
# read in the environment that has the phenomenon is half an instrument.
LOG=/var/log/kythira-heartbeat-bootstrap.log
note() { printf '[kythira-bootstrap] %s %s\n' "$(date -u +%FT%TZ)" "$*" >>"$LOG"; printf '[kythira-bootstrap] %s\n' "$*" >/dev/console 2>/dev/null || true; }
note "starting"

MD="http://169.254.169.254/opc/v2"
AUTH=(-H "Authorization: Bearer Oracle")

url=""
# About 10 minutes of polling: the tag is written after the suite sees
# the instance RUNNING, which can trail cloud-init by a provision-poll
# cycle.
i=0
while [ "$i" -lt 300 ]; do
  i=$((i + 1))
  tags=$(curl -sf -m 5 "${AUTH[@]}" "$MD/instance/freeformTags" || true)
  url=$(printf '%s' "$tags" | python3 -c '
import json,sys
try:
    print(json.load(sys.stdin).get("kythira-artifact-url",""))
except Exception:
    print("")
' || true)
  [ -n "$url" ] && break
  [ $((i % 30)) -eq 0 ] && note "still polling for artifact tag (i=$i)"
  sleep 2
done

if [ -z "$url" ]; then
  note "no kythira-artifact-url tag within the window; idling as before"
  exit 0
fi
note "artifact url tag found after $i polls"

BIN=/opt/kythira-heartbeat
i=0
while [ "$i" -lt 5 ]; do
  i=$((i + 1))
  curl -sf -m 120 -o "$BIN" "$url" && break
  note "download attempt $i failed"
  sleep 5
done
if [ ! -s "$BIN" ]; then
  note "artifact download failed"
  exit 1
fi
chmod +x "$BIN"
note "artifact downloaded ($(stat -c%s "$BIN") bytes)"

region=$(curl -sf -m 5 "${AUTH[@]}" "$MD/instance/canonicalRegionName" || true)
note "launching heartbeat writer (region=${region:-unresolved})"
KYTHIRA_OCI_REGION="$region" nohup "$BIN" --interval 10 \
  >/var/log/kythira-heartbeat.log 2>&1 &
writer_pid=$!
note "writer launched, pid $writer_pid"

# A writer that dies at startup (a missing shared library was the first
# real occurrence) would otherwise be invisible from outside the
# instance. 15 seconds is enough for a loader failure or an immediate
# crash to have happened; the writer's own log tail carries the reason.
sleep 15
if ! kill -0 "$writer_pid" 2>/dev/null; then
  note "WRITER DIED WITHIN 15s: $(tail -c 400 /var/log/kythira-heartbeat.log 2>/dev/null | tr '\n' ' ')"
  exit 1
fi
note "writer alive after 15s"
