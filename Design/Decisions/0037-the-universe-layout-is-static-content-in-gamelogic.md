# 0037 — The universe layout is static content in GameLogic

Status: accepted
Date: 2026-08-30

## Context

`Design/Archive/Stations.md` §5 needs the starting solar system to exist as *places*: a star anchor and a
few planets at real positions on the plane. Two halves of the game consume that. The server half
spawns a Core Vanguard Command station at every planet; the client half draws the world at each
site and marks it on the minimap from the first frame, before any record has arrived — which is the
owner's "static so it can be marked".

The obvious precedent points the other way, which is why this needs a record rather than a commit
message. `Outpost/BodyCatalogue` — what a planet looks like — is client content and was deliberately
placed there by ADR 0002, "content readers live with their consumer". A planet's position looks like
more of the same thing.

It is not the same thing, and the difference is who needs it.

## Decision

`GameLogic/UniverseLayout.h` and `.cpp`: `PlanetSite`, `SystemLayout`, `SystemDesc`, and

```cpp
[[nodiscard]] SystemLayout LayOutSystem(std::uint64_t _seed, const WorldPos& _starPos, const SystemDesc& _desc);
```

A pure function of its arguments, drawing one `Neuron::Pcg32` in one fixed order, called at boot by
whichever composition root wants a system. `BodyCatalogue` stays exactly where it is: the layout
hands it a seed and a radius and nothing else.

This is also the arrival of randomness in `GameLogic`, which AGENTS.md §5 and ADR 0012 have
promised since the tree began, and the shape it arrives in is part of the decision: a pure function
of a caller-supplied seed, at boot, whose output is then ordinary spawn input. Nothing inside
`World::Step` draws. The replay contract therefore sees positions and never a generator.

## Alternatives considered

ADR 0008's three-way elimination, re-run for this type rather than cited:

- **`Outpost`, beside `BodyCatalogue`.** The precedent, and it would work today. Rejected for
  ADR 0008's reason: content that lives in one executable is in the wrong one the day there are
  two. A dedicated server has to spawn the stations, so it needs the sites; it has no business
  linking a client's composition root to get them. That day is the entire point of the seam.
- **`NeuronCore`.** It already holds `Pcg32`, and "lay points on a circle" is arguably an engine
  primitive. Rejected on the rule that makes `NeuronCore` worth having: zero game semantics. A
  `PlanetSite` is a game noun.
- **On the wire, authored by the server.** The most MMO-shaped answer, and the one this design will
  eventually want. Rejected as premature and, more usefully, as *not a redesign when it comes*: a
  server-authored layout is the same struct arriving by download instead of by call, which is
  exactly what shipping it in both binaries buys. Nothing about the consumers changes.
- **Keep the sites in `ViewTuning.h` as constants.** Three planets is three positions; a table
  would do. Rejected because the positions have to be *argued* against the path grid's ceiling and
  against each other's separation, and a table of constants cannot be tested for a property that
  has to hold for every seed. In `GameLogic` the bound is a `GameLogicTests` assertion; in the
  executable layer it would be a hope, because that layer has no suite.

## Consequences

- `GameLogic` gains one public header and the discipline that it stays position-and-seed only. What
  a planet *wears* remains the client's, and the day this header names a texture it has drifted.
- The grid ceiling is now checked rather than assumed. `TheLayoutRespectsTheGridCeiling` computes
  the worst-case static span from the shipped bounds — 2 × 6 500 m plus two grid margins, 439 cells
  of 32 m against a ceiling of 512 — and the check matters because `PathIslands` declines to build
  past the ceiling *quietly*: the symptom is ships that stop routing, a long way from this file.
- `SystemDesc`'s defaults are the shipped numbers, not placeholders. That is what lets the ceiling
  and separation bounds be proved in a suite that cannot see `ViewTuning.h`, and it means a retune
  happens in one place with a test watching it.
- Bearings are slotted with a bounded jitter rather than drawn free, so the minimum separation
  between two planets is a property of the bounds rather than of the seed. No rejection loop, and
  therefore no loop whose iteration count depends on the draw.
- Every draw happens for every planet, pinned or not: `pinFirstPlanet` overwrites what was drawn
  and never skips a draw. A flag that shifted the stream would make one seed mean two systems.
- `LayOutSystem` takes a star position precisely so that a second system is content rather than
  redesign. Nothing calls it twice today, and the per-region path grids and interest regions a
  second system 100 km away would force are `Design/Archive/RegionalPathfinding.md`'s, not this
  record's.
