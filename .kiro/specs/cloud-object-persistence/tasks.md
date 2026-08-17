# Implementation Plan — Cloud Object Persistence

## Status: tasks 1-3 done; task 0 closed for Alibaba and S3

**Done:** the seam (task 1), the generic engine (task 2) and the Alibaba
instantiation (task 3), August 15, 2026. Together they add **no capability on
purpose** — a default-configured bucket is byte-identical to what shipped,
which is the property task 3's unmodified test suites demonstrate.

**Task 0, Alibaba's cells: closed live, August 16, 2026** (spike-notes.md).
The decisive one went the way this plan's Notes section anticipated: **OSS has
no overwrite compare-and-swap** — `If-Match` on PutObject is
`400 NotImplemented` for a current ETag as well as a stale one — so
Requirement 9.8 fires and `compare_and_swap` will be a **compile error** for
the OSS engine. Create-only (`x-oss-forbid-overwrite` → **409
`FileAlreadyExists`**), `Content-MD5` verification, ETag determinism and
list-after-write are all confirmed live; conditional DELETE is **accepted and
silently ignored**.

**Task 19's Alibaba half is also done:** the real suite ran green against the
live bucket *after* the hoist, so the live-verified claim now belongs to the
generic engine over the OSS client.

**Task 0, AWS S3's cells: closed live, August 16, 2026** (Finding 10), and
they went the other way: **every** conditional primitive works, conditional
delete included (the cell design.md had marked OPEN), so S3 is the first
provider that could carry `compare_and_swap`. The one thing the run did *not*
produce is a **409 `ConditionalRequestConflict`** — 53 racing losers, all 412 —
so the 412/409 mapping stays a documentation-derived rule with a mandatory
unit case rather than becoming "409 never happens".

**Azure Blob, GCS and OCI cells remain open**, and tasks 5-6 wait on them —
but nothing external blocks them now. **All five providers have a bucket**
(task 17's bucket half, done August 16, 2026: `kythira-ci-827617851594` on S3,
`kythirarealtestobj`/`kythira-raft` on Azure Blob,
`kythira-ci-prefab-sky-500619-s9` on GCS; OCI and Alibaba already had one), and
all five authenticate from this host. Two credential caveats worth knowing
before reaching for them: **AWS's working credentials are in the `personal`
CLI profile** and resolve to the account **root** — fine for a read-only spike,
wrong for least-privilege CI — and **GCP's user credential is expired**, though
application-default credentials still work via
`CLOUDSDK_AUTH_ACCESS_TOKEN`. Full table in spike-notes.md.

The probes live in `scripts/object-store-probes/`; adding a provider means a
sibling of `probe_alibaba_oss.py`, reusing its status-code-based verdicts
rather than re-deriving them.

One thing in this plan has already been **live-verified**, which is why the
plan is shaped the way it is:
`alibaba_oss_persistence_engine` passed all four of its real cases against a
live `ap-southeast-1` bucket on August 14, 2026, including a fresh engine
reading back another engine's writes. Task 2 hoists that engine's body into
a generic one; task 19 re-runs its live suite to prove the hoist preserved
it. **A refactor of a live-verified component that is not re-verified live
has discarded the spec's single strongest piece of evidence** — that is what
task 19 exists for, and it is why it is a task rather than a footnote.

## Overview

Reference implementations to study before starting, in this order:

- `include/raft/alibaba_oss_persistence.hpp` — **the thing being
  generalized.** Read its header comment end to end first; it contains the
  durability argument, the one-object-per-entry rationale, the 20-digit
  padding rationale and the corruption-is-fatal contrast that this spec
  hoists rather than reinvents.
- `include/raft/persistence.hpp` — the concept: twelve synchronous,
  non-const methods, with the file comment that requires
  `save_current_term`/`save_voted_for` to be flushed to stable storage
  synchronously before returning.
- `include/raft/file_persistence.hpp` — the incumbent, and the source of
  three of this design's deliberate departures: single overwritten snapshot
  slot, silent `catch (...)` corruption tolerance, and **no `fsync`
  anywhere** (its `atomic_write` does write-then-rename only).
- `tests/alibaba_oss_persistence_unit_test.cpp` — the 41-case test pattern
  the conformance suite generalizes; note the durability-ordering case
  (`a_returned_write_has_already_reached_the_store`) and the
  mirror-unchanged case (`a_failed_put_throws_and_leaves_the_mirror_unchanged`).
- `tests/oci_mock_server.hpp` header comment — why a mock verifies
  signatures from the bytes that actually arrived, and the two real defects
  a non-verifying mock shipped.
- `tests/aws_quorum_manager_localstack_test.cpp` + its CMake registration —
  the established LocalStack tier in this repo, which S3 inherits.
