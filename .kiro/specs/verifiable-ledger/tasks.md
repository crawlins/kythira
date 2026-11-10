# Implementation Plan

Depends on `.kiro/specs/persistent-state-machine/` tasks 1-3 (concepts,
`memory_kv_engine`, node integration) for task 3 on, its task 5 (RocksDB)
for the RocksDB CI leg, and `.kiro/specs/file-install-snapshot/` for
follower catch-up (task 9). Tasks 1-2 need neither and can start now. Each
task is one PR unless noted.

- [x] 1. Module skeleton
  - `KYTHIRA_LEDGER` Kconfig option (default off), `services/ledger/` and
    `cmd/ledger_node/` CMake targets, the no-`*_impl.hpp` include check
  - Off-by-default build proved unchanged
  - _Requirements: 1.1-1.4_
  - Done 2026-11-07. As built:
    - The symbol is `CONFIG_LEDGER` (`services/ledger/Kconfig`, in the
      "Service Modules" menu), `depends on ION_SERIALIZER && OPENSSL`; the
      in-memory engine is always built, so 1.1's engine condition holds.
      The top level sets `KYTHIRA_BUILD_LEDGER` and enters
      `services/ledger/` after `tests/`, like `services/iam/`.
    - The daemon lives at `services/ledger/cmd/ledger_node/`, per
      `services/README.md`, as a `--self-test` placeholder until task 7.
    - The include check is `scripts/check_module_boundaries.py`'s
      `impl-header` rule, which already runs over every `services/*/` on
      every pull request; no ledger-specific CMake regex was added.
    - Only `configs/ci_ion_defconfig` turns the module on (it needs
      ion-c); every other defconfig, and so every other CI build, is
      unchanged.

- [x] 2. Ion hash, `dot` and the Merkle accumulator
  - SHA-256 Ion hash over ion-c; amazon-ion-hash test vectors
  - `dot`, proof folding, accumulator with an abstract node store; test
    against the AWS QLDB verification tutorial example
  - _Requirements: 4.6, 12.1, 12.2_
  - Done 2026-11-07. As built:
    - `ion_hash.hpp` is header-only and parameterised over the hash
      function, so the suite's identity vectors check the exact bytes;
      `ion_hash()` is the SHA-256 form. All 166 identity and 5 MD5 cases of
      amazon-ion/ion-hash-test pass (`tests/vectors/`, copied unchanged).
    - ion-c quirks it works around: timestamps are local fields plus an
      offset, so they are converted to UTC; annotations are read before
      stepping into a container (stepping in drops them); strings and lobs
      are read in pieces, because a refused `get_string_length` on a long
      text value consumes part of it; and the walk has its own depth limit,
      since the text reader ignores `max_container_depth`.
    - The binary-Ion length preflight and the reader owner moved from
      `raft/ion_serializer.hpp` to core `raft/ion_preflight.hpp`, so the
      ledger reads Ion without the serializer or a future backend.
    - The AWS tutorial publishes the verification code but no hash values,
      so `tests/tools/qldb_dot_vectors.py` runs the tutorial's Python
      functions as published and prints the vectors the C++ test checks.
    - Level 0 of the node store holds the leaves (block hashes) as well as
      the interior nodes. The tree is RFC 6962's shape with `dot`; its
      roots for 1-9 leaves are pinned in the test.

