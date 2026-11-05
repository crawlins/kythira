#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Verifies a Kythira OIDC token with PyJWT, a library Kythira does not share
code with (`.kiro/specs/iam-compatible-auth/` task 2).

    oidc_pyjwt_verify.py <token file> <jwks file> <issuer> <audience>

Picks the JWKS key by the token's kid, then checks signature, iss, aud, exp,
nbf and iat the way a relying party would. Exit codes: 0 verified, 1 refused,
77 PyJWT (with its cryptography backend) is not installed, so the caller can
report the check as skipped.
"""

import json
import sys

try:
    import jwt
    from jwt import PyJWK
    import cryptography  # noqa: F401  (RS256/ES256 need it)
except ImportError as exc:
    print(f"PyJWT unavailable: {exc}", file=sys.stderr)
    sys.exit(77)


def main() -> int:
    token_path, jwks_path, issuer, audience = sys.argv[1:5]
    with open(token_path, encoding="ascii") as f:
        token = f.read().strip()
    with open(jwks_path, encoding="utf-8") as f:
        jwks = json.load(f)

    header = jwt.get_unverified_header(token)
    matches = [k for k in jwks["keys"] if k.get("kid") == header.get("kid")]
    if len(matches) != 1:
        print(f"no single JWKS key for kid {header.get('kid')!r}", file=sys.stderr)
        return 1
    key = PyJWK(matches[0])
    try:
        claims = jwt.decode(
            token,
            key=key.key,
            algorithms=[matches[0]["alg"]],
            audience=audience,
            issuer=issuer,
            options={"require": ["iss", "sub", "aud", "iat", "nbf", "exp", "jti"]},
        )
    except jwt.PyJWTError as exc:
        print(f"PyJWT refused the token: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(claims, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
