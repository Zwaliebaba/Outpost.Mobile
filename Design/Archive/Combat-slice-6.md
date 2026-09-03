# Work order — Combat slice 6: the turret turns, and the content it needs

Implements slice 6 of [`Combat.md`](Combat.md) §16, the last one, and the slice that closes the
combat design.

**Layer:** `NeuronClient` + `Outpost` + `Tools/`.
**Depends on:** slices 3 and 4 — a `MeshData` that keeps named submeshes, and a client that already
draws muzzles, tracers and pips off the fire message.
**Blocks:** nothing. When it lands, `Combat.md` moves to `Archive/` with its six work orders.

---

## 1. Why this is the last slice, and why it was cut out of two others

Three things were deferred here, and all three wait on the same missing piece: **a binding from a
mount to the parts of the hull that carry it.**

Slice 4 shipped muzzle flashes, tracers, impacts and condition pips, and cut out **the turret slew**
and **the target bar**: the slew needs a renderer entry point that does not exist — a draw of one
submesh range, and the hull drawn as its own complement — and only a screenshot can accept either
(`Combat-slice-4.md`). Slice 5 measured the pacing targets and cut out **the `Gun` markers** and
**the mount-versus-marker consistency check**, because slice 3 found the muzzle position is already
exact in the art as a submesh's bind-pose centre, so authoring markers before a table reads them
would put a third copy of a number the art and `HullSpec` already carry (`Combat-slice-5.md` §3).

The binding is the piece. It is a **client-side table read off submesh names**, which is where ADR
0002 puts it: content is the renderer's, and a headless server has none of it.

## 2. Scope

1. **`NeuronClient/SceneRenderer` — a submesh-range draw, and the complement.**

   `DrawMesh` draws a whole mesh. This slice adds a way to draw **one submesh range under its own
   world matrix**, and to draw **the rest of the hull** — the complement of the ranges a caller is
   posing itself. A hull with a turning turret is then two or three draws instead of one: the
   complement at the hull's matrix, each posed part at its own.

   The complement is the awkward half and it is the reason this is a renderer change rather than a
   caller's. A submesh is a contiguous vertex run (`MeshData.h`), so the complement of *k* posed
   ranges is at most *k+1* runs, and the entry point takes them as a span rather than making every
   caller compute them. A hull with no posed part is one draw, exactly as now, and pays nothing.

2. **`NeuronClient` — the mount-to-part table.**

   A table binding mount index to submesh name hash, per hull, read at load time off the names the
   art already carries (`battleship_turret_0` and its siblings). It lives in the client for ADR
   0002's reason and is authored beside the mesh library rather than in `GameLogic`, which must not
   learn what a hull looks like.

   A hull whose art has no turret submesh binds nothing and draws exactly as it does today. That is
   most of the roster, and it is why this slice cannot regress a hull it does not touch.

3. **`Outpost` — the slew.**

   Each mount's part turns toward the last target that mount was seen firing at, at the device's
   `traverseRadPerSec`, and drifts back to rest when there is nothing. **Presentation only**: it is
   driven from the fire message the client already receives, it feeds nothing back, and a mount
   whose aim the simulation settled is not consulted — the server decided the shot landed and this
   is a drawing of it (§10.2). A fixed mount does not slew, which falls out of `Fixed()` and needs
   no branch of its own.

4. **`Outpost` — the target bar** (§10.3, cut out of slice 4). A thin condition bar on the ordered
   target's selection bracket, and the HUD stat panel's `HULL` bar finally reading the record's
   `hullFraction` instead of the hard-coded whole it shows today.

5. **`Tools/` and the art — the `Gun` markers, and the check that they agree.**

   Author a `Gun` marker per mount on the hulls that carry turrets, at the position the binding now
   reads, and extend `Tools/NmoShippedArtTest.py` to assert **that the markers and `HullSpec`'s
   mount table agree** — the consistency check slice 5 deferred. It needs to see the simulation's
   table and the game's art at once, and §3 below decides how.

