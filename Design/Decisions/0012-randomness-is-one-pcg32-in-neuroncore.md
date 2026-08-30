# 0012 — Randomness is one seeded PCG32, and it lives in NeuronCore

Status: accepted
Date: 2026-08-29

## Context

AGENTS.md §5 has said since the tree began that GameLogic gets "one seeded PCG32 when randomness
arrives", and [`GameLogic/GameLogic.h`](../../GameLogic/GameLogic.h) repeats it. Until now nothing
in the tree drew a random number at all, so the sentence was a promise rather than a fact and said
nothing about *where* the generator would live.

[`Design/Archive/SpaceshipExplosion.md`](../Archive/SpaceshipExplosion.md) is what makes it arrive, and it arrives
from an awkward direction: the first caller is a **visual effect**, in the executable, and
presentation randomness is explicitly allowed to differ between two clients watching one match.
An effect could therefore hide a generator in `Outpost` and nothing would notice — until the
simulation needs one, finds it cannot reach into the executable (AGENTS.md §2: nothing depends on
the executable), and writes a second. Two generators is the outcome the rule was written to
prevent, and by then each has callers.

## Decision

There is one generator type, `Neuron::Pcg32`, and it lives in `NeuronCore` — the one library both
`GameLogic` and `NeuronClient` may depend on. It is the reference PCG-XSH-RR 64/32: header-only,
`constexpr`-constructible, seeded by the reference procedure so that a seed names the same stream
on every machine, with `Next`, `Below`, `Float01` and `Signed` and nothing else. It seeds itself
from nothing: no clock, no OS entropy, no `<random>`. Whoever holds a generator seeds it and states
what it seeded from — the simulation will hold one, the view holds as many as it likes, and the two
are never the same stream.

## Alternatives considered

- **A private generator in the executable, or in the effect.** Rejected: it is the cheapest thing
  today and guarantees a second generator the day `GameLogic` needs one, because the executable is
  the one place no library may reach.
- **`<random>`'s engines and distributions.** Rejected: `std::mt19937` is specified but every
  `std::uniform_*_distribution` is implementation-defined, so the same seed and the same
  distribution give different numbers on two standard libraries. That is the exact property a seed
  exists to provide.
- **The generator in `GameLogic`.** Rejected: `NeuronClient` may not name a game type or depend on
  `GameLogic` (AGENTS.md §2), so the client could not reach it and would write its own.
- **Seeding from the clock or the OS when "unpredictable" is wanted.** Rejected: it puts a
  non-deterministic input inside a library that both halves link, one `#include` away from the
  simulation. A caller that wants variety builds a seed out of something it can name — a handle, a
  tick — which is reproducible and still different every time.
- **A richer surface now** — Gaussian, shuffle, a unit vector on a sphere. Rejected: each has more
  than one correct definition, and the first caller wanting one should write it at the call site
  where its convention is visible. They can move here when a second caller wants the same one.

## Consequences

- `NeuronCore` gains a header and a responsibility it did not have. That is what this record is
  for: it is the library both halves share, and randomness is not graphics and not game semantics.
- Every future "where do I get a random number" has one answer, and reviews have something to
  point at.
- Nothing in the tree draws from entropy, so no run can be irreproducible by accident. The cost is
  that a caller wanting an unpredictable stream must construct a seed deliberately and say so.
- AGENTS.md §5's sentence becomes true rather than aspirational; it does not change.
- The number is 0012, not the 0011 the work order asked for: 0011 was taken by the NMO record
  between the design being written and this landing, and records are never renumbered.
