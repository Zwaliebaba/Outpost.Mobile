# Work order — NMO slice 5: liveries

Implements slice 5 of [`NmoFormat.md`](NmoFormat.md) §14: the visible half of `RaceTinted`. A hull
stops being tinted as one object and starts being painted by two authorities — the model paints its
structure, the faction paints its livery — and the faction-to-colour branch the client has carried
since Hostiles becomes the table [Stations.md](Stations.md) §9.3 describes.

**Layer:** `NeuronClient` (shaders and `SceneRenderer`), `Outpost`.
**Depends on:** slice 2 (`MeshVertex::race`, `MeshMarker::raceTinted`) and slice 4 (the view holds
exhaust colours, so there is something to multiply).
**Blocks:** nothing. Slice 6 (articulated parts) is independent.

---

## 1. Why this is a slice

Everything before it carried the flag without acting on it: slice 2 reads `RaceTinted` into a
vertex channel and a marker bool, and nothing downstream looks at either. That was deliberate —
a channel that is carried and ignored cannot break a screenshot — but it means the corpus has been
shipping greyscale liveried surfaces since slice 3, and the ships have been drawing in a flat grey
that is *correct and ugly*. This slice is the one where they get their paint back, and it is the
first one whose acceptance is a screenshot rather than a test.

It is also where a rule that has been implicit since the first hull gets stated: **whose colour is
this pixel?** Until now the answer was "55 % the tint's, 45 % the model's, everywhere on the hull,"
which is why a red enemy came out olive (`ViewTuning.h`, the worked example that is about to be
deleted). The answer after this slice is "the model's, unless the material said otherwise, in which
case the faction's" — and it is exact, per surface, at every livery.

---

## 2. Scope

### 2.1 `NeuronClient/Shaders/Scene.hlsli`, `ScenePS.hlsl` — the combine rule

`VsIn`/`VsOut` already carry `race` from slice 2. `ScenePS` reads it:

```hlsl
// Two authorities paint a hull. i.col is the model's own -- plating, glass -- and stands as
// authored. Where the material was RaceTinted (NmoFormat.md 5.5) i.col is a *shade* instead, and
// the livery in i.tint supplies the hue: one multiply, so the faction's colour survives exactly
// and the shade ladder (plate < accent < thruster) survives with it.
float3 albedo = lerp(i.col, i.tint.rgb * i.col, i.race);
```

**Multiply, and not the `lerp` it replaces.** The old rule mixed a tint against whatever the model
was painted, so the two hues argued: `ViewTuning.h` works through a red tint over a green panel
arriving at `#B0A932`, olive, "which is not a red ship". A multiply cannot argue, because the shade
it multiplies has no hue to contribute. This is the decision the ADR below owes an explanation for;
it is the alternative a reasonable person proposes again.

`i.tint.w` is **freed** — it was the material mix and there is no mix any more. It becomes the
selection highlight, §2.2.

### 2.2 `NeuronClient/RenderTypes.h`, `SceneRenderer` — what an instance carries

`MeshInstance::tint` keeps its shape (`float[4]`, offset 64, `sizeof` 80 — both `static_assert`s
stand) and changes meaning: `rgb` is the livery, `w` is a highlight lift in `0..1` applied after
lighting. Restate that at the declaration; it is the same four floats saying something new, which
is exactly the kind of drift a comment is for.

`DrawMesh`'s `Rgba _baseColour, float _materialMix` become `Rgba _livery, float _highlight`. Every
non-hull caller (the ground quad, decals, anything drawing an unflagged mesh) is unaffected by the
rename in behaviour: their vertices carry `race = 0`, so the livery never reaches them.

### 2.3 `Outpost/ViewTuning.h` — the livery table

The three-knob hostile block (`HOSTILE_SHIP_COLOUR`, `HOSTILE_SHIP_MATERIAL_MIX`,
`HOSTILE_ACCENT_COLOUR`) and `SHIP_COLOUR`/`SHIP_MATERIAL_MIX` are **deleted**, along with the
comment that works through the olive. One colour per faction replaces all of it:

