# Implementation Plan — Cloud Object Persistence

## Status: tasks 1-13 done; task 0 closed except 0.7 (needs a container runtime)

**The client wave is complete, August 17, 2026.** Tasks 7, 8, 9 and 10
(`aws_s3_client`, `azure_blob_client`, `gcp_gcs_client`,
`oci_object_storage_client`) are done, and with them the one piece task 6 left
open: `alibaba_oss_client` now signs and sends `Content-MD5` and declares
`version_is_content_md5`, so both halves of Requirement 7 hold for the one
shipped provider, re-verified live. **All five providers now have a client**,
and `object_store_persistence_engine` is generic over every one of them.

**Task 11 (build integration) is also done**: every provider is now
independently gateable, and each gate was verified to remove its target rather
than merely to print that it had. `configs/no_cloud_defconfig` makes
Requirement 16.3's all-providers-off configuration reproducible — the engine's
213 conformance cases and the backup catalog's 25 both pass in it.

**Tasks 12 and 13 (the backup catalog and both restore modes) are done too**,
with the restore runbooks in `doc/cloud_object_persistence.md`. Next is task 14
(the CLI), which owes Requirement 16.4's provider-reporting half and the exact
command sequences that document currently leaves open.

One honest distinction to carry into task 18: `alibaba_oss_client` is
**live-verified** end to end. `aws_s3_client`, `azure_blob_client`,
`gcp_gcs_client` and `oci_object_storage_client` are **documentation-derived
plus live task-0 probes of the wire questions they rest on** — and for GCS that
probe base is unusually wide, because writing the client turned up five more
service and library facts that Finding 12 had not covered (Finding 17). Their
own real suites are task 16.

**Done:** the seam (task 1), the generic engine (task 2) and the Alibaba
instantiation (task 3), August 15, 2026. Together they add **no capability on
purpose** — a default-configured bucket is byte-identical to what shipped,
which is the property task 3's unmodified test suites demonstrate.

**Task 4 (snapshot retention) is done, August 17, 2026** — the first task to
add a capability, and it adds it strictly above the default: at
`snapshot_retention = 1` the bucket *and the request pattern* are unchanged.
It is also the first feature to arrive with its option rather than ahead of
it, which is what `object_persistence_options` being deliberately partial was
for.

**Task 5 (fencing) is done, August 17, 2026** — the first task whose whole
value is a *refusal*, and the one Requirement 9.8 was written for: it is a
**compile error** for Alibaba OSS and available for the other four. Everything it
needed already existed against `mock_object_store`, so it landed before any of
the new clients. Details, the design correction it forced, and the residual it
found are in the task entry below.

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

**Task 0, Azure Blob / GCS / OCI: closed live, August 17, 2026** (Findings
11-13), which unblocks tasks 5 and 6. All three express every conditional
primitive, **conditional delete included** — so **four of the five providers
can carry `compare_and_swap` and only Alibaba OSS cannot**, exactly the
outcome Requirement 9.8 exists for. Two results are worth carrying into the
client tasks because they are the opposite of a convergence: **Azure answers
409 `BlobAlreadyExists` to create-only but 412 `ConditionNotMet` to
`If-Match`** — one provider, two statuses, both meaning "you lost" — and the
**ETag is a content MD5 on only two of the five** (S3, OSS), being a UUID on
OCI, an opaque timestamp token on Azure and an opaque token on GCS, whose
version is a decimal **generation carried as a query parameter**. What remains
of task 0 is 0.2 (an OCI documentation search), 0.4 (per-provider size limits)
and 0.7 (`fake-gcs-server` fidelity, which needs a container runtime this host
does not have).

**All five providers have a bucket**
(task 17's bucket half, done August 16, 2026: `kythira-ci-827617851594` on S3,
`kythirarealtestobj`/`kythira-raft` on Azure Blob,
`kythira-ci-prefab-sky-500619-s9` on GCS; OCI and Alibaba already had one), and
all five authenticate from this host. Two credential caveats worth knowing
before reaching for them: **AWS's working credentials are in the `personal`
CLI profile** and resolve to the account **root** — fine for a read-only spike,
wrong for least-privilege CI — and **GCP's user credential is expired**, though
application-default credentials still work via
`CLOUDSDK_AUTH_ACCESS_TOKEN`. Full table in spike-notes.md.

