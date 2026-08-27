# Outpost — how to work here

Conformance rules for anyone, human or agent, writing code in this repository.

Outpost is a Windows game built on the **Neuron** engine libraries. The engine and the game are
separate on purpose: `Neuron*` is meant to carry more than one game, and the day it stops being
able to is the day it stopped being an engine. Most of the rules below exist to hold that line.

**This tree is greenfield. Nothing is grandfathered** — the rules apply to every line.

Read this file before you start. Where a rule blocks the task, say so in your report rather than
quietly bending it.

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
| Macro | `SCREAMING_SNAKE` | `ASSERT`, `ENUM_HELPER` |
| Namespace | `PascalCase` | `Neuron`, `Game`, `Outpost` |
| File | `PascalCase.cpp` / `.h` | `ServerHost.cpp` |

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
`cooldownMs`, `dragThresholdPx` are encouraged — this game measures a plane in metres, a wire in
centimetres, and time in ticks, so unit ambiguity is a real defect class. Never encode the type:
no `iCount`, `pShip`, `strName`, `dwFlags`.

**R7 — A file is named for its primary type**, PascalCase, `.h` / `.cpp` only. `.hpp`, `.cc` and
`.inl` are not used; template implementations live in the header. Two exceptions: the per-project
`pch.h`/`pch.cpp` keep the name MSBuild expects, and a file holding one *family* takes the
family's name (`RenderTypes.h`, `GpuHelpers.h`, `SimTuning.h`). Formats get hand-written parsers
— there is no generated code in this tree and none is to be introduced.

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

### Worked example — this is the target style

```cpp
// NeuronCore/UdpTransport.h
#pragma once
#include <cstdint>

namespace Neuron
{

inline constexpr std::uint32_t MAX_DATAGRAM_BYTES = 1152; // R3: constant -> UPPER_CASE

enum class ConnectionState : std::uint8_t { Connecting, Connected, Draining, Closed };

/// Loopback/plaintext transport. R2: no prefix on the type; R8: private state carries m_.
class UdpTransport
{
public:
  struct Desc                                             // R8: aggregate -> plain fields
  {
    std::uint16_t port;
    std::uint32_t receiveBufferBytes;                     // R6: unit in the name
  };

  [[nodiscard]] static bool Create(const Desc& _desc,      // R1: _ on parameters
                                   UdpTransport& _outTransport) noexcept;

  void Poll() noexcept;                                   // delivery on the owning thread
  [[nodiscard]] ConnectionState State() const noexcept { return m_state; }

private:
  ConnectionState m_state = ConnectionState::Closed;
  std::uint64_t m_bytesSent = 0;
};

} // namespace Neuron
```

### Enforcement

[`.clang-tidy`](.clang-tidy) is the machine-readable statement of the table and is the **single
source of truth for the option values** — this document states the rules in prose and does not
repeat the settings, so there is nothing to drift. [`.clang-format`](.clang-format) owns
whitespace; the two never overlap.

**Neither runs in CI yet — there is no CI in this repository** (§6). Until there is, run
clang-tidy yourself on the files you wrote, not on the tree:

```
clang-tidy --quiet NeuronCore/YourNewFile.cpp -- -I . -D _WIN32 -D _DEBUG /std:c++latest
```

Two rules `.clang-tidy` structurally cannot state, so check them by eye:

- **R2's suffixes.** clang-tidy can require an *absent* prefix but cannot ban a *present* suffix,
  so `FooBase` slips through it entirely. A bare `struct Impl;` is the pimpl idiom and is not a
  suffix on anything; `FooImpl` is.
- **R7's file naming**, and that a new file is registered in the `.vcxproj` **and** the
  `.filters` (§3). Registering it in one and not the other is the miss §8's checklist exists to
  prevent.

---

## 2. Repository map and the dependency rules

| Path | What it is |
|---|---|
| `NeuronCore/` | Engine primitives shared by every layer — zero game semantics, no graphics API. Diagnostics, file IO, framerate-independent easing, the frame clock, mesh data, the OBJ/MTL parser, and `Transport`. |
| `GameLogic/` | The deterministic simulation, namespace `Game`. `World`, `ShipState`, `Movement`, `Formation`, `SimTuning`. Depends on NeuronCore only. |
| `NeuronClient/` | The presenting half — `AppWindow`, `PointerTracker`, `Camera`, `GpuDevice`, `SceneRenderer`, `TextRenderer`, `MeshLibrary`. |
| `NeuronServer/` | The authoritative half — `ServerHost` and the `Simulation` interface it drives. |
| `Outpost/` | The executable: composition root, presentation state, HUD, boot and shutdown ordering. `Outpost/Assets/` is the content the MSIX package deploys. |
| `*Tests/` | VS CppUnitTestFramework suites, one per library. |
| `Outpost.Toolset.props`, `Outpost.Compile.props` | Build settings shared by every project (§6). |

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

### Where the client/server seam stands today

`Outpost.exe` runs both halves in one process, and `WorldView` reads `Game::World` **directly**
rather than through a snapshot. That is deliberate and current: this repository has established
the one-way dependency, not the wire.

`NeuronCore/Transport.h` declares the seam and **nothing implements it**. It is there so the seam
is a named thing with an owner rather than a plan. When the halves separate, `WorldView` stops
holding a `World&` and starts holding a snapshot buffer; nothing else in the client changes, which
is the whole point of the layering. Do not shortcut it in the meantime: if you find yourself
wanting the simulation to call into the renderer, or the renderer to write to the world, that is
the seam telling you the change belongs somewhere else.