- [x] 3. Ledger state machine: tables, documents, journal
  - `ledger_state_machine` on the persistent concept: catalog keys, tables,
    insert/replace/patch/delete, revisions, block building and hashing,
    leader hash in the entry and replica check, split/merge veto
  - _Requirements: 2.1, 2.2, 3.1-3.4, 5.1-5.5_
  - Done 2026-11-09, on persistent-state-machine task 1 (the concepts and
    `memory_kv_engine`); it does not need that spec's tasks 2-3. As built:
    - `basic_ledger_state_machine<Engine>` satisfies
      `persistent_splittable_state_machine`. Its whole state, the applied
      index included, is under the ledger's range `l/<id>/` (key layout in
      the header comment), so checkpoint, ingest and drop move the ledger
      as one range.
    - The entry and statement formats are in `journal_types.hpp`. The
      leader's `prepare()` fills in seq, previousBlockHash, the clamped
      timestamp and blockHash. A replica tells two failures apart: an entry
      whose seq, previous hash or timestamp no longer follow the last block
      is `stale_block` (rejected, nothing written but the applied index;
      the leader prepared it against a tip another entry has since moved),
      while a different blockHash for the same inputs throws
      `ledger_divergence_error`, a `persistent_storage_error`, before
      anything is written. Task 5's session layer must therefore keep one
      transaction per ledger in flight, or re-prepare on `stale_block`.
    - Every rejection (malformed entry, OCC-style statement failures:
      `table_exists`, `table_not_found`, `table_inactive`, `table_active`,
      `document_exists`, `document_not_found`, `version_mismatch`,
      `invalid_path`, `document_too_large`) writes no block; a transaction
      is all or nothing. Each statement that writes a document makes one
      revision, so a document written twice in a transaction gets two
      versions.
    - Documents go through `ion_value.hpp`, a small Ion tree: containers
      are parsed, scalars keep their own binary, so arbitrary-precision
      numbers and timestamp precision survive a patch unchanged. It writes
      through ion-c's stream writer, because copying from a reader cannot
      be rerun into a bigger fixed buffer.
    - Block seq starts at 0 and the first block's previousBlockHash is 32
      zero bytes. Patch path steps are field names or list indexes; setting
      index n of an n-element list appends.
    - Not here: the Merkle nodes in the block's batch (task 4), the
      split/merge policy-hook veto and its metric (task 7, with the host;
      `split_at` and `adopt` throw `ledger_split_vetoed` as the backstop),
      and the ledger-catalog group (Requirement 2.3-2.4, task 7).

- [x] 4. Digests and proofs
  - Engine-backed Merkle store, frontier rebuild on open, `GetDigest`,
    `GetBlock`, `GetRevision` with proofs at any tip
  - _Requirements: 4.1-4.4_
  - Done 2026-11-09. As built:
    - The block hash changed to `dot(dot(entriesHash,
      ionhash({hashVersion, seq, txId, blockTimestamp})), previousBlockHash)`
      (design "Hashing"). With the first formula, an Ion hash over a header
      holding entriesHash, a revision proof could not fold through its
      block, so `GetRevision` could not meet Requirements 4.3 and 4.6. No
      ledger existed yet, so `hashVersion` stays 1.
    - `engine_merkle_store` keeps nodes at `merkle/<level>/<index>` in the
      data column family and buffers writes; apply appends the block hash
      to the accumulator and moves the new nodes into the block's batch. If
      the engine write fails, the state machine reloads (frontier
      included) before rethrowing.
    - `get_digest()` reads only the in-memory frontier; `digest_at`,
      `get_block_proof` and `get_revision_proof` read O(log n) stored nodes
      plus, for a revision, its one block. A tip below the block or beyond
      the last block throws `ledger_query_error`; a missing node throws
      `merkle_store_error` rather than returning a short proof.
    - The linearizable read path for `GetDigest` (Requirement 4.2) is the
      node's, in task 7.

