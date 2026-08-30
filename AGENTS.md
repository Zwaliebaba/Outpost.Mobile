# Outpost — how to work here

Conformance rules for anyone, human or agent, writing code in this repository.

Outpost is a Windows game built on the **Neuron** engine libraries. The engine and the game are
separate on purpose: `Neuron*` is meant to carry more than one game, and the day it stops being
able to is the day it stopped being an engine. Most of the rules below exist to hold that line.

**This tree is greenfield. Nothing is grandfathered** — the rules apply to every line.

Read this file before you start. Where a rule blocks the task, say so in your report rather than
quietly bending it.

### What is actually here

This document describes the tree as it stands, not as it is meant to end up, because the version
of it that arrived here described a different repository entirely and every rule in it was
therefore unverifiable. Keep it that way: if a change makes a sentence below false, the sentence
changes in the same commit.

**Built and tested.** Five projects and four test suites, Debug|x64, gating in CI (§6). The game
is a fleet of three hulls in open space: select them, order them somewhere in formation, watch
them route around architecture, give way to each other, and arrive without passing through
anything — and F4 shatters a selected hull into tumbling debris, a fireball and smoke. Not every
ship is the player's: an enemy station sits 1.2 km northeast with three Interceptors patrolling a
ring around it, drawn red in the scene and on the minimap and counted as contacts, and they cannot
be selected or ordered — the simulation refuses an order from the wrong faction and the client does
not offer one.
There is still no combat. One world and six asteroids share the sky with the fleet, and they are
made two different ways (`Design/Decisions/0026`): the world is a smooth sphere wearing an authored
equirectangular map, sampled per pixel off the direction so it has no seam, while a rock is still a
seeded low-poly heightfield on a cube-sphere with one flat colour per triangle from a colour ramp and
a wire-frame outline over the top. Behind them is a procedurally generated star field: a seeded
catalogue of stars, dust clouds and a galactic band, uploaded once and expanded into billboards in
the vertex shader (`Design/Archive/Skybox.md`). F5 reseeds the lot, sky included; all of it is presentation
only and a ship flies straight through a rock (`Design/Decisions/0016`). There is no ground: the scene pass draws no plane and has no grid
(`Design/Decisions/0025`), and the flat y = 0 plane a move order lands on is arithmetic in `Camera`
rather than geometry. D3D12 renderer, WM_POINTER input covering mouse and touch, a main-screen HUD
drawn through one overlay pipeline (bitmap font atlases, coverage-mask icons, untextured quads),
textured FX pipelines for the explosion's fragments and sprites, a two-pass body pipeline, an
additive sky pass, OBJ/MTL hulls, DXC-compiled shader model 6.7 shaders.
`Transport` has a QUIC implementation over MsQuic, and the game boots on it and only on it:
`Outpost.exe` listens and dials across `127.0.0.1`, so every frame of every run crosses a real
network stack. There is no fallback — a boot that cannot open the wire says which stage refused and
stops, rather than running on a second path nobody is testing (`Design/Decisions/0028`). The
`LINK | QUIC` line in the event log is what a good boot looks like. `LoopbackTransport` is still in
`NeuronCore`, now as what the tests drive: it is the only way to drop or delay a datagram on
purpose.

**Deliberately not here yet**, so nobody goes looking for it: no audio, no combat, no economy, no
damage model, no save format, no content pipeline beyond OBJ and DDS, and no configuration file —
tuning is `constexpr` in `SimTuning.h`, `HullSpec.h` and `ViewTuning.h` (§5). The hostiles above
have no weapons and no senses: the patrol is a metronome that never reacts to anything, and the
station cannot be destroyed. The networking stops well short of a network: one client, one process,
`127.0.0.1` only, and a self-signed certificate the client does not validate. The wire has two lanes
and the format chooses by asking whether a later message makes a lost one right (`ADR 0029`):
positions are datagrams and heal themselves, while departures and move orders take the reliable
lane and cannot be lost. The seam serves N subscribers now rather than one: `Game::Publisher` holds
a table of them, each with its own interest set, writer, faction, phase, order budget and despawn
cursor (ADR 0030). What remains missing is the far end — this executable adds exactly one entry,
there is no second machine to be on the other side of it, and no decided way to tell a headless
build what to be.
The client sees the world through the seam, filtered to what one subscriber can see (§2).
Where the HUD shows a number the simulation does not yet have, it is a placeholder supplied by the
composition root, and it says so at the definition.

---

## 1. Naming convention (normative — no exceptions)

| Kind | Convention | Example |
|---|---|---|
| Type (class, struct, enum, concept, alias) | `PascalCase` | `SnapshotBuffer` |
| Function, method | `PascalCase` | `SolveFormation()` |
| Member variable | `m_camelCase` | `m_deviceLost` |
| Static member | `sm_camelCase` | `sm_activeDevice` |
| Global | `g_camelCase` | `g_frameCount` |
| Parameter | `_camelCase` | `_shipId`, `_fileName` |
| Local | `camelCase` | `stationIndex` |
| Constant (`constexpr`, `static constexpr`) | `UPPER_CASE` | `TICK_HZ`, `MAX_DATAGRAM_BYTES` |
| Enumerator | `PascalCase` | `QueueFull` |
| Macro | `SCREAMING_SNAKE` | `ASSERT`, `ASSERT_TEXT` |
| Namespace | `PascalCase` | `Neuron`, `Game`, `Outpost` |
| File | `PascalCase.cpp` / `.h` | `ServerHost.cpp` |
| Directory | `PascalCase` | `Design/Decisions/`, `NeuronClient/Shaders/` |

### The rules behind the table

