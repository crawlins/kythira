# Design Document

## Overview

| Component | Concept satisfied | Mechanism | Closest sibling |
|---|---|---|---|
| `key_object_store.hpp` | — (new concept) | Five-operation store interface + `conditional_key_object_store` refinement | `persistence.hpp`'s own concept (shape), `network_client` (compile-time-only seam) |
| `object_store_persistence.hpp` | `kythira::persistence_engine` | `alibaba_oss_persistence_engine`'s body, hoisted and made generic over the store | `alibaba_oss_persistence_engine` (**it is this, generalized**), `file_persistence_engine` (layout/mirror ancestry) |
| `object_store_backup.hpp` + `cmd/raft_object_backup` | — (tooling) | Object copy + manifest, off the Raft path entirely | none — first backup/restore surface in this repo |
| `aws_s3_client` | `conditional_key_object_store` | `Aws::S3::S3Client`, retries forced to zero | `aws_asg_quorum_manager` (SDK usage), none (data plane) |
| `azure_blob_client` | `conditional_key_object_store` | Hand-rolled Blob REST over httplib, AAD bearer token | `alibaba_oss_client` (hand-rolled data plane), `oci_http_client` (transport idiom) |
| `gcp_gcs_client` | `conditional_key_object_store` | `google-cloud-cpp` `storage`, retry policy zeroed | `gcp_mig_quorum_manager` (SDK + opt-in feature) |
| `oci_object_storage_client` | `conditional_key_object_store` | `oci_signing` + a new raw-bytes path on `oci_http_client` | `alibaba_oss_client` (near-identical role) |
| `alibaba_oss_client` (amended) | `conditional_key_object_store` | Existing client, ETag-returning PUT/GET added | — (it is the reference) |

**The load-bearing architectural decision of this spec: one engine, five
stores.** The Raft-correctness-critical body — key layout and 20-digit
padding, the in-memory mirror, mirror-after-acknowledgement ordering,
parse-or-throw loading, the single idempotent-PUT retry, retention, fencing
— is written **once**, in `object_store_persistence_engine`, generic over a
`key_object_store`. Each provider contributes only a client.

This is a **deliberate departure** from how every other cloud family in this
repo is built. The quorum managers (`aws_asg_`, `azure_vmss_`, `gcp_mig_`,
`oci_instance_pool_`, `alibaba_ess_`) and the certificate providers
(`aws_acm_pca_`, `azure_key_vault_`, `gcp_privateca_`, `oci_certificates_`)
are **independent siblings** — separate classes sharing a concept and
literally nothing else; there is no base class, no CRTP and no shared
template anywhere in `include/raft/` for either family. That was the right
call there and is the wrong call here, for a reason worth stating precisely:

- A scaling group, a VMSS, a MIG and an instance pool differ **semantically**.
  Different capacity models, different lifecycle vocabularies, different
  liveness signals, different provisioning races — OCI needed an
  application-level heartbeat, AWS did not; ECS `TagResources` is additive,
  OCI `freeformTags` is whole-map replacement. Forcing those behind one
  abstraction would produce a worse abstraction than five honest classes.
- Object stores do **not** differ semantically. All five are S3-shaped: PUT
  a key, GET a key, DELETE a key, LIST a prefix, with per-key atomic
  whole-object writes and no cross-key transaction. What differs is
  authentication, error wire format, listing pagination, and the spelling of
  the conditional-write precondition — none of which is Raft-correctness
  logic.

The part that can silently violate Log Matching is the part that is now
written once. Writing it five times means proving it five times, and
`file_persistence_engine`'s history in this repo — a silent `catch (...)`
that drops unparseable records — is the standing demonstration that
persistence bugs of this class do not announce themselves.

**The second structural decision: the durability contract is never traded
for latency.** No batching, no write-behind, no coalescing, no "relaxed
durability" mode, no asynchronous flush with a fence. The concept
(`persistence.hpp:18`) forbids them and this design does not smuggle one in
under a configuration flag. The consequence — one PUT round trip per log
entry per node, four sequential durable writes per election round — is
instead made **visible and computable** (Requirements 4.5–4.6, 6.2–6.4), so
an operator sizing a cluster gets a number rather than a surprise. A design
that recovered throughput by weakening this would be trading a Raft safety
property for a benchmark, which is exactly the trade nobody should be able
to make by editing a config file.

**The third decision, per provider: SDK where one is already paid for,
hand-rolled where it is not.** The tree's dependency situation is genuinely
asymmetric and this design follows it rather than imposing uniformity —
see the client table in Data Models below, and Non-Goals for what that
costs.

### Is a cloud object store an appropriate Raft persistence backend at all?

This design's answer, stated before anything is built on it, because a spec
that presented an object-store WAL as free would be worse than useless:

**Yes, for a narrow and real class of deployments. No, as a general
replacement for a local write-ahead log.**

- **Appropriate** for low-write-rate, durability-dominated clusters —
  membership, configuration, coordination state — where election timeouts
  can be measured in seconds and losing the node's local disk must not lose
  the node's vote. Kythira's own CA-cluster and quorum metadata are this
  shape.
- **Appropriate** as the off-node durable tier that makes backup and
  restore-into-a-fresh-cluster possible at all. Today neither exists:
  `file_persistence_engine` keeps a single overwritten snapshot slot on a
  local disk it never fsyncs.
- **Not appropriate** where the append rate is high. Sustained commit
  throughput is bounded above by one PUT round trip per entry per node, and
  the request bill scales linearly with it (worked numbers below).
- **Not appropriate** where the election timeout cannot be raised. Below the
  four-sequential-writes floor the cluster times out its own elections and
  livelocks.

The measured evidence, and the only measurement this project has:
`.kiro/specs/alibaba-cloud-services/spike-notes.md` Finding 7 — **~2–3 s per
object round trip** from a developer machine to `ap-southeast-1`; ~9.6 s for
the term+vote case, ~34 s for twelve log appends. That is an upper bound
**dominated by geography, not by OSS**, and the in-region figure — the one
that decides whether this engine is usable — has never been measured.
Requirement 4.5 makes measuring it a deliverable rather than a hope.

## Non-Goals

- **No latency-hiding mechanism, at any price.** No batched appends, no
  write coalescing, no write-behind, no asynchronous flush, no relaxed
  durability mode. Recovering append throughput requires a batch operation
  on `kythira::persistence_engine` itself — a raft-layer change affecting
  every engine including `file_persistence_engine` and
  `memory_persistence_engine` — and is recorded as a follow-on with this
  spec as its motivation, not smuggled in here.
