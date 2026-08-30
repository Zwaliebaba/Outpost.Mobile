# Work order — QUIC transport slice 2: the composition root boots over QUIC

Implements slice 2 of [`QuicTransport.md`](../QuicTransport.md) §13: `Outpost.exe` opens a
`QuicListener` and a `QuicTransport` to it on `127.0.0.1`, wires the simulation and the view to the
two QUIC ends, and falls back to the loopback pair with a logged reason when it cannot (design
§6, §12 decision 2).

**Layer:** `Outpost` only, plus `AGENTS.md`'s description of what is here.
**Depends on:** slice 1 (landed and archived).
**Blocks:** nothing scheduled. Slices 3a/3b wait on this having been lived with.

---

## 1. Why this is a slice

Slice 1 proved the transport in a suite; this slice makes every run of the game cross it, so a
break in the stack is seen the day it happens and not the day a second machine is tried. The
change is confined to `OutpostApp` — the simulation, the format, the interest set and the
renderer do not learn which transport they got, and the diff must show that: no file in
`GameLogic/`, `NeuronServer/`, `NeuronClient/` or `NeuronCore/` is touched.

---

## 2. Scope

### 2.1 `Outpost/OutpostApp.h` — the members

Beside the existing `m_serverLink` / `m_clientLink` (which stay — they are the fallback and the
instrument):

```cpp
Neuron::QuicApi m_quic;
Neuron::QuicListener m_listener;
Neuron::QuicTransport m_clientQuic;
Neuron::QuicTransport* m_serverQuic = nullptr; // the listener's, once accepted; null on the fallback
bool m_linkIsQuic = false;
```

