# DynamoDB invariants

The rules the AWS DynamoDB documentation states that a compatible service
must hold, each with the page it comes from and the unit test that checks
it. Every case is in `tests/dynamodb_invariants_test.cpp`, named by its id
(`a01_...`), and drives the front end the way an SDK request reaches it,
over the local backend.

The expectations were checked against DynamoDB Local 3.3.2 as well: set
`KYTHIRA_DYNAMODB_ORACLE=host:port` and the same binary sends every request
there instead. Three cases cover behaviour where DynamoDB Local departs
from the documentation (marked *Local differs* below) and return early in
that mode; k06 needs kythira's adjustable collection limit and does too.
Everything else passes against both.

Building this list found four places where kythira did not hold the rule,
all fixed with it:

- An empty attribute name, at the top of an item or inside a map, was
  accepted (a03). It is now refused with `Empty attribute name`, as
  DynamoDB Local answers.
- BatchGetItem added items until the response had passed 16 MB, so it
  could return a little over the limit (h08). It now stops before an item
  that would pass it.
- An item could nest 32 lists or maps inside an attribute (b12). The
  attribute itself is the first of the 32 levels, so 31 is the most, as
  DynamoDB Local answers. Stored items read back with the old allowance.
- The BatchGetItem size test in `dynamodb_query_test` expected the
  over-limit answer; it now expects 42 items, as DynamoDB Local returns.

## Sources

