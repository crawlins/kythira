---
name: kiro-spec
description: >-
  Write, extend, or repair a Kiro-style specification for a Python project in
  `.kiro/specs/<feature-name>/` — the three-document set of `requirements.md`
  (EARS acceptance criteria), `design.md`, and `tasks.md` (a numbered, traceable
  implementation plan). Use this skill whenever the user asks for a spec, a
  requirements document, a design document, an implementation plan, acceptance
  criteria, EARS requirements, or a task breakdown; whenever they mention
  `.kiro`, "kiro spec", or name a directory under `.kiro/specs/`; whenever they
  ask to plan a feature, a module, a package, or an API change before writing
  code, to add requirements or tasks to an existing spec, or to bring a spec back
  in line with what the code actually does. Use it even when the words "kiro" or
  "spec" never appear — "write up what we need to build before we start" is this
  skill. Do NOT use it for ordinary code changes that no spec covers, for commit
  messages, for docstrings, or for prose docs under `docs/`.
allowed-tools: Read, Write, Edit, Glob, Grep, Bash
metadata:
  target-model: gemma-4-31b-it-128k-202604
  target-language: python
---

# Kiro specifications for Python projects

A Kiro spec is three Markdown files in one directory:

```
.kiro/specs/<feature-name>/
├── requirements.md   what must be true when this is done
├── design.md         how it will be built
└── tasks.md          the ordered plan, each task tied back to requirements
```

They are written in that order, and each one is approved before the next is
started. The order is the whole point: a design written before the requirements
are settled ends up defending itself instead of serving them, and a task list
written before the design is settled turns into a to-do list of guesses.

## Model notes — read this first

This skill is written for **`gemma-4-31b-it-128k-202604`**. Three consequences
shape everything below, and knowing why they are here will help you apply them
where the letter of the instruction runs out:

1. **The context window is 128k, and specs are long.** One spec's three
   documents commonly total 10,000–15,000 tokens, and a mature project's spec
   tree runs to dozens of them. Reading five specs "for background" spends a
   third of your window before you write a word. The reading budget in step 1 is
   not frugality for its own sake — it is what leaves you room to hold the
   Python source you are actually specifying.
2. **Templates beat invention.** The exact section order and heading text are
   in `references/`. Copy them. A spec that invents its own headings costs a
   reviewer time on every section and buys nothing, and it draws warnings from
   the validator for structure it cannot recognise.
3. **Check mechanically, not by eye.** `scripts/check_spec.py` verifies the
   things that are tedious and easy to miss: missing sections, criteria without
   `SHALL`, tasks that cite requirements that do not exist, requirements no task
   covers. Run it instead of re-reading your own work looking for slips.

## Step 1 — Read, on a budget

Do exactly this. Do not read more.

1. `ls .kiro/specs/` — see what exists. If a directory for this feature already
   exists, you are **editing**, not creating; read its three files in full.
2. Read **one** nearby exemplar spec, chosen for topic adjacency. If the tree is
   empty, the templates in `references/` are your model instead.
3. Read `pyproject.toml` (or `setup.cfg` / `tox.ini` / `noxfile.py`). It tells
   you the package layout, the supported Python versions, the test runner and
   its markers, and the lint and type-check configuration — all of which the
   spec will need to name correctly, and all of which are cheap to read.
4. Read the modules the feature actually touches. This is where your remaining
   budget should go. A spec whose claims about the code are wrong is worse than
   no spec, because it will be believed.

Do not read `.kiro/steering/` unless the spec concerns project-wide coding
style, test conventions, or example programs — those are coding standards, not
spec conventions.

## Step 2 — requirements.md

Read `references/requirements-template.md` and follow it. Read
`references/ears-patterns.md` before writing the first acceptance criterion.

The shape:

```markdown
# Requirements Document

## Introduction
## Glossary
## Requirements
### Requirement 1: <short name>
**User Story:** As a <role>, I want <capability>, so that <benefit>.
#### Acceptance Criteria
1. WHEN <trigger> THEN <system> SHALL <response>
```

Four things carry most of the weight:

- **The Introduction states the problem, not the solution.** If the problem is
  a measured defect, give the measurement and where it came from — a profile, a
  failing test, a traceback, a latency percentile. A reader who disagrees with
  the introduction should be able to say so before reading further, which is
  only possible if the introduction is falsifiable.
- **The Glossary defines every term the spec then uses precisely.** Python specs
  lean on words like *record*, *session*, *worker*, *task* that already mean
  three things each in an async codebase. Defining them once is what lets the
  acceptance criteria be terse.
- **Scope and out-of-scope are explicit.** Say what this spec will not do.
  Reviewers argue about absent boundaries far more than present ones.
- **Every acceptance criterion is numbered and contains `SHALL`.** The numbers
  are the addresses that `tasks.md` cites. Without them there is no traceability
  and the spec is three disconnected essays.

**Stop when `requirements.md` is done.** Show it to the user and ask whether it
is right before starting the design. Do not write `design.md` in the same turn.
This gate exists because requirements are cheap to change and designs are not.

## Step 3 — design.md

Read `references/design-template.md` and follow it.

Standard section order — use these headings verbatim, and omit a section only
when it genuinely does not apply:

```
## Overview
## Architecture
## Components and Interfaces
## Data Models
## Correctness Properties
## Error Handling
## Testing Strategy
## Dependencies
## Non-Goals
```

