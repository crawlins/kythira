# EARS acceptance criteria

EARS (Easy Approach to Requirements Syntax) constrains a requirement to a small
set of sentence shapes. The constraint is the value: a criterion that fits one
of these shapes has a stated trigger, a stated actor, and a stated response, so
two readers cannot disagree about what it demands.

Every criterion is a **numbered list item** containing the word **`SHALL`**.
Those two properties are what make the criteria addressable (`tasks.md` cites
`1.3`) and enforceable (the validator can find them).

## The five shapes

### Event-driven — `WHEN`

The common case. Use it whenever a trigger causes a response.

```
1. WHEN `write_record` returns successfully THEN the record SHALL survive a
   `SIGKILL` of the process and SHALL be readable after restart
```

### State-driven — `WHILE`

For behaviour that holds throughout a state, not at an instant.

```
2. WHILE the circuit breaker is open `SessionPool.acquire` SHALL raise
   `CircuitOpen` immediately rather than queueing the caller
```

### Unwanted behaviour — `IF ... THEN`

For error paths and conditions the system must not let pass.

```
3. IF the upstream returns 5xx THEN `fetch_record` SHALL raise `UpstreamError`
   carrying the status code, and SHALL NOT return a partially populated
   `Record`
```

### Optional feature — `WHERE`

For behaviour that exists only under an optional dependency, an extra, or a
feature flag.

```
4. WHERE the `redis` extra is installed the package SHALL expose
   `store.backends.RedisStore` satisfying the `Store` protocol, and SHALL
   import cleanly without it
```

### Ubiquitous — no trigger

For invariants that hold unconditionally. Common for API surface and packaging
requirements; the subject leads the sentence.

```
5. `store.writer.FileWriter` SHALL be exported from `store/__init__.py` and
   SHALL be fully annotated, so that `mypy --strict` passes over it
```

## Writing a good criterion

**Name the actor.** "The system" is acceptable, but a named module, class, or
function is better because it says where the behaviour lives: `` `fetch_record`
SHALL raise `` beats "the system SHALL raise".

**State the response as an observable.** If nothing an observer could look at
distinguishes pass from fail, the criterion is untestable. Prefer a return
value, an exception type, a file on disk, a log record, a metric, an ordering,
or an exit status. In Python the exception *type* is usually the observable
worth naming — "SHALL raise `ValidationError`" is testable, "SHALL fail
gracefully" is not.

**Keep one demand per criterion.** Two demands joined by "and" that could fail
independently should be two numbered criteria, because a task can then cite the
one it satisfies.

**Say `SHALL NOT` when the prohibition is the point.** Requirements that forbid
are as important as requirements that require, and they are the ones most often
left implicit — no blocking call inside the event loop, no mutation of a
caller's argument, no bare `except`.

**Do not name the mechanism.** `SHALL survive a SIGKILL` leaves the design free;
`SHALL call os.fsync()` does not. If the mechanism genuinely is the requirement
— a wire format, a named standard, an interoperability constraint, a minimum
Python version — then say so and say why in the Introduction.

**Pin versions the way `pyproject.toml` does.** When a criterion constrains a
dependency or an interpreter, quote the constraint rather than describing it:
`SHALL support Python 3.11 through 3.13` beats "SHALL support recent Python".

## Numbering and citation

Criteria are numbered within their requirement:

```markdown
### Requirement 3: Connection pooling

**User Story:** ...

#### Acceptance Criteria

1. WHEN several coroutines request a connection concurrently THEN
   `SessionPool` SHALL satisfy them from at most `max_size` connections
2. WHEN a caller is waiting for a connection THEN `SessionPool` SHALL NOT hold
   a lock that prevents another caller from releasing one
```

These are cited from `tasks.md` as `3.1` and `3.2`:

```markdown
  - _Requirements: 3.1, 3.2_
```

A range is written `1.1–1.6` (en dash) and the validator expands it. Prefer
listing the individual criteria when there are three or fewer.