**R1 — The leading underscore on parameters is deliberate.** It is legal C++: the reserved forms
are `_Uppercase`, anything containing `__`, and `_lowercase` **at global scope**. A parameter is
never at global scope, so `_shipId` is safe. Never introduce a reserved form — no `_Impl`, no
`__helper`, no file-scope `_cache` (use `g_cache` in an anonymous namespace).

**R2 — A type name carries no prefix, and that includes abstract ones.** The transport abstraction
is `Transport`, **not `ITransport`**. A base class is not `BaseTransport` or `AbstractTransport`.
This also bans `CFoo`, `SFoo`, `EFoo`, `FooBase`, `FooImpl`, and `_t` suffixes. Name the concept
and let the concrete types say what they are — `Transport` and, when one exists, `QuicTransport`.
Where a type never had and never will have a second implementation, do not invent a base class
for it.

**R3 — Compile-time constants are UPPER_CASE.** `static constexpr` members and namespace-scope
`constexpr`/`inline constexpr` take UPPER_CASE: `TICK_HZ`, `TICK_DT`, `MAX_DATAGRAM_BYTES`,
`FRAME_COUNT`. Enumerators are not constants in this sense and stay PascalCase (`QueueFull`).
`sm_` is reserved for *mutable* statics, which are rare and must document their thread-safety.

**R4 — Acronyms capitalize as words**: `HudRoster`, `UdpTransport`, `GpuDevice`, `JsonWriter` —
never `HUDRoster` or `UDPTransport`. Identifiers from an external SDK keep that SDK's spelling and
are never renamed to fit: `XMFLOAT2`, `XMVECTOR`, `ID3D12Device`, `IDXGISwapChain4`, `HRESULT`.

**R5 — Template parameters are PascalCase**: `T`, `Fn`, `BlockBytes`, `Ts...`.

**R6 — Units belong in names; types do not.** `posXCm`, `velXCmPerSec`, `etaTicks`, `zoomMetres`,
`cooldownMs`, `dragThresholdPx` are encouraged — this game measures a plane in meters, a wire in
centimeters, and time in ticks, so unit ambiguity is a real defect class. Never encode the type:
no `iCount`, `pShip`, `strName`, `dwFlags`.

**R7 — A file is named for its primary type**, PascalCase, `.h` / `.cpp` only. `.hpp`, `.cc` and
`.inl` are not used; template implementations live in the header. Two exceptions: the per-project
`pch.h`/`pch.cpp` keep the name MSBuild expects, and a file holding one *family* takes the
family's name (`RenderTypes.h`, `GpuHelpers.h`, `SimTuning.h`).

**R7a — A directory is PascalCase too**: `Design/Decisions/`, `Design/Archive/`, `Outpost/Assets/Textures/`,
`NeuronClient/Shaders/`. A lowercase folder beside PascalCase ones reads as somebody else's, and on a
case-insensitive file system a `decisions/` and a `Decisions/` are the same folder until the day they
are checked out on one that is not. Two names are not ours and stay as their owners spell them:
`.github/` (GitHub's) and `packages/` (NuGet's), along with anything a tool writes such as `x64/`.

Shaders are the one thing in this tree that generates code, and they are named differently on
purpose — see §3. Formats get hand-written parsers; no other generator is to be introduced, and
nothing generated is committed.

**R8 — `m_` marks encapsulated state, not every field.** A `class` with invariants prefixes
private members `m_`. A public aggregate — a config struct, a wire record, a POD handed to the
renderer — uses plain `camelCase` fields so brace initialization reads naturally. So
`MeshVertex{ px, py, pz, r, g, b }` and `Camera::Desc{ .minZoom = … }` carry no prefix, while
`ServerHost` keeps `m_accumulatorSec`.

**R9 — Namespaces.** Three, all flat:

- `Neuron` — the four engine libraries. Flat, not `Neuron::Client`, because engine code is meant
  to move between the libraries without a rename pass; the library boundary and §2's dependency
  rules do the separating, and the namespace does not need to repeat it.
- `Game` — GameLogic.
- `Outpost` — the executable's own types.

Do not nest further (`Neuron::Core::Detail`), and do not add a `Detail`/`Internal` namespace where
an anonymous namespace in a `.cpp` will do.

**R10 — Integer widths are spelled `std::uint32_t`.** Design prose may use `u8`/`u16`/`u32` as
shorthand; code uses the standard names, and any wire struct states its width explicitly on every
field.

**R11 — English is US English.** Prose everywhere — comments, design documents, commit messages,
HUD strings — spells color, behavior, meter, center, gray. New identifiers do the same, under one
exception that outranks it: a name extending a family the tree already has matches that family's
spelling — an `orderSpeedCapMetresPerSec` beside `maxSpeedMetresPerSec`, never a `MetersPerSec`
beside a `MetresPerSec`, because two spellings of one suffix is a grep that silently misses half
its answers. The standing UK-spelled families are `*Metres*`/`*_METRES`, `*Colour*`/`*_COLOUR`,
`Neighbour*`, `*Centre*` and `*_GREY`; each keeps its spelling until it is renamed whole, one
family per dedicated slice, and leaves this list in the same commit that renames it. Older prose —
the earlier design documents, the comments beside those families — converts with those slices or
in a documentation pass of its own; prose written from now on is US English.

### Worked example — this is the target style

[`NeuronServer/ServerHost.h`](NeuronServer/ServerHost.h), abridged. It is a real file rather than
an invented one, so it can be read in full and cannot drift from the rules it illustrates:

```cpp
#pragma once

#include "Simulation.h"

#include <cstdint>

namespace Neuron                                        // R9: flat, PascalCase
{
class ServerHost                                        // R2: no prefix, no Base/Impl suffix
{
public:
  struct Desc                                           // R8: aggregate -> plain camelCase fields
  {
    float tickHz = 60.0f;
    float maxCatchUpSec = 0.25f;                        // R6: unit in the name
  };

  void Init(const Desc& _desc, Simulation& _simulation) noexcept;   // R1: _ on parameters
  int Advance(float _dtSec);
  [[nodiscard]] float InterpolationAlpha() const noexcept;

private:
  Simulation* m_simulation = nullptr;                   // R8: encapsulated state carries m_
  float m_accumulatorSec = 0.0f;
};
} // namespace Neuron
```