The header comment at `OutpostApp.h:76–79` ("The two halves meet only at the transport … one
executable and stays one") gains a sentence: the transport is QUIC over localhost when it can be,
the loopback when it cannot, and the game cannot tell which.

### 2.2 `Outpost/OutpostApp.cpp` — the constant and the boot

```cpp
// The port the in-process server listens on and the in-process client dials, on 127.0.0.1 only.
// Arbitrary and unregistered. Here rather than in a config file because there is no config file
// (AGENTS.md 5), beside the loopback's latency knob for the same reason: if the port is taken,
// boot says so and runs on the loopback, so the number is never the reason the game did not start.
constexpr std::uint16_t OUTPOST_QUIC_PORT = 30081;
```

`Init`, replacing the block at `OutpostApp.cpp:108–115`, as design §6 steps 1–5, in a private
`bool OpenQuicLink()` that returns false with the reason already logged:

1. `QuicApi::Desc quicDesc; quicDesc.allowUnvalidatedPeer = true;` — with the comment that this
   is the development placeholder ADR 0020 records. `m_quic.Open(quicDesc)` false →
   `LINK | QUIC UNAVAILABLE | <Reason()>`, return false.
2. `m_listener.Start(m_quic, OUTPOST_QUIC_PORT, {})` false → `LINK | PORT %u REFUSED | <Reason()>`,
   return false.
3. `m_clientQuic.Connect(m_quic, {"127.0.0.1", OUTPOST_QUIC_PORT}, {})` false → log, return false.
4. Pump: loop `m_listener.Poll(); m_clientQuic.Poll();` plus `Poll` on the accepted transport
   once `Accepted()` is non-empty, until the client is `Connected` and the accepted end is
   `Connected`, or `QUIC_HANDSHAKE_TIMEOUT_MS` elapses on `m_clock` (`FrameClock::Now` /
   `ElapsedMs`). Timeout → `LINK | HANDSHAKE TIMED OUT | client %s server %s` with the two states'
   names, `m_clientQuic.Close()`, `m_listener.Stop()`, return false.
5. `m_serverQuic = m_listener.Accepted()[0]; m_linkIsQuic = true;` log
   `LINK | QUIC | 127.0.0.1:%u | %.1f MS` with the handshake time. Return true.

Then, in `Init`:

```cpp
Neuron::Transport* serverEnd = &m_serverLink;
Neuron::Transport* clientEnd = &m_clientLink;
if (OpenQuicLink())
{
  serverEnd = m_serverQuic;
  clientEnd = &m_clientQuic;
}
else
{
  LoopbackTransport::Desc linkDesc;   // the existing block, comment and all
  linkDesc.latencyTicks = 0;
  LoopbackTransport::Connect(m_serverLink, m_clientLink, linkDesc);
  m_log.PushFormat(EventLog::Severity::Alert, 0.0f, "LINK | LOOPBACK");
}
m_simulation.Connect(*serverEnd);
m_view.Init(*clientEnd, m_camera, m_meshes, m_sceneRenderer.UnitQuad());
```

The `LINK |` lines use the same `PushFormat` and the same shape as `FLEET ONLINE | %u SHIPS`
(`OutpostApp.cpp:155`), so they show in the HUD's event log at boot. `Severity::Friendly` (green)
for the QUIC line, `Severity::Alert` (amber) for every fallback line — `EventLog::Severity` has
only `Info`, `Friendly` and `Alert`, and none is added.

### 2.3 `Run` and `Shutdown`

- `Run` (`OutpostApp.cpp:409–451`) changes nothing: `AdvanceTo` on the loopback pair is harmless
  when it is unconnected, and `m_view.PumpNetwork()` plus `WorldSimulation::ApplyIncomingOrders`
  already call `Poll()` on whatever transport they hold. Do not add a QUIC-specific pump.
- `Shutdown` (`OutpostApp.cpp:506`), before `m_gpu.Shutdown()`, when `m_linkIsQuic`:
  `m_clientQuic.Close(); m_listener.Stop(); m_quic.Close();` — in that order, because a
  registration cannot close over a live connection (design §6). No logging in a shutdown path.

### 2.4 `AGENTS.md`

- "What is actually here": `Transport` has a QUIC implementation over MsQuic and the game boots
  on it across `127.0.0.1`, falling back to the loopback with a logged reason.
- "Deliberately not here yet": "no networking" and "`Transport` has a loopback implementation and
  no socket" go; in their place: one client, one process, no certificate validation, no reliable
  lane yet (`Design/QuicTransport.md` §11).
- §2 "Where the client/server seam stands today": the paragraph beginning "`NeuronCore/
  Transport.h` declares the seam" gains the QUIC sentence and loses "what remains is a socket".
- §5 external libraries: "MsQuic … no code references it yet either" becomes a statement that it
  is the transport.

### 2.5 What this slice deliberately does **not** do

- Touch any file outside `Outpost/`, `AGENTS.md`, `Design/`.
- Add a HUD element. The event log line is the indicator; a `LINK` readout on the HUD is a
  separate, later ask.
- Make the port, the fallback or the credential configurable. There is no configuration file.
- Remove `LoopbackTransport` from the root: it is the fallback and the lag instrument
  (`OutpostApp.cpp:108–111`).

---

## 3. What to build on

| File | What it already gives you |
|---|---|
| `Outpost/OutpostApp.cpp:104–117` | the `ServerHost` init, the loopback block this replaces, `m_simulation.Connect`, `m_view.Init` |
| `Outpost/OutpostApp.cpp:155`, `:339` | `PushFormat` and the `FLEET ONLINE` line — the shape of a boot line |
| `Outpost/OutpostApp.cpp:409–451`, `:506` | `Run` (unchanged) and `Shutdown` (three lines) |
| `NeuronCore/QuicApi.h`, `QuicTransport.h`, `QuicListener.h` | `Desc`s, `Open`/`Start`/`Connect`, `Reason()`, `Accepted()`, `State()` — slice 1's surface, archived at `Design/Archive/QuicTransport-slice-1.md` |
| `NeuronCore/FrameClock.h` | `Now()`, `ElapsedMs()` for the bounded handshake wait |
| `NeuronCore/Transport.h` | the `ConnectionState` names for the timeout line |
| `Design/QuicTransport.md` §4.3, §6 | the numbers and the five boot steps |
| ADR 0020 | why `allowUnvalidatedPeer` is set and where that is written down |

---

## 4. Acceptance

There is no `Outpost` test suite (ADR 0014 says why), so acceptance is what a screen and a log can
decide:

- **Screenshots at two window sizes** (1600×900 and one other), each showing the event log with
  `LINK | QUIC | 127.0.0.1:30081 | N MS` and `FLEET ONLINE | 7 SHIPS`, the fleet selectable and
  orderable, the hostiles patrolling. Play for a minute: no ghost ships, no stutter beyond
  today's; F4 still explodes a selected ship.
- **The fallback, on purpose.** Hold port 30081 with another process (a second `Outpost.exe`
  suffices — the first one has it), start the game: the log shows `LINK | PORT 30081 REFUSED |
  …` then `LINK | LOOPBACK`, and the game plays identically. One screenshot.
- **The handshake time** in the pull request, from the log line. Under 100 ms is expected on
  localhost; over `QUIC_HANDSHAKE_TIMEOUT_MS` is the fallback, and if that happens on the
  implementer's machine, it is a finding, not a pass.
- **Shutdown is clean**: closing the window exits without a hang and without an `ASSERT_TEXT`
  from `QuicApi`'s destructor.
- **`git diff --stat`** shows paths only under `Outpost/`, `AGENTS.md`, `Design/`.
- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass; all four suites
  green and unchanged.
- No decision record is due: nothing moves, no rule changes, no dependency is added.
- `Design/QuicTransport.md` §13 marks slice 2 `landed`; this file moves to `Design/Archive/`.

---

## 5. Assumptions the implementer may make

- **The handshake completes inside one boot** on any machine the game already runs on; the wait
  is a boot-time bounded spin, not a state machine spread over frames. If the owner wants a
  connecting screen, that is a later design.
- **Boot order is unchanged otherwise**: hulls, fleet, hostile base and bodies load after the
  link exactly as they do today, so `FLEET ONLINE` follows `LINK` in the log.
- **The loopback pair stays constructed** even when unused; it costs two 288 KB rings and nothing
  else, and keeping it means the lag instrument still works by editing the one line.
- **`Severity::Alert`** is right for the fallback lines. A fallback is not an error — the game
  runs — but it is something the player should notice, and amber is what the log has for that.
