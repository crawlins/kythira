# Running kythira's GCP quorum managers

Setup for `kythira::gcp_compute_quorum_manager`
(`include/raft/gcp_compute_quorum_manager.hpp`) and
`kythira::gcp_mig_quorum_manager` (`include/raft/gcp_mig_quorum_manager.hpp`).
The companion [`gcp_quorum_manager.env.example`](gcp_quorum_manager.env.example)
lists every configuration field; this file is about the GCP resources those
fields point at, which the library deliberately does **not** create. The
design (node identity, the label scheme, the autohealing guard) is in
[`doc/gcp_quorum_manager_README.md`](../../doc/gcp_quorum_manager_README.md).

Both are built when `google-cloud-cpp`'s compute components are found
(`KYTHIRA_HAS_GCP_SDK`).

## Which manager

| | `gcp_compute_quorum_manager` | `gcp_mig_quorum_manager` |
|---|---|---|
| Provisions by | `instances.insert` into the zone | `instanceGroupManagers.resize` of the zone's MIG |
| Launch settings live in | the config (machine type, image, network, service account, Spot, startup script) | the MIG's instance template |
| Node identity | Drawn by kythira, encoded in the instance name `kythira-<cluster>-<id>` | Drawn by kythira, written to the `kythira-node-id` label |
| Best for | Dev, staging, simple deployments | Production: instance templates, one place to change the image |

For both, a topology group id **is** a zone name (`us-central1-a`): the
compute manager inserts into that zone, and `mig_by_group` maps it to that
zone's MIG.

## What kythira does and does not create

| Resource | Created by |
|---|---|
| VPC network and subnetworks, firewall rule for `node_port` | **you** |
| Node service account (optional) | **you** |
| Resource (placement) policies (optional, compute manager) | **you** |
| Instance template and one zonal MIG per zone (MIG manager) | **you** |
| Instances and their `kythira-*` labels | kythira |

## 1. Network

```sh
gcloud compute firewall-rules create kythira-raft --network default \
    --allow tcp:7000 --source-tags kythira --target-tags kythira
```

The compute manager attaches no network tags, so with it, scope the rule by
source range (your subnetwork's CIDR) instead of tags. The managers return
`<internal-ip>:<node_port>`.

## 2. For the MIG manager: one zonal MIG per zone, no autohealing

```sh
gcloud compute instance-templates create kythira-node \
    --machine-type e2-medium --image-family ubuntu-2204-lts \
    --image-project ubuntu-os-cloud --tags kythira \
    --service-account kythira-node@my-project.iam.gserviceaccount.com --scopes cloud-platform
gcloud compute instance-groups managed create kythira-prod-a \
    --zone us-central1-a --template kythira-node --size 0
# …and kythira-prod-b, kythira-prod-c in their own zones.
```

Do **not** add `--health-check`/autohealing. The constructor refuses a MIG
with any `autoHealingPolicies`: an autohealer replacing VMs on its own
health signal races kythira's remediation, and the two can each replace a
node the other still counts. A MIG at size 0 costs nothing.

## 3. IAM

The principal the manager runs as needs `roles/compute.instanceAdmin.v1` on
the project (instances, MIG resize and list, zone operations, labels). If
the compute manager attaches a `service_account_email` to nodes, the
principal also needs `roles/iam.serviceAccountUser` on **that one** service
account; granted project-wide, it reaches every service account in the
project. `scripts/ci-cloud-credentials/gcp/policies/gcp-quorum-manager.json`
is CI's version, which adds what the test fixtures need.

Credentials come from `gcp_client_config::credentials_json` (an inline key)
or, left empty, Application Default Credentials. On GCE, attach a service
account to the node running the manager and store no key at all.

## 4. Worked example: three voters in three zones

```sh
cp gcp_quorum_manager.env.example /etc/default/gcp_quorum_manager
$EDITOR /etc/default/gcp_quorum_manager   # project, zones, image or MIGs
chmod 0600 /etc/default/gcp_quorum_manager
```

Then build the config struct in your loader; both managers have a full
example in [`doc/gcp_quorum_manager_README.md`](../../doc/gcp_quorum_manager_README.md#configuration-examples).

## 5. Verifying

`tests/gcp_quorum_manager_real_gce_test.cpp` runs against a real project.
It is registered only when CMake is configured with
`-DKYTHIRA_GCP_REAL_TESTS=ON`, runs only with `KYTHIRA_GCP_REAL_TESTS=1`,
`GCP_PROJECT_ID` and `GCP_REGION` set (otherwise it exits 77, a skip), and
launches real instances that cost money. Its MIG cases additionally need
`GCP_TEST_MIG_A`/`B`/`C` (and `GCP_TEST_MIG_AUTOHEAL` for the guard case) naming
pre-created MIGs; see the file and `.github/workflows/real-cloud-tests.yml`.
