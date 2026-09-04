# Galaxy map — the graph as a picture, and a destination as a tap

**Status: agreed with the owner on 2026-09-02, as drafted; §7's decision was cut on 2026-09-04 and
came out larger than drafted — the hop is forced, as §7 said, but WHERE a route is planned from was
open and the owner chose it
([ADR 0069](Decisions/0069-a-voyage-lives-on-the-fleet-and-is-planned-from-where-it-is.md)). Slices
1, 2 and 3 have landed ([`GalaxyMap-slice-1.md`](GalaxyMap-slice-1.md),
[`GalaxyMap-slice-2.md`](GalaxyMap-slice-2.md), [`GalaxyMap-slice-3.md`](GalaxyMap-slice-3.md));
slice 4 is listed in §7 and is cut when it is next. §1, §2, §4.2, §4.3, §6.4, §6.5 and §8 are amended
to say what was built. **Screenshots are owed against all three landed slices** and are recorded as
owed rather than dropped.**

`Design/Archive/Universe.md` deferred this three times, in slices 3, 4 and 4b, always for the same reason:
a fleet can cross a gate without a map, and a map is UI work that should not gate the mechanism.
The mechanism landed. This is the screen it was waiting for.

The player-facing sentence: **you can see the frontier, and point at it.** Today a galaxy of 54
systems exists and the only way to learn its shape is to fly through it one gate at a time, holding
the rest in your head.

---

## 1. What is already there

Nearly all of it, and that is the reason this design is small. Three things landed for other reasons
and each pays for a piece of the map:

- **`GalaxyLayout`** — 54 `SystemSite`s with real positions and 68 `GateLink`s. The client already
  holds it (`OutpostApp::m_galaxy`), laid out at boot from the seed in the save header, so it is the
  same graph the server spawned gates against (ADR 0055, ADR 0057).
- **`FleetStatus`** — a position, an order kind, its flags, a reserved stance byte and a size **for all five fleets**, decoded out of the
  snapshot header rather than out of the interest set, so a player is told where every fleet is
  whether or not any of them is on screen (`Design/Archive/Fleets.md` 8.2). Four of five routinely
  are not. The map's hardest data question was answered by a decision taken for the fleet bar.