- **No `save_hard_state(term, vote)` concept widening**, which would halve
  the election-path round trips and make the pair atomic. It is the most
  attractive optimization available and it is still out of scope: it changes
  the concept every engine implements. Recorded in Requirement 4.7 as a
  candidate follow-on so a future maintainer finds the fork already mapped.
- **No multipart/chunked upload.** Each provider's multipart surface is
  several operations with their own failure modes, for a case — a snapshot
  larger than the single-PUT cap — that no state machine in this repo
  currently produces. Requirement 7.3's loud error is the honest handling
  until one does.
- **No bucket lifecycle management.** Buckets, containers, versioning,
  lifecycle rules, encryption configuration and access policies are
  operator-owned prerequisites, as in every sibling provider spec. The
  engine reads and writes objects; it does not administer storage.
- **No dependence on provider-native versioning or soft-delete** (S3
  Versioning, Azure soft delete, GCS Object Versioning, OSS versioning) for
  correctness. An engine whose correctness depends on a bucket setting it
  does not control breaks when someone tidies the bucket policy — and OSS
  makes the point concretely: `x-oss-forbid-overwrite` is documented as
  *silently ignored* when bucket versioning is enabled.
- **No fencing that coordinates.** Requirement 9 detects a second writer and
  latches; it does not arbitrate, take over, renew a lease, or fail over.
  Azure Blob leases — the strongest primitive any of the five offers — are
  considered and rejected in Data Models below.
- **No `chaos_node` integration.** `chaos_node` uses `file_persistence_engine`
  by design; wiring a cloud engine into a product binary is separate work.
- **No new consensus behaviour.** The raft layer's treatment of persistence
  exceptions is unchanged, including its treatment of the new
  `persistence_fenced_error` (which it sees as an exception like any other —
  the *latch* is what makes it terminal, inside the engine).

## Architecture

```
                       raft node (hot loop)
                              │
                              │  12 synchronous persistence_engine calls
                              ▼
        ┌─────────────────────────────────────────────────┐
        │  object_store_persistence_engine<Store>         │
        │                                                 │
        │   reads ──► in-memory mirror  (no network, ever)│
        │   writes ─► serialize ─► fault point ─► Store   │
        │                             │                   │
        │                             ▼                   │
        │              mirror updated ONLY after 2xx      │
        │              version token recorded per key     │
        └─────────────────────────────────────────────────┘
                              │  key_object_store concept
                              │  (+ conditional_key_object_store)
        ┌─────────────┬───────┴───────┬──────────────┬──────────────┐
        ▼             ▼               ▼              ▼              ▼
  aws_s3_client  azure_blob_    gcp_gcs_client  oci_object_    alibaba_
                   client                        storage_        oss_client
        │             │               │           client            │
   Aws::S3::     httplib +       google-cloud-   oci_signing +   OSS V4 +
   S3Client      AAD bearer      cpp storage     raw-bytes path  httplib
   (SDK, dep     (no SDK dep)    (opt-in gcp     (no SDK)        (shipped,
    already                       feature)                        live-
    present)                                                      verified)
        │             │               │              │              │
        ▼             ▼               ▼              ▼              ▼
       S3         Blob Storage       GCS       OCI Obj Storage     OSS


  off the hot path entirely, never instantiates the engine:

  cmd/raft_object_backup ──► object_store_backup<Store>
                                  │  create / list / verify
                                  │  clone-restore / seed-restore
                                  ▼
                             <backup_prefix>/<backup_id>/…  (+ manifest)
```

## Data Models

### Object layout

Everything one engine instance owns lives under one `prefix` in one bucket.
The first four rows are **exactly what ships today** in
`alibaba_oss_persistence_engine`; only the fifth is new, and it is written
only when `snapshot_retention > 1`.

```
<prefix>/term                             "42"
<prefix>/voted_for                        "7" | "none"
<prefix>/log/00000000000000000042         {"term":3,"index":42,"command":"…","type":0}
<prefix>/snapshot                         {last_included_index, …}   ← the commit point
<prefix>/snapshots/00000000000000000041   retained predecessors (retention > 1 only)
<prefix>/owner                            {owner_id, epoch, started_at}  (fencing only)
```

Three properties of this layout carry weight and are unchanged from the
shipped engine:

1. **One object per log entry.** The file engine keeps its whole log in one
   file and rewrites it on truncation — one `write` + `rename`, microseconds,
   correct for a filesystem. In an object store the same idiom is a full
   re-upload of the entire log for an O(1) logical change. One object per
   entry makes `append_log_entry` exactly one PUT, `truncate_log` and
   `delete_log_entries_before` bounded batches of DELETEs touching only
   affected entries, and recovery one LIST plus one GET per live object.
2. **20 zero-padded digits.** `2^64 - 1` is 20 digits, so padding every index
   to exactly 20 makes lexicographic listing order identical to numeric
   index order for every representable `LogIndex`. Without it `log/10` sorts
   before `log/9`. Recovery correctness rests on this, which is why a key
   whose suffix is not exactly 20 digits is corruption rather than a key to
   skip.
3. **The same JSON codec `file_persistence_engine` writes**, so records stay
   byte-comparable and a bucket can be diffed against a data directory
   during a migration.

### Per-provider consistency and durability

Established from vendor primary documentation, August 14, 2026. **The
confirmation column is load-bearing and must survive into
`doc/cloud_object_persistence.md` intact** (Requirements 4.2, 5.3): a table
that averages five citations into one confident sentence has laundered its
weakest one.

What the engine actually needs, so the table can be read against something:

- **N1 — durability on response.** The only *always*-load-bearing guarantee.
  Raft's `currentTerm`/`votedFor` rule is exactly this.
- **N2 — read-after-write on overwrite.** Needed **once per process
  lifetime**, at construction, when the mirror loads. After that no read
  touches the network, so the engine is insensitive to read consistency for
  its entire steady-state life.
- **N3 — list-after-write.** Needed once, for the same reason. This is the
  one that can hurt silently: a lagging listing produces a **short log** at
  recovery, which is data loss rather than a visible error.
- **N4 — a conditional-write primitive.** Only under
  `fencing_mode::compare_and_swap`.