- `.kiro/specs/alibaba-cloud-services/spike-notes.md` Findings 7 and 8 — the
  measured latency figure and the httplib query-rewriting hazard.

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 0, "tasks": [0], "description": "Spike: close every OPEN cell in the consistency, durability and conditional-write tables against live services" },
    { "wave": 1, "tasks": [1], "description": "The seam: key_object_store concept, in-memory mock store, conformance-suite skeleton" },
    { "wave": 2, "tasks": [2, 7, 8, 9, 10], "description": "The engine hoist and the four new clients (parallelizable — clients depend only on the concept)" },
    { "wave": 3, "tasks": [3, 4, 5, 6, 11], "description": "Alibaba becomes an instantiation; retention, fencing, integrity; build integration" },
    { "wave": 4, "tasks": [12, 13], "description": "Backup catalog; restore (both modes)" },
    { "wave": 5, "tasks": [14, 15], "description": "CLI; per-provider emulator/mock tiers" },
    { "wave": 6, "tasks": [16], "description": "Real-tier suites — compiled, gated, skip-correct" },
    { "wave": 7, "tasks": [17], "description": "CI wiring + idempotent bucket provisioning scripts" },
    { "wave": 8, "tasks": [18], "description": "Documentation, operator examples, TODO/CHANGELOG close-out" },
    { "wave": 9, "tasks": [19], "description": "Live verification across all five providers, incl. the Alibaba re-run" }
  ]
}
```

## Tasks

- [ ] 0. **Spike: close every OPEN cell before building on it**

  Run against vendor documentation **and** live services. Record findings in
  `spike-notes.md` with the OCI spec's CONFIRMED/CORRECTED/WAS format,
  folding every correction back into requirements.md/design.md **in place**.
  Every cell marked OPEN in design.md's Data Models tables is a sub-item
  here, and none of them may be closed by analogy to another provider.

  - [~] 0.1 **List-after-write, empirically** — for Azure Blob, OCI Object
        Storage and Alibaba OSS (S3 and GCS document it explicitly and need
        only a confirming run). Write N objects under a fresh prefix, LIST
        immediately, assert all N appear; repeat under concurrency. This is
        the one consistency question the engine cannot detect at runtime —
        a lagging listing produces a silently short log at recovery — so
        documentation is deliberately the weaker evidence.
        **Alibaba OSS and AWS S3: CLOSED live** — 3 × 25 objects, immediate
        LIST, complete every round on both (spike-notes.md Findings 5 and 10).
        OSS is recorded as *empirically consistent* rather than guaranteed,
        since the vendor publishes no listing-specific statement; S3's run
        confirms documentation that is already explicit. **Azure Blob and OCI
        remain open.**
  - [ ] 0.2 **Durability-on-response wording for OCI.** Find or fail to find
        primary documentation that a 2xx PUT means durably stored before the
        response. If it does not exist, say so in that provider's section
        rather than letting the four confirmed providers' wording stand in
        for it (Requirement 4.2's honesty rule).
  - [~] 0.3 **The conditional-write matrix, live, per provider** — create-only
        precondition, If-Match overwrite, conditional delete: exact header
        spelling, exact status code on rejection, and behaviour under
        concurrency. Three cells decide real design outcomes: **S3's 412 vs
        409 `ConditionalRequestConflict`** must be distinguishable (mapping
        both to a precondition failure would latch a healthy engine
        permanently); **OCI's conditional delete** is unconfirmed; and
        **whether Alibaba OSS supports ETag-based overwrite CAS at all** —
        only `x-oss-forbid-overwrite` is documented, and it is *silently
        ignored when bucket versioning is enabled*. If OSS cannot express
        the overwrite precondition, `alibaba_oss_client` satisfies only the
        base concept and `compare_and_swap` becomes a compile error for it,
        by design (Requirement 9.8).
        **Alibaba OSS: CLOSED live, and it triggered exactly that clause** —
        `If-Match` on PutObject is `400 NotImplemented` for a *current* ETag
        as well as a stale one, so there is no overwrite CAS at all;
        create-only is `x-oss-forbid-overwrite` rejecting with **409
        `FileAlreadyExists`** (not 412 — and S3 uses 409 for the opposite,
        retryable meaning, so no client may map a bare 409); conditional
        DELETE is **accepted and silently ignored** (spike-notes.md Findings
        1-3).
        **AWS S3: CLOSED live** — every conditional primitive works, including
        the **conditional delete** this design had marked OPEN (stale `If-Match`
        → 412 *and the object survives*; current → 204), so S3 is the first
        provider that could carry `compare_and_swap`. Its **409
        `ConditionalRequestConflict` was not elicited** — 53 racing losers, all
        412 — so the 412/409 split stays a documentation-derived rule with a
        mandatory unit case, explicitly not "409 does not happen"
        (spike-notes.md Finding 10). **Azure Blob, GCS and OCI remain open.**
  - [ ] 0.4 **Single-PUT size limits** per provider; `max_object_bytes`'s
        default is chosen from the smallest (Requirement 7.3).
  - [~] 0.5 **Checksum spelling** per provider — `Content-MD5` where
        universally supported, the provider's native header where preferred
        — and for which providers the returned ETag is a deterministic
        function of single-part content (Requirement 7.2).
        **Alibaba OSS and AWS S3: CLOSED live** — `Content-MD5` is verified
        end to end on both, and on both the ETag is the MD5 hex of single-part
        content, so both halves of Requirement 7 apply. The spellings differ
        and a client must not assume: mismatch is **`InvalidDigest`** on OSS
        and **`BadDigest`** on S3, and the ETag is **uppercase** on OSS,
        **lowercase** on S3 — so a local ETag check must compare
        case-insensitively (spike-notes.md Findings 4 and 10). **Other
        providers open.**
  - [ ] 0.6 **Azure Blob: REST vs SDK decision checkpoint.** The recorded
        decision is hand-rolled REST over httplib with an AAD bearer token,
        avoiding a new unconditional dependency. IF the surface proves
        materially harder than documented — container-level auth quirks, an
        undocumented required header, listing pagination surprises — THEN
        record the fallback to `azure-storage-blobs-cpp` (12.18.0, present
        in the registry) and why.
  - [ ] 0.7 **GCS mock tier decision.** No Google-supplied GCS emulator
        exists. Assess `fake-gcs-server`'s fidelity **specifically on
        generation preconditions**; if it is not trustworthy there, the
        fallback is a hand-written mock. Emulator fidelity on the newest,
        least uniformly implemented feature in this design is exactly what
        cannot be assumed.
  - [ ] 0.8 **OCI namespace and endpoint.** Confirm the `objectstorage`
        regional endpoint spelling and whether the namespace must be
        resolved via the API or can be configured — `oci_http_client`'s
        header already records that OCI's domain is per service and not
        derivable from one template, a lesson that cost two defects.
  - Verify: `spike-notes.md` exists with a finding per sub-item; **every
    OPEN cell in design.md's three Data Models tables is now either
    CONFIRMED with a citation or CORRECTED in place**; no cell was closed by
    analogy.
  - _Requirements: 4.2, 4.3, 5.3, 5.4, 7.1–7.3, 9.7, 13.2, 15.3, 17.5_

- [x] 1. **The seam: `key_object_store`, the mock store, the conformance suite**
      — done August 15, 2026. `include/raft/key_object_store.hpp`,
      `tests/mock_object_store.hpp`, `tests/object_store_conformance.hpp`.
      **One deviation from the design sketch, recorded:** the concept's
      requires-expression names `const std::string&` for `bucket`/`key` rather
      than `std::string_view`, because it is the *more permissive* of the two
      (a `string_view`-parameter client still satisfies it; a
      `const std::string&`-parameter client fails a `string_view`-argument
      check, since `std::string`'s converting constructor is explicit). The
      engine builds every key as a `std::string` anyway, and this is what lets
      `alibaba_oss_client` keep its signatures.

  - Create `include/raft/key_object_store.hpp` per the design sketch:
    `object_version`, `put_result`, `get_result`, `object_precondition_failed`,
    the `key_object_store` concept and the `conditional_key_object_store`
    refinement with `precondition = variant<if_absent, if_version>`.
    Compiled unconditionally — no provider headers, no SDK includes.
  - Create `tests/mock_object_store.hpp`: in-memory, satisfying both
    concepts, with injectable failures (fail the Nth PUT, fail a
    precondition, return a corrupt body, **lag a listing**) and a request
    log the durability-ordering assertions read.
  - Create `tests/object_store_conformance.hpp` as a **template**
    instantiated per store, with the cases the Alibaba unit suite already
    proves: round-trip and reload-survival per field, binary command bytes
    including embedded nulls and high bytes, the 20-digit padding boundary
    (9→10 digits), truncation and compaction, corrupt-object → constructor
    throw, PUT-failure → throw with mirror unchanged, durability ordering
    from the request log. Retention and fencing cases are added by tasks 4
    and 5.
  - Verify: `key_object_store.hpp` compiles in a build with **zero** cloud
    providers enabled; `mock_object_store` satisfies both concepts by
    `static_assert`; instantiating the conformance suite for a new store is
    demonstrably one line.
  - _Requirements: 1.1–1.4, 16.3, 17.1, 17.2_

- [x] 2. **`object_store_persistence_engine` — the hoist** — done August 15,
      2026, in `include/raft/object_store_persistence.hpp`, with
      `tests/object_store_persistence_unit_test.cpp` running the conformance
      suite against `mock_object_store` (43 conformance cases + 13 substrate-
      specific ones, green). The file-scope `static_assert` is made over a
      synthetic in-header store so it needs no provider, and the test repeats
      it over a real one.

      **`object_persistence_options` is deliberately partial**: it carries
      `write_retries` only. `snapshot_retention`, `fencing`, `owner_id`,
      `takeover_epoch`, `verify_checksums` and `max_object_bytes` arrive **with
      the behaviour that honours them** in tasks 4-6, rather than being
      accepted and ignored now. An ignored fencing knob is precisely the
      silent-degradation failure Requirement 9.8 exists to forbid, and
      `max_object_bytes`'s default cannot be chosen before task 0.4 measures
      the per-provider limits anyway.

  - Create `include/raft/object_store_persistence.hpp` with
    `object_persistence_options` (aggregate; every default preserving
    today's shipped behaviour) and
    `template<key_object_store Store, …> class object_store_persistence_engine`,
    satisfying `persistence_engine` with a file-scope `static_assert`.
  - The body is `alibaba_oss_persistence_engine`'s body, **hoisted, not
    rewritten**: same mirror loaded once at construction, same synchronous
    mutation path, same mirror-after-acknowledgement ordering, same
    parse-or-throw load naming the offending key, same layout, same 20-digit
    padding, same JSON codec byte-compatible with `file_persistence_engine`,
    same single PUT-only retry.
  - Move the substantial header documentation — the durability argument, the
    one-object-per-entry rationale, the padding rationale, the
    corruption-is-fatal contrast — to this header rather than deleting or
    duplicating it.
  - Fault points `raft/objstore/{put,get,delete,list}_object` at the store
    boundary; provider-independent, because the engine now is.
  - `tests/object_store_persistence_unit_test.cpp` runs the conformance
    suite against `mock_object_store`, plus options validation.
  - Verify: the conformance suite is green against `mock_object_store`; the
    engine header contains **no** provider name, no vendor error code and no
    `#ifdef KYTHIRA_HAS_*` (grep is the check).
  - _Requirements: 2.1–2.6, 4.1, 4.4, 17.3_

