# Work order — Spaceship explosion slice 4: the recipe, the trigger, and F4

Implements slice 4 of [`SpaceshipExplosion.md`](SpaceshipExplosion.md) §14: a ship that vanishes
from the snapshot explodes where it was last drawn — three hull shatters, a fireball, a ring of
smoking debris, and a camera shake — with every number in `ViewTuning.h` and a debug key to make
it happen.

**Layer:** `Outpost` only. No `GameLogic` file, no engine file.
**Depends on:** slices 1–3.
**Blocks:** nothing.

---

## 1. What this slice is for

Slices 1–3 built the parts and none of them knows what a ship is. This slice is the one that
does: it holds the source's `Building::Destroy` + `Location::Bang` recipe as Outpost code
(design §1, §3), detects the despawn in the one place that can (design §9), and feeds the
renderer in the order design §8.3 gives. It is also the first time any of it is on screen, so its
screenshots are slice 3's evidence too.

---

## 2. Scope

### 2.1 `Outpost/ViewTuning.h` — a new section

Between "order markers" and "banking and thrusters", in the file's style, every number the
effect uses. The values are the source's (design §5.4, §6.1), metres and seconds, and are the
same numbers slice 2's `Desc` defaults carry — this is where they become *named*:

```cpp
// --- ship explosion ----------------------------------------------------------------------------
// The source values from Interstellar Outpost's Building::Destroy, read as metres and seconds and
// scaled per ship by max(halfExtents) / EXPLOSION_REFERENCE_HALF_SIZE (Design/SpaceshipExplosion.md 3).
inline constexpr float EXPLOSION_REFERENCE_HALF_SIZE = 10.0f;
inline constexpr float EXPLOSION_INTENSITY = 100.0f;           // Building::Destroy(_intensity)
inline constexpr int EXPLOSION_HULL_COPIES = 3;
inline constexpr float EXPLOSION_HULL_FRACTION = 1.0f;
inline constexpr float EXPLOSION_FRAGMENT_LIFETIME_SEC = 5.0f;
inline constexpr float EXPLOSION_FRAGMENT_RADIAL_SPEED = 3.0f;
inline constexpr float EXPLOSION_FRAGMENT_MAX_ANG_VEL = 4.0f;
inline constexpr float EXPLOSION_FRAGMENT_FRICTION = 0.05f;
inline constexpr float EXPLOSION_FRAGMENT_ROT_FRICTION = 0.2f;
inline constexpr float EXPLOSION_FRAGMENT_MIN_CIRCUMFERENCE = 6.0f;   // scaled
inline constexpr int EXPLOSION_FRAGMENT_CAP = 500;
inline constexpr float EXPLOSION_CORE_SIZE_MIN = 120.0f;             // scaled; Bang's cores
inline constexpr float EXPLOSION_CORE_SIZE_RANGE = 120.0f;           // scaled
inline constexpr float EXPLOSION_CORE_SPEED_XZ = 30.0f;              // scaled; Signed(30) on X and Z
inline constexpr float EXPLOSION_CORE_SPEED_UP_MIN = 10.0f;          // scaled
inline constexpr float EXPLOSION_CORE_SPEED_UP_RANGE = 10.0f;        // scaled
inline constexpr float EXPLOSION_CORE_LIFT = 0.3f;                   // × range, along up
inline constexpr float EXPLOSION_EXTRA_CORE_SIZE = 100.0f;           // scaled; Destroy's own cores
inline constexpr float EXPLOSION_EXTRA_CORE_SPEED = 100.0f;          // scaled; Signed(100) on all axes
inline constexpr int EXPLOSION_DEBRIS_MIN = 2;
inline constexpr int EXPLOSION_DEBRIS_MAX = 30;
inline constexpr float EXPLOSION_DEBRIS_SPEED_MIN = 20.0f;           // scaled
inline constexpr float EXPLOSION_DEBRIS_SPEED_RANGE = 30.0f;         // scaled
inline constexpr float EXPLOSION_DEBRIS_UP_MIN = 2.0f;               // the ring's tilt, before normalising
inline constexpr float EXPLOSION_DEBRIS_UP_RANGE = 2.0f;
inline constexpr float EXPLOSION_DEBRIS_SIZE_MIN = 20.0f;            // scaled
inline constexpr float EXPLOSION_DEBRIS_SIZE_RANGE = 20.0f;          // scaled
inline constexpr float EXPLOSION_DEBRIS_LIFT = 0.2f;                 // × range, along up
inline constexpr bool EXPLOSION_PLUME_ALONG_UP = true;               // false: the ring is random on a sphere
inline constexpr std::uint32_t EXPLOSION_PARTICLE_CAPACITY = 4096;
```