6. **Prose in the same commit**: `Combat.md` §16's slice 6 entry becomes what landed, §10.1's "start
   turning in slice 6" becomes the present tense, and §10.2's note that the slew "is slice 6 for
   exactly that reason" says what the entry point turned out to be.

7. **A decision record** for the mount-to-part table: a client table read off submesh names, and why
   not a marker, a bone or a field in `HullSpec`.

## 3. The one choice this order leaves open, and how to close it

The consistency check needs `HullSpec`'s mount table, which is a C++ `constexpr` table, and the art,
which is NMO under `Outpost/Assets/Meshes/`. `Tools/` is stdlib Python and sees the art but not the
table. Slice 5 named the two candidates and did not choose:

- **Parse `HullSpec.h`** from the Python check. No build step, no generated file, and a parser for a
  C++ aggregate that will break the day the table's shape changes.
- **Generate a small table** the check reads. Robust to the header's formatting, and it adds a
  generated file to a tree whose rule is that nothing generated is committed (AGENTS.md §3).

**Take the parse, and bound it**: the check reads only `HULL_SPECS`' mount bearings and counts, it
fails loudly rather than silently when it cannot find them, and the rule it enforces is stated in
`HullSpec.h` beside the table so the day the shape changes the reason to look is next to it. A
generated file would be a second source of truth for a table that is already the only one, which is
the thing ADR 0058 exists to refuse — and the parser's fragility is a red check rather than a wrong
answer.

## 4. How it must behave

1. A hull with no bound part draws in exactly one call, and no frame it appears in changes.
2. A bound part turns at its device's traverse rate, never faster, and rests when the mount has no
   remembered target.
3. Nothing here reaches the simulation: no order, no message, no field on any snapshot. Turn the
   slew off and the same shots land on the same ticks.
4. The `HULL` bar and the target bar read `hullFraction` off the record, and a target this client
   holds no record for draws an outline rather than a full bar — `Combat.md` §10.3's rule for pips,
   applied to the one bar that did not have it.
5. The art check fails when a marker and its mount disagree, and says which hull and which mount.

## 5. Acceptance

- **Screenshots at two window sizes** — a fight and a quiet frame — which are what accepts this
  slice *and* what pays the debt slice 4 left. `Combat.md` §16 records them as owed against slice 4;
  this order is where they come due.
- `NeuronClientTests`: the complement of a set of submesh ranges is the ranges the mesh does not
  cover, over an empty set, one range, adjacent ranges and the whole mesh.
- `GameLogicTests` unchanged and green, which is the claim that nothing simulated moved.
- `python Tools/NmoShippedArtTest.py` passes and fails on a planted disagreement.
- `python Build/CheckProjectFiles.py`, `python Build/CheckFormat.py`, clang-tidy over the changed
  sources.
- The decision record is in `Design/Decisions/` and indexed.
- **`Combat.md` moves to `Design/Archive/` with its six work orders in this commit**, every slice
  landed, and every citation to them is retargeted in the same commit.

## 6. Assumptions the implementer may make

- **Bones and clips stay skipped.** No shipped hull has a rig (§10.1); a turret turns about its
  bind-pose centre, and the day a rigged hull is authored is the day a slice poses it.
- **A turret and its barrels are separate submeshes** and are turned together by the binding, which
  is why the table binds a mount to a *set* of names rather than one.
- **The slew is not in the replay contract** and never enters it. If a later design wants a turret's
  angle to decide a shot, that is a simulation field and a format bump, and it is not this.

---

## 7. What changed on contact, and what is deliberately not here

**This commit is items 1 to 4 and item 7. Items 5 and 6 are not in it, and the design does NOT move
to `Archive/`, because the slice is not finished.** §5's last line says the move happens when every
slice has landed; this one has not.

- **The mount-to-part table is in `Outpost`, not `NeuronClient`** (§2.2). It names `Game::HullId`, and
  `NeuronClient` may not list `GameLogic` (AGENTS.md §3) — the order asked for something the layer
  rule forbids. `Outpost` is the composition root, sees both layers, and already holds the
  hull-to-mesh table this is the sibling of; a headless shard still has none of it, which is the half
  of ADR 0002 that mattered. [ADR 0064](../Decisions/0064-a-mount-is-bound-to-its-art-by-a-client-table-of-submesh-names.md)
  records it.