For a constant and an enum in the same style, see
[`NeuronCore/Transport.h`](NeuronCore/Transport.h): `MAX_DATAGRAM_BYTES` (R3, UPPER_CASE) and
`ConnectionState::Draining` (R3, enumerators stay PascalCase).

### Enforcement

[`.clang-tidy`](.clang-tidy) is the machine-readable statement of the table and is the **single
source of truth for the option values** — this document states the rules in prose and does not
repeat the settings, so there is nothing to drift. [`.clang-format`](.clang-format) owns
whitespace; the two never overlap.

**Both gate in CI** (§6). `clang-format` runs in its own Linux job — it only reads C++ as text, so
it needs no SDK and no Windows runner. `clang-tidy` runs on Windows after the build, over GameLogic.
Both arrived non-blocking and were promoted once a run came back clean, which is the only way a
linter should ever start gating.

Run either yourself before you push — clang-tidy on the files you wrote, not on the tree:

```
python Build/CheckFormat.py            report what is not formatted
python Build/CheckFormat.py --fix      format it

clang-tidy --quiet NeuronCore/YourNewFile.cpp -- -I . -D _WIN32 -D _DEBUG /std:c++latest
```

**Pin your clang-format to the version CI uses** — 18.1.3, via `pip install clang-format==18.1.3`.
Its output changes between releases, so a different local version reformats files CI then rejects,
and the two of you take turns undoing each other.

The tree is formatted and a whole-tree run is a no-op. Keep it that way: format the lines you
write, and do not reformat files you are only passing through.

Three rules `.clang-tidy` structurally cannot state. All three are checked by
[`Build/CheckProjectFiles.py`](Build/CheckProjectFiles.py) instead, which needs no compiler and runs
in CI before the build:

- **R2's affixes.** clang-tidy can require an *absent* prefix but cannot ban a *present* suffix, so
  `FooBase` slips through it entirely. A bare `struct Impl;` is the pimpl idiom and is not a suffix
  on anything; `FooImpl` is. The check tests a declared type name for a two-capital start, a
  `Base`/`Impl` suffix, an `Abstract` prefix or a `_t` suffix — the two-capital test being safe only
  because R4 already bans `GPUDevice` in favour of `GpuDevice`.
- **R7's file naming**, and that a new file is registered in the `.vcxproj` **and** the
  `.filters` (§3). Registering it in one and not the other is the miss §8's checklist exists to
  prevent.
- **R11's spelling.** No tool in this tree reads English, and none needs to: the check reads only
  names *this tree declares*, told apart by the tree's own markers — R1's leading `_`, R8's `m_`/
  `sm_`, R3's UPPER_CASE, and type names. A local spelled plain camelCase is out of its reach, and
  so is every call into D3D12 and Win32, which spell `Color` and `Center` and are not ours to
  rename.

Two more things it checks that no rule had to state, because both cost a CI run first: a declarator
named after a macro `<windows.h>` defines (`near`, `far`, `small`, `interface`, …), which expands
rather than failing where it is written; and a braced call argument of nothing but literals, such as
`Connect(client, server, {0, 256, 3})`, where inserting a field into the aggregate silently rebinds
every element. Neither reaches the cases needing types — a positional aggregate of *variables* is
the same hazard and is not caught — and the file says so at each check rather than implying cover it
does not have.

---

## 2. Repository map and the dependency rules

| Path | What it is |
|---|---|
| `NeuronCore/` | Engine primitives shared by every layer — zero game semantics, no graphics API, headless (below). Diagnostics, file IO, framerate-independent easing, the frame clock, the seeded `Pcg32` (ADR 0012), and `Transport`. No content readers: those live with their consumer (below). |
| `GameLogic/` | The deterministic simulation, namespace `Game`. `World`, `ShipState`, `WorldPos`, `HullSpec`, `Movement`, `Collision`, `SpatialIndex`, `PathGrid`, `Formation`, `Patrol`, `SimTuning`, `InterestSet`, `PathIslands` (the architecture partitioned into islands, one `PathGrid` over each, ADR 0033), `WorldSnapshot` (the wire format, ADR 0008) and `Publisher` (the fan-out to N subscribers, ADR 0030). Depends on NeuronCore only. |
| `NeuronClient/` | The presenting half — `AppWindow`, `PointerTracker`, `Camera`, `GpuDevice`, `SceneRenderer`, `TextRenderer`, `BitmapFont`, `ScreenImage`, `MeshLibrary`, the explosion's `FxRenderer`/`MeshShatter`/`SpriteParticles` and the `GlowBillboards` the thruster plume is built with, `ViewCulling` (the camera's frustum and the sphere test everything drawn is gated on), the planet pipeline (`CubeSphere`, `Noise3`, `BodyDesc`/`BodyParams`/`BodyField`, `BodyMeshBuilder`, `BodyRenderer`, `ColourRamp` — see [`Design/Archive/PlanetRenderer.md`](Design/Archive/PlanetRenderer.md)), the star field (`SkyField`, `SkyRenderer`, `SkyVertex` — [`Design/Archive/Skybox.md`](Design/Archive/Skybox.md)), and the content readers `DdsImage`, `ObjParser`/`MeshData`. Everything that names a graphics type lives here and nowhere else. |
| `NeuronServer/` | The authoritative half — `ServerHost` and the `Simulation` interface it drives. |
| `Outpost/` | The executable: composition root, presentation state, the HUD and its event log, boot and shutdown ordering. `Outpost/Assets/` is the content the MSIX package deploys. |
| `Tests/*Tests/` | VS CppUnitTestFramework suites, one per library. |
| `NeuronClient/Shaders/` | HLSL (§3). DXC compiles it, as shader model 6.7 DXIL, into `NeuronClient/CompiledShaders/`, which is build output and not in source control. |
| `Build/` | The checks CI runs and you can run: `CheckProjectFiles.py`, `CheckFormat.py`, and `Projects.py`, which both read the project list out of the solution (§6). |
| `Tools/` | Content tools, stdlib Python only: the NMO ship-mesh codec and Blender add-on (`BlenderNmo/`), the OBJ→NMO converter (`ObjToNmo.py`), and their tests (`Nmo*Test.py` — the codec test needs bare python3, the Blender one the `bpy` wheel). [`Design/NmoFormat.md`](Design/NmoFormat.md) is the format; nothing here is engine code, and no `.vcxproj` names it. |
| `Design/` | Designs with a slice still open, `Screenprints/`, `Archive/` for designs whose slices have all landed and for the work orders that landed them, and `Design/Decisions/` — the architecture decision records (§9). An archived design is still the document its area is reviewed against and is cited from code as before; `Design/` itself is the list of what is unfinished. Its `README.md` says which document is which and how a slice moves from a design into the tree (§7). |
| `.github/` | CI (§6) and the pull request template every slice answers (§7). |

