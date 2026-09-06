# 0071 — Looking at a system costs presence

Status: accepted
Date: 2026-09-06

## Context

[`GalaxyMap.md`](../GalaxyMap.md) §4.2 landed the map's tap-to-look with one sentence of
justification: *"Nothing about the simulation changes. The player is looking somewhere else, which is
not an order."*

That sentence is false, and it is the whole of this record. `OutpostApp::HandleInput` pushes
`SetViewCentre` from the camera target every frame, and `Publisher` sends a subscriber every record
inside its interest circle about that centre. Moving the camera *is* asking the server to describe
somewhere. With `CameraInterestRadiusMetres` capping at half a sector, a camera dropped on a star is
told about everything within 4 096 m of it — which at a gate is the camp sitting on it.

So the map's tap was a free, server-sanctioned scout. The owner's objection is a game-design one and
it is the right one: a player who can look into any of fifty-four systems for nothing can never be
ambushed, a gate camp can never be laid, and the hull you would otherwise have sent to find out is
worth nothing. Intel that costs nothing is intel nobody trades, and this game is aimed at a scale
where trading it is most of the point ([`GameDesignReview.md`](../GameDesignReview.md) §1).

## Decision

A tap on the galaxy map with nothing selected flies the camera only to a system this client has eyes
in: one holding one of its own fleets, or the one the camera already stands in. Any other system
refuses — the map closes, as it does for every tap that names a system, and the event log says
`NO EYES THERE`.

Presence is read off the fleet status block, which is stamped for all five slots on every update
whether or not a member is inside the interest circle, so the answer is right for a fleet a galaxy
away (`UniverseView::FleetPosition`). `Game::SystemAt` is nearest-star and a fleet is always in some
system, since a gate despawns and respawns it rather than leaving it in between (ADR 0056). The
system the camera is already in counts as presence by definition: a player whose last fleet died must
still be able to re-centre on what they are looking at.

## Alternatives considered

- **Remove the flight outright** — "tapping a system with nothing selected does not fly the camera
  there", which is how the rule was first put. It matches its own reason less well than the reason
  does: the objection is to looking where you have nothing, not to looking. The map already draws
  each of your fleets as its slot digit, so removing the flight leaves a fleet you can see on the map
  and cannot look at, and sends the player back to panning — which reaches the same place more slowly
  and is not gated at all.
- **Leave the map alone and fix it server-side only.** Right eventually, and not sufficient on its
  own: this tree's rule is twice on purpose — affordances tell the truth, and clients are not trusted
  (`Design/Archive/Stations.md` 9.2, the dock refusal). An affordance that offers a scout the server
  would refuse is a lie either way round.
- **Gate on "are there any fleets in that system".** Not answerable, and it is worth saying why so it
  is not proposed again: a client is told about nothing outside its interest circle, so for any
  remote system the honest answer is always "none I have been told about". Gate on that and the
  camera could never leave. "Do *I* have a fleet there" is a different question and is answerable,
  which is why it is the one above.
- **Count a station's ledger as presence too.** The honest superset — ships docked in a system are as
  much a reason to look as ships flying in it — and not answerable today: a ledger is a request and a
  reply, per station, on demand (ADR 0051), not something the client holds. Left out rather than
  approximated, and named in the consequences so it is a known gap and not an oversight.

## Consequences

- **The hole is narrowed, not closed, and this record must not be read as closing it.** The camera's
  target is unbounded: `Camera::Update` clamps pitch, zoom and field of view, and nothing clamps
  where the target may be. `PanByGround` moves it by the drag, so a player who drags far enough
  arrives in the next system with the interest circle in tow. The affordance now tells the truth; the
  seam still does not enforce it.
- **The server half is owed, and it already has a shape.** `ShardSimulation::Step` derives a
  session's centre from `Universe::TryCentreOfOwnedFleets` rather than taking one from the client —
  which is this rule, enforced. The in-process root is the one that reads a camera instead, and
  `Outpost/UniverseSimulation.h` says why it may: it holds both halves and is not a network peer.
  [`ShardServer.md`](../ShardServer.md) slice 5 hands the session a camera, and **this record is a
  constraint on that slice**: what a client says it is looking at must be clamped to that client's
  presence before it becomes an interest centre, or slice 5 ships this leak to a server that does
  have network peers.
- A player with no fleets anywhere can look only at the system the camera is in. That is the correct
  reading of the rule and not an edge case to soften: a player with nothing in space has nothing
  looking for them.
- Nothing about the simulation changes — and unlike the sentence this record replaces, that is true
  here. No message, no wire field, no tick. The refusal is a client affordance and a log line.
- `Outpost` has no test suite (AGENTS.md 2), so this is a code read and a screenshot, like every
  other change in this layer.
