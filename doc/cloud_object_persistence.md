# Cloud object persistence

Raft state — term, vote, log, snapshot — stored as individual objects in a
cloud key-object store, with one engine generic over five providers (AWS S3,
Azure Blob, Google Cloud Storage, OCI Object Storage, Alibaba OSS).

> **Status, August 19, 2026.** All five provider engines are **live-verified**
> against their real services — see [Verification status](#verification-status)
> for exactly what that phrase covers on each, because it is not the same claim
> everywhere. What remains documentation-derived is named as such in the tables
> below rather than averaged away.

## Is this the right persistence engine for you?

Read the [operating envelope](#the-operating-envelope) before anything else.
This engine's write path is **one HTTP round trip per Raft log entry per
node**, and that single fact decides most deployments:

- Sustained append throughput per node cannot exceed roughly `1 / p50(PUT)`.
  Measured: **~35 entries/s** for a well-placed node (Azure Blob from inside
  Azure) down to **~0.9 entries/s** for a cross-ocean one — two to three
  orders of magnitude below a local NVMe write-ahead log either way.
- Request charges scale **linearly with append rate**. A three-node cluster
  sustaining 100 entries/s costs roughly **$3,900/month in request charges
  alone**, before storage or egress.
- Election-path latency is four sequential durable writes, which pushes
  `election_timeout_min` up by `4 × p99(PUT)` — measured at **≥122 ms** for a
  well-placed node and **≥4.6 s** for a cross-ocean one.

It is the right engine for a cluster whose state changes at human or
configuration timescales — service registries, cluster membership, control
planes, leader election over a small replicated configuration — and where the
operational value of *having no disks to manage* outweighs the latency. It is
the wrong engine for a data-plane log.

## The durability contract

The engine makes exactly one promise, and it is stronger than
`file_persistence_engine`'s: **`save_current_term` and `save_voted_for` return
only after the object store has acknowledged the write.** No buffering, no
background flush, no asynchronous path anywhere in the engine
(`.kiro/specs/cloud-object-persistence/` Requirement 4.1). That is what Raft's
`currentTerm`/`votedFor` rule actually demands, and it is why those calls are
WAN-latency operations rather than memory writes.

Four properties of a store carry that promise. They are numbered here as the
design numbers them, because how *often* each is needed differs by orders of
magnitude and that difference is the whole reason this engine can sit on an
object store at all:

| | Property | How often the engine needs it |
|---|---|---|
| **N1** | A 2xx response means the write is durable | **Always.** Every term and vote write depends on it |
| **N2** | Read-after-write, including on overwrite | **Once per process lifetime**, at construction, when the in-memory mirror loads |
| **N3** | List-after-write | **Once**, for the same reason — and this is the one that can hurt silently |
| **N4** | A conditional-write primitive | Only under `fencing_mode::compare_and_swap` |

**The mirror is what buys the narrow requirement.** After construction, no
read touches the network: the engine answers `get_log_entry` and friends from
memory. A design that read from the store on the Raft hot path would be
exposed to read consistency continuously instead of once. The mirror looks
like a performance optimization; it is also a correctness simplification.

**N3 is the dangerous one.** A lagging listing at construction produces a
**short log** — data loss, silently, with no error anywhere. That is why the
N3 cells below were closed by measurement and not by analogy.

### Per-provider consistency and durability

Vendor documentation established August 14, 2026; live confirmations dated in
the cells. **The confirmation column is load-bearing.** A table that averages
five citations into one confident sentence has laundered its weakest one, so
the weakest ones are spelled out.

| | AWS S3 | Azure Blob | GCS | OCI Object Storage | Alibaba OSS |
|---|---|---|---|---|---|
| **N2** read-after-write, new **and** overwrite | Strong — **explicit**, all operations, since Dec 2020 | Strong — documented as a general storage-account guarantee | Strong — **explicit**, "never … stale data", globally | Strong — **explicit** overview statement | Strong — **explicit**, "no scenarios in which data is not obtained" |
| **N3** list-after-write | Strong — **explicit**, called out beside read consistency; **confirmed live** | Documented only in general terms, without S3/GCS's specificity — but **confirmed live**, so this cell is empirical evidence over weak documentation | Strong — **explicit**, "bucket listing and object listing are strongly consistent" (Spanner-backed); **confirmed live** | **No listing-specific vendor statement exists**; **confirmed live** — evidence, not a guarantee | **No listing-specific vendor statement exists**; **confirmed live** — evidence, not a guarantee |
| **N1** 2xx ⇒ durable | Yes — "if you receive a success response, Amazon S3 added the entire object"; 11 nines | **Depends on account redundancy** — see below. ZRS: **explicit** synchronous write to all three zones before success | Yes — **explicit**: the object becomes visible only once the upload completed and the server sent success | Implied by the documented redundancy model + 11 nines; **no "before returning success" wording exists — searched and not found, August 17, 2026.** Oracle publishes a steady-state redundancy statement and a read-side consistency statement, and neither says when the redundancy exists relative to the PUT response | Implied — "replicas of the object are created for redundancy" on a success response; 12 nines |
| **Confirmed in this repo** | conditional writes, checksums and list-after-write **live-verified** (Aug 16 2026, `us-east-1`); **durability wording documentation-derived** | as S3 (Aug 17 2026, `eastus`, Standard_ZRS); **durability wording documentation-derived** | as S3 (Aug 17 2026, `us-central1`); **durability wording documentation-derived** | as S3 (Aug 17 2026, `us-phoenix-1`); **durability wording still OPEN** | **live-verified** (`ap-southeast-1`, Aug 14 2026; re-verified Aug 16 2026 against the generic engine) |

Every N3 cell was closed the same way on every provider: write 25 objects
under a fresh prefix, LIST immediately, assert all 25 appear — three rounds,
complete every round, on all five. For Azure Blob and OCI the run *is* the
evidence, because neither vendor publishes a listing-specific statement, and
those cells say so rather than borrowing S3's or GCS's wording.

**No provider's N1 has been verified by this repo, on any provider.** Proving
"a 2xx means durable" requires killing the service, which is not a test anyone
here can run. Every N1 cell is documentation, and the OCI cell is documentation
that does not exist.

### Azure: the one N1 the operator controls

Azure Blob is the only provider whose durability-on-response claim is
**account configuration the engine does not read and cannot enforce**:

- **ZRS** — the only mode Microsoft documents as writing synchronously to all
  three zone replicas before returning success. **Use this.**
- **LRS** — acceptable; synchronous, but single-datacenter durability.
- **GRS / GZRS** — the cross-region copy is **asynchronous**. A 2xx means
  durable *in the primary region only*.

`scripts/ci-cloud-credentials/azure/provision-object-persistence-container.sh`
creates `Standard_ZRS` for exactly this reason, and says so in its header. If
you provision the account by hand, this is the setting to get right.

### Encrypted buckets and `verify_checksums`

On S3 and OSS every PUT is checked twice. The service checks the
`Content-MD5` the client sends and refuses a mismatch. That half is always on.
The engine then checks a digest the store returned against one it computed
from the bytes it sent. `object_persistence_options::verify_checksums`
(default `true`) governs only this second, local half.

What the local check reads, measured on 2026-10-27
(`.kiro/specs/object-store-encrypted-bucket-checksums/spike-notes.md`):

| Store | Bucket encryption | ETag is the content MD5? | Local check reads | `verify_checksums` |
|---|---|---|---|---|
| AWS S3 | none, SSE-S3 | yes | `x-amz-checksum-crc64nvme` | leave on |
| AWS S3 | SSE-KMS, DSSE-KMS | **no**, and it differs between PUTs of the same bytes | `x-amz-checksum-crc64nvme` | leave on |
| AWS S3 | SSE-C required by policy | n/a: the PUT is refused `403 AccessDenied`, since the client sends no SSE-C headers | — | — |
| S3-compatible (`endpoint_override`) | any | if it sends no CRC-64/NVME, the ETag is checked as an MD5 | CRC-64/NVME if sent, else the ETag | set `false` only if the store sends neither a CRC nor an MD5 ETag |
| Alibaba OSS | none, SSE-OSS, SSE-KMS | yes, unchanged by encryption | the ETag | leave on |
| Azure, GCS, OCI | any | not used | no local check | no effect |

Real S3 sends `x-amz-checksum-crc64nvme` on every PUT under every encryption
mode without the client asking for it, so `aws_s3_client` adds no request
header and no round trip. Against AWS a missing CRC fails the write as a
broken promise rather than skipping the check. Before 2026-10-27 the S3 client
compared the ETag instead, so every write to an SSE-KMS bucket failed and the
advice was to turn `verify_checksums` off. That advice is withdrawn.

## The operating envelope

### Measured write latency

**Where the measurement was taken, because that is the first thing that makes
two latency figures incomparable:** client-side, around the engine call. It
includes TLS, request signing, the round trip and the engine's own
bookkeeping — what a Raft node actually waits for. It is **not** service-side
latency and is **not** comparable with any vendor's published figure.

Measured August 19, 2026 by `tests/object_persistence_real_cases.hpp`'s
latency case, which emits a fixed greppable line recording that position:

```
KYTHIRA_LATENCY provider=s3 op=append_log_entry samples=20 p50_ms=… p99_ms=…
measured=client-side-around-engine-call
```

**Two measurement positions, and they disagree by 2-12×.** Both are recorded
because neither is "the" answer — which is the point.

*From a developer machine* (August 19, 2026), the first figures this project
had:

| Provider | Region | `save_current_term` p50 | `append_log_entry` p50 |
|---|---|---|---|
| S3 | `us-east-1` | 128 ms | 128 ms |
| GCS | `us-central1` | 145 ms | 113 ms |
| OCI Object Storage | `us-phoenix-1` | 356 ms | 342 ms |
| Azure Blob | `eastus` | 348 ms | 365 ms |
| Alibaba OSS | `ap-southeast-1` | 1648 ms **(distance, not OSS)** | 1476 ms **(ditto)** |

*From a GitHub-hosted CI runner* (August 21, 2026, run 32432380565) — closer
to what a cloud-hosted node sees, and the first figures carrying a p99:

| Provider | `save_current_term` p50 / p99 | `append_log_entry` p50 / p99 |
|---|---|---|
| Azure Blob | 28.7 / 30.4 ms **(in-provider — see below)** | 28.7 / 42.6 ms **(ditto)** |
| S3 (x64) | 66.5 / 81.3 ms | 67.1 / 74.8 ms |
| S3 (arm64) | 62.5 / 69.9 ms | 59.4 / 83.1 ms |
| GCS | 135.1 / 153.2 ms | 121.0 / 255.0 ms |
| Alibaba OSS | 1096.9 / 1140.5 ms **(distance, not OSS)** | 1090.3 / 1191.9 ms **(ditto)** |

OCI is absent from the second table because its run did not complete — see
[Verification status](#verification-status).

**Three of these numbers are traps, and every one of them is a measurement
position rather than a property of the service.**

1. **Azure Blob's 28.7 ms is an in-provider measurement.** GitHub-hosted
   runners run on Azure, so that row is a node talking to storage inside its
   own provider's network. Reading it as "Azure Blob is 2× faster than S3 and
   5× faster than GCS" would be exactly wrong — it measures proximity. It is
   also, for that reason, the closest thing here to what a *correctly placed*
   node sees on any provider: co-locate and the round trip collapses.
2. **Alibaba's ~1.1 s is a cross-ocean distance measurement.** Its bucket is
   `ap-southeast-1` and the runners are not. It says nothing about OSS.
3. **`get_log_entry` measures sub-microsecond on every provider** (~0.00008 ms
   p50). That is a memory-mirror hit, not a round trip.

**The arm64 row is a deliberate negative result.** S3 on arm64 is within noise
of x64, which is what should happen: this path is network-dominated and
nothing in it branches on host architecture. The arm64 leg earns its place by
compiling the suite, not by measuring anything new.

**On the word "p99".** The suite takes 8 samples for `save_current_term` and
20 for `append_log_entry`, and computes percentiles by *nearest rank, clamped*
— deliberately not interpolating, because at these counts interpolation would
invent precision the data does not carry. At 8 and 20 samples the clamp means
**the reported p99 is the slowest observed request**, not a 99th percentile in
any statistical sense. It is a useful worst-of-run; it is not a tail estimate,
and a true tail over thousands of requests would very likely be worse.

**Two numbers in the neighbourhood of this table are traps.**

1. **Alibaba's figures are a distance measurement, not a provider one.** Its
   bucket is in `ap-southeast-1` and every other provider's is in a US region,
   so the 4–10× gap is the cross-ocean round trip, not anything about OSS.
   Quoting that row as a provider comparison would be wrong.
2. **`get_log_entry` measures sub-microsecond on every provider.** That is a
   memory-mirror hit, not a round trip. Reading it as a storage-read latency
   would be badly wrong.

**The developer-machine table is p50 only, and from no node's position.**
Size your own deployment against a measurement taken from where your nodes
actually run. The CI-runner table carries the suite's p99s (worst-of-run, as
above), and the sizing rule below is worked from those rather than from p50.

*Batched layouts and write-behind, from GitHub-hosted CI runners* (October
22, 2026, runs
[39727417123](https://github.com/crawlins/kythira/actions/runs/39727417123)
for S3 and
[39776156973](https://github.com/crawlins/kythira/actions/runs/39776156973)
for the rest, `.kiro/specs/object-store-batched-layout/` task 10). Every
provider passed all 12 cases, Alibaba its 12 without the two fencing ones.
p50 / p99 in ms, 8 samples for the combined write and 10 for each segment:

| Provider | single `append_log_entry` | `save_hard_state` (combined) | 100-entry segment | ~0.88 MiB segment |
|---|---|---|---|---|
| S3 (x64) | 161 / 179 | 179 / 202 | 175 / 186 | 230 / 700 |
| S3 (arm64) | 35 / 42 | 35 / 57 | 40 / 44 | 89 / 313 |
| GCS | 139 / 155 | 144 / 153 | 139 / 150 | 494 / 515 |
| Azure Blob | 246 / 294 | 251 / 277 | 311 / 327 | 589 / 648 |
| OCI Object Storage | 288 / 357 | 292 / 328 | 345 / 355 | 662 / 721 |
| Alibaba OSS | 939 / 1083 **(distance)** | 949 / 1084 | 908 / 921 | 1982 / 2028 |

- **A 100-entry segment costs about one PUT.** With small entries the body
  is a few KiB, and the request, not the bytes, is the price. That is the
  whole case for `log_layout::segmented`: a follower run of 100 is one
  round trip instead of 100.
- **A segment near `max_segment_bytes` costs 1.4-3.6 single PUTs at p50**
  and at most 721 ms at p99 in-region (2 s for Alibaba across the ocean).
  The 1 MiB default stands. What it does demand is that the RPC timeout a
  leader gives an AppendEntries exceeds the follower's segment PUT p99, or a
  full segment times out on every retry. Lower `max_segment_bytes` instead
  if your timeout cannot grow.
- **The S3 rows disagree with August's: 2.4x slower on x64 and 1.7x faster
  on arm64.** Neither moved the bucket; the runners did. It is the
  measurement-position trap again, and it is why each run records its own
  position.

### Segment size

`max_segment_bytes` was swept from 64 KiB to 8 MiB on every provider
(`.kiro/specs/object-store-segment-size-sweep/`): S3 on both runner
architectures in run
[40143696448](https://github.com/crawlins/kythira/actions/runs/40143696448),
and GCS, OCI Object Storage, Azure Blob and Alibaba OSS (x64) in run
[40326601959](https://github.com/crawlins/kythira/actions/runs/40326601959),
both on `main` at f0e2548. Like the tables above they were measured from
GitHub-hosted runners, so they are cross-region for most providers.

What a follower pays to persist one 100-entry AppendEntries of 32 KiB
entries (~4.4 MB), p50 / max in ms over 30 samples. This is the number the
default is judged by. The run must finish inside the 5 s
`append_entries_timeout`, or the leader resends a run the follower is still
writing:

| S | S3 x64 | S3 arm64 | GCS | OCI | Azure Blob |
|---|---|---|---|---|---|
| 64 KiB | 492 / 546 | 572 / 652 | 1587 / 1948 | 4444 / 4651 | 3334 / 3563 |
| 256 KiB | 181 / 247 | 195 / 290 | 682 / 912 | 1321 / 1471 | 952 / 1052 |
| 512 KiB | 117 / 216 | 125 / 191 | 545 / 599 | 928 / 1586 | 671 / 1047 |
| 1 MiB | 107 / 167 | 115 / 242 | 406 / 1096 | 549 / 636 | 448 / 495 |
| 2 MiB | 98 / 145 | 106 / 218 | 457 / 560 | 567 / 681 | 444 / 523 |
| 4 MiB | 108 / 165 | 106 / 334 | 475 / 2611 | 628 / 713 | 450 / 603 |
| 8 MiB | 109 / 117 | 108 / 277 | 503 / 679 | 622 / 751 | 491 / 603 |

Loading a 16 MiB log of 128 KiB entries in a fresh engine, p50 of 5. Recovery
reads the log objects one at a time, so it scales with their count:

| S | S3 x64 | S3 arm64 | GCS | OCI | Azure Blob |
|---|---|---|---|---|---|
| 64 KiB | 3.9 s | 4.2 s | 20.6 s | 46.4 s | 33.9 s |
| 256 KiB | 3.8 s | 4.2 s | 20.1 s | 47.0 s | 34.0 s |
| 512 KiB | 2.0 s | 2.2 s | 11.3 s | 28.4 s | 20.7 s |
| 1 MiB | 0.9 s | 1.0 s | 5.0 s | 14.2 s | 10.5 s |
| 2 MiB | 0.5 s | 0.6 s | 2.8 s | 7.9 s | 5.9 s |
| 4 MiB | 0.4 s | 0.4 s | 1.7 s | 4.9 s | 3.8 s |
| 8 MiB | 0.3 s | 0.3 s | 1.1 s | 3.1 s | 2.6 s |

One full segment PUT, p50 in ms:

| S | S3 x64 | S3 arm64 | GCS | OCI | Azure Blob | Alibaba OSS |
|---|---|---|---|---|---|---|
| 64 KiB | 37 | 42 | 158 | 338 | 256 | 1467 |
| 256 KiB | 49 | 45 | 250 | 434 | 330 | 1885 |
| 512 KiB | 51 | 71 | 290 | 470 | 366 | 2091 |
| 1 MiB | 70 | 84 | 335 | 506 | 393 | 2309 |
| 2 MiB | 83 | 86 | 403 | 566 | 437 | 2536 |
| 4 MiB | 107 | 107 | 504 | 621 | 482 | 2750 |
| 8 MiB | 182 | 176 | 564 | 733 | 542 | 3206 |

- **Every size keeps the follower inside the bound on the four providers that
  finished.** The closest is OCI at 64 KiB, 4.65 s, where every 32 KiB entry
  is its own object. With 4 KiB entries (~0.55 MB per run) every size is
  at most 0.8 s.
- **Above 1 MiB the follower gains nothing.** A run that fits in at most
  `append_concurrency` (8) segments goes out in one parallel wave, so past
  that point a larger segment can only make the wave slower. From 1 MiB to
  8 MiB the 32 KiB p50 moves between −6% and +24%.
- **Recovery is where size pays.** Each step up halves the object count, and
  8 MiB loads 16 MiB three to five times faster than 1 MiB. A truncation's
  rewrite and a single full-segment PUT cost 30-70% more at 8 MiB.
- **The default stays 1 MiB, now measured** (the sweep spec's Requirement
  8.3). It passes the bound everywhere, and no smaller size beats the largest
  passing one, 8 MiB, by the 20% on three providers the rule asks for. For a
  node whose log is large and whose restarts must be fast, raise it to 4 or
  8 MiB; both pass the bound on every provider measured. Loading never
  consults the setting, so an existing bucket reads at any value.
- **Alibaba OSS finished only the PUT case.** Its RAM session expired 30
  minutes into the bundle, partway through the follower runs. At 1.5-3.2 s per
  PUT from the runner, a 32 KiB run at 64 KiB would take about 18 s, so small
  segments do not fit the bound there; 1 MiB and up should, as five or fewer
  parallel PUTs.

### Election-timeout sizing

A candidate persists an incremented term and then a self-vote before sending
its first RequestVote: two sequential durable writes. A follower at a lower
term persists the term bump and then its vote before replying: two more. One
election round is therefore **four sequential PUT round trips plus one RPC
round trip**:

```
election_timeout_min  ≥  4 × p99(PUT) + rpc_rtt + margin
```

and the randomized election-timeout **range width** must be at least that same
quantity, or nodes re-time out inside each other's elections and livelock.

`save_hard_state` (`.kiro/specs/batched-durable-writes/`) writes the term
before the vote on both sides and skips a slot that has not changed, so a
follower already at the candidate's term pays one PUT rather than two. The
common case, a follower learning the term from the RequestVote itself, still
changes both, so the rule above stands.

Until October 20, 2026 that common case cost the follower **three** PUTs, so
an election was five: stepping down to the new term stored the term and "no
vote" before the grant stored the vote. `handle_request_vote` now stores the
new term once, with the vote when it grants and alone when it refuses
(`.kiro/specs/object-store-batched-layout/`, task 1). Size clusters running
older builds with `5 × p99(PUT)`.

With `hard_state_layout::combined` (see
[Combined hard state](#combined-hard-state-and-upgrading-a-bucket-in-place))
the term and the vote are one object, so each side writes once:

```
election_timeout_min  ≥  2 × p99(PUT) + rpc_rtt + margin      (combined)
```

The three rules side by side, worked against the CI-runner p99s below:

| p99(PUT) | Older builds (5×) | `separate`, this build (4×) | `combined` (2×) |
|---|---|---|---|
| 30.4 ms Azure Blob, in-provider | 152 ms | 122 ms | **61 ms** |
| 81.3 ms S3 `us-east-1` | 407 ms | 325 ms | **163 ms** |
| 153.2 ms GCS `us-central1` | 766 ms | 613 ms | **306 ms**, but keep ≥ ~1 s (below) |
| 1140.5 ms OSS, cross-ocean | 5.70 s | 4.56 s | **2.28 s** |

All are plus `rpc_rtt` and margin, and the randomized range width must be at
least the same quantity. Pre-vote adds one RPC round trip and no PUT.

**On GCS, keep `election_timeout_min` at about one second or more under either
layout.** GCS throttles mutations of one object to about one per second. The
combined record is one object, written once per election and once per term
change per node, which is no more often than `<prefix>/term` is written under
`separate`, so the limit binds no harder than it did. It does bind below the
2× figure: a node that campaigns faster than once a second can be answered
`429` on its own `term` object.

| p99(PUT) | Floor on `election_timeout_min` | Verdict |
|---|---|---|
| ~3 s (the cross-ocean figure measured before the real tier existed) | **≥ 12 s** | Consistent with the 9.6 s the Alibaba real tier observed for the term+vote case alone. Usable only for clusters that tolerate a 12 s+ leaderless window |
| **1141 ms** (Alibaba OSS, cross-ocean) | **≥ 4.6 s** + RTT | What a *badly placed* node costs you. Not a property of OSS |
| **153 ms** (GCS, `us-central1`) | **≥ 612 ms** + RTT | |
| **81 ms** (S3, `us-east-1`) | **≥ 325 ms** + RTT | |
| **30 ms** (Azure Blob, **in-provider**) | **≥ 122 ms** + RTT | What a *well placed* node costs you — inside the range Raft deployments normally use. This is the configuration the engine is actually for |

These replace the hypothetical 40 ms placeholder the design carried before any
measurement existed, and they are real p99s rather than the p50-derived lower
bounds this table carried until August 21, 2026.

**Two caveats, and neither is small.** First, "p99" here is **the slowest of 8
or 20 observations** — see the note under the latency tables — so it is a
worst-of-run, not a tail estimate; a real tail would be worse. Second, the
spread across those rows is **almost entirely network placement**, not
provider choice: the fastest and slowest differ by 38×, and the two extremes
are the same workload measured from inside the provider's network and from the
other side of an ocean.

The practical reading: **size against a p99 measured from where your nodes
actually run**, and treat co-location as the single biggest lever on this
number — larger than which of the five providers you pick.

### Throughput

With one entry per AppendEntries, sustained append throughput per node cannot
exceed roughly `1 / p50(PUT)` entries per second, because each entry is its own
round trip. (Until `.kiro/specs/batched-durable-writes/` a follower also
rewrote `<prefix>/term` after every AppendEntries that changed its log, so the
real ceiling was about half this; that write is now skipped when the term has
not changed.) An AppendEntries carrying N entries now costs about
`N / append_concurrency` round trips rather than N, because
`append_log_entries` keeps up to `append_concurrency` (default 8) PUTs in
flight. A leader's own appends are still one per client command. The table is
the one-entry ceiling:

From the **developer-machine** p50s:

| Provider | Ceiling |
|---|---|
| S3 `us-east-1` | ~7.8 entries/s |
| GCS `us-central1` | ~8.8 entries/s |
| OCI `us-phoenix-1` | ~2.9 entries/s |
| Azure Blob `eastus` | ~2.7 entries/s |
| Alibaba OSS `ap-southeast-1` | ~0.7 entries/s (**distance, not OSS**) |

From the **CI-runner** p50s, which are closer to a cloud-hosted node:

| Provider | Ceiling |
|---|---|
| Azure Blob (**in-provider**) | ~35 entries/s |
| S3 | ~15 entries/s |
| GCS | ~8 entries/s |
| Alibaba OSS (**cross-ocean**) | ~0.9 entries/s |

**Placement moves this ceiling by 38×, and provider choice barely moves it at
all.** The two Azure rows are the same engine against the same service: ~2.7
entries/s from across the internet, ~35 entries/s from inside the provider's
network. No provider in this table is fast enough to change the conclusion —
even the best row is two orders of magnitude below a local NVMe WAL — but a
badly placed node gives up another order of magnitude on top.

**By default the ceiling is the leader's, and neither layout moves it.** A
leader's `submit_command` makes one PUT per command while it holds `node`'s
mutex, so a cluster commits at most about `1 / p50(PUT)` entries a second under
`log_layout::per_entry` and `log_layout::segmented` alike. The opt-in
write-behind group commit below is what lifts it. What the segmented log
changes instead is the follower and recovery side:

| | `per_entry` | `segmented` (defaults) |
|---|---|---|
| Follower requests per AppendEntries run of N | N | ⌈N / 100⌉, fewer if 1 MiB fills first |
| Follower catch-up, N = 10,000 on S3 (p50 ≈ 67 ms) | 10,000 PUTs, ≈ 1,250 rounds at concurrency 8, ≈ 84 s | 100 PUTs, ≈ 13 rounds, ≈ 1 s (a 100-entry segment measured at about one PUT, October 22, 2026) |
| Recovery GETs for a log of N | N | about N / B, B the entries per segment |

#### Write-behind group commit

`object_persistence_options::write_behind = true`
(`.kiro/specs/object-store-group-commit/`) makes an append *stage* the entry
with no request, and makes the durability barrier `node` already waits on
*flush* everything staged as one run. The engine then answers
`durability_class::barrier`, so `node` releases its mutex while it waits, and
commands that arrive during one flush are carried together by the next. At most
one flush is in flight per engine. With it on, a leader's ceiling is about

```
commit rate  ≈  B / p50(flush)
```

where `B` is the entries per flush, which is roughly the commands that arrive
during one flush. Under `segmented` a flush of up to `max_segment_entries`
entries is one PUT, so `p50(flush)` is about the p50 of a segment PUT. Under
`per_entry` it is about `⌈B / append_concurrency⌉` round trips of single-entry
PUTs. `B` is a property of the workload, not of the engine: one client issuing
commands one at a time gets `B = 1` and today's ceiling. Measure it as the
leader's commands divided by its log PUTs, or by `flushes_issued()`. On the
mock store with every PUT held for 60 ms, 24 concurrent commands cost the
leader 2 PUTs (`raft_object_store_group_commit_test`).

**Measured on every provider** (October 22, 2026, the runs above): 16
submitters each appending through one leader-style mutex, 64 commands, with
`log_layout::segmented`. `B` is commands per log PUT:

| Provider | synchronous commits/s | write-behind commits/s | gain | `B` | mean flush |
|---|---|---|---|---|---|
| S3 (x64) | 6.4 | 24.1 | 3.8x | 4.0 | 166 ms |
| S3 (arm64) | 30.1 | 65.0 | 2.2x | 2.5 | 38 ms |
| GCS | 6.0 | 18.4 | 3.1x | 3.4 | 183 ms |
| Azure Blob | 2.9 | 13.3 | 4.5x | 3.4 | 253 ms |
| OCI Object Storage | 3.3 | 9.1 | 2.7x | 2.8 | 305 ms |
| Alibaba OSS | 1.1 | 3.0 | 2.9x | 2.9 | 959 ms |

`B / mean flush` reproduces each write-behind rate, so the formula above is
the right one to size with. `B` stayed between 2.5 and 4 although 16
callers were waiting: a flush takes whatever is staged when it starts, and
callers released by one flush stage their next command during the following
flush, not the current one. More concurrent clients raise `B`; a faster
store lowers it, which is why the slowest-PUT rows gain the most.

**Latency moves the other way at the median.** The commit p50 rose about
3x (S3 x64 157 → 500 ms, OCI 303 → 909 ms), because a command now waits for
a flush to start and then for it to finish. The p99 fell 2-4.5x (S3 x64 10.0 →
2.7 s, OCI 19.2 → 7.0 s). The synchronous p50 flatters: an unfair mutex
lets most commands through after one PUT while a few wait for the whole run.

**The default is unchanged.** With `write_behind` off, every append is a PUT
acknowledged before the call returns. The engine answers
`durability_class::synchronous` (durable, and a barrier never blocks), and
`node` keeps its lock and sends exactly the requests it sent before. What
changes when you switch it on:

- A failed flush publishes the prefix of the run the store acknowledged. The
  rest stays staged and is retried at the next barrier, after any objects the
  failed flush sent are deleted. Every caller the flush was to cover gets the
  error. A refused precondition under `compare_and_swap` latches, as always.
- A truncation or compaction waits for a running flush. Staged entries it
  removes are dropped without a request, and a barrier on one of them throws,
  which is how a follower's truncated batch is refused.
- Nothing staged is on the store, so a crash loses only entries no caller was
  told were durable. That is the barrier contract the file engine meets with
  `fsync`.

**GCS additionally rate-limits mutations of a single object to roughly 1/s**,
per object. S3 took the identical pattern unthrottled. The engine's `term`,
`voted_for`, `snapshot` and `owner` keys are single-slot objects, so on GCS
that limit binds on the election path specifically, not on log appends (which
each write a distinct key).

### Cost

Object stores bill per request; this engine writes one request per entry per
node; so the bill scales linearly with append rate. Approximate published PUT
prices retrieved August 14, 2026 — ~$0.005/1,000 requests for S3, Azure Blob,
GCS and OCI, and ~$0.0014/1,000 for OSS after a large free tier — per **node**:

| Sustained append rate | PUTs/month/node | S3 / Azure / GCS / OCI | Alibaba OSS |
|---|---|---|---|
| 1 entry/s | ~2.6 M | ~**$13**/month | ~$0 (inside the free tier) |
| 100 entries/s | ~259 M | ~**$1,300**/month | ~**$220**/month |

On a three-node cluster the 100 entries/s row is roughly **$3,900/month in
request charges alone**, before storage or egress — and note that 100
entries/s is already above every measured throughput ceiling in the table
above, so that row describes a cluster that cannot exist rather than one that
is merely expensive.

Stated as a conclusion rather than left as an exercise: **sustained
high-append-rate workloads are financially prohibitive in this engine, not
merely slow.**

Per **three-node cluster** and by log layout, at ~$0.005 per 1,000 PUTs (S3,
Azure Blob, GCS, OCI). `B` is the entries carried by an AppendEntries that
carries any. The leader writes one PUT per entry under either layout; each
follower writes one per entry under `per_entry` and one per AppendEntries under
`segmented`:

| Sustained rate | `per_entry` (3 PUTs/entry) | `segmented`, B = 1 | B = 2 | B = 10 | B → ∞ |
|---|---|---|---|---|---|
| 1 entry/s | ~$39/month | ~$39 | ~$26 | ~$16 | ~$13 |
| 10 entries/s | ~$389/month | ~$389 | ~$259 | ~$156 | ~$130 |

The honest reading: **at steady state with one client command in flight, B is
about 1 and the segmented layout saves almost nothing.** When AppendEntries
batch (a burst, a slow follower, many concurrent clients) it saves a third of
the bill at B = 2, approaching two thirds, the whole follower share, as B
grows. Its
large savings are on catch-up and recovery, which are not steady-state costs.
The leader's PUT per entry is the floor, about $13/month per entry/s per
cluster, unless `write_behind` is on. With it on, the leader pays one PUT per
flush, so the floor divides by the leader's `B`. The combined hard state's effect on
cost is negligible: one fewer PUT per election per node.

Storage itself is negligible for the workloads this engine suits. The
recurring cost of a CI or small production deployment is the *bucket*, not the
objects in it.

## Object layout

Everything one engine instance owns lives under one `prefix` in one bucket:

```
<prefix>/term                             "42"
<prefix>/voted_for                        "7" | "none"
<prefix>/log/00000000000000000042         {"term":3,"index":42,"command":"…","type":0}
<prefix>/snapshot                         {last_included_index, …}   ← the commit point
<prefix>/snapshots/00000000000000000041   retained predecessors (retention > 1 only)
<prefix>/owner                            {owner_id, epoch, started_at}  (fencing only)
```

Three properties of this layout carry weight:

1. **One object per log entry, by default.** The file engine keeps its whole
   log in one file and rewrites it on truncation — correct for a filesystem,
   catastrophic in an object store, where the same idiom is a full re-upload
   of the entire log for an O(1) logical change. One object per entry makes
   `append_log_entry` exactly one PUT, `truncate_log` and
   `delete_log_entries_before` bounded batches of DELETEs touching only
   affected entries, and recovery one LIST plus one GET per live object. The
   opt-in segmented log (below) keeps all of that for bounded runs of entries.
2. **20 zero-padded digits.** `2^64 - 1` is 20 digits, so padding every index
   to exactly 20 makes lexicographic listing order identical to numeric index
   order for every representable `LogIndex`. Without it `log/10` sorts before
   `log/9`. Recovery correctness rests on this, which is why a key whose
   suffix is not exactly 20 digits is treated as **corruption**, not as a key
   to skip.
3. **The same JSON codec `file_persistence_engine` writes**, so records stay
   byte-comparable and a bucket can be diffed against a data directory during
   a migration.

### Layout options

Two independent options choose how the hard state and the log are stored. The
defaults are the layout above, byte for byte and request for request.

| Option | Default | Alternative | What the alternative changes |
|---|---|---|---|
| `hard_state` | `separate`: `term` and `voted_for` are two objects, written term first | `combined`: `{"term":42,"voted_for":7}` in `<prefix>/term` | One PUT per hard-state write instead of up to two; election rule `2 × p99(PUT)` |
| `log` | `per_entry`: `log/<index>` holds one entry | `segmented`: `log/<first>` may hold `{"first_index":f,"last_index":l,"entries":[…]}` | One PUT per segment of an AppendEntries run instead of one per entry; one GET per segment at recovery |

Under `segmented`, `max_segment_entries` (default 100, at least 2) and
`max_segment_bytes` (default 1 MiB, at most `max_object_bytes`) bound a
segment. An entry larger than `max_segment_bytes` is written alone. Each
element of `entries` is exactly the record a per-entry object holds.

- **Append.** A run that continues the log is cut greedily into segments, and
  each segment is one PUT, up to `append_concurrency` in flight. A run that
  does not start at the last index + 1 is refused before any request.
- **Truncation** deletes the objects at or above the cut, highest first, then
  rewrites the segment that straddles the cut in place, keeping its entries
  below it (`If-Match` under fencing). That rewrite is the commit point.
- **Compaction** deletes the objects wholly below the cut, lowest first, then
  writes the straddling segment's surviving part under its own key and deletes
  the segment. That DELETE is the commit point.
- **Recovery** walks the chain from the lowest log object: each next object
  starts one past the previous one's end. An object off the chain is a
  *stray*, left by a crash part-way through one of the operations above. It is
  ignored on load and deleted before the next log write.

Each node's layout is its own, and nothing on the wire changes, so a rolling
upgrade can switch nodes one at a time.

| From → to | In place? | How |
|---|---|---|
| `separate` → `combined` | yes | lazily, at the first hard-state write (see below) |
| `combined` → `separate` | **refused** at construction | restore from a backup taken before the upgrade |
| `per_entry` → `segmented` | yes | nothing to do: per-entry objects are already a valid segmented log |
| `segmented` → `per_entry` | **refused** while any segment exists | restore from a backup taken before the upgrade, or wait until compaction has removed every segment |
| any upgraded prefix → an older binary | **refused** by the older binary | its strict parse of `term` and `log/<index>` |

**Fencing under `segmented` detects slightly less.** A stale appender whose
object starts strictly inside one of the owner's segments is not refused by
the create-only PUT, because the key is new. The owner's next load discards it
as a stray instead of loading it. The other stale-appender cases are refused as
before. Details: `.kiro/specs/object-store-batched-layout/` design, Part 3.

### Snapshot retention

`object_persistence_options::snapshot_retention` counts generations
*including* the live `<prefix>/snapshot`. The default is `1` — the shipped
behaviour — which writes no `<prefix>/snapshots/` object at all. `0` is
rejected at construction rather than silently read as "keep none", which would
delete the copy of the snapshot just written.

**Retention is not backup.** Retained snapshots live under the same prefix, in
the same bucket, owned by the same engine, and a takeover or a `--force`
restore can remove them. If you need a copy that survives losing the prefix,
use [backup](#backup-and-restore), which writes somewhere else on purpose.

### Combined hard state, and upgrading a bucket in place

`object_persistence_options::hard_state = hard_state_layout::combined` stores
the term and the vote together in `<prefix>/term` as
`{"term":42,"voted_for":7}` (`null` for no vote), so every hard-state write is
one PUT instead of up to two. The default, `separate`, is the layout above.

Setting `combined` on a prefix a default engine wrote migrates it in place,
lazily. Startup writes nothing for it (under fencing, nothing but the owner
object, as always). The first hard-state write then PUTs the combined record
to `<prefix>/term`, which is the commit point, and after it PUTs `retired` to
`<prefix>/voted_for` if that object exists. Under `compare_and_swap` both are
`If-Match` the versions the engine loaded, so a duplicate still running the
old layout is refused at its next write to either. A transient failure of the
second PUT does not fail the call: it is retried before the next hard-state
write, and by a fenced engine when it next starts.
`vote_slot_retirement_pending()` and `last_vote_slot_retirement_error()`
report it until then. Nothing else is needed, and nodes can be switched one at
a time: nothing on the wire changes.

**Runbook: an older binary that says `corrupt object <prefix>/term` on an
upgraded bucket is the rollback guard working, not damage.** Once a node has
written a combined record it may have acknowledged a vote that exists only
there. Every earlier engine and backup tool parses `<prefix>/term` as a bare
decimal and refuses the JSON record instead of falling back to the stale
`voted_for` beside it. Reading that stale vote is how a rolled-back node would
vote twice in one term. Do not "repair" the object by writing a decimal into
it. To go back to an older binary, restore the prefix from a backup taken
before the upgrade, or stay on this build with `combined` set. A build of this
engine configured `separate` refuses the same prefix, with a message saying
that an in-place downgrade is not supported.

**Take a backup before switching a prefix to `combined`; it is the way back to
an older binary.** After the switch, back the prefix up with the
`raft_object_backup` from this build or a later one. It reads the combined
record: `create` takes the term and the vote from `<prefix>/term` (`null`
appears in the manifest as `"none"`, as the legacy sentinel does), ignores the
`retired` vote slot or any vote a crash left beside the record, and writes
`format_version` 2 in the manifest. A prefix still on the default layout gets
`format_version` 1 and the same manifest, byte for byte, that the older tool
writes. No new flag is involved.

An older `raft_object_backup` refuses an upgraded prefix, loudly at every step:

- its `create` parses `<prefix>/term` as a bare decimal, fails with `corrupt
  object <prefix>/term`, and writes no manifest;
- its `verify` of a `format_version` 2 backup reports `manifest_version` ("is
  not the 1 this build understands") and checks nothing else;
- its `restore-clone` and `restore-seed` run that `verify` first and abort
  before writing anything.

So verify and restore a backup of an upgraded prefix with this build or later
as well. A clone restore copies the bytes, so the restored prefix is still
upgraded and needs an engine configured `combined`. A seed restore writes a
decimal `term` and no vote, which an engine at either setting starts from; a
`combined` engine migrates it on its first hard-state write.

If `create` on a running node fails with `"retired" is written only after term
holds a combined hard-state record`, the node migrated between the tool reading
`term` and reading `voted_for`. The copy would hold no state the node ever had,
so no manifest is written. Take the backup again.

## Fencing

Two modes, on `object_persistence_options`:

- **`fencing_mode::none`** (default) — the shipped behaviour. Single-writer by
  assertion: no `<prefix>/owner` object, no conditional headers, no extra
  request. Exactly one process may own a `{bucket, prefix}` pair, the same way
  exactly one process owns the file engine's directory.
- **`fencing_mode::compare_and_swap`** — conditional writes wherever they cost
  no extra round trip, plus an owner object and a latch. Requires a non-empty
  `owner_id`; setting `owner_id` without this mode is rejected, so a fencing
  knob can never be set on an engine that is not fencing.

### What compare_and_swap actually gives you

**It detects a second writer. It does not arbitrate between them.** This
distinction was corrected by a live run, against the opposite assumption:

> A takeover alone does **not** invalidate the previous engine's writes. When
> a new owner takes over, the *stale* writer keeps succeeding and the *new
> owner* is refused, until the winner writes the same object. The loser finds
> out only once the other party has written that object.

The safety argument is a chokepoint one: a second writer cannot cause a Raft
*safety* violation without first writing `term` or `voted_for`. A candidate
must persist an incremented term and a self-vote before sending any
RequestVote; a follower must persist a term bump before replying. There is no
path to a second leader, a diverging committed log or a double vote that does
not pass through one of those two single-slot objects, and both carry an
`If-Match` CAS.

That covers safety and not corruption, so log-entry PUTs additionally carry a
**create-only** precondition: a stale leader appends without changing its
term, and left unconditional it would interleave log objects with the rightful
owner's for as long as it ran, surfacing only at the next recovery as a short
log. Create-only never rejects a legal write, because legitimate re-use of an
index is always preceded by `truncate_log`'s DELETE.

### What it does not cover

`<prefix>/snapshot`, `<prefix>/snapshots/<index>` and **every DELETE** stay
unconditional. This is a bounded residual, stated rather than papered over: a
second writer whose only interaction with the prefix is a snapshot overwrite
or a truncation goes undetected. Conditioning them would buy little — both
follow a term or append write in every sequence a live node produces — and an
honest statement of the gap is worth more than a claim of totality an operator
might rely on.

### The latch

A refused precondition raises `persistence_fenced_error`, naming the key, the
expected version and the provider. **Every subsequent mutating call throws the
same error without contacting the store, and the latch is not clearable.**
Restart the process; do not attempt to continue.

The type is deliberately distinct from the `std::runtime_error` an ordinary
store failure produces — a transient failure says nothing about ownership, and
conflating the two in either direction is the mistake it exists to prevent.
The Raft layer needs no special handling; operator automation can catch this
one specifically to tell "split brain" from "the network broke".

### Takeover procedure

Construction over a prefix another owner holds **fails**, naming the recorded
owner. To take it over deliberately:

1. **Confirm the recorded owner is stopped and will not return.** Nothing in
   the engine can check this for you, and a takeover against a live owner
   produces two writers, which is the situation fencing exists to detect
   rather than to create.
2. Read the recorded epoch from `<prefix>/owner`.
3. Set `object_persistence_options::takeover_epoch` to a value **greater than**
   the recorded epoch. An epoch that does not advance is not a takeover and is
   rejected rather than ignored.
4. Construct. The claim is recorded with the new epoch, leaving an audit trail.

A restart by the *recorded* owner needs none of this: it is allowed, and
read-increments the epoch. That is deliberate — a crash-restart and a
duplicated deployment are indistinguishable at construction time, since a
duplicate runs with the same node identity, and a crash-restart must succeed.
Construction refuses only a **different** `owner_id`; the duplicate is caught
by the first conditional write. This is why the fence lives on the writes at
all: a construction-time check cannot be the fence, and building one there
would only look like protection.

### Alibaba OSS cannot fence, and that is a finding rather than a gap

`alibaba_oss_client` satisfies `key_object_store` and **not**
`conditional_key_object_store`, because OSS's `If-Match` on PutObject is
rejected `400 NotImplemented` for a *current* ETag as well as a stale one —
there is no ETag-predicated write to build a fence on. So
`fencing_mode::compare_and_swap` over OSS is a **compile-time** unavailable
option. It does not silently degrade to unconditional writes.

That is the single most important consequence of the provider comparison, and
it is decided against the provider this engine's reference implementation came
from. A fence that is believed in and absent is worse than no fence.

Two adjacent facts, both live-verified, both traps for anyone writing a sixth
provider:

- **OSS's create-only rejection is `409 FileAlreadyExists`**; S3's `409
  ConditionalRequestConflict` means the *opposite* — a benign race to retry.
  **No client may map a bare status code to either meaning**; the service's own
  error code has to be read. Collapsing S3's 409 into 412 would turn a benign
  race into a permanent, unclearable latch.
- **OSS's conditional DELETE is accepted and ignored** — a stale `If-Match`
  returns 204 and deletes the object anyway. A conditional delete must never
  enter the concept without a per-provider live negative control.

## Limits

| Limit | Value | Why |
|---|---|---|
| `max_object_bytes` | **64 MiB** | Deliberately far below every provider's documented single-request limit (the smallest is 5 GB). The binding constraint is this engine's *shape*, not the service's: one retry, no multipart, no resumption, no progress reporting, and the mutex held for the whole round trip. The cap turns "this deployment has outgrown a single-PUT persistence engine" into a loud error at the first snapshot that reaches it. Configurable upward by an operator who has measured their own case; `0` is rejected |
| `write_retries` | **1** | PUT-only, and **unconditional-PUT-only**. A conditional write is never retried: a retry cannot distinguish "my write landed and the response was lost" from "I lost the race" |
| `max_segment_entries` | **100** | Entries per segment under `log_layout::segmented`; at least 2, since one entry is always a singleton. A non-default value under `per_entry` is refused rather than ignored |
| `max_segment_bytes` | **1 MiB** | Body size per segment under `log_layout::segmented`, at most `max_object_bytes`. Swept from 64 KiB to 8 MiB in October 2026 (see "Segment size"): a follower's 100 × 32 KiB run is at most 1.1 s at 1 MiB on S3, GCS, OCI and Azure Blob, against the 5 s `append_entries_timeout`. Raise to 4-8 MiB for 3-5x faster recovery of a large log; below 512 KiB a run of large entries needs several waves of PUTs |
| `write_behind` | **off** | Leader group commit, opt-in (see [Write-behind group commit](#write-behind-group-commit)). Off, every append is a PUT before the call returns, as it always was. On, appends stage and the durability barrier flushes them as one run. Accepted with either log layout and either fencing mode |
| `append_concurrency` | **8** | Log PUTs `append_log_entries` keeps in flight for one AppendEntries. Used only on a store that declares `supports_concurrent_requests` (all five shipped clients do); any other store gets one at a time. Still one PUT per entry: the request count and the keys are the same at every setting. `0` is rejected |

## Per-provider configuration

The engine is one class template over a `key_object_store`. Every provider
supplies the same three things — a client config, a bucket, a prefix — and the
per-provider differences are entirely in authentication.

**Fencing is a template argument, not a runtime option**, which is what makes
it unavailable at compile time on a store that cannot express a precondition:

```cpp
// Unfenced — the shipped behaviour, and the default.
using engine_t = kythira::object_store_persistence_engine<Store>;

// Fenced. The constraint sits on the alias's own parameter, so asking for this
// over a store that is not a conditional_key_object_store fails at the NAME
// rather than deep inside the template.
using fenced_engine_t = kythira::fenced_object_store_persistence_engine<Store>;
```

`object_persistence_options` carries the runtime knobs — `snapshot_retention`,
`write_retries`, `owner_id`, `takeover_epoch`, `verify_checksums`,
`max_object_bytes`. Setting `owner_id` or `takeover_epoch` on an *unfenced*
engine is rejected at construction, so a fencing knob can never sit on an
engine that is not fencing.

Every example below carries the same warning, because it applies to all five:

> **Persistence writes cost a network round trip, on the election hot path.**
> `save_current_term` and `save_voted_for` return only once the service has
> acknowledged the write. That is the point, and it is stronger than
> `file_persistence_engine`, which does not even fsync — but it makes those
> calls WAN-latency operations. **Size election timeouts against a measurement
> taken from where your nodes actually run**, not against the figures in this
> document and not against local-disk intuition. If your election timeout is
> shorter than a round trip, a node cannot persist its vote before the election
> it is voting in has timed out.

### AWS S3

```cpp
#include <raft/aws_s3_client.hpp>
#include <raft/object_store_persistence.hpp>

kythira::aws_client_config aws;
aws.region = "us-east-1";                 // credentials come from the SDK chain

kythira::object_persistence_options opts;
opts.snapshot_retention = 3;
opts.owner_id = "node-1";                 // required under fencing, rejected without

kythira::fenced_object_store_persistence_engine<kythira::aws_s3_client> engine{
    kythira::aws_s3_client{aws}, "my-raft-bucket", "clusters/alpha/node-1", opts};
```

Prerequisites: a bucket with public access blocked. Credentials from the AWS
SDK's own chain, exactly as the engine takes them. Least-privilege grant:
object `Get`/`Put`/`Delete` under the prefix plus prefix-conditioned
`ListBucket` — **no bucket administration**; see
`scripts/ci-cloud-credentials/aws/policies/object-persistence.json`.

### Azure Blob

```cpp
#include <raft/azure_blob_client.hpp>

kythira::azure_blob_config blob;
blob.account = "mystorageaccount";        // https://<account>.blob.core.windows.net
// blob.azure.credential — injected TokenCredential; otherwise the default chain

kythira::fenced_object_store_persistence_engine<kythira::azure_blob_client> engine{
    kythira::azure_blob_client{blob}, "raft", "clusters/alpha/node-1", opts};
```

Prerequisites: a **ZRS** storage account (see
[above](#azure-the-one-n1-the-operator-controls)) and a private container.
**AAD bearer tokens only** — this client takes no storage-SDK dependency and
writes no SharedKey signing at all, deliberately. The identity needs
`Storage Blob Data Contributor`, which **an Owner role does not imply**: a
subscription Owner can create the account and the container and still not
write a single blob.

### Google Cloud Storage

```cpp
#include <raft/gcp_gcs_client.hpp>

kythira::gcp_client_config gcp;
gcp.project_id = "my-project";            // ADC supplies the credentials

kythira::fenced_object_store_persistence_engine<kythira::gcp_gcs_client> engine{
    kythira::gcp_gcs_client{gcp}, "my-raft-bucket", "clusters/alpha/node-1", opts};
```

Prerequisites: a bucket with **uniform bucket-level access** and public access
prevention. Credentials from Application Default Credentials, which is how a
node on GCE authenticates. Consider disabling **soft delete**: its 7-day
default bills deleted objects for a week, and on a create-and-delete workload
that is the dominant line item.

Two GCS-specific behaviours: its version token is a **decimal generation
counter** carried as a query parameter, not an ETag header; and it
**rate-limits mutations of a single object to ~1/s**, which binds on the
single-slot keys.

### OCI Object Storage

```cpp
#include <raft/oci_object_storage_client.hpp>

kythira::oci_object_storage_config oci;
oci.oci.region = "us-phoenix-1";
// oci.namespace_name left empty — resolved once at construction via GET /n/

kythira::fenced_object_store_persistence_engine<kythira::oci_object_storage_client> engine{
    kythira::oci_object_storage_client{oci}, "my-raft-bucket",
    "clusters/alpha/node-1", opts};
```

Prerequisites: a bucket in the compartment. The **namespace is deliberately
optional** and resolved at construction, so an operator does not have to know
it. Least-privilege grant: `manage objects in compartment` — see
`scripts/ci-cloud-credentials/oci/policies/object-persistence.txt`, and read
its `where`-clause warning before narrowing it further.

OCI's ETag is a **UUID**, not the content MD5; the content MD5 comes back
separately as `opc-content-md5`.

### Alibaba OSS

```cpp
#include <raft/alibaba_oss_persistence.hpp>

kythira::alibaba_client_config oss;
oss.region = "ap-southeast-1";
oss.access_key_id = …; oss.access_key_secret = …;   // or STS + security_token

// fenced_object_store_persistence_engine<alibaba_oss_client> does NOT compile:
// the alias's own constraint rejects the store by name. See above.
kythira::alibaba_oss_persistence_engine engine{oss, "my-raft-bucket",
                                               "clusters/alpha/node-1"};
```

`alibaba_oss_persistence_engine` is the named instantiation this whole design
was hoisted out of; the generic engine over `alibaba_oss_client` is the same
thing. See `docker/alibaba_quorum_manager/README.md` for the ESS quorum
manager alongside it.

**This is the one provider where fencing is unavailable at compile time.** Do
not work around it.

## Verification status

Written as of **August 21, 2026** and updated **October 2, 2026** for OCI's
August 23 result; to be updated as this changes rather than written
aspirationally. **All five providers are now green in CI under the
least-privilege grants**: OCI, the last, passed 3/3 dispatched runs at 5/5
cases on August 23, 2026, once the tenancy-side `404 BucketNotFound` flake
described below stopped.

**Now also verified from CI, not only from a developer machine.** Run
[32432380565](https://github.com/crawlins/kythira/actions/runs/32432380565)
dispatched every provider's object-persistence bundle against its real
service, under the least-privilege CI grants rather than an operator's
credentials — which is a stronger claim than the earlier runs, because those
authenticated as principals that already held broader policies:

| Provider | CI result |
|---|---|
| AWS S3 (x64 **and** arm64) | **pass**, 5/5 |
| Azure Blob | **pass**, 5/5 |
| Alibaba OSS | **pass** |
| GCS | **pass**, 5/5 on re-run ([32441129124](https://github.com/crawlins/kythira/actions/runs/32441129124)). The first attempt was 4/5: `backup_verify_restore_read_back` hit a Google-side `502` ("the server encountered a temporary error… please try again in 30 seconds") after the client's retry policy was exhausted |
| OCI Object Storage | **did not run** in this dispatch — the read-only pre-flight was declined `404 BucketNotFound`, the tenancy flake below. **Passed 5/5 in each of 3 later dispatched runs, August 23, 2026** |

**Those two failures were not the same kind of failure**, and acting on the
difference is why one was re-run and the other was not. GCS was told by the
service, in the service's own words, that the request failed temporarily — so
a re-run was legitimate, and it passed 5/5. OCI was told that a bucket which
plainly exists does not exist: a wrong answer whose cause is unknown, where
re-running until green would launder the unknown away. **OCI was therefore not
re-run against that flake.** It was re-run only after the flake had stopped on
its own (August 22), and then passed 3/3.

**A note on the GCS transient, which is a design observation rather than a
defect.** The 502 exhausted the client's retry policy, and
`object_persistence_options::write_retries` defaults to **1**. That default is
right for the engine's hot path — and conditional writes are never retried at
all, deliberately — but the failure landed in *restore*, which is a bulk
unconditional write far from any Raft invariant. One retry is thin there
against ordinary cloud transients. Raising it is a per-caller decision the
option already supports; the default is not changed here.

| Provider | Engine | Live-verified | Documentation-derived |
|---|---|---|---|
| AWS S3 | `aws_s3_client` | conditional writes, checksums, list-after-write, fencing race, backup/verify/restore, latency (Aug 16–19 2026, `us-east-1`) | durability-on-response (N1) |
| Azure Blob | `azure_blob_client` | as above (Aug 17–19 2026, `eastus`, Standard_ZRS) | durability-on-response (N1), which is additionally account configuration |
| GCS | `gcp_gcs_client` | as above (Aug 17–19 2026, `us-central1`) | durability-on-response (N1) |
| OCI Object Storage | `oci_object_storage_client` | as above (Aug 17–19 2026, `us-phoenix-1`) | durability-on-response (N1) — and here the vendor **publishes no wording at all**, so this is weaker than the other four |
| Alibaba OSS | `alibaba_oss_client` | conditional create-only, checksums, list-after-write, backup/verify/restore, latency (Aug 14–19 2026, `ap-southeast-1`) | durability-on-response (N1). **Fencing is not verified because it does not exist** on this provider |

**All five providers run the same five checks from one shared file**
(`tests/object_persistence_real_cases.hpp`), so "S3 passes" and "GCS passes"
are the same claim rather than five copies that drift. Alibaba runs three of
the five plus four of its own: the fencing case is *uninstantiable* over a
store that is not a `conditional_key_object_store`, and would not compile.

**A resolved caveat specific to OCI, kept because it shaped the evidence.**
Between August 12 and 22, 2026 the CI tenancy episodically declined valid
Object Storage requests with `404 BucketNotFound` — identical bursts measured
0.00% to 13.40%, on every verb. It stopped on its own, and OCI's suite has
since passed 3/3 CI runs at 5/5 cases.

**It was a property of one tenancy, never of the client or of OCI**, and the
service's own log said so: a declined `ListObjects` had a byte-identical twin
that succeeded seconds earlier under the same credential, and the same request
from a differently-privileged principal was declined zero times in five
thousand. Operators should not read any of it as guidance about
`oci_object_storage_client`. Every OCI figure in this document measured before
August 22 was taken *through* that noise, which makes it stronger evidence than
it looked at the time, not weaker.

**What the local test tiers cannot prove, recorded because assuming otherwise
cost real defects.** The real tier found two shipped OCI defects, each of
which alone made OCI object persistence non-functional against the live
service, and **neither was catchable locally**:

- Endpoint derivation is never exercised, because every unit case sets
  `endpoint_override`, which replaces the host outright.
- A signature-verifying mock verifies the signature against the bytes that
  *arrived*, so a client and a mock that encode a request identically **always
  agree, however wrongly they both encode**. A signature bug of that shape is
  only observable against a party that computes the signature independently.

## Running a cluster on S3

`multi_raft_node --persistence s3` runs a whole cluster on this engine, one
engine over `aws_s3_client` per group
(`.kiro/specs/multi-raft-node-s3-persistence/`). It exists so a Tier C, D or E
row can take S3 as the durable log, and so a cluster can be exercised end to
end with no cloud account. **`multi_raft_node` is a measurement host, not a
server**: it has no data-path authentication, and S3 persistence does not
change that.

Every host of a cluster can share one bucket and one prefix. Each appends
`/nodes/<node-id>` itself, so group `g` of node `n` lives under
`<prefix>/nodes/<n>/groups/<g>/`, and the host prints that prefix at startup.

Three hosts against a LocalStack (or MinIO) on `localhost:4566`:

```sh
export AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test
curl -X PUT http://localhost:4566/kythira-runs        # create the bucket
for id in 1 2 3; do
  multi_raft_node --node-id $id --bind 127.0.0.1 \
    --raft-port 900$id --data-port 910$id --control-port 920$id \
    --peer 1=http://127.0.0.1:9001 --peer 2=http://127.0.0.1:9002 \
    --peer 3=http://127.0.0.1:9003 \
    --persistence s3 --s3-bucket kythira-runs --s3-prefix runs/local-1 \
    --s3-endpoint http://localhost:4566 --s3-region us-east-1 &
done
```

Against real S3, drop `--s3-endpoint`, give the region, and size the election
timeout from a measurement (see below):

```sh
multi_raft_node --node-id 1 ... \
  --persistence s3 --s3-bucket my-bucket --s3-prefix runs/$RUN_TAG \
  --s3-region eu-west-3 --election-timeout-min 1500 --election-timeout-max 3000 \
  --s3-log segmented --s3-write-behind 1
```

What to know:

- **Defaults.** `--s3-fencing cas` and `--s3-hard-state combined`; every other
  `--s3-*` flag defaults to `object_persistence_options`'s own default, so
  group commit (`--s3-write-behind 1`) is on only when a row asks for it.
  `--help` lists every flag. An option the engine rejects refuses startup with
  the engine's message, before any server binds.
- **Election timeout.** Below 1000 ms the host warns, because a vote must be
  persisted inside the election and one S3 PUT can take longer than that from
  where the nodes run; see [Election-timeout sizing](#election-timeout-sizing).
  It is a warning, not a refusal: against a loopback LocalStack the short
  default is fine.
- **A reused node id is refused.** Under `cas` each group's owner is
  `<node-id>@<own Raft URL>`, so restarting a host with the same flags claims
  the next epoch, while a second process under the same id, at another
  address, is refused at startup naming the owner object. The cost is that a
  host that comes back at a different address under the same prefix is
  refused too; use a fresh, run-scoped prefix.
- **Status.** The control port's `/describe` reports `"persistence":"s3"`,
  an `"s3"` object (bucket, host prefix, endpoint, layouts, write-behind,
  append concurrency and `flushes`, the engines' flush count) and
  `"fenced_groups"`. A group fenced at run time is logged once and listed
  there; the host does not retry or clear the latch.
- **Credentials and endpoints** follow the backup tool: the SDK default chain
  only, and `AWS_ENDPOINT_URL` is not read (see
  [S3 region and endpoint](#s3-region-and-endpoint)).

## Future work

The two limits this section used to list are closed as opt-in layouts
(`.kiro/specs/object-store-batched-layout/`): `log_layout::segmented` carries a
run of entries in one object, and `hard_state_layout::combined` writes the term
and vote as one. What remains:

- **Write-behind is opt-in.** By default the leader still writes one PUT per
  client command under `node`'s mutex, and the cluster's commit rate stays at
  about `1 / p50(PUT)`. With `write_behind` it is about `B / p50(flush)`,
  measured at 2.2-4.5x on the five providers with 16 submitters. Whether it
  becomes the default is open: it trades a higher median commit latency for
  throughput and a lower tail.
- **No in-place downgrade.** A prefix with a combined `term` or a segment is
  refused by an older binary and by a build configured for the older layout.
  The way back is a backup taken before the upgrade. An offline
  `raft_object_backup relayout` verb is specified but optional (task 11).
- **Segment size on Alibaba OSS is half measured.** The sweep's follower,
  truncation, recovery and write-behind cases did not run there, because the
  CI's RAM session lasts 30 minutes and OSS is the slowest provider from the
  runner. A longer session, or a re-login between cases, would finish it.

## Backup and restore

`include/raft/object_store_backup.hpp` provides `object_store_backup<Store>`
with `create`, `list`, `verify`, `restore_clone` and `restore_seed`. It takes a
store and a prefix, never instantiates the persistence engine, and cannot be
reached from the Raft hot path.

The CLI is `cmd/raft_object_backup`. It exists because operators do not have a
C++ compiler in a recovery window, and it is built **whatever providers this
build contains** — a build with every cloud gate off still produces a working
binary that names all five providers as absent and says what would enable each.
"Command not found" is the wrong thing to hand someone mid-outage.

```
raft_object_backup --help          # verbs, options, exit codes, and which
                                   # providers this binary actually carries
```

Exit codes are scriptable, and `verify` has its own:

| Code | Meaning |
|---|---|
| 0 | success; for `verify`, the backup is clean |
| 1 | usage error, or a provider not compiled into this binary |
| 2 | the operation failed — could not be done |
| 3 | `verify` completed and found problems — done, and the answer is bad |

2 and 3 are separate deliberately. "I could not check this backup" and "I
checked it and it is broken" call for different responses, and a script gating a
restore on `verify` has to tell them apart: the first is worth retrying, the
second never is.

**Credentials** come from the environment only, never from flags: a secret on
the command line is readable by every user on the host through `ps` and
`/proc`. `raft_object_backup --help` lists the variables for each provider
compiled into that binary.

| Provider | Variables |
|---|---|
| `s3` | the AWS SDK default chain (`AWS_PROFILE`, `AWS_ACCESS_KEY_ID`, ...). The region comes from `--region`, else the SDK chain; the endpoint from `--endpoint` only, since `AWS_ENDPOINT_URL` is not read (see [S3 region and endpoint](#s3-region-and-endpoint)) |
| `azure-blob` | `KYTHIRA_AZURE_STORAGE_ACCOUNT` (the bucket options name the *container*, which is not enough to build the endpoint), plus the Azure SDK default chain |
| `gcs` | `GOOGLE_CLOUD_PROJECT`, plus Application Default Credentials |
| `oci-objectstorage` | `KYTHIRA_OCI_REGION`, and the variables for one auth mode (below) |
| `oss` | `KYTHIRA_ALIBABA_REGION`, `KYTHIRA_ALIBABA_ACCESS_KEY_ID`, `KYTHIRA_ALIBABA_ACCESS_KEY_SECRET`; `KYTHIRA_ALIBABA_SECURITY_TOKEN` for STS keys; optionally `KYTHIRA_ALIBABA_ENDPOINT_OVERRIDE` |

#### S3 region and endpoint

`--provider s3` takes two flags that no other provider does. They are not
secrets, so unlike credentials they may be flags, and a one-off restore then
records its target in the shell history.

- `--region <name>` is the region to sign for. Without it the AWS SDK chain
  decides, first match wins: `AWS_DEFAULT_REGION`, then `AWS_REGION` (note
  the order: the reverse of the AWS CLI), then `region` in the config
  profile, then EC2 instance metadata, then `us-east-1`.
- `--endpoint <url>` points the tool at an S3-compatible service such as
  LocalStack or MinIO. It implies path-style addressing and turns off the
  instance-metadata lookup. The URL needs an `http` or `https` scheme, may
  carry a port, and may not carry a path, query, fragment or `user:pass@`;
  a value that breaks a rule is exit 1 before any connection. Plaintext
  `http` to anything but `localhost`, `127.x.x.x` or `[::1]` warns.

`AWS_ENDPOINT_URL` and `AWS_ENDPOINT_URL_S3` are **not read**, by the linked
SDK or by the tool, which warns when either is set without `--endpoint`. CI
points `AWS_ENDPOINT_URL` at an unrelated build cache, and a restore must not
follow ambient state to the wrong cloud. The OCI and OSS providers refuse both
flags and name their own `KYTHIRA_*_REGION` / `KYTHIRA_*_ENDPOINT_OVERRIDE`
variables instead.

```sh
# LocalStack (accepts any keys and any region)
AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test \
raft_object_backup list --provider s3 \
    --endpoint http://localhost:4566 \
    --dest-bucket my-backups --dest-prefix raft-backups

# MinIO, with its own keys and region
AWS_ACCESS_KEY_ID=minioadmin AWS_SECRET_ACCESS_KEY=... \
raft_object_backup restore-clone --provider s3 \
    --endpoint https://minio.internal:9000 --region us-east-1 \
    --dest-bucket my-backups --dest-prefix raft-backups \
    --backup-id 20260817T221530Z \
    --target-bucket my-raft --target-prefix node-1
```

CI runs the full `create`, `list`, `verify`, `restore-clone` cycle this way
against LocalStack on every push (`scripts/backup-s3-localstack-cycle.sh`).

OCI and Alibaba have no SDK in this tree, so nothing else would supply their
credentials; the tool reads the same `KYTHIRA_OCI_*` and `KYTHIRA_ALIBABA_*`
names the real-cloud suites and CI already export. OCI's mode comes from
`KYTHIRA_OCI_AUTH`:

| `KYTHIRA_OCI_AUTH` | Also required |
|---|---|
| `api_key` | `KYTHIRA_OCI_TENANCY_ID`, `KYTHIRA_OCI_USER_ID`, `KYTHIRA_OCI_FINGERPRINT`, and a key |
| `security_token` | `KYTHIRA_OCI_SECURITY_TOKEN`, and its session key |
| `instance_principal` | nothing; any API-key or token variable that is set draws a warning and is ignored |

A key is exactly one of `KYTHIRA_OCI_PRIVATE_KEY_PEM` (the PEM itself) or
`KYTHIRA_OCI_PRIVATE_KEY_FILE` (a path), plus
`KYTHIRA_OCI_PRIVATE_KEY_PASSPHRASE` if it is encrypted. When
`KYTHIRA_OCI_AUTH` is unset the mode is `security_token` if
`KYTHIRA_OCI_SECURITY_TOKEN` is set and `api_key` otherwise. Instance
principal is never inferred: off OCI its metadata fetch fails only after a
timeout, with a message about `169.254.169.254` rather than the variable you
forgot. `KYTHIRA_OCI_NAMESPACE` skips the tenancy namespace lookup
(`GET /n/`) the client otherwise makes first.

A variable set to the empty string counts as unset. Everything missing for the
chosen mode is reported in one message, with exit 1, before any network call;
messages name variables and never print their values.

### Where backups go, and why not beside the source

Write backups to a **different bucket** from the source, and to a different
account or project where the provider supports it. Nothing enforces this,
because one bucket is better than no backup — but a backup sharing its source's
bucket shares its blast radius: one bad lifecycle rule, one mistaken prefix
delete, one compromised credential takes both copies.

### Layout

```
<backup_prefix>/<backup_id>/objects/<key relative to the source prefix>
<backup_prefix>/<backup_id>/manifest.json      ← written LAST
```

The manifest being written last is the entire commit protocol. None of the five
services offers cross-key atomicity, so "the manifest exists" is the only thing
"the backup finished" can mean. A run that dies partway leaves object copies and
no manifest, and `list` ignores any directory without one — so a torn copy is
invisible rather than restorable.

`backup_id` defaults to `YYYYMMDDTHHMMSSZ`: fixed width, zero padded, no
punctuation, so lexicographic listing order *is* chronological order, which is
the only ordering these stores give.

### `verify`, and why a quiesced claim is not believed

A backup taken from a running node is a **smear** across the copy window.
Objects are read one at a time and the node keeps writing, so the copy can hold
state read early and state written late. `backup_options::source_quiesced`
records the operator's claim about the source and the manifest carries it — but
a claim is not evidence, so `verify` checks the copy against itself:

| Check | Catches |
|---|---|
| index contiguity from the snapshot's `last_included_index + 1` | an entry truncated out from under the copy |
| every object's size and MD5 | destination corruption, or a copy rewritten after its checksum was taken |
| `current_term` ≥ every copied entry's term | term read early, node advanced and appended |

Contiguity is judged over the entries that actually **verified**, not over what
the manifest lists: an object named in the manifest but absent or corrupt is not
present, and it is a hole a restore would reproduce. So a deleted log object
reports twice — once as the object that broke, once as the gap it costs. That is
deliberate. The first says what happened; the second says what it means.

Every problem carries a machine-readable `kind` (`missing_object`,
`checksum_mismatch`, `size_mismatch`, `log_gap`, `term_regression`,
`unreadable_entry`, `manifest_version`) and a sentence naming the actual values,
because "this backup is bad" is not actionable and "log index 42 is missing
between the copied entries at 41 and 43" is.

A prefix on `log_layout::segmented` is read by the same rules. Its log objects
are judged by the engine's chain rule: from the lowest object, each next object
must start one past where the previous one ends. An object that starts *inside*
that range is reported as a **note** of kind `stray_object`, not as a problem,
and it does not change `ok` or `verify`'s exit status. The usual source is a
compaction that crashed between writing its new first segment and deleting the
segment it was cut from. The restored engine skips such an object and deletes it
before its first log write. An object that starts *past* the chain's end is
still a `log_gap`. Every entry inside a segment gets the `term_regression`
check, with at most one problem per segment. A segment also makes the manifest
`format_version` 2, so an older tool refuses the backup, as it does for a
combined `term`.

`verify` reports **every** problem it finds. Restore aborts on the **first**.
That difference is intentional: `verify` is a diagnostic run by a human who
wants the whole picture, and restore is a gate.

---

## Runbook — clone restore

**Use it for:** replacing the storage or the instance underneath an otherwise
unchanged node identity. The node keeps its identity; only where its bytes live
changes.

**Do not use it for:** starting a new cluster from an old one's state. That is
seed restore, and the two are separate operations precisely so they cannot be
confused.

### The safety check that is not ours to make

**Starting the restored node while the original is still running is a
split-brain, and no restore tool can prevent it.** Two copies of one node
identity, both writing, is an operational fact about what you have started — not
something this code can detect. Restore only when the original is
**definitively gone**.

What the tool *can* do, and does, is make the failure loud where the engine is
fenced (`fencing_mode::compare_and_swap`). If the backup carried an `owner`
object, it is restored with a strictly **higher epoch**. A returning original
then finds its own epoch stale and fences itself out on its **next write**
rather than writing alongside the restored node. The returned report names the
epoch the restored prefix now claims.

Under `fencing_mode::none` there is no owner object, none is invented, and this
protection does not exist. That is a reason to run fenced, not a gap in restore.

### Sequence

```sh
# 1. Find the backup. Ids sort chronologically, so the last line is the newest.
raft_object_backup list --provider s3 \
    --dest-bucket my-backups --dest-prefix raft-backups

# 2. Read the whole problem list yourself before committing to anything.
#    Restore runs verify again and aborts on the first problem — but it
#    aborts, it does not explain.
raft_object_backup verify --provider s3 \
    --dest-bucket my-backups --dest-prefix raft-backups \
    --backup-id 20260817T221530Z
#    exit 0 = clean, 3 = problems found, 2 = could not check

# 3. Confirm the original node is stopped and will not return.
#    If you cannot confirm it, stop here.

# 4. Restore. Add --force ONLY if the target is non-empty and you have read
#    what --force deletes (below).
raft_object_backup restore-clone --provider s3 \
    --dest-bucket my-backups --dest-prefix raft-backups \
    --backup-id 20260817T221530Z \
    --target-bucket my-raft --target-prefix node-1

# 5. Read the output: objects written, the owner epoch if the backup was
#    fenced, and how many keys --force deleted.
# 6. Start the node against the target prefix.
```

### Failure modes

| What you see | What it means | What to do |
|---|---|---|
| `refusing to restore backup … — <kind> on <key>` | `verify` found a problem; nothing was written | Run `verify` for the full list. A `log_gap` or `term_regression` means the backup is a smear — prefer an earlier one |
| `target prefix … already holds N object(s) — refusing to restore into it without force` | The target is not empty | Confirm you have the right target. If you do, re-run with `force`, which **deletes** the target's engine-owned keys first |
| `corrupt object …/owner: owner record has no epoch` | The backed-up owner object is malformed | The restored node could not fence a returning original. Investigate before starting anything |
| `… vanished from the backup during the restore — the target prefix … is now partially written` | The backup was mutated mid-restore | **Do not start the target.** Clear it and restore again from an intact backup |
| `… manifest_version on …/manifest.json: manifest format version 2 is not the 1 this build understands` | An older `raft_object_backup` against a backup of a prefix upgraded to the [combined hard state](#combined-hard-state-and-upgrading-a-bucket-in-place); nothing was written | Use the tool from the build that took the backup, or a later one |

`force` deletes the target's **engine-owned** keys — `term`, `voted_for`,
`snapshot`, `owner`, `log/*`, `snapshots/*` — and nothing else. Objects under
the prefix that the engine does not own are left untouched, because they are
yours and the engine neither reads nor writes them either.

`force` never merges. There is no code path that writes into a prefix still
holding another node's Raft state: the target is either emptied of engine-owned
keys or the restore throws. A merged log is two nodes' histories interleaved,
which is not a recoverable state and which no later repair untangles, so the
tool cannot produce one.

---

## Runbook — seed restore

**Use it for:** starting a **new** cluster from an old cluster's snapshot —
disaster recovery into fresh infrastructure, or forking an environment.

**Do not use it for:** replacing one node. That is clone restore.

### What is kept and what is discarded

| Kept | Discarded |
|---|---|
| the snapshot's state-machine bytes | the old cluster's configuration — **replaced** by your node set |
| `last_included_index` / `last_included_term` | the log, which is left empty |
| | the vote, which is cleared |
| | any joint-consensus marker |

The term is reset to the snapshot's `last_included_term`. The vote is cleared by
**omission** rather than by writing a `"none"` sentinel: an absent object is how
this engine spells "never voted", and writing the sentinel would make a fenced
engine's first vote an `If-Match` against an object it never wrote.

The joint-consensus marker is dropped deliberately. The old cluster may have
been mid-reconfiguration; a new one never is, and carrying the marker across
would seed every node with a membership change nobody proposed.

**The new cluster's node identities are unrelated to the old one's.** Nothing is
inherited but the state machine.

### Sequence

```sh
# 1. list and verify, exactly as for clone restore.

# 2. Decide the new node ids. They must match the representation this
#    cluster's engine uses — see the failure table below.

# 3. Seed. One prefix is written per node, at <target-prefix>/<node-id>.
raft_object_backup restore-seed --provider s3 \
    --dest-bucket my-backups --dest-prefix raft-backups \
    --backup-id 20260817T221530Z \
    --target-bucket my-raft --target-prefix fresh-cluster \
    --nodes alpha,beta,gamma

# 4. The output ends with the node ids and prefixes it wrote. THAT is what you
#    must configure — the tool writes the state, not the deployment.
# 5. Start the new cluster with exactly that node set and those prefixes.
```

Every target is checked before any is written, so a refusal on the third node
does not leave the first two seeded into a cluster that will never reach quorum.

### Failure modes

| What you see | What it means | What to do |
|---|---|---|
| `backup … has no snapshot, so there is nothing to seed a new cluster from` | The backup holds a log but no snapshot | Use a backup that has one. A log without a snapshot is the history of a cluster whose configuration is exactly what seed restore discards |
| `this cluster's node ids are numbers, but "…" is not one` | The engine's `NodeId` is an integer type and you supplied a name | Supply numeric ids. This is checked because the engine parses the configuration with `as_int64()` and **throws** on a string — the failure would otherwise surface when you start the new cluster, not here |
| `the snapshot in … records no cluster configuration, so the node-id representation this cluster uses cannot be determined` | The snapshot's node list is empty | The representation cannot be inferred and is not guessed. Seed from a backup whose snapshot has a configuration |
| `seed restore needs at least one node id` | Empty node set | Supply the new cluster's membership |
| `target prefix … already holds N object(s)` | One of the per-node prefixes is occupied | As for clone restore. Note nothing has been written yet — every target is checked first |

---

## Where this came from

The design, the per-provider probes and every measurement quoted above live in
`.kiro/specs/cloud-object-persistence/` — `requirements.md`, `design.md`, and
`spike-notes.md`, which records each finding with the run that produced it.

**The deferral chain, traceable in both directions.**
`.kiro/specs/alibaba-cloud-services/` Requirement **15.7** deferred snapshot
retention, backup catalogs and restore-into-fresh-cluster flows to "the
forthcoming cloud key-object persistence-engine spec", and Requirement **15.8**
deferred conditional-PUT fencing to the same place, recording that
`alibaba_oss_persistence_engine` "provides no fencing". This document is the
other end of both deferrals:

- 15.7's retention landed as
  [`snapshot_retention`](#snapshot-retention); its backup and restore landed as
  [backup and restore](#backup-and-restore) and `cmd/raft_object_backup`.
- 15.8's fencing landed as [`fencing_mode`](#fencing) — **and the answer for
  OSS specifically turned out to be that it cannot be done at all**, which is
  why that requirement was right to defer it rather than guess. OSS's
  `If-Match` on PutObject is `400 NotImplemented` for a current ETag as well as
  a stale one. The single-writer statement 15.8 made about the OSS engine still
  stands, and now stands as a measured finding rather than an unexamined
  constraint.

Requirement 15.7 also said that spec "SHALL NOT front-run that spec's
cross-provider decisions". It did not, and the hoist that turned
`alibaba_oss_persistence_engine` into an instantiation of the generic engine
was verified by re-running its existing tests unmodified — including the real
suite against a live bucket — because the only honest way to refactor a
component whose value is "it has run against a real service" is to prove the
tests that established that still pass without being edited into agreement.