---

## 3. Files, layout and includes

- **Flat project directories.** All of a project's `.h`/`.cpp` sit directly in its folder. No code
  subdirectories — grouping lives in `.vcxproj.filters` only.
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
- **Every added, removed or renamed file updates both** the `.vcxproj` and the `.vcxproj.filters`
  of its project, in the same commit.

---

## 4. Layout and formatting

[`.clang-format`](.clang-format) is the authority; [`.editorconfig`](.editorconfig) repeats only
what an editor needs before the first save. The shape: **Allman braces, 2-space indent, 140
columns, no tabs, `namespace` contents not indented, pointer binds left** (`ID3D12Device*
_device`).

Include order is **not** sorted automatically and is grouped by hand: `pch.h` first, then this
project's headers, then the headers of libraries it depends on, then SDK headers, then the
standard library. A formatter reordering these behind your back is a correctness risk, not a style
preference.

Format the lines you write. Do not reformat files you are only passing through.

---

## 5. C++ rules for this codebase

- **C++ latest** (`/std:c++latest` on VS 2026, `stdcpp20` on 2022), MSVC v145, `ConformanceMode`
  on, `/fp:precise`, no `/arch`. Do not turn conformance off to make something compile. **None of
  these are spelled in a `.vcxproj`** — they live in `Outpost.Compile.props`, which every project
  imports (§6). If you add a project, import both sheets; their two positions are not
  interchangeable.
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
  state to the main thread. Foreign threads (msquic workers, XAudio2 callbacks) enqueue to a ring
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
  (Win32, Winsock2, D3D12/DXGI, DirectXMath, XAudio2/X3DAudio), the packages already in
  `packages.config`, and C++/WinRT as above. If you believe a third-party library is justified,
  present the case and **stop** — do not assume approval.

---

## 6. Build and verify

```
msbuild Outpost.slnx /p:Configuration=Debug   /p:Platform=x64
msbuild Outpost.slnx /p:Configuration=Release /p:Platform=x64
vstest.console.exe x64\Debug\NeuronCoreTests.dll x64\Debug\GameLogicTests.dll ^
                   x64\Debug\NeuronClientTests.dll x64\Debug\NeuronServerTests.dll
```

x64 is the configuration that is built and run. Win32 and ARM64 exist in the project files but are
not exercised; the solution maps `*|ARM64` onto the x64 libraries, which will not link against an
ARM64 executable. Treat anything other than x64 as unverified.

### The two property sheets

Every project imports both, and **no project spells a centralised setting itself**:

- **`Outpost.Toolset.props`** — imported at the top of each `.vcxproj`, **ahead of
  `Microsoft.Cpp.Default.props`**, because a toolset and a configuration flavour have to be decided
  before the platform defaults fill them in. Holds `PlatformToolset`, the target platform version,
  `CharacterSet`, and `UseDebugLibraries` per configuration.
- **`Outpost.Compile.props`** — imported from each project's `PropertySheets` group, **after
  `Microsoft.Cpp.props`**. Holds the language standard, `ConformanceMode`, warning level,
  `/fp:precise`, the precompiled-header settings, and the per-configuration optimisation and
  preprocessor settings.

The two positions are not interchangeable. If you add a project, import both.

This is not tidiness. Before the sheets existed, all seven projects set `UseDebugLibraries=true`
and defined `_DEBUG` in **Release** as well as Debug, so a Release build was a Debug build under
another name and any number measured in it was measuring the wrong binary. A setting spelled per
project drifts, and the one it drifts on is always the one that mattered.

### There is no CI

Nothing in this repository builds it but you. `.clang-format` and `.clang-tidy` are enforced by
review, and the test suites run only when someone runs them. Until that changes, the checklist in
§8 is the only gate there is — which is worth knowing before you rely on something catching a miss
for you.

**Report what you actually did.** "Builds clean, not run" and "builds and runs the fleet-move
slice" are different claims. Never imply the second when you only did the first, and say which
configurations you built.

---

## 7. Working rules

- Change the lines the task requires and no others. No drive-by reformatting, no opportunistic
  renames.
- New files follow §1's worked example: `#pragma once`, PascalCase filename, Allman braces, the
  right namespace.
- **Comments say why, not what.** The code already says what it does. A comment earns its place by
  recording the reason a decision went the way it did, the defect it prevents, or the constraint
  that is not visible from the call site.
- If a rule here blocks the task, say so in your report rather than quietly bending it.

---

## 8. Before you hand work back

- [ ] Naming conforms to §1 — `_` on parameters, `m_` on class state, no `I`/`C`/`Base`/`Impl`
      prefixes or suffixes, units in names, `UPPER_CASE` on `constexpr`.
- [ ] Files are PascalCase, flat, and unique repo-wide — including against the CRT and the STL.
- [ ] Every added, removed or moved file is in both the `.vcxproj` **and** the `.filters`.
- [ ] The dependency rules in §2 still hold: no engine project names a game type, the client and
      server do not name each other, nothing names the executable.
- [ ] No `argv`, no environment reads, no `XMVECTOR` stored in a struct or container, no `RH` call.
- [ ] GameLogic touched? The replay-equality test in `GameLogicTests` still passes, and nothing you
      added reads a clock, draws unseeded randomness, or keys on a pointer.
- [ ] New engine setting? It went in a property sheet, not a `.vcxproj`.
- [ ] It builds — Debug at minimum — and you said which configurations you actually built.
- [ ] Tests for the layer you touched were run, and you said which.
- [ ] Your report states plainly what you verified, what you assumed, and any rule you bent.