- **A posed hull leaves the instanced path, and that cost is bounded twice.** §2.1 said "two or three
  draws instead of one" and did not say what that does to `MmoScalabilityReview` G2's instancing: an
  instance carries one matrix, so a hull whose parts each need one cannot be in the bucket at all.
  Two bounds, both in `ViewTuning.h`: a turret within `TURRET_STOWED_RAD` of rest counts as stowed and
  stays instanced — which is every hull in the game outside a fight, and is free accuracy, since a
  stowed turret draws identically either way — and at most `MAX_POSED_HULLS` hulls pose in one frame,
  taken **nearest first**. Nearest rather than in submission order, because a hull flickering in and
  out of the cap as another ship moved would flicker its turrets with it.
- **`ConditionColour` moved from `FleetSheet.cpp` to `ViewTuning.h`.** The bracket bar reads the same
  condition as the sheet's pips and must read it in the same colours; a second copy would have been
  one condition meaning two colours depending on where it was drawn.
- **The `HULL` bar reads the selection's MEAN, not one ship's.** §2.4 said "the record's
  `hullFraction`" and the panel describes a selection, which is usually more than one ship. A minimum
  would read as the fleet being in worse shape than it is the moment one Interceptor is scratched; a
  sum would mean nothing. One selected ship is its own mean, which is the common case.
- **The ordered target is remembered client-side, because nothing carries it back.** §2.4 asked for a
  bar on "the ordered target's" bracket. The fleet status block says a fleet is *attacking*, never
  what — `Combat.md` §9.1 withholds the target's identity with the rest of the intent — so the only
  thing that can name it is the order this client sent. It is dropped the moment the record leaves the
  interest set, rather than left pointing at whichever ship inherits the slot.
- **Nothing simulated moved.** No order, no message, no field on any snapshot; the slew runs on the
  view's real-time clock beside the tracer's, for ADR 0053's reason. `GameLogicTests` is untouched.

## 8. The question this slice cannot answer for itself

**Item 5 — the `Gun` markers and the mount-versus-marker check — is blocked on what a marker is
supposed to correspond to, and the shipped art and `HullSpec` currently answer differently.**

§2.5 says "author a `Gun` marker per mount, at the position the binding now reads", and §4.5 says the
check "fails when a marker and its mount disagree". Neither is well defined against what is actually
in the tree. Measured, per hull, mounts from `HullSpec` and markers from the NMO:

| Hull | Mounts (`HullSpec`) | Turret submeshes | `Gun` markers | Named |
|---|---|---|---|---|
| Interceptor | 1 × LightGun (fixed) | `interceptor_railgun` ×2 | 2 | `GunA`, `GunB` |
| Bomber | 1 × StrikeCannon (fixed) | — | 0 | — |
| Corvette | 2 × LightTurret | `corvette_turret` ×2 | **0** | — |
| Frigate | 2 × MediumTurret, fore and aft | `frigate_battery` ×2 **abeam**, `frigate_lance` ×2 **forward** | 2 | `GunA`, `GunB`, on the lances |
| Battleship | 3 × HeavyTurret, **2 × LightTurret** | `battleship_turret_0..2` (+ barrels) | 6 | `GunA`..`GunF`, two per turret |
| Carrier | 4 × LightTurret | **none** | 0 | — |
| Fighter | (not in `HULL_SPECS`) | `fighter_pod` ×2 | 2 | `PodGunA`, `PodGunB` |

Three things fall out, and each changes what the check can be:

1. **A marker is not one per mount.** The Battleship carries two markers per turret, one per barrel,
   which is right for a muzzle flash and means marker count can never equal mount count.
2. **A marker's name does not say which mount it belongs to.** `GunA` is a letter, so nothing joins a
   marker to a mount except position, and position does not do it either — the Battleship's three
   turrets sit at 0° and ±64.5° *by position* while their mounts bear 0° and ±120°, because a mount's
   bearing is which way it faces and not where it sits.