- [x] 3. **`alibaba_oss_persistence_engine` becomes an instantiation** — done
      August 15, 2026, as a thin derived type (an alias could not preserve the
      `{alibaba_client_config, bucket, prefix}` constructor).

      **The verification bar was met as written**:
      `alibaba_oss_persistence_unit_test` (41 cases),
      `alibaba_oss_persistence_mock_test` and `alibaba_oss_persistence_real_test`
      compile and pass with **zero source changes** — not one assertion moved.
      The only edited test lines in the whole hoist are two in
      `alibaba_oss_client_unit_test.cpp` (`.value()` → `.value().body`), which
      is exactly the mechanical Requirement 1.6 return-type change.

      The `raft/alibaba/oss/*` fault points moved onto the client, where they
      fire just inside the store boundary the generic
      `raft/objstore/*` points sit just outside; the existing fault-injection
      cases pass unchanged, which is the evidence that the move is invisible.

      **Re-verified live, August 16, 2026**: all four real cases green against
      `kythira-ci-5633986662052576` in `ap-southeast-1` *after* the hoist
      (spike-notes.md Finding 7), so the live-verified claim now belongs to the
      generic engine over the OSS client — which is the reason the hoist was
      acceptable at all. That is task 19's Alibaba half, done early because the
      credentials were available.

  - Amend `alibaba_oss_client` additively: `put_object` returns the response
    ETag, `get_object` returns a `get_result`. Signing, addressing,
    pagination and error handling untouched.
  - Redefine `alibaba_oss_persistence_engine` in its existing header as the
    `object_store_persistence_engine<alibaba_oss_client, …>` instantiation,
    **preserving the `{alibaba_client_config, bucket, prefix}` constructor**
    (via a thin derived type if an alias alone cannot).
  - Retain `raft/alibaba/oss/*` fault points on the client itself so no
    existing chaos configuration breaks silently.
  - Leave in the Alibaba header only what is OSS-specific, plus a pointer to
    the generic header.
  - Verify: **`alibaba_oss_persistence_unit_test` (41 cases),
    `alibaba_oss_persistence_mock_test` and `alibaba_oss_persistence_real_test`
    compile and pass unmodified**, apart from mechanical changes forced by
    the two return types. Any change to an *assertion* is a defect in the
    hoist, not a test that needs updating — treat it as such.
  - _Requirements: 1.6, 3.1–3.4_

