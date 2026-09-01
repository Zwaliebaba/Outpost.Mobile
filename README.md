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

You start with three hulls — a Bomber, a Corvette and a Frigate — as **Fleet 1**, under an RTS
camera over open space. A fleet is the unit of command here: you never hold a ship, you hold one of
five fleets, and everything below follows from that. You can:

- **Select a fleet** by tapping any of its hulls, by banding a box over it, or by tapping its button
  on the bar — which also flies the camera to it, wherever in the universe it is. Shift-tap adds a
  second fleet; there is no sub-fleet selection, on purpose.
- **Order** by tapping the ground with a fleet selected, or by dragging from the destination to set
  the facing the formation arrives on. The ships solve slots, path around architecture, separate
  from each other on the way, and stop without anything passing through anything. The whole fleet
  cruises at its slowest member's speed, so it arrives together.
- **Attack** by tapping a hostile record, and **dock** by tapping a station. A station whose owner
  holds you hostile refuses before the order is even sent.
- **Read a fleet** by holding its button: a sheet over the bar names the fleet, what it is doing,
  the hulls in it, and its four commands — `MOVE`, `ATTACK`, `DOCK`, `STOP`. The first three arm the
  next universe tap, which is how the verbs get names a tap alone could never teach.
- **Compose a fleet** by holding a station you are docked at: the assembly screen lists what you
  have inside, you draft up to eight hulls out of it into a free slot, and `LAUNCH` pours them out
  of the dock one at a time and forms them up outside. Docking a fleet dismantles it back into the
  ledger.
- **Watch a fleet defend itself.** Anything that attacks one rouses its combatants — the armed hulls
  turn on the attacker while the Miners and Haulers carry on with their orders — and its button and
  minimap digit pulse red for as long as the alert holds. Anything that shoots one fires it: a
  landed hit is what states the act.
- **Fight.** An armed hull shoots what its fleet was ordered at, what struck it, or the nearest
  thing its faction already holds hostile — whichever is first in reach. A shot lands when geometry
  says it does: in range, inside the mount's arc, and with the turret's aim settled, so a heavy
  turret genuinely loses a fighter crossing close aboard and holds one at three hundred metres.
  Muzzle flashes and tracers in the shooter's own colours say who fired at whom, the fleet sheet's
  condition pips say how much is left, and a hull at zero shatters. Nothing rolls dice.
- **Read the universe** through the HUD: minimap with the sector pair, your fleets' digits on it
  wherever they are, contact count, event log, and a function rail whose screens are not built yet.

A second faction lives 1.2 km northeast: a Vandal station with three Interceptors walking a ring
around it. They are drawn red, they count as contacts, and they cannot be selected or ordered — the
simulation refuses an order from the wrong faction, and the client does not offer one. They are also
armed, and they shoot: fly a fleet inside a hundred and sixty metres of one and it opens fire, your
combatants answer, and the fight runs itself. Their helms are still a metronome — they walk the ring
whatever happens — because guns react in this game and courses do not.

The government is here too. The starting solar system is laid out from a seed, and at each of its
three planets stands a Core Vanguard Command station — azure in the scene, a hollow diamond on the
minimap from the first frame, and the place your ships can dock. The Vanguard takes anyone who has
not attacked it; order an attack on one of its ships or its stations and it stops taking you — the
law turns red across the map, and the provoked station launches Corvette protectors that hunt the
aggressor until it dies.

Behind all of it: three worlds rendered as an authored equirectangular map — baked to BC with a
full mip chain — on smooth spheres, six procedurally generated asteroids, and a seeded star field
of stars, dust clouds and a galactic band expanded into billboards on the GPU. Every body is carried
at three levels of detail and drawn at the one its size on screen asks for. All of it presentation
— a ship flies straight through a rock.

### Keys

| Key | What it does |
|---|---|
| `Esc` | Closes the assembly screen, then the fleet sheet, then the selection; quits once nothing is left |
| `F1` | Debug readout |
| `F3` | Camera shake (tuning hook) |
| `F4` | Shatters the selected hulls into tumbling debris, a fireball and smoke (tuning hook) |
| `F5` | Reseeds every body's look and the sky — the worlds and their stations hold still |
| `1` `2` `3` | Time scale: quarter, normal, quadruple |

Input is `WM_POINTER`, so mouse and touch are the same path: drag with the second or third button to
move the camera, pinch and twist with two contacts to zoom and turn.

### Deliberately not here yet

So nobody goes looking: no economy, no audio, no save file, no mining, and no turret that turns —
a hull's guns fire and its geometry holds still. Combat is here and is one number deep: hull points,
one damage figure per device, and no shields, armour classes or resistances. Tuning is `constexpr`
in `SimTuning.h`, `HullSpec.h`, `DeviceSpec.h` and `ViewTuning.h`; what a deployment may change
without a rebuild lives in `Outpost/Assets/Server.cfg`. The networking is real QUIC over a real
stack, and it is still one client in one process on `127.0.0.1` with a self-signed certificate the
client does not validate.

**The universe is authored, not discovered.** `Tools/UniverseGen` writes a `Universe.sav` and
`Outpost` runs what it finds beside itself; a first checkout has to run the tool once before the game
will start, and the game says so if you forget. The two are separate executables in separate output
directories, so give the tool the game's path — `UniverseGen 0 "…\Outpost\Assets\Universe.sav"` —
and `UniverseGen --help` spells it out. A different first argument writes a different galaxy from the
same code (ADR 0058).

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
| `GameLogic/` | The deterministic simulation, namespace `Game`: universe, movement, collision, formation, pathfinding islands, gunnery, the wire format and the publisher |
| `NeuronClient/` | Everything that names a graphics type: D3D12 device, scene and text renderers, the planet and star-field pipelines, the explosion FX, the content readers |
| `NeuronServer/` | The authoritative half — `ServerHost` and the `Simulation` interface it drives |
| `Outpost/` | The executable: composition root, presentation state, HUD, the fleet sheet and the station assembly screen, and `Outpost/Assets/` |

The simulation ticks at a fixed rate, is bit-identical across two runs of the same seed, and depends
on nothing but `NeuronCore`. The client sees the universe only as snapshots that arrived over the wire,
filtered to what one subscriber can see, and sends orders back up the same wire. The engine
libraries never name the game; `Outpost.exe` injects the game through interfaces the engine
declares. `Build/CheckProjectFiles.py` checks the parts of that a build can check.

Renderer specifics: D3D12, shader model 6.7 DXIL compiled by DXC, one overlay pipeline for the HUD,
textured FX pipelines for the explosion, a two-pass body pipeline, an additive sky pass, and hulls
authored as GLB and shipped as the tree's own NMO format.

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