No shake constant: `Camera::Shake()` takes no amplitude (`Camera.h:62`) and its amplitude is
already `CAMERA_SHAKE_AMPLITUDE`. The death reuses `TriggerCameraShake()` as it is.

"scaled" means multiplied by `hullScale` at spawn. Speeds and sizes scale; lifetimes, friction and
counts do not. That is the rule design §3 gives and the comment in the file repeats it.

### 2.2 `Outpost/ShipExplosion.h/.cpp`

The recipe, and the state of one dead ship's effect. Namespace `Outpost`.

```cpp
class ShipExplosion
{
public:
  struct Spawn
  {
    const Neuron::MeshData* mesh = nullptr;
    DirectX::XMFLOAT4X4 world;                 // the hull's last drawn matrix, bank and hover included
    DirectX::XMFLOAT3 velMetresPerSec;         // (sin h, 0, cos h) * speed at death
    DirectX::XMFLOAT3 halfExtents;             // ShipView's, for hullScale
    std::uint64_t seed;                        // handle.slot << 32 | handle.generation, mixed with the tick
  };

  // Shatters the hull EXPLOSION_HULL_COPIES times and emits every particle Bang and Destroy would,
  // into the shared pool. Nothing here is per frame; it runs once.
  void Start(const Spawn& _spawn, Neuron::SpriteParticles& _particles);

  // True when every shatter has expired. Particles outlive the object in the shared pool.
  bool Advance(float _dtSec);

  void BuildFragments(std::vector<Neuron::FxVertex>& _out) const;

private:
  std::array<Neuron::MeshShatter, EXPLOSION_HULL_COPIES> m_shatters;   // fixed: the count is a constant
  Neuron::Pcg32 m_rng;
};
```

