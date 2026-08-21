#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Measure the kythira-ci-artifacts `404 BucketNotFound` decline rate directly.

This is the instrument that exonerated the `Oracle-Tags` policy `where` clause
on 2026-08-21 (spike-notes.md Finding 26). It talks to Object Storage as a
chosen principal and counts outcomes; it does not read logs.

RUN IT WITH THE OCI CLI'S OWN INTERPRETER -- the system python3 has no `oci`
module on this host:

    /home/clark/lib/oracle-cli/bin/python3 probe-object-storage-decline-rate.py \
        --principal ci --n 500 --workers 16 --repeat 10

THREE THINGS TO KNOW BEFORE YOU BELIEVE A NUMBER FROM THIS
==========================================================

1. THE FAULT IS EPISODIC. Twelve identical bursts measured 0.00%-13.40%, and
   two consecutive N=1000 bursts five minutes apart read 11.70% then 1.70%.
   A single burst's rate is NOT a parameter of anything. Always use --repeat
   and report the spread, and treat "episodes" (bursts >= 5%) as the unit --
   requests inside an episode are correlated, so a per-request confidence
   interval will badly overstate your power.

2. CONCURRENCY IS PART OF THE MEASUREMENT, not a detail of how fast you got
   it. Serial reads 0.30%; 16-way reads 11.70% for the same principal and the
   same request. Comparing a serial number to a concurrent one is comparing
   two different experiments.

3. THE PRINCIPAL IS PART OF THE MEASUREMENT. An Administrator is clean across
   5000 requests at the concurrency where `kythira-ci` episodes. `--principal
   admin` exists to re-run that control, which is what makes any `ci` number
   mean something.

WHAT IS ALREADY ELIMINATED, so nobody spends another session on it: the client
(a declined request has byte-identical successful twins), the credential type
(reproduces under a plain API key, not just the CI UPST), tenancy policy (the
clause was removed, measured, restored -- no improvement), and this principal's
authorization generally (its Compute and Identity calls are clean at the same
concurrency). The residual is Oracle-side: Object Storage's authorization path
for non-administrator principals failing intermittently under concurrent load.
The open action is a support ticket carrying opc-request-ids, not a policy edit.
"""
import argparse, collections, json, os, sys, time
import concurrent.futures as cf

import oci

TENANCY = "ocid1.tenancy.oc1..aaaaaaaadyhzu5oyq6rk7wgemydpmackco6biawlw7dcw3zhylfdlvpslmeq"
PRINCIPALS = {
    # IAM user kythira-ci, a member of group kythira-ci -- the same group the
    # CI job's UPST lands in, and the one whose requests are declined.
    "ci": dict(user="ocid1.user.oc1..aaaaaaaa5o5l5nuzsexhzih724qozir2zslid3uekwvzragabzsqhpwzdrkq",
               fingerprint="74:ff:32:74:53:44:22:2c:90:c2:cf:a5:a9:d0:43:28",
               key_file="~/.oci/kythira-ci.pem"),
    # The control. Unconditional `manage all-resources IN TENANCY`.
    "admin": dict(user="ocid1.user.oc1..aaaaaaaahz76ox4ueoysnkykippqk667yylncw2f5bdb6mwltqkguuux7wra",
                  fingerprint="e2:02:71:68:28:a5:56:99:17:6a:a7:df:ac:51:e2:06",
                  key_file="~/.oci/oci_api_key.pem"),
}
NS, BUCKET, PREFIX = "axunmw4f0mln", "kythira-ci-artifacts", "kythira-real-test/"
EPISODE_THRESHOLD_PCT = 5.0


def make_client(which):
    p = PRINCIPALS[which]
    cfg = {"tenancy": TENANCY, "region": "us-phoenix-1", "user": p["user"],
           "fingerprint": p["fingerprint"],
           "key_file": os.path.expanduser(p["key_file"])}
    oci.config.validate_config(cfg)
    return oci.object_storage.ObjectStorageClient(cfg)


def burst(client, n, workers):
    def one(_):
        try:
            r = client.list_objects(NS, BUCKET, prefix=PREFIX, limit=1000)
            return str(r.status), None
        except oci.exceptions.ServiceError as e:
            return f"{e.status} {e.code}", e.request_id
    if workers == 1:
        res = [one(i) for i in range(n)]
    else:
        with cf.ThreadPoolExecutor(max_workers=workers) as ex:
            res = list(ex.map(one, range(n)))
    bad = [r for r in res if not r[0].startswith("2")]
    return collections.Counter(r[0] for r in res), [r[1] for r in bad if r[1]]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--principal", choices=sorted(PRINCIPALS), default="ci")
    ap.add_argument("--n", type=int, default=500, help="requests per burst")
    ap.add_argument("--workers", type=int, default=16, help="1 = serial; part of the measurement")
    ap.add_argument("--repeat", type=int, default=10, help="bursts; >1 is what makes the result readable")
    a = ap.parse_args()

    client = make_client(a.principal)
    rates, total, declines, ids = [], 0, 0, []
    print(f"principal={a.principal}  n={a.n}  workers={a.workers}  repeat={a.repeat}")
    for i in range(a.repeat):
        t0 = time.time()
        counts, rids = burst(client, a.n, a.workers)
        d = sum(v for k, v in counts.items() if not k.startswith("2"))
        rate = 100.0 * d / a.n
        rates.append(rate); total += a.n; declines += d; ids += rids
        print("  burst %2d  declines=%3d  rate=%6.2f%%  %5.1fs  %s"
              % (i + 1, d, rate, time.time() - t0, dict(counts)))

    eps = sum(1 for r in rates if r >= EPISODE_THRESHOLD_PCT)
    print("\n  requests=%d  declines=%d  overall=%.2f%%" % (total, declines, 100.0 * declines / total))
    print("  per-burst rate: min=%.2f%%  median=%.2f%%  max=%.2f%%"
          % (min(rates), sorted(rates)[len(rates) // 2], max(rates)))
    print("  episodes (>=%.0f%%): %d of %d bursts   <-- compare THIS across conditions"
          % (EPISODE_THRESHOLD_PCT, eps, a.repeat))
    if ids:
        print("  sample opc-request-ids for a support ticket:")
        for r in ids[:5]:
            print("   ", r)
    if a.repeat == 1:
        print("\n  WARNING: --repeat 1. This fault is episodic; one burst tells you"
              "\n           almost nothing. Do not compare single bursts.", file=sys.stderr)


if __name__ == "__main__":
    main()
