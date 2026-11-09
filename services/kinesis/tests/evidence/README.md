# Kinesis wire evidence

What real Kinesis clients put on the wire, recorded once against a stub and
checked in (`.kiro/specs/kinesis-compatible-streams/` task 1). The front end
(task 4), the codec work (task 2) and the conformance jobs (task 17) build
against these files, not against what the documentation implies.

## How it is recorded

`recorder.py` is a small Kinesis stub on 127.0.0.1. It knows one stream
with one shard and keeps records in memory. That is enough for each client
to finish one scripted scenario. It records every request: the request
line, the HTTP version, every header and the decoded body. Values that
change on every call (signature, date, invocation id, port, kernel) are
replaced with a placeholder of the same shape. It answers in the protocol
the request used, and gives `UnknownOperationException` for anything it
does not script.

`run_client.sh` starts a recorder, runs one client against it with dummy
keys (the signature is real SigV4, but nothing can reach AWS), and writes
`data/<client>.json`. `record_all.sh` re-records every client. The ctest
`kinesis_wire_evidence` checks the files: every operation is in the pinned
model, every scenario finished, and the protocols are the ones this table
claims.

| Client | Version | Scenario | File |
|---|---|---|---|
| AWS CLI v2 | 2.31.0 | create, describe summary, list shards, put record, put records, iterator, get records, delete | `data/awscli-v2.json` |
| boto3 | 1.43.100 (botocore 1.43.100, the model pin) | the same, with the `stream_exists` waiter (`DescribeStream`) | `data/boto3.json` |
| aws-sdk-java-v2 | 2.57.11, sync client, URL connection HTTP | the same as the CLI | `data/java-sdk-v2.json` |
| KPL | 0.15.12 (native producer) | ten user records into an existing stream over HTTPS | `data/kpl.json` |

## What the gateway must accept

| | AWS CLI v2 | boto3 | Java SDK v2 | KPL |
|---|---|---|---|---|
| Body | JSON 1.1 | JSON 1.1 | **CBOR 1.1** (`application/x-amz-cbor-1.1`) | JSON 1.1 |
| Blobs (`Data`) | base64 string | base64 string | CBOR byte string | base64 string |
| HTTP | 1.1 | 1.1 | 1.1, keep-alive | 1.1 over TLS, **`Expect: 100-continue`** |
| Signed headers | content-type, host, x-amz-date, x-amz-target | same | adds amz-sdk-invocation-id, amz-sdk-request, content-length, x-amz-content-sha256 | adds x-amz-api-version, the SDK headers and x-amz-content-sha256 |
| `x-amz-content-sha256` | absent | absent | present (hex) | present (hex) |
| Other calls | none | none | none | **STS `GetCallerIdentity`** (Query, form-encoded) to build `StreamARN`, then `ListShards` for its shard map |
| Stream naming | `StreamName` | `StreamName` | `StreamName` | `StreamName` **and** `StreamARN` on every `PutRecords` |

Consequences for later tasks:

- **CBOR is not optional.** The Java SDK v2, and so KCL 2.x/3.x and the
  Flink connector built on it, sends CBOR by default. Task 2's CBOR codec
  is on the path of the first Java client, not only of an exotic one.
- **`Expect: 100-continue`** must be answered by the shared Beast host
  (task 4), or KPL stalls for its continue timeout on every batch.
- **STS on the same endpoint.** KPL resolves the stream ARN with STS
  `GetCallerIdentity` before it writes. It takes a separate STS endpoint
  and port (`StsEndpoint`, `StsPort`). The gateway can point those at IAM's
  STS (`services/iam`), or answer `GetCallerIdentity` itself for
  static-key setups. Without either, KPL never sends a record. It also
  reaches out to real `sts.us-east-1.amazonaws.com` when no STS endpoint
  is set, even with a custom Kinesis endpoint, which `doc/kinesis.md`
  (task 18) must warn about.
- **`StreamARN` alongside `StreamName`.** KPL sends both, so the front end
  must accept either and check that they agree.
- **No aggregation was observed.** With aggregation on (the default), KPL
  0.15.12 still sent each user record as its own `PutRecords` entry with
  `ExplicitHashKey` set to the MD5 of the partition key. This held with one
  partition key or ten, and with or without an explicit flush. The KPL
  aggregated-record format (protobuf behind the `f3 89 9a c2` magic) still
  has to be deaggregated on read for KCL, so task 7 takes its vectors from
  KPL's own aggregation tests rather than from this recording.

## Not recorded yet, and why

| Client | When | Why not now |
|---|---|---|
| aws-sdk-cpp | task 7 | Its producer and polling consumer run end to end against the gateway there. The sandbox that recorded these cannot build the SDK (no vcpkg). |
| KCL 2.x and 3.x | tasks 9 and 16 | KCL needs a lease table (DynamoDB or the native lease API) before it calls Kinesis at all, and the stub has neither. |
| Flink connector | task 17 | Same as KCL (it runs KCL's polling or EFO consumer), plus a Flink job harness. |
| `SubscribeToShard` (HTTP/2 event stream) | task 13 | The stub speaks HTTP/1.1 only. |
