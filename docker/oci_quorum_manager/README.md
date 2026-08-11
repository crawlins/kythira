# Running kythira's OCI quorum manager and certificate provider

Setup for `kythira::oci_instance_pool_quorum_manager` and
`raft::testing::oci_certificates_provider`
(`.kiro/specs/oci-cloud-provider/`, Requirement 15.2). The companion
[`oci_quorum_manager.env.example`](oci_quorum_manager.env.example) lists every
configuration field; this file is about the OCI resources those fields point
at, all of which the library deliberately does **not** create.

## What kythira does and does not create

| Resource | Created by |
|---|---|
| Compartment, VCN, subnet | **you** (§1) |
| Instance Configuration | **you** (§2) |
| Instance Pool — **one per Availability Domain** | **you** (§2) |
| Certificate Authority | **you** (§5) |
| Compute instances inside the pool | kythira (`provision_node` grows the pool) |
| `kythira-*` freeform tags on those instances | kythira |
| Leaf certificates | kythira (`sign_csr`) |

The split is the same one `aws_acm_pca_provider` draws around the ACM Private
CA and `aws_asg_quorum_manager` draws around the ASG's launch template: the
expensive, long-lived, policy-bearing resources are an operator decision, and
the thing that scales up and down at 3am is not.

## 0. The constraint to read before anything else: one pool per AD

`UpdateInstancePool` takes a single pool-wide `size`. OCI's own placement
algorithm decides which of the pool's placement configurations absorbs new
capacity, and **no OCI API accepts a per-AD instance count** — confirmed
against `terraform-provider-oci`'s `oci_core_instance_pool` resource
(`spike-notes.md`, Finding 2). This is the one place OCI differs materially
from an AWS Auto Scaling Group's per-AZ subnet mapping.

A manager pointed at a multi-AD pool therefore *cannot* implement
`provision_node(target_group)`: it could only grow the pool and hope. So a
three-AD cluster is three Instance Pools, each with exactly one placement
configuration, and three `oci_instance_pool_quorum_manager` instances, each
configured with its own pool and its own single-entry `topology`.

The manager enforces its half — `provision_node` rejects a `target_group` its
pool has no placement configuration for, rather than provisioning into the
wrong failure domain. It cannot enforce yours: if you give a pool two ADs, it
will accept either as a `target_group` and OCI will place wherever it likes.

## 1. Compartment, VCN and subnet

```bash
COMPARTMENT_ID=$(oci iam compartment create \
  --compartment-id "$TENANCY_OCID" --name kythira --description "kythira cluster" \
  --query 'data.id' --raw-output)

VCN_ID=$(oci network vcn create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-vcn --cidr-blocks '["10.0.0.0/16"]' \
  --query 'data.id' --raw-output)
```

Create one **private** subnet per Availability Domain you intend to run in.
Nodes need no public IP: `provision_node` reads the primary VNIC's `privateIp`
and returns `{private_ip}:{node_port}`, so the address the cluster gossips is
reachable only inside the VCN. Give the subnet a Service Gateway (for OCI
service endpoints) and, if the node image pulls anything from the internet, a
NAT Gateway.

Ports to open in the subnet's security list or an NSG: the Raft RPC port
(`OCI_NODE_PORT`, 7000 by default) between the nodes themselves, and nothing
inbound from outside the VCN.

## 2. Instance Configuration and Instance Pools

The Instance Configuration is the template — shape, image, VNIC, metadata,
cloud-init. It is where every "what to launch" decision lives, which is why
`oci_instance_pool_quorum_manager_config` has no `image_id`, `shape`,
`subnet_id` or `user_data` field.

```bash
oci compute-management instance-configuration create \
  --compartment-id "$COMPARTMENT_ID" \
  --instance-details file://instance-details.json \
  --display-name kythira-node
```

Then one pool per AD, each with a **single** placement configuration:

```bash
oci compute-management instance-pool create \
  --compartment-id "$COMPARTMENT_ID" \
  --instance-configuration-id "$INSTANCE_CONFIG_ID" \
  --size 0 \
  --placement-configurations '[{
      "availabilityDomain": "kIdk:PHX-AD-1",
      "primarySubnetId": "'"$SUBNET_AD1_ID"'"
  }]' \
  --display-name kythira-pool-ad1
```