- [x] 5. Transactions and OCC
  - Leader session manager, reads at S, read sets, change markers,
    validation in apply, limits, leader-change failure, retryable errors
  - Serializability property test
  - _Requirements: 7.1-7.7, 12.3_
  - Done 2026-11-09. As built:
    - S is the number of blocks applied at begin; reads see the newest
      revision in a block below S (`get_document_at`, `scan_table_at`,
      reverse seek over `rev/`). The entry carries an optional `readSet`
      {blocks, documents [{table, id, version}], indexes [{table, index,
      valueHash}], tables}.
    - Apply (and prepare, so the leader fails early) rejects with
      `occ_conflict` when a read document's `current` pointer, an index
      key's `ichg` marker or a scanned table's `tchg` marker names a block
      at or after S. One rule covers changed, deleted and newly created
      documents, so a read that found nothing conflicts with a later
      insert of that id. `ichg` markers are written by task 6.
    - Limits: apply rejects `transaction_too_large` over 40 document
      statements or 4 MiB of statement Ion (`ledger_options`); the
      session manager refuses the statement that would cross them, and
      expires sessions after 30 s.
    - `ledger_session_manager` keeps sessions in leader memory with tokens
      `<term>:<id>`; `leadership_changed` and a newer term fail them with
      retryable `session_expired`. `retryable_rejection` names the codes a
      client retries (`occ_conflict`, `stale_block`, `session_expired`);
      the client's retry loop is task 7. Reads do not see the session's own
      buffered writes.
    - The property test runs five seeds of 600 interleaved steps: up to
      four sessions read, scan and write values computed from their reads,
      with blind writes mixed in. Every commit must have read exactly the
      state it serializes after (commit order), and every `occ_conflict`
      must have had a read that no longer held.

- [x] 6. History and equality indexes
  - `history()` by document and time range; index definition, backfill,
    activation entry; bounded scans
  - _Requirements: 6.1-6.3_
  - As built:
    - `history(table, id?, start?, end?, after?, limit)` returns revisions
      in commit order, deletes included, with inclusive time bounds and a
      cursor. By document it walks `rev/`; by table it walks
      `hist/<t>/<seq>/<id>/<v>` keys written with each block, and turns
      the time bounds into block bounds by binary search over block
      timestamps (they never decrease).
    - `create_index` is a statement (`{op: create_index, table, index,
      path}`), checked for `index_exists` and `index_limit` (20 per
      table). On a table with no live document the index is active at
      once. Otherwise it builds: `ledger_index_build` entries each index
      up to `limit` documents past a stored cursor, and the step that
      reaches the end activates the index, so the "activation entry" is
      the last build step. Build steps write no block (the index is
      derived state, rebuilt identically on every replica). Proposing
      them is the node's job (task 7).
    - Every write maintains every index on its table, building ones
      included, and sets the `ichg/<t>/<i>/<valueHash>` marker of each
      key a document moves into or out of, which is what session lookups
      are validated against.
    - `find_by_index` is refused (`ledger_query_error`) until the index
      is active. It reads the snapshot: the index holds the current
      state, so when the key changed since the snapshot, or the index is
      newer than it, the lookup falls back to a filtered snapshot scan.
      A session lookup through an index newer than its snapshot is
      recorded as a table read, since the index has no markers for what
      happened before it existed.
    - Scans take `scan_options`: a cursor, a document limit, a byte
      budget (1 MiB default; a page always holds at least one document)
      and an equality filter on a path.