`Start` is `Building::Destroy` + the visual half of `Location::Bang`, in this order, with
`range = EXPLOSION_INTENSITY`, `damage = range / 4`, `up = (0, 1, 0)` (the hull's local up
after banking is a detail nobody will see; world up is the plume's axis, as the source),
`pos = world's translation`, and `hullScale = max(halfExtents.x, .y, .z) /
EXPLOSION_REFERENCE_HALF_SIZE` clamped to `[0.25, 8]`:

1. **Hull ×3**: for each copy, a `MeshShatter::Desc` from the `EXPLOSION_FRAGMENT_*` constants
   with `minCircumferenceMetres × hullScale`, `Spawn(mesh, world, vel, desc, m_rng)`. The three
   copies draw from the same `m_rng` in sequence, so they tumble differently, which is the
   point of three copies (design §3). Trace the summed dropped count once if non-zero.
2. **Bang cores**: `numCores = int(range × damage × 0.01)` = 25, `+= rng.Below(numCores + 1)`
   → 25..50. Each: `vel = (Signed(30), 10 + Float01() × 10, Signed(30)) × hullScale + shipVel`,
   `size = (120 + Float01() × 120) × hullScale`, position `pos + up × range × 0.3 × hullScale`,
   type `Core`.
3. **Bang debris**: `numDebris = clamp(int(range × damage × 0.01), 2, 30)` = 25. For `i` in
   `0..numDebris`: `angle = 2π i / numDebris`; `dir = (cos angle, 0, sin angle)` — a ring in the
   ground plane, which is `RotateAroundAxis((1,0,0), up, angle)` with `up = +Y`; `dir.y = 2 +
   Float01() × 2` if `EXPLOSION_PLUME_ALONG_UP`, else `dir` is a random unit vector; `vel =
   normalize(dir) × (20 + Float01() × 30) × hullScale + shipVel`; `size = (20 + Float01() × 20)
   × hullScale`; position `pos + up × range × 0.2 × hullScale`; type `Debris`.
4. **Destroy's extra cores**: `int(EXPLOSION_INTENSITY / 4)` = 25 of them, `vel = (Signed(100),
   Signed(100), Signed(100)) × hullScale + shipVel`, `size = 100 × hullScale`, at `pos`, type
   `Core`.

The random-unit-vector branch, when it is taken, is the standard `z = Signed(1)`,
`φ = Float01() × 2π`, `r = sqrt(1 − z²)`, `(r cos φ, z, r sin φ)` — written inline in the `.cpp`,
not added to `Pcg32` (slice 1 said so).

### 2.3 `Outpost/WorldView` — the trigger, the pool, and the draw

`ShipView` gains two fields, both written in `Render` each frame the ship is drawn:

```cpp
DirectX::XMFLOAT4X4 lastWorld;                          // what DrawMesh was given
DirectX::XMFLOAT3 lastVelMetresPerSec{0.0f, 0.0f, 0.0f}; // (sin h, 0, cos h) * ship.speed
bool drawn = false;                                      // false until the first Render
```

`WorldView` gains: `std::vector<ShipExplosion> m_explosions`, `Neuron::SpriteParticles
m_particles` (initialised in `Init` with `EXPLOSION_PARTICLE_CAPACITY`), `Neuron::FxRenderer*
m_fx` (set by a new `SetFxRenderer`, the way `SetTracker` is), and two scratch vertex vectors
`m_fxFragmentVerts`, `m_fxSpriteVerts` reserved once.

**The trigger, in `ApplySnapshot`.** After the carry loop, every `m_carryScratch[at]` whose
`m_carryHandles[at]` was not matched is a ship that vanished. The loop today finds matches from
the new snapshot's side; add a `std::vector<bool> m_carryTaken` (or mark the handle invalid on
match — `ShipHandle{0, 0}` is never live) so the leftovers can be walked afterwards. For each
leftover with `drawn && mesh != INVALID_MESH`:

```cpp
ShipExplosion::Spawn spawn;
spawn.mesh = &m_meshes->Data(view.mesh);
spawn.world = view.lastWorld;
spawn.velMetresPerSec = view.lastVelMetresPerSec;
spawn.halfExtents = view.halfExtents;
spawn.seed = (std::uint64_t(handle.slot) << 32 | handle.generation) ^ (m_receiver.Latest().tick * 0x9E3779B97F4A7C15ull);
m_explosions.emplace_back().Start(spawn, m_particles);
TriggerCameraShake();
if (m_log) m_log->PushFormat(EventLog::Severity::Alert, SimTimeSec(), "SHIP LOST");
```

**Ageing, in `UpdateFeedback(_dtSec)`**, beside the order markers: `m_particles.Advance(dt,
m_fxRng)` where `m_fxRng` is a `Pcg32` member seeded once in `Init` (smoke emission is per
frame and does not need the per-explosion seed); every explosion `Advance(dt)`;
`std::erase_if` the finished ones.

**Drawing, in `Render`**, per design §8.3, so the body becomes:

```
BeginScene; ground; hulls (and record lastWorld / lastVel / drawn = true per ship)
if (m_fx && m_fx->Ready() && !m_explosions.empty())
    m_fxFragmentVerts.clear(); each explosion BuildFragments(m_fxFragmentVerts)
    m_fx->Begin(gpu, viewProj, lightDir, AMBIENT_LEVEL, eye); m_fx->DrawFragments(gpu, m_fxFragmentVerts)
DrawFeedback (unchanged)
if (m_fx && m_fx->Ready() && m_particles.Count() > 0)
    m_fx->Begin(...)
    m_fxSpriteVerts.clear(); m_particles.Build(Dark, right, up, m_fxSpriteVerts); m_fx->DrawSpritesDark(...)
    m_fxSpriteVerts.clear(); m_particles.Build(Additive, right, up, m_fxSpriteVerts); m_fx->DrawSpritesAdd(...)
