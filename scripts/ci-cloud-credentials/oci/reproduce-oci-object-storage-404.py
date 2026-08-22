#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Reproduce: Object Storage intermittently declines a correctly-authorized
non-administrator principal with 404 BucketNotFound / 401 NotAuthenticated,
under concurrency.

SELF-CONTAINED AND TENANCY-AGNOSTIC. It reads standard ~/.oci/config profiles
and takes every OCID as an argument, so it can be run against any tenancy --
including by Oracle support against a reproduction of their own. Only the
`oci` SDK is required.

    pip install oci
    ./reproduce-oci-object-storage-404.py \
        --subject-profile CI --control-profile DEFAULT \
        --compartment-id ocid1.compartment.oc1..xxxx \
        --bucket my-bucket

WHAT IT DOES

Runs one matrix per region and prints a verdict. Each row changes exactly one
variable from the row above it, which is the whole point: the fault needs a
non-administrator principal AND Object Storage AND concurrency, and removing
any one of the three makes it disappear.

    subject  / Object Storage / serial        <- near-clean
    subject  / Object Storage / concurrent    <- DECLINES
    control  / Object Storage / concurrent    <- clean (rules out load)
    subject  / Compute        / concurrent    <- clean (rules out the principal)
    subject  / Identity       / concurrent    <- clean (rules out the credential)

THREE THINGS THAT MAKE A RESULT MEANINGFUL

1. THE FAULT IS EPISODIC. Twelve identical bursts measured 0.00%-13.40%, and
   two consecutive 1000-request bursts five minutes apart read 11.70% then
   1.70%. A single burst proves nothing either way. --repeat defaults to 10
   and the verdict is stated in EPISODES (bursts over --episode-threshold),
   not in a pooled percentage: requests inside an episode are correlated, so
   a pooled rate with a binomial interval overstates its own confidence.

2. CONCURRENCY IS PART OF THE EXPERIMENT, not a detail of how fast you got the
   answer. Comparing a serial number to a concurrent one compares two
   different things.

3. A CLEAN RUN IS NOT A REFUTATION. Between episodes the subject principal is
   near-clean for minutes at a time. If everything comes back 0.00%, re-run
   later before concluding anything; --repeat is the cheapest way to widen the
   window.

EXIT STATUS
    0  declines observed for the subject principal (reproduced)
    1  no declines observed anywhere (inconclusive -- see note 3)
    2  configuration/preflight error