**On the name.** The repository is `Outpost.Mobile`; Outpost is a Windows game and nothing in this
document says otherwise. The name is historical. What is actually mobile-adjacent in the tree is
`WM_POINTER` touch input, MSIX packaging, and the ARM64 configurations §6 records as unverified —
there is no phone target, and no plan stated anywhere for one. Read the name as a repository
identifier, not as an intention.

The dependency rules are hard, and each of them is one thing this structure buys:

```
                NeuronCore
               /     |     \
      NeuronClient  GameLogic  NeuronServer
               \     |     /
                  Outpost.exe
```

- **GameLogic depends on NeuronCore and nothing else.** No renderer, no window, no clock. That is
  what lets it be unit-tested, replayed, and one day run on a server with no GPU.
- **Client and server never depend on each other.** When the halves separate they will talk over
  `Transport` and nothing else — no shared memory, no cross-half calls, no singleton bridging
  them. Anything that works in-process and not over a wire is a bug that only shows up on the day
  it is expensive to find.
- **The engine libraries never reference GameLogic.** `Neuron*` is a shared engine; the game is
  reached through engine-declared interfaces that `Outpost.exe` injects. `NeuronServer` declares
  `Simulation`; `Outpost/WorldSimulation.h` implements it over `Game::World`. The moment an engine
  project names a game type it stops being an engine.
- **Nothing depends on the executable.**
- **NeuronCore and NeuronServer are headless.** The server will one day run in a container with
  no screen, so neither may include a graphics header — no `<d3d12.h>`, no `<dxgi*.h>`, not even
  `<dxgiformat.h>` — and neither may hold a device, a swapchain, a window, a descriptor, or any
  other data only a GPU can use. Everything client-specific lives in `NeuronClient`. DirectXMath
  is mathematics, not graphics, and stays. `Build/CheckProjectFiles.py` checks the include rule.
- **A content reader lives with what consumes it**, not in NeuronCore by default. The test is
  "who calls it", not "does it include a graphics header": the texture and mesh readers are client
  code because only the renderer reads a texture or a mesh. Anything the simulation needs to know
  about a hull — its size, its collision shape — arrives as authored numbers in GameLogic, never
  as something derived from a mesh at load time.

Both of the last two rules have decision records behind them (§9); read those before arguing
either one, because the alternatives were considered and the records say why they lost.

### Where the client/server seam stands today

`Outpost.exe` runs both halves in one process, and it stays one process. What is no longer shared
is memory: `WorldView` reads a snapshot that arrived over a `Transport` and sends move orders back
up the same wire, and its header does not include `World.h`, so the seam is structural rather than
a convention. Nor is the wire pretend any more — the two halves talk over QUIC across `127.0.0.1`,
so the process boundary is the only one left to cross.

`NeuronCore/Transport.h` declares the seam, and two implementations answer it. `LoopbackTransport`
puts the far end in this process, with latency and loss you can configure — counted in ticks rather
than seconds, so a measurement reproduces. `QuicTransport` puts it on one MsQuic connection, with
the same datagrams and the same contract (`Design/Decisions/0021`); `QuicListener` beside it is the
half a dedicated server would start. The composition root chooses between them at boot and
nothing below it can tell which it got, so what remains is a second process, and it is not
scheduled: the code boundary is the part that had to land early, because it is what
stops the two halves growing into each other. Do not
shortcut it: if you find yourself wanting the simulation to call into the renderer, or the renderer
to reach into the world, that is the seam telling you the change belongs somewhere else.

---

## 3. Files, layout and includes

- **Flat project directories.** All of a project's `.h`/`.cpp` sit directly in its folder. No code
  subdirectories — grouping lives in `.vcxproj.filters` only. Two folders are exempt because
  neither holds hand-written C++: `Shaders/` and `CompiledShaders/` (below).
- **File names are unique repo-wide**, and also unique against the CRT, the STL and the Windows
  SDK — **case-insensitively**. A header named `Time.h` or `Assert.h` shadows `<time.h>` or
  `<assert.h>` for every translation unit that can see this folder, and the errors land inside the
  STL with nothing pointing at you. Several project roots sit on the include path at once, so a
  duplicate name silently resolves to whichever root comes first. Check the map in §2 before
  creating a file.
