# `design.md` template

Copy the skeleton and fill it in. Use the heading text verbatim. Omit a section
only when it genuinely does not apply — an empty `## Error Handling` is worse
than none, but a missing one on a spec that has error paths is worse still.

---

````markdown
# Design Document

## Overview

<What is being built and the one or two decisions that shape everything else.
A reader who stops here should be able to describe the approach in a sentence.>

## Architecture

<Where the change sits: which packages and modules, and how a call flows
through them. A module map or an ASCII data-flow sketch in a fenced block
carries this better than prose.>

## Components and Interfaces

### 1. `src/store/writer.py`

<Purpose in one or two sentences.>

#### Configuration

```python
@dataclass(frozen=True, slots=True)
class WriterConfig:
    directory: Path
    segment_bytes: int = 64 * 1024 * 1024   # rolled at this size
    fsync_on_commit: bool = True            # False only in tests
```

#### Protocol

```python
class Writer(Protocol):
    async def append(self, record: Record) -> int:
        """Append `record`; return its sequence number."""

    async def commit(self, through: int) -> None:
        """Make every record up to `through` durable."""
```

#### `commit()` sequence

1. <step>
2. <step>

## Data Models

<Record shapes, on-disk layouts, wire formats, database rows. Show the
`@dataclass`, `TypedDict`, or pydantic model, and give the field semantics and
units in a table where the names do not carry them.>

## Correctness Properties

### Property 1: <the property, stated as a claim>
**Validates: Requirements 1.1, 1.5**

<Why the property holds. An argument, not a restatement.>

### Property 2: <...>
**Validates: Requirements 2.3**

<...>

## Error Handling

| Failure | Raised as | Caller sees | Expected response |
|---|---|---|---|
| <failure> | `<ExceptionType>` | <what propagates> | <retry, abort, degrade> |

<Prose bullets are fine too; the point is that each failure is traced from
cause to caller.>

## Testing Strategy

- **`tests/test_writer.py::test_<name>`**: <what it does, and what it proves>.
  <Where it proves less than it appears to, say so.>

## Dependencies

| Package | Constraint | Used for |
|---|---|---|
| `httpx` | `>= 0.27, < 0.29` | async HTTP client |

<New runtime dependencies go in `pyproject.toml`'s `dependencies`; optional
backends go behind an extra. Say which, and why.>

## Non-Goals

- <thing this design deliberately does not do, and why>
````

---

## Section notes

### Overview

Lead with the decision, not the background — the background is in
`requirements.md`. If the design turns on one insight, that insight is the first
paragraph.

### Components and Interfaces

Number the components and head each with its module path, so the design doubles
as a map of the change. Prefer showing the actual signatures: a `Protocol` or a
`@dataclass` settles more questions per line than a paragraph describing it, and
it is the thing reviewers will actually check against the requirements.

Habits worth keeping in Python:

- **Annotate everything in the sketch**, including the return type. An unhinted
  sketch hides exactly the decisions the design is for.
- **Show `async def` where the real signature is async**, and say which calls
  block. "Is this awaited?" is the most common question a Python design leaves
  unanswered.
- **Prefer `Protocol` to a base class** when the design only needs a shape, and
  say so — it is a real decision about how implementations are substituted, and
  it determines whether tests can pass a stub.
- Where a public function gains a parameter, show the signature as it will be
  after the change, not a diff, and say whether the parameter is keyword-only
  and what its default is. Defaults are behaviour.

### Data Models

Give units and ranges the names do not carry — `timeout: float` is seconds or
milliseconds depending on who reads it. Say whether a model is frozen, whether
it is validated at construction, and what happens to unknown fields on the way
in.

### Correctness Properties

This is the section that makes a design reviewable, and the one most often
written thinly. Each property needs:

1. A heading stating the property as a claim that could be false.
2. A `**Validates: Requirements N.M, N.M**` line. This is the second half of
   the traceability chain — requirements are cited by design properties and by
   tasks, from opposite ends.
3. An argument. "Two coroutines cannot both observe the pool as non-empty
   because `acquire` decrements under the same `asyncio.Lock` that `release`
   increments under" is an argument. "Acquisition is thread-safe" is a
   restatement.

Where a property depends on an assumption, name the assumption — a single event
loop, a GIL-protected increment, a filesystem that honours `fsync`, a caller
that does not retain the buffer. A property resting on an unstated assumption is
the failure mode this section exists to prevent.

### Error Handling

Trace each failure end to end: what fails, what the code does, which exception
type the caller sees, and what the caller is expected to do about it. Two things
are worth stating explicitly because they are so often left implicit:

- **Which exceptions are part of the public API.** A caller cannot handle
  `KeyError` from three layers down; say what it is wrapped in.
- **What happens on cancellation and at shutdown.** `asyncio.CancelledError`,
  `__aexit__`, and `finally` blocks are where swallowed errors accumulate, and
  a design that ignores them will be corrected by production rather than review.

### Testing Strategy

Name the tests and say what each proves. Three habits are worth keeping:

- **Name the file and test function**, so the plan in `tasks.md` can point at
  the same thing: `tests/test_writer.py::test_commit_survives_sigkill`.
- **State the limits.** If a test proves the data reached the OS but not that it
  reached the platter, say both halves. A test whose limits are stated is worth
  more than one whose limits are discovered later.
- **Say which tests do not exist yet and why.** Tests needing a container, a
  cloud account, or a real clock belong here marked as future work, with the
  `pytest` marker or CI job that will run them.

Where the project uses `hypothesis`, say which properties are worth generating
inputs for; where it uses fixtures, say which ones the new tests need and
whether any must be `session`-scoped.

### Optional sections

`## Performance Considerations`, `## Security Considerations`,
`## Packaging`, `## Migration`, `## Implementation Notes`, `## Trade-offs`, and
`## Future Enhancements` are all reasonable additions. Add them when they carry
weight. A free-form heading is acceptable where a standard one would obscure the
point — `## Why the writer owns the lock, and not the pool` says more than
`## Architecture` would.
