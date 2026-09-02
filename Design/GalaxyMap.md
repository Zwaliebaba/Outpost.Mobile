# Galaxy map — the graph as a picture, and a destination as a tap

Status: drafted 2026-09-01, not yet agreed with the owner.

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

- **The rail's `Universe` button already exists**, lights on press, and is read by nothing. That is
  the entry point, already drawn.

What is *not* there: any way to name a system, and any way to send a fleet further than one gate.

## 2. What is being built

- **A modal screen** over the game, opened by the rail's `Universe` button, in `AssemblyScreen`'s
  shape and for its reasons: it holds state the HUD has no business in, it consumes every pointer
  event rather than letting them fall through, and `Hud.cpp` is already eight hundred lines.
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

This costs almost nothing, and the reason is slice 4b: the scenery follows **where the camera is**,
not a jump event, and that slice's own work order named "a galaxy map that flies you somewhere" as
one of the things the choice covered in advance. So the map hands `Camera::SnapGoal` a position and
everything else — the worlds, the rocks, the station marks — already follows. It is worth landing as
its own slice precisely *because* it is the claim slice 4b made without being able to test it.

Nothing about the simulation changes. The player is looking somewhere else, which is not an order.

### 4.3 Go (slice 3)

With a fleet selected, tapping a system orders it there — across as many gates as the route takes.

This is the only part that is not a view. It needs, in `GameLogic`:

- **A search over the gate graph.** Breadth-first over `GateLink`, from the fleet's system to the
  tapped one, giving a hop sequence. The graph is 54 nodes and 68 edges and the search runs on an
  order, not on a tick.
- **An order that survives its own hops.** A fleet crossing a gate is despawned and respawned under
  the same identity (ADR 0056), so *whatever holds the rest of the route cannot be a ship*. It has
  to live on the fleet row, which survives the crossing — the same row `orderGate` already lives on.
- **A tick that advances it.** On arrival, the next hop's gate is found in the new system and the
  fleet is ordered at it, until the last one.

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
4. **A route across gates is planned once, at the order**, not re-planned per hop: the graph is a
   pure function of the layout, which does not change while the fleet is flying. If it ever can, the
   route re-plans on the same terms `AdvanceRoute` re-plans a path (ADR 0059).
5. **A fleet that loses its route stands down** rather than continuing to the next hop. A gate that
   has gone, a hop that no longer connects, or a fleet reduced to nothing are each a reason to stop
   with the fleet somewhere real — the same rule slice 2 of the universe design applied to a single
   jump through a stale gate.
6. **The map never writes to the simulation directly.** It asks `UniverseView` to send an order, the
   one-way seam every screen in this tree keeps.

## 7. Slices

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 1 | The map, drawn: the screen, the projection, the graph, the fleets, and the rail button that opens it | `Outpost` | M | — | — |
| 2 | Tap to look: the camera flies to the tapped system | `Outpost` | S | 1 | — |
| 3 | Tap to go: gate-graph search, a multi-hop fleet order, and the tick that advances it | `GameLogic`+`Outpost` | L | 1 | ADR: a multi-hop route lives on the fleet, because a ship does not survive its own jump |
| 4 | Systems get names | `GameLogic` | M | 1 | ADR: a system's name is generated from its seed, or it is authored |

Slice 3 is the one with a decision in it, and the decision is forced rather than chosen: a fleet is
despawned and respawned at every gate it crosses, so anything remembering "three more hops" that
lives on a ship is destroyed by the first hop. The fleet row is the only thing in the simulation that
survives a crossing, which is what makes it the answer — and it is the same reason `orderGate` is
already there.

## 8. What this will cost that is not obvious

- **The client's galaxy layout is laid out at boot and never again.** That is right today, because
  the galaxy is static. The day a station is built or destroyed, the *layout* still does not change —
  only the universe standing in it — so the map stays correct for free. The day a **gate** can be
  built or destroyed, it does not, and the map needs the graph on the wire. Named here so the day is
  recognised when it comes.
- **Slice 3 puts a graph search in `GameLogic`**, which is a new kind of thing there: everything else
  is either a pure layout function or a per-tick pass. It runs on an order, at human cadence, over 54
  nodes. If that ever stops being true it belongs behind the same "once, at boot" discipline
  `LayOutGalaxy` keeps.
- **A fleet crossing several gates is out of interest for most of the trip**, so the player watches
  it as a dot on this map and as a status block, and not as ships. That is correct and it is also the
  first time the game asks anybody to be satisfied with that.