- [ ] 7. Native HTTP API, client and `ledger_node`
  - HTTP/JSON and Ion API with mTLS roles and request limits; C++ client
    with OCC retry; the daemon with config and data-directory layout; the
    ledger catalog group; the split/merge policy veto for ledger ranges and
    its metric
  - _Requirements: 2.2, 2.3, 2.4, 8.1-8.4_
  - Done so far (7a, transport-free):
    - `ledger_catalog.hpp`: the catalog group's persistent state machine
      (create, update, delete, purge; deletion protection; names checked
      against the QLDB rules). It refuses splits and merges.
    - `ledger_api.hpp`: request routing for ledgers, tables, indexes,
      documents, history, sessions, digests, proofs and exports, over an
      `api_request`/`api_response` pair so any HTTP server can host it.
      Roles are read/write/admin per ledger with a `*` wildcard. Commits
      hold a per-ledger lock across prepare, propose and apply and retry
      `stale_block` up to three times. Errors are `{code, message,
      retryable}`; not-leader is 503 with the leader's address.
    - `ledger_sessions.hpp`: `pending_transaction`, `finish` and
      `execute_pending` split prepare from apply so a node can propose
      between them.
  - Done so far (7b, HTTPS and client):
    - `ledger_http.hpp` serves the API on `raft/beast_https_server.hpp`
      (whose connection gate, size limits and timeouts are the
      `http-server-request-limits` ones). The principal is the verified
      client certificate's first URI SAN; no certificate is a 401.
    - ion-c keeps a system symbol table per thread and never frees it
      when the thread exits (about 128 KiB). The server is one thread per
      connection, so the handler runs API work on `ion_worker_pool`, a
      fixed set of long-lived threads, and does no Ion work itself.
    - `ledger_client.hpp`: Ion bodies, an mTLS Beast transport with host
      name checks, leader redirects and failover for requests not yet
      sent, and `transact()` with OCC retry and jittered backoff.
  - Done so far (7c, the node's core over multi_raft):
    - `ledger_group_state_machine.hpp`: multi_raft builds every group's
      state machine from one type, so the group state machine opens as
      the catalog (`c/catalog/`) or as a ledger (`l/<id>/`) by range and
      records what it opened in a shared context, where the node finds it
      without the Raft node's lock. Split policy hooks report no size and
      veto every key; a split or merge that reaches apply throws. Both are
      counted for the metric.
    - The catalog places ledgers: a create entry may carry the voters the
      leader chose, and apply assigns the group id from a counter in the
      catalog (catalog group 1, ledgers from 2), so ids are unique with no
      allocator and every replica reads the same placement.
    - `ledger_host.hpp` is ledger_api's Backend over multi_raft: catalog
      reads from the local replica (only the catalog leader answers
      ledger_not_found, since a follower may lag a create), writes and
      ledger routes on the group leader, not_leader with the leader's URL
      otherwise. Each node creates the groups it votes in from its catalog
      replica and destroys purged ones. A new leader passes a read-index
      barrier once per term before preparing, so prepare sees every
      committed entry. A commit timeout or lost leadership answers 503
      `commit_unconfirmed`, not retryable, since the entry may still
      commit. A maintenance thread proposes index build steps, seals and
      export finishes on the ledgers the node leads.
  - Done so far (7c, the daemon):
    - `cmd/ledger_node/`: configured from the environment (`config.cpp`,
      `ledger_node --help` lists every variable); Raft over Beast with
      mutual TLS by default (plaintext only with http:// peers on
      loopback or `KYTHIRA_ALLOW_PLAINTEXT_RAFT`); one RocksDB database
      for every group's state, a binary file Raft log per group; the API
      on the HTTPS server with client certificates and a roles file;
      optional digest signing. A replica's hash mismatch is logged at
      critical with both hashes.
    - `ledger_node_cluster_test` runs three daemons on loopback: writes
      through the client, `ledger_ctl verify --api`, writes with each of
      two members killed in turn, and a whole-cluster restart.
    - The Boost future backend now pins a Beast connection's executor for
      as long as a chain via()'d onto it can submit to it (the Folly
      backend already did, through `KeepAlive`); killing a member crashed
      the survivors with a use-after-free in the Raft client.
  - Still owed: metrics export (a `/metrics` endpoint) and the
    object-store export runner; exports finish as failed until then.