- **Includes are unqualified**: `#include "Camera.h"`. Each project lists the libraries it is
  entitled to as `$(MSBuildThisFileDirectory)..\<Project>` include paths — a relative path rather
  than `$(SolutionDir)`, so building a `.vcxproj` directly resolves the same way a solution build
  does.
- **The include path is not the dependency rule.** `NeuronClient` and `NeuronServer` must not list
  `GameLogic`, and no engine project may.
- **Each project reaches the engine through one umbrella header** — `NeuronCore.h`,
  `NeuronClient.h`, `NeuronServer.h`, `GameLogic.h` — pulled in by its `pch.h`. That is where the
  Windows headers are configured, in the one order that works, and it is why no `.cpp` in this
  tree includes `<windows.h>` itself. `Outpost/pch.h` includes three of them because the
  composition root is the only thing entitled to see every layer.
- **A header that declares a member of type `T` includes `T`'s header itself**, even though the
  umbrella would have supplied it. The umbrella is a convenience, not a contract.
- **Every project's `pch.cpp` contains exactly `#include "pch.h"`** and nothing else. `/Yc`
  requires the translation unit that *creates* the precompiled header to include it; an empty one
  is `C2857`, reported at line 1 column 1 of a file whose entire contents are the thing that is
  missing. Six of them were empty once, and it cost a CI run.
- **Every added, removed or renamed file updates both** the `.vcxproj` and the `.vcxproj.filters`
  of its project, in the same commit. [`Build/CheckProjectFiles.py`](Build/CheckProjectFiles.py)
  checks this, and runs in CI before anything is compiled — run it yourself before you push.

### Shaders

HLSL lives in `<Project>/Shaders/`, one entry point per file, named for the stage it is:

| | |
|---|---|
| `<Name>VS.hlsl` | vertex shader |
| `<Name>PS.hlsl` | pixel shader |
| `<Name>CS.hlsl` | compute shader — `BodyBakeCS`, `BodyBakeMaxCS` |
| `<Name>.hlsli` | declarations the stages of `<Name>` share — cbuffers, and the `VsOut` struct that is the contract between them |

**DXC compiles them at build time**, as shader model 6.7 DXIL, into `<Project>/CompiledShaders/<Name>.h`, as a byte array
called `g_p<Name>` — so `Shaders/SceneVS.hlsl` becomes `CompiledShaders/SceneVS.h` holding
`g_pSceneVS`, and the renderer says:

```cpp
#include "CompiledShaders/SceneVS.h"
…
pso.VS.pShaderBytecode = g_pSceneVS;
pso.VS.BytecodeLength = sizeof(g_pSceneVS);
```

Nothing compiles HLSL at runtime. That is the point of the arrangement: a shader mistake is a
build error rather than a message box on a player's machine, startup does no compilation, and the
binary carries no dependency on `d3dcompiler_47.dll` or `dxcompiler.dll`. The one thing it does depend on
is the player's driver reaching shader model 6.7, which `GpuDevice::Init` checks up front so the
failure names the adapter rather than surfacing as an `E_INVALIDARG` from a pipeline state
(`Design/Decisions/0018`).

`CompiledShaders/` is build output and is **not** committed — committing it would mean reviewing a
byte array on every shader edit, and would let the header and the `.hlsl` disagree. The generated
headers are exempt from §1: `g_p<Name>` is the HLSL compiler's convention, not this repository's, and R6 does
not apply to a name a tool chose.

Shader settings live in `NeuronClient.vcxproj`, in the same per-configuration
`ItemDefinitionGroup`s as the compiler settings (§6) — `ShaderModel` 6.7, which is what makes
Visual Studio's `FxCompile` task run the Windows SDK's `dxc.exe` rather than `fxc.exe`, and
`AllResourcesBound`, which is true because every resource a shader here reads sits in a
root-signature slot that is always filled — except the two that genuinely differ per
file — whether it is a vertex or a pixel shader, and where its header goes — which are spelled on
the `FxCompile` item.

---

## 4. Layout and formatting

[`.clang-format`](.clang-format) is the authority; [`.editorconfig`](.editorconfig) repeats only
what an editor needs before the first save. The shape: **Allman braces, 2-space indent, 140
columns, no tabs, `namespace` contents not indented, pointer binds left** (`ID3D12Device*
_device`).

Include order is **not** sorted automatically and is grouped by hand: `pch.h` first, then this
project's headers, then the headers of libraries it depends on, then SDK headers, then the
standard library. A formatter reordering these behind your back is a correctness risk, not a style
preference — `SortIncludes` is off for that reason.

**The whole tree is formatted, and CI gates on it** (§6). `python Build/CheckFormat.py --fix`
applies it. Format the lines you write; do not reformat files you are only passing through.

`.clang-format` carries one entry worth knowing about. The Visual Studio test macros read as
function calls, so without help clang-format collapses a whole `TEST_CLASS` into one line of
nonsense and guesses the braces from there. The `Macros:` block tells it what `TEST_CLASS` and
`TEST_METHOD` expand to, which is what makes a test file safe to format at all. Add to it if a new
macro of that shape appears.

---

## 5. C++ rules for this codebase

- **C++20 is the floor**, `ConformanceMode` on, `/fp:precise`, no `/arch`. Every project and every
  configuration compiles `/std:c++latest` on `v145`, spelled literally, so your desk and CI compile
  the same standard. Nothing here may *need* C++23 to build even so; if something does, it goes
  behind a feature test, because the pin is a decision that will be revisited and a feature test
  survives that. Do not turn conformance off to make something compile. **These are spelled in
  every `.vcxproj`, per configuration and identically** (§6): change one and you are changing nine,
  or you have introduced the drift the guard exists to catch.
