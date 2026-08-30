# Outpost: Frontier

A real-time fleet game in open space, for Windows, built on the **Neuron** engine libraries in this
same tree. You command ships rather than a ship: select them, order them somewhere, and watch them
route around what is in the way, give way to each other, and arrive in formation.

The repository is `Outpost.Mobile` and the executable, its project and its namespace are all spelled
`Outpost` — those are identifiers, and only what a person actually reads carries the full title.

---

## The game

Known space has a government, and it has people who ignore it. **Core Vanguard Command** — the
Vanguard — is the civic power: its stations sit at the planets, they take anyone who has not shot at
them, and they answer an attack by launching protectors that pursue the attacker until the matter is
settled. The **Vandal Collective** is the other answer to the same frontier. Between the two is the
player's fleet, and a solar system that does not much care which of them wins.

That is the direction. What is *built* is narrower, and this file says which is which rather than
describing an ambition as a feature.

### What you can do today

You start with three hulls — a Bomber, a Corvette and a Frigate — under an RTS camera over open
space. You can:

- **Select** by tapping a hull, shift-tapping to add one, or banding a box over several.
- **Order** by tapping the ground with a selection, or by dragging from the destination to set the
  facing the formation arrives on. The fleet solves slots, paths around architecture, separates from
  its own members on the way, and stops without anything passing through anything.
- **Keep control groups** — five of them, on the HUD buttons: tap to recall, hold to assign.
- **Read the world** through the HUD: minimap with the sector pair, contact count, event log, and a
  function rail whose screens are not built yet.

A second faction lives 1.2 km northeast: a Vandal station with three Interceptors walking a ring
around it. They are drawn red, they count as contacts, and they cannot be selected or ordered — the
simulation refuses an order from the wrong faction, and the client does not offer one. They are also
a metronome. They do not react to you, because nothing in this game can yet shoot at anything.

Behind all of it: one world rendered as an authored equirectangular map on a smooth sphere, six
procedurally generated asteroids, and a seeded star field of stars, dust clouds and a galactic band
expanded into billboards on the GPU. All of it presentation — a ship flies straight through a rock.

### Keys

| Key | What it does |
|---|---|
| `Esc` | Drops the selection; quits once nothing is selected |
| `F1` | Debug readout |
| `F3` | Camera shake (tuning hook) |
| `F4` | Shatters the selected hulls into tumbling debris, a fireball and smoke (tuning hook) |
| `F5` | Reseeds every body and the sky — a different scene, and the same one again after a restart |
| `1` `2` `3` | Time scale: quarter, normal, quadruple |

Input is `WM_POINTER`, so mouse and touch are the same path: drag with the second or third button to
move the camera, pinch and twist with two contacts to zoom and turn.

### Deliberately not here yet

So nobody goes looking: no combat, no damage model, no economy, no audio, no save format, no
configuration file — tuning is `constexpr` in `SimTuning.h`, `HullSpec.h` and `ViewTuning.h`. The
networking is real QUIC over a real stack, and it is still one client in one process on `127.0.0.1`
with a self-signed certificate the client does not validate.

---

## How it is put together

Two halves that talk over a wire, in one process, on purpose — so that the day they are two
processes, nothing has to be rewritten to make it work.

```
                NeuronCore
               /     |     \
      NeuronClient  GameLogic  NeuronServer
               \     |     /
                  Outpost.exe
```

| Project | What it is |
|---|---|
| `NeuronCore/` | Engine primitives, headless and with zero game semantics: diagnostics, file IO, the frame clock, a seeded PCG32, and the `Transport` seam with its MsQuic implementation |
| `GameLogic/` | The deterministic simulation, namespace `Game`: world, movement, collision, formation, pathfinding islands, the wire format and the publisher |
| `NeuronClient/` | Everything that names a graphics type: D3D12 device, scene and text renderers, the planet and star-field pipelines, the explosion FX, the content readers |
| `NeuronServer/` | The authoritative half — `ServerHost` and the `Simulation` interface it drives |
| `Outpost/` | The executable: composition root, presentation state, HUD, and `Outpost/Assets/` |

The simulation ticks at a fixed rate, is bit-identical across two runs of the same seed, and depends
on nothing but `NeuronCore`. The client sees the world only as snapshots that arrived over the wire,
filtered to what one subscriber can see, and sends orders back up the same wire. The engine
libraries never name the game; `Outpost.exe` injects the game through interfaces the engine
declares. `Build/CheckProjectFiles.py` checks the parts of that a build can check.

Renderer specifics: D3D12, shader model 6.7 DXIL compiled by DXC, one overlay pipeline for the HUD,
textured FX pipelines for the explosion, a two-pass body pipeline, an additive sky pass, and hulls
authored as OBJ and shipped as the tree's own NMO format.

---

## Building

Visual Studio 2026 (toolset `v145`, `stdcpplatest`). `Debug|x64` is the configuration that is built
and run; treat anything else as unverified.

```
nuget restore Outpost\packages.config -PackagesDirectory packages    (and one per project)

msbuild Outpost.slnx /p:Configuration=Debug /p:Platform=x64

vstest.console.exe x64\Debug\NeuronCoreTests.dll x64\Debug\GameLogicTests.dll ^
                   x64\Debug\NeuronClientTests.dll x64\Debug\NeuronServerTests.dll
```

The projects use `packages.config`, so `msbuild -t:restore` does nothing for them — restore is
`nuget restore` per config file. CI builds `Debug|x64` and runs all four suites on every pull
request and every push to `main`, and it gates.

A good boot prints `LINK | QUIC` in the event log. There is no fallback link: a boot that cannot
open the wire names the stage that refused and stops.

---

## Where the documents are

- [`AGENTS.md`](AGENTS.md) — the conformance rules. Read it before writing a line here; they are
  not defaults, and nothing in this tree is grandfathered.
- [`Design/`](Design/) — designs with a slice still open, and [`Design/README.md`](Design/README.md)
  for how an idea becomes a design, a work order, a branch and a merge.
- [`Design/Decisions/`](Design/Decisions/) — the architecture decision records: what was chosen,
  what lost, and why. Read the record before re-proposing the thing it turned down.
- [`Design/Archive/`](Design/Archive/) — designs whose slices have all landed, and the work orders
  that landed them. Archived is not retired: they are still what their area is reviewed against.
- [`Tools/`](Tools/) — the NMO mesh codec, its Blender add-on and the OBJ converter, stdlib Python.
