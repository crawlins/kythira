#!/usr/bin/env python3
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

"""Structural validator for a Kiro spec directory.

Checks the mechanical properties of `requirements.md`, `design.md` and
`tasks.md` that are tedious to verify by eye and easy to get wrong: missing
sections, acceptance criteria without `SHALL`, tasks that cite requirements
which do not exist, and requirements that no task covers.

It deliberately checks structure, not judgement. A spec can pass every check
here and still be a bad spec; the point is that a human reviewer's attention
should go to the parts a script cannot judge.

Usage:
    check_spec.py .kiro/specs/<feature-name> [--strict]

Exit status is 1 if any error was reported, 0 otherwise. `--strict` also fails
on warnings.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

MAX_PROSE_WIDTH = 80

REQUIRED_SECTIONS = {
    "requirements.md": ["Introduction", "Requirements"],
    "design.md": ["Overview"],
    "tasks.md": [],
}

RECOMMENDED_SECTIONS = {
    "requirements.md": ["Glossary"],
    "design.md": [
        "Architecture",
        "Components and Interfaces",
        "Correctness Properties",
        "Error Handling",
        "Testing Strategy",
    ],
    "tasks.md": ["Overview", "Task Dependency Graph", "Notes"],
}

# `### Requirement 3: Group commit` or `## Requirement 3 — Group commit`
REQUIREMENT_HEADING = re.compile(
    r"^#{2,3}\s+Requirement\s+(\d+)\s*[:—\-]?\s*(.*)$"
)
# `1. WHEN ... THEN ... SHALL ...`, at the left margin. Indented numbered
# items are sub-lists inside a criterion, not criteria in their own right.
CRITERION = re.compile(r"^ {0,1}(\d+)\.\s+(.*)$")
USER_STORY = re.compile(r"^\*\*User Story:\*\*", re.IGNORECASE)
ACCEPTANCE_HEADING = re.compile(
    r"^(#{3,4})\s+Acceptance Criteria\s*$", re.IGNORECASE
)
# `- [ ] 12. Title` / `- [x] 3.1 Title`
TASK = re.compile(r"^-\s+\[([ x~])\]\s+(\d+(?:\.\d+)?)\.?\s+(.*)$")
# Two citation forms are in use: `_Requirements: 1.1, 1.2_` on a task, and
# `**Validates: Requirements 1.1, 1.2**` on a task that pairs with a design
# property. Both are traceability links, so accept either.
REQ_REF_LINE = re.compile(
    r"^\s*[-*]?\s*(?:_Requirements?:\s*(?P<req>.+?)_"
    r"|\*\*Validates:\s*Requirements?\s*(?P<val>.+?)\*\*)\s*$",
    re.IGNORECASE,
)
# A third citation form appears inline in task bodies: `(Req 3.2, 4.4)`.
INLINE_REQ_REF = re.compile(r"\(Req(?:uirements?)?\.?\s+([\d.,\s–—-]+)\)", re.I)
# `1.2`, or a range `1.1-1.6` / `1.1–1.6`
REQ_REF = re.compile(r"(\d+)\.(\d+)\s*(?:[–—-]\s*(\d+)\.(\d+))?")
HEADING = re.compile(r"^(#{1,6})\s+(.*)$")
FENCE = re.compile(r"^\s*```")


@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def error(self, where: str, message: str) -> None:
        self.errors.append(f"{where}: {message}")

    def warn(self, where: str, message: str) -> None:
        self.warnings.append(f"{where}: {message}")


def strip_fenced_blocks(lines: list[str]) -> list[tuple[int, str]]:
    """Return (line_number, text) pairs outside fenced code blocks."""
    out: list[tuple[int, str]] = []
    in_fence = False
    for number, text in enumerate(lines, start=1):
        if FENCE.match(text):
            in_fence = not in_fence
            continue
        if not in_fence:
            out.append((number, text))
    return out


def headings(body: list[tuple[int, str]]) -> list[str]:
    return [m.group(2).strip() for _, text in body if (m := HEADING.match(text))]


BLOCK_START = re.compile(r"^\s*(?:[-*+]\s|\d+\.\s|\||>|#{1,6}\s)")


def join_wrapped(body: list[tuple[int, str]]) -> list[tuple[int, str]]:
    """Fold wrapped continuation lines into the item they continue.

    Specs wrap prose at 80 columns, so a single acceptance criterion or user
    story routinely spans three lines and the `SHALL` lands on the second. A
    line-at-a-time parser would report that as a missing `SHALL`, so join the
    continuations back together before matching. Only list items and user
    stories absorb continuations; anything that starts a new block (a heading,
    a bullet, a numbered item, a table row) ends the fold.
    """
    joined: list[tuple[int, str]] = []
    open_item = False

    for number, text in body:
        if not text.strip():
            open_item = False
            joined.append((number, text))
            continue

        if open_item and not BLOCK_START.match(text):
            start, existing = joined[-1]
            joined[-1] = (start, f"{existing} {text.strip()}")
            continue

        joined.append((number, text))
        open_item = bool(
            CRITERION.match(text)
            or TASK.match(text)
            or USER_STORY.match(text.strip())
            or REQ_REF_LINE.match(text)
        )

    return joined


def check_line_width(name: str, body: list[tuple[int, str]], report: Report) -> None:
    """Warn about over-long prose. Tables, links and headings are exempt."""
    offenders = []
    for number, text in body:
        if len(text) <= MAX_PROSE_WIDTH:
            continue
        stripped = text.strip()
        if stripped.startswith(("|", "#")) or "](" in stripped or "http" in stripped:
            continue
        if "`" in stripped and len(stripped.split()) < 4:
            continue
        offenders.append(number)
    if offenders:
        shown = ", ".join(str(n) for n in offenders[:8])
        more = f" (+{len(offenders) - 8} more)" if len(offenders) > 8 else ""
        report.warn(
            name,
            f"{len(offenders)} prose line(s) exceed {MAX_PROSE_WIDTH} columns: "
            f"line {shown}{more}",
        )


def check_sections(name: str, body: list[tuple[int, str]], report: Report) -> None:
    present = headings(body)
    for wanted in REQUIRED_SECTIONS.get(name, []):
        if not any(h.startswith(wanted) for h in present):
            report.error(name, f"missing required section '## {wanted}'")
    for wanted in RECOMMENDED_SECTIONS.get(name, []):
        if not any(h.startswith(wanted) for h in present):
            report.warn(name, f"no '## {wanted}' section")


def parse_requirements(
    body: list[tuple[int, str]], report: Report
) -> dict[int, set[int]]:
    """Return {requirement number: {criterion numbers}}."""
    body = join_wrapped(body)
    criteria: dict[int, set[int]] = {}
    current: int | None = None
    saw_user_story = False
    acceptance_level = 0
    titles: dict[int, str] = {}

    for number, text in body:
        heading = REQUIREMENT_HEADING.match(text)
        if heading:
            if current is not None and not saw_user_story:
                report.warn(
                    "requirements.md",
                    f"Requirement {current} has no '**User Story:**' line",
                )
            current = int(heading.group(1))
            if current in criteria:
                report.error(
                    "requirements.md",
                    f"line {number}: Requirement {current} is defined twice",
                )
            criteria.setdefault(current, set())
            titles[current] = heading.group(2).strip()
            saw_user_story = False
            acceptance_level = 0
            continue

        if current is None:
            continue

        if USER_STORY.match(text.strip()):
            saw_user_story = True
            if "so that" not in text.lower():
                report.warn(
                    "requirements.md",
                    f"line {number}: Requirement {current}'s user story has no "
                    "'so that' clause — the benefit is what justifies the "
                    "requirement",
                )
            continue

        acceptance = ACCEPTANCE_HEADING.match(text)
        if acceptance:
            acceptance_level = len(acceptance.group(1))
            continue

        # Criteria are often grouped under deeper sub-headings (`##### Unit
        # tests`) with the numbering running straight through them, so only a
        # heading at or above the Acceptance Criteria heading's own level ends
        # the section.
        heading_here = HEADING.match(text)
        if heading_here and len(heading_here.group(1)) <= acceptance_level:
            acceptance_level = 0
            continue

        item = CRITERION.match(text)
        if item and acceptance_level:
            index = int(item.group(1))
            if index in criteria[current]:
                report.error(
                    "requirements.md",
                    f"line {number}: Requirement {current} numbers criterion "
                    f"{index} twice",
                )
            criteria[current].add(index)
            if "SHALL" not in item.group(2):
                report.error(
                    "requirements.md",
                    f"line {number}: criterion {current}.{index} contains no "
                    "'SHALL' — it states a wish, not a requirement",
                )

    if current is not None and not saw_user_story:
        report.warn(
            "requirements.md", f"Requirement {current} has no '**User Story:**' line"
        )

    if not criteria:
        report.error(
            "requirements.md",
            "no '### Requirement N: ...' headings found — nothing for tasks.md "
            "to cite",
        )

    for index in sorted(criteria):
        if not criteria[index]:
            report.error(
                "requirements.md",
                f"Requirement {index} ({titles.get(index, '?')}) has no numbered "
                "acceptance criteria under an '#### Acceptance Criteria' heading",
            )
        else:
            expected = set(range(1, max(criteria[index]) + 1))
            missing = sorted(expected - criteria[index])
            if missing:
                report.warn(
                    "requirements.md",
                    f"Requirement {index} skips criterion number(s) "
                    f"{', '.join(str(n) for n in missing)}",
                )

    expected = set(range(1, max(criteria) + 1)) if criteria else set()
    gaps = sorted(expected - set(criteria))
    if gaps:
        report.warn(
            "requirements.md",
            f"requirement numbering skips {', '.join(str(n) for n in gaps)}",
        )

    return criteria


def expand_refs(raw: str) -> set[tuple[int, int]]:
    """Parse a traceability citation into {(requirement, criterion)} pairs.

    A criterion of 0 is a wildcard meaning "every criterion of this
    requirement" — specs cite `_Requirements: 1_` as well as `1.1, 1.2`.
    """
    refs: set[tuple[int, int]] = set()
    for token in re.split(r"[,;]| and ", raw):
        token = token.strip().strip(".")
        if not token:
            continue
        span = re.fullmatch(
            r"(\d+)\.(\d+)\s*[–—-]\s*(\d+)\.(\d+)", token
        )
        if span:
            req, start, end_req, end = (int(g) for g in span.groups())
            if req == end_req and end >= start:
                refs.update((req, n) for n in range(start, end + 1))
            else:
                refs.add((req, start))
                refs.add((end_req, end))
            continue
        single = re.fullmatch(r"(\d+)\.(\d+)", token)
        if single:
            refs.add((int(single.group(1)), int(single.group(2))))
            continue
        whole = re.fullmatch(r"(\d+)\s*(?:[–—-]\s*(\d+))?", token)
        if whole:
            first = int(whole.group(1))
            last = int(whole.group(2)) if whole.group(2) else first
            refs.update((n, 0) for n in range(first, last + 1))
            continue
        # Anything else (prose, a bare requirement name) is left for the
        # caller to report; a fallback scan catches embedded `N.M` pairs.
        refs.update(
            (int(m.group(1)), int(m.group(2)))
            for m in re.finditer(r"\b(\d+)\.(\d+)\b", token)
        )
    return refs


def parse_tasks(
    body: list[tuple[int, str]], report: Report
) -> tuple[dict[str, set[tuple[int, int]]], list[str]]:
    """Return ({task id: cited criteria}, [top-level task ids])."""
    body = join_wrapped(body)
    cited: dict[str, set[tuple[int, int]]] = {}
    order: list[str] = []
    blanket: set[str] = set()
    untraceable: list[str] = []
    current: str | None = None

    for number, text in body:
        task = TASK.match(text)
        if task:
            if current is not None and not cited[current] and current not in blanket:
                untraceable.append(current)
            current = task.group(2)
            if current in cited:
                report.error(
                    "tasks.md", f"line {number}: task {current} is numbered twice"
                )
            cited.setdefault(current, set())
            order.append(current)
            if not task.group(3).strip():
                report.warn("tasks.md", f"line {number}: task {current} has no title")
            continue

        if current is None:
            continue

        if HEADING.match(text):
            if not cited[current] and current not in blanket:
                untraceable.append(current)
            current = None
            continue

        inline = INLINE_REQ_REF.search(text)
        if inline:
            cited[current] |= expand_refs(inline.group(1))

        ref = REQ_REF_LINE.match(text)
        if ref:
            raw_ref = ref.group("req") or ref.group("val") or ""
            # Citations carry parenthetical asides — `12 (descope record)`,
            # `All (documentation only)`. Strip them before parsing.
            raw_ref = re.sub(r"\([^)]*\)", " ", raw_ref).strip()
            found = expand_refs(raw_ref)
            if not found:
                if re.match(r"all\b", raw_ref.strip(), re.IGNORECASE):
                    blanket.add(current)
                    report.warn(
                        "tasks.md",
                        f"line {number}: task {current} cites 'all requirements'. "
                        "That satisfies traceability formally but tells a reader "
                        "nothing — cite the criteria the task actually delivers",
                    )
                elif not raw_ref:
                    # `_Requirements: (housekeeping)_` — a deliberate marker
                    # that the task serves no requirement. Legitimate, but
                    # worth a second look: most tasks should trace somewhere.
                    blanket.add(current)
                    report.warn(
                        "tasks.md",
                        f"line {number}: task {current} names no requirement. "
                        "Fine for pure housekeeping; otherwise cite the "
                        "criteria it delivers",
                    )
                else:
                    report.error(
                        "tasks.md",
                        f"line {number}: task {current}'s traceability line cites "
                        f"nothing parseable: '{raw_ref}' (expected 'N.M', comma "
                        "separated)",
                    )
            cited[current] |= found

    if current is not None and not cited[current] and current not in blanket:
        untraceable.append(current)

    parents = {task.split(".")[0] for task in cited if "." in task}
    for task in untraceable:
        # A parent task whose sub-tasks carry the citations is a container, not
        # an untraceable task — the traceability lives one level down.
        if task in parents:
            continue
        report.error(
            "tasks.md",
            f"task {task} has no '_Requirements: ...' line — it is untraceable",
        )

    if not cited:
        report.error(
            "tasks.md",
            "no tasks found — expected checklist items like '- [ ] 1. Title'",
        )

    return cited, order


def check_dependency_graph(
    lines: list[str], top_level: list[str], report: Report
) -> None:
    text = "\n".join(lines)
    if "Task Dependency Graph" not in text:
        return

    blocks = re.findall(r"```json\s*\n(.*?)\n```", text, re.DOTALL)
    graph = None
    for block in blocks:
        try:
            candidate = json.loads(block)
        except json.JSONDecodeError as exc:
            report.error("tasks.md", f"dependency graph is not valid JSON: {exc}")
            return
        if isinstance(candidate, dict) and "waves" in candidate:
            graph = candidate
            break

    if graph is None:
        report.warn(
            "tasks.md",
            "a 'Task Dependency Graph' section exists but holds no ```json block "
            "with a 'waves' key",
        )
        return

    scheduled: set[int] = set()
    for wave in graph["waves"]:
        if "description" not in wave:
            report.warn(
                "tasks.md", f"wave {wave.get('wave', '?')} has no 'description'"
            )
        for task in wave.get("tasks", []):
            if task in scheduled:
                report.error(
                    "tasks.md", f"task {task} appears in more than one wave"
                )
            scheduled.add(task)

    tops = {int(t) for t in top_level if "." not in t}
    for missing in sorted(tops - scheduled):
        report.warn("tasks.md", f"task {missing} appears in no wave")
    for unknown in sorted(scheduled - tops):
        report.error(
            "tasks.md", f"the dependency graph schedules task {unknown}, which "
            "is not a top-level task"
        )


def check_traceability(
    criteria: dict[int, set[int]],
    cited: dict[str, set[tuple[int, int]]],
    report: Report,
) -> None:
    all_cited: set[tuple[int, int]] = set()
    for task, refs in cited.items():
        for req, crit in sorted(refs):
            if crit == 0:
                if req not in criteria:
                    report.error(
                        "tasks.md",
                        f"task {task} cites Requirement {req}, which does not "
                        "exist in requirements.md",
                    )
                else:
                    all_cited.update((req, n) for n in criteria[req])
                continue
            if req not in criteria:
                report.error(
                    "tasks.md",
                    f"task {task} cites Requirement {req}, which does not exist "
                    "in requirements.md",
                )
            elif crit not in criteria[req]:
                report.error(
                    "tasks.md",
                    f"task {task} cites criterion {req}.{crit}, but "
                    f"Requirement {req} has only {len(criteria[req])} "
                    f"criteri{'on' if len(criteria[req]) == 1 else 'a'}",
                )
            else:
                all_cited.add((req, crit))

    uncovered = sorted(
        (req, crit)
        for req, items in criteria.items()
        for crit in items
        if (req, crit) not in all_cited
    )
    if uncovered:
        listed = ", ".join(f"{req}.{crit}" for req, crit in uncovered[:12])
        more = f" (+{len(uncovered) - 12} more)" if len(uncovered) > 12 else ""
        report.warn(
            "traceability",
            f"{len(uncovered)} acceptance criteria are cited by no task: "
            f"{listed}{more}. Either the plan is incomplete or the criteria "
            "were never really wanted — both are worth settling before "
            "implementation starts",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spec_dir", type=Path, help="path to .kiro/specs/<feature>")
    parser.add_argument(
        "--strict", action="store_true", help="treat warnings as failures"
    )
    args = parser.parse_args()

    report = Report()
    spec = args.spec_dir

    if not spec.is_dir():
        print(f"error: {spec} is not a directory", file=sys.stderr)
        return 2

    bodies: dict[str, list[tuple[int, str]]] = {}
    raw: dict[str, list[str]] = {}
    for name in ("requirements.md", "design.md", "tasks.md"):
        path = spec / name
        if not path.is_file():
            report.error(spec.name, f"missing {name}")
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        raw[name] = lines
        bodies[name] = strip_fenced_blocks(lines)
        check_sections(name, bodies[name], report)
        check_line_width(name, bodies[name], report)

    criteria: dict[int, set[int]] = {}
    cited: dict[str, set[tuple[int, int]]] = {}
    top_level: list[str] = []

    if "requirements.md" in bodies:
        criteria = parse_requirements(bodies["requirements.md"], report)
    if "tasks.md" in bodies:
        cited, top_level = parse_tasks(bodies["tasks.md"], report)
        check_dependency_graph(raw["tasks.md"], top_level, report)
    if criteria and cited:
        check_traceability(criteria, cited, report)

    for message in report.errors:
        print(f"ERROR   {message}")
    for message in report.warnings:
        print(f"warning {message}")

    total = sum(len(v) for v in criteria.values())
    print(
        f"\n{spec.name}: {len(criteria)} requirements, {total} acceptance criteria, "
        f"{len(cited)} tasks — {len(report.errors)} error(s), "
        f"{len(report.warnings)} warning(s)"
    )

    if report.errors:
        return 1
    if args.strict and report.warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
