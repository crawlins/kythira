# Cost estimate: dogfooding the Redis gateway as sccache's backend

Pre-registered cost estimate for running `redis_gateway_node`
(`feat/redis-compatible-kv`, `.kiro/specs/redis-compatible-kv/`) as a
three-node shared compile cache behind [sccache](https://github.com/mozilla/sccache)
for this repository's own CI and for local builds. Written in the same spirit
as `doc/aws_acm_pca_test_cost_estimate.md` and
`doc/multi_raft_performance_cloud_cost_estimate.md`: the number is committed
**before** the deployment, so that a surprise on the bill is a falsified
estimate rather than a discovery.

Every rate below was read on **September 4, 2026** from the provider's public
price pages or from third-party price trackers where the provider's page does
not print a per-hour figure. None was read from a pricing API. The OCI compute
rate in particular is quoted inconsistently by third-party sites and MUST be
re-read from the OCI price list before this estimate is treated as binding.

## Headline

| Deployment | Monthly |
|---|---:|
| **OCI, three preemptible A1 Flex nodes (2 OCPU, 16 GB), one region** | **~$55** |
| OCI, the same three nodes on-demand | ~$103 |
| Hetzner, three CAX31 (4 vCPU, 8 GB) | ~€47 |
| AWS, three m7g.xlarge on-demand, one AZ | $373 **plus $100 to $450 egress** |
| AWS, the same three on spot | ~$110 to $180 **plus $100 to $450 egress** |

The cluster is not the expensive part anywhere. The expensive part is that
sccache downloads every cache object on every hit, GitHub-hosted runners are
not in the cluster's network, and three of the five providers this tree
speaks to bill that traffic at roughly $0.09/GB. OCI's 10 TB and Hetzner's
20 TB monthly egress allowances make that line zero; nothing else does short
of moving the runners.

## What the deployment is

One `redis_gateway_node` daemon per voter, three voters, one shard cut
(`sccache/8`) so that two shards usually have different leaders and a single
client endpoint exercises forwarding. sccache on each CI runner and on the
local machine speaks RESP to any node; `KYTHIRA_REDIS_FORWARDING` handles the
rest. `docker/sccache-e2e-compose.yml` on the branch is the topology, minus
the network between the nodes.

Facts about the branch's daemon that size the nodes, all read from
`cmd/redis_gateway_node/run_host.hpp` and `doc/redis-gateway.md` at
`f0de1b4`:

- **Memory persistence, by design.** `memory_persistence_engine`, with the
  comment that a replica which restarts catches up from its peers and a
  cluster that loses every replica at once has lost a cache. Rolling restarts
  only.
- **No log compaction.** Nothing outside the tests calls
  `node::create_snapshot`, so the in-memory Raft log holds every value ever
  written and never shrinks. The state machine is bounded by the eviction
  budget (`KYTHIRA_REDIS_MAX_SHARD_BYTES`, 1 GiB per shard by default, two
  shards). The log is bounded only by write volume. See *Known gaps* below;
  this is what decides between 8 GB and 16 GB nodes.
- **CPU is trivial.** Two IO threads, eight command workers, four executor
  stripes. Two vCPUs are sufficient.
- **arm64 is fine.** The image is a host-built binary on `ubuntu:24.04`, and
  CI already builds the tree on `ubuntu-24.04-arm`.
- **8 MiB value ceiling** (`KYTHIRA_REDIS_MAX_VALUE_BYTES`). Coverage and
  ThreadSanitizer objects from Boost.Test-heavy translation units can exceed
  it even compressed; those are refused safely and simply not cached.
- **CBOR on the wire.** `KYTHIRA_WIRE_SERIALIZER` defaults to `cbor`; the
  JSON serializer would add a third to every replicated value (measured on
  the branch: 10.7 MiB against 8.0 MiB for an 8 MiB value, 158 ms against
  5 ms of leader CPU).
- **Exposed with TLS and the ACL.** The plaintext listener is left empty,
  `KYTHIRA_REDIS_TLS_LISTEN` serves `rediss://`, PR runners get the
  `read_only` role, and failed `AUTH`s are rate-limited per source address.
  GitHub's runner address ranges are too large to firewall.

## The workload that sizes it

### CI cadence

Read from the `ci.yml` run list on September 4, 2026:

| Measure | Value |
|---|---:|
| `ci.yml` runs, all time | 886 |
| Runs between 2026-08-29 01:31Z and 2026-09-03 23:07Z | 100 |
| Of which `pull_request` / `push` | 78 / 22 |
| Implied runs per month | ~450 to 500 |
| Jobs per run that restore a ccache | 8 |

The eight cache-reading legs are the four `build-and-test` matrix entries
(g++-13 and clang++-18, x64 and arm64), `gcp-sdk-build`, `coverage`, `tsan`
and `future-backend-compat`.

### Bytes per leg

From `.kiro/specs/ccache-adoption/requirements.md` and `ci.yml`:

| Measure | Value |
|---|---:|
| Cacheable compiler calls per `build-and-test` leg | 187 of 399 |
| ccache size cap per leg (`--max-size`) | 2G (1G for `coverage`) |
| Estimated compressed object set per leg | 0.3 to 1.5 GB |

The per-leg estimate is the number this document is most likely to be wrong
about. It is bounded above by the 2G cap and below by a Release build of 187
mostly test translation units. **Falsify it first**: `sccache --show-stats`
on one warm leg reports bytes read, and one number replaces the range.

### Egress if every leg hits

| Measure | Value |
|---|---:|
| Per run, all eight legs warm | 2.4 to 12 GB |
| Per month at 450 runs | 1 to 5 TB |
| AWS / Azure / GCP after the free 100 GB | $0.087 to $0.12 per GB |
| **Monthly egress on AWS** | **$100 to $450** |
| Monthly egress on OCI (10 TB free) | $0 |
| Monthly egress on Hetzner (20 TB per server) | €0 |

The current `actions/cache` ccache path pays $0 for the same bytes because it
lives next to the runners. Any cloud-hosted sccache backend, this gateway or
plain S3, pays the egress; the gateway's instances are the only incremental
cost over S3.

### Runner minutes, for the Azure option below

Run 33714319093 (main, September 3, 2026, all 13 jobs green, wall clock
2h34m): about 480 runner-minutes, of which 154 were a `gcp-sdk-build` vcpkg
cache miss. A typical run is about 350. At 450 runs that is roughly 160,000
to 215,000 runner-minutes a month, all free today because the repository is
public.

## Rates

### OCI

| Item | Rate |
|---|---:|
| A1 Flex (Ampere) compute | $0.01 per OCPU-hour |
| A1 Flex memory | $0.0015 per GB-hour |
| Preemptible discount | flat 50% off compute and memory |
| E5 Flex (x86) compute, as a capacity hedge | $0.03 per OCPU-hour |
| Block volume, balanced tier | $0.0425 per GB-month (capacity plus 10 VPU) |
| Reserved public IPv4 | $0 |
| Network Load Balancer | $0 |
| Internet egress | first 10 TB per month free, then $0.0085/GB |
| Traffic between nodes in one region, any AD | $0 |

### AWS, for comparison (on-demand, Linux, us-east-1)

| Item | Rate |
|---|---:|
| t4g.large (2 vCPU, 8 GiB) | $0.0672/hr |
| m7g.xlarge (4 vCPU, 16 GiB) | $0.1632/hr |
| Spot, effective after interruptions | 40 to 60% off |
| EBS gp3 | $0.08 per GB-month |
| Public IPv4 | $0.005/hr |
| Internet egress after 100 GB | $0.09/GB to 10 TB |

### Others

| Provider | Compute, 3 x 8 GB | Spot | Egress |
|---|---:|---:|---:|
| Azure (Zone 1) | ~$160 | ~80% off | $0.087/GB after 100 GB |
| GCP standard tier | ~$150 | at least 60% off | $0.085/GB after 200 GiB |
| Hetzner CAX21 / CAX31 | €24 / €45 | none | 20 TB per server, then €1/TB |

## The OCI estimate

Three nodes at 2 OCPU and 16 GB, one region, spread across availability
domains where the region has more than one.

| Line | On-demand | Preemptible |
|---|---:|---:|
| 3 x (2 OCPU + 16 GB), 730 hours | $96.36 | $48.18 |
| 3 x 50 GB boot volumes, balanced | $6.38 | $6.38 |
| 3 reserved public IPs | $0.00 | $0.00 |
| Egress at 1 to 5 TB | $0.00 | $0.00 |
| Replication and InstallSnapshot traffic | $0.00 | $0.00 |
| **Total** | **~$103** | **~$55** |

Variants:

| Variant | On-demand | Preemptible |
|---|---:|---:|
| 3 x (2 OCPU + 8 GB), needs log compaction first | ~$76 | ~$41 |
| One Always Free node (2 OCPU + 12 GB) | $0 | n/a |

Annualized, the preemptible configuration is about **$660**, less than one
month of the AWS egress line it replaces.

### What could move the number

- **The Always Free A1 allowance may offset a paid tenancy.** Paid tenancies
  keep the Always Free allowances, which since June 15, 2026 are 2 OCPU and
  12 GB of A1. If that applies, the on-demand total drops to roughly $82. It
  is not assumed here, and whether preemptible instances draw from it is
  unconfirmed.
- **Preemptible instances are terminated, not stopped.** A node comes back
  as a fresh instance. Keep the boot volume on preemption so the
  configuration and ACL file survive; the volumes are priced in for that
  reason.
- **A1 capacity runs out.** "Out of host capacity" is a common A1 error in
  busy regions, and preemptible A1 is more exposed than on-demand. Spreading
  the nodes across availability domains costs nothing in traffic. One E5
  Flex x86 node is the other hedge; the tree builds for x64 anyway.
- **No load balancer.** `doc/redis-gateway.md` says to point different
  runners at different nodes and let forwarding cover leadership. A single
  stable endpoint, if wanted, is the Network Load Balancer at $0.
- **Not included:** taxes, any support plan, and Object Storage, which this
  deployment does not use.

## Why not the others

### AWS, Azure, GCP

Same cluster shape, three to four times the compute, and the egress line on
top. Spot cuts the compute by half to two thirds and does nothing to egress:

| Provider, 3 x 16 GB | On-demand | Spot | Egress | Total with spot |
|---|---:|---:|---:|---:|
| AWS m7g.xlarge, one AZ | $373 | ~$110 to $180 | $100 to $450 | $210 to $630 |
| GCP t2a, standard tier | ~$330 | ~$100 to $140 | $85 to $425 | $185 to $565 |
| Azure Dpsv5 | ~$350 | ~$80 | $90 to $430 | $170 to $510 |
| OCI A1 Flex preemptible | ~$103 | ~$55 | $0 | **~$55** |
| Hetzner CAX31 | ~€47 | n/a | €0 | **~€47** |

### Azure, same region as the runners

GitHub-hosted runners run in Azure, so a cluster in the runners' region would
pay the same-region rate. Three things limit that:

1. **The region cannot be chosen** for the free standard runners this public
   repository uses. It can be *measured* from inside a job via the Azure
   instance metadata endpoint
   (`http://169.254.169.254/metadata/instance/compute/location`), and a
   cluster in the majority region gets the discount on that share of the
   traffic only.
2. **Same-region is a discount, not zero.** Cross-zone traffic has been free
   since May 2024 and same-VNet traffic is free, but the runners are in
   GitHub's subscription. The published rate for same-region traffic between
   VNets over public addressing is $0.01/GB; whether traffic to another
   tenant's public IP is metered at that rate or as internet egress is not
   stated unambiguously and needs one VM and one run to find out.
3. **Pinning the region costs more than the egress.** The only GitHub product
   that places a runner in a chosen VNet is larger runners with Azure
   private networking, billed per minute. At this repository's 160,000 to
   215,000 runner-minutes a month that is roughly $950 to $1,300 on the
   Linux 2-core tier ($0.006/min) and $1,900 to $2,600 on 4-core
   ($0.012/min), five to ten times the egress it removes, on smaller
   machines than the free 4-core public runners.

Best plausible Azure outcome is egress of $30 to $150 plus about $80 of spot
compute. OCI gets to $0 egress with no experiment.

### Hetzner

Cheapest in absolute terms and needs no provider client, since the gateway
and its host only need a Linux VM. It is not chosen because the tree has a
live OCI tenancy, keyless OCI CI, and provisioning that has been exercised
(PR #229), and Hetzner would need new provenance and leak-audit scripts. It
is the fallback if A1 capacity proves unreliable.

## What removes the egress line entirely

Independent of provider, in ascending order of effort:

1. **Send only the Rust build through sccache.** Keep the free `actions/cache`
   ccache path for the C++ objects and set `RUSTC_WRAPPER=sccache` alone.
   The `lakers` FFI crate and its dependencies are tens of MB per leg, so
   monthly egress lands around 50 to 100 GB, inside every provider's free
   allowance, while still producing on the order of 250,000 real operations
   a month across two shards. Enough to exercise forwarding and elections;
   not enough bytes to stress replication.
2. **A provider that does not bill it** at this volume: OCI or Hetzner.
3. **Runners next to the cluster.** Self-hosted runners in the cluster's
   network make every hit intra-network. A second project, and the x64 legs
   of the matrix need x86 hosts too.

Option 1 is recommended for the first month regardless of where the cluster
runs: it keeps the egress term out of the falsification of everything else.

## Preemptible nodes and what they exercise

The point of running on preemptible capacity is that node loss is the
failure the cluster exists to survive. What the branch's daemon does and does
not do about it:

- **Static peers, no membership change.** `KYTHIRA_PEERS` is a fixed
  id-to-host list and the host uses `default_membership_manager`. The
  library has `add_server` and `handle_cluster_join`; the daemon does not
  wire them. A reclaimed node therefore has to come back with the **same
  node id and the same address**: a per-node instance pool of size one, a
  reserved private IP, and a DNS record set from cloud-init.
- **Memory persistence plus reclaim is not Raft-safe in the textbook sense.**
  A replacement returns with term zero and no recorded vote, so it can vote
  twice in one term. File-backed term and vote would not help either, since
  a preemption takes the disk with it unless a volume is reattached. For this
  workload the blast radius is a lost cache entry: values are immutable and
  content-addressed. The honest fix is membership change (remove the old id,
  add the new one), which preemptible capacity is the reason to wire in.
- **Correlated reclaims.** Two of three nodes gone means no quorum, every
  command times out, and every build still succeeds with misses (Requirement
  property 3 of the spec: a miss is not an error). Different shapes or
  availability domains per node reduce the correlation.
- **Rejoin traffic.** A replacement catches up by InstallSnapshot from a
  peer, up to the 2 GiB of shard state. Intra-region, $0 on every provider
  in this document.

## Known gaps to close before or during the dogfood

1. **Log compaction.** Add a policy-phase trigger in `run_host.hpp` that
   calls `create_snapshot` when a group's log passes a byte or entry
   threshold. Until then the 16 GB node size is what keeps a month of
   main-branch churn from exhausting memory, and a rolling restart is the
   manual compaction.
2. **Membership change in the daemon**, per the section above.
3. **The sccache adoption spec.** Wiring `RUSTC_WRAPPER=sccache` into the
   `lakers` port and CI was explicitly excluded from `redis-compatible-kv`
   and does not exist yet. `docker/sccache_runner/run.sh` is the pattern:
   start the server explicitly and export the wrapper only if that worked,
   so an unreachable cache is never a build dependency.

## Monthly exposure and the ceiling

| Cadence | Preemptible | On-demand |
|---|---:|---:|
| Per month | ~$55 | ~$103 |
| Per year | ~$660 | ~$1,240 |

There is no per-run cost to bound; the cluster is a fixed monthly charge
and the runner traffic is inside the free allowance by a factor of two or
more at the top of the estimated range. The two things that could exceed the
estimate are egress past 10 TB, which at $0.0085/GB adds $8.50 per extra TB,
and forgetting a boot volume after tearing the cluster down, which bills
$2.13 per volume per month until noticed. Both are audited the same way the
perf-cloud scripts audit AWS: a listing after teardown that fails if anything
survives.

## Sources

Read September 4, 2026.

- [OCI Price List, Oracle](https://www.oracle.com/cloud/price-list/)
- [Preemptible instances at a 50% discount, Oracle](https://blogs.oracle.com/cloud-infrastructure/post/announcing-preemptible-instances-a-new-kind-of-compute-instance-available-at-a-50-discount)
- [Oracle halves free tier A1 limits, InfoQ](https://www.infoq.com/news/2026/07/oracle-cloud-free-tier-limits/)
- [Oracle Cloud free tier including 10 TB egress, cloudpricecheck.com](https://cloudpricecheck.com/free-tier/oracle)
- [OCI storage pricing 2026, Oracle Licensing Experts](https://oraclelicensingexperts.com/blog/oracle-oci-storage-pricing/)
- [Creating a reserved public IP, Oracle docs](https://docs.oracle.com/en-us/iaas/Content/Network/Tasks/reserved-public-ip-create.htm)
- [Amazon EC2 on-demand pricing, AWS](https://aws.amazon.com/ec2/pricing/on-demand/)
- [t4g.medium and m7g.xlarge pricing, economize.cloud](https://www.economize.cloud/resources/aws/pricing/ec2/m7g.xlarge/)
- [AWS data transfer out pricing, EgressCost.com](https://egresscost.com/aws/data-transfer-pricing/)
- [EC2 Spot vs On-Demand true cost difference 2026, DEV Community](https://dev.to/zop_8abedcc7e12/ec2-spot-vs-on-demand-the-true-cost-difference-in-2026-2maj)
- [Azure bandwidth egress pricing by zone, EgressCost.com](https://egresscost.com/azure/zones-explained/)
- [A guide to Azure data transfer pricing, Microsoft Community Hub](https://techcommunity.microsoft.com/blog/azurenetworkingblog/a-guide-to-azure-data-transfer-pricing/4374538)
- [Azure Spot VMs pricing and eviction, Usage.ai](https://www.usage.ai/blogs/azure/spot-vms/)
- [About Azure private networking for GitHub-hosted runners, GitHub Docs](https://docs.github.com/en/organizations/managing-organization-settings/about-azure-private-networking-for-github-hosted-runners-in-your-organization)
- [GitHub Actions pricing 2026 per-minute rates, CICDCalculator](https://cicdcalculator.com/github-actions)
- [Google Cloud egress premium vs standard tier, EgressCost.com](https://egresscost.com/gcp/)
- [Hetzner Cloud pricing after the April 2026 increase, bitdoze.com](https://www.bitdoze.com/hetzner-cloud-cost-optimized-plans/)
