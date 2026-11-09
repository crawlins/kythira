# Pinned botocore models

The DynamoDB and DynamoDB Streams service models this module answers to
(`.kiro/specs/dynamodb-compatible-api/` Requirement 2). The set of
operations, shapes, constraints and error shapes is taken from these files
and nothing else.

| File | Source | SHA-256 |
|---|---|---|
| `dynamodb-2012-08-10.json` | botocore `1.43.126`, `botocore/data/dynamodb/2012-08-10/service-2.json` | `1e427b97e61dc839908822f78b9f86bdf4715232b9c3b106a33bc3a0e763fc89` |
| `dynamodbstreams-2012-08-10.json` | botocore `1.43.126`, `botocore/data/dynamodbstreams/2012-08-10/service-2.json` | `b07b6602d39c5c071bcb2323cd8b8f7b66da973045eed973e176420eb0eb5f4d` |

They are Apache-2.0 data files from the botocore project
(<https://github.com/boto/botocore>), copied byte for byte.

## Upgrading

An upgrade is a reviewed change (Requirement 2.4):

1. `tools/fetch_models.sh <botocore version>` downloads both files at that
   tag and prints their SHA-256; update the table above.
2. `tools/regen_models.sh` regenerates
   `include/kythira/dynamodb/model/*_model.hpp`. The generated diff shows
   every new operation, shape and member; keep it in the pull request.
3. A new operation fails `dynamodb_model_coverage_test` until it is
   implemented or listed in `doc/unsupported.md` (Requirement 2.2).
4. Re-run the wire-evidence test; a member the recorded clients send that
   the new model dropped fails it.