| | AWS S3 | Azure Blob | GCS | OCI Object Storage | Alibaba OSS |
|---|---|---|---|---|---|
| **N2** read-after-write, new **and** overwrite | Strong — **explicit**, all operations, since Dec 2020 | Strong — documented as a general storage-account guarantee | Strong — **explicit**, "never … stale data", globally | Strong — **explicit** overview statement | Strong — **explicit**, "no scenarios in which data is not obtained" |
| **N3** list-after-write | Strong — **explicit**, called out beside read consistency | **OPEN** — the general claim covers "list operations" but without S3/GCS's specificity | Strong — **explicit**, "bucket listing and object listing are strongly consistent" (Spanner-backed) | **OPEN** — no listing-specific statement found | **OPEN** — no listing-specific statement found |
| **N1** 2xx ⇒ durable | Yes — "if you receive a success response, Amazon S3 added the entire object"; 11 nines | **Depends on account redundancy.** ZRS: **explicit** synchronous write to all three zones before success. LRS: synchronous, one datacenter. GRS/GZRS: primary synchronous, **secondary region asynchronous** | Yes — **explicit**: the object becomes visible only once the upload completed and the server sent success | Implied by the documented redundancy model + 11 nines; **no "before returning success" wording found — OPEN** | Implied — "replicas of the object are created for redundancy" on a success response; 12 nines |
| Verification status **in this repo** | documentation-derived | documentation-derived | documentation-derived | documentation-derived | **live-verified** (Aug 14 2026, `ap-southeast-1`) |

Three consequences the design takes seriously:

- **The mirror is what buys the narrow consistency requirement.** A design
  that read from the store on the Raft hot path would be exposed to read
  consistency continuously instead of once. This is worth saying out loud
  because the mirror looks like a performance optimization and is also a
  correctness simplification.
- **The N3 OPENs cannot be closed by analogy.** Requirement 5.4 blocks
  marking any provider production-ready whose listing is eventually
  consistent in any documented circumstance, and Task 0 closes each cell
  **empirically as well as documentarily** — write N objects under a fresh
  prefix, LIST immediately, assert all N appear. Documentation is the weaker
  evidence here precisely because the failure is silent.
- **Azure's N1 is account configuration, and the engine does not control
  it.** The operator documentation must state that a Blob account backing
  this engine SHOULD be **ZRS or GZRS** (ZRS is the only mode Microsoft
  documents as writing synchronously to all replicas before returning
  success), that **LRS is acceptable but is single-datacenter durability**,
  and that GRS's cross-region copy is **asynchronous** — a 2xx means durable
  in the primary region only. The engine will not read or enforce the
  setting; it documents the prerequisite and leaves it operator-owned, as
  every other prerequisite resource in this tree is.

### The operating envelope: latency, throughput and cost

The arithmetic behind Requirements 4.6 and 6.2–6.4, worked here so the
operator documentation quotes a derivation rather than a claim.

**Request model.** One PUT per log entry per node; one PUT per term change;
one per vote; one per snapshot (two when `snapshot_retention > 1`); one
DELETE per truncated or compacted entry. Reads cost one LIST plus N GETs per
process start and **nothing thereafter** — the mirror.

**Election-path latency.** A candidate persists an incremented term and then
a self-vote before sending its first RequestVote: two sequential durable
writes. A follower at a lower term persists the term bump and then its vote
before replying: two more. So one election round costs **four sequential
PUT round trips plus one RPC round trip**, which gives the sizing rule:

```
election_timeout_min  ≥  4 × p99(PUT) + rpc_rtt + margin
```

and the randomized election-timeout **range width** must be at least that
same quantity, or nodes re-time-out inside each other's elections and
livelock. Worked both ways:

| p99(PUT) | Floor on `election_timeout_min` | Verdict |
|---|---|---|
| ~3 s (the measured cross-ocean figure) | **≥ 12 s** | Consistent with the 9.6 s the Alibaba real tier observed for the term+vote case alone. Usable only for clusters that can tolerate a 12 s+ leaderless window |
| 40 ms (hypothetical in-region — **not measured**) | **≥ 160 ms** + RTT | Inside the range Raft deployments normally use. **This is the configuration the engine is actually for**, and the number is a placeholder until Requirement 4.5's measurement exists |

The second row is deliberately marked hypothetical. No in-region measurement
exists for any of the five providers, and none of the five publishes a
small-object PUT latency figure — AWS's performance guidance says "tens of
milliseconds" median for sub-512 KB requests and offers no SLA; Azure, GCS,
OCI and OSS publish nothing. Requirement 4.5 exists because this row cannot
be filled in from documentation by anyone.

**Throughput ceiling.** With no batch operation on
`kythira::persistence_engine`, sustained append throughput per node cannot
exceed roughly `1 / p50(PUT)` entries per second. At the measured 2.8 s/entry
(34 s for 12 appends) that is **~0.36 entries/s**. At a hypothetical 40 ms it
is ~25 entries/s. Either way this is two to three orders of magnitude below
a local NVMe WAL, and it is the single number that decides whether a
workload fits.

**Cost.** Object stores bill per request and this engine's write pattern is
one request per entry per node, so the bill scales linearly with append
rate. Using each provider's approximate published PUT price (retrieved
August 14, 2026; ~$0.005 per 1,000 requests for S3, Azure Blob, GCS and OCI,
and ~$0.0014 per 1,000 for OSS after a large free tier), per **node**:

| Sustained append rate | PUTs/month/node | S3 / Azure / GCS / OCI | Alibaba OSS |
|---|---|---|---|
| 1 entry/s | ~2.6 M | ~**$13**/month | ~$0 (inside the free tier) |
| 100 entries/s | ~259 M | ~**$1,300**/month | ~**$220**/month |

On a three-node cluster the 100 entries/s row is roughly **$3,900/month in
request charges alone**, before any storage or egress. The conclusion is
stated as a conclusion rather than left as an exercise: **sustained
high-append-rate workloads are financially prohibitive in this engine, not
merely slow.** That, together with the throughput ceiling, is what defines
the deployment shape in the Overview — and it is why Requirement 6 makes
publishing this envelope a deliverable rather than a README afterthought.

### Per-provider conditional-write primitives (Requirement 9.7)

This is where the five providers genuinely diverge, and it is the most
interesting table in the design.