- **Math is DirectXMath, used natively** — no wrapper types, functions, or aliases. Store
  `XMFLOAT2/3/4`, `XMFLOAT4X4`; compute in `XMVECTOR`/`XMMATRIX` as locals and parameters. Never a
  stored `XMVECTOR` or a `std::vector<XMVECTOR>`.
- **This tree is left-handed. Where an API offers `LH` and `RH`, take `LH`** — every time, without
  deliberating: `XMMatrixLookAtLH`, `XMMatrixPerspectiveFovLH`,
  `BoundingFrustum::CreateFromMatrix(…, rhcoords: false)`. Render space is `(east, up, north)`,
  which is a left-handed basis, and Direct3D is left-handed too, so `LH` is also every SDK's
  default. **An `RH` call does not fail, it mirrors** — east ends up on the left of the screen —
  so nothing catches this but the rule.
- **GameLogic is deterministic.** No wall clock, no OS entropy, no pointers as keys, no iteration
  order that isn't dense-array order, one seeded PCG32 when randomness arrives. `XM*Est` functions
  are **banned** there, and `/fp:fast` and `/arch` are banned solution-wide, because a
  floating-point mode that differs between two projects makes the same tick produce two answers.
  `GameLogicTests` runs the same order twice and compares every tick; that suite is the gate.
- **Presentation state does not live in the simulation.** Ring fades, camera lag, thruster glow
  and trail sampling belong to `WorldView`. A value that would have to be sent over a wire to a
  spectator belongs to `World`; a value that would not, does not.
- **No argv, no environment variables.** Configuration is loaded by the composition root only;
  libraries receive plain config structs (`Camera::Desc`, `ServerHost::Desc`,
  `PointerTracker::Desc`) and never read files or the registry themselves.
- **Single-writer state.** The authoritative world belongs to whichever thread ticks it, render
  state to the main thread. Today both are the same thread, which is why this rule is easy to break
  without noticing: when a transport's workers or an audio callback arrive, they enqueue to a ring
  and touch nothing else.
- **COM lifetimes are RAII, through `winrt::com_ptr`** (aliased as `Neuron::GpuPtr`), not
  `Microsoft::WRL::ComPtr` and never raw `Release()` calls. Two idioms: create with
  `Thing(IID_PPV_ARGS(thing.put()))`, query with `thing.try_as<IOther>()` (null on failure).
  `IID_PPV_ARGS` derives the IID from the pointer's own type, and hand-spelling `__uuidof` and
  `put_void()` separately lets the two disagree, which the compiler cannot catch. **`put()` asserts
  the pointer is empty** rather than silently releasing what was there, so release before refilling
  — and do not reuse one `com_ptr` across the iterations of an enumeration loop. Include
  `<unknwn.h>` before `<winrt/base.h>` or classic COM interfaces are unsupported. This is the
  COM-helper sanction only — do not reach for the WinRT projection as a UI or async framework.
- **There is one error path, and it throws.** An `HRESULT` that must succeed goes through
  `winrt::check_hresult`; a Win32 call that must succeed goes through `winrt::throw_last_error`; a
  broken invariant goes through `Debug.h`'s `ASSERT`/`DEBUG_ASSERT`. All three arrive at the single
  `try` in `wWinMain`, which is the only place that knows there is a person to tell. Do not write a
  log-and-return helper, and do not catch inside a library — there is nothing it could sensibly
  decide.

  Three deliberate exceptions: capability **probes** (`SUCCEEDED` on an optional feature, adapter
  enumeration, the debug-layer attempt) are control flow, not error checking; **shutdown paths**
  (`Shutdown`, destructors) must not throw, so they do nothing rather than report; and per-frame
  **`Present`** logs and carries on until device-removed handling exists.
- **Errors that are the user's fault are diagnostics, not crashes.** Anything parsing content or
  configuration reports what was wrong and fails closed; it never throws on malformed input and
  never asserts. A missing hull logs and is skipped — it does not fail boot.
- **No external libraries without the owner's explicit approval.** Pre-approved: the Windows SDK
  (Win32, Winsock2, D3D12/DXGI, DirectXMath), the packages already in the
  `packages.config` files, and C++/WinRT as above. If you believe a third-party library is
  justified, present the case and **stop** — do not assume approval.

  What the tree does *not* use yet, whatever the package list suggests: there is no audio and no
  WinUI. Several Windows App SDK packages are restored because the executable was generated from an
  MSIX template; none of them is referenced by code. MsQuic
  (`Microsoft.Native.Quic.MsQuic.Schannel`) is no longer one of those: it is what
  `NeuronCore/QuicTransport` is built on (`Design/Decisions/0021`), `NeuronCoreTests` drives a real
  connection over `127.0.0.1` against it, `Outpost.exe` boots on it, and `msquic.dll` is copied
  beside every executable that imports it by the package's own targets — nothing in this tree copies
  it and nothing should start.

---

## 6. Build and verify

```
nuget restore Outpost\packages.config -PackagesDirectory packages    (and one per project)

msbuild Outpost.slnx /p:Configuration=Debug   /p:Platform=x64
msbuild Outpost.slnx /p:Configuration=Release /p:Platform=x64

vstest.console.exe x64\Debug\NeuronCoreTests.dll x64\Debug\GameLogicTests.dll ^
                   x64\Debug\NeuronClientTests.dll x64\Debug\NeuronServerTests.dll
```

The projects use `packages.config`, not `PackageReference`, so `msbuild -t:restore` does nothing
for them — restore is `nuget restore` per config file, which is what the CI step does.

x64 is the configuration that is built and run. Win32 and ARM64 exist in the project files but are
not exercised; the solution maps `*|ARM64` onto the x64 libraries, which will not link against an
ARM64 executable. Treat anything other than x64 as unverified.

### The project files are MSVC-native