```cpp
// A faction's paint. One colour, where an enemy used to need three: the mix is gone with the lerp
// (slice 5), and the accent is gone because the shade ladder already provides it -- a thruster is
// this colour at 1.0 and a plate is it at 0.45, so a livery is authored at the brightness its
// nozzles should burn and the hull falls out of it (NmoFormat.md 13.1).
//
// The first two cannot be chosen. Core Vanguard Command is the government and the Vandal
// Collective is what the government is for; a player wearing either would be lying about who they
// are, and the affordance that would let them is simply absent -- a chooser walks
// SELECTABLE_LIVERIES and structurally cannot reach these two.
inline constexpr Neuron::Rgba LIVERY_VANGUARD{0.24f, 0.52f, 0.95f, 1.0f}; // CVC azure -- reserved
inline constexpr Neuron::Rgba LIVERY_VANDAL{0.92f, 0.26f, 0.22f, 1.0f};   // Vandal red -- reserved

inline constexpr Neuron::Rgba SELECTABLE_LIVERIES[] = {
  {0.54f, 1.00f, 0.14f, 1.0f}, // green -- what every hull in the game wore before liveries existed
  {0.95f, 0.66f, 0.15f, 1.0f}, // amber
  {0.62f, 0.36f, 0.92f, 1.0f}, // violet
  {0.20f, 0.80f, 0.82f, 1.0f}, // cyan
  {0.95f, 0.45f, 0.16f, 1.0f}, // orange
  {0.86f, 0.88f, 0.92f, 1.0f}, // white
};
inline constexpr std::size_t PLAYER_LIVERY_INDEX = 0; // until something can choose one
```

There is no configuration file in this tree (AGENTS.md), so the player's livery is a constant and
the chooser is not this slice. `PLAYER_LIVERY_INDEX` is the seam it will arrive at, and it is an
index into the selectable array rather than a colour precisely so that whatever chooses later
cannot choose a reserved one.

`SELECTED_COLOUR` survives but stops being a hull tint — it is the selection ring and the HUD's
green, which is what it always should have been. The new constant beside it is the lift:

```cpp
inline constexpr float SELECTED_HIGHLIGHT_LIFT = 0.35f; // toward white, on the selected hull
```

### 2.4 `Outpost/WorldView.cpp` — one table, three consumers

**The hull.** `DrawShips`' three-case branch becomes a lookup:

```cpp
Rgba LiveryOf(Game::FactionId _faction, bool _hostileToMe);
```

with the precedence [Stations.md](Stations.md) §9.3 sets and does not get to be re-litigated here:
**hostile outranks faction.** A Vanguard ship whose faction holds this client hostile paints
`LIVERY_VANDAL`'s red, because the law turning on you is the thing the player must see. Own faction
takes `SELECTABLE_LIVERIES[PLAYER_LIVERY_INDEX]`, `FACTION_VANGUARD` takes `LIVERY_VANGUARD`, and
`FACTION_HOSTILE`/`FACTION_VANDAL` takes `LIVERY_VANDAL`. `FACTION_VANGUARD` exists in
`GameLogic/ShipState.h` and nothing spawns one yet; the row is written now so that the day Stations
lands, no client code changes.

**Selection stops being a colour.** It sets `tint.w = SELECTED_HIGHLIGHT_LIFT` and leaves `rgb`
alone. This is not tidiness: under liveries a mint-green selected hull *reads as a different
faction*, and the player's own livery might be mint. Selection is a brightness and a ring; identity
is a hue. The hover lift folds into the same channel.

**The plume.** Slice 4 gave `ExhaustView` an authored colour. Where the marker was `RaceTinted` —
every shipped exhaust is — the view multiplies it by the same livery the hull got, so one authored
plume burns azure, red or the player's own. Where it was not, it draws as authored. Nav lights
never multiply: port red and starboard green are a convention, not a livery, and
[NmoFormat.md](NmoFormat.md) §5.10 says why at more length.

### 2.5 `Outpost/ShipExplosion.cpp`, `NeuronClient/MeshShatter` — the debris matches