Two sections are load-bearing and are the ones most often written thinly:

- **Correctness Properties.** Each property gets a heading, a
  `**Validates: Requirements N.M, N.M**` line, and an argument for *why* the
  property holds — not a restatement that it does. This is the section that
  turns a design into something reviewable, because it is where a reader can
  find the reasoning and disagree with it.
- **Testing Strategy.** Name the tests that will exist and say what each one
  proves. Where a test proves less than it appears to, say so; a test whose
  limits are stated is worth more than one whose limits are discovered later.

Prefer concrete signatures, tables, and short code sketches over prose. A
`Protocol` or `@dataclass` showing the actual attributes and methods — with
type hints, and `async` where it matters — settles more questions per line than
a paragraph describing them.

**Stop when `design.md` is done.** Show it and ask before starting tasks.

## Step 4 — tasks.md

Read `references/tasks-template.md` and follow it.

```markdown
# Implementation Plan

## Overview

## Tasks

- [ ] 1. <imperative summary of the task>
  - <what specifically to do>
  - <the decision this task settles, or the check that proves it done>
  - _Requirements: 1.1, 1.2_

## Task Dependency Graph
## Notes
```

Rules that matter:

- **Every task ends with a `_Requirements:` line** citing the numbered criteria
  it satisfies. This is the traceability link, and `check_spec.py` enforces both
  directions of it: no task may cite a criterion that does not exist, and no
  requirement may go uncited by every task.
- **Every task is work a developer can start and finish.** "Implement the
  feature" is not a task. "Add `commit()` to the `Writer` protocol, implement it
  on `FileWriter` as an fsync of the segment and its directory, and leave
  `MemoryWriter.commit()` a no-op" is.
- **Order for evidence, not convenience.** Where a spec fixes a defect, the
  first task usually produces a *failing* pytest case that reproduces it,
  because a test written after the fix cannot show the defect was ever real.
- **Checkbox states**: `[ ]` not started, `[x]` done, `[~]` partial.
- **The dependency graph is a fenced `json` block** listing waves of tasks that
  can proceed in parallel. Wave 0 is whatever gates everything else.

## Step 5 — Validate

```bash
python3 .claude/skills/kiro-spec/scripts/check_spec.py .kiro/specs/<feature-name>
```

Fix every error. Errors are unambiguous defects: a criterion with no `SHALL`, a
duplicated criterion number that makes a citation ambiguous, a task citing a
requirement that does not exist.

Warnings are judgements the script cannot make for you — a missing `## Glossary`,
a user story with no benefit clause, acceptance criteria that no task covers.
Read each one and decide. The last of those is worth real attention: a criterion
no task delivers means either the plan is incomplete or the requirement was never
really wanted.

**On a spec you are writing from scratch, run it with `--strict`** and clear the
warnings too. The default is lenient because spec trees accumulate drift, and
someone editing an older spec should not have to wade through fifty warnings to
find the error they introduced.

## House conventions

| Convention | Rule |
|---|---|
| Prose line width | Wrap at 80 columns. Tables, code blocks, and URLs may exceed it. |
| Code blocks | Fence as `` ```python ``. Show type hints; show `async def` where the real signature is async. |
| Code references | Backticks with the path and, where useful, the line: `` `src/store/writer.py:118` ``. A dotted path (`` `store.writer.FileWriter.commit` ``) is better when the location may move. |
| Cross-spec links | Relative: `` `.kiro/specs/other-spec/` `` or `[text](../other-spec/requirements.md)` |
| Directory name | `kebab-case`, naming the feature, not the change type — `durable-record-writes`, not `fix-fsync-bug` |
| Version constraints | Quote them as `pyproject.toml` spells them: `httpx >= 0.27, < 0.29` |
| File headers | Specs are Markdown; follow whatever the project's own convention says about headers in Markdown, which is usually "none". |

## Failure modes to avoid

**Vague criteria.** The test is whether two readers would agree on whether the
system passes.

```markdown
✗  1. The system SHALL handle errors appropriately

✓  1. IF the upstream returns 5xx THEN `fetch_record` SHALL raise
      `UpstreamError` carrying the status code, and SHALL NOT return a
      partially populated `Record`
```

**Design decisions smuggled into requirements.** Requirements say what must be
true; the design says how. Naming the mechanism in a requirement forecloses the
design review before it happens.

```markdown
✗  1. WHEN a record is written THEN the system SHALL call `os.fsync()` on
      the segment's file descriptor

✓  1. WHEN `write_record` returns successfully THEN the record SHALL
      survive a `SIGKILL` of the process, and SHALL be readable after
      restart
```

**Tasks that restate requirements.** A task says what to *do*; if it only
repeats what must be true, the plan has not been made yet.

```markdown
✗  - [ ] 3. Make writes durable
     - _Requirements: 1.1_

✓  - [ ] 3. Route every write through `FileWriter.commit()`
     - Fsyncs the segment and its containing directory before returning
     - `MemoryWriter.commit()` stays a no-op, so the in-memory path is free
     - _Requirements: 1.1, 1.2_
```

**Padding the count.** Six sharp requirements beat twenty that overlap. If two
requirements can only ever fail together, they are one requirement.

**Writing all three files in one turn.** The approval gates in steps 2 and 3 are
what make the spec the user's rather than yours.