3. **Six mounts in the shipped roster have no art at all** — the Battleship's two light turrets and
   all four of the Carrier's. `Combat.md` §12 admits this in passing ("mounts the shipped art can
   *mostly* already wear"), and §3.1 permits it ("a mount without an authored marker draws its effects
   from the hull's origin"). So a check that demands a marker per mount fails the shipped game by
   design, and one that does not demand it cannot catch a mount that lost its marker.

The choice is the owner's because it decides how much work item 5 is, and the range is wide:

- **Rename to bind.** Markers become `Gun<N>` (or `Gun<N>A`/`Gun<N>B` for twin barrels), where N is
  the mount. The check then reads: every marker names a mount the hull has, no two name the same one
  with the same suffix, and a mount without a marker is allowed and draws from the origin. Cheapest:
  it renames markers in four files and authors two on the Corvette. It also makes the markers
  *load-bearing* — the muzzle flash could finally come off the marker instead of the hull origin,
  which is what `Combat.md` §10.2 has always said it should.
- **Reconcile the art with the table.** Move the Frigate's mounts abeam to match its batteries, or
  move the batteries fore and aft to match the table, and author light-turret geometry for the
  Battleship and the Carrier. Honest, and much more work — it is modelling, and `HullSpec`'s bearings
  are simulation content that changes recorded outcomes.
- **Check only what is true today**: a hull with no mounts carries no `Gun` marker, a hull with mounts
  carries at least one, and every marker sits inside its own mesh's bounds. Cheap and weak; it would
  not have caught the Frigate.

The first is the recommendation: it is the only one that makes the marker mean something the client
reads, and it leaves the geometry question — the Carrier's four missing turrets — where it belongs,
in art work that is not this slice's.

## 9. What was verified, and how — and the honest gap

`RangeComplement` was run against every case its suite asserts, and each answer matched to the run:
nothing posed gives the whole mesh, one run in the middle gives two gaps, a run at either end gives
one, the whole mesh gives none, adjacent runs merge rather than leaving an empty draw between them,
three runs handed over backwards give the same four gaps as forwards, an overlapping or nested pair
merges, an empty or past-the-end run covers nothing, and an empty mesh has no complement. The
property under all of it — posed runs plus gaps tile the mesh exactly once — was checked as a census
per vertex and holds in every case where the caller's own runs do not overlap.

`SlewMount` and `MountBearingToward` were run against the shipped device table:

```
HeavyTurret (18 deg/s, +/-150 deg), target dead abeam: wants 90.0 deg
  aim after 1.00 s = 18.00 deg -- the rate exactly, never more
target dead astern for a +/-150 turret: clamps to the arc edge, not through the hull
a bow gun (+/-20 deg) asked for a target abeam: clamps to 20.0 deg
the hold runs out at 0: 90 -> 72 -> 54 -> 36 -> 18 deg, home at the same rate it left
a fixed mount asked to bear, held for five seconds: 0.00 deg
```

**The gaps, and there are three.** No screenshot: nothing in this container has MSVC, D3D12 or a
window, so §5's acceptance — a fight and a quiet frame at two window sizes, which also pays slice 4's
debt — is unpaid and stays unpaid until someone runs it on Windows. Nothing in `Outpost` has a test
suite, so `ResolveMounts`, the slew and the bars are verified by running the arithmetic beside the
tree rather than in it — the same gap `TickStats` and `GalaxyScreen` have. And `SceneRenderer`'s two
new entry points are D3D12 and were not compiled here at all; the half of them that can be wrong
silently is the complement, which is why it is in `MeshData.h` and in the suite.

---

## 10. The owner's answer, and the back half of the slice

**Put on 2026-09-02, answered the same day, both as recommended.**

> **The marker rule: rename to bind.** A `Gun` marker names its mount — `Gun<N>` for a single muzzle,
> `Gun<N><letter>` for one of several on mount N.
>
> **The Frigate's mounts: the batteries.** `frigate_battery` and `frigate_battery.001`, not the
> lances — they are the parts named like guns and they sit where a traversing turret belongs.