| Key | Page |
|---|---|
| Limits | [Quotas in DynamoDB](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Constraints.html) |
| Types | [Naming rules and data types](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/HowItWorks.NamingRulesDataTypes.html) |
| Reads | [Read consistency](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/HowItWorks.ReadConsistency.html) |
| Items | [Working with items](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/WorkingWithItems.html) |
| Ops | [Comparison operator and function reference](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Expressions.OperatorsAndFunctions.html) |
| Update | [Update expressions](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Expressions.UpdateExpressions.html) |
| Reserved | [Reserved words](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/ReservedWords.html) |
| Query | [Query API reference](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_Query.html) |
| Scan | [Scan](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/Scan.html) |
| BWI | [BatchWriteItem API reference](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_BatchWriteItem.html) |
| BGI | [BatchGetItem API reference](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_BatchGetItem.html) |
| UpdateItem | [update-item CLI reference](https://docs.aws.amazon.com/cli/latest/reference/dynamodb/update-item.html) |
| DeleteTable | [DeleteTable API reference](https://docs.aws.amazon.com/amazondynamodb/latest/APIReference/API_DeleteTable.html) |
| Indexes | [Secondary index guidelines](https://docs.aws.amazon.com/amazondynamodb/latest/developerguide/bp-indexes-general.html) |

## A. Names and keys

| Id | Invariant | Source |
|---|---|---|
| a01 | A table name is 3 to 255 characters of `a-z A-Z 0-9 _ - .` | Types |
| a02 | Table and attribute names are case-sensitive (*Local differs*: it folds table names) | Types |
| a03 | An attribute name is at least one character | Types |
| a04 | A key attribute is a string, number or binary scalar | Types |
| a05 | A partition key value is 1 to 2048 bytes | Limits |
| a06 | A sort key value is 1 to 1024 bytes | Limits |
| a07 | A key attribute cannot be an empty string or binary | Types |
| a08 | Key attributes have the types the key schema declares | Items |
| a09 | GetItem, PutItem, UpdateItem and DeleteItem take the full primary key, and nothing more | Items |

## B. Data types and sizes

| Id | Invariant | Source |
|---|---|---|
| b01 | A number has at most 38 significant digits | Types |
| b02 | A number is between 1E-130 and 9.9999999999999999999999999999999999999E+125 in magnitude, or zero | Types |
| b03 | Leading and trailing zeros are trimmed: equal numbers are the same key | Types |
| b04 | Number sort keys order numerically | Query |
| b05 | String sort keys order by their UTF-8 bytes | Query |
| b06 | Binary sort keys order as unsigned bytes | Query |
| b07 | A set cannot be empty | Types |
| b08 | A set's elements are unique | Types |
| b09 | A set's elements are all of the set's type | Types |
| b10 | Empty strings and binaries are allowed outside keys | Types |
| b11 | Empty lists and maps are allowed | Types |
| b12 | Documents nest at most 32 levels, the attribute being the first | Types |
| b13 | An item is at most 400 KB, names counted with values | Limits |
| b14 | A `NULL` attribute exists for `attribute_exists` | Ops |

## C. Condition and filter expressions

| Id | Invariant | Source |
|---|---|---|
| c01 | An expression is at most 4 KB | Limits |
| c02 | An expression attribute name or value placeholder is at most 255 bytes | Limits |
| c03 | An expression has at most 300 operators | Limits |
| c04 | `IN` takes at most 100 operands | Limits |
| c05 | A reserved word used as a name needs a placeholder | Reserved |
| c06 | `AND` binds tighter than `OR`, and parentheses override both | Ops |
| c07 | `BETWEEN` includes both ends | Ops |
| c08 | `attribute_type` takes every type token | Ops |
| c09 | `contains` works on strings, sets and lists, and its operand cannot be the same path | Ops |
| c10 | `size` counts a string's or binary's bytes and a set's, list's or map's elements | Ops |
| c11 | `begins_with` matches string prefixes | Ops |
| c12 | A comparison between different types is false | Ops |

## D. Update expressions

| Id | Invariant | Source |
|---|---|---|
| d01 | Each of `SET`, `REMOVE`, `ADD` and `DELETE` appears at most once | Update |
| d02 | Every action reads the item as it was before the update | Update |
| d03 | `SET` adds an attribute or replaces it | Update |
| d04 | `if_not_exists` keeps a value that is present | Update |
| d05 | `list_append` appends or prepends by its argument order | Update |
| d06 | Setting a list index past the end appends | Update |
| d07 | Setting a nested attribute needs its parent map | Update |
| d08 | `+` and `-` work on numbers only | Update |
| d09 | `REMOVE` of an absent attribute does nothing, and removing list elements closes the gap | Update |
| d10 | `ADD` on an absent number starts from zero, and on a set takes the union | Update |
| d11 | `DELETE` removes elements from a set and works on sets only | Update |
| d12 | A key attribute cannot be updated | UpdateItem |
| d13 | UpdateItem on an absent item creates it | UpdateItem |
| d14 | `ReturnValues` gives the old, new or updated attributes as documented | UpdateItem |

## E. Writes

| Id | Invariant | Source |
|---|---|---|
| e01 | PutItem replaces the whole item | Items |
| e02 | PutItem's `ALL_OLD` returns the item it replaced, and only that | Items |
| e03 | DeleteItem returns the deleted item when asked and succeeds when the item is absent | Items |
| e04 | A failed condition changes nothing | Items |
| e05 | A condition on an absent item sees no attributes | Items |
| e06 | `ReturnValuesOnConditionCheckFailure` returns the item with the failure | Items |
| e07 | Atomic counters apply every increment | Items |
| e08 | A conditional update can be retried safely | Items |
| e09 | A write that failed is never read | Reads |

## F. Query

| Id | Invariant | Source |
|---|---|---|
| f01 | The key condition tests the partition key for equality, and may add one sort key condition | Query |
| f02 | Sort key conditions are `=`, `<`, `<=`, `>`, `>=`, `BETWEEN` and `begins_with`, and not `<>` | Query |
| f03 | `begins_with` is refused on a number sort key | Query |
| f04 | Results come in sort key order, forward or with `ScanIndexForward` false | Query |
| f05 | `Limit` counts the items read before the filter | Query |
| f06 | A page reads at most 1 MB before filtering (*Local differs*: no 1 MB cap) | Query |
| f07 | The next page starts after `LastEvaluatedKey` | Query |
| f08 | A page can be empty and still not be the last | Query |
| f09 | A filter cannot name a key attribute | Query |
| f10 | Without a filter, `Count` equals `ScannedCount` | Query |
| f11 | `Select` rules: `COUNT` returns no items, and a projection goes with `SPECIFIC_ATTRIBUTES` or no `Select` | Query |
| f12 | No match is an empty result, not an error | Query |
| f13 | A read of a missing table is `ResourceNotFoundException` | Query |
| f14 | `ExclusiveStartKey` takes scalar key attributes only | Query |

## G. Scan

| Id | Invariant | Source |
|---|---|---|
| g01 | Parallel scan segments split the table by partition key, each item in one segment | Scan |
| g02 | Scan returns a partition's items in sort key order | Scan |
| g03 | `Limit`, filters and pages follow the same rules as Query | Scan |

## H. Batch operations

| Id | Invariant | Source |
|---|---|---|
| h01 | BatchWriteItem takes at most 25 requests | BWI |
| h02 | BatchWriteItem refuses the same item twice | BWI |
| h03 | BatchWriteItem refuses the whole batch for a missing table, a bad key or an oversize item | BWI |
| h04 | A BatchWriteItem put replaces the item | BWI |
| h05 | BatchGetItem takes at most 100 keys | BGI |
| h06 | BatchGetItem refuses a repeated key | BGI |
| h07 | BatchGetItem leaves out missing items | BGI |
| h08 | BatchGetItem returns at most 16 MB, and the rest as `UnprocessedKeys` | BGI |

## J. Tables

| Id | Invariant | Source |
|---|---|---|
| j01 | DeleteTable answers `DELETING` and removes every item (*Local differs*: answers `ACTIVE`) | DeleteTable |
| j02 | A table name is unique | DeleteTable |
| j03 | Deletion protection blocks DeleteTable | DeleteTable |

## K. Local secondary indexes

| Id | Invariant | Source |
|---|---|---|
| k01 | An LSI has the table's partition key | Indexes |
| k02 | A table has at most 5 LSIs | Limits |
| k03 | A table's indexes project at most 100 attributes between them | Limits |
| k04 | An LSI needs a table with a sort key | Indexes |
| k05 | An LSI is sparse, and a query on it can fetch unprojected attributes | Indexes |
| k06 | An item collection with LSIs is bounded (10 GB), and a write past it fails | Limits |
| k07 | An LSI key value cannot be empty or of another type | Indexes |

## Checked elsewhere

| Invariant | Source | Test |
|---|---|---|
| A strongly consistent read reflects every write that succeeded before it, from any node | Reads | `dynamodb_raft_backend_test` |
| A successful write survives the loss of a group's leader | Reads | `dynamodb_raft_backend_test` |
| Error types, messages and status codes match DynamoDB Local's for the recorded operations | — | `dynamodb_ops_corpus_test` |

## Deferred

These belong to features not built yet. Each moves into the tables above
with the task that builds it.

| Invariant | Source | Task |
|---|---|---|
| A transaction has at most 100 items and 4 MB, never names one item twice, and is all or nothing | Limits | 10 |
| A global secondary index is read eventually: `ConsistentRead` true on one is a `ValidationException` | Indexes | 11 |
| A table has at most 20 GSIs | Limits | 11 |
| Reads and writes are charged in 4 KB and 1 KB units, and over-capacity requests are throttled | Limits | 12 |
| Time to live can be changed at most once an hour | Limits | 13 |
| PartiQL statements follow the same item rules | — | 14 |
| At most 2 readers share a stream shard | Limits | 16 |
| Global tables replicate with last writer wins | — | 22 |
| Resource policies are at most 20 KB | Limits | 21 |
| Encryption at rest and DAX | — | out of scope |