- [x] 8. Segments, offload and export
  - Seal entries, deterministic segment files, object-store offload and
    fetch with SHA-256, disk floor, `ExportJournal` jobs
  - _Requirements: 3.5, 3.6, 10.2-10.4_
  - As built (the state machine side and the leader-side helpers;
    proposing seals, offloads and exports on a timer is the node's job in
    task 7):
    - A seal entry `{type: ledger_seal, first, last}` must start at the
      first unsealed block and end at or before the tip, else
      `invalid_seal`. Every replica writes the blocks to
      `seg-<first>-<last>.kyl` in `ledger_options::segment_directory`
      ("KYLSEG01", then a 4-byte big-endian length and the block's Ion
      binary per block), fsyncs it (temporary, fsync, rename, fsync of
      the directory), and in one batch records `meta/segment/<first>`
      `{first, last, file, size, sha256}` and erases the range from
      `journal/`. A seal on a replica with no segment directory throws
      (configuration error). `seal_candidate()` proposes the oldest
      unsealed blocks once they reach `segment_bytes` (64 MiB).
    - Segment records live in `meta/segment/`, not a separate key space
      as the design table had it, so they sit with the other catalog
      records.
    - Reads (`get_block`, proofs, history by table) fall back from
      `journal/` to the segment holding the block, through a two-entry
      cache. A segment is checked against its size and SHA-256 on every
      load: the local copy first, then the offloaded object; neither
      good is `ledger_segment_error`. Refetching from a peer's local copy
      is left to the node.
    - Offload: `upload_segment` (leader) checks the bytes against the
      recorded SHA-256 before sending them to any `key_object_store`;
      the offload entry `{type: ledger_offload, first, objectKey}`
      records the key, and replicas then delete their local copy (unless
      `keep_offloaded_segments`), also on open if a crash left it behind.
      `segment_fetcher` reads it back.
    - Disk floor: `disk_guard` on the session manager refuses `begin`
      and `commit` with non-retryable `insufficient_storage` below a free
      space floor (1 GiB default). Seals, offloads and reads are not
      affected, since they free space. The check is leader-side, never in
      apply, because free space differs between replicas.
    - `ExportJournal`: a start entry records the job `in_progress` with
      the tip at that moment, so the export covers exactly the blocks up
      to that tip in [start, end]; `export_journal()` (leader) writes
      `<prefix><jobId>/<first>-<last>.<ion|10n|json>` objects of up to
      1000 blocks and `manifest.json` listing each with its range, size
      and SHA-256; a finish entry records completed or failed. A rerun
      after a leader change rewrites the same objects. JSON is one block
      per line, with hashes as base64 strings: ion-c 1.1.3's JSON
      down-conversion still writes blobs as `{{...}}`, so `ion_value`
      writes containers and lobs itself.

- [x] 9. Snapshots and catch-up
  - Checkpoint plus segment list as the snapshot, file-install-snapshot
    catch-up test for a lagging follower
  - _Requirements: 10.1_
  - Done so far: `basic_ledger_state_machine::checkpoint()` adds every
    sealed segment that is not offloaded to the engine's checkpoint as
    `ledger-<file>`, after checking its SHA-256 (a damaged local copy
    fails the checkpoint rather than send a journal with a hole).
    `ingest()` splits those files from the engine's, checks each against
    the manifest, writes them into the segment directory before the
    engine ingest (a segment's bytes depend only on its blocks, so an
    early write never disagrees with any state), then ingests the range
    and removes `seg-*.kyl` files the new state does not list. Tests in
    `ledger_segments_test` stream the checkpoint through
    `snapshot_stream_reader`/`snapshot_stream_writer` as Raft does.
  - `ledger_host_test`'s
    `a_lagging_replica_catches_up_from_a_snapshot_with_its_segments`: a
    replica is cut off while its ledger's log compacts past it (16 KiB
    snapshot threshold, 2 KiB segments), then comes back through a
    format-1 snapshot with the leader's block count, digest and segment
    files.
  - multi_raft now gives each group its own subdirectory of the
    configured staging directory (`group-<id>`). With one shared
    directory, a group created at run time (every new ledger) deleted the
    staging transfers and leader checkpoints of every group already
    running, since a node clears its staging directory when it starts.