"""

import argparse
import collections
import concurrent.futures as cf
import json
import os
import sys
import time
import uuid

try:
    import oci
except ImportError:
    sys.exit("the 'oci' SDK is required: pip install oci")


# --------------------------------------------------------------------------
# config
# --------------------------------------------------------------------------

def load(profile, region):
    """Standard ~/.oci/config profile, optionally retargeted at a region."""
    try:
        cfg = oci.config.from_file(profile_name=profile)
    except Exception as exc:                                  # noqa: BLE001
        raise SystemExit(f"could not load profile '{profile}': {exc}") from exc
    if region:
        cfg = dict(cfg, region=region)
    oci.config.validate_config(cfg)
    return cfg


def whoami(cfg):
    """Describe the principal so the report names it rather than a profile."""
    try:
        ident = oci.identity.IdentityClient(cfg)
        user = ident.get_user(cfg["user"]).data
        groups = []
        for m in ident.list_user_group_memberships(
                compartment_id=cfg["tenancy"], user_id=cfg["user"]).data:
            try:
                groups.append(ident.get_group(m.group_id).data.name)
            except Exception:                                 # noqa: BLE001
                groups.append(m.group_id)
        return f"{user.name} [{', '.join(groups) or 'no groups'}]"
    except oci.exceptions.ServiceError:
        # Expected for the subject: a least-privilege principal usually cannot
        # read its own user or group memberships. Say so, rather than printing
        # a bare OCID that reads like a failure.
        return f"{cfg.get('user', '<unknown>')} (cannot self-describe: no identity read)"
    except Exception:                                         # noqa: BLE001
        return cfg.get("user", "<unknown>")


# --------------------------------------------------------------------------
# the burst
# --------------------------------------------------------------------------

Result = collections.namedtuple("Result", "label declines total counts rids rates")


def run_bursts(call, n, workers, repeat, label, verbose=True):
    """`call(i)` -> (ok: bool, status: str, request_id: str|None)."""
    rates, counts, rids, declines, total = [], collections.Counter(), [], 0, 0
    for r in range(repeat):
        t0 = time.time()
        if workers == 1:
            res = [call(i) for i in range(n)]
        else:
            with cf.ThreadPoolExecutor(max_workers=workers) as ex:
                res = list(ex.map(call, range(n)))
        bad = [x for x in res if not x[0]]
        counts.update(x[1] for x in res)
        rids += [x[2] for x in bad if x[2]]
        declines += len(bad)
        total += n
        rate = 100.0 * len(bad) / n
        rates.append(rate)
        if verbose:
            print(f"    burst {r + 1:>2}/{repeat}  declines={len(bad):>4}  "
                  f"rate={rate:6.2f}%  {time.time() - t0:5.1f}s")
    return Result(label, declines, total, counts, rids, rates)


def storage_call(client, namespace, bucket, prefix):
    def one(_):
        try:
            resp = client.list_objects(namespace, bucket, prefix=prefix, limit=1000)
            return True, str(resp.status), None
        except oci.exceptions.ServiceError as e:
            return False, f"{e.status} {e.code}", e.request_id
    return one


def compute_call(client, compartment):
    def one(_):
        try:
            return True, str(client.list_instances(compartment_id=compartment).status), None
        except oci.exceptions.ServiceError as e:
            return False, f"{e.status} {e.code}", e.request_id
    return one


def identity_call(client):
    def one(_):
        try:
            return True, str(client.list_regions().status), None
        except oci.exceptions.ServiceError as e:
            return False, f"{e.status} {e.code}", e.request_id
    return one


# --------------------------------------------------------------------------
# per-region bucket handling
# --------------------------------------------------------------------------

def ensure_bucket(cfg, compartment, bucket, seed, create):
    """Return (namespace, bucket, created?). Buckets are REGIONAL: a bucket in
    the home region does not exist in another one, which is why --create-bucket
    is offered for the multi-region comparison."""
    os_client = oci.object_storage.ObjectStorageClient(cfg)
    namespace = os_client.get_namespace().data
    if not create:
        os_client.get_bucket(namespace, bucket)               # raises if absent
        return namespace, bucket, False

    name = f"{bucket}-{cfg['region']}-{uuid.uuid4().hex[:8]}"
    os_client.create_bucket(namespace, oci.object_storage.models.CreateBucketDetails(
        name=name, compartment_id=compartment))
    for i in range(seed):
        os_client.put_object(namespace, name, f"repro/obj-{i:04d}", b"x")
    return namespace, name, True


def delete_bucket(cfg, namespace, bucket):
    os_client = oci.object_storage.ObjectStorageClient(cfg)
    while True:
        page = os_client.list_objects(namespace, bucket, limit=1000).data
        if not page.objects:
            break
        for o in page.objects:
            os_client.delete_object(namespace, bucket, o.name)
    os_client.delete_bucket(namespace, bucket)


# --------------------------------------------------------------------------

def main():                                                   # noqa: C901
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--subject-profile", default="DEFAULT",
                    help="~/.oci/config profile for the NON-ADMIN principal under test")
    ap.add_argument("--control-profile",
                    help="profile for an ADMINISTRATOR. Optional but strongly "
                         "recommended: without it you cannot tell a service "
                         "fault from ordinary load.")
    ap.add_argument("--compartment-id", required=True)
    ap.add_argument("--bucket", required=True,
                    help="existing bucket, or the name PREFIX when --create-bucket")
    ap.add_argument("--prefix", default="", help="object prefix to list")
    ap.add_argument("--regions", default="",
                    help="comma-separated; default is the subject profile's own region")
    ap.add_argument("--create-bucket", action="store_true",
                    help="create (and delete) a throwaway bucket per region")
    ap.add_argument("--keep-bucket", action="store_true", help="do not delete it")
    ap.add_argument("--seed-objects", type=int, default=25)
    ap.add_argument("--n", type=int, default=500, help="requests per burst")
    ap.add_argument("--workers", type=int, default=16, help="concurrency for the concurrent rows")
    ap.add_argument("--repeat", type=int, default=10, help="bursts per row; >1 is what makes it readable")
    ap.add_argument("--episode-threshold", type=float, default=5.0,
                    help="a burst at or above this %% counts as an episode")
    ap.add_argument("--serial-repeat", type=int, default=2,
                    help="bursts for the serial row (it is slow and reliably near-clean)")
    ap.add_argument("--json", help="write the full result, including request ids, here")
    a = ap.parse_args()

    regions = [r.strip() for r in a.regions.split(",") if r.strip()] or [None]
    report = {"generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "n": a.n, "workers": a.workers, "repeat": a.repeat, "regions": {}}
    any_decline = False

    for region in regions:
        subj = load(a.subject_profile, region)
        ctrl = load(a.control_profile, region) if a.control_profile else None
        rname = subj["region"]
        print(f"\n{'=' * 74}\nREGION {rname}\n{'=' * 74}")
        print(f"  subject : {whoami(subj)}")
        if ctrl:
            print(f"  control : {whoami(ctrl)}")

        created = False
        try:
            ns, bucket, created = ensure_bucket(
                subj, a.compartment_id, a.bucket, a.seed_objects, a.create_bucket)
        except oci.exceptions.ServiceError as e:
            print(f"  PREFLIGHT FAILED: {e.status} {e.code} -- {e.message}")
            print("  (if this is 404 BucketNotFound it may BE the fault; re-run, "
                  "since preflight is a single request and this fault is episodic)")
            report["regions"][rname] = {"preflight_error": f"{e.status} {e.code}"}
            continue
        print(f"  namespace {ns}   bucket {bucket}"
              f"{'  (created for this run)' if created else ''}\n")

        rows = []
        try:
            sc = oci.object_storage.ObjectStorageClient(subj)
            print(f"  [1] subject / Object Storage / SERIAL")
            rows.append(run_bursts(storage_call(sc, ns, bucket, a.prefix),
                                   a.n, 1, a.serial_repeat, "subject/storage/serial"))

            print(f"  [2] subject / Object Storage / {a.workers}-way CONCURRENT")
            rows.append(run_bursts(storage_call(sc, ns, bucket, a.prefix),
                                   a.n, a.workers, a.repeat, "subject/storage/concurrent"))

            if ctrl:
                cc = oci.object_storage.ObjectStorageClient(ctrl)
                print(f"  [3] CONTROL (admin) / Object Storage / {a.workers}-way CONCURRENT")
                rows.append(run_bursts(storage_call(cc, ns, bucket, a.prefix),
                                       a.n, a.workers, a.repeat, "control/storage/concurrent"))

            print(f"  [4] subject / Compute ListInstances / {a.workers}-way CONCURRENT")
            rows.append(run_bursts(compute_call(oci.core.ComputeClient(subj), a.compartment_id),
                                   a.n, a.workers, max(1, a.repeat // 5), "subject/compute/concurrent"))

            print(f"  [5] subject / Identity ListRegions / {a.workers}-way CONCURRENT")
            rows.append(run_bursts(identity_call(oci.identity.IdentityClient(subj)),
                                   a.n, a.workers, max(1, a.repeat // 5), "subject/identity/concurrent"))
        finally:
            if created and not a.keep_bucket:
                try:
                    delete_bucket(subj, ns, bucket)
                    print(f"\n  cleaned up bucket {bucket}")
                except Exception as exc:                      # noqa: BLE001
                    print(f"\n  WARNING: could not delete {bucket}: {exc}", file=sys.stderr)

        print(f"\n  {'row':<30} {'N':>6} {'declines':>9} {'pooled':>8} {'episodes':>9}  max")
        print(f"  {'-' * 74}")
        rr = {}
        for r in rows:
            eps = sum(1 for x in r.rates if x >= a.episode_threshold)
            pooled = 100.0 * r.declines / r.total if r.total else 0.0
            print(f"  {r.label:<30} {r.total:>6} {r.declines:>9} {pooled:>7.2f}% "
                  f"{eps:>4}/{len(r.rates):<4} {max(r.rates):>6.2f}%")
            rr[r.label] = {"n": r.total, "declines": r.declines,
                           "pooled_pct": round(pooled, 3), "episodes": eps,
                           "bursts": len(r.rates),
                           "per_burst_pct": [round(x, 2) for x in r.rates],
                           "status_counts": dict(r.counts),
                           "decline_request_ids": r.rids[:50]}
            if r.declines and r.label.startswith("subject/storage"):
                any_decline = True

        subj_conc = rr.get("subject/storage/concurrent", {})
        ctrl_conc = rr.get("control/storage/concurrent")
        print()
        if subj_conc.get("declines"):
            print(f"  VERDICT [{rname}]: REPRODUCED. The subject principal was declined "
                  f"{subj_conc['declines']} times in {subj_conc['n']} concurrent requests.")
            if ctrl_conc is not None:
                print(f"    control (administrator), same concurrency, same minutes: "
                      f"{ctrl_conc['declines']} declines in {ctrl_conc['n']}.")
            for label in ("subject/compute/concurrent", "subject/identity/concurrent"):
                if label in rr:
                    print(f"    {label}: {rr[label]['declines']} declines in {rr[label]['n']}.")
            ids = subj_conc.get("decline_request_ids", [])
            if ids:
                print("    sample opc-request-ids for a support ticket:")
                for x in ids[:5]:
                    print(f"      {x}")
        else:
            print(f"  VERDICT [{rname}]: not observed in this window. "
                  f"This fault is EPISODIC -- that is not a refutation. Re-run, "
                  f"or raise --repeat.")
        report["regions"][rname] = {"namespace": ns, "bucket": bucket, "rows": rr}

    if a.json:
        with open(a.json, "w") as fh:
            json.dump(report, fh, indent=2)
        print(f"\nfull result written to {a.json}")

    if len(regions) > 1:
        print(f"\n{'=' * 74}\nCROSS-REGION\n{'=' * 74}")
        for rname, d in report["regions"].items():
            row = d.get("rows", {}).get("subject/storage/concurrent")
            if row:
                print(f"  {rname:<18} {row['declines']:>5} / {row['n']:<6} "
                      f"({row['pooled_pct']:.2f}%)  episodes {row['episodes']}/{row['bursts']}")
            else:
                print(f"  {rname:<18} {d.get('preflight_error', 'no data')}")
        print("\n  Declines in ONE region only implicate that region's Object Storage")
        print("  fleet. Declines in EVERY region implicate the tenancy or the")
        print("  principal, and make a regional fleet explanation untenable.")

    return 0 if any_decline else 1


if __name__ == "__main__":
    sys.exit(main())