- **`Game::SystemAt`** — a `UniversePos` to the system it is in, nearest star, ties to the lower
  index (ADR 0059's sibling in slice 4b). It turns each of those five positions into a dot on the
  map without the client doing geometry of its own.

- **The rail's `Universe` button** was already drawn and lit on press and was read by nothing. Since
  slice 1 it is the map's switch: the composition root follows `Hud::ActiveRail`, so the lit button
  and the open screen are one state rather than two that can drift, and Escape unlights the button
  rather than closing the screen behind its back.

What is *not* there: any way to name a system. Sending a fleet further than one gate was the other
half of this sentence until slice 3, which is what `FleetOrderKind::Voyage` and `Universe::StepVoyages`
now do (§4.3).

## 2. What is being built

- **A modal screen** over the game, opened by the rail's `Universe` button, in `AssemblyScreen`'s
  shape and for its reasons: it holds state the HUD has no business in, it consumes every pointer
  event rather than letting them fall through, and `Hud.cpp` is already eight hundred lines.

  With one difference that slice 1 found and the assembly screen does not have: the map sits *behind*
  the HUD in the pointer chain rather than ahead of it, because the rail button that opens it has to
  stay reachable to close it. The HUD is narrowed to its rail while the map is up — the minimap, the
  bar and the fleet buttons are under the map's scrim and take nothing — and the map consumes
  everything the rail did not.
- **The graph, drawn**: every system as a node at its real position, every link as an edge, the
  system the camera is in marked, and each fleet on the system it is in.
- **A tap that flies the camera** to the tapped system.
- **A tap that sends a fleet** across as many gates as it takes — which is the one piece that is not
  drawing, and is most of the work.

## 3. The projection

Systems are drawn from `starPos`, not from `cellQ`/`cellR`.

The lattice is a clean hexagon and the cells would draw a prettier picture, but the jitter is what
makes the map *true*: a player reading distances off a map that has quietly regularised them will
misjudge which of two gates is the long one. The jitter is bounded at 0.2 of the pitch, so the
picture is still legibly a lattice; it just is not a lie.

The map fits the galaxy's bounding box to the screen with a margin, isotropically — one scale for
both axes, because a galaxy stretched to fill a 16:9 screen would make the same misjudgement in the
other direction.

## 4. What a tap does

Three verbs, and they are three slices because they cost three very different amounts.

### 4.1 Look (slice 1)

The map opens, draws, and closes. No tap does anything. This is the whole screen except the verbs,
and it is worth landing alone because it is the half that cannot be wrong in an interesting way.

### 4.2 Go and look (slice 2)

Tapping a system moves the camera there and closes the map.

It cost almost nothing, and the reason was slice 4b: the scenery follows **where the camera is**, not
a jump event, and that slice's own work order named "a galaxy map that flies you somewhere" as one of
the things the choice covered in advance. **The claim held, and it held more completely than this
paragraph expected**: the flight rebuilds no scenery at all. The frame loop already asks
`SystemAtCamera` once per frame, after the last thing that moves the camera and before `Render`, and
rebuilds when the answer changes — a check written for a fleet crossing a gate, which covers a camera
put anywhere for free. `FlyToSystem` is `SnapGoal`, a cancelled focus, and the two lines that close
the map.

`SnapGoal` and not `SetGoal`: the map is a jump in attention rather than a pan, and a galaxy is three
orders of magnitude past the distance `UniverseView::FollowFocusedFleet` already calls "beyond any
worth watching". The focus is released with it, or a followed fleet would drag the camera back on the
next frame.

Nothing about the simulation changes. The player is looking somewhere else, which is not an order.

### 4.3 Go (slice 3)

With a fleet selected, tapping a system orders it there — across as many gates as the route takes.
With nothing selected the tap still flies the camera, and the two are exclusive: a camera that flew
ahead of a fleet crossing fourteen gates would leave the player watching an empty system while the
thing they ordered is behind them. §4.2 is "go and look" and this is "go".

This is the only part that is not a view. What landed, in `GameLogic`:

- **A search over the gate graph.** `RouteAcrossGates`: breadth-first over `GateLink`, giving a hop
  sequence. The graph is 54 nodes and 68 edges and the search runs on an order and on an arrival,
  never on a tick.
- **An order that survives its own hops.** A fleet crossing a gate is despawned and respawned under
  the same identity (ADR 0056), so *whatever holds the rest of the route cannot be a ship*. It lives
  on the fleet row, which survives the crossing — the same row `orderGate` already lives on.
  `FleetOrderKind::Voyage` is that order: `orderPoint` is where the fleet is going, exactly as it is
  for a Move, and `orderGate` is the door it is flying at now.
- **The galaxy, in the simulation.** The hop could not be resolved without it: a gate row names its
  far end by `EntityId` and nothing in `Universe` knew what a system was. `ConfigureGalaxy` takes the
  layout from the composition root, on `ConfigureShard`'s terms — derived from the save header's
  seed, never saved, and a universe without one refuses every voyage (ADR 0069).
- **A tick that advances it.** `StepVoyages`, immediately after the jump pass, so a fleet that
  crossed this tick is given its next door on the same tick. `StepJumps` clears a voyaging fleet's
  gate where it clears a Jump's kind, which is the whole of what "survives its own hops" means in
  code.

The failure modes are the interesting part and §6 lists them.

## 5. What is deliberately not here

- **System names.** 54 dots labelled `(-2, 3)` is not a map a person enjoys, and a name generator is
  a content system with its own argument — a syllable table is content, and content that both a
  server and a client must agree on is `GameLogic`'s, which is a rule ADR 0037 states and a name
  strains. Slice 1 labels by cell and says at the code that it is a placeholder. **Slice 4** is the
  name generator, and it is deferred rather than skipped because a map is the first thing that makes
  the lack hurt.
- **Fog of war.** Every system is drawn, whether or not anybody has been there. Exploration is a
  design this game does not have, and inventing one inside a map screen is how it would get a bad
  one.
- **Anything about other players.** One subscriber, still.
- **Zoom and pan on the map.** 54 systems fit a screen. The day the galaxy has five hundred, this
  gets a camera of its own and that is a slice, not a redesign.
- **Ordering anything but a move.** No "attack that system", no "dock at that station". A tap sends
  a fleet somewhere; what it does when it arrives is the orders that already exist.

## 6. How it must behave

1. **The map is a picture of the client's own galaxy layout**, which is laid out from the save
   header's seed — so it cannot disagree with where the gates actually are (ADR 0057).
2. **Fleet dots come from `FleetStatus`, not from the interest set**, or four of five fleets vanish
   off the map whenever the camera is not on them.
3. **A tap that resolves to no system does nothing.** The map has empty space in it and a tap there
   is not a destination.
4. **The next hop is planned from where the fleet is**, at every arrival, and no route is stored.
   This section originally said the opposite — planned once, at the order, not re-planned per hop —
   on the argument that the graph is a pure function of the layout and cannot move under a fleet.
   That argument holds, and it is exactly why the two give the same answer; what decided it the other
   way is what a stored route costs. It needs a fixed array on the fleet row and therefore a cap, and
   the shipped galaxy's diameter is **fourteen gates** — close enough to any cap worth writing that a
   differently-seeded galaxy would reach it, and a destination a player can see and cannot be sent to
   is worse than a search over 68 links. A plan is also a statement about where the fleet *was*: one
   reloaded from a save or re-ordered mid-voyage is somewhere else, and planning from its own
   position cannot go stale. It is `AdvanceRoute`'s discipline (ADR 0059) taken as the default rather
   than as the fallback, and [ADR 0069](Decisions/0069-a-voyage-lives-on-the-fleet-and-is-planned-from-where-it-is.md)
   has the alternatives.
5. **A fleet that loses its route stands down** rather than continuing to the next hop. A gate that
   has gone, a hop that no longer connects, or a fleet reduced to nothing are each a reason to stop
   with the fleet somewhere real — the same rule slice 2 of the universe design applied to a single
   jump through a stale gate.

   This is where a voyage parts company with a Jump, and the difference is deliberate: a Jump *waits*
   at a door whose far side has gone, because the player asked for that gate and losing a fleet into
   it is the one failure `StepJumps` must not have. A voyage asked for a destination, so a door it
   cannot use is a road it must be told about — it releases the gate and either finds another way or
   stops where it stands. A road out of the shard counts as one it cannot use: a fleet *row* does not
   travel in a handoff, so a voyage that stepped through would arrive with nothing to tell it where it
   had been going (ADR 0069).
6. **The map never writes to the simulation directly.** It asks `UniverseView` to send an order, the
   one-way seam every screen in this tree keeps.

## 7. Slices

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 1 | [The map, drawn: the screen, the projection, the graph, the fleets, and the rail button that opens it](GalaxyMap-slice-1.md) — **landed**: `GalaxyScreen`, `Neuron::FitBoxIsotropic` and its suite, the rail as the map's one switch | `Outpost`+`NeuronClient` | M | — | — |
| 2 | [Tap to look: the camera flies to the tapped system](GalaxyMap-slice-2.md) — **landed**: a nearest-node hit test through the layout the draw used, and slice 4b's claim tested for the first time | `Outpost` | S | 1 | — |
| 3 | [Tap to go: gate-graph search, a multi-hop fleet order, and the tick that advances it](GalaxyMap-slice-3.md) — **landed**: `RouteAcrossGates`, `FleetOrderKind::Voyage`, `Universe::ConfigureGalaxy` and `StepVoyages` | `GameLogic`+`Outpost` | L | 1 | [ADR 0069](Decisions/0069-a-voyage-lives-on-the-fleet-and-is-planned-from-where-it-is.md) |
| 4 | Systems get names | `GameLogic` | M | 1 | ADR: a system's name is generated from its seed, or it is authored |

Slice 3 was the one with a decision in it, and half of it was forced exactly as this paragraph said:
a fleet is despawned and respawned at every gate it crosses, so anything remembering "three more
hops" that lives on a ship is destroyed by the first hop. The fleet row is the only thing in the
simulation that survives a crossing, which is what makes it the answer — and it is the same reason
`orderGate` is already there.

**What this paragraph did not see is that the row could not answer the question on its own.** A gate
names its far end by `EntityId`; nothing in `Universe` knew what a system was, and neither
composition root nor the publisher held a layout to ask. So the decision the slice actually took was
larger than "where does the route live": it was *what the simulation is allowed to know about the
galaxy*, and the answer is the sites and the links, from the root, unsaved, read by the voyage pass
and by nothing else. ADR 0069 records it with the three alternatives it beat.

## 8. What this will cost that is not obvious

- **The client's galaxy layout is laid out at boot and never again.** That is right today, because
  the galaxy is static. The day a station is built or destroyed, the *layout* still does not change —
  only the universe standing in it — so the map stays correct for free. The day a **gate** can be
  built or destroyed, it does not, and the map needs the graph on the wire. Named here so the day is
  recognised when it comes.
- **Slice 3 put a graph search in `GameLogic`**, which is a new kind of thing there: everything else
  is either a pure layout function or a per-tick pass. It runs on an order and on each arrival, at
  human cadence, over 54 nodes and 68 links. If that ever stops being true it belongs behind the same
  "once, at boot" discipline `LayOutGalaxy` keeps.
- **A voyage across the shipped galaxy is long, and slice 3 measured how long.** The diameter is
  fourteen gates, the mean between two systems is 5.71, and a fleet flies about seven kilometres
  across each system it passes through at its slowest member's cruise. That is minutes per hop and
  the better part of an hour end to end. It is the intent rather than a defect — the point below
  about watching a dot is what it feels like — but it is the first number in this game that makes the
  galaxy's *size* something a player spends time on, and it is the one to look at first if the map
  ever reads as tedious rather than large.
- **A fleet crossing several gates is out of interest for most of the trip**, so the player watches
  it as a dot on this map and as a status block, and not as ships. That is correct and it is also the
  first time the game asks anybody to be satisfied with that.