- [ ] 10. `ledger_ctl`, digest signing and observability
  - `verify` and `prove` (API, data directory, export); optional CA-signed
    digests; metrics and the hash-mismatch alert path; tamper tests
  - _Requirements: 4.5, 9.1-9.3, 11.1, 11.2, 12.3_
  - Done so far (offline verification, `verify` through the API, digest
    signing, and the state machine side of metrics and the hash-mismatch
    alert; exporting them and `verify` from a live data directory need
    `ledger_node`, task 7):
    - `journal_verifier` checks blocks in order: sequence, previous hash
      chain, timestamps never running backwards, every revision hash,
      entriesHash, blockHash, and the digest at a saved digest's tip. It
      keeps only the accumulator frontier (a discarding node store), so
      memory does not grow with the journal. The first failure names the
      block.
    - The byte-by-byte tamper sweep over a segment file found that a
      revision's `tableName` and `blockAddress` are outside its hash
      (QLDB's revision hash covers data and metadata only) and so were
      bound by nothing. `block::verify()` and the verifier now require
      each revision to name its own block and a table and id written by
      one of the block's statements, which the entries hash covers.
      After that every single-byte change is caught.
    - Sources: any state machine (`verify_state_machine`), a segment
      directory, and an export directory (each object checked against
      the manifest's size and SHA-256 first; JSON exports are refused
      because they have lost the Ion types).
    - `proof_document` is the Ion form of GetBlock / GetRevision results;
      `check_proof` re-hashes the block or revision and folds the proof
      to a saved digest at the same tip.
    - `ledger_ctl verify` and `prove`; exit 0 verified, 1 failed, 2 usage
      or I/O. `ledger_verifier_test` runs the built tool.
    - `ledger_ctl verify --api URL --ledger L` (with the client's TLS CA,
      certificate and key) reads GetDigest, fetches blocks 0..tip with
      GetBlock at that tip, and re-hashes them; the journal must reach
      the node's digest and a saved `--digest` at its tip. `--ca` checks
      the saved digest's signature, else the node's. Unreachable or
      refused is exit 2. `ledger_http_test` runs it against an HTTPS
      member, including a revision altered in the node's engine (fails
      naming the block) and a wrong saved digest.
    - Signed digests (4.5, `journal_signing.hpp`): `digest_signer` signs
      the Ion hash of {ledger, tip, digest, signedAt} with a CA-issued
      key (ECDSA or RSA over SHA-256, Ed25519 directly) and attaches the
      certificate chain; the result still reads as a saved digest.
      `check_signed_digest` verifies the chain against a CA bundle at
      signedAt, then the signature, and returns the signer's URI SANs.
      `ledger_api::set_digest_signer` makes GetDigest return signed
      digests (off by default, which answers design open question 2 for
      now: the node signs when it is given a key). `ledger_ctl --ca`
      requires and checks the signature.
    - Metrics (11.1) and the alert (11.2), state machine side:
      `stats()` returns blocks, blocks applied and rejections by code
      since open (the node derives commit and OCC conflict rates),
      hash-check failures, unsealed, local and offloaded journal bytes,
      segment count and indexes still backfilling.
      `set_hash_mismatch_handler` is called with the block and both
      hashes before apply throws `ledger_divergence_error`, so the node
      can log it with its replica id and raise the alert metric. Digest
      latency is measured by the node.

- [ ] 11. Crash, chaos and docs
  - Failpoint kills during apply, sealing and offload; identical journals
    after recovery
  - docker-chaos ledger scenario under Docker and rootless Podman, journal
    verified after
  - `doc/` page: model, API, limits, verification procedure
  - _Requirements: 12.4, 12.5_
  - Done so far: `doc/verifiable_ledger.md` (model, API, limits,
    verification procedure, operations). It says what waits on
    `ledger_node` and needs a pass once the daemon and its config land.
    The crash and chaos tests wait on `ledger_node` and
    `rocksdb_kv_engine`.