Start at `--size 0` and let `maintain_quorum` grow each pool to its
`topology` target. Starting non-zero means the pool launches instances before
the manager exists to tag them; those instances have no `kythira-node-id`
tag, so the first `provision_node` call adopts one of them instead of the
instance it just paid for — harmless, but confusing to watch.

## 3. IAM policy

The manager needs, in the compartment holding the pools:

```
Allow group kythira-operators to manage instance-pools in compartment kythira
Allow group kythira-operators to manage instance-family in compartment kythira
Allow group kythira-operators to read virtual-network-family in compartment kythira
```

`manage instance-family` is what covers `UpdateInstance` — the tag write —
and `read virtual-network-family` covers `ListVnicAttachments`/`GetVnic`,
which is how a node's private IP is discovered. A deployment that grants
`manage instance-pools` alone provisions successfully and then fails at the
tagging step, leaving an untagged instance in the pool that the next
`provision_node` call adopts.

For the certificate provider, in the compartment holding the CA:

```
Allow group kythira-operators to manage leaf-certificate-family in compartment kythira
Allow group kythira-operators to read certificate-authority-family in compartment kythira
```

## 4. Choosing between API-key and Instance Principal auth

**Use API-key auth today.** Instance Principal is defined in the config
surface (`oci_client_config::use_instance_principal`) but is **not
implemented**: setting it makes every signing attempt throw a named error.

That is deliberate, and the alternative was worse. What is missing is not
code but a *contract* — the instance metadata service endpoints that hand out
the short-lived signing certificate, and the cadence at which they must be
refreshed before expiry (`spike-notes.md`, Task 0(b)). Implementing it from a
guess would produce a client that passes against a mock built from the same
guess and fails against OCI. A silent fallback to the API-key path would be
worse still: it would authenticate as the wrong principal, which is an
authorization decision made by accident.

So, until that is closed:

- **Control-plane processes** (whatever calls `maintain_quorum`) use an API
  key belonging to a dedicated IAM user, with the policy above and nothing
  else. Rotate it on whatever schedule your tenancy mandates; the key is read
  from `oci_client_config::private_key_pem` as a string, so a rotation is a
  config reload, not a restart.
- **The nodes themselves** write their own `kythira-last-heartbeat` tag.
  Requirement 4.4 has that done by the kythira process from inside the
  instance, which is exactly the case Instance Principal exists for. Until it
  lands, either give the node image its own scoped API key, or run with
  `OCI_HEARTBEAT_TIMEOUT_SECONDS=0` and accept `lifecycleState`-only
  liveness — which detects a stopped or terminated VM but not a wedged
  kythira process on a healthy one.

## 5. Certificate Authority

The CA needs a Vault and an **RSA** master key first. Three things bite here:
the vault type (`DEFAULT`, not the much pricier `VIRTUAL_PRIVATE`), the key
algorithm (an AES key is accepted by `key create` and then rejected by the CA
with `InvalidParameter: ... has an invalid shape.`, which never mentions the
algorithm), and the subcommand name (it ends in `-details`).

```bash
VAULT_ID=$(oci kms management vault create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-vault --vault-type DEFAULT \
  --wait-for-state ACTIVE --query 'data.id' --raw-output)
EP=$(oci kms management vault get --vault-id "$VAULT_ID" \
  --query 'data."management-endpoint"' --raw-output)

# length is in BYTES: 256 = RSA-2048.
KEY_ID=$(oci kms management key create --compartment-id "$COMPARTMENT_ID" \
  --display-name kythira-ca-key --key-shape '{"algorithm":"RSA","length":256}' \
  --endpoint "$EP" --wait-for-state ENABLED --query 'data.id' --raw-output)

oci certs-mgmt certificate-authority create-root-ca-by-generating-config-details \
  --compartment-id "$COMPARTMENT_ID" --name kythira-ca --kms-key-id "$KEY_ID" \
  --subject '{"commonName": "kythira root"}' \
  --wait-for-state ACTIVE --max-wait-seconds 1200
```

`oci_certificates_provider` issues from this CA with
`configType = MANAGED_EXTERNALLY_ISSUED_BY_INTERNAL_CA`: the caller generates
the key pair and CSR locally and submits **only the CSR**. The private key
never reaches OCI, and `sign_csr` returns `pem_material` with
`private_key_pem` empty — the same contract every other `certificate_provider`
in this project honours.

