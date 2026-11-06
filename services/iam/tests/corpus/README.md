# IAM policy corpus

Test cases for the IAM policy language, one JSON file per topic, read by
`services/iam/tests/iam_policy_test.cpp`. Every `*.json` file under this
directory is loaded, in sorted path order, so a new file needs no CMake
change. Task 10 of `.kiro/specs/iam-compatible-auth/` started it with the
parser and the matchers; task 11 adds condition operators and policy
variables, and task 12 grows it past 300 cases with `evaluate` cases.

## File format

```json
{
  "source": "https://docs.aws.amazon.com/IAM/latest/UserGuide/...",
  "description": "What the file covers.",
  "cases": [ { "name": "...", "kind": "...", ... } ]
}
```

- `source` (optional at file level) is the AWS documentation page the cases
  come from. A case may give its own `source`, which wins. Every case must
  cite one or the other.
- `name` is unique within the file and is what a failure prints.

## Case kinds

**`parse`**: parse a document as one policy type and check the outcome.

| Field | Meaning |
|---|---|
| `policy_type` | `identity`, `trust`, `resource`, `boundary` or `session` |
| `policy` | the document as a JSON value, or |
| `policy_text` | the document as a string, for input JSON cannot hold as a value: duplicate keys, syntax errors, exact sizes |
| `expect` | `"valid"`, or `{"code": "MalformedPolicyDocument" \| "LimitExceeded", "message": "<substring of the error message>"}` |
| `statements` | optional, with `"valid"`: the number of statements parsed |
| `max_size` | optional: a size quota other than the type's default |

**`action_match`**: `pattern` against `value` as an action
(case-insensitive), expecting the boolean `match`.

**`arn_match`**: `pattern` against `value` as a resource ARN
(case-sensitive, segment by segment), expecting the boolean `match`.

Error messages follow AWS's wording where AWS has a message for the
problem, so `message` is usually a phrase from the AWS error.
