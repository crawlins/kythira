# Cost estimate: one cloud performance run

Pre-registered cost estimate for `.github/workflows/perf-cloud.yml`, required
by `.kiro/specs/multi-raft-performance/requirements.md` Requirement 18.11 and
written in the same spirit as `doc/aws_acm_pca_test_cost_estimate.md`: the
number is committed **before** the run, so that a surprise on the bill is a
falsified estimate rather than a discovery.

All rates are **AWS on-demand, Linux, shared tenancy, `us-east-1`**, read from
the AWS Pricing API on **August 29, 2026**. They are quoted to four decimal
places because the run is billed per second and the totals are small enough
that rounding to cents hides the comparison between shapes.

## The rates this estimate is built on

| Instance type | Arch | vCPU | Memory | Stated network | On-demand |
|---|---|---:|---|---|---:|
| `c5.2xlarge` | x86-64 | 8 | 16 GiB | Up to 10 Gigabit | $0.3400/hr |
| `m5.2xlarge` | x86-64 | 8 | 32 GiB | Up to 10 Gigabit | $0.3840/hr |
| `m7i.2xlarge` | x86-64 | 8 | 32 GiB | Up to 12500 Megabit | $0.4032/hr |
| `c6g.2xlarge` | Graviton2 | 8 | 16 GiB | Up to 10 Gigabit | $0.2720/hr |
| `c7g.2xlarge` | Graviton3 | 8 | 16 GiB | Up to 15 Gigabit | $0.2900/hr |
| `m6g.2xlarge` | Graviton2 | 8 | 32 GiB | Up to 10 Gigabit | $0.3080/hr |

None is burstable, which Requirement 18.5 makes a hard precondition rather
than a preference — see `scripts/perf-cloud/capture-provenance.sh`, which
refuses to run on a `t*` type rather than flagging it.

**8 vCPU is chosen to match the local machine's core count, not to exceed
it.** The point of Requirement 18.7 is to remove the hardware confound, and a
16-vCPU cloud row compared against a 4-core local one would replace one
confound with a larger one. The local development machine has 4 cores; 8 is
the smallest non-burstable size that leaves headroom for the benchmark's own
io threads without making the comparison a core-count study.

## Shape 1 — one instance, Tier B (task 16)

The primary deliverable of Requirement 18.7: one instance, the same binary and
the same scenarios as the local rows, for minutes.

| Phase | Wall clock | Billed |
|---|---:|---:|
| Instance boot + dependency install | ~4 min | yes |
| Fetch prebuilt benchmark artifact | ~2 min | yes |
| Measured phase (ceiling, see below) | **≤ 45 min** | yes |
| Artifact upload + teardown | ~3 min | yes |
| **Total per run** | **≤ 54 min** | |

At the ceiling, one `c5.2xlarge` run is **0.9 instance-hours × $0.3400 =
$0.31**. The other candidates at the same ceiling:

| Instance type | Cost per run at the 54-minute ceiling |
|---|---:|
| `c6g.2xlarge` | $0.24 |
| `c7g.2xlarge` | $0.26 |
| `m6g.2xlarge` | $0.28 |
| `c5.2xlarge` | $0.31 |
| `m5.2xlarge` | $0.35 |
| `m7i.2xlarge` | $0.36 |

**The expected run is well under the ceiling.** The measured phase is the
existing local sweep, which takes about 7 minutes on the local machine for the
value-size case and under 4 minutes for the serializer axis. A realistic
single-shape run is ~20 minutes wall clock, i.e. **$0.11–$0.13**. The ceiling
exists to bound a hang, not to describe a run.

### Storage, transfer and addresses

- **EBS**: one `gp3` root volume, 30 GiB, for under an hour. gp3 is
  $0.08/GB-month, so 30 GiB for one hour is **$0.0033** — negligible, and
  included above as rounding. It is nonetheless audited
  (`audit-aws-leaks.sh`), because a *leaked* volume bills $2.40/month forever,
  which is the failure mode worth catching.
- **Data transfer out**: the artifacts are JSON and CSV, kilobytes. The first
  100 GB/month egress is free. **$0.00.**
- **Elastic IP**: none allocated. The instance uses an auto-assigned public
  IPv4. Note AWS bills **$0.005/hr for every public IPv4**, in-use or not, so
  one hour is **$0.005** — again rounding, and again audited, because an
  address leaked from a failed run bills $3.60/month.

## Shape 2 — Tier E, one host per instance (task 20)

Three instances plus a placement group. The placement group itself is free;
the cost is three times shape 1, plus **cross-AZ traffic if the placement is
spread** ($0.01/GB each direction). Requirement 18.8 requires measuring
inter-node RTT and bandwidth before the measured window, and that probe is
what makes the transfer volume predictable rather than a guess.

| Shape | Instances | Cost per run at ceiling |
|---|---:|---:|
| Shape 1 (Tier B) | 1 | $0.31 |
| Shape 1 on Graviton (task 18) | 1 | $0.26 |
| Shape 2 (Tier E, same-AZ cluster group) | 3 | $0.93 |

Same-AZ placement is the default for shape 2 precisely because cross-AZ egress
would add a variable cost on top of a variable measurement.

## The ceiling is enforced, not documented

Requirement 18.11 asks for a wall-clock ceiling "so a hung benchmark cannot
bill indefinitely". Three independent mechanisms, because the interesting
failure is the one where the first two do not run:

1. **`timeout` around the measured phase** on the instance — bounds the
   benchmark itself.
2. **`timeout-minutes` on the job** — bounds the whole job including a runner
   that wedges before or after the benchmark.
3. **Unconditional teardown (`if: always()`) plus the post-run audit**, which
   *fails the job* if anything survives. This is the one that matters: the
   first two stop the work, and only the third proves the billing stopped.

A fourth would be an instance-side self-destruct (a `shutdown -h` scheduled at
boot, surviving a runner that dies entirely). It is **not** implemented and is
noted here as the known gap: if the GitHub runner is killed between
`RunInstances` and teardown, nothing on the AWS side stops the instance, and
the leak is caught by the *next* run's audit rather than this one's. Whether
that gap is worth closing depends on how often the workflow runs, which is
"on dispatch only" — so the exposure is bounded by someone noticing.

## Monthly exposure

The workflow is dispatch-only with no schedule (Requirement 18.2), so there is
no baseline monthly cost. For planning:

| Cadence | Shape 1 only | Shape 1 + Graviton + Shape 2 |
|---|---:|---:|
| Once per release | $0.31 | $1.50 |
| Weekly | $1.34/month | $6.50/month |
| Daily | $9.30/month | $45.00/month |

**Daily is not recommended and is not what this workflow is for.** Requirement
6.3's stability gate is expected to fail *more* often in the cloud, not less
(Requirement 18.9), so a daily cadence buys a stream of UNSTABLE rows at
$45/month. The intended cadence is per-release or on demand.

## What this estimate does not cover

- **Support plan charges**, which are a percentage of spend and therefore
  proportional to the numbers above.
- **Taxes.**
- **Any other provider.** Requirement 18.12 says deliver one provider end to
  end before adding a second, so this document covers AWS only. It gains a
  section per provider as tasks 21 and beyond land, rather than being
  rewritten.
