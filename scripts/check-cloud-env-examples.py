#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Check that each cloud quorum manager's .env.example documents its config.

The AWS, Azure, GCP and OCI docker/<provider>_quorum_manager/*.env.example
files are the documented contract between a deployment's config loader and
the library's config structs. Nothing compiles them, so without a check they
drift silently: a field added to a struct simply never appears in the
example (the OCI file had drifted exactly this way before this check
existed). Each file names the fields it documents as `<struct>::<field>` in
its comments. The Alibaba file predates the convention and is not checked. This asserts,
for every struct listed below, that

  * every `<struct>::<field>` the file mentions is a real field of <struct>,
    so a rename or removal fails here, and
  * every field of <struct> is mentioned at least once, so an addition does.

A field that cannot be set from a file (an SDK object such as a credential)
is still mentioned, with a comment saying so.

Usage: check-cloud-env-examples.py   (exits 1 on any mismatch)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# env file -> [(header, struct), ...]
CONTRACTS = {
    "docker/aws_quorum_manager/aws_quorum_manager.env.example": [
        ("include/raft/aws_client_config.hpp", "aws_client_config"),
        ("include/raft/aws_ec2_quorum_manager.hpp", "aws_ec2_quorum_manager_config"),
        ("include/raft/aws_ec2_quorum_manager.hpp", "ec2_placement_group_config"),
        ("include/raft/aws_ec2_quorum_manager.hpp", "ec2_spot_options"),
        ("include/raft/aws_asg_quorum_manager.hpp", "aws_asg_quorum_manager_config"),
    ],
    "docker/azure_quorum_manager/azure_quorum_manager.env.example": [
        ("include/raft/azure_client_config.hpp", "azure_client_config"),
        ("include/raft/azure_vm_quorum_manager.hpp", "azure_vm_quorum_manager_config"),
        ("include/raft/azure_vm_quorum_manager.hpp", "azure_image_reference"),
        ("include/raft/azure_vm_quorum_manager.hpp", "azure_placement_config"),
        ("include/raft/azure_vm_quorum_manager.hpp", "azure_spot_options"),
        ("include/raft/azure_vmss_quorum_manager.hpp", "azure_vmss_quorum_manager_config"),
    ],
    "docker/gcp_quorum_manager/gcp_quorum_manager.env.example": [
        ("include/raft/gcp_client_config.hpp", "gcp_client_config"),
        ("include/raft/gcp_compute_quorum_manager.hpp", "gcp_compute_quorum_manager_config"),
        ("include/raft/gcp_compute_quorum_manager.hpp", "gcp_placement_policy_config"),
        ("include/raft/gcp_mig_quorum_manager.hpp", "gcp_mig_quorum_manager_config"),
    ],
    "docker/oci_quorum_manager/oci_quorum_manager.env.example": [
        ("include/raft/oci_client_config.hpp", "oci_client_config"),
        ("include/raft/oci_instance_pool_quorum_manager.hpp",
         "oci_instance_pool_quorum_manager_config"),
        ("include/raft/oci_certificates_provider.hpp", "oci_certificates_provider_config"),
    ],
}


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    # Preprocessor lines (#ifdef around SDK-only fields) are dropped; the
    # fields they guard are still fields.
    return re.sub(r"^\s*#[^\n]*", "", src, flags=re.M)


def struct_fields(header: Path, struct: str) -> list[str]:
    """Data members declared directly in `struct <struct> { ... };`."""
    src = strip_comments(header.read_text())
    m = re.search(r"\bstruct\s+" + re.escape(struct) + r"\s*\{", src)
    if not m:
        raise SystemExit(f"{header.relative_to(ROOT)}: struct {struct} not found")
    i = m.end()
    fields, stmt, depth = [], "", 1
    while depth > 0:
        c = src[i]
        if c == "{":
            if depth == 1 and "(" not in stmt:
                # Brace initialiser: skip it, the statement continues.
                j, d = i + 1, 1
                while d:
                    d += {"{": 1, "}": -1}.get(src[j], 0)
                    j += 1
                i = j
                continue
            if depth == 1:
                # Member-function body: discard the whole declaration.
                j, d = i + 1, 1
                while d:
                    d += {"{": 1, "}": -1}.get(src[j], 0)
                    j += 1
                i, stmt = j, ""
                continue
            depth += 1
        elif c == "}":
            depth -= 1
        elif c == ";" and depth == 1:
            s = stmt.strip()
            if s and "(" not in s and not s.startswith(("using ", "static ", "friend ")):
                s = re.sub(r"=.*$", "", s, flags=re.S).strip()
                name = re.search(r"(\w+)\s*$", s)
                if name:
                    fields.append(name.group(1))
            stmt = ""
        else:
            stmt += c
        i += 1
    return fields


def check(env_rel: str, contracts: list[tuple[str, str]]) -> list[str]:
    text = (ROOT / env_rel).read_text()
    errors = []
    mentioned = set(re.findall(r"\b(\w+)::(\w+)\b", text))
    for header, struct in contracts:
        fields = struct_fields(ROOT / header, struct)
        if not fields:
            errors.append(f"{env_rel}: parsed no fields from {struct} in {header}")
            continue
        for field in fields:
            if (struct, field) not in mentioned:
                errors.append(f"{env_rel}: {struct}::{field} ({header}) is not documented")
        for s, field in sorted(mentioned):
            if s == struct and field not in fields:
                errors.append(f"{env_rel}: mentions {struct}::{field}, "
                              f"which {header} does not declare")
    known = {s for _, s in contracts}
    for s, field in sorted(mentioned):
        if s.endswith("_config") and s not in known:
            errors.append(f"{env_rel}: mentions {s}::{field}, but {s} is not "
                          "listed for this file in scripts/check-cloud-env-examples.py")
    return errors


def main() -> int:
    errors = []
    for env_rel, contracts in CONTRACTS.items():
        errors += check(env_rel, contracts)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if not errors:
        print(f"ok: {len(CONTRACTS)} cloud .env.example files match their config structs")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
