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
| [0003](0003-neighbour-list-sorts-by-surface-not-centre.md) | The neighbour list sorts by surface proximity, not centre distance | accepted |
| [0004](0004-separation-clamp-caps-the-pair-not-the-ship.md) | The separation clamp caps what a pair closes, then splits it | accepted |
| [0005](0005-ship-handles-carry-a-slot-not-an-index.md) | A ship handle carries a stable slot, not the ship's array index | accepted |
| [0006](0006-separation-solve-iterates-within-a-tick.md) | The separation solve runs several times per tick | accepted |
| [0007](0007-the-index-stores-a-whole-position.md) | The spatial index stores a whole position, and pays for it | accepted |
| [0008](0008-the-wire-format-lives-in-gamelogic.md) | The wire format lives in GameLogic | accepted |
| [0009](0009-a-snapshot-carries-a-view-record.md) | A snapshot carries a view record, not the simulation's ship | accepted |
| [0010](0010-interest-sets-are-sorted-vectors.md) | Interest sets are sorted vectors, not hash maps | accepted |
| [0011](0011-ship-meshes-are-nmo-and-its-tools-are-python.md) | Ship meshes are NMO v2, and its reference tooling is Python | accepted |
| [0012](0012-randomness-is-one-pcg32-in-neuroncore.md) | Randomness is one seeded PCG32, and it lives in NeuronCore | accepted |
| [0013](0013-allegiance-is-identity-on-the-wire.md) | Allegiance is identity on the wire, not a relation | accepted |
| [0014](0014-command-authority-is-gated-in-the-simulation.md) | Command authority is gated in the simulation, not the adapter | accepted |
| [0015](0015-npc-behavior-lives-in-gamelogic.md) | NPC behavior lives in GameLogic, inside the tick | accepted |
| [0016](0016-bodies-are-presentation.md) | A planet or an asteroid is presentation, not a simulation entity | accepted |
| [0017](0017-the-tree-gains-a-compute-pipeline.md) | The tree gains a compute pipeline, and the CPU generator stays as its reference | accepted |
| [0018](0018-shaders-are-dxil-6-7.md) | Shaders are DXIL for shader model 6.7, compiled by DXC | accepted |
| [0019](0019-fxvertex-is-packed.md) | FxVertex is packed: float position, SNORM16 normal, UNORM8 colour, half uv | accepted |
| [0020](0020-the-bake-is-the-producer.md) | The compute bake is the producer, and three silent defects had to go first | accepted |
| [0021](0021-the-network-transport-is-msquic.md) | The network transport is MsQuic, and the seam stays datagram-shaped | accepted |
| [0022](0022-msquic-workers-enqueue-and-the-owning-thread-delivers.md) | MsQuic's workers enqueue to a ring, and the owning thread delivers | accepted |
| [0023](0023-the-development-credential-is-self-signed-at-boot.md) | The development credential is self-signed at boot, and the client does not validate | accepted |
