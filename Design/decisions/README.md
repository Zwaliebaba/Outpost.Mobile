# Architecture decision records

One file per decision that shaped the tree, written when the decision was made, kept when it is
reversed. AGENTS.md states the rules as they stand; this folder is why they stand, and what was
turned down on the way.

Numbered in order of writing, never renumbered: `NNNN-short-slug.md`. A record is never edited
into a different decision — a change of mind is a new record that names the one it supersedes,
and the old one gets `Status: superseded by NNNN`.

## Template

```
# NNNN — Title

Status: accepted | superseded by NNNN
Date: YYYY-MM-DD

## Context
What was true, and what was forcing a choice. Facts, not preferences.

## Decision
One paragraph. What was decided, in the imperative.

## Alternatives considered
Each one, and the reason it lost. This is the part a rulebook cannot hold.

## Consequences
What the decision costs and what it makes easier, including what now has to be done by hand.
```

## Index

| # | Decision | Status |
|---|---|---|
| [0001](0001-headless-core-and-server.md) | NeuronCore and NeuronServer are headless | accepted |
| [0002](0002-content-readers-live-with-their-consumer.md) | A content reader lives in the library that consumes what it reads | accepted |
