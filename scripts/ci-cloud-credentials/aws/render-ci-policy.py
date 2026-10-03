#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Render and lint the AWS CI role's bundle policies.

render: merge the policies/<bundle>.json fragments named by --bundles into
        the one inline policy document provision-oidc-role.sh puts on the
        CI role, and print it.
--check: lint every fragment in policies/ and render all of them together,
        without AWS access. Run by CI on every change.

Merging drops a statement whose Sid an earlier bundle already contributed,
provided the two are identical. The EC2 bundles share their tagging
statements (Ec2TagOnCreate, Ec2TagExistingNonInstanceResources) so that each
bundle stays self-contained for provision-developer-user.sh, which attaches
one managed policy per bundle. IAM rejects duplicate Sids in one document,
and the role's inline policy has 10,240 characters to share between every
bundle, so the copies must collapse to one. Two different statements under
one Sid is an error: one of them would be silently lost.

The lint enforces the rule the README's "Instance actions are tag-scoped"
section explains: no statement may let CI stop, start, terminate, reboot or
retag an EC2 instance without a condition. An unconditioned ec2:CreateTags
on instances defeats every tag condition in every bundle, because CI could
tag someone else's instance into scope and then terminate it.
"""

import argparse
import fnmatch
import json
import pathlib
import sys

POLICY_DIR = pathlib.Path(__file__).resolve().parent / "policies"

# IAM's limits, in characters excluding whitespace (IAM does not count it).
ROLE_INLINE_LIMIT = 10240
# provision-developer-user.sh measures the rendered document as-is, so its
# check counts whitespace; mirror that rather than the looser IAM rule.
MANAGED_POLICY_LIMIT = 6144

# Actions that change an existing instance's state or its tags. Tags matter
# as much as state: every instance condition in policies/ is a tag condition.
INSTANCE_MUTATING_ACTIONS = (
    "ec2:CreateTags",
    "ec2:DeleteTags",
    "ec2:ModifyInstanceAttribute",
    "ec2:ModifyInstanceMetadataOptions",
    "ec2:RebootInstances",
    "ec2:StartInstances",
    "ec2:StopInstances",
    "ec2:TerminateInstances",
)

# Long enough to stand in for any real account id or bucket name when
# measuring the rendered size (S3 names are at most 63 characters).
SAMPLE_ACCOUNT_ID = "123456789012"
SAMPLE_BUCKET = "b" * 63


def as_list(value):
    return value if isinstance(value, list) else [value]


def load_bundle(name, account_id, bucket):
    path = POLICY_DIR / f"{name}.json"
    if not path.is_file():
        valid = " ".join(sorted(p.stem for p in POLICY_DIR.glob("*.json")))
        raise SystemExit(f"ERROR: unknown bundle '{name}' (no {path}); valid bundles: {valid}")
    text = path.read_text().replace("{{ACCOUNT_ID}}", account_id).replace("{{BUCKET}}", bucket)
    return json.loads(text)


def merge(bundles, account_id, bucket):
    by_sid = {}
    statements = []
    for name in bundles:
        for stmt in load_bundle(name, account_id, bucket):
            sid = stmt.get("Sid")
            if sid in by_sid:
                if by_sid[sid][1] != stmt:
                    raise SystemExit(
                        f"ERROR: Sid '{sid}' differs between bundles '{by_sid[sid][0]}' and "
                        f"'{name}'. Shared statements must be identical; rename one otherwise.")
                continue
            if sid:
                by_sid[sid] = (name, stmt)
            statements.append(stmt)
    statements.append({"Sid": "StsGetCallerIdentity", "Effect": "Allow",
                       "Action": "sts:GetCallerIdentity", "Resource": "*"})
    return {"Version": "2012-10-17", "Statement": statements}


def compact_len(doc):
    return len(json.dumps(doc, separators=(",", ":")))


def reaches_instances(stmt):
    if "NotResource" in stmt:
        return not any(fnmatch.fnmatchcase("instance/*", r.split(":", 5)[-1])
                       for r in as_list(stmt["NotResource"]) if r.startswith("arn:aws:ec2:"))
    for res in as_list(stmt.get("Resource", [])):
        if res == "*" or (res.startswith("arn:aws:ec2:")
                          and fnmatch.fnmatchcase("instance/x", res.split(":", 5)[-1])):
            return True
    return False


def lint_bundle(name):
    errors = []
    stmts = load_bundle(name, SAMPLE_ACCOUNT_ID, SAMPLE_BUCKET)
    sids = [s.get("Sid") for s in stmts]
    for sid in {s for s in sids if sids.count(s) > 1}:
        errors.append(f"{name}: Sid '{sid}' appears more than once")
    for stmt in stmts:
        if stmt.get("Effect") != "Allow" or "Condition" in stmt or not reaches_instances(stmt):
            continue
        granted = [a for a in INSTANCE_MUTATING_ACTIONS
                   if any(fnmatch.fnmatchcase(a, pat) for pat in as_list(stmt.get("Action", [])))]
        if granted:
            errors.append(
                f"{name}: statement '{stmt.get('Sid')}' grants {', '.join(granted)} on instances "
                f"with no condition. Scope it with an aws:ResourceTag condition (see README).")
    size = len(json.dumps({"Version": "2012-10-17", "Statement": stmts}))
    if size > MANAGED_POLICY_LIMIT:
        errors.append(f"{name}: renders to {size} characters, over the {MANAGED_POLICY_LIMIT}-"
                      f"character managed policy limit provision-developer-user.sh needs")
    return errors


def check():
    bundles = sorted(p.stem for p in POLICY_DIR.glob("*.json"))
    errors = [e for b in bundles for e in lint_bundle(b)]
    try:
        size = compact_len(merge(bundles, SAMPLE_ACCOUNT_ID, SAMPLE_BUCKET))
    except SystemExit as exc:
        errors.append(str(exc))
    else:
        print(f"all {len(bundles)} bundles merged: {size} of {ROLE_INLINE_LIMIT} characters")
        if size > ROLE_INLINE_LIMIT:
            errors.append(f"every bundle together renders to {size} characters, over the role "
                          f"inline policy limit of {ROLE_INLINE_LIMIT}")
    for e in errors:
        print(f"::error::{e}", file=sys.stderr)
    return 1 if errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="lint every bundle and exit")
    parser.add_argument("--bundles", help="comma-separated bundle names")
    parser.add_argument("--account-id", default=SAMPLE_ACCOUNT_ID)
    parser.add_argument("--bucket", default="")
    args = parser.parse_args()
    if args.check:
        return check()
    if not args.bundles:
        parser.error("--bundles is required unless --check is given")
    doc = merge([b for b in args.bundles.split(",") if b], args.account_id, args.bucket)
    size = compact_len(doc)
    if size > ROLE_INLINE_LIMIT:
        raise SystemExit(f"ERROR: the merged policy is {size} characters, over IAM's "
                         f"{ROLE_INLINE_LIMIT}-character role inline policy limit")
    print(json.dumps(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main())
