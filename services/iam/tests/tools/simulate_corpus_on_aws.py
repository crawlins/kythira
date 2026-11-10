#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Compare the policy corpus with AWS's own policy simulator
(.kiro/specs/iam-compatible-auth/ task 24, Requirement 15.5).

Every corpus `evaluate` case that iam:SimulateCustomPolicy can express is
sent to real AWS: an IAM user in its own account, one or more identity
policies, an optional permissions boundary and resource policy, and no
session policy (the same selection iam_query_api_test's
simulate_custom_policy_matches_the_corpus_decisions makes against
Kythira). SimulateCustomPolicy creates nothing and costs nothing; the
caller needs only iam:SimulateCustomPolicy on "*".

The corpus's example account, 111122223333, is replaced in every case by
the caller's real account, since AWS simulates against the caller's
account.

Each disagreement is written, with AWS's answer as the expected decision,
to the output file (default corpus/evaluate/aws_simulator.json) in the
corpus's own format, so iam_policy_test fails on it until the evaluator
agrees with AWS or the case is marked as a simulator divergence. A case
whose `aws_simulator` field names the decision AWS gave is a known place
where the simulator departs from documented enforcement: it is reported as
KNOWN and not counted. Before committing an output file, put the example
account back in place of the real one. The run prints a summary and exits
1 if any case disagreed, 2 on an error.

Usage:
    simulate_corpus_on_aws.py [--profile NAME] [--corpus DIR] [--out FILE]
                              [--dry-run]

--dry-run lists the cases that would be sent and calls nothing.
"""

import argparse
import json
import pathlib
import sys
import time

EXAMPLE_ACCOUNT = "111122223333"
HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_CORPUS = HERE.parent / "corpus"
SIMULATOR_SOURCE = "https://docs.aws.amazon.com/IAM/latest/APIReference/API_SimulateCustomPolicy.html"

DECISIONS = {"allowed": "allow", "explicitDeny": "explicit_deny", "implicitDeny": "implicit_deny"}

# SimulateCustomPolicy needs each context value's type. Keys not named here
# are strings; a JSON array makes the List form of the type.
KEY_TYPES = {
    "aws:sourceip": "ip",
    "aws:currenttime": "date",
    "aws:tokenissuetime": "date",
    "aws:epochtime": "numeric",
    "aws:multifactorauthage": "numeric",
    "aws:multifactorauthpresent": "boolean",
    "aws:securetransport": "boolean",
    "aws:viaawsservice": "boolean",
    "aws:principalisawsservice": "boolean",
}


def expressible(case):
    """Whether SimulateCustomPolicy can express this case."""
    return (case.get("kind") == "evaluate"
            and case.get("principal", "").startswith("user/")
            and case.get("identity")
            and "session" not in case
            and "principal_account" not in case
            and "resource_account" not in case
            and case.get("resource_kind", "ordinary") == "ordinary")


def localize(value, account):
    """The value with the corpus's example account replaced by `account`."""
    return json.loads(json.dumps(value).replace(EXAMPLE_ACCOUNT, account))


def as_text(doc):
    return doc if isinstance(doc, str) else json.dumps(doc)


def context_entries(context):
    entries = []
    for key, value in context.items():
        base = KEY_TYPES.get(key.lower(), "string")
        values = value if isinstance(value, list) else [value]
        entries.append({
            "ContextKeyName": key,
            "ContextKeyValues": [str(v) for v in values],
            "ContextKeyType": base + "List" if isinstance(value, list) else base,
        })
    return entries


def request_of(case, account):
    c = localize(case, account)
    req = {
        "PolicyInputList": [as_text(d) for d in c["identity"]],
        "ActionNames": [c["action"]],
        "ResourceArns": [c.get("resource", "*")],
        "CallerArn": f"arn:aws:iam::{account}:{c['principal']}",
    }
    if "boundary" in c:
        req["PermissionsBoundaryPolicyInputList"] = [as_text(c["boundary"])]
    if "resource_policy" in c:
        req["ResourcePolicy"] = as_text(c["resource_policy"])
        req["ResourceOwner"] = f"arn:aws:iam::{account}:root"
    if c.get("context"):
        req["ContextEntries"] = context_entries(c["context"])
    return req


def cases(corpus):
    for path in sorted((corpus / "evaluate").glob("*.json")):
        if path.name == "aws_simulator.json":
            continue
        doc = json.loads(path.read_text())
        for case in doc["cases"]:
            if expressible(case):
                yield path.name, doc.get("source"), case


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--profile")
    ap.add_argument("--corpus", type=pathlib.Path, default=DEFAULT_CORPUS)
    ap.add_argument("--out", type=pathlib.Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    out = args.out or args.corpus / "evaluate" / "aws_simulator.json"

    selected = list(cases(args.corpus))
    if args.dry_run:
        for name, _, case in selected:
            print(f"{name}: {case['name']}")
        print(f"{len(selected)} cases")
        return 0

    import boto3  # only for a real run
    from botocore.config import Config
    session = boto3.Session(profile_name=args.profile)
    account = session.client("sts").get_caller_identity()["Account"]
    iam = session.client("iam", config=Config(retries={"max_attempts": 8, "mode": "adaptive"}))

    disagreements, known, errors = [], [], []
    for name, file_source, case in selected:
        try:
            r = iam.simulate_custom_policy(**request_of(case, account))
            got = DECISIONS[r["EvaluationResults"][0]["EvalDecision"]]
        except Exception as e:  # report every case, stop on none
            errors.append(f"{name}: {case['name']}: {e}")
            continue
        if got != case["expect"] and got == case.get("aws_simulator"):
            print(f"KNOWN {name}: {case['name']}: corpus {case['expect']}, AWS simulator {got}")
            known.append(case["name"])
        elif got != case["expect"]:
            print(f"DISAGREE {name}: {case['name']}: corpus {case['expect']}, AWS {got}")
            new = dict(case)
            new["name"] = f"{case['name']} (AWS simulator, from {name})"
            new["expect"] = got
            new.pop("decided_by", None)
            new["source"] = case.get("source") or file_source or SIMULATOR_SOURCE
            disagreements.append(new)
        time.sleep(0.1)

    print(f"compared {len(selected) - len(errors)} cases with SimulateCustomPolicy: "
          f"{len(disagreements)} disagreed, {len(known)} known simulator divergences, "
          f"{len(errors)} errors")
    for e in errors:
        print(f"ERROR {e}", file=sys.stderr)
    if disagreements:
        out.write_text(json.dumps({
            "source": SIMULATOR_SOURCE,
            "description": "Cases where AWS's SimulateCustomPolicy disagreed with the corpus "
                           "(task 24); the expected decision is AWS's.",
            "cases": disagreements,
        }, indent=2) + "\n")
        print(f"wrote {out}")
    if errors:
        return 2
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main())