That settles the rule against the shipped art rather than the other way round, and it is the only one
of the three options that makes a marker mean something a consumer can *read*: `MeshMarker` keeps a
name as a hash and cannot parse one, so `Gun<N>` is the only shape the client can look up. Which is
what let the muzzle flash finally come off the gun.

### 10.1 What was authored

| Hull | Before | After |
|---|---|---|
| Battleship | `GunA`..`GunF`, letter-named, two per turret | `Gun0A`/`Gun0B`, `Gun1A`/`Gun1B`, `Gun2A`/`Gun2B` — positions unchanged, matched to mounts by the barrel each sits on |
| Interceptor | `GunA`, `GunB` | `Gun0A`, `Gun0B` — one fixed mount, two railgun muzzles |
| Frigate | `GunA`, `GunB` at the **lance** tips, both owned by the port battery's marker run | `Gun0`, `Gun1` at the batteries' outboard faces, one on each battery's own run |
| Corvette | none | `Gun0`, `Gun1` authored on each turret's forward face at its own centre height |
| Fighter | `PodGunA`, `PodGunB` | unchanged — there is no `Fighter` `HullId`, so the check reports it as unchecked rather than passing it |
| Bomber, Carrier | none | none — allowed: they draw from the origin, per `Combat.md` §3.1 |

`NmoFormat.write` round-trips all eleven shipped meshes **byte-identically** before the edit, which is
what makes each diff exactly the marker change and nothing else. Verified again after.

### 10.2 The check, and what it can hold

`Tools/NmoShippedArtTest.py` parses `HULL_SPECS`' mount counts — the parse §3 chose over a generated
table, and every step of it raises rather than defaulting, so its fragility is a red check and not a
wrong answer. It reads the `HullId` enum for row order, the `LOADOUT_*` constants for their counts,
and the loadout named in each row of the `HULL_SPECS` initialiser. Against the header today:

```
Interceptor 1  Bomber 1  Corvette 2  Miner 0  Frigate 2
Hauler 0  Battleship 5  Carrier 4  Stargate 0  Structure 0
```

The rule it enforces is the *reverse* of "a marker per mount", and that is deliberate. A mount may
carry several muzzles and a mount may carry none, so neither direction of a count comparison is true.
What can be held is: **every marker names a mount its hull has, no two claim the same muzzle of the
same mount, and every marker's name is of the shape.** A mount with no marker is reported, not failed.

Planted each of the three failures against the Corvette and confirmed the check catches them and
exits 1:

```
Gun0 -> GunZ   FAILED: Gun marker 'GunZ' does not name a mount -- the name is Gun<N> or Gun<N><letter>
Gun1 -> Gun7   FAILED: Gun marker 'Gun7' names mount 7, but the hull carries 2
Gun1 -> Gun0   FAILED: Gun markers 'Gun0' and 'Gun0' claim the same muzzle of mount 0
```

and its clean run reports the four things that are true and unfailable:

```
Battleship.nmo: mount(s) 3, 4 carry no Gun marker and draw from the hull origin
Bomber.nmo:     mount(s) 0 carry no Gun marker and draw from the hull origin
Carrier.nmo:    mount(s) 0, 1, 2, 3 carry no Gun marker and draw from the hull origin
Fighter.nmo:    2 Gun marker(s), but no HullId of that name -- nothing to check them against
```

### 10.3 The disagreement this leaves, stated rather than fixed

**On both the Corvette and the Frigate, `HullSpec` bears the two turret mounts fore and aft (0 and
π) while the art puts both turrets port and starboard.** It is one systematic disagreement, not two
quirks: the same `{0, π}` pair is authored for both hulls and neither hull's art was built to it.

With a ±150° arc it changes no engagement — a port turret resting forward can still bear on nearly
anything — so what it costs is where a *stowed* turret points. Correcting it means moving mount
bearings, which are simulation content: every one of them changes recorded outcomes, and the matchup
matrix (slice 5) would have to be re-measured against the new table. That is a slice, not a line, and
it is owed rather than done here.

### 10.4 What the muzzle change actually did

