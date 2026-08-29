# Design — how a feature gets from an idea into the tree

Five kinds of document exist, and they answer five different questions. Keep them apart: a design
that tries to be a work order is vague where it needs to be exact, and a work order that tries to
be a design argues where it should instruct.

| Document | Question it answers | Where |
|---|---|---|
| Rules | *What are the rules right now?* | the conformance file at the repository root |
| Design | *What are we building, and what shape does it have?* | `Design/<topic>.md` |
| Work order | *What exactly do I do next, and when am I done?* | `Design/<topic>-work-order.md`, or `Design/<topic>-slice-N.md` when a design yields several |
| Decision record | *Why did it go this way, and what lost?* | `Design/Decisions/NNNN-<slug>.md` |
| Review | *Where does the tree stand against a stated goal?* | `Design/<topic>Review.md` |

## Designs

A design states the problem, the options, the chosen shape, and what it deliberately leaves out.
It may run long. It is not implementable on its own, and it should say so: its last section is
**Slices**, an ordered list of work orders with the dependencies between them, so the reader knows
what has to exist before what.

## Work orders

A work order is what an agent, or a person on a Monday, implements. One slice, one branch, one
pull request. It carries:

- **Scope** and, just as important, **out of scope** — what this slice must not touch.
- **What to build on** — the existing types and files, by name, so nothing is reinvented.
- **Acceptance** — how "done" is decided, and by what: a test where a test can decide it, a
  screenshot where only a screen can, a stated assumption where neither can. "Renders at any
  resolution" is a screenshot at two sizes; "no allocation per frame" is a code read; "the ring
  keeps the newest eight" is a test.
- **Assumptions the implementer may make** — placeholders and stubs it is allowed to leave, so
  they are declared in the order rather than discovered in the review.

A good work order needs no guessing about what finished means: positions and sizes as numbers,
colours and paddings by the name of the constant that holds them, behaviour as rules, and an
acceptance list at the end.

A work order is finished when its pull request merges, and it then moves to `Design/Archive/` in
the same commit that marks the slice landed in its design. It is kept, not deleted: code comments
and decision records cite work orders by section, and the citations are meant to be followed. A
design never moves — it stays the document later changes are reviewed against.

## Decision records

A decision record is one page: the context that forced a choice, the decision, the alternatives
considered and why each lost, and the consequences. Records are numbered in order of writing and
never renumbered or rewritten into a different decision; a change of mind is a new record that
supersedes the old one, which stays with its status changed. One is required whenever a type
moves between libraries, a dependency rule or build guard changes, a project or dependency is
added or removed, or an approach is turned down that someone will propose again.

## Reviews

A review is a point-in-time audit of the tree against a stated goal, citing `file:line` at a named
commit. It changes nothing by itself: a finding becomes work only when someone turns it into a work
order, and a review goes stale by design — it describes the commit it names, and nothing keeps it
true afterwards. That is the difference from a design, which stays the document later changes are
reviewed against.

## The loop

```
design (if non-trivial)
  -> work order: scope, out of scope, acceptance
    -> branch per slice
      -> implement; build; all test suites; the build checks; decision record if one is due
        -> pull request carrying the evidence: test names, screenshots, the hand-back report
          -> human review
            -> merge; next slice
```

**One agent per slice, one slice per layer at a time.** Slices in different layers — a simulation
table and a client panel — can run in parallel because they share no files. Two slices in the
same layer cannot: both edit the same project files, umbrella header and rulebook, and the second
to merge spends its time on conflicts.

The review gate between slices is where a rule whose premise has expired gets caught. Rules are
obeyed long after the reason for them has gone unless something makes a person look, and a review
of a slice against its work order is that something.

## What every slice hands back

The hand-back checklist in the rulebook, plus:

- for anything visual, a screenshot at two window sizes in the pull request;
- the assumptions made and the placeholders left, stated, not implied;
- a decision record when one is due, in the same pull request as the change it explains.