The probes live in `scripts/object-store-probes/` — one per provider, all five
written and run. The shared rule they encode is in `probe_common.py`: verdicts
key on the **status and the service's own error code**, never on the effect;
`cas_verdict()` refuses to call CAS usable unless a *correct* precondition is
accepted, which is what caught a probe bug that read exactly like "OCI cannot
do CAS"; and response headers are case-insensitive, because OCI spells the
ETag header `etag` and a plain dict lookup silently yielded an empty
precondition.

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

  - [x] 0.1 **List-after-write, empirically** — for Azure Blob, OCI Object
        Storage and Alibaba OSS (S3 and GCS document it explicitly and need
        only a confirming run). Write N objects under a fresh prefix, LIST
        immediately, assert all N appear; repeat under concurrency. This is
        the one consistency question the engine cannot detect at runtime —
        a lagging listing produces a silently short log at recovery — so
        documentation is deliberately the weaker evidence.
        **CLOSED for all five, live** — 3 × 25 objects, immediate LIST,
        complete every round on every provider (spike-notes.md Findings 5, 10,
        11, 12, 13). OSS, **Azure Blob and OCI** are recorded as *empirically
        consistent* rather than guaranteed, since none of those three vendors
        publishes a listing-specific statement; S3's and GCS's runs confirm
        documentation that is already explicit. One methodological correction
        came out of the OCI run and is now part of the check: the comparison is
        **listing-versus-acknowledged-writes**, never listing-versus-25 — a
        round where PUTs were refused and the listing is correspondingly short
        is a complete listing, and reading it otherwise invents a consistency
        defect out of an authorization flake.
  - [x] 0.2 **Durability-on-response wording for OCI** — **closed with a
        negative result, August 17, 2026** (Finding 15). Find or fail to find
        primary documentation that a 2xx PUT means durably stored before the
        response. If it does not exist, say so in that provider's section
        rather than letting the four confirmed providers' wording stand in
        for it (Requirement 4.2's honesty rule).
        **It does not exist.** Oracle publishes the steady-state redundancy
        statement and a read-side strong-consistency statement, and **neither
        says when the redundancy exists relative to the PUT response** — the
        one claim the engine's durability contract needs. The N1 cell stays
        OPEN in the durability table, now reading "searched and not found"
        rather than "not yet searched", and the operator documentation must
        carry the gap visibly. This blocks nothing in the client: the engine
        behaves identically either way. A support ticket is the remaining
        avenue and is recorded as such — a support answer is evidence of a
        different kind from published documentation and must be labelled that
        way if it arrives.
  - [x] 0.3 **The conditional-write matrix, live, per provider** — create-only
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
        (spike-notes.md Finding 10).
        **Azure Blob, GCS and OCI: CLOSED live, August 17, 2026** (Findings
        11-13). All three express every primitive, conditional delete included
        — which closes the OCI cell this plan called unconfirmed and makes
        **four of five providers fenceable; only OSS is not**. The rejection
        spellings do not converge and must not be guessed: **Azure answers 409
        `BlobAlreadyExists` for create-only but 412 `ConditionNotMet` for
        `If-Match`** — one provider, two statuses, both meaning "you lost" —
        while GCS answers 412 `conditionNotMet` to both (via generation *query
        parameters*, not headers) and OCI answers 412 `IfNoneMatchFailed` /
        `IfMatchFailed`. Set against S3's 409 meaning "retry", the rule stands
        reinforced: **no client may map a bare status; it must read the error
        code.**
  - [x] 0.4 **Single-PUT size limits** per provider; `max_object_bytes`'s
        default is chosen from the smallest (Requirement 7.3). **Closed from
        primary documentation, August 17, 2026** (Finding 16): S3 **5 GB**,
        OSS **5 GB**, Azure **5,000 MiB** *for `x-ms-version` 2019-12-12 and
        later* (256 MiB before — a second reason the client pins a dated
        version), OCI **50 GiB**, GCS **5 TiB**. Azure is the only one that
        names the failure: **413 Request Entity Too Large**.
        **The default is deliberately far below the smallest**, and
        Requirement 7.3 is amended to say why: the binding limit is the
        engine's shape, not the service's. One retry, no multipart, no
        resumption, the mutex held for the whole round trip, ~2-3 s measured
        per round trip, and AWS's own advice to abandon single-PUT above
        100 MB. **Recommended default: 64 MiB**, configurable upward — the cap
        exists to turn "this deployment has outgrown a single-PUT persistence
        engine" into a loud error at the first snapshot that reaches it, not to
        predict the service's 413.
  - [x] 0.5 **Checksum spelling** per provider — `Content-MD5` where
        universally supported, the provider's native header where preferred
        — and for which providers the returned ETag is a deterministic
        function of single-part content (Requirement 7.2).
        **CLOSED for all five, live.** The service-side check exists
        everywhere and is spelled differently everywhere — `InvalidDigest`
        (OSS), `BadDigest` (S3), `Md5Mismatch` (Azure), `invalid` (GCS),
        `UnmatchedContentMD5` (OCI) — so a client may not guess it and a
        *probe* may not treat "any 4xx" as proof the digest was checked (that
        mistake produced a false "VERIFIED" line once; Finding 13).
        **Requirement 7's two halves come apart**: the ETag is the MD5 hex of
        single-part content only on **S3 (lowercase) and OSS (uppercase)** — so
        a local ETag check must compare case-insensitively — while Azure's ETag
        is an opaque timestamp token, OCI's is a **UUID** and GCS's is an
        opaque token. On those three, local verification must read
        `Content-MD5` / `opc-content-md5` / the `md5Hash` metadata field
        instead, all three of which were confirmed to match the content
        (spike-notes.md Findings 4, 10, 11, 12, 13).
  - [x] 0.6 **Azure Blob: REST vs SDK decision checkpoint** — **closed in
        favour of the recorded decision, August 17, 2026.** The probe
        (`probe_azure_blob.py`) *is* the hand-rolled surface, and nothing about
        it was harder than documented: a bearer token from the CLI's own
        resolver, one dated `x-ms-version`, `x-ms-blob-type: BlockBlob` on PUT,
        `?restype=container&comp=list&prefix=` for listing. No
        `azure-storage-blobs-cpp` dependency is needed. Three details the
        client header must record because each is a plausible wrong guess:
        **success is 201 on PUT and 202 on DELETE**, the machine-readable error
        code is the **`x-ms-error-code` response header**, and a bodiless PUT
        still needs an explicit `Content-Length: 0` (Finding 11).
  - [~] 0.7 **GCS mock tier decision.** No Google-supplied GCS emulator
        exists. Assess `fake-gcs-server`'s fidelity **specifically on
        generation preconditions**; if it is not trustworthy there, the
        fallback is a hand-written mock. Emulator fidelity on the newest,
        least uniformly implemented feature in this design is exactly what
        cannot be assumed.
        **Still open, and deliberately not guessed at** — it needs the emulator
        run and this development host has no container runtime. What the live
        run *did* produce is the **fidelity bar**, now stated as measurements
        rather than expectations (Finding 12): `ifGenerationMatch=0` on an
        existing object must be **412 `conditionNotMet`** and must leave the
        object unchanged; a stale `ifGenerationMatch` must refuse an upload
        *and* a delete; and the generation must advance on every write.
  - [x] 0.8 **OCI namespace and endpoint** — **closed live, August 17,
        2026** (Finding 13). The endpoint is
        `https://objectstorage.<region>.oraclecloud.com` and the namespace is
        **both** configurable and resolvable: `GET /n/` returns it as a bare
        JSON string. `oci_object_storage_client` should therefore accept a
        configured namespace and resolve-and-cache only when none is given —
        one call at construction, never per request.
        **One signing consequence, found by writing the signer:**
        `oci_signing.hpp` signs `content-type: application/json` as a
        constant, because every existing caller is a control-plane API. Object
        storage is a data plane with opaque bodies, so the signed content type
        must become a **parameter** as part of task 10's raw-bytes path.
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

- [x] 4. **Snapshot retention** — done August 17, 2026, in
      `include/raft/object_store_persistence.hpp`, with 11 new conformance
      cases (8 plain, 3 injecting a failure at each of the three steps).
      `snapshot_retention` counts generations **including** the live slot, so
      the default of 1 is today's behaviour without a special case: no
      `<prefix>/snapshots/` object, one PUT per `save_snapshot`, no LIST and no
      DELETE — asserted as a **request-count** case, not only a key-set one.

      **Three decisions, folded into requirements.md/design.md in place:**
      step (c) does not throw (it runs after the commit point; it records
      `last_prune_error()`, keeps the index, and retries on the next save —
      observable and self-healing, where an exception would report a lost
      snapshot that is not lost and would abandon `node::install_snapshot`'s
      term write over a garbage-collection error); the retained indices are
      noted at construction from the listing the load path already performs,
      so pruning issues no LIST of its own and the load path still never GETs
      a retained copy; and an unparseable key under `<prefix>/snapshots/` is
      neither pruned nor fatal — the **opposite** of the same shape under
      `<prefix>/log/`, because that one breaks recovery and this one is never
      read.

      Verified against a mutant as well as against the tests: skipping the
      prune and writing a retained copy at retention 1 fail 13 cases,
      including the two pre-existing key-set cases.

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

- [x] 5. **Fencing** — done August 17, 2026, in
      `include/raft/object_store_persistence.hpp`, with 10 new fencing
      conformance cases **and the whole existing suite re-run with fencing on**
      (136 cases in `object_store_persistence_unit_test`, was 67). The second
      full run is the substance, not a bonus: `compare_and_swap` rewrites the
      precondition on most of the engine's writes, so the evidence it broke
      nothing is the other 54 cases passing with it enabled — and that doubles as
      the negative control the fencing cases need, since "the stale write was
      rejected" says nothing about a fence unless every legitimate write is still
      accepted.

      **One design correction, folded into requirements.md and design.md in
      place: `fencing_mode` is a template argument, not an options field.** The
      sketched runtime field cannot satisfy Requirement 9.8. It would have to be
      read behind `if constexpr` so a store lacking the refinement still compiles
      the rest of the engine — and a discarded `if constexpr` branch is never
      instantiated, so `compare_and_swap` over such a store would compile and then
      run **unconditional writes with fencing configured**: 9.8's forbidden
      outcome, reached through the mechanism meant to prevent it. It sits last in
      the parameter list, so no existing instantiation changed.
      `alibaba_oss_persistence.hpp` now carries
      `static_assert(!conditional_key_object_store<alibaba_oss_client>)`, which
      turns the live OSS finding into a compile-time fact.

      **One residual discovered by implementing it, recorded rather than papered
      over:** a conditional PUT whose response is lost *after* the service applied
      it leaves the engine holding a stale version, so its next write to that key
      is refused and it latches although it is the only writer. Loud and safe, but
      an availability cost `none` does not have, and inherent to predicating a
      write on a version learned only from the response — the caller's own retry
      re-issues the same stale precondition, so declining to retry does not avoid
      it. Two things follow and are implemented: a conditional PUT is issued
      **once** whatever `write_retries` says, and **only**
      `object_precondition_failed` latches. The identified mitigation — a
      read-repair that adopts the returned version when the object already holds
      the intended bytes — is named as future work in design.md rather than
      implemented, because it weakens "a rejected precondition means you lost",
      which is the sentence the safety argument is written against.

      Two claims were **checked rather than assumed**. `raft.hpp`'s AppendEntries
      path appends only where `entry_index > get_last_log_index()` and truncates a
      conflicting index through `_persistence.truncate_log` first, so create-only
      log PUTs never refuse a legal append (a create-only precondition that did
      would latch a healthy leader). And a restart by the recorded owner must be
      *allowed*: a crash-restart and a duplicated deployment are indistinguishable
      at construction, since a duplicate carries the same node identity — which is
      the concrete reason the fence has to live on the writes and a
      construction-time check cannot be it.

      Mutation-tested before being believed, five mutants, each caught: not
      recording the latch (10 failures), degrading single-slot writes to
      unconditional PUTs (15), dropping create-only from log PUTs (3, all in the
      stale-appender case), latching on *any* store failure (1 — the 409/412
      case), and opening a prefix another owner holds (1).

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

- [x] 6. **Object integrity and size limits** — the engine's half done
      August 17, 2026, in `include/raft/object_store_persistence.hpp`, with 9 new
      conformance cases and 4 MD5 known-answer cases (213 cases in
      `object_store_persistence_unit_test`, was 136). The **whole** suite now runs
      over three substrates — plain, fenced, and content-MD5-versioned — because
      "the engine verifies digests" is worth having only if every legitimate write
      still passes the verification.

      **The correction this task forced, folded into requirements.md as a new
      criterion 7.0 and into design.md in place: Requirement 7 spans two layers
      and one flag cannot govern both.** 7.1's service-side check is a header the
      client sends and the service evaluates, so it is each **client's** own
      configuration — the engine does not speak HTTP and the client is
      constructed by the caller, so an engine option could never reach it. 7.2's
      local check is the **engine's**, once, over a `content_md5_versioned_store`
      trait the client declares. This task's own verification bar settles that
      independently: *a mock store returning a wrong ETag must make the write
      throw*, and a mock store is not a client. `verify_checksums` therefore
      governs the local half only, and says so.

      Three decisions, each recorded because the obvious alternative is worse.
      **A checksum mismatch is retryable, not fatal** — it sits inside the single
      PUT retry, because a corrupted transfer is exactly what re-sending the
      identical bytes to the identical key repairs. **The MD5 is hand-rolled in
      the engine header**, beside the base64 codec that is there for the same
      reason: this header must compile with every cloud gate off, and OpenSSL
      reaches this tree only through the gated provider signing headers. It is
      pinned against RFC 1321 §A.5, the three padding boundaries and an
      all-256-bytes input — and the first transcription of that suite's 80-digit
      vector was wrong, which is precisely what a known-answer test is for.
      **Quotes and case are normalised at the comparison**, not at the client: S3
      spells the hex lowercase, OSS uppercase, and OSS's client returns the ETag
      verbatim with its quotes because it is an opaque token that goes back to the
      service unmodified.

      `max_object_bytes` defaults to **64 MiB** and caps every PUT, refusing it
      *before* the request; `0` is rejected at construction. The error names all
      three facts Requirement 7.3 asks for — size, cap, and that multipart is a
      documented non-goal — and the cases assert all three are present, plus that
      raising the cap admits the very snapshot it refused (a limit, not a hidden
      one).

      Mutation-tested, four mutants, each caught: the size cap never firing (16
      failures), the digest check never running (6), the comparison not
      normalising case (54), and one corrupted MD5 constant (14).

      **Not done in this task: 7.1's header on `alibaba_oss_client`**, which does
      not send `Content-MD5` today. It is the one shipped client, and adding the
      header touches `alibaba_signing` (which signs `content-md5` when present) on
      a live-verified component, so it belongs with the client work of tasks 7-10
      rather than smuggled in here. Note the two encodings when it is done:
      `Content-MD5` is **base64 of the raw 16 bytes**, the ETag is **hex**.

      **Closed with task 7, August 17, 2026.** `alibaba_oss_client` now signs and
      sends `Content-MD5` on every PUT and declares `version_is_content_md5`, so
      both halves of Requirement 7 now hold for the one shipped provider. Two
      consequences were found by making the change rather than reasoned about
      beforehand, and both are the interesting part:

      - **Declaring the trait turned two mocks' invented ETags into a defect.**
        `alibaba_oss_persistence_unit_test`'s bucket returned **no ETag at all**
        and `alibaba_mock_server.hpp` returned a truncated SHA-256. Harmless
        placeholders while nothing compared them; the moment the engine verifies
        every PUT's returned version, each of them fails every write in its file.
        Both now mint the ETag OSS actually returns — the content MD5 in
        **uppercase hex, quoted** — which makes those suites a negative control
        for the verification rather than a casualty of it.
      - **Both mocks now verify the digest** and answer `InvalidDigest` on a
        mismatch, as live OSS does. A `Content-MD5` no mock reads is a header
        that is transmitted, not a check that is performed.

      The signing half needed no change to `alibaba_signing`: OSS V4 canonicalises
      `content-md5` when the request carries one, by exactly the rule it uses for
      `content-type`, and the mock server derives its signed set **from what
      arrived** — so a header sent outside the signed set fails as
      `SignatureDoesNotMatch` rather than passing quietly.

      **Re-verified live, August 17, 2026, because this lands on a live-verified
      component**: `alibaba_oss_persistence_real_test` 4/4 green against
      `kythira-ci-5633986662052576` in `ap-southeast-1`, 65.7 s total (9.3 / 34.3
      / 15.7 / 6.4 s per case), residual objects under the test prefix **0**,
      counted from a listing. That run is what makes the trait declaration a
      live fact rather than a documented expectation: real OSS accepted the
      signed `Content-MD5` on every PUT — a header outside the signed set is
      `SignatureDoesNotMatch`, a wrong digest is `InvalidDigest` — and every
      real ETag matched the digest the engine computed locally. Three mutants
      confirm the path is not inert: a mock minting a wrong ETag fails 30 cases
      in the unit suite and 13 in the mock suite, and a client sending the digest
      of the wrong bytes fails 14.

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

- [x] 7. **`aws_s3_client`** — done August 17, 2026, in
      `include/raft/aws_s3_client.hpp`, with 29 cases in
      `tests/aws_s3_client_unit_test.cpp` against a local `httplib::Server`. The
      first of the four clients, and the only one that adds no dependency at
      all: `${AWSSDK_LINK_LIBRARIES}` already carries `s3`.

      **Every assertion is made against the bytes that arrived**, never against
      the client's own fields — the value of a client is entirely what it puts
      on the wire, and a test reading the client back would pass just as
      happily if nothing were sent. The mock records requests and the test
      thread asserts on them afterwards, rather than asserting inside a server
      thread where a failure is reported against the wrong case.

      Four decisions worth not re-deriving:

      - **412 and 409 are read from the service's own error code, and the 409
        test comes first.** Neither is modelled in `Aws::S3::S3Errors` — this
        SDK has no `PRECONDITION_FAILED` enumerator — so both arrive as
        `UNKNOWN` carrying the wire code in `GetExceptionName()`, and keying on
        the error *type* would collapse them into one another. Testing
        `ConditionalRequestConflict` before the 412 fallback means a future S3
        that answered the conflict with some other 4xx still would not latch.
      - **The SDK's flexible-checksum default is switched off**
        (`requestChecksumCalculation = WHEN_REQUIRED`) and an explicit
        `Content-MD5` sent instead. The default computes a CRC32 per request and
        can move the body onto `aws-chunked` trailer framing to carry it; the
        pinned header is in the encoding task 0.5 verified, and a case asserts
        `x-amz-sdk-checksum-algorithm` and `x-amz-trailer` are absent from the
        wire.
      - **`delete_object_if` with `if_absent` throws.** S3's DeleteObject models
        `If-Match` and **not** `If-None-Match`, so that precondition has no wire
        spelling; the only alternative is an unconditional DELETE issued by a
        caller who believes a guard is in place, which is Requirement 9.8's
        doctrine one layer down. The engine never issues the combination — its
        DELETEs are unconditional by design — so this guards a future caller.
      - **The truncated-listing guard stops a hang, not just a short answer.**
        Found by deleting it: the mutant did not fail the suite, it *hung* it.
        An empty continuation token is carried into the next iteration, which
        re-issues the identical request and receives the identical truncated
        page, forever, holding the engine's mutex.

      One residual is stated rather than glossed: `alibaba_oss_client` throws on
      a listing that carries **no `<IsTruncated>` at all**, because it owns its
      parser; the SDK's generated parser defaults that field to false, which
      this client cannot tell from an explicit false. Real S3 always sends it,
      so the difference is in what a *malformed* response does.

      **Mutation-tested, and three of the first nine mutants survived — all
      three because the mutation was inert, which is its own finding.** Removing
      the `ConditionalRequestConflict` early-return changes nothing, because the
      status fallback below it only fires when the error *code* is empty; the
      same is true of the `NoSuchBucket` early-return. Both are defence against a
      future reordering of those functions rather than against the service, and
      the *real* bugs — keying the verdict on the 4xx class, or on a bare 404 —
      are caught (3 and 1 failures). The third survivor is the addressing finding
      above. Ten mutants in total are now caught: content-MD5 not sent (1), the
      SDK's checksum default restored (4), SDK retries restored (4), a truncated
      listing returned silently (1), `if_absent` DELETE silently unconditional
      (2), `If-None-Match` not sent (1), precondition keyed on the status class
      (3), 409 and 412 both latching (2), any 404 read as absence (1), and
      path-style not forced on a hostname endpoint (1).

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

- [x] 8. **`azure_blob_client`** — done August 17, 2026, in
      `include/raft/azure_blob_client.hpp`, with 32 cases in
      `tests/azure_blob_client_unit_test.cpp` against a local `httplib::Server`.
      Task 0.6's decision held: no `azure-storage-blobs-cpp` dependency, no
      SharedKey signing, AAD bearer token from the credential chain
      `azure_client_config` already carries.

      **The suite caught a bug that would have shipped, and it is the kind no
      round-trip test can see.** `Azure::DateTime` is *not* a `system_clock`
      time_point: its clock counts 100 ns ticks from **year 0001**. Reading
      `ExpiresOn.time_since_epoch()` and treating it as a Unix duration put every
      bearer token about two millennia in the future, so the first token was
      cached forever and every write would have started failing an hour after
      startup — in production, not in any test that only writes and reads back.
      The refresh-margin case is what failed. `static_cast` to
      `system_clock::time_point` (the explicit conversion operator azure-core
      provides) is the only correct route.

      Five decisions, each recorded in the header:

      - **Both of Azure's rejection statuses latch**, and each is recognised by
        its *error code*: 409 `BlobAlreadyExists` for a lost `If-None-Match: *`,
        412 `ConditionNotMet` for a lost `If-Match`. Requirement 13.4's original
        wording named only the 412 — which would have let a create-only collision,
        the exact case that catches a stale leader appending log entries, pass as
        an ordinary error and fence nothing. A 409 that is *not*
        `BlobAlreadyExists` (a lease conflict, say) must not latch, and S3's 409
        means the opposite outright, so the status is consulted only when no code
        arrived, and then only for 412.
      - **DELETE is not idempotent on the wire and is made so here.** Azure
        answers **404 `BlobNotFound`** where S3 and OSS answer 204. The concept
        requires an idempotent delete and the engine's truncation depends on it.
        A 404 naming the *container* stays an error — swallowing that would let
        an engine pointed at a non-existent container start up as though its
        prefix were merely empty, and then delete "successfully".
      - **`version_is_content_md5` is deliberately absent**, pinned by a
        `static_assert` on the negative. Azure's ETag is an opaque timestamp
        token, so declaring the trait would make the engine compare that token
        against an MD5 and reject **every** write. The trait being a declaration
        by the client rather than an inference by the engine is what makes this a
        non-event. `Content-MD5` is still sent — 7.1's service-side half — and
        Azure's GET-side `Content-MD5` response header is named as the route a
        future local check would take.
      - **Listing has no `IsTruncated` flag**, so the structural guard cannot be
        "is the flag present". It is instead that the body must be an
        `<EnumerationResults` document: without that, any unexpected 200 body
        parses as *zero blobs*, which a persistence engine reads as an empty log
        rather than as an error. An empty or absent `<NextMarker>` is how Blob
        says "last page".
      - **Blob listings have no `encoding-type` parameter** — the escape hatch
        `alibaba_oss_client` uses to sidestep XML entities entirely does not exist
        on this API — so names are XML-unescaped here. The engine's own keys never
        contain a metacharacter; a foreign key under the same prefix can, and
        returning it mangled would make the listing disagree with the store.

      One deviation from the design sketch, recorded: the sketch's constructor
      took `(azure_client_config, account, container)`, but the container **is**
      the concept's `bucket` parameter and arrives per call. Storage-specific
      settings live in a new `azure_blob_config` that embeds `azure_client_config`
      for the credential chain and timeout, rather than widening the shared struct
      — which carries a subscription, resource group and ARM endpoint that a
      data-plane blob call has no use for — with three fields every other consumer
      would ignore.

      Mutation-tested, twelve mutants, **every one caught**: only-412-latches
      (1), any-409-or-412-latches (1), an absent-blob DELETE treated as an error
      (2), `ContainerNotFound` read as absence (1), the `EnumerationResults`
      guard removed (1), `Content-MD5` not sent (1), `x-ms-blob-type` not sent
      (1), `x-ms-version` not sent (4), the `DateTime` epoch bug reintroduced
      (1), XML entities left encoded (1), `NextMarker` ignored (2), and 201 not
      treated as success (11).

      **Not done here, and it is the honest gap:** no live Azure run. Task 0's
      probe closed the wire questions this client is built on (Finding 11), but
      this client itself has only been exercised against a local server. Task 16
      is where its real suite lands, and until then it is documentation-derived
      in exactly the sense task 18 has to write down.

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

- [x] 9. **`gcp_gcs_client`** — done August 17, 2026, in
      `include/raft/gcp_gcs_client.hpp`, with 45 cases in
      `tests/gcp_gcs_client_unit_test.cpp` against a local `httplib::Server`,
      12/12 mutants caught. The **last of the five clients**; GCS is the only
      one that takes an SDK dependency, and the "considered and rejected"
      note in the header records why (service-account JWT signing, plus ADC —
      a new security-critical component whose failure mode is silent, against
      a library that already has both).

      **The suite caught two bugs that would have shipped, and one of them
      only real GCS could have revealed.**

      - **A zero-byte read reports no generation, and no response headers at
        all.** Measured: reading a 0-byte object returns an ok status, an empty
        body and `headers().size() == 0`, while the same run's 5-byte read
        returned 22 headers including `x-goog-generation`. With no bytes to
        stream the library never captures the response. The first version threw
        on a missing generation — which is right for a *non-empty* read and
        would have failed every legitimate empty-object read. `get_object` now
        falls back to `GetObjectMetadata` and re-checks that `size` is still 0,
        so a version is never reported for bytes that were not read. The
        five-byte leg is the negative control; without it, "GCS does not report
        generations" was a plausible and wrong reading.
      - **google-cloud-cpp rewrites the status message.** `No such object: b/k`
        arrives as `Permanent error, with a last message of No such object:
        b/k`. The HTTP-status→`StatusCode` mapping had been probed by calling
        `rest_internal::AsStatus` directly — correct, but one layer below where
        the client reads — and the prefix match written from it recognised
        nothing. Six cases failed on the suite's first run. Absence is now a
        substring search, and a case asserts the wrapper is seen *through*.

      **GCS is the one provider whose 404 carries no machine-readable
      object-vs-bucket discriminator**: an absent object and an absent bucket
      are both `404 reason=notFound`, differing only in prose (measured live —
      Finding 17.1). S3 has `NoSuchKey`/`NoSuchBucket`, OCI has
      `ObjectNotFound`/`BucketNotFound`; GCS has neither. So absence is a
      whitelist keyed on the message, and every unrecognised 404 throws. The
      failure direction is the justification: reading a misconfigured bucket as
      "not written yet" would show the engine an empty Raft log, which is the
      one failure it cannot detect at runtime, whereas a reworded message makes
      absence *throw*. Matching prose is unattractive and is done because the
      alternative is worse.

      Four more decisions, each recorded in the header:

      - **The conditional DELETE answers a 404 two different ways.**
        `if_absent` succeeds (the precondition held; `ifGenerationMatch=0` is
        expressible on a GCS delete, unlike S3's `If-Match`-only DeleteObject)
        and `if_version` throws `object_precondition_failed` (the object the
        caller expected is gone, so it lost). Collapsing them would swallow
        exactly what the fence exists to detect. Both measured live.
      - **`RetryPolicyOption` is `LimitedErrorCountRetryPolicy(0)`**, and here
        the default it replaces is a **15-minute `LimitedTimeRetryPolicy`**,
        not "three tries". Deleting that line does not fail the suite so much
        as *hang* it — the mutant was caught by the per-mutant timeout, which
        is the second time that harness feature has earned itself.
      - **`gcp_detail::make_base_options()` is deliberately not reused.**
        `storage::Client` has its own option set: the endpoint is
        `storage::RestEndpointOption`, not `google::cloud::EndpointOption`, and
        the stall bounds are the public `storage::` ones, not the
        `rest_internal` ones that helper sets. Passing it would have left the
        endpoint override silently ignored and pointed the whole unit suite at
        real GCS.
      - **`version_is_content_md5` is absent**, `static_assert`ed on the
        negative: the version is a generation counter and the ETag is an opaque
        token (`CL/I06jPqJYDEAE=`). `MD5HashValue` is still sent on every insert
        for the service-side check (Requirement 7.1), and `md5Hash` in the
        metadata is named as the route a future local check would take.

      **On the test suite's own credentials**, because the obvious approach is
      a trap: `CLOUD_STORAGE_EMULATOR_ENDPOINT` **overrides** an explicit
      `RestEndpointOption` (measured — with the variable at one local port and
      the client at another, every request went to the variable's port), so a
      suite that used it would exercise the environment variable and never the
      `endpoint_override` field. Each case instead builds a throwaway service
      account whose RSA key is generated at run time, which google-cloud-cpp
      turns into a locally-signed JWT: no network, and no key material in the
      tree for the secret scanner to find.

      **Build integration landed here rather than in task 11**, because the
      client cannot compile without it: `GCP_STORAGE` (Kconfig, *not* depending
      on `GCP_SDK` — the storage component shares no code with the Compute API
      surface), `find_package(google_cloud_cpp_storage)` defining
      `KYTHIRA_HAS_GCP_STORAGE`, `CONFIG_GCP_STORAGE` in both defconfigs (a new
      `default y` symbol would otherwise FATAL_ERROR the non-GCP legs under
      `KYTHIRA_KCONFIG_STRICT`), `storage` in `vcpkg.json`'s `gcp` feature,
      DEPENDENCIES.md, and the target in the GCP CI job's build and ctest
      lists.

      Note on that `vcpkg.json` edit, since it is easy to write up wrongly:
      **`storage` is already in google-cloud-cpp's own default feature set**,
      which this manifest never disables, so adding it changes nothing about
      what is built today. It makes the dependency intentional, so an upstream
      change to those defaults cannot silently remove a component a persistence
      backend needs. Writing it up as "this is what enables GCS" would record a
      false reason for a correct change.

      **To build this locally**, and read this before touching vcpkg:
      `vcpkg_installed` in this repo is a **symlink** to the
      `kythira-libnyoci` worktree's copy — one tree shared by both checkouts —
      and `vcpkg install` *prunes* whatever the requested feature set does not
      need, so running CI's `--x-feature=gcp` in place strips `lakers` and
      `ion-c` out of **both**. Install into a separate root instead, and point
      the two variables this project actually wires vcpkg through at it
      (CMakeLists.txt:29-52 — not the toolchain file):

          /opt/vcpkg/vcpkg install --triplet x64-linux \
            --x-install-root=vcpkg_installed_gcp \
            --x-feature=gcp --x-feature=edhoc --x-feature=ion \
            --no-print-usage --clean-after-build

          cmake -S . -B build-gcp -DCMAKE_CXX_COMPILER=clang++-18 \
            -DCMAKE_PREFIX_PATH=<repo>/vcpkg_installed_gcp/x64-linux \
            -D_VCPKG_INSTALLED_DIR=<repo>/vcpkg_installed_gcp

      `vcpkg_installed_*` is gitignored. Note this is a mixed-tree build —
      CMakeLists still hardcodes `${CMAKE_SOURCE_DIR}/vcpkg_installed/...` for
      boost_context/boost_json — which is fine because both trees came from the
      same baseline, but is worth knowing when a version looks wrong.

      **Verify, as written:** unit suite green (45 cases); a build *without* the
      `gcp` feature configures cleanly and emits
      `google-cloud-cpp storage component not found — gcp_gcs_client disabled
      (configure with --x-feature=gcp to enable the GCS persistence backend)`
      plus the matching test-target skip.
      - _Requirements: 14.1–14.6, 16.1, 17.4_

- [x] 10. **`oci_object_storage_client`** — done August 17, 2026, in
      `include/raft/oci_object_storage_client.hpp`, with 29 cases in
      `tests/oci_object_storage_client_unit_test.cpp`. Adds **no dependency**:
      it reuses `oci_signing`, `oci_client_config` and `oci_http_client`, so
      Instance Principal — the auth a node on an OCI instance actually uses —
      comes along for free.

      **The verify bar was met as written**: all seven existing OCI test targets
      build and pass **unchanged**, 73 cases across
      `oci_signing_unit_test` (9), `oci_http_client_unit_test` (12),
      `oci_federation_unit_test` (11), `oci_quorum_manager_unit_test` (15),
      `oci_heartbeat_writer_unit_test` (5), `oci_quorum_manager_mock_test` (12)
      and `oci_certificates_provider_mock_test` (9). Not one test line was
      edited. The two mock suites verify signatures from the bytes that arrived,
      so their passing is real evidence that parameterising the signed content
      type left every JSON caller byte-identical.

      **The signed content type is now a parameter**, threaded through
      `build_signing_string` → `sign_request_with_key` → `sign_request` →
      `instance_principal_signer::sign` → `oci_http_client::prepared_headers`,
      defaulted to `application/json` at every level so no existing call site
      changed. Task 0.8 predicted this; writing it confirmed the shape.

      **`oci_http_client::request_raw` is additive**, and differs from
      `request()` in three deliberate ways:

      - **No status becomes an exception.** A 404 is an *answer* for a
        persistence load path and an error for a control-plane GET, and only the
        caller knows which. A transport failure still throws — there is nothing
        to hand back.
      - **The content type is caller-chosen and signed.** Object bodies are
        `application/octet-stream`.
      - **No 429 retry.** `request()` retries once (Requirement 1.9); here the
        engine owns retries and its idempotency argument depends on knowing what
        was re-sent.

      It also **lowercases every response header on the way in**, which is not
      tidiness: OCI spells its ETag `etag` and its digest `opc-content-md5`, and
      task 0's probe looked up `ETag`, got nothing, sent `if-match: ""`, was
      refused 412 on a *correct*-ETag write, and read the run as "OCI cannot do
      CAS" — the conclusion Alibaba OSS genuinely earns. Only a negative control
      caught it. The suite asserts the ETag is read under three spellings.

      Three more decisions:

      - **`BucketNotFound` is never an absent object.** It is a real
        misconfiguration *and* the code this tenancy's intermittent
        authorization flake returns at 3-16%; reading it as absence would let a
        flake present as an empty Raft log, which is the one failure this engine
        cannot detect at runtime. `ObjectNotFound` is absence, and a DELETE of an
        absent object succeeds.
      - **`version_is_content_md5` is deliberately absent**, pinned by a
        `static_assert` on the negative: OCI's ETag is a **UUID**. `Content-MD5`
        is still sent (400 `UnmatchedContentMD5`), and `opc-content-md5` on GET
        is named as the route a future local check would take.
      - **The namespace is resolved once at construction** and only when not
        configured, per task 0.8; a case asserts two writes issue no second
        lookup. The conditional headers and `Content-MD5` travel **unsigned**,
        which is correct: OCI's `Authorization` names its signed set explicitly.

      **Mutation-tested, eleven mutants, and one survivor was a real test
      gap rather than an inert mutation** — which is the difference worth
      recording. Dropping the `is_array()` half of the listing guard still
      *threw*, because `get_array()` throws on the wrong kind, so the suite
      stayed green while the caller silently lost the message naming which key
      it was reading. A case for `objects` present-but-not-an-array, asserting
      the message names the listing, closes it and catches the mutant. The other
      survivor is inert for the same reason two of task 7's were: **absence is
      decided by a whitelist of codes**, so deleting the explicit
      `BucketNotFound` exclusion changes nothing — the whitelist already refuses
      every unknown code, and *that* is the property the tests protect (the
      any-404-is-absence mutant is caught). The named check protects the reader,
      not the behaviour, and the header now says so.

      The nine caught outright: the signed content type back to a constant (2),
      the sent content type back to JSON (1), any 404 read as absence (1),
      `Content-MD5` not sent (1), `if-none-match` not sent (1), `nextStartWith`
      ignored (2), the namespace resolved per request (27), and the precondition
      verdict keyed on the 4xx class (1).

      **Not done here, and stated rather than glossed:** no live OCI run. The
      tenancy flake below has to be understood first, or a real-tier failure is
      indistinguishable from a regression. Task 16 is where its real suite lands.

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

- [x] 11. **Build integration** — done August 17, 2026. Every provider is now
      independently gateable, and each gate was verified to actually *gate*
      rather than merely to print that it did.

      **Four new Kconfig symbols**, each in its provider's existing menu:
      `AWS_S3_PERSISTENCE` (depends on `AWS_SDK`), `AZURE_BLOB_PERSISTENCE`
      (depends on `HTTP_TRANSPORT_TLS && AZURE_SDK`), `OCI_OBJECT_PERSISTENCE`
      (depends on `HTTP_TRANSPORT_TLS`) and `GCP_STORAGE_PERSISTENCE` (depends
      on `GCP_STORAGE`). None invents a `KYTHIRA_HAS_*` counterpart, because
      none gates a `find_package`: they select what is *built*. `GCP_STORAGE`
      and its `find_package` had already landed with task 9, since
      `gcp_gcs_client` could not compile without them.

      Two of those dependencies are worth stating, because both look wrong at a
      glance:

      - **`AZURE_BLOB_PERSISTENCE` depends on `AZURE_SDK`** even though task 0.6
        decided to take **no** `azure-storage-blobs-cpp` dependency and the
        client speaks Blob REST over httplib. What it needs from
        azure-core/azure-identity is the **credential chain** its bearer token
        comes from. The decision to avoid the storage SDK stands.
      - **`OCI_OBJECT_PERSISTENCE` joins the `KYTHIRA_OCI_SHARED` condition**
        rather than sitting beside it. `oci_object_storage_client` is built on
        `oci_http_client` and `oci_signing`, so a configuration wanting object
        persistence with the quorum manager and certificate provider both off
        would otherwise ask for a client whose transport was never built.

      **The gates select something, and that was checked in both directions.**
      A symbol that gates nothing is worse than no symbol — this tree already
      has the scar, `COAP_TRANSPORT` having been inert for months while
      appearing to work. So each of the four client suites is now gated on its
      symbol, and:

      - with `configs/no_cloud_defconfig`, all four targets are **absent** from
        the generated build and `ctest -N` lists none of them — not merely
        reported as skipped;
      - with the gates on (`build-default`, and `build-gcp` for GCS) all four
        are **present**, which is the control that makes the absence mean
        something;
      - turning off **one** symbol removes **one** target and leaves the others
        present, so the gates are independent rather than all-or-nothing.

      **`configs/no_cloud_defconfig` is new**, and exists so Requirement 16.3 is
      reproducible instead of reconstructed by hand. It is deliberately *not*
      "minimal_defconfig plus more": `minimal_defconfig` also turns off
      `HTTP_TRANSPORT_TLS`, which reaches far past the cloud providers, so a
      failure under it would not say whether the cloud gating or the TLS layer
      caused it. Everything in the new file is left at its default except the
      cloud symbols.

      **Verify, as written:** with **every** cloud gate off,
      `key_object_store.hpp` and `object_store_persistence.hpp` still compile
      and `object_store_persistence_unit_test` still runs the conformance suite
      against `mock_object_store` — **213 cases, green**. Every disabled
      provider prints a named configure-time STATUS message
      (`aws_s3_client disabled (CONFIG_AWS_S3_PERSISTENCE=n)` and its three
      siblings). Both strict-mode defconfigs still configure clean:
      `ci_full_defconfig` (which resolves `GCP_STORAGE_PERSISTENCE` to off
      purely through `depends on`, with no explicit entry needed — it is listed
      anyway, matching how `GCP_PRIVATECA` is) and `ci_gcp_defconfig`, whose
      GCS suite builds and passes 45/45 under it.

      **Two items of Requirement 16 are NOT closed here, and cannot be**, since
      they name things that do not exist yet: 16.3's `object_store_backup.hpp`
      (task 12) and 16.4's `cmd/raft_object_backup` provider reporting
      (task 14). Both are unconditional-compilation obligations of the same
      kind this task discharged for the engine, and each task owes its own.
      - _Requirements: 16.1, 16.2, 16.5 (16.3 partly — the backup header is
        task 12; 16.4 is task 14)_

- [x] 12. **Backup catalog** — done August 17, 2026, in
      `include/raft/object_store_backup.hpp`, with 25 cases in
      `tests/object_store_backup_unit_test.cpp` and **13/13 mutants caught**.
      `create` / `list` / `verify` over a store and a prefix.

      **Requirement 10.7 is structural, not conventional**: no engine type is
      named anywhere in the file. It does reuse two free helpers from
      `object_store_persistence.hpp` (`md5_hex`, `iso8601_utc`), which
      instantiates nothing — the alternative was a second hand-rolled MD5 with
      its own separate known-answer test.

      **The manifest is written last, and that is the whole commit protocol.**
      None of the five services offers cross-key atomicity, so "the manifest
      exists" is the only thing "the backup finished" can mean. `list` ignores
      any directory without one, which makes a half-written copy invisible
      rather than restorable.

      **The verify bar was met as written, and the fixture matters as much as
      the assertion.** The smear is produced by mutating the source *while the
      copy is in flight* — a new `on_get` hook on `mock_object_store` — and not
      by seeding a source that was never consistent. Those are different
      failures, and conflating them would leave the check looking tested when
      only the fixture had been. The copy takes its listing, the node truncates
      an entry the listing already promised, and `verify` answers:

          log index 7 is missing between the copied entries at 6 and 8

      The caller even declares `source_quiesced = true`; the claim is recorded
      and **not believed**, which is the entire design. The negative control —
      the identical source copied without interference — verifies clean, and
      without it a `verify` that rejected every backup would look like a working
      detector.

      Three decisions worth not re-deriving:

      - **Contiguity is judged over the entries that actually verified**, not
        over what the manifest claims. The tests disagreed with the first
        implementation and were right: Requirement 10.4 asks whether every index
        is *present*, and an object listed but absent or corrupt in the
        destination is not present — it is a hole a restore would reproduce. So
        a deleted log object reports twice, as the object that broke and as the
        gap it costs. That is deliberate, not noise.
      - **A vanished source object is a gap, never a phantom manifest entry.**
        An object listed by the LIST and gone by the GET is simply not in this
        backup, so the manifest — an inventory of what the backup *holds* —
        must not claim it. Recording it with an empty checksum would turn "this
        backup is missing index 7" into "this backup is corrupt", a different
        and wrong diagnosis. Found by a surviving mutant.
      - **Absent metadata is fine; present-and-unparseable throws**, matching
        the engine's own load path. A fresh node with no term, no vote and no
        snapshot is a legal thing to back up. A corrupt one is not, and no
        manifest is written in that case, so the partial copy stays invisible to
        `list`.

      Objects this file does not interpret — `owner`, retained snapshots,
      genuinely foreign keys — are copied byte for byte and contribute nothing
      to the manifest's reading of the node. A backup that silently dropped keys
      it did not understand would restore an incomplete prefix.

      `backup_id` defaults to `YYYYMMDDTHHMMSSZ`, so lexicographic listing order
      *is* chronological order — the only ordering these stores give. An id
      containing `/` is refused before anything is written.

      **The suite is registered unconditionally**, like the conformance suite
      and for the same reason (Requirement 16.3): it names no provider and
      includes no SDK, so it must pass with every cloud gate off. It does —
      verified under `configs/no_cloud_defconfig`, alongside the engine's 213.
      - _Requirements: 10.1–10.5, 10.7_

- [x] 13. **Restore — both modes, separately named** — done August 17, 2026,
      as `restore_clone` / `restore_seed` on `object_store_backup`, with 17
      further cases (42 in the suite) and **13/13 mutants caught**.

      **A bug this task wrote and the design caught, worth recording because it
      would have surfaced at the worst possible moment.** The first
      `restore_seed` wrote the new cluster's node ids as JSON strings
      unconditionally. The engine reads that array with `as_string()` when
      `NodeId` is `std::string` and `as_int64()` when it is an integer, and
      boost::json **throws** on the wrong one — so seeding a numeric-id cluster
      would have produced a snapshot that parsed cleanly in every unit case and
      exploded the moment an operator started the new cluster from it. The
      representation is now **inferred from the backup's own snapshot**, not
      guessed from the supplied ids: guessing would get `"1", "2", "3"` wrong
      for a string-id cluster, which is an ordinary way to name nodes. Where it
      cannot be inferred (an empty configuration) it refuses and says so rather
      than picking one.

      **Clone restore** reproduces a backup byte for byte. Requirement 11.1's
      epoch bump is done by parsing and re-serialising the owner record, not by
      patching it textually, so a differently-formatted owner object cannot pass
      through unchanged; an owner record with no `epoch` is refused, because
      restoring it unchanged would leave a returning original able to write. A
      backup with **no** owner object restores without one being invented —
      inventing one would make an unfenced prefix look fenced.

      **Seed restore** keeps the state-machine bytes and the snapshot's
      index/term, replaces the configuration, resets the term to
      `last_included_term`, and clears the vote **by omission** rather than by
      writing a `"none"` sentinel — an absent object is how this engine spells
      "never voted", and the sentinel would make a fenced engine's first vote an
      `If-Match` against an object it never wrote. The joint-consensus marker is
      dropped: the old cluster may have been mid-reconfiguration, a new one
      never is, and carrying it across would seed every node with a membership
      change nobody proposed.

      **The merge has no code path, structurally.** `prepare_target` either
      throws or leaves the target with no engine-owned keys, and nothing writes
      until it has returned. `force` deletes `term`, `voted_for`, `snapshot`,
      `owner`, `log/*` and `snapshots/*` — and **only** those. Foreign objects
      are left alone, because the engine neither reads nor writes them either
      and they cannot merge with Raft state. The case that proves it seeds a
      *different* node's entries at 40-41 and checks they are gone rather than
      interleaved with the restored 4-6.

      Two more decisions:

      - **Seed prepares every target before writing any**, so a refusal on the
        third node does not leave the first two seeded into a cluster that can
        never reach a quorum.
      - **Restore aborts on the first problem; `verify` collects them all.**
        `verify` is a diagnostic run by a human who wants the whole picture, and
        restore is a gate.

      **Requirement 11.6's runbooks are in `doc/cloud_object_persistence.md`** —
      one per mode, each with its sequence, its safety checks and a failure
      table keyed on the actual message text. The document states plainly that
      the exact `cmd/raft_object_backup` command lines land with task 14 rather
      than guessing at them now, and that the durability contract and
      per-provider evidence table are task 18's.
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
