# `requirements.md` template

Copy the skeleton below and fill it in. Keep the heading text exactly as
written — `tasks.md`, the validator, and every reviewer's habit all depend on
it.

---

```markdown
# Requirements Document

## Introduction

<Two to five paragraphs. State the problem, not the solution.>

<If a measurement motivates this spec, give it: the number, where it came from,
and what it means — a profile, a failing test, a traceback, a p99 latency, a
count of `# type: ignore` comments. A defect that was measured is arguable; a
defect that was asserted is not.>

<Then state, in one sentence each:>

**Scope**: <what this spec covers>

**Out of scope**: <what it deliberately does not, and where that work lives if
it lives anywhere>

## Glossary

- **<Term>** — <definition, narrow enough that the acceptance criteria can use
  the term without further qualification>
- **`identifier`** — <for code entities, say what it is and which module it
  lives in>

## Requirements

### Requirement 1: <short noun phrase naming the requirement>

**User Story:** As a <role>, I want <capability>, so that <benefit>.

#### Acceptance Criteria

1. WHEN <trigger> THEN <actor> SHALL <observable response>
2. IF <unwanted condition> THEN <actor> SHALL raise <ExceptionType>
3. <actor> SHALL <invariant that holds unconditionally>

### Requirement 2: <...>

**User Story:** As a <role>, I want <capability>, so that <benefit>.

#### Acceptance Criteria

1. ...
```

---

## Section notes

### Introduction

This is the section a reviewer reads to decide whether the spec is worth
reviewing, so it has to be falsifiable. Concretely:

- Say what is wrong today, with evidence — a measurement, a code path, a
  failure that happened, a traceback pasted verbatim.
- Where the evidence is a code path, cite it: `` `src/store/writer.py:118` ``. A
  table of call sites is often clearer than a paragraph about them.
- Where a related spec already decided something, link it and defer to it
  rather than re-deciding.
- If the honest consequence of doing this work is unwelcome — it will be
  slower, it will drop a Python version, it will break a public signature — say
  so here. A spec that hides its costs gets them raised in review anyway, later
  and with less trust.

### Glossary

Define every term the acceptance criteria use in a narrow sense. The test:
could a competent reader who has not worked on this package misread a criterion?
If so, the term belongs here. Words like *task*, *worker*, *session*, *client*,
and *record* almost always need it in an async codebase, where each already
means several things.

Include the code identifiers the spec keeps referring to, so the criteria can
say `` `SessionPool` `` without re-explaining it each time.

### Requirements

**One user story per requirement.** The `so that` clause is not decoration — it
is where the requirement justifies its own existence, and a requirement whose
benefit clause is empty or circular is usually one that should be deleted.

**Group by what fails together.** Criteria that would all be violated by the
same defect belong in one requirement. Criteria that fail independently belong
in different ones, because that is how tasks will cite them.

**Include the negative requirements.** Nearly every worthwhile spec has a "no
silent regression" requirement — the existing test suite still passes, the
public API keeps its signatures, the supported Python versions do not shrink,
the import of an optional backend stays optional. It is easy to leave implicit
and expensive to leave out.

**Cover packaging and typing when the change touches them.** A new module that
is not exported, not annotated, or not included in the wheel is not delivered.
Requirements about `__init__.py` exports, `py.typed`, `mypy --strict`
cleanliness, and `pyproject.toml` dependency constraints belong in the spec
rather than being discovered in review.

**Heading style.** `### Requirement 1: Name` is the default. Match the
surrounding spec if you are extending one.

### Optional sections

- `## Non-Goals` or `## Out of Scope` — when the boundary needs more than the
  one line the Introduction gives it.
- `## Implementation Status` — when a spec is being brought back in line with
  code that already exists.
- `## Appendix <letter> — <title>` — for related work, prior measurements, or
  survey material that would swamp the Introduction.
