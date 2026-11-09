#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""The IAM conformance driver (.kiro/specs/iam-compatible-auth/ task 23,
Requirements 10.11 and 15.1), run in the conformance stack by
iam_conformance_test as `compose --profile cli run --rm driver`.

It drives Kythira's IAM and STS with boto3 exactly as an AWS user would,
plus the Kythira CA's issuer and the Redis gateways' AUTH, and prints one
line per check:

    PASS <name>
    FAIL <name>: <why>

then exits non-zero if any check failed. It also leaves, in /shared, the
AWS config and credentials files and a web identity token that the test
then hands to the AWS CLI v2 for its own credential-provider checks.

Inputs (environment): IAM_ENDPOINT (the IAM leader's https URL), ROOT_A_ID,
ROOT_A_SECRET (account 111111111111's root key), ROOT_B_ID, ROOT_B_SECRET
(account 222222222222's), REDIS_HOSTS (host:port,...; empty skips the
Redis checks, for a local run without the gateways), CA_AUTH_TOKEN.
"""

import base64
import datetime
import json
import os
import socket
import ssl
import subprocess
import sys
import time
import traceback
import urllib.parse
import urllib.request
import uuid

import boto3
import botocore
from botocore import UNSIGNED
from botocore.config import Config
from botocore.exceptions import ClientError
from botocore.model import ServiceId
from botocore.signers import RequestSigner
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.x509.oid import NameOID

ENDPOINT = os.environ["IAM_ENDPOINT"]
ACCOUNT_A = "111111111111"
ACCOUNT_B = "222222222222"
ROOT_A = (os.environ["ROOT_A_ID"], os.environ["ROOT_A_SECRET"])
ROOT_B = (os.environ["ROOT_B_ID"], os.environ["ROOT_B_SECRET"])
REDIS_HOSTS = [h for h in os.environ.get("REDIS_HOSTS", "").split(",") if h]
CA_URL = "https://ca-1"
CA_TOKEN = os.environ.get("CA_AUTH_TOKEN", "conformance-ca-token")
CA_BUNDLE = "/pki/ca.pem"
TEST_ISSUER = "https://oidc-issuer"
WEB_AUDIENCE = "sts.amazonaws.com"
CACHE = "kythira-cache"
REGION = "us-east-1"
SHARED = "/shared"
ISSUER_WWW = "/srv/www"

failures = []


def check(name):
    """Runs the decorated function as one named check."""
    def run(fn):
        try:
            fn()
            print(f"PASS {name}", flush=True)
        except Exception as e:  # every failure is reported, none stops the run
            detail = "".join(traceback.format_exception_only(type(e), e)).strip()
            print(f"FAIL {name}: {detail}", flush=True)
            traceback.print_exc(file=sys.stderr)
            failures.append(name)
        return fn
    return run


def client(service, key=None, token=None, signed=True):
    cfg = Config(retries={"max_attempts": 2, "mode": "standard"},
                 signature_version=None if signed else UNSIGNED)
    kw = {}
    if key is not None:
        kw = {"aws_access_key_id": key[0], "aws_secret_access_key": key[1]}
        if token is not None:
            kw["aws_session_token"] = token
    return boto3.client(service, endpoint_url=ENDPOINT, region_name=REGION, verify=CA_BUNDLE,
                        config=cfg, **kw)


def creds(c):
    """(id, secret, token) from an STS Credentials element."""
    return c["AccessKeyId"], c["SecretAccessKey"], c["SessionToken"]


def error_code(fn):
    try:
        fn()
    except ClientError as e:
        return e.response["Error"]["Code"]
    return None


def expect_error(code, fn):
    got = error_code(fn)
    if got != code:
        raise AssertionError(f"expected {code}, got {got or 'success'}")


def eventually(fn, within=30.0, every=0.5):
    """Calls fn until it returns truthy or `within` seconds pass."""
    deadline = time.monotonic() + within
    while True:
        try:
            value = fn()
            if value:
                return value
        except Exception:
            if time.monotonic() >= deadline:
                raise
        if time.monotonic() >= deadline:
            raise AssertionError(f"not true within {within:.0f} s")
        time.sleep(every)


def policy(*statements):
    return json.dumps({"Version": "2012-10-17", "Statement": list(statements)})


def allow(action, resource="*"):
    return {"Effect": "Allow", "Action": action, "Resource": resource}


def trust(principal, **condition):
    s = {"Effect": "Allow", "Principal": principal, "Action": "sts:AssumeRole"}
    if condition:
        s["Condition"] = condition
    return policy(s)


# ── JWTs (the test issuer) ───────────────────────────────────────────────────

ISSUER_KEY = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ISSUER_KID = "conformance-1"


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


def jwt(claims):
    header = {"alg": "RS256", "typ": "JWT", "kid": ISSUER_KID}
    signing_input = (b64url(json.dumps(header).encode()) + "." +
                     b64url(json.dumps(claims).encode())).encode()
    sig = ISSUER_KEY.sign(signing_input, padding.PKCS1v15(), hashes.SHA256())
    return signing_input.decode() + "." + b64url(sig)


def web_token(sub="conformance-workload", lifetime=3600, **extra):
    now = int(time.time())
    claims = {"iss": TEST_ISSUER, "sub": sub, "aud": WEB_AUDIENCE, "iat": now, "nbf": now - 5,
              "exp": now + lifetime}
    claims.update(extra)
    return jwt(claims)


def publish_test_issuer():
    numbers = ISSUER_KEY.public_key().public_numbers()
    jwk = {"kty": "RSA", "kid": ISSUER_KID, "use": "sig", "alg": "RS256",
           "n": b64url(numbers.n.to_bytes((numbers.n.bit_length() + 7) // 8, "big")),
           "e": b64url(numbers.e.to_bytes(3, "big"))}
    os.makedirs(f"{ISSUER_WWW}/.well-known", exist_ok=True)
    with open(f"{ISSUER_WWW}/.well-known/openid-configuration", "w") as f:
        json.dump({"issuer": TEST_ISSUER, "jwks_uri": f"{TEST_ISSUER}/jwks",
                   "response_types_supported": ["id_token"],
                   "subject_types_supported": ["public"],
                   "id_token_signing_alg_values_supported": ["RS256"]}, f)
    with open(f"{ISSUER_WWW}/jwks", "w") as f:
        json.dump({"keys": [jwk]}, f)


# ── The Kythira CA ───────────────────────────────────────────────────────────

def ca_request(path, body=None, content_type="application/json", cert=None):
    ctx = ssl.create_default_context(cafile=CA_BUNDLE)
    if cert is not None:
        ctx.load_cert_chain(*cert)
    headers = {} if cert is not None else {"Authorization": f"Bearer {CA_TOKEN}"}
    if body is not None:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(CA_URL + path, data=body, headers=headers,
                                 method="POST" if body is not None else "GET")
    with urllib.request.urlopen(req, context=ctx, timeout=30) as r:
        return r.read().decode()


def kythira_workload_token(uri):
    """A workload certificate for `uri` from ca-1, then the OIDC token ca-1
    issues it over mutual TLS (Requirement 1.6)."""
    key = ec.generate_private_key(ec.SECP256R1())
    csr = (x509.CertificateSigningRequestBuilder()
           .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "conformance")]))
           .sign(key, hashes.SHA256()))
    issued = json.loads(ca_request("/v1/certificates", json.dumps({
        "csr_pem": csr.public_bytes(serialization.Encoding.PEM).decode(),
        "uri_names": [uri], "client_auth": True, "server_auth": False}).encode()))
    cert_path, key_path = f"{SHARED}/wl-cert.pem", f"{SHARED}/wl-key.pem"
    with open(cert_path, "w") as f:
        f.write(issued["chain_pem"])
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    answer = ca_request("/v1/oidc/token", f"audience={WEB_AUDIENCE}".encode(),
                        "application/x-www-form-urlencoded", (cert_path, key_path))
    return json.loads(answer)["token"]


# ── Redis ────────────────────────────────────────────────────────────────────

def resp(host, *args):
    """Sends one RESP command on a fresh connection and returns the reply's
    first line."""
    name, port = host.rsplit(":", 1)
    with socket.create_connection((name, int(port)), timeout=10) as s:
        out = f"*{len(args)}\r\n".encode()
        for a in args:
            b = a.encode()
            out += f"${len(b)}\r\n".encode() + b + b"\r\n"
        s.sendall(out)
        data = b""
        while b"\r\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.split(b"\r\n", 1)[0].decode()


def redis_token(key, user, token=None):
    """An ElastiCache IAM auth token, made the way the ElastiCache
    documentation shows (botocore's presigner)."""
    session = botocore.session.get_session()
    session.set_credentials(key[0], key[1], token)
    signer = RequestSigner(ServiceId("elasticache"), REGION, "elasticache", "v4",
                           session.get_credentials(), session.get_component("event_emitter"))
    url = urllib.parse.urlunparse(("https", CACHE, "/", "",
                                   urllib.parse.urlencode({"Action": "connect", "User": user}),
                                   ""))
    signed = signer.generate_presigned_url(
        {"method": "GET", "url": url, "body": {}, "headers": {}, "context": {}},
        operation_name="connect", expires_in=900, region_name=REGION)
    return signed.removeprefix("https://")


def connect_resources(user="iam-farm"):
    return [f"arn:aws:elasticache:{REGION}:{ACCOUNT_A}:user:{user}",
            f"arn:aws:elasticache:{REGION}:{ACCOUNT_A}:replicationgroup:{CACHE}"]


# ── The checks ───────────────────────────────────────────────────────────────

def main():
    iam_a = client("iam", ROOT_A)
    iam_b = client("iam", ROOT_B)
    state = {}

    @check("create a user, an access key and a policy")
    def _():
        iam_a.create_user(UserName="conf-alice")
        key = iam_a.create_access_key(UserName="conf-alice")["AccessKey"]
        state["alice"] = (key["AccessKeyId"], key["SecretAccessKey"])
        arn = iam_a.create_policy(
            PolicyName="conf-read-users",
            PolicyDocument=policy(allow(["iam:ListUsers", "iam:GetUser"])))["Policy"]["Arn"]
        iam_a.attach_user_policy(UserName="conf-alice", PolicyArn=arn)
        iam_a.put_user_policy(UserName="conf-alice", PolicyName="conf-sts", PolicyDocument=policy(
            allow(["sts:AssumeRole", "sts:GetFederationToken", "sts:DecodeAuthorizationMessage"]),
            allow("elasticache:Connect", connect_resources())))
        eventually(lambda: client("iam", state["alice"]).list_users())

    alice = state.get("alice", ("missing", "missing"))
    iam_alice = client("iam", alice)
    sts_alice = client("sts", alice)
    alice_arn = f"arn:aws:iam::{ACCOUNT_A}:user/conf-alice"

    @check("an allowed call and a denied call")
    def _():
        names = [u["UserName"] for u in iam_alice.list_users()["Users"]]
        assert "conf-alice" in names, names
        expect_error("AccessDenied", lambda: iam_alice.create_user(UserName="conf-mallory"))

    @check("a bad signature")
    def _():
        bad = client("iam", (alice[0], "not-" + alice[1]))
        expect_error("SignatureDoesNotMatch", lambda: bad.list_users())

    @check("cross-account AssumeRole with ExternalId")
    def _():
        iam_b.create_role(RoleName="conf-cross", AssumeRolePolicyDocument=trust(
            {"AWS": alice_arn}, StringEquals={"sts:ExternalId": "conf-external"}))
        arn = f"arn:aws:iam::{ACCOUNT_B}:role/conf-cross"
        expect_error("AccessDenied",
                     lambda: sts_alice.assume_role(RoleArn=arn, RoleSessionName="conf-session"))
        expect_error("AccessDenied", lambda: sts_alice.assume_role(
            RoleArn=arn, RoleSessionName="conf-session", ExternalId="wrong-external"))
        c = sts_alice.assume_role(RoleArn=arn, RoleSessionName="conf-session",
                                  ExternalId="conf-external")["Credentials"]
        k = creds(c)
        who = client("sts", k[:2], k[2]).get_caller_identity()
        assert who["Arn"] == \
            f"arn:aws:sts::{ACCOUNT_B}:assumed-role/conf-cross/conf-session", who
        assert who["Account"] == ACCOUNT_B, who

    @check("GetSessionToken")
    def _():
        c = sts_alice.get_session_token(DurationSeconds=900)["Credentials"]
        k = creds(c)
        assert k[0].startswith("ASIA"), k[0]
        who = client("sts", k[:2], k[2]).get_caller_identity()
        assert who["Arn"] == alice_arn, who
        # Requirement 10.10: no IAM without MFA, which no session has.
        expect_error("AccessDenied", lambda: client("iam", k[:2], k[2]).list_users())

    @check("GetFederationToken without a session policy")
    def _():
        r = sts_alice.get_federation_token(Name="conf-fed", DurationSeconds=900)
        assert r["FederatedUser"]["Arn"] == \
            f"arn:aws:sts::{ACCOUNT_A}:federated-user/conf-fed", r["FederatedUser"]
        k = creds(r["Credentials"])
        who = client("sts", k[:2], k[2]).get_caller_identity()
        assert who["Arn"] == r["FederatedUser"]["Arn"], who
        state["fed_bare"] = k
        expect_error("AccessDenied", lambda: client("iam", k[:2], k[2]).list_users())

    @check("GetFederationToken with a session policy")
    def _():
        r = sts_alice.get_federation_token(
            Name="conf-fed", DurationSeconds=900,
            Policy=policy(allow("elasticache:Connect", connect_resources())))
        state["fed_scoped"] = creds(r["Credentials"])
        assert "PackedPolicySize" in r, r

    @check("DecodeAuthorizationMessage")
    def _():
        # No service in this stack puts an encoded message in its errors
        # (Kythira's IAM API does not, matching AWS IAM), so this checks the
        # action's own answers: a message it did not seal is refused, and
        # a caller without sts:DecodeAuthorizationMessage is denied.
        expect_error("InvalidAuthorizationMessageException",
                     lambda: sts_alice.decode_authorization_message(
                         EncodedMessage="not-an-encoded-message"))
        iam_a.create_user(UserName="conf-bob")
        bob = iam_a.create_access_key(UserName="conf-bob")["AccessKey"]
        sts_bob = client("sts", (bob["AccessKeyId"], bob["SecretAccessKey"]))
        expect_error("AccessDenied", lambda: sts_bob.decode_authorization_message(
            EncodedMessage="not-an-encoded-message"))

    @check("AssumeRoleWithWebIdentity with a test issuer")
    def _():
        publish_test_issuer()
        provider = iam_a.create_open_id_connect_provider(
            Url=TEST_ISSUER, ClientIDList=[WEB_AUDIENCE],
            ThumbprintList=["0" * 40])["OpenIDConnectProviderArn"]
        assert provider == f"arn:aws:iam::{ACCOUNT_A}:oidc-provider/oidc-issuer", provider
        iam_a.create_role(RoleName="conf-web", AssumeRolePolicyDocument=policy({
            "Effect": "Allow", "Principal": {"Federated": provider},
            "Action": "sts:AssumeRoleWithWebIdentity",
            "Condition": {"StringEquals": {"oidc-issuer:aud": WEB_AUDIENCE,
                                           "oidc-issuer:sub": "conformance-workload"}}}))
        unsigned = client("sts", signed=False)
        r = unsigned.assume_role_with_web_identity(
            RoleArn=f"arn:aws:iam::{ACCOUNT_A}:role/conf-web", RoleSessionName="web",
            WebIdentityToken=web_token())
        assert r["AssumedRoleUser"]["Arn"] == \
            f"arn:aws:sts::{ACCOUNT_A}:assumed-role/conf-web/web", r["AssumedRoleUser"]
        expect_error("AccessDenied", lambda: unsigned.assume_role_with_web_identity(
            RoleArn=f"arn:aws:iam::{ACCOUNT_A}:role/conf-web", RoleSessionName="web",
            WebIdentityToken=web_token(sub="someone-else")))

    @check("an expired token")
    def _():
        # Past the 5 minutes of clock skew STS allows, as AWS does.
        unsigned = client("sts", signed=False)
        long_ago = int(time.time()) - 3600
        expect_error("ExpiredTokenException", lambda: unsigned.assume_role_with_web_identity(
            RoleArn=f"arn:aws:iam::{ACCOUNT_A}:role/conf-web", RoleSessionName="web",
            WebIdentityToken=web_token(lifetime=-600, iat=long_ago, nbf=long_ago)))

    @check("AssumeRoleWithWebIdentity with the Kythira issuer")
    def _():
        provider = iam_a.create_open_id_connect_provider(
            Url="https://ca-1", ClientIDList=[WEB_AUDIENCE],
            ThumbprintList=["0" * 40])["OpenIDConnectProviderArn"]
        uri = "spiffe://kythira.test/wl/conformance/api"
        iam_a.create_role(RoleName="conf-kythira", AssumeRolePolicyDocument=policy({
            "Effect": "Allow", "Principal": {"Federated": provider},
            "Action": "sts:AssumeRoleWithWebIdentity",
            "Condition": {"StringEquals": {"ca-1:aud": WEB_AUDIENCE,
                                           "ca-1:sub": f"workload:{uri}"}}}))
        token = eventually(lambda: kythira_workload_token(uri), within=60)
        r = client("sts", signed=False).assume_role_with_web_identity(
            RoleArn=f"arn:aws:iam::{ACCOUNT_A}:role/conf-kythira", RoleSessionName="kythira",
            WebIdentityToken=token)
        assert r["AssumedRoleUser"]["Arn"] == \
            f"arn:aws:sts::{ACCOUNT_A}:assumed-role/conf-kythira/kythira", r["AssumedRoleUser"]

    # Requirement 10.11: the SDK's own providers, configured as a user
    # configures them, with STS's endpoint in the standard variable.
    @check("credential providers: a two-role source_profile chain")
    def _():
        iam_a.create_role(RoleName="conf-a", AssumeRolePolicyDocument=trust({"AWS": alice_arn}))
        iam_a.put_role_policy(RoleName="conf-a", PolicyName="next", PolicyDocument=policy(
            allow("sts:AssumeRole", f"arn:aws:iam::{ACCOUNT_A}:role/conf-b")))
        iam_a.create_role(RoleName="conf-b", AssumeRolePolicyDocument=trust(
            {"AWS": f"arn:aws:iam::{ACCOUNT_A}:role/conf-a"}))
        with open(f"{SHARED}/web-token", "w") as f:
            f.write(web_token(lifetime=3600))
        with open(f"{SHARED}/credentials", "w") as f:
            f.write(f"[conf-alice]\naws_access_key_id = {alice[0]}\n"
                    f"aws_secret_access_key = {alice[1]}\n")
        with open(f"{SHARED}/config", "w") as f:
            f.write(f"[profile conf-role-a]\nrole_arn = arn:aws:iam::{ACCOUNT_A}:role/conf-a\n"
                    "source_profile = conf-alice\nregion = us-east-1\n\n"
                    f"[profile conf-role-b]\nrole_arn = arn:aws:iam::{ACCOUNT_A}:role/conf-b\n"
                    "source_profile = conf-role-a\nregion = us-east-1\n\n"
                    f"[profile conf-web]\nrole_arn = arn:aws:iam::{ACCOUNT_A}:role/conf-web\n"
                    f"web_identity_token_file = {SHARED}/web-token\n"
                    "role_session_name = web\nregion = us-east-1\n")
        out = provider_child({"AWS_PROFILE": "conf-role-b"})
        assert out.startswith(f"arn:aws:sts::{ACCOUNT_A}:assumed-role/conf-b/"), out

    @check("credential providers: web_identity_token_file")
    def _():
        out = provider_child({"AWS_PROFILE": "conf-web"})
        assert out == f"arn:aws:sts::{ACCOUNT_A}:assumed-role/conf-web/web", out

    @check("credential providers: AWS_ROLE_ARN and AWS_WEB_IDENTITY_TOKEN_FILE")
    def _():
        out = provider_child({"AWS_ROLE_ARN": f"arn:aws:iam::{ACCOUNT_A}:role/conf-web",
                              "AWS_WEB_IDENTITY_TOKEN_FILE": f"{SHARED}/web-token",
                              "AWS_ROLE_SESSION_NAME": "env-pair"},
                             files=False)
        assert out == f"arn:aws:sts::{ACCOUNT_A}:assumed-role/conf-web/env-pair", out

    if REDIS_HOSTS:
        redis_checks(alice, state)
    else:
        print("SKIP redis: REDIS_HOSTS is empty", flush=True)

    print(f"{len(failures)} failed" if failures else "all passed", flush=True)
    return 1 if failures else 0


def provider_child(env, files=True):
    """GetCallerIdentity's Arn from a fresh process whose only credentials
    are what `env` and the shared files configure: the providers resolve
    them, as in any user's shell."""
    child = {k: v for k, v in os.environ.items()
             if not k.startswith("AWS_") or k in ("AWS_CA_BUNDLE", "AWS_DEFAULT_REGION",
                                                  "AWS_EC2_METADATA_DISABLED")}
    child["AWS_ENDPOINT_URL_STS"] = ENDPOINT
    if files:
        child["AWS_CONFIG_FILE"] = f"{SHARED}/config"
        child["AWS_SHARED_CREDENTIALS_FILE"] = f"{SHARED}/credentials"
    else:
        child["AWS_CONFIG_FILE"] = "/dev/null"
        child["AWS_SHARED_CREDENTIALS_FILE"] = "/dev/null"
    child.update(env)
    code = "import boto3; print(boto3.client('sts').get_caller_identity()['Arn'])"
    r = subprocess.run([sys.executable, "-c", code], env=child, capture_output=True, text=True,
                       timeout=60)
    if r.returncode != 0:
        raise AssertionError(r.stderr.strip().splitlines()[-1] if r.stderr else "no output")
    return r.stdout.strip()


def redis_checks(alice, state):
    @check("Redis AUTH with a password")
    def _():
        for host in REDIS_HOSTS:
            reply = eventually(lambda: resp(host, "AUTH", "farm", "ci-main-secret") == "+OK",
                               within=60)
            assert reply, host
            assert resp(host, "AUTH", "farm", "wrong") .startswith("-WRONGPASS"), host
            assert resp(host, "AUTH", "iam-farm", "disabled").startswith("-WRONGPASS"), host

    @check("Redis AUTH with an IAM token")
    def _():
        for host in REDIS_HOSTS:
            eventually(lambda: resp(host, "AUTH", "iam-farm",
                                    redis_token(alice, "iam-farm")) == "+OK", within=60)
            # The token names its user.
            assert resp(host, "AUTH", "iam-farm",
                        redis_token(alice, "farm")).startswith("-WRONGPASS"), host

    @check("Redis AUTH with federated credentials follows the session policy")
    def _():
        bare, scoped = state["fed_bare"], state["fed_scoped"]
        for host in REDIS_HOSTS:
            assert resp(host, "AUTH", "iam-farm", redis_token(
                bare[:2], "iam-farm", bare[2])).startswith("-WRONGPASS"), host
            assert resp(host, "AUTH", "iam-farm", redis_token(
                scoped[:2], "iam-farm", scoped[2])) == "+OK", host

    @check("policy-change propagation to the gateways' replicas")
    def _():
        iam_a = client("iam", ROOT_A)
        iam_a.delete_user_policy(UserName="conf-alice", PolicyName="conf-sts")
        for host in REDIS_HOSTS:
            eventually(lambda: resp(host, "AUTH", "iam-farm",
                                    redis_token(alice, "iam-farm")).startswith("-WRONGPASS"),
                       within=30)
        iam_a.put_user_policy(UserName="conf-alice", PolicyName="conf-sts", PolicyDocument=policy(
            allow("elasticache:Connect", connect_resources())))
        for host in REDIS_HOSTS:
            eventually(lambda: resp(host, "AUTH", "iam-farm",
                                    redis_token(alice, "iam-farm")) == "+OK", within=30)


if __name__ == "__main__":
    sys.exit(main())