**This is a hard rule, and it is the reason the build looks the way it does.** Every `.vcxproj` in
this tree is the shape Visual Studio itself writes and round-trips. Three things follow, and none
of them is negotiable:

- **Settings are spelled per `Configuration|Platform`**, in `PropertyGroup Label="Configuration"`
  and `ItemDefinitionGroup` blocks conditioned exactly as
  `'$(Configuration)|$(Platform)'=='Debug|x64'` — one block per configuration the project declares,
  repeated. That is the form Visual Studio's property pages read and edit.
- **Values are literal.** `v145`, `stdcpplatest`, `Unicode`. Not computed, not conditioned on
  `$(VisualStudioVersion)`, not derived from a property this repository invented.
- **No MSBuild machinery of our own.** No custom `$(Outpost…)` properties, no property sheets in
  this repository, no marker comments that an external tool parses, no conditions Visual Studio
  would not have written itself.

**If a setting seems to want machinery, say so in your report instead of building it.** The
machinery is invisible in the IDE and does not survive a round-trip through it: settings that leave
a shape Visual Studio understands are settings Visual Studio will quietly rewrite. That is not
hypothetical — it is how all seven projects once ended up with Release carrying no optimiser, no
release CRT and `_DEBUG` defined.

**`PlatformToolset` is `v145` and `LanguageStandard` is `stdcpplatest`, everywhere.** That pins the
tree to the Visual Studio 2026 toolset on purpose. Two things about it are worth knowing before you
change it:

- v143 will not build this tree as spelled, so a machine with only Visual Studio 2022 needs the
  pin changed — deliberately, in all nine projects, not worked around with a condition.
- Leaving `PlatformToolset` empty is **not** the portable fallback it looks like.
  `Microsoft.Cpp.Default.props` drops all the way to `v100` and the build stops with `MSB8020`
  naming Visual Studio 2010, on a runner that has 2026 installed. That is measured, not read: it is
  what a CI run did.

The cost of all this is duplication — nine projects, four or six configurations each, the same
values written out longhand. That is the deliberate trade, and it is checked rather than trusted:
[`Build/CheckProjectFiles.py`](Build/CheckProjectFiles.py) reads the settings that must agree out
of every project's XML and fails the build, before anything is compiled, if one has drifted. It
reads XML rather than text, so a project Visual Studio has rewritten is still checked.

Before the sheets that used to hold these existed, all seven projects set `UseDebugLibraries=true`
and defined `_DEBUG` in **Release** as well as Debug, so a Release build was a Debug build under
another name and any number measured in it was measuring the wrong binary. The guard is what stands
in for the sheet now. A setting that drifts is always the one that mattered.

### CI

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds **Debug|x64** and runs all four
test suites on every push and pull request, and **it gates** — a red job means the branch does not
build or a test failed. Three things about it are worth knowing before you read a red run:

- **The toolset is pinned, so the runner has to carry it.** Every project spells `v145` and
  `stdcpplatest` literally (§6), which is the toolset a developer on Visual Studio 2026 uses and
  the one `windows-latest` has today. A runner image that dropped v145 would fail with `MSB8020`
  rather than silently compiling something else — which is the failure mode worth having, but it
  does mean the image is a dependency. **Nothing in this tree may need C++23 to build**; if
  something does, it goes behind a feature test.
- **It passes `/p:SolutionDir=` explicitly.** Without it every project writes its output beside
  itself instead of into `x64\Debug\`, and the test discovery below finds nothing.
- **The test suites are discovered from `x64\Debug\*Tests.dll`**, and the packages to restore from
  the `packages.config` files in the tree — neither list is spelled in the workflow, so a suite or a
  project added by a later slice is covered the day it lands.

**A guard runs before anything is compiled.**
[`Build/CheckProjectFiles.py`](Build/CheckProjectFiles.py) checks that every project file is
well-formed XML, that every source file is registered in both its `.vcxproj` and its `.filters`
and that nothing listed is missing from disk, that file names are unique repo-wide, and that no
engine project names the game, that no graphics header reaches the headless libraries, and that
the settings which must agree across the nine projects (§6) do agree.
Each of those fails, unguarded, at a point that names something other than the mistake — the step
exists because a `--` inside an XML comment cost a CI run and reported as nine identical `MSB4024`
errors, none of which mentioned the comment. Run it yourself before you push; it takes no arguments
and needs nothing but Python.

Both it and `CheckFormat.py` read the project list out of `Outpost.slnx` rather than spelling it,
so moving a project cannot make either of them check the wrong tree. That is not hypothetical: when
the test suites moved under `Tests/`, one of them failed naming the old location and the other
quietly skipped four projects and still reported success.

`build.log`, `test.log` and the TRX results are uploaded on every run, which is worth knowing
before you conclude a red job is a build failure: a failing test prints its assertion message
there rather than in the step summary.

**`clang-format` gates, in its own Linux job.** It only reads C++ as text, so it needs neither the
SDK nor a Windows runner, and what costs twenty seconds on Linux would be billed several times over
against Windows minutes. The version is pinned to 18.1.3; see §1.

**`clang-tidy` gates too**, scoped to GameLogic: the layer where its checks matter most and the
only one that does not reach the D3D12 headers. It still reaches C++/WinRT through `NeuronCore.h`,
and that projection is generated during the build, which is why the step runs after the build and
discovers the generated directory rather than assuming where it is.

It landed non-blocking and was promoted two runs later, which is the process working. **A finding
is therefore yours**: GameLogic came back clean, so anything the step reports arrived with your
change. One exception is worth knowing — the clang-tidy is whichever one Visual Studio ships on the
runner (LLVM 22 at the time of writing) and is **not** version-pinned, unlike clang-format. If a
run goes red on a day nobody touched GameLogic, compare the LLVM version line against the last
green run before assuming the code moved.

What the first sweep found is worth recording, because it is the argument for landing a linter
non-blocking rather than switching it on: 77 diagnostics, of which 46 were in generated C++/WinRT
headers admitted by a `HeaderFilterRegex` whose `.*` also matched
`<Project>/x64/Debug/Generated Files/`. The other 31 were real and all in the two files this
repository started with — parameters missing their `_`, a static member spelled `m_` instead of
`sm_`, and `bugprone-macro-parentheses` firing on `ENUM_HELPER`, where the macro argument is a
*type* and `static_cast<(T)>` does not compile. Filter tightened, naming fixed, false positive
silenced for that block with its reason rather than disabled for the tree. `ENUM_HELPER` and
its suppression have since gone: the macro had zero uses, and deleting dead code is a better
answer to a diagnostic than a `NOLINT` over it. The paragraph stands as the record of why a
linter lands non-blocking.

**Release|x64 is the only thing still not in CI.** Release only recently became a real release
build (see the settings above) and has no history of being green. Worth adding on the same
terms: non-blocking first, promoted on a clean run.

**Report what you actually did.** "Builds clean, not run" and "builds and runs the fleet-move
slice" are different claims. Never imply the second when you only did the first, and say which
configurations you built.

---

## 7. Working rules

- **Work arrives as a slice.** A design says what is being built; a work order says what one
  slice is, what it must not touch, and how "done" is decided. `Design/README.md` defines both.
  One slice is one branch and one pull request, answered in the template under `.github/`, and
  carries its own evidence: which configurations built, which suites ran, screenshots at two
  window sizes for anything visual, and every assumption or placeholder stated rather than left
  to be found.
- **One slice per layer at a time.** Slices in different layers can proceed in parallel because
  they share no files; two in the same layer collide on the project files, the umbrella header
  and this document, and the second to merge pays for it.
- Change the lines the task requires and no others. No drive-by reformatting, no opportunistic
  renames.
- New files follow §1's worked example: `#pragma once`, PascalCase filename, Allman braces, the
  right namespace.
