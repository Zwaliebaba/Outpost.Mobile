# 0054 — A design is amended in place as its slices land

Status: accepted
Date: 2026-09-01

## Context

`Design/README.md` stated a rule for what happens to a design once the code disagrees with it: *"a
design is never rewritten to match what was built; where the code diverged, the decision record says
so and the design keeps its argument."* It was written to protect something real. A design is the
document later changes are reviewed against, and a design edited to match whatever shipped is a
ratchet that can only ever agree with the code — it stops being able to say *the code is wrong*,
which is most of what it is for. The rule also refuses a specific dishonesty: retrofitting a
rationale, so that a decision taken under pressure reads afterwards as one that was always intended.

`Design/Combat.md` ran that rule over six slices and showed what it costs. Five slices landed, and
four of them came back different from the design in ways worth recording: the fire events took their
own datagram message rather than a block in the fragment header; no shipped hull carries the bones
§10.1 planned to pose; the turret slew and the `Gun` markers moved to a sixth slice; the fire pass
runs last in the tick rather than in the standing-intent slot. Each was recorded where the rule said
to record it — a note at the top of the design, a section in a work order, a decision record — and
each was correct and traceable.

The result was a document whose first screen was three blocks of correction and whose body then
described a game that does not exist. §9.2 was headed *The fire block*. §10.1 described bones. §10.3
said a bar reads that nothing draws. A reader had to hold the corrections in their head while
reading past the sections they corrected, and the failure mode is not hypothetical: this session's
own documentation pass found four code comments and two rule-document sentences that had been
falsified and missed, including one that ADR 0052 explicitly says "changes in this commit" and then
did not. Corrections at the top are easy to write and easy to stop reading.

The owner read the finished result and asked for the body amended in place.

## Decision

**A design is amended in place as its slices land.** Where the code came back different, the design
changes to say what was built, in the commit that built it, so that every section of a live design
describes the game as it stands.

Two limits keep what the old rule was protecting. **What is amended is the shape, never the
argument**: a section whose reason a slice took away says which slice took it and why, and no
section acquires a rationale it did not have — a design must still be able to say the code is wrong,
and a reader must still be able to tell a decision from a rationalisation. And **the history goes to
the work order and the decision record**, which are append-only and never rewritten: a work order
says what changed on contact and what the change cost, a record says what was chosen and what lost.
An amended design says at its top that it is amended and points at both. An **archived** design is
not amended at all — it is finished, and editing it would be rewriting history rather than
maintaining a live document.

## Alternatives considered

- **Keep the rule as it stood.** It is the safer default and it is what a reader of this folder will
  propose again, so its argument is recorded rather than left to be re-derived: a design that cannot
  disagree with the code is a design that has stopped doing its job, and the correction-note form
  makes every divergence deliberate and visible in the diff. It lost on what six slices of evidence
  showed it produces — a body that lies and a header nobody finishes reading — and because the two
  limits above keep the part that mattered. A live design is a description of a shape; the argument
  for that shape survives in the same document, and the argument for *changing* it survives in the
  work order that changed it.
- **Amend in place with no limits.** Rejected for exactly the ratchet the old rule named. Without
  "shape, not argument" the first hard slice rewrites the design into a description of itself, and
  the next reviewer has nothing left to review against.
- **A "current state" section at the top of each design, body untouched.** This is the
  correction-note form with better formatting, and it fails the same way at the same place: the
  body still contradicts it, and the contradiction is what a reader carries away.
- **Delete the design once its slices land and keep only the work orders.** Rejected because the
  slice-shaped record is the wrong shape for a reader: no work order holds the argument for why
  combat resolves without dice, and reconstructing it from six of them is not a thing anyone will
  do.
- **Let each design choose.** Rejected because the value of the rule is that a reader knows, without
  checking, whether the document in front of them describes the game or describes an intention.

## Consequences

A live design now reads as one document rather than a body plus a correction stack, and the
`Design/` directory answers "what is the shape of this feature today" without a second lookup. That
is worth most on a design like Combat's, where six slices touch four layers.

It costs discipline in three places, and none of them is enforceable by a check:

- **A slice that changes a design's shape must amend the design in the same commit.** This is the
  same obligation `AGENTS.md` already places on the rulebook and on code comments, extended to one
  more document; the failure mode is the one this session found — the sentence that everybody agreed
  would change, and then nobody changed.
- **The work order must be written as if it is the record**, because it now is: it is the only place
  that says what the design used to say. `Combat-slice-1.md` §4a is the shape of it.
- **`git log` and `git blame` become load-bearing** for anything the work orders do not name.

`Design/Combat.md` is amended under this record in the commit that carries it, and is the first
design maintained this way. Every design already in `Design/Archive/` keeps the old rule, which
costs nothing: they are finished, and nothing in them is going to diverge again.