| | Create-only precondition | Overwrite CAS | Conditional delete | Version token |
|---|---|---|---|---|
| **AWS S3** | `If-None-Match: *` (Aug 2024). 412 on exists; **409 `ConditionalRequestConflict` on a benign race** | `If-Match: <etag>` | `if-match` on DeleteObject — general-purpose buckets since Sept 2025 | ETag |
| **Azure Blob** | `If-None-Match: *` on Put Blob | `If-Match: <etag>` on Put Blob | `If-Match` on Delete Blob | ETag |
| **GCS** | `ifGenerationMatch=0` | `ifGenerationMatch=<generation>` | `ifGenerationMatch=<generation>` | **generation number** (integer) |
| **OCI Object Storage** | `if-none-match: *` (only `*` is a valid value) | `if-match: <etag>` | `if-match` — **OPEN**, confirm in Task 0 | ETag |
| **Alibaba OSS** | `x-oss-forbid-overwrite: true` → `FileAlreadyExists`. **Documented as silently ignored when bucket versioning is enabled or suspended** | **OPEN — not confirmed.** `If-Match`/`If-None-Match` are not listed among PutObject's supported headers | **OPEN** | ETag |

Design consequences, each of which is a decision rather than an observation:

1. **S3's 409 must not be collapsed into 412.** A 412 means "you lost — a
   second writer exists". A 409 `ConditionalRequestConflict` means "two
   conditional requests raced; retry". Mapping both to
   `object_precondition_failed` would turn a benign race into a permanent
   fence latch (Requirement 9.5 makes the latch unclearable, so this
   mistake would be unrecoverable without a restart). The S3 client maps
   409 to a retryable exception and only 412 to
   `object_precondition_failed`.
2. **GCS's generation is not an ETag, and is stronger.** A generation is a
   monotonically-assigned integer identifying a specific object version;
   `ifGenerationMatch=0` is a sentinel for "does not exist" rather than a
   wildcard. The concept's opaque `object_version` string is exactly what
   lets an integer generation and an ETag share one interface without the
   engine ever inspecting either.
3. **Alibaba OSS may not qualify for `compare_and_swap` at all**, and the
   design must not pretend otherwise. Only the existence-check header is
   confirmed; ETag-based overwrite CAS is unconfirmed and possibly absent.
   Requirement 9.8's rule applies exactly here: if OSS cannot express the
   overwrite precondition, `alibaba_oss_client` does **not** satisfy
   `conditional_key_object_store`, and `compare_and_swap` becomes a
   **compile-time** unavailable option for it. It does not silently degrade
   to a no-op. This is the single most important thing this table decides,
   and it is decided against the provider this spec's reference
   implementation comes from.
