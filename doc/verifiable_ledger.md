# Verifiable ledger

A Kythira ledger is a tamper-evident document store in the style of
Amazon QLDB: every committed transaction becomes a block in a hash-chained
journal, a Merkle accumulator over the block hashes yields a digest, and any
block or document revision can be proved against a digest with the
procedure AWS published for QLDB. The module lives in `services/ledger/`;
its spec is `.kiro/specs/verifiable-ledger/`.

Status: the state machine, transactions, indexes, segments, export,
offline verification, signed digests, the native API with its HTTPS server
and the C++ client are built and tested. The `ledger_node` daemon (task 7c)
and snapshot catch-up (task 9) wait on the persistent-state-machine node;
until then the API runs in tests over an in-process backend.

## Model

- **Ledger.** One Raft group per ledger, whose state machine is a
  persistent state machine over the node's key-value engine. Every
  transaction is single-group, so no cross-shard machinery is involved;
  a cluster scales by ledger count. Ledger groups never split or merge.
  The ledger catalog (names, ids, deletion protection) is its own group.
- **Tables** hold **documents**: Ion values, up to 128 KiB each, with
  system-assigned 22-character base62 ids. Table names are 1-128 letters,
  digits and underscores, starting with a letter or underscore. Ledger
  names are 1-32 letters, digits and hyphens, starting with a letter.
- **Revisions.** Every write makes a new revision of a document (version
  0, 1, ...). A delete leaves a revision with no data. Nothing is ever
  overwritten; history by document or by table is always available.
- **Blocks.** A committed transaction is one block: its statements, the
  revisions they produced, a timestamp that never runs backwards, and the
  previous block's hash. A rejected transaction writes no block.
- **Hashes** (`hashVersion` 1):
  - revision = `dot(ionhash(data), ionhash(metadata))`, metadata alone for
    a delete
  - entriesHash = the Merkle root over statement hashes then revision
    hashes, in block order
  - blockHash = `dot(dot(entriesHash, ionhash({hashVersion, seq, txId,
    blockTimestamp})), previousBlockHash)`
  - digest at tip t = the accumulator over block hashes 0..t
  
  `dot(a, b)` is QLDB's: compare the two hashes as signed bytes from the
  last byte to the first, concatenate smaller first, SHA-256. Only `dot`
  sits above the Ion hashes, so a revision's proof folds all the way to
  the digest.
- **Indexes** are equality indexes on a document path, up to 20 per
  table. On a non-empty table an index backfills in steps and is used
  only once active.
- **Transactions** use snapshot reads with optimistic concurrency. A
  session reads at a snapshot and records what it read (documents, index
  keys, whole tables for scans); at commit the replicas reject it with
  `occ_conflict` if any of that changed since the snapshot. Clients retry
  on `occ_conflict`, `stale_block` and `session_expired`.
- **Segments.** Old blocks move out of the engine into segment files
  (64 MiB by default) that are byte-identical on every replica, and from
  there optionally to object storage. Reads and proofs fetch them back
  and check each one's SHA-256.

## API

HTTPS with mutual TLS. Clients present a certificate issued by the
Kythira CA; its first URI SAN (for example
`spiffe://kythira/ledger/app`) is the identity that roles are granted to.
Each ledger grants `read`, `write` or `admin` (admin implies write implies
read), and a grant on `*` covers every ledger. A request without a usable
client certificate gets 401. `GET /healthz` needs no certificate.

Bodies are JSON, or Ion with `Content-Type: application/ion` (text or
binary). Responses are JSON, or Ion binary with `Accept: application/ion`.
JSON is fine for writing, but read in Ion if you mean to re-hash what you
read: JSON loses Ion's types (decimals, timestamps, blobs), and hashes
cover the Ion value.

| Method and path | Role | Body or query |
|---|---|---|
| `GET /v1/ledgers` | read on any | |
| `POST /v1/ledgers` | admin on `*` | `{name, deletionProtection?}` (protection defaults on) |
| `GET /v1/ledgers/{L}` | read | |
| `PATCH /v1/ledgers/{L}` | admin | `{deletionProtection}` |
| `DELETE /v1/ledgers/{L}` | admin | refused while protected |
| `POST /v1/ledgers/{L}/execute` | write | `{statements: [...]}`, one transaction |
| `POST /v1/ledgers/{L}/sessions` | write | returns `{token, snapshot}` |
| `POST .../sessions/{T}/get` | write | `{table, id}` |
| `POST .../sessions/{T}/scan` | write | `{table, after?, limit?}` |
| `POST .../sessions/{T}/lookup` | write | `{table, index, value, after?, limit?}` |
| `POST .../sessions/{T}/insert` | write | `{table, value}`, returns `{id}` |
| `POST .../sessions/{T}/statements` | write | `{statements: [...]}` |
| `POST .../sessions/{T}/commit` | write | |
| `DELETE .../sessions/{T}` | write | abort |
| `GET /v1/ledgers/{L}/tables` | read | |
| `GET .../tables/{t}/indexes` | read | |
| `GET .../tables/{t}/documents/{id}` | read | |
| `GET .../tables/{t}/documents` | read | `?after=&limit=` |
| `GET /v1/ledgers/{L}/history/{t}[/{id}]` | read | `?start=&end=&after=&limit=` |
| `GET /v1/ledgers/{L}/digest` | read | signed when the node has a signing key |
| `GET /v1/ledgers/{L}/blocks/{seq}` | read | `?tip=`, block with proof |
| `GET /v1/ledgers/{L}/revisions/{t}/{id}/{version}` | read | `?tip=`, revision with proof |
| `POST /v1/ledgers/{L}/exports` | admin | `{start, end, format, bucket, prefix}` |
| `GET /v1/ledgers/{L}/exports[/{job}]` | read | |

