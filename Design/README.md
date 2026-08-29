# Design — how a feature gets from an idea into the tree

Four kinds of document live here or next to here, and they answer four different questions. Keep
them apart: a design that tries to be a work order is vague where it needs to be exact, and a
work order that tries to be a design argues where it should instruct.

| Document | Question it answers | Where |
|---|---|---|
| Rules | *What are the rules right now?* | [`AGENTS.md`](../AGENTS.md) |
| Design | *What are we building, and what shape does it have?* | `Design/<topic>.md` — e.g. [`Collision.md`](Collision.md) |
| Work order | *What exactly do I do next, and when am I done?* | `Design/<topic>-work-order.md`, or a numbered `Design/<topic>-slice-N.md` — e.g. [`IMPLEMENTATION_PROMPT.md`](IMPLEMENTATION_PROMPT.md) |
| Decision record | *Why did it go this way, and what lost?* | [`Design/decisions/`](decisions/) (AGENTS.md §9) |

## Designs

A design states the problem, the options, the chosen shape, and what it deliberately leaves out.
It may run long. It is not implementable on its own, and it should say so: its last section is
**Slices**, an ordered list of work orders with the dependency between them, so the reader knows
what has to exist before what. (`Collision.md` §5 carries that information — `HullSpec` first —
and it belongs at the end of the document under that heading, not two hundred lines in.)

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

The HUD prompt is the worked example: pixel layout, colours by constant name, touch rules, an
acceptance list. It went in cleanly because nobody had to guess what finished meant.

## The loop

```
design (if non-trivial)
  -> work order: scope, out of scope, acceptance
    -> branch per slice
      -> implement; build; all suites; Build/ checks; ADR if AGENTS.md §9 says so
        -> pull request carrying the evidence: test names, screenshots, the §8 report
          -> human review
            -> merge; next slice
```

**One agent per slice, one slice per layer at a time.** Slices in different layers — a `GameLogic`
table and a `NeuronClient` panel — can run in parallel because they share no files. Two slices in
the same layer cannot: both edit the same `.vcxproj`, `.filters`, umbrella header and `AGENTS.md`,
and the second to merge spends its time on conflicts.

The review gate between slices is where a rule whose premise has expired gets caught. That is not
hypothetical: the OBJ reader stayed in NeuronCore for a day on the argument that meshes were
collision geometry, and the collision design says the opposite ([ADR 0002](decisions/0002-content-readers-live-with-their-consumer.md)).

## What every slice hands back

The checklist in AGENTS.md §8, plus:

- for anything visual, a screenshot at two window sizes in the pull request;
- the assumptions made and the placeholders left, stated, not implied;
- an ADR when a type moved, a rule changed, or an approach was turned down that someone will
  propose again.