`ResolveMuzzles` hashes the names a mount's muzzles may carry and looks those up, because the client
cannot read a marker's name — only its hash. A shot then draws from its mount's muzzle, **carried
round by that mount's current aim**, using the same pivot-rotate the posed draw uses applied to one
point instead of a vertex run. A hull that authors none is unchanged and draws from its origin.

One defect was found and fixed while wiring it: `ShipView::mounts` holds only the mounts the art binds
and the device turns, so it is **dense where mount indices are not** — the Battleship binds 0, 1 and 2
of five. Indexing it by a mount id worked on every shipped hull, because the bound mounts happen to be
a prefix on all of them, and would have pointed at the wrong turret the first time one was not.
`MountView` carries its own index now and `FindMount` looks it up. This is slice 3's `FleetInSlot` bug
in a different costume, caught before CI rather than by it.

Resolved against the shipped art, at load, as the client will:

```
Corvette:   2 of 2 mounts bound  | mount 0 pivot (-1.93, 2.53, 0.70) rest    0 deg, 180 deg/s
                                 | mount 1 pivot ( 1.93, 2.53, 0.70) rest  180 deg, 180 deg/s
            2 muzzles, one per mount, on each turret's forward face
Battleship: 3 of 5 mounts bound  | mount 0 pivot (0.00, 13.18, 19.50) rest    0 deg
                                 | mount 1 pivot (-9.75, 13.18, 4.65) rest  120 deg
                                 | mount 2 pivot ( 9.75, 13.18, 4.65) rest -120 deg
            6 muzzles, two per bound mount; FindMount(3) is null, as it must be
            draws as 5 posed runs + 2 gaps: [0,+2000) [2400,+600)
```

### 10.5 The gap that remains

The screenshots. §5 asks for a fight and a quiet frame at two window sizes, which is what accepts a
screen, and nothing in this environment has MSVC, D3D12 or a window. The owner made CI-green the gate
on 2026-09-02 with Windows-only manual checks waived; `Combat.md`'s header records them as owed rather
than dropping them, and this is where that debt comes to rest.

### 10.6 What CI caught that nothing here did

Run 249 failed the build with two errors, both the same one:

```
Outpost\UniverseView.cpp(936,38): error C2668: 'Outpost::FindMount': ambiguous call to overloaded function
Outpost\UniverseView.cpp(951,28): error C2668: 'Outpost::FindMount': ambiguous call to overloaded function
```

`FindMount` was a const overload taking `std::span<const MountView>` and a mutable one taking
`std::span<MountView>`. A `std::vector<MountView>&` converts to both at the same rank, so **every
call to it was ill-formed** — and it is not an MSVC quirk: clang rejects the same three lines, which
was checked afterwards rather than assumed.

The reason it reached CI is worth more than the fix. `HullParts.cpp` — where the pair was *defined* —
compiled clean at `-Wall -Wextra` here, and a definition of an ambiguous overload set is perfectly
well-formed. Both **calls** live in `UniverseView.cpp`, which needs D3D12 and cannot be compiled off
Windows at all, so nothing in this environment ever instantiated the shape that was broken. Checking
the definition proved exactly nothing about the calls, and the report said "checked".

The fix is one function returning an **index** rather than a pointer: `MountSlotOf`, with
one-past-the-end for "not found", which is the idiom `UniverseView::IndexOfEntity` already uses. No
overload set, so nothing to be ambiguous about.

The gap is closed by a scratch translation unit that lifts the two call shapes out of
`UniverseView.cpp` and compiles them against `HullParts.h` — a `vector<T>&` argument and a
`const vector<T>&` one — and exercises the sparse case the index exists for: mounts 0 and 2 bound
with 1 unbound, aiming at mount 2 finding slot 1, and aiming at unbound mount 1 doing nothing.

**The standing lesson, and it is the second time this branch has paid it**: compiling the file that
declares a thing is not compiling the code that uses it. Slice 3 lost a CI run to
`FleetInSlot(FactionId, ...)` converting silently, and §10.4 above records catching that same class
of bug by hand in this slice. This one got through because the harness stops at the layer boundary
`Outpost` sits on, and only CI compiles past it.