Statements are structs with an `op`:

```
{op: create_table, table: "acct"}
{op: drop_table, table: "acct"}            {op: undrop_table, table: "acct"}
{op: create_index, table: "acct", index: "by_owner", path: ["owner"]}
{op: insert, table: "acct", id: "<22 chars>", value: {...}}
{op: replace, table: "acct", id: "...", value: {...}, ifVersion: 3}
{op: patch, table: "acct", id: "...", set: [{path: ["balance"], value: 10}],
            remove: [["note"]], ifVersion: 3}
{op: delete, table: "acct", id: "..."}
```

Paths are lists of field names and list positions. `ifVersion` makes a
write conditional on the current version. Times (`start`, `end`) are Ion
timestamps at any precision.

Errors are `{code, message, retryable}` with these statuses: 400 for a
malformed request, 403 for a missing role, 404 for something not found,
409 for conflicts (`occ_conflict`, `stale_block` and `session_expired` are
retryable), 429 for too many sessions, 503 `not_leader` (with the leader's
URL when known), and 507 `insufficient_storage`.

### C++ client

`kythira/ledger/ledger_client.hpp` wraps the API in Ion binary. Use
`https_transport()` with the CA bundle and your client certificate, list
every member as an endpoint, and run read-modify-write work through
`transact()`, which retries conflicts with jittered backoff. The function
you pass may run more than once.

```cpp
ledger_client client({.endpoints = {"https://ledger-1:8443", "https://ledger-2:8443"}},
                     https_transport({.ca_path = "ca.pem",
                                      .cert_path = "app.pem",
                                      .key_path = "app.key"}));
client.transact("bank", [&](ledger_client::transaction& t) {
    auto acct = t.get("acct", id);
    // ... compute the new value from acct ...
    t.add(statements::replace("acct", id, updated));
});
```

The client follows `not_leader` redirects and moves past members it cannot
connect to. It never resends a request that may already have reached a
server.

## Limits

| Limit | Default |
|---|---|
| Document size | 128 KiB |
| Documents changed per transaction | 40 |
| Statement bytes per transaction | 4 MiB |
| Transaction (session) lifetime | 30 s |
| Open sessions per ledger | 10,000 |
| Indexes per table | 20 |
| Request body | 4 MiB |
| HTTPS connections per node | 512 |
| Segment size | 64 MiB |
| Free-space floor for transactions | 1 GiB |

Below the free-space floor the leader refuses new transactions with
`insufficient_storage`. Sealing, offload and reads keep running, since they
are how space comes back.

## Verifying

Verification needs nothing from the cluster beyond the data being checked
and a digest you saved earlier.

1. **Save digests.** Call `GET /v1/ledgers/{L}/digest` from time to time
   and keep the results: `{tip, digest}`. If the node has a signing key,
   the digest also carries `ledger`, `signedAt`, `signature` and
   `certificateChain`.
2. **Prove a document or block.** Fetch the revision or block with
   `?tip=` set to a saved digest's tip, in Ion, and check it:

   ```
   ledger_ctl prove --proof revision.ion --digest digest.ion [--ca ca.pem]
   ```

   This re-hashes the revision or block from its contents, folds the proof
   with `dot`, and requires the saved digest. The proof is QLDB's shape,
   so AWS's published verification code checks it too.
3. **Verify a whole journal.** Export it (Ion text or Ion binary, not
   JSON) or take a node's segment directory, and run:

   ```
   ledger_ctl verify --export DIR [--digest digest.ion [--ca ca.pem]]
   ledger_ctl verify --segments DIR [--digest digest.ion [--ca ca.pem]]
   ```

   This checks every revision hash, the revision labels, every
   entriesHash and blockHash, the previous-hash chain, and timestamps
   that never run backwards. With `--digest` it also checks the digest at
   that tip. Export objects are checked against the manifest's sizes and
   SHA-256 first. A failure names the first bad block.
4. **Signed digests.** With `--ca`, the digest must be signed by a
   certificate that chains to that CA bundle, and was valid at `signedAt`.
   A signed digest shows the digest came from this cluster, not just that
   the journal agrees with itself.

`ledger_ctl` exits 0 when verified, 1 when verification fails (including a
bad signature or an unreadable journal), and 2 on a usage or I/O error.

## Operations

- **Metrics.** The state machine reports blocks, blocks applied and
  rejections by code (from which commit and conflict rates follow),
  hash-check failures, unsealed, local and offloaded journal bytes,
  segments, and indexes still backfilling. `ledger_node` will export these.
- **Hash mismatch.** If a replica computes a different block hash from
  the leader's for the same entry, the replica stops applying that group
  rather than store a block it disagrees with. The state machine reports
  the block and both hashes so the node can log them and raise an alert.
- **Threads and ion-c.** ion-c 1.1.3 keeps a system symbol table per
  thread and does not free it when the thread exits. Code that parses Ion
  should run on long-lived threads; the HTTPS handler hands requests to a
  fixed worker pool for this reason.