4. **The OSS versioning caveat generalizes into a test obligation.** A
   precondition that a bucket setting can silently disable is a fence that
   can silently stop fencing — the worst outcome Requirement 9 has. Only a
   **live negative control** catches it: the real tier asserts that a
   stale-version write is actually rejected by the live service
   (Requirement 17.7's fencing race), not merely that the client sent the
   header.

**Azure Blob leases: considered, rejected, recorded.** Azure is the only
provider offering a real lease (`x-ms-lease-duration`, 15–60 s or infinite;
the lease ID becomes mandatory on every write to the blob or the write 412s).
It is the strongest exclusion primitive on offer and it is not used, for two
reasons that are properties of this engine rather than of Azure. A finite
lease must be renewed on a timer, which puts a background thread and a
liveness dependency inside an engine whose entire design (Requirement 4.1)
is that it has **no asynchronous path** — and a renewal that loses a race
would fence the rightful owner out. An infinite lease survives the process
that took it, converting a crash into an operator-visible outage requiring a
manual lease break. The ETag path costs nothing, needs no thread, and is
uniform across all five providers.

### Fencing state machine

```
        construct (fencing = compare_and_swap)
                    │
                    │  GET <prefix>/owner
          ┌─────────┴──────────┐
       absent                present
          │                     │
   PUT owner if-none-match   owner_id == mine, or takeover_epoch > recorded?
          │                     │            │
       OWNING  ◄────────────  yes           no ──► throw (construction fails,
          │                                          names the recorded owner)
          │
          │  every save_current_term / save_voted_for:
          │     PUT key If-Match: <version tracked in mirror>
          │
   ┌──────┴────────┐
 2xx              412 ─────► persistence_fenced_error  ──►  LATCHED
   │                          (names key, expected version,       │
 record new                    provider)                          │
 version,                                                          │
 update mirror                every subsequent mutating call throws
   │                          the same error WITHOUT contacting the
   └── OWNING                 store; the latch is not clearable
```

The chokepoint argument, which is why `term` and `voted_for` carry the
If-Match CAS: **a second writer cannot cause a Raft *safety* violation
without first writing one of them.** A candidate must persist an incremented
term and a self-vote before sending any RequestVote; a follower must persist
a term bump before replying and `voted_for` before granting at a term it has
already recorded. There is no path to a second leader, a diverging committed
log, or a double vote that does not pass through one of those two single-slot
objects.

**That argument covers safety and not corruption, and the difference
matters.** A stale leader appends without changing its term — it will never
gather a quorum again, but nothing in Raft makes it write `term` or
`voted_for` before writing `<prefix>/log/<index>`. Left unconditional, it
interleaves log objects with the rightful owner's for as long as it runs, and
neither writer notices; the corruption surfaces only at the next recovery,
which is precisely the silent-short-log failure this design refuses
elsewhere. So log-entry PUTs carry the **create-only** precondition. It costs
nothing — a header on a PUT already in flight — and any collision at an index
latches one of the two writers loudly. Legitimate re-use of an index is
always preceded by `truncate_log`'s DELETE, so create-only never rejects a
legal write.

`<prefix>/snapshot`, `<prefix>/snapshots/<index>` and every DELETE stay
unconditional, and this is a **bounded residual, not a covered case**: a
second writer whose only interaction with the prefix is a snapshot overwrite
or a truncation goes undetected. Conditioning them would buy little — both
follow a term or append write in every sequence a live node actually produces
— and the honest statement of the gap is worth more than a claim of totality
that an operator might rely on. The steady-state round-trip count is
unchanged either way: preconditions ride on requests already being sent, and
a precondition failure is an error path, not a retry.

### Backup manifest

Written **last**, which is what makes its presence the backup's commit
point; a torn backup is one without a manifest and `list` ignores it.

```json
{
  "format_version": 1,
  "provider": "s3",
  "backup_id": "2026-08-14T18-22-05Z",
  "created_at": "2026-08-14T18:22:05Z",
  "source": { "bucket": "…", "prefix": "raft/node-3" },
  "quiesced": true,
  "state": {
    "current_term": 42,
    "voted_for": "7",
    "log_first_index": 118, "log_last_index": 903,
    "snapshot": { "last_included_index": 117, "last_included_term": 39,
                  "nodes": [...], "is_joint_consensus": false }
  },
  "objects": [ { "key": "…", "bytes": 214, "checksum": "…" } ]
}
```

`verify` checks internal consistency against this — every index from the
snapshot's `last_included_index + 1` to `log_last_index` present, every
checksum matching, `current_term` ≥ every entry's term — which is what makes
a **smeared** backup (one taken from a running node across a copy window)
*detectable* rather than merely warned about. There is no cross-key
atomicity in any of these stores, so a running-node backup is a smear by
construction; the manifest records whether the caller declared the source
quiesced, and `verify` catches the smears that matter.

### Restore modes

| | Clone restore | Seed restore |
|---|---|---|
| Purpose | Move one node's state to new storage/instance, identity unchanged | Start a **new** cluster from a backup's snapshot |
| term | preserved | reset to snapshot's `last_included_term` |
| voted_for | preserved | cleared |
| log | preserved entry-for-entry | empty |
| snapshot state bytes | preserved | preserved |
| snapshot configuration | preserved | **replaced** by an operator-supplied node set |
| owner epoch | restored **higher**, so a returning original fences itself out | fresh |
| Refuses when | target prefix non-empty without `--force` | as clone, **plus** the backup has no snapshot |

Keeping these two as separate, separately-named modes is the point. "Move
this node's state" and "start a new cluster from this backup" have opposite
answers for term, vote, log and membership, and a single `restore` verb with
flags is how an operator in a recovery window produces a split-brain by
typing one word.

## Components and Interfaces

### 1. `include/raft/key_object_store.hpp`

Compiled unconditionally — no provider headers, no SDK includes.

```cpp
namespace kythira {

/// Opaque per-object version: an HTTP ETag on S3/Azure/OCI/OSS, a decimal
/// generation number on GCS. The engine never inspects it.
using object_version = std::string;

struct put_result { object_version version; };
struct get_result { std::string body; object_version version; };

/// Thrown ONLY when a service evaluated a precondition and rejected it.
/// Distinct type because the engine's whole fencing behaviour rests on
/// telling "you lost the race" apart from "the network broke".
class object_precondition_failed : public std::runtime_error { /* … */ };

template<typename S>
concept key_object_store =
    requires(const S& s, std::string_view bucket, std::string_view key,
             std::string_view bytes) {
        { s.put_object(bucket, key, bytes)  } -> std::same_as<put_result>;
        { s.get_object(bucket, key)         } -> std::same_as<std::optional<get_result>>;
        { s.delete_object(bucket, key)      } -> std::same_as<void>;
        { s.list_keys(bucket, key)          } -> std::same_as<std::vector<std::string>>;
        { s.provider_name()                 } -> std::convertible_to<std::string_view>;
    };

/// "Create only if absent", or "replace only if exactly at this version".
struct if_absent {};
struct if_version { object_version expected; };
using precondition = std::variant<if_absent, if_version>;

template<typename S>
concept conditional_key_object_store =
    key_object_store<S> &&
    requires(const S& s, std::string_view bucket, std::string_view key,
             std::string_view bytes, const precondition& p) {
        { s.put_object_if(bucket, key, bytes, p) } -> std::same_as<put_result>;
        { s.delete_object_if(bucket, key, p)     } -> std::same_as<void>;
    };

}  // namespace kythira
```

Both conditional operations report a rejected precondition by **throwing
`object_precondition_failed`**, never by a sentinel return. A sentinel would
be silently ignorable at exactly the call sites where ignoring it is a
safety violation.

### 2. `include/raft/object_store_persistence.hpp`

```cpp
enum class fencing_mode { none, compare_and_swap };

struct object_persistence_options {
    std::size_t snapshot_retention{1};
    fencing_mode fencing{fencing_mode::none};
    std::string owner_id;
    std::optional<std::uint64_t> takeover_epoch;
    bool verify_checksums{true};
    std::size_t max_object_bytes{/* Task 0 */};
    unsigned write_retries{1};
};

template<key_object_store Store,
         typename NodeId = std::uint64_t, typename TermId = std::uint64_t,
         typename LogIndex = std::uint64_t>
requires node_id<NodeId> && term_id<TermId> && log_index<LogIndex>
class object_store_persistence_engine {
public:
    object_store_persistence_engine(Store store, std::string bucket,
                                    std::string prefix,
                                    object_persistence_options opts = {});
    // … the 12 persistence_engine methods, unchanged in signature …

    static constexpr std::size_t k_index_digits = 20;
private:
    Store _store;
    std::string _bucket, _prefix;
    object_persistence_options _opts;

    mutable std::mutex _mu;
    TermId _current_term{0};
    std::optional<NodeId> _voted_for;
    std::map<LogIndex, log_entry_t> _log;   // ordered: every log op is a range op
    std::optional<snapshot_t> _snapshot;

    // Fencing (Requirement 9): version per single-slot key, and the latch.
    std::unordered_map<std::string, object_version> _versions;
    std::optional<std::string> _fenced_reason;   // set once, never cleared
};
```

Write path per mutating method, unchanged from the shipped engine except for
the two bracketed additions: serialize → fault point
(`raft/objstore/put_object`) → `put_object` *[or `put_object_if` under
`compare_and_swap`]* → *[record the returned version]* → update mirror →
return. The mirror is updated **after** the store acknowledges, so a
throwing write leaves memory exactly equal to the store.

Load path: `list_keys(prefix + "/")` → GET each → parse-or-throw naming the
key. Unknown keys under the prefix are neither read nor written nor deleted.

### 3. `include/raft/object_store_backup.hpp` and `cmd/raft_object_backup`

```cpp
template<key_object_store Store>
class object_store_backup {
public:
    auto create(const source_ref& src, const dest_ref& dst,
                backup_options opts) const -> backup_manifest;
    auto list(const dest_ref& dst) const -> std::vector<backup_manifest>;
    auto verify(const dest_ref& dst, std::string_view backup_id) const
        -> verification_report;
    auto restore_clone(const dest_ref& src, const source_ref& target,
                       restore_options) const -> void;
    auto restore_seed(const dest_ref& src, const source_ref& target,
                      const std::vector<std::string>& new_nodes,
                      restore_options) const -> void;
};
```

It takes a store and a prefix. It **never** instantiates
`object_store_persistence_engine` and cannot be reached from the Raft hot
path — the structural guarantee behind Requirement 10.7. The CLI exists
because operators do not have a C++ compiler in a recovery window.

### 4. `include/raft/aws_s3_client.hpp`

```cpp
class aws_s3_client {  // KYTHIRA_HAS_AWS_SDK
public:
    explicit aws_s3_client(aws_client_config cfg);
    // key_object_store + conditional_key_object_store
private:
    Aws::S3::S3Client _s3;   // constructed with a ZERO-retry strategy
};
```

**No `vcpkg.json` change.** `aws-sdk-cpp` is an unconditional dependency and
already carries the `s3` feature; `CMakeLists.txt:1072` already resolves
`find_package(AWSSDK COMPONENTS core ec2 autoscaling iam s3 sts)`. S3 is the
one provider where this capability is a pure addition to an existing,
already-paid-for build. The retry strategy is forced to zero
(Requirement 1.5) — the engine's single retry must be the only retry, or its
idempotency argument is unverifiable from the outside.

### 5. `include/raft/azure_blob_client.hpp`

```cpp
class azure_blob_client {  // no SDK dependency; httplib + AAD bearer token
public:
    explicit azure_blob_client(azure_client_config cfg,
                               std::string account, std::string container);
};
```

**Hand-rolled, and `azure-storage-blobs-cpp` is the recorded fallback rather
than the default** — the one place this design departs from "use the SDK you
already have", so the reasoning is stated rather than assumed. The tree
carries `azure-core-cpp`, `azure-identity-cpp` and
`azure-security-keyvault-keys-cpp` unconditionally, but **not**
`azure-storage-blobs-cpp` (it exists in the registry at 12.18.0 and would be
a new dependency on the default build for every developer). The surface
needed is four operations; AAD bearer tokens are already what
`azure_client_config`'s credential chain and CI federation produce, so no
SharedKey signing is written at all. Task 0 can refute this: if the REST
surface proves materially harder than documented — container-level auth
quirks, an undocumented required header, listing pagination surprises — the
SDK is adopted and the extra dependency is paid.

Two details that are how a working client breaks later, so they are pinned:
`x-ms-version` is a **named, dated** API version rather than unset, and
`x-ms-blob-type: BlockBlob` is required on PUT. `List Blobs` XML is parsed
by the same bounded element-scanning approach `alibaba_oss_client` uses,
rather than taking an XML dependency for four element names.

### 6. `include/raft/gcp_gcs_client.hpp`

```cpp
class gcp_gcs_client {  // KYTHIRA_HAS_GCP_STORAGE
public:
    explicit gcp_gcs_client(gcp_client_config cfg);
private:
    google::cloud::storage::Client _gcs;   // retry policy limited to 0
};
```

Uses `google-cloud-cpp`'s `storage` component, added to the **existing
opt-in `gcp` vcpkg feature** — so GCS persistence is unavailable in a
default build, exactly like the GCP quorum managers, and for the same
honest reason that feature is opt-in at all (the compute component builds an
enormous API surface). `KYTHIRA_HAS_GCP_STORAGE` is found independently of
`KYTHIRA_HAS_GCP_SDK` and `KYTHIRA_HAS_GCP_PRIVATECA`, the established
pattern for that provider.

The considered-and-rejected alternative is recorded so a future maintainer
sees the fork rather than re-deriving it: a hand-rolled JSON API client over
httplib with an ADC/metadata bearer token would work in a *default* build,
but requires implementing service-account JWT signing, which the SDK already
does correctly.

### 7. `include/raft/oci_object_storage_client.hpp`

Reuses `oci_signing` and `oci_client_config` — **including Instance
Principal**, which is what a node actually running on an OCI instance will
use, and which `oci_federation.hpp` already implements.

The one non-obvious piece of work: `oci_http_client::request()` returns a
parsed `boost::json::value` for every call, which cannot carry object bytes.
That was a control-plane assumption, correct for the services it was written
for. A **raw-bytes request path** is added alongside it — returning status,
headers and body unparsed — without changing the existing method's behaviour
or any existing caller.

Addressing is `/n/<namespace>/b/<bucket>/o/<key>` on the `objectstorage`
regional endpoint, with the namespace configured or resolved once and
cached. `oci_http_client`'s existing header records the lesson that OCI's
domain is per service and not derivable from one template (its `iaas` vs
`oci` realm split cost two defects); this client inherits that caution
rather than re-learning it. `ListObjects` pages via `nextStartWith`.

The real tier reuses the **existing `kythira-ci-artifacts` bucket** — the one
`real-cloud-tests.yml` already uploads the heartbeat writer to — confined to
its own prefix, rather than provisioning a second one.

### 8. `alibaba_oss_client`, amended

Additive only: `put_object` returns the response ETag instead of `void`, and
`get_object` returns a `get_result`. Signing, virtual-host/path-style
addressing, pagination and error handling are untouched, and its existing
tests pass unmodified except where they name those two return types. Whether
it can also satisfy `conditional_key_object_store` is **OPEN** and decided by
Task 0 (see the conditional-write table above) — if OSS has no ETag-based
overwrite CAS, it satisfies only the base concept and `compare_and_swap` is
a compile error for it, by design.

## Correctness Properties

### Property 1: A returned write is a durable write
**Validates: Requirements 4.1, 4.4**

Every mutating method returns only after the store answered 2xx; there is no
buffered or asynchronous path to get this wrong by construction, and the
mirror is updated strictly after the acknowledgement so a throwing write
leaves memory equal to the store.

**How it could fail silently:** by treating a non-2xx as success — the
single-line mistake that no integration test notices because the *next*
read comes from the mirror, which would then hold state the store never
accepted. Every subsequent read would agree with itself and disagree with
reality until the process restarted. The conformance suite therefore pins it
directly: a store injecting a PUT-500 must make the method throw **and**
leave the mirror unchanged, and a separate case asserts from the store's
request log that the PUT completed before the method returned.

### Property 2: Log-key ordering equals log-index ordering, on every provider
**Validates: Requirements 2.2, 5.2**

20-digit zero-padding makes lexicographic listing order equal numeric index
order for every representable index, so recovery reads the log in order from
a single prefixed LIST.

**How it could fail silently:** two ways, both quiet. A padding regression at
a digit boundary (index 9 → 10) reorders the recovered log without any
error; the conformance suite pins the boundary explicitly. More insidiously,
a **provider whose listing is not strongly consistent** returns a short list
rather than an error, and the engine has no way to detect the difference
between "this log has 40 entries" and "this log has 60 entries and I was
shown 40". This is why Requirement 5.4 forbids marking such a provider
production-ready and why Task 0 closes the three OPEN N3 cells empirically
against live services rather than from documentation.

### Property 3: A fenced writer cannot complete another mutation
**Validates: Requirements 9.4, 9.5**

Under `compare_and_swap`, `save_current_term` and `save_voted_for` write with
`If-Match` against the version this engine last observed, and
`append_log_entry` writes create-only. A second writer must pass through one
of the two single-slot objects before it can cause any Raft safety
violation, and must collide at an index before it can corrupt a log without
touching them, so a precondition failure at either is proof of a second
writer, and the latch makes every subsequent mutation throw without
contacting the store. Snapshot writes and DELETEs remain unconditional and
are the stated residual (Requirement 9.3).

**How it could fail silently:** three ways, and each has a specific
countermeasure. (a) The precondition header is sent but the service ignores
it — OSS documents exactly this when bucket versioning is on — so only a
**live negative control** proves the fence works, not a client-side
assertion that the header was set. (b) A benign race (S3's 409
`ConditionalRequestConflict`) is mapped to `object_precondition_failed`,
latching a healthy engine permanently and unrecoverably; the S3 client maps
the two statuses separately and a unit case pins it. (c) A provider lacking
the primitive silently degrades to unconditional writes, which is a fence
that is *believed in* and absent — the worst state of all, and why
Requirement 9.8 makes it a **compile-time** unavailability rather than a
runtime fallback.

### Property 4: The snapshot commit point cannot be lost
**Validates: Requirements 8.2**

`save_snapshot` writes the retained copy first, then `<prefix>/snapshot` —
**the commit point** — then prunes. Failure at the first step leaves the
previous state fully intact; at the second, an unreferenced retained copy
that is inert; at the third, extra copies costing storage and nothing else.
No ordering of these failures loses the current snapshot.

**How it could fail silently:** by pruning before committing, or by writing
the commit point first "since it's the one that matters" — either inverts
the safety and neither produces an error at the time. Recovery reads
`<prefix>/snapshot` and nothing else, so the damage would surface only at
the next restart, arbitrarily later. The conformance suite injects a failure
at each of the three steps and asserts a readable current snapshot survives
all three.

### Property 5: Retention is not backup
**Validates: Requirements 8.6, 10.5**

Retained snapshots live in the same bucket, under the same prefix, behind
the same credentials, inside the same blast radius. Backups are written to a
different prefix and are *recommended* to be written to a different bucket
or account.

**How it could fail silently:** not as a code defect but as a documentation
one — an operator reads "retention: 3" and believes they have backups. The
countermeasure is that Requirement 8.6 makes the disclaimer a deliverable of
the retention feature itself rather than a line in a separate document
someone may not read.

### Property 6: A smeared backup is detectable, not merely warned about
**Validates: Requirements 10.4**

There is no cross-key atomicity in any of these stores, so a backup taken
from a running node is a smear across the copy window by construction.
`verify` checks the manifest's internal consistency — index contiguity from
the snapshot's `last_included_index`, checksums, `current_term` ≥ every
entry's term — so the smears that matter fail verification.

**How it could fail silently:** by making "was the source quiesced?" a
free-text field nobody checks, so a smeared backup restores cleanly and
produces a node whose term is older than its log. Restore therefore runs
`verify` **before writing anything** and aborts on the first inconsistency,
naming it (Requirement 11.5).

### Property 7: Five providers cost one engine's worth of proof
**Validates: Requirements 2.1, 17.2**

The engine is generic over the store, so Properties 1–4 are proved against
the engine once and instantiated per store by the shared conformance suite.
Adding a provider is one instantiation line.

**How it could fail silently:** by a provider's client quietly diverging in
a way the conformance suite does not reach — pagination that truncates
instead of throwing, a 404 mapped to an exception instead of `nullopt`, an
error status swallowed. Those are precisely what the per-provider unit tests
(Requirement 17.4) exist to cover, and the division of labour is deliberate:
the conformance suite proves the engine, the per-provider tests prove the
client, and neither is asked to do the other's job.

### Property 8: A default-configured bucket is byte-identical to today's
**Validates: Requirements 2.4, 3.2, 8.3**

Every field of `object_persistence_options` defaults to the shipped
engine's behaviour: `snapshot_retention = 1` writes **no**
`<prefix>/snapshots/` object at all, `fencing = none` writes no
`<prefix>/owner` and sends no conditional header, `write_retries = 1` is the
existing PUT-only retry. An operator who upgrades and changes nothing sees a
bucket with exactly the keys it had before.

**How it could fail silently:** by a new feature writing "just one small
extra object" unconditionally — a manifest, an owner record, a retained
snapshot at retention 1 — which nothing errors on and which quietly changes
the on-bucket format for every existing deployment, breaking any operator
tooling or diff-against-a-data-directory workflow that depended on it. The
conformance suite therefore asserts the **exact key set** a default-options
engine produces after a full exercise, not merely that reads round-trip.

### Property 9: The engine deletes only what it can prove it wrote
**Validates: Requirements 2.2, 8.5**

Truncation, compaction and retention pruning act only on keys this format
defines and whose index suffix parses as exactly 20 digits. An operator's
notes, a future format's objects, and a retained copy with an unexpected key
shape are left alone rather than tidied away.

**How it could fail silently:** a prune implemented as "delete everything
under `<prefix>/snapshots/` except the newest N" destroys anything else
stored there, and the operator learns about it the next time they look. The
countermeasure is that the rule is inverted — the engine enumerates what it
recognizes and deletes from that set — and a conformance case seeds a
foreign key under each prefix and asserts it survives a full truncate,
compact and prune cycle.

## Error Handling

| Condition | Behaviour |
|---|---|
| Store transport failure or non-2xx on a PUT | One retry (PUT-only, `write_retries`), then `std::runtime_error` naming the key and both attempts' messages — the shipped engine's exact message shape |
| Store failure on a DELETE | Throws immediately, no retry; the caller re-runs the whole idempotent truncation |
| Conditional write rejected (412 / `FileAlreadyExists` / generation mismatch) | `object_precondition_failed` from the client → `persistence_fenced_error` from the engine, naming key, expected version and provider; **latched** |
| S3 409 `ConditionalRequestConflict` | Retryable exception, **not** a precondition failure — mapped separately in the S3 client |
| Corrupt object at load | **Construction throws**, naming the offending key — deliberately stricter than `file_persistence_engine`'s silent `catch (...)`, which its own header lists as a limitation. A truncated upload must not degrade into silent state loss |
| Log record whose body disagrees with its key's index | Construction throws — the key *is* the index, and disagreement means the ordering guarantee is gone |
| Log key not exactly 20 digits | Construction throws — it cannot be ordered against the others |
| Snapshot exceeding `max_object_bytes` | Throws naming size, cap, and that multipart is a documented non-goal |
| Checksum mismatch on a returned ETag | Throws naming the key (Requirement 7.2) |
| Empty required config field | `std::invalid_argument` naming the field |
| Truncated/unparseable listing page | Throws rather than returning a short list — the one place a bug would be silent data loss instead of a visible failure |
| Manifest absent from a backup directory | That backup is **ignored** by `list` (a torn backup is one without a manifest) |
| Restore into a non-empty prefix without `--force` | Refused; `--force` deletes engine-owned keys before restoring, never merges |

## Testing Strategy

Four tiers, following the house convention, with one addition the shared
engine makes possible.

### Conformance suite (the addition)
`tests/object_store_conformance.hpp` — **one test body, instantiated per
store**: round-trip and reload-survival of every field, binary command bytes
including embedded nulls, the 20-digit padding boundary, truncation and
compaction, corrupt-object → constructor throw, PUT-failure →
mirror-unchanged, durability ordering observable from the store's request
log, retention at 1/2/N with a failure injected at each of the three
`save_snapshot` steps, and the two-engine fencing race. Instantiating it for
a new provider is one line. This is the mechanism that makes five engines
cost roughly one engine's worth of review.

### Unit tests (no network)
The conformance suite against `tests/mock_object_store.hpp` (in-memory, with
injectable failures: fail the Nth PUT, fail a precondition, return a corrupt
body, lag a listing). Plus per-provider unit tests covering only what is
provider-specific — request shape, conditional-header spelling, listing
pagination, error-to-exception mapping, checksum handling, endpoint override
— against a local `httplib::Server` or the SDK's endpoint override, the
established no-seam idiom.

### Emulator / mock tier (no cloud account) — **and here the providers differ**

| Provider | Substrate | Notes |
|---|---|---|
| **AWS S3** | **LocalStack** | Already an established tier in this repo (`aws_quorum_manager_localstack_test`, CTest labels `…;aws;localstack;slow`). A genuine advantage S3 has over OCI/Alibaba |
| **Azure Blob** | **Azurite** | Microsoft-maintained, supersedes the deprecated Storage Emulator. Second genuine advantage |
| **GCS** | `fake-gcs-server` (third-party) **or** a hand-written mock | Task 0 decides. No Google-supplied GCS emulator exists; if `fake-gcs-server`'s conditional-write fidelity is not trustworthy, a hand-written signature-verifying mock is the fallback |
| **OCI Object Storage** | Hand-written mock, extending `tests/oci_mock_server.hpp` | No official emulator; the two community projects found have neither the maturity nor the adoption to be trusted here |
| **Alibaba OSS** | Existing `tests/alibaba_mock_server.hpp` OSS routes | Extended, not duplicated |

Where a provider's mock server already exists (Alibaba, OCI) it is
**extended** and keeps **verifying signatures from the bytes that actually
arrived** — the `oci_mock_server.hpp` rule whose absence previously shipped
two real defects, and which the Alibaba mock proved again by reproducing a
live `SignatureDoesNotMatch` as a standing test.

An emulator is worth more than a mock only where it is trustworthy on the
specific behaviour under test. For round-trip and pagination, LocalStack and
Azurite are worth more. For **conditional writes** — the newest and least
uniformly implemented feature in this design — emulator fidelity is exactly
what cannot be assumed, which is why Requirement 17.7's fencing race is also
a real-tier case on every provider.

### Real-cloud tests (opt-in, per provider)
Compiled under `KYTHIRA_<PROVIDER>_REAL_TESTS`, **never CTest-registered**,
exit-77 skip naming each missing value, read-only pre-flight whose failure
skips rather than fails, cost-reporting and signal-teardown fixtures from
`oci_real_test_support.hpp`. Four load-bearing cases per provider:

1. **Fresh-engine read-back** — a fresh engine sharing no memory with the
   writer reads back term, vote, a multi-entry log and a snapshot. This is
   what turns a documentation citation into evidence, and is the case the
   Alibaba tier already passes live.
2. **Measured per-operation latency**, p50/p99, with the measurement
   position recorded (in-region instance vs. out-of-region host). Requirement
   4.5. No provider's number may be quoted for another.
3. **The fencing race**, including the live negative control that a
   stale-version write is genuinely rejected by the service.
4. **Backup → verify → clone-restore → fresh-engine read-back.**

Plus the **list-after-write empirical check** closing the three OPEN cells:
write N objects under a fresh prefix, LIST immediately, assert all N appear.

A task requiring live verification is **not** checked off against the mock
tier — the doctrine every provider spec in this repo follows.

## Dependencies

```
Already present, unconditional:
  aws-sdk-cpp   (s3 feature ALREADY listed and ALREADY found —
                 CMakeLists.txt:1072; zero build change for S3)
  cpp-httplib   (Azure Blob + OCI Object Storage transport)
  OpenSSL       (checksums; OCI/Alibaba signing)
  boost::json   (records, manifest)
  libfiu        (optional — fault points, FIU_FOUND-gated)

Changed:
  google-cloud-cpp — add the `storage` component to the EXISTING opt-in
                     `gcp` vcpkg feature (alongside compute, privateca).
                     GCS persistence is therefore absent from a default
                     build, exactly like the GCP quorum managers.

Deliberately NOT added:
  azure-storage-blobs-cpp — exists in the registry (12.18.0) and is the
                     recorded Task 0 fallback, but is not taken by default:
                     four operations over httplib with the AAD bearer token
                     azure_client_config already produces costs nothing on
                     the default build, which a new unconditional dependency
                     would.
```

New Kconfig symbols: `AWS_S3_PERSISTENCE` (depends on `AWS_SDK`),
`AZURE_BLOB_PERSISTENCE` and `OCI_OBJECT_PERSISTENCE` (depend on
`HTTP_TRANSPORT_TLS`, the no-SDK-to-find shape `OCI_QUORUM_MANAGER` and
`ALIBABA_OSS_PERSISTENCE` already use), `GCP_STORAGE` +
`GCP_STORAGE_PERSISTENCE` (depend on the SDK component).
`ALIBABA_OSS_PERSISTENCE` is unchanged.

`DEPENDENCIES.md` changes only where a dependency actually changes — the
`storage` component on the opt-in `gcp` feature. S3, Azure Blob and OCI add
nothing.