- [ ] 4. **Snapshot retention**

  - Implement the three-step `save_snapshot`: PUT `<prefix>/snapshots/<20-digit>`,
    then PUT `<prefix>/snapshot` (**the commit point**), then prune oldest
    first beyond `snapshot_retention`.
  - Recovery reads `<prefix>/snapshot` and nothing else; retained copies are
    never consulted by the load path, so a corrupt retained copy cannot
    break startup.
  - Pruning leaves alone any key that does not parse as a 20-digit index —
    the engine deletes only what it can prove it wrote.
  - Conformance cases at `snapshot_retention` 1, 2 and N, **with an injected
    failure at each of the three steps**, asserting a readable current
    snapshot survives all three.
  - Verify: with `snapshot_retention = 1` (the default) **no
    `<prefix>/snapshots/` object is written at all** — a default-configured
    engine's bucket is byte-identical to what ships today, asserted directly
    against the mock store's key set.
  - _Requirements: 8.1–8.6_

- [ ] 5. **Fencing**

  - `fencing_mode::compare_and_swap`: track the `object_version` per key in
    the mirror; write `<prefix>/term` and `<prefix>/voted_for` with
    `If-Match: <tracked version>` (the safety chokepoint — no second writer
    reaches a double vote, a second leader or a diverging committed log
    without passing through one of them); write each `<prefix>/log/<index>`
    **create-only**, which costs nothing and is the only thing that catches
    a **stale leader appending without ever changing its term**; leave
    snapshot writes and DELETEs unconditional as a **stated residual**, not
    a covered case.
  - `<prefix>/owner` written create-only at construction with
    `{owner_id, epoch, started_at}`; construction over a prefix owned by
    another writer throws unless `takeover_epoch` exceeds the recorded
    epoch, making takeover an explicit auditable act rather than a race.
  - `persistence_fenced_error` naming key, expected version and provider;
    **the latch**: every subsequent mutating call throws the same error
    without contacting the store, and the latch is not clearable at runtime.
  - `compare_and_swap` requires `conditional_key_object_store` at **compile
    time**. A provider that cannot express the precondition gets a compile
    error, never a silent degradation to unconditional writes.
  - Conformance case: the genuine two-engine race — A writes, B is
    constructed over the same prefix and writes, A's next write fails with
    `persistence_fenced_error` and A is latched.
  - Conformance case: the **stale-appender race** — A appends log entries
    without ever writing term or vote while B owns the prefix; one of the
    two latches at the first colliding index. This is the case the
    chokepoint argument alone does not cover, so it is tested separately
    rather than folded into the race above.
  - Verify: the two-engine race passes against `mock_object_store`; a
    `static_assert`-driven negative compile test shows `compare_and_swap`
    is unavailable for a store lacking the refinement; **a case pins that
    S3's 409 does not latch and its 412 does** (task 0.3's finding).
  - _Requirements: 9.1–9.9_

