# `tasks.md` template

Copy the skeleton and fill it in.

---

````markdown
# Implementation Plan

## Overview

<One or two paragraphs: how the work is divided, and what principle sets the
order. If the order matters — a failing test before the fix, correctness before
performance — say so here, because a reader who does not know the principle
will reorder the tasks.>

## Tasks

- [ ] 1. <imperative summary: what this task delivers>
  - <specific action, module, or signature>
  - <the decision this task settles, or the check that proves it done>
  - _Requirements: 1.1, 1.2_

- [ ] 2. <...>
  - <...>
  - _Requirements: 2.1_

- [ ] 3. <parent task with sub-tasks>
  - <scope of the parent>
  - _Requirements: 3.1_

- [ ] 3.1 <sub-task>
  - <...>
  - _Requirements: 3.1_

## Task Dependency Graph

```json
{
  "waves": [
    {
      "wave": 0,
      "tasks": [1],
      "description": "Failing test that reproduces the defect — gates everything else"
    },
    {
      "wave": 1,
      "tasks": [2, 3],
      "description": "Independent of each other; both depend on wave 0"
    }
  ]
}
```

## Notes

- <constraint, gate, or explicit exclusion a reader of the task list would
  otherwise have to infer>
````

---

## Section notes

### Task granularity

A task is one unit of work a developer can pick up and finish. Test both ends:

- **Too large**: "Make writes durable." Nobody can tell when it is done.
- **Too small**: "Add the `import os`." It is a step inside a task, so it
  belongs in a bullet.
- **Right**: "Add `commit(through: int)` to the `Writer` protocol; implement it
  on `FileWriter` as an fsync of the segment and its containing directory; leave
  `MemoryWriter.commit()` a no-op so the in-memory path stays free."

Aim for 8–20 top-level tasks. Use `N.M` sub-tasks when a task has genuinely
separable parts, not to pad the count.

### The `_Requirements:` line

Every task ends with one. It is the traceability link, and `check_spec.py`
enforces both directions:

- **Forward**: a cited criterion must exist in `requirements.md`. A citation of
  `4.7` when Requirement 4 has six criteria is a typo the validator will catch.
- **Backward**: every criterion must be cited by at least one task. An uncited
  criterion means either the plan is incomplete or the requirement was never
  really wanted — both worth knowing before implementation starts.

Format: `  - _Requirements: 1.1, 1.2, 2.3_`. Ranges use an en dash: `1.1–1.6`.

### Ordering

State the ordering principle in the Overview and then honour it. Four that
recur in Python work:

- **Evidence before change.** Where the spec fixes a defect, task 1 writes a
  *failing* pytest case that reproduces it. A test written after the fix cannot
  show the defect was ever real, and the observed failure is worth recording in
  the task when it fails.
- **Correctness before performance.** Land the slow, obviously-correct version
  first so the fast one has a known-correct baseline to be measured against.
- **Gates first.** A spike that could invalidate the design — an unproven
  library, a C extension that may not build on a supported version — goes in
  wave 0, and the Notes say what happens if it comes back negative.
- **Types and packaging with the code, not after.** Annotations, `__init__.py`
  exports, and `pyproject.toml` changes belong in the task that introduces the
  module. Deferring them to a "tidy up" task at the end means the intervening
  tasks are all built against an unfinished surface.

### Checkbox states

| Marker | Meaning |
|---|---|
| `- [ ]` | not started |
| `- [x]` | complete |
| `- [~]` | partially complete — the task body says which part |

### Recording what actually happened

When a task is completed and reality diverged from the plan, amend the task
rather than only ticking it. A note like "**Observed: the failing test showed 0
of 33 records fsynced, not the 20–25% predicted**", with the explanation of why,
is the most valuable thing in a finished `tasks.md`, because it is the only
place the difference between the plan and the work survives.

Where two tasks were delivered as one change, say so in the task body and say
why splitting them would have cost more than it bought.

### Task Dependency Graph

A fenced `json` block of waves. Every top-level task number appears in exactly
one wave. Wave 0 is whatever gates the rest; tasks within a wave can proceed in
parallel. Each wave carries a `description` saying what it accomplishes and what
it depends on.

### Notes

The place for constraints that do not belong to a single task: hard gates, work
explicitly excluded, the reasoning behind an exclusion a reader would otherwise
question, and any command a developer will need repeatedly — the marker that
selects the new tests, the `nox` session that reproduces CI.

### Optional headings

`## Status: Complete`, `## Summary`, and phase headings
(`## Phase 1: <name> (Tasks 1–4)`) are all reasonable. Phases are useful when a
plan exceeds ~15 tasks; keep the task numbering continuous across them so
`_Requirements:` citations stay unambiguous.