- **Comments say why, not what.** The code already says what it does. A comment earns its place by
  recording the reason a decision went the way it did, the defect it prevents, or the constraint
  that is not visible from the call site.
- If a rule here blocks the task, say so in your report rather than quietly bending it.
- **A decision that shapes the tree gets a record** in `Design/Decisions/` (§9), in the same
  commit as the change it explains.

---

## 8. Before you hand work back

- [ ] Naming conforms to §1 — `_` on parameters, `m_` on class state, no `I`/`C`/`Base`/`Impl`
      prefixes or suffixes, units in names, `UPPER_CASE` on `constexpr`.
- [ ] Prose and new identifiers are US English (R11); a name extending a standing UK-spelled
      family matches that family.
- [ ] Files are PascalCase, flat, and unique repo-wide — including against the CRT and the STL.
- [ ] Every added, removed or moved file is in both the `.vcxproj` **and** the `.filters`, and
      `python Build/CheckProjectFiles.py` passes.
- [ ] `python Build/CheckFormat.py` passes, on clang-format 18.1.3.
- [ ] Shader touched? It is `<Name>VS.hlsl`, `<Name>PS.hlsl` or `<Name>CS.hlsl` under `Shaders/`,
      and nothing generated under `CompiledShaders/` was committed.
- [ ] The dependency rules in §2 still hold: no engine project names a game type, the client and
      server do not name each other, nothing names the executable, and no graphics header reaches
      NeuronCore or NeuronServer.
- [ ] No `argv`, no environment reads, no `XMVECTOR` stored in a struct or container, no `RH` call.
- [ ] GameLogic touched? The replay-equality test in `GameLogicTests` still passes, and nothing you
      added reads a clock, draws unseeded randomness, or keys on a pointer.
- [ ] New engine setting? It is MSVC-native (§6) — literal, per `Configuration|Platform`, no
      custom MSBuild — and it went into all nine `.vcxproj` files identically, for every
      configuration, with `python Build/CheckProjectFiles.py` agreeing.
- [ ] It builds — Debug at minimum — and you said which configurations you actually built.
- [ ] Tests for the layer you touched were run, and you said which.
- [ ] Your report states plainly what you verified, what you assumed, and any rule you bent.
- [ ] Moved a type between libraries, changed a dependency rule, added a project, or turned an
      alternative down that someone will propose again? There is a decision record for it (§9),
      the index lists it, and any sentence here it made false has changed.
- [ ] The pull request answers its template: work order, layers touched, out of scope,
      assumptions, evidence.

---

## 9. Architecture decision records

This file states the rules as they stand. It does not say what was tried first, what was turned
down, or under which assumption a rule was made — and a rule whose premise has quietly expired is
the most expensive kind, because it goes on being obeyed. That history lives in
`Design/Decisions/`, one short file per decision, in the format its `README.md` gives: context,
decision, alternatives considered, consequences.

When to write one — any of these, and in the same commit as the change:

- a type or file moves between libraries, or a library gains or loses a responsibility;
- a dependency rule in §2 changes, or a guard in `Build/` starts enforcing something new;
- a project, a third-party dependency, a toolset pin or a build setting is added or removed;
- an approach is rejected that a reasonable person will propose again.

Rules for the records themselves: numbered in order of writing and never renumbered, except when
two branches claimed the same number and meet — the one already on the trunk keeps it, the arriving
one moves up with its citations, in the merge commit; never edited into a different decision — a change of mind is a new record that supersedes the old one,
and the old one stays with its status changed; and the index in the README lists every one. A
record is a page, not a paper: if it needs more than that, the design document it came from is
where the rest goes, and the record links to it.

A record may reverse a sentence in this file. When it does, the sentence changes in the same
commit and the record is the place that says why — that is the folder doing its job, not a
contradiction to be smoothed over.