- [ ] 6. **Object integrity and size limits**

  - `verify_checksums` (default on): every PUT carries an end-to-end
    checksum the service verifies, per task 0.5's per-provider spelling;
    where the returned ETag is a deterministic function of single-part
    content, verify it locally and throw naming the key on mismatch.
  - `max_object_bytes` caps every PUT; a `save_snapshot` over the cap throws
    naming size, cap, and that multipart is a documented non-goal.
  - Verify: a mock store returning a wrong ETag makes the write throw naming
    the key; an oversized snapshot throws with all three facts in the
    message. A snapshot silently truncated at a provider limit is the worst
    outcome available here, so the error is the deliverable.
  - _Requirements: 7.1–7.4_

- [ ] 7. **`aws_s3_client`**

  - `include/raft/aws_s3_client.hpp` over `Aws::S3::S3Client`, gated by the
    existing `KYTHIRA_HAS_AWS_SDK`. **No `vcpkg.json` change** — the `s3`
    feature is already listed and `CMakeLists.txt:1072` already finds it.
  - Construct `ClientConfiguration` with an explicitly **zero-retry**
    strategy, and record in the header why: the engine's single retry must
    be the only retry or its idempotency argument is unverifiable.
  - Honour `aws_client_config`'s `region`, `endpoint_override`,
    `api_timeout` and credentials provider chain — no S3-specific parallel
    config.
  - Conditional writes: `If-None-Match: *` and `If-Match: <etag>`, **mapping
    412 to `object_precondition_failed` and 409 `ConditionalRequestConflict`
    to a retryable exception, separately**.
  - `ListObjectsV2` paged to completion; a listing that cannot be completed
    throws rather than truncating.
  - `tests/aws_s3_client_unit_test.cpp`: request shape, both conditional
    forms, the 412/409 split, pagination, 404 → `nullopt`, error mapping.
  - Verify: unit test green; the zero-retry strategy is asserted, not
    assumed (a unit case counts requests against a failing endpoint).
  - _Requirements: 1.5, 12.1–12.5, 17.4_

- [ ] 8. **`azure_blob_client`**

  - `include/raft/azure_blob_client.hpp`: Blob REST over httplib with
    `Authorization: Bearer <token>` from `azure_client_config`'s existing
    `TokenCredential` chain — **no `azure-storage-blobs-cpp` dependency**,
    unless task 0.6 refuted the decision.
  - Pin `x-ms-version` to a **named, dated** API version; send
    `x-ms-blob-type: BlockBlob` on PUT; record in the header that an
    unpinned version is how a working client breaks on a service update.
  - `List Blobs` with `prefix`, following `NextMarker` to completion, parsed
    by bounded element scanning rather than an XML dependency — the
    `alibaba_oss_client` approach, including its rule of throwing on any
    structurally unexpected page.
  - `If-Match` / `If-None-Match: *`, 412 → `object_precondition_failed`.
  - `endpoint_override` for a local mock **and for Azurite**.
  - `tests/azure_blob_client_unit_test.cpp` against a local `httplib::Server`.
  - Verify: unit test green including a truncated-XML → throw case; the
    pinned `x-ms-version` appears on every request (asserted from received
    bytes, not from the client's own constant).
  - _Requirements: 13.1–13.6, 17.4_

- [ ] 9. **`gcp_gcs_client`**

  - `include/raft/gcp_gcs_client.hpp` over `google-cloud-cpp`'s `storage`,
    gated by a new `KYTHIRA_HAS_GCP_STORAGE` found independently of
    `KYTHIRA_HAS_GCP_SDK` and `KYTHIRA_HAS_GCP_PRIVATECA`.
  - Add `storage` to the **existing opt-in `gcp` vcpkg feature** and update
    that feature's description, which already explains why it is opt-in.
  - Disable the storage client's retry policy
    (`LimitedErrorCountRetryPolicy(0)` or equivalent).
  - Generation preconditions: `IfGenerationMatch(0)` create-only,
    `IfGenerationMatch(g)` overwrite, with the numeric generation carried as
    the opaque `object_version` — the concept never inspects it.
  - Honour `gcp_client_config`'s `project_id`, `credentials_json` (ADC when
    empty), `endpoint_override`, `api_timeout`.
  - Record the considered-and-rejected hand-rolled-JSON-API alternative and
    its trade-off (works in a default build, but needs service-account JWT
    signing) so the fork is visible rather than re-derived.
  - Verify: unit test green; a build **without** the `gcp` feature still
    compiles everything else and emits a configure-time STATUS message
    naming what is disabled and why.
  - _Requirements: 14.1–14.6, 16.1, 17.4_