screen-space feedback (unchanged)
```

The fragment pass runs before `DrawFeedback` because it writes depth and the decals only test;
the sprite passes run after because they do not write depth and must see the rings.

### 2.4 `Outpost/OutpostApp` — boot and the key

- A `Neuron::FxRenderer m_fxRenderer` member after `m_textRenderer`; `Init` calls
  `m_fxRenderer.Init(m_gpu, {L"Textures\\ShapeWireframe.dds", L"Textures\\Particle.dds",
  L"Textures\\Starburst.dds"})` **immediately after `m_textRenderer.Init`** — both record
  uploads and each calls `ExecuteAndWait`, and both must precede the first frame. Then
  `m_view.SetFxRenderer(m_fxRenderer)`.
- `OnKeyDown`: `case VK_F4:` despawns every selected ship — walk `m_view.Ships()` with
  `m_view.IsSelected(i)`, collect the handles first, then `m_world.DespawnShip(handle)` for each
  (collect first: despawn swap-and-pops the array being walked). Comment it as the debug hook it
  is, beside F3's, and update the `OutpostApp.h` comment that lists the debug keys.
- The F1 readout gains one line: live explosions, particle count, particles dropped, fx verts
  dropped — the numbers the acceptance below asks for, read off the screen instead of a debugger.
  The readout is `Hud::DrawDebug`, fed from `Hud::Frame::stats`, which the composition root fills
  in `OutpostApp::Render` (`OutpostApp.cpp:183–189`); so the four numbers are four `stats`
  fields, read from `m_view` and `m_fxRenderer` there, and one `DrawTextLine` in `DrawDebug`.
  `WorldView` exposes them through two accessors (`ExplosionCount()`, `Particles()` const).

### 2.5 Project files and documents

`Outpost.vcxproj` and `.filters` gain `ShipExplosion.h/.cpp`. No asset change: the textures are
already `<Image>` items (design §11). `Design/SpaceshipExplosion.md` §14 marks the four slices
`landed` in each one's pull request, this one last; `AGENTS.md`'s "Deliberately not here yet"
paragraph is checked — it says no combat and no damage model, both of which stay true; nothing
in it becomes false.

---

## 3. Out of scope

- **Any `GameLogic` change.** No health, no damage, no death reason, no wire message. The debug
  key calls `World::DespawnShip` directly from the composition root and the pull request says
  it is a debug hook, not a path a player reaches. `GameLogicTests` is not touched and is
  expected to pass unchanged.
- **Making the despawn an order over the transport.** Design §9 turns it down for a tuning aid.
- **A `Starburst.dds` flash, gravity, the `Fire` type, slow-motion ageing.** Design §12.
- **HUD treatment beyond the readout line and the event-log entry** — no minimap marker, no
  roster change. A ship that despawns already leaves the roster by the existing carry.
- **Sound.** There is no audio in the tree.
- **Tuning by eye beyond making the effect visible.** The numbers land as the source's; the
  owner tunes them in the file the day they look at it. The pull request may note what looked
  wrong; it must not silently change a constant away from the source's value without saying so.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `Outpost/WorldView.cpp:56–107` | `ApplySnapshot`'s carry loop — the leftovers *are* the trigger |
| `Outpost/WorldView.cpp:248–251` | The order-marker age-and-erase, the pattern for `m_explosions` |
| `Outpost/WorldView.cpp:507–572` | `Render`: the hull matrix (`hull`, line 541) is `lastWorld`; `ship.headingRad`/`ship.speed` give `lastVel` |
| `Outpost/WorldView.cpp:677–678` | `m_camera->Right()` / `Up()` already fetched for the thruster billboards |
| `Outpost/WorldView.cpp:166` | `TriggerCameraShake` |
| `Outpost/OutpostApp.cpp:36–104, 129–158` | Boot order (`m_textRenderer.Init` then `ExecuteAndWait`), the key switch with F3 |
| `Outpost/ViewTuning.h:43–52` | The marker section — the shape for the explosion section |
| `Outpost/Hud.cpp` | The F1 debug readout, for the one new line |
| `NeuronClient/MeshShatter.h`, `SpriteParticles.h`, `FxRenderer.h` (slices 2, 3) | `Spawn`/`Emit`/`Build`/`Draw*` |
| `NeuronCore/Pcg32.h` (slice 1) | `Signed`, `Float01`, `Below` |
| Design §3, §5.4, §6.1, §8.3, §9 | Scale rule, constants, draw order, the trigger |

---

## 5. What will surprise the implementer

### 5.1 The dead ship is not in `Latest()` any more

At the moment `ApplySnapshot` sees a handle missing, the new snapshot has no record for it; the
only thing that knows where the hull was is the `ShipView` being discarded. That is why
`lastWorld` and `lastVel` are written in `Render`, not looked up at death. A ship that despawns
before its first `Render` has `drawn == false` and does not explode; that is correct, and the
work order says so.

### 5.2 The carry loop matches from the wrong side for this

It walks the *new* ships looking for an old handle; the leftovers are the *old* ships not found.
Marking matches as they happen and walking the old list afterwards is the change; do not
restructure the loop, which ADR 0005 and slice 2b already reasoned about.

### 5.3 Despawning while walking `Ships()`

`World::DespawnShip` swap-and-pops, and the view's `Ships()` is a span over the receiver's
snapshot, not the world — so the walk is safe, but the *handles* must be collected before the
first despawn or a stale index is fed to the second. Two loops, three lines.

### 5.4 The shake is not scaled by the ship

`Camera::Shake()` takes no amplitude and `CAMERA_SHAKE_AMPLITUDE` is the only knob. A Carrier
and an Interceptor shake the camera the same. Making the shake scale with `hullScale` means a
parameter on `Camera`, which is a `NeuronClient` change and not this slice's; note it in the
pull request if it looks wrong, do not add it.

### 5.5 A Battleship explodes at 664 × 3 fragments and it is fine

Only Bomber, Corvette and Frigate load today (`OutpostApp.cpp:25–26`), all under the cap. The
readout line shows dropped fragments and dropped particles so the day a Carrier dies the numbers
are on screen rather than in a guess.

---

## 6. Decision records due

None expected. The trigger and the RNG placement were recorded before writing (design §13) and
slice 1 wrote the RNG record. **If the effect turns out to want simulation time** — so that keys
1/2/3 slow it with the ships — that is a record, because it reverses the "presentation state
ages in real time" convention every other effect in `WorldView` follows.

---

## 7. Acceptance

**Screenshots at two window sizes (AGENTS.md §7), from a single F4 on a selected Corvette,
captured at roughly 0.1 s, 1 s, 3 s and 6 s after the key:**

- At 0.1 s: the hull is gone, its triangles are in the air at the hull's position and colour,
  wire lines visible on the shards, a red bloom where it was; no frame shows both the hull and
  the fragments — the F1 readout's tick number in the two frames either side of the key is the
  evidence.
- At 1 s: fragments have flown outward and are tumbling; the fireball is at its widest; the ring
  of debris is visible with smoke behind each piece; the grid behind the smoke is **darker**, not
  lighter.
- At 3 s: the fireball is gone (2 s life); fragments are half-faded; smoke persists.
- At 6 s: fragments gone (5 s); the last debris has just expired (6 s); smoke puffs remain and
  fade over the next 5 s; `m_explosions` is empty on the readout.
- A moving Frigate (order it somewhere, F4 mid-flight): the wreck keeps drifting along its
  heading — inherited velocity is visible as the fragment cloud's centre moving.
- Three ships selected, F4: three explosions; the readout shows particles ≤ 4096 and reports
  how many were dropped (expected: some smoke, at three simultaneous deaths near the peak).
- The camera shakes on death.
- After all of that: rings, markers, thrusters and the HUD are unchanged from before this slice —
  one screenshot of a selected, moving ship with the effect idle.

**Determinism, by inspection with a fixed seed:** F4 on the same ship at the same tick twice
(restart the game, same selection, same key) gives the same first-frame fragment layout — two
screenshots that match by eye. The seed formula is in §2.3; the pull request states it.

**The tree:**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- Debug|x64 builds and runs; all four suites run and pass unchanged — `GameLogicTests` in
  particular, since nothing there was touched.
- `git diff --stat` shows no file under `GameLogic/`, `NeuronCore/`, `NeuronClient/`,
  `NeuronServer/`.
- `Design/SpaceshipExplosion.md` §14 marks slice 4 `landed`. Design §13 has no open question;
  if building this raised one, it is recorded there rather than settled silently in code.
- `AGENTS.md` checked; no sentence made false.

---

## 8. Assumptions the implementer may make

- **F4 is the key** — settled with the owner (design §13), not an assumption; it is unbound
  today. The `OutpostApp.h` comment listing debug keys is updated.
- **World up is the plume axis.** The source used world +Y for a building; a banked hull's local
  up would tilt a plume a few degrees and nobody would see it.
- **`hullScale` from `max(halfExtents)`**, clamped `[0.25, 8]`, is the scale rule; it uses a
  value `ShipView` already holds and reads no `HullSpec`.
- **The effect ages in real time**, like every other feedback in `WorldView`, unaffected by keys
  1/2/3 (§6 says what it takes to change that).
- **Particle capacity is 4096 and the pool is shared** across explosions; smoke is what drops
  first at overload, and that is acceptable.
- **A ship despawned before its first draw does not explode** (§5.1).
- **The fragment colour is the panel's vertex colour mixed with `SHIP_COLOUR` by
  `SHIP_MATERIAL_MIX`** — the same mix `ScenePS` applies — done on the CPU at spawn so a shard
  matches the hull it came from. Settled with the owner (design §13); not a choice the
  implementer makes. Since `MeshShatter` copies the `MeshVertex` colour verbatim, the mix is
  applied by `ShipExplosion` on a per-spawn copy of the colour — or, simpler, by
  `MeshShatter::Desc` gaining `tintColour` and `tintMix` fields with defaults that leave the
  colour untouched. Prefer the `Desc` fields: three lines in slice 2's type, no copy of the mesh.
- **The wireframe decal is white**, as the texture holds it; no tint constant is added this
  slice.