This is worth stating because OCI's *default* issuance flow
(`ISSUED_BY_INTERNAL_CA`) does the opposite: it generates the key pair itself
and deposits the private key in Vault. If you create certificates through the
console and wonder why the key is in Vault, that is why — kythira does not
use that config type.

## 6. Worked example: a three-node cluster in one region

```
region                     us-phoenix-1
compartment                ocid1.compartment.oc1..kythira
pools                      kythira-pool-ad1  →  kIdk:PHX-AD-1
                           kythira-pool-ad2  →  kIdk:PHX-AD-2
                           kythira-pool-ad3  →  kIdk:PHX-AD-3
managers                   one per pool, each topology = { that AD: 1 }
```

Each manager gets its own copy of `oci_quorum_manager.env.example` differing
in exactly two lines — `OCI_INSTANCE_POOL_ID` and
`OCI_TOPOLOGY_AVAILABILITY_DOMAIN`:

```cpp
kythira::oci_instance_pool_quorum_manager_config cfg;
cfg.oci.region           = "us-phoenix-1";
cfg.oci.tenancy_id       = std::getenv("OCI_TENANCY_ID");
cfg.oci.user_id          = std::getenv("OCI_USER_ID");
cfg.oci.fingerprint      = std::getenv("OCI_FINGERPRINT");
cfg.oci.private_key_pem  = read_file("/etc/oci/api_key.pem");
cfg.compartment_id       = std::getenv("OCI_COMPARTMENT_ID");
cfg.cluster_name         = "kythira-prod";
cfg.instance_pool_id     = std::getenv("OCI_INSTANCE_POOL_ID");
cfg.node_port            = 7000;
cfg.topology.groups.push_back({.group_id = "kIdk:PHX-AD-1", .target_count = 1});

kythira::oci_instance_pool_quorum_manager<> manager{cfg};

// One call per reconciliation tick. Returns the health from BEFORE it
// repaired anything, so a caller logging this sees the fault that triggered
// the repair rather than the state after it.
auto pre = std::move(manager.maintain_quorum(cluster)).get();
```

Constructing the manager makes one `GetInstancePool` call, so a wrong OCID or
a missing policy fails immediately with `std::runtime_error` rather than at
the first remediation. Missing required config fields throw
`std::invalid_argument` before any network call at all.

## 7. Verifying without an OCI tenancy

`tests/oci_mock_server.hpp` is an in-memory stand-in for exactly the routes
these two components call — OCI publishes no LocalStack equivalent, so this
tier is built rather than downloaded. Point `endpoint_override` at it and the
components cannot tell the difference:

```bash
ctest --test-dir build-default -R '^oci'
```

Five entries: signing golden vectors, the HTTP client's transport behaviour,
the manager's construction/tagging/fault points, and the two mock-server
suites covering provisioning, assessment, decommission and certificate
issuance.

## Known limitations

- **Instance Principal auth is not implemented** (§4).
- **NodeIds are reused after a decommission.** The next id is
  max-of-`kythira-node-id`-tags plus one, scanned across the pool — and a
  decommissioned instance is detached, so its tag is no longer visible.
  Decommission the highest-numbered node and the next provision gets that
  number back. Requirement 6.4 scopes the scan to
  `ListInstancePoolInstances`, and no OCI call exposes a detached instance's
  tags, so a deployment that cannot tolerate a recycled identity has to keep
  the assignment outside the pool.
- **No real-OCI integration test tier yet.** `tasks.md` Task 6 is open, and
  two of its inputs are still unresolved spike questions: OCI's exact
  out-of-capacity error shape and whether OCI federates GitHub Actions' OIDC
  tokens to a Dynamic Group.

  What *has* been validated against a live tenancy (2026-08-11,
  `spike-notes.md` Findings 5-7): request signing end to end, the per-service
  host derivation, and the error unwrapping, via authenticated `ListRegions`
  and `ListInstances` calls. That pass found two defects invisible to the whole
  mock suite — the `oci` label missing from the endpoint domain, which meant
  the certificates hostnames did not resolve at all, and a `Host` header that
  disagreed with the signed one. Both are fixed.

  What remains unvalidated against real OCI is everything that needs a
  *resource*: `GetInstancePool`, the provisioning and decommission sequences,
  and certificate issuance. Those are still mock-only, which is the position
  AWS, Azure and GCP were each in before their first live run — and each of
  those runs surfaced three or four real defects.