- [ ] 10. **`oci_object_storage_client`**

  - Add a **raw-bytes request path** to `oci_http_client` returning status,
    headers and body unparsed, **without changing `request()`'s behaviour or
    any existing caller** — object storage is a data-plane API and parsing
    every response as JSON was a control-plane assumption.
  - `include/raft/oci_object_storage_client.hpp` reusing `oci_signing` and
    `oci_client_config`, **including Instance Principal** (the auth a node
    on an OCI instance actually uses, already implemented in
    `oci_federation.hpp`).
  - Addressing `/n/<namespace>/b/<bucket>/o/<key>` on the `objectstorage`
    regional endpoint, namespace configured or resolved once and cached
    (task 0.8).
  - `if-match` / `if-none-match`, 412 → `object_precondition_failed`;
    `ListObjects` paged via `nextStartWith` to completion.
  - `tests/oci_object_storage_client_unit_test.cpp`.
  - Verify: unit test green; **every existing `oci_http_client` caller and
    test passes unchanged** — the raw path is additive, and a regression
    there would break two shipped, live-verified OCI components.
  - _Requirements: 15.1–15.5, 17.4_

- [ ] 11. **Build integration**

  - Kconfig: `AWS_S3_PERSISTENCE` (depends on `AWS_SDK`),
    `AZURE_BLOB_PERSISTENCE` and `OCI_OBJECT_PERSISTENCE` (depend on
    `HTTP_TRANSPORT_TLS`, the no-SDK-to-find shape `OCI_QUORUM_MANAGER` and
    `ALIBABA_OSS_PERSISTENCE` already use), `GCP_STORAGE` +
    `GCP_STORAGE_PERSISTENCE`. Help text follows the existing convention of
    saying whether a symbol gates a `find_package` or only selects what is
    built, and invents **no** `KYTHIRA_HAS_*` counterpart where nothing is
    being found.
  - `find_package` + gate wiring in `CMakeLists.txt` for GCS only; S3,
    Azure Blob and OCI need none.
  - `DEPENDENCIES.md`: the `storage` component on the opt-in `gcp` feature,
    and nothing else — S3, Azure and OCI add no dependency.
  - Verify: a build with **every** cloud gate off still compiles
    `key_object_store.hpp`, `object_store_persistence.hpp` and
    `object_store_backup.hpp` and still runs the conformance suite against
    `mock_object_store`; each disabled provider produces a named
    configure-time STATUS message.
  - _Requirements: 16.1–16.5_

- [ ] 12. **Backup catalog**

  - `include/raft/object_store_backup.hpp`: `create` / `list` / `verify`
    over a store and a prefix. It **never** instantiates the engine and
    cannot be reached from the Raft hot path — that is a structural
    guarantee, not a convention.
  - Backups at `<backup_prefix>/<backup_id>/`, with the **manifest written
    last** so its presence is the commit point; `list` ignores directories
    without one.
  - Manifest per the design schema: format version, provider, source
    bucket/prefix, backup id, UTC ISO-8601 timestamp, quiesced flag, term,
    voted-for, log index range, snapshot metadata and configuration, and one
    entry per object with key, size and checksum.
  - `verify` checks internal consistency: index contiguity from the
    snapshot's `last_included_index`, every checksum, `current_term` ≥ every
    entry's term.
  - Verify: a backup taken from a **deliberately smeared** source (objects
    mutated mid-copy) **fails `verify` with the inconsistency named**. This
    is the case that distinguishes a manifest from a warning.
  - _Requirements: 10.1–10.5, 10.7_

