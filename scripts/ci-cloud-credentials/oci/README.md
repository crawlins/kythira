# OCI real-cloud test credentials and tenancy setup

Setup for Kythira's OCI components against a real tenancy
(`.kiro/specs/oci-cloud-provider/` Requirement 14.3). Sibling of
[`../aws/`](../aws/README.md) and [`../azure/`](../azure/README.md), with two
deliberate differences called out where they occur: OCI policies are statements
rather than JSON documents, and **this directory issues no credentials** — see
[Credentials](#credentials).

## Contents

| | |
|---|---|
| [`oci_tenancy_check.cpp`](oci_tenancy_check.cpp) | Progressive validator — build it and run it before anything else |
| [`provision-ci-identity.sh`](provision-ci-identity.sh) | Creates the IAM group and policy for the given bundles |
| [`policies/`](policies/) | One policy fragment per test bundle |

## Read this first: run the checker before you build anything

`oci_tenancy_check` makes one authenticated, read-only call and tells you
whether the foundation works. **Stage 1 launches nothing and costs nothing.**

```bash
cmake --build build-default --target oci_tenancy_check
./build-default/tests/oci_tenancy_check
```

It is not a formality. The first time anything in this project made a real
signed request to OCI, it failed — and getting from there to a working
provision surfaced **four defects that the entire mock test suite could not
see**:

1. The endpoint domain was missing its `oci` label, so two of the four service
   hostnames **did not resolve at all**.
2. cpp-httplib appended `:443` to `Host`, a *signed* header, so every request
   failed signature verification.
3. Every request with a body carried a **duplicate `Content-Type`**, which OCI
   answers with a bare `400`.
4. `provision_node` tagged the new instance before OCI would accept the write,
   and read its VNIC before OCI would answer for it.

None of these were reachable locally: `endpoint_override` replaces the whole
host, a mock necessarily binds a non-default port, cpp-httplib's *server*
tolerates duplicate headers, and a mock materialises instances already
`RUNNING`. Coverage was irrelevant to all four. See `spike-notes.md`
Findings 5-8.

### The stages

Each is skipped when its env var is unset, so the same binary is useful at
every point of the setup below.

| Stage | Needs | Proves |
|---|---|---|
| 1. `ListRegions` | credentials only | signing, host derivation, error unwrapping |
| 2. `ListInstances` | `..._COMPARTMENT_ID` | the compartment and read permission |
| 3. `GetInstancePool` | `..._INSTANCE_POOL_ID` | exactly what the manager's constructor does, and that the pool has **one** placement config |
| 4. `ListInstancePoolInstances` | both of the above | the listing `assess_quorum` drives |
| 5. `GetCertificateAuthorityBundle` | `..._CERTIFICATE_AUTHORITY_ID` | the *other* service host |
| 6. full lifecycle | `KYTHIRA_OCI_ALLOW_LAUNCH=1` | **launches a billable instance**: provision → assess → decommission |

Stage 6 is gated twice over because it is the only one that costs money. It
cleans up whatever happens, including on failure.

On failure the checker prints the canonical string it signed — the one thing a
401 never contains.

### Environment

```bash
export KYTHIRA_OCI_REGION=us-phoenix-1
export KYTHIRA_OCI_TENANCY_ID=ocid1.tenancy.oc1..…
export KYTHIRA_OCI_USER_ID=ocid1.user.oc1..…
export KYTHIRA_OCI_FINGERPRINT=aa:bb:…
export KYTHIRA_OCI_PRIVATE_KEY_PEM="$(cat ~/.oci/oci_api_key.pem)"
# optional, per stage:
export KYTHIRA_OCI_COMPARTMENT_ID=…
export KYTHIRA_OCI_INSTANCE_POOL_ID=…
export KYTHIRA_OCI_CERTIFICATE_AUTHORITY_ID=…
```

If you already have a working OCI CLI config, take them from it rather than
retyping:

```bash
eval "$(python3 - <<'PY'
import configparser, os, shlex
c = configparser.ConfigParser(); c.read(os.path.expanduser("~/.oci/config"))
s = c["DEFAULT"]
out = {"KYTHIRA_OCI_REGION": s["region"], "KYTHIRA_OCI_TENANCY_ID": s["tenancy"],
       "KYTHIRA_OCI_USER_ID": s["user"], "KYTHIRA_OCI_FINGERPRINT": s["fingerprint"],
       "KYTHIRA_OCI_PRIVATE_KEY_PEM": open(os.path.expanduser(s["key_file"])).read()}
for k, v in out.items(): print(f"export {k}={shlex.quote(v)}")
PY
)"
```

## Bundles and toggles

A bundle is one group of permissions mapped 1:1 to one real-cloud CTest binary,
so enabling one never grants blast radius for another.

| Bundle | CTest binary | Toggle |
|---|---|---|
| `instance-pool` | `oci_quorum_manager_real_test` | `REAL_CLOUD_TESTS_OCI_INSTANCE_POOL_ENABLED` |
| `certificates` | `oci_certificates_provider_real_test` | `REAL_CLOUD_TESTS_OCI_CERTIFICATES_ENABLED` |

Both sit under `REAL_CLOUD_TESTS_OCI_ENABLED` and the master
`REAL_CLOUD_TESTS_ENABLED`, per [`../README.md`](../README.md)'s three-level
model. **Neither test binary exists yet** — Task 6 is open; the bundles are
defined here so the policy and toggles are ready when it lands.

## Tenancy setup

### 1. Compartment

```bash
COMPARTMENT_ID=$(oci iam compartment create --compartment-id "$TENANCY_OCID" \
  --name kythira-ci --description "kythira real-cloud tests" \
  --wait-for-state ACTIVE --query 'data.id' --raw-output)
```

Everything else goes inside it: one IAM scope, one blast radius, one place to
audit for leaks.

### 2. Group and policy

```bash
./provision-ci-identity.sh --compartment-id "$COMPARTMENT_ID" \
    --bundles instance-pool,certificates --dry-run   # inspect first
./provision-ci-identity.sh --compartment-id "$COMPARTMENT_ID" \
    --bundles instance-pool,certificates
```

Then **run stage 1 of the checker**, before creating anything billable.

### 3. VCN and subnet

Check your ADs first — many OCI regions have only one, which limits how much
per-AD behaviour you can exercise:

```bash
oci iam availability-domain list --compartment-id "$COMPARTMENT_ID" --query 'data[].name'
```

Then a VCN and **one regional subnet**. Regional (no `--availability-domain`)
is enough for every pool: a placement configuration takes
`{availabilityDomain, primarySubnetId}`, so one subnet serves all ADs.
Confirmed working — instances launched into a regional subnet from an AD-scoped
placement config and received addresses from its range.

```bash
VCN_ID=$(oci network vcn create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-vcn --cidr-blocks '["10.0.0.0/16"]' --dns-label kythiraci \
  --wait-for-state AVAILABLE --query 'data.id' --raw-output)
SL_ID=$(oci network vcn get --vcn-id "$VCN_ID" --query 'data."default-security-list-id"' --raw-output)
SUBNET_ID=$(oci network subnet create --compartment-id "$COMPARTMENT_ID" --vcn-id "$VCN_ID" \
  --display-name kythira-ci-subnet --cidr-block 10.0.1.0/24 \
  --prohibit-public-ip-on-vnic true --security-list-ids "[\"$SL_ID\"]" --dns-label nodes \
  --wait-for-state AVAILABLE --query 'data.id' --raw-output)
```

**No gateways.** With `heartbeat_timeout = 0` the instances make no outbound
call at all — the manager talks to OCI's control plane from wherever *it* runs,
not from the instance. So no NAT Gateway (which bills hourly for as long as it
exists), no Service Gateway, no Internet Gateway. Add a **Service** Gateway
only if you later want nodes writing their own `kythira-last-heartbeat` tag
from inside the instance; that is the case a NAT Gateway is *not* the right
answer to.

The default security list is fine. Nothing in these tests connects *to* the
instances — the manager only calls the REST API.

### 4. Instance Configuration

Shape and image live here, which is why
`oci_instance_pool_quorum_manager_config` has no `image_id` or `shape` field.

```bash
cat > /tmp/ic.json <<JSON
{
  "instanceType": "compute",
  "launchDetails": {
    "compartmentId": "$COMPARTMENT_ID",
    "displayName": "kythira-node",
    "shape": "VM.Standard.E2.1",
    "sourceDetails": { "sourceType": "image", "imageId": "$IMAGE_ID" },
    "createVnicDetails": { "subnetId": "$SUBNET_ID", "assignPublicIp": false }
  }
}
JSON
IC_ID=$(oci compute-management instance-configuration create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-node-config --instance-details file:///tmp/ic.json \
  --query 'data.id' --raw-output)
```

Two deliberate omissions. **No `availabilityDomain`** — the pool's placement
configuration supplies it, and two sources of truth for placement means the
losing one is what you read when an instance lands in the wrong AD. **No
`shapeConfig`** — `E2.1` is a fixed shape; any `*.Flex` shape requires
`"shapeConfig": {"ocpus": 1, "memoryInGBs": 6}` and is rejected without it.

On shape choice: Ampere (`VM.Standard.A1.Flex`) is the Always-Free-eligible
family and much cheaper, but is frequently out of capacity. Prove the loop on
something that launches first. A stockout is not wasted, though — the exact
error OCI returns is what Task 0(g) needs recorded, and Requirement 13.14 says
outright it cannot be written from inspection.

No cloud-init, no `kythira-node` binary, no Object Storage bucket: a stock
Oracle Linux image is enough, because with `heartbeat_timeout = 0` the tests
only observe lifecycle state. The cost is that the heartbeat-freshness path
stays mock-only.

### 5. Instance Pool — size 0, exactly one placement configuration

```bash
oci compute-management instance-pool create --compartment-id "$COMPARTMENT_ID" \
  --instance-configuration-id "$IC_ID" --size 0 \
  --placement-configurations "[{\"availabilityDomain\":\"$AD\",\"primarySubnetId\":\"$SUBNET_ID\"}]" \
  --display-name kythira-ci-pool-ad1 --wait-for-state RUNNING
```

**One placement configuration per pool** is the constraint the whole design
rests on — `UpdateInstancePool` takes one pool-wide `size` and OCI chooses
placement, so a multi-AD pool cannot honour `provision_node(target_group)`. A
three-AD cluster is three pools and three manager instances. Stage 3 of the
checker asserts the count.

**Size 0**: start non-zero and the pool launches untagged instances before a
manager exists to tag them, which the first `provision_node` then adopts
instead of the instance it just paid for.

Now run stages 3-4, then stage 6.

### 6. Certificate Authority (only for the `certificates` bundle)

A CA requires a Vault and a master encryption key — a dependency that is not
obvious from the CA documentation, and **the one part of this setup that bills
while idle**. If the certificates bundle is not worth a standing charge, enable
only `instance-pool`; the per-bundle toggle exists for exactly this.

Three things to get right, each of which costs a full create cycle to discover:

```bash
# 1. DEFAULT, not VIRTUAL_PRIVATE. A virtual private vault is a dedicated HSM
#    partition and is *substantially* more expensive; nothing here needs one.
VAULT_ID=$(oci kms management vault create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-vault --vault-type DEFAULT \
  --wait-for-state ACTIVE --query 'data.id' --raw-output)
EP=$(oci kms management vault get --vault-id "$VAULT_ID" \
  --query 'data."management-endpoint"' --raw-output)

# 2. The key must be RSA. A CA signs, so a symmetric key is not usable — an AES
#    key is accepted by `key create` and then rejected by the CA with
#    `InvalidParameter: The encryption key with the OCID ... has an invalid
#    shape.`, which does not mention the algorithm. `length` is in BYTES:
#    256 = RSA-2048.
KEY_ID=$(oci kms management key create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ci-ca-key --key-shape '{"algorithm":"RSA","length":256}' \
  --endpoint "$EP" --wait-for-state ENABLED --query 'data.id' --raw-output)

# 3. The subcommand ends in `-details`. Without it the CLI suggests the right
#    name, but only after the vault and key already exist.
oci certs-mgmt certificate-authority create-root-ca-by-generating-config-details \
  --compartment-id "$COMPARTMENT_ID" --name kythira-ci-ca --kms-key-id "$KEY_ID" \
  --subject '{"commonName": "kythira ci root"}' \
  --wait-for-state ACTIVE --max-wait-seconds 1200
```

Vault and CA creation are both slow — minutes each, not seconds.

`oci_certificates_provider` issues with
`configType = MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`: the caller generates
the key pair and submits **only the CSR**, so the private key never reaches
OCI. OCI's *default* flow does the opposite and deposits the key in Vault —
worth knowing if you create certificates through the console and wonder why.

### 7. A budget alert

Set an OCI Budget on the compartment before the first launch. Every provider in
this project leaked something on its first live run; a budget alert is the
cheapest version of the audit that catches it.

## Credentials

**Preferred (Requirement 14.2): federated, no long-lived secret.** Blocked on
Task 0(f) — whether OCI federates GitHub Actions' OIDC tokens to a Dynamic
Group for this shape is an open spike question. `provision-ci-identity.sh`
deliberately stops short of issuing anything rather than quietly doing the
weaker thing.

**Fallback: a long-lived API key as a CI secret.** Requirement 14.2 permits it
only with "explicit sign-off ... not a silent regression from the AWS job's
stronger posture", recorded in `spike-notes.md`. If you take this path, scope
the user to the group above and nothing else, and rotate on your tenancy's
schedule — `oci_client_config::private_key_pem` takes the key as a string, so
rotation is a config reload rather than a rebuild.

**Instance Principal is not implemented** (`oci_client_config::
use_instance_principal`). Setting it throws a named error rather than silently
falling back to the API key, which would authenticate as the wrong principal.
What is missing is a contract — the metadata-service endpoints and refresh
cadence — not code; see `spike-notes.md` Task 0(b).

## Operational notes from real runs

Things that cost time to learn and are not in Oracle's documentation in this
form:

- **Pool scale-down terminates with a lag.** `UpdateInstancePool(size=0)`
  returns, and the instance is still `RUNNING` for a minute or two. Do not
  conclude you have leaked one until you have watched it for a few minutes.
- **`ListInstancePoolInstances` and `GetInstance` disagree on case.** The
  listing reports `state: "Provisioning"`/`"Running"`; `GetInstance` reports
  `lifecycleState: "PROVISIONING"`/`"RUNNING"`. The manager reads the latter.
- **A new instance is not taggable immediately.** `UpdateInstance` against one
  the pool is still building answers `409 Conflict: instance ... is currently
  being modified, try again later`.
- **`GetVnic` answers `404 NotAuthorizedOrNotFound` before the VNIC is
  visible** — the same status and code as a missing permission, so a transient
  race reads exactly like a broken policy. Check whether the CLI can read the
  same VNIC with the same credentials before touching IAM.
- **Boot volumes are released on terminate.** Verified after a full lifecycle:
  both volumes went `TERMINATED` with their instances. Azure's equivalent
  orphaned a permanently-billing disk on every VM, so this is worth confirming
  yourself after your first run rather than assuming.