`MeshShatter` bakes a shard's colour at spawn from the vertex it came off
(`Lerp(tintColour, vertexColour, tintMix)`). That signature is the old rule, so it changes with it:
`ShatterDesc` takes a livery instead of a tint-and-mix, and the shard applies the same
`lerp(col, livery * col, race)` the pixel shader does, reading `MeshVertex::race` that slice 2 put
there. A shard off a grey hull plate stays grey; a shard off a liveried panel keeps the faction's
paint. Getting this wrong is visible and cheap to check — blow up a Vandal Interceptor and the
debris is red.

### 2.6 The HUD does not change

`HUD_ACCENT_GREEN` and `HUD_ALERT_RED` stay what they are and stay derived from where they are
derived. The minimap answers "friend or enemy", not "what colour is that ship" — a player who picks
a red-ish livery must not turn their own dots into the colour the HUD uses for hostiles, and a
Vanguard station's dot follows §9.3's `HUD_VANGUARD_BLUE` because that is a relation too. Liveries
are a scene language; the HUD is a relation language. Say so at the constants so the next person
does not "fix" the inconsistency.

### 2.7 What this slice deliberately does **not** do

- **No livery chooser.** No UI, no persistence, no wire field. `PLAYER_LIVERY_INDEX` is a constant
  and the seam is named; the feature that sets it is its own design.
- **No emissive.** The thruster material's emissive is still green and still unread (slice 2 §2.7).
  Whichever slice lights it re-authors it as a shade first — and then it is livery-multiplied like
  everything else flagged.
- **No standings.** `hostileMask` arrives with Stations; until it does, `_hostileToMe` is the
  existing "not my faction" test and `LiveryOf` takes it as a parameter so that swapping the source
  is one call site.
- **No content change.** The `.nmo` corpus already carries the flags and the shades; this slice
  reads what is there. If a hull comes out grey, its material lost its flag in `Art/`, and that is
  an asset bug, not this slice's.

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| [`NmoFormat.md`](NmoFormat.md) §5.5, §5.10, §13.1 | The two-authority rule, the marker bit, and the shade ladder the corpus is authored against |
| [`Stations.md`](Stations.md) §9.3 | The faction-to-colour table and the precedence this slice implements |
| `NeuronClient/Shaders/Scene.hlsli` | `race` and `tint` already reach the pixel stage; only `ScenePS`'s one line is new |
| `Outpost/ViewTuning.h` | The block being deleted, and the worked example that says exactly why the lerp lost |
| `Outpost/WorldView.cpp` `DrawShips` | The three-case branch becoming a table |

---

## 4. Acceptance

- **Screenshots**, which are the point of this slice: the player fleet in its livery, the Vandal
  patrol in red, a selected hull showing a lift and not a hue change, a plume in the flying
  faction's colour, and one explosion whose debris matches the hull it came off.
- A grey ship anywhere is a failure, and names its cause: a material that lost `RaceTinted` in
  `Art/`, or a livery that never reached the instance.
- Nav port/starboard lights are still red and green on every faction. Check a Vandal hull
  specifically — that is where a wrongly-liveried nav light hides.
- All four suites green; `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- **A decision record is due**, and it owes two things: that a liveried surface is declared by the
  material rather than recognised by its name, and that the combine is a multiply rather than the
  lerp it replaces. Both are alternatives a reasonable person proposes again — the second one was
  in this tree for a year — so both go in the record with the numbers that decided them
  (AGENTS.md §9).
- `NmoFormat.md` §14 marks slice 5 landed; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **Every liveried material is greyscale.** The corpus is authored that way (§13.1) and the
  multiply assumes it. A hue that survives into a flagged material is an asset bug and shows up as
  a hull in the wrong colour; the loader does not police it, because content errors it can render
  are not errors it should refuse.
- **`race` is 0 or 1, never between.** It is per triangle and constant across one, so no material
  seam interpolates. Nothing needs to handle a half-liveried pixel.
- **One livery per ship.** No per-submesh override, no second colour, no pattern. If a faction ever
  wants two, that is a second flag bit and a second instance channel, and it is not free.