- [ ] 13. **Restore — both modes, separately named**

  - **Clone restore**: reproduce a backup into a target prefix byte for
    byte, and restore the owner object with a **higher** epoch so a
    returning original fences itself out on its next write.
  - **Seed restore**: preserve the snapshot's state-machine bytes, **replace**
    its configuration with an operator-supplied node set, reset term to the
    snapshot's `last_included_term`, clear the vote, empty the log; refuse
    when the backup has no snapshot; write one prefix per new node and emit
    exactly what the operator must configure.
  - Both refuse a non-empty target without `--force`; `--force` deletes
    engine-owned keys before restoring and **never merges**.
  - Both run `verify` **before writing anything** and abort on the first
    inconsistency, naming it.
  - Verify: a clone-restored prefix opened by a fresh engine matches the
    original field for field; a seed-restored prefix has the new
    configuration, an empty log, no vote, and the snapshot's term; **an
    attempted merge into a non-empty prefix is impossible to produce** —
    there is no code path to it.
  - _Requirements: 11.1–11.6_

- [ ] 14. **`cmd/raft_object_backup` CLI**

  - create / list / verify / restore-clone / restore-seed, `--provider`
    selecting a compiled-in store, credentials from the same configuration
    the engines take, following existing `cmd/` conventions.
  - Asking for a provider that was not compiled in reports it **by name**.
  - Verify: `--help` documents both restore modes as distinct verbs;
    requesting an uncompiled provider exits non-zero naming it; a full
    create → list → verify → restore-clone cycle works against
    `mock_object_store` in a build with zero cloud providers.
  - _Requirements: 10.6, 16.4_

- [ ] 15. **Per-provider emulator / mock tiers**

  - [ ] 15.1 **S3 → LocalStack.** Follow the established
        `aws_quorum_manager_localstack_test` registration and labels; run
        the conformance suite against it.
  - [ ] 15.2 **Azure Blob → Azurite.** Same shape, via `endpoint_override`.
  - [ ] 15.3 **GCS → task 0.7's decision** (`fake-gcs-server` or a
        hand-written mock), with the deciding factor recorded: fidelity on
        generation preconditions specifically.
  - [ ] 15.4 **OCI → extend `tests/oci_mock_server.hpp`** with the object
        routes. Extended, not duplicated, and it keeps **verifying
        signatures from the bytes that actually arrived**.
  - [ ] 15.5 **Alibaba → extend `tests/alibaba_mock_server.hpp`**'s existing
        OSS routes for ETags and any conditional header task 0.3 confirmed.
  - CTest labels `integration;<provider>;mock;object-persistence;cloud`.
  - Verify: the **same conformance suite** is green against all five
    substrates; a deliberately corrupted signature in a one-off build is
    rejected by both signature-verifying mocks (proving verification is
    live, the negative control the Alibaba mock already uses).
  - _Requirements: 17.2, 17.5_

- [ ] 16. **Real-tier suites (compiled, gated, skip-correct)**

  - `tests/<provider>_object_persistence_real_test.cpp` per provider, under
    `KYTHIRA_<PROVIDER>_REAL_TESTS`, **never CTest-registered**, exit-77
    skip naming each missing value, read-only pre-flight whose failure skips
    rather than fails, cost-reporting and signal-teardown fixtures from
    `oci_real_test_support.hpp`.
  - Four load-bearing cases each: fresh-engine read-back; **measured p50/p99
    per-operation latency with the measurement position recorded**; the
    fencing race including the live negative control that a stale-version
    write is genuinely rejected; and backup → verify → clone-restore →
    fresh-engine read-back.
  - Plus the list-after-write empirical check closing task 0.1's cells.
  - Verify: every suite builds; running with no environment exits 77 with
    the SKIP lines naming each missing value; `ctest -N` lists **none** of
    them. The latency report prints in a fixed greppable format so runs and
    providers are comparable.
  - _Requirements: 4.5, 5.5, 17.6, 17.7_

- [~] 17. **CI wiring + bucket provisioning** — **the bucket half is done**
      (August 16, 2026): idempotent `provision-object-persistence-*` scripts in
      `scripts/ci-cloud-credentials/{aws,azure,gcp}/`, each run twice and each
      verified with a real write/read/delete round trip, with cost notes and
      the two traps recorded in their READMEs (Azure's Owner-is-not-blob-data
      and the unregistered-provider `SubscriptionNotFound`). OCI reuses
      `kythira-ci-artifacts`, Alibaba reuses `kythira-ci-5633986662052576`.
      **Not done:** the per-provider least-privilege grants for each CI
      identity, the repository variables, and the toggles below — there is no
      suite to enable yet.

  - Per-provider `REAL_CLOUD_TESTS_<PROVIDER>_OBJECT_PERSISTENCE_ENABLED`
    bundle toggle plus a matching `workflow_dispatch` input, **inside each
    provider's existing job** — the credentials, federation steps and
    zero-bundle guards already live there.
  - Exit 77 fails the bundle loudly: a real-cloud job that silently skips is
    indistinguishable from one that passes.
  - Idempotent provisioning in `scripts/ci-cloud-credentials/<provider>/`
    with a least-privilege policy per provider scoped to the test bucket and
    prefix — object get/put/delete/list only, **no bucket administration**.
  - OCI reuses the existing `kythira-ci-artifacts` bucket; Alibaba reuses
    whatever bucket the existing `ALIBABA_OSS_BUCKET` repository variable
    names. Only AWS, Azure and GCP need a new bucket, created with public
    access blocked and a lifecycle rule expiring test prefixes.
  - Cost estimates in each provisioning README — these suites' request
    counts are rounding errors against storage minimums, which is the honest
    counterpart to Requirement 6's production cost warning.
  - Verify: workflow YAML parses; a dispatch with the toggles off skips;
    **a dispatch with a bundle on fails closed naming the missing
    variables** (verifiable before any bucket exists); each provisioning
    script run twice is idempotent.
  - _Requirements: 18.1–18.4, 18.6_

- [ ] 18. **Documentation, operator examples, close-out**

  - `doc/cloud_object_persistence.md`: the durability contract and its
    **per-provider evidence table with the confirmation column intact**; the
    consistency table; the measured latency figures **with their measurement
    positions**; the election-timeout sizing rule with both worked rows; the
    cost and throughput envelope with the worked per-provider examples; the
    object layout including retention; fencing modes, limits and the
    takeover procedure; and the backup/restore runbooks for **both** modes.
  - A plain statement of which provider engines are live-verified and which
    are documentation-derived, written as of the day it is written rather
    than aspirationally.
  - Per-provider operator examples in the shape of
    `docker/alibaba_quorum_manager/README.md`, each carrying the latency
    warning.
  - doc/TODO.md's "Cloud key-object persistence engines — write the spec"
    entry closed out with this spec's location and its verification status;
    `doc/CHANGELOG.md` entry; README.md provider list.
  - Cross-reference `.kiro/specs/alibaba-cloud-services/` Requirements 15.7
    and 15.8 from here, so the deferral chain is traceable in both
    directions.
  - Name, as explicit future work, the two changes that would lift the
    limits this spec accepts: a batched `append_log_entries` and a
    `save_hard_state(term, vote)` on the `persistence_engine` concept —
    stating that both are concept changes affecting every engine, not cloud
    concerns.
  - Verify: every Requirement 19 artifact exists; the WAN latency figure
    appears **nowhere** as a production number; no provider's measurement is
    quoted for another.
  - _Requirements: 6.2–6.5, 19.1–19.5_

- [ ] 19. **Live verification**

  - Run every provider's real suite against a real bucket; fold every live
    correction back into spike-notes.md/requirements/design **in place**
    (the OCI doctrine).
  - **Re-run the Alibaba real suite** (**DONE**, see below) and record the
    result: this is the
    verification that the task 2/3 hoist preserved a live-verified
    component, and it is the reason the hoist was acceptable at all.
    **Done August 16, 2026** — 4/4 green post-hoist, wall times recorded in
    spike-notes.md Finding 7. The remaining providers' runs, the CI toggles
    and the measured p50/p99 are still outstanding.
  - Record each provider's measured p50/p99 in the documentation with its
    measurement position; update the election-timeout table's second row
    with real numbers, replacing the hypothetical.
  - Flip the CI toggles; one green dispatched run per provider bundle.
  - Verify: green run URLs recorded in spike-notes.md; leak audit clean;
    **the documentation's live-verified column now names more than one
    provider**, and any provider still documentation-derived says so.
  - _Requirements: 4.5, 17.8, 18.5, 19.3_

## Notes

- **The hoist is the risky task, not the new providers.** Tasks 2 and 3 move
  a shipped, live-verified engine. Task 3's verification bar is deliberately
  brutal — the existing 41 unit cases, the mock suite and the real suite pass
  **unmodified** — because the only honest way to refactor a component whose
  value is "it has run against a real service" is to prove the tests that
  established that still pass without being edited into agreement.
- **Do not let `compare_and_swap` degrade.** If a provider cannot express
  the overwrite precondition, the correct outcome is a compile error, not a
  runtime fallback to unconditional writes. A fence that is believed in and
  absent is worse than no fence, and this is the single most likely place
  for a well-meaning "just make it work" change to destroy the feature.
- **Alibaba may be the provider that cannot fence** (task 0.3). That is an
  acceptable, documented outcome, and specifically must not be worked around
  by relaxing Requirement 9.8 for the provider this spec's reference
  implementation came from.
- **Retention is not backup**, and the documentation says so as part of the
  retention feature rather than in a separate document. Retained snapshots
  share a bucket, a prefix, a credential and a blast radius with the thing
  they would be recovering from.
- **The two restore modes stay two verbs.** Merging them into one `restore`
  with flags is how an operator in a recovery window produces a split-brain
  by typing one word.
- **httplib rewrites query strings** (`spike-notes.md` Finding 8): it
  re-encodes parsed parameters, leaving `, ! $ ' ( ) * ; /` literal and
  turning spaces into `+`. This was verified harmless against Alibaba, which
  canonicalises from parsed parameters. It has **not** been verified against
  Azure Blob or OCI Object Storage, both of which this spec puts on httplib
  and both of which sign requests — check it before assuming it transfers.
- **No task may be checked off against the mock tier if it requires live
  verification.** The doctrine every provider spec in this repo follows, and
  the one the OCI and Alibaba specs both paid for in real defects found only
  on first contact with a live service.
