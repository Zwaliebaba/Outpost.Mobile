# Work order — Collision slice 2b: the loopback transport

Implements slice 2b of [`Collision.md`](Collision.md) §19: the client half stops reading
`Game::World` and starts reading a snapshot that arrived through a `Transport`, with configurable
artificial latency and loss in the loopback.

**Layers:** `NeuronCore`, `GameLogic`, `Outpost`, and their suites. This is the slice §19 says
"wants the tree to itself", and it is the only one in this design that touches four layers at once.
**Depends on:** slice 3 (the pass structure the snapshot is taken between) and slice 0, whose
`ShipHandle` is what lets a snapshot name a ship without naming an array index.
**Blocks:** slice 6. Interest management is a filter on this pipeline and cannot start before it.

---

## 1. Why this is the important one

§15 is blunt about it, and the reasoning is the reason this slice is not last:

> The original table scheduled the first contact with the network at phase 6, which inverts the risk
> order: phases 1 through 5 are single-process work of a kind this tree has already done well, while
> the replication pipeline is the thing that has not been done at all.

It also converts three of this document's open questions from argument into measurement — §18 on
hard blocking between player ships, §10's claim that avoidance should be server-only and
unpredicted, §9's clamp calibrated against prediction error. None of those is answerable without
latency in the loop.

**It changes no gameplay.** A single-player build with zero configured latency must look and play
exactly as it does today; that is the acceptance bar, not a hope.

---

## 2. Scope

### 2.1 `LoopbackTransport` implements the declared seam

`NeuronCore/LoopbackTransport.h/.cpp`. `NeuronCore/Transport.h` already declares the interface and
already documents this exact plan; nothing in it changes.

Two independent queues — one per direction — so a single object can be handed to both halves with
each holding its own end. `Send` enqueues, `Poll` promotes what is due into the receive queue,
`Receive` copies the oldest out. A full queue returns `false` from `Send` and drops, which
`Transport.h` already says is normal and not an error.

**Latency is counted in ticks, not seconds.** This is the decision this slice most needs to get
right. A wall clock would make the transport's behaviour depend on how fast the machine ran, which
makes every measurement irreproducible and every test flaky, and `AGENTS.md` §5 bans a clock in the
simulation for exactly this reason. `Poll` takes the current tick; a datagram sent at tick `T` with
a configured latency of `L` becomes receivable at tick `T + L`. Reproducible, testable, and it turns
"120 ms of lag" into "7 ticks at 60 Hz" — which is what the measurement actually wants.

```cpp
struct Desc
{
  std::uint32_t latencyTicks = 0;      // 0 is the single-player default: same tick, no delay
  std::uint32_t capacityDatagrams = 256;
  std::uint32_t dropOneInN = 0;        // 0 disables; N drops every Nth datagram, counted not random
};
```

`dropOneInN` counts rather than randomises deliberately: `AGENTS.md` §5 bans unseeded randomness,
and a counted drop is reproducible, which is the only kind of loss worth testing against.

### 2.2 The wire format lives in `GameLogic`

`GameLogic/WorldSnapshot.h/.cpp`. It has to live somewhere, and the other three candidates are all
wrong:

- **`NeuronCore`** would have to name `ShipState` — zero game semantics, so no.
- **`NeuronServer`** never names `GameLogic`, and `Simulation.h` says so in its own comment. No.
- **`Outpost`** could, but then the executable owns the wire format and the day there are two
  executables it is in the wrong one.

`GameLogic` owns the types being encoded and already depends on `NeuronCore`, so it may include
`Transport.h`. This is the same test `AGENTS.md` §2 applies to content readers — the code lives with
what it is about — and it is worth saying in the record because someone will propose `NeuronCore`.

### 2.3 What a snapshot carries, field by field

**Not `ShipState` verbatim.** The seam's whole purpose is to make "what the client is allowed to
know" a named, reviewable list rather than whatever happens to be in a struct. Sending
`avoidHeadingRad` and `steerTargetPos` would tell any client exactly what every ship intends next,
which is a thing to decide once, here, rather than to discover later.

| Field | Why the client needs it |
|---|---|
| `ShipHandle handle` | Identity across ticks. **Not `ShipId`** — that is an array index and despawn moves it, which ADR 0005 says shows up as one ship interpolating into another while a player watches |
| `WorldPos posWorld`, `prevPos` | The two the view interpolates between |
| `float headingRad`, `prevHeading` | The same, for facing |
| `float speed`, `accelSample` | Thruster response and bank |
| `float turnRateRadPerSec` | Bank |
| `OrderState order` | The HUD shows it |
| `std::uint32_t hullId` | Which mesh |

Deliberately absent, and each is a sentence in the record if anyone wants it back:
`steerTargetPos`, `orderFacingRad`, `orderHasFacing`, `avoidHeadingRad`.

A snapshot header carries the **tick** and the **ship count**. The tick is what makes a stale or
out-of-order snapshot detectable, and out-of-order is not hypothetical the moment latency is
configurable.

### 2.4 Snapshots fragment, because one does not fit

`MAX_DATAGRAM_BYTES` is 1152. The record above is roughly 90 bytes packed, so a datagram holds on
the order of a dozen ships — fine for the three the game spawns, and nowhere near the 5,000 the
slice-2 benchmark sweeps. **Fragmentation is the substance of this slice**, not a detail to defer:
it is the first thing a real socket would need and the last thing anyone wants to retrofit under
interest management.

Each datagram carries `{snapshotId, fragmentIndex, fragmentCount, tick}` and a run of records. The
receiver reassembles by `snapshotId`; a snapshot missing any fragment when the next `snapshotId`
begins is **dropped whole**, never rendered partially. Half a snapshot is worse than a stale one:
stale looks like lag, partial looks like ships vanishing.

Reassembly holds at most one snapshot in progress. That is not a simplification to be embarrassed
about — with an ordered loopback there is never more than one, and the day the transport reorders,
one-in-progress degrades to "drop the older", which is what a snapshot pipeline should do anyway.

### 2.5 Orders travel the other way

The client currently calls `World::IssueMoveOrder` directly. It becomes a datagram: an order id, a
count, the handles, the destination `WorldPos`, and the facing. The server decodes and applies it.

`IssueMoveOrder` returns a heading today and `WorldView` uses it. Over a wire nothing returns, so
that value has to be either predicted client-side or waited for. **Waited for**, in this slice: the
client shows the order as pending until the next snapshot reflects it. Prediction is a slice of its
own and §10 is not yet decided about what should be predicted at all.

### 2.6 The client stops holding a `World&`

`WorldView::Init` takes a snapshot source rather than `Game::World&`; `Hud::Draw` likewise.
`WorldView.h`'s own comment already anticipates this — *"this is the class that stops holding a
`World&` and starts holding a snapshot buffer"* — and that sentence becomes true or changes.

The composition root owns both halves, one `LoopbackTransport` between them, and is the only place
that can still see both. Every phase in this design stays one `Outpost.exe` (§2); what changes here
is the code boundary, not the process boundary.

---

## 3. Out of scope

- **A socket.** No `winsock`, no ports, no `NeuronNet` project. The loopback is the whole of it.
- **Two processes.** §2 is explicit that every phase in this document runs one executable.
- **Client-side prediction and reconciliation.** The client renders what arrived, late. Making it
  predict is the next slice and needs §10 settled first.
- **Delta encoding and baselines.** Every snapshot is full. ADR 0005 already noted the baseline is
  where a stale id hurts; that is a note for when deltas land, not a licence to build them.
- **Interest management.** Slice 6. Every entity goes in every snapshot here, deliberately, because
  that is the thing slice 6 measures itself against.
- **Compression or quantisation.** Full fidelity means full fidelity; the payload shrinks in slice 6
  by sending less, not by sending it smaller.
- **Answering §18, §10 or §9's open questions.** This slice builds the instrument. Reading it is
  separate work, and pretending otherwise would smuggle three design decisions into a plumbing PR.

---

## 4. What to build on

| File | What it already gives you |
|---|---|
| `NeuronCore/Transport.h` | The interface, `MAX_DATAGRAM_BYTES`, `ConnectionState`, and a comment describing this slice |
| `NeuronServer/ServerHost.h` | The tick loop and `InterpolationAlpha()`, which is what the view interpolates by |
| `NeuronServer/Simulation.h` | The engine's whole knowledge of the game; it must stay that way |
| `GameLogic/ShipState.h` | `ShipHandle`, and the comment explaining why a wire uses one |
| `GameLogic/World.h` | `HandleOf`, `Resolve`, `Ships()`, `Tick()` |
| `Outpost/WorldView.h` | The comment naming this exact change |
| `Outpost/OutpostApp.h` | The composition root that will own the transport |

---

## 5. What will surprise the implementer

### 5.1 `ShipState` is 120 bytes, and that is the whole fragmentation argument

Measured, not estimated. Nine of them fill a datagram. The view record in §2.3 is about 90 bytes,
so a dozen fit — still two orders of magnitude short of the benchmark's 5,000. Anyone who decides
fragmentation can wait should run the numbers first.

### 5.2 A snapshot is not a `memcpy`

`WorldPos` now holds two `int64` and two `float` with padding (24 bytes), `ShipState` has an enum
and a `bool` among floats, and both will change again. Serialising field by field into a byte buffer
is what makes the format survive a struct change and what makes it portable the day the two ends are
different binaries. `AGENTS.md` §5's ban on stored `XMVECTOR` is the same instinct.

### 5.3 The view indexes by array position and the snapshot does not

`WorldView::m_ships` is parallel to `World::Ships()` and indexed the same way — that is written in
its header. A snapshot carries handles, so the view needs a handle-to-slot map of its own, rebuilt
or maintained as ships enter and leave. This is the first place ADR 0005's stable slot earns its
keep, and it is more work than it looks.

### 5.4 Zero latency must be genuinely zero

The single-player default is `latencyTicks = 0`, and it has to mean the snapshot taken this tick is
readable this tick — otherwise the game gains a frame of lag it did not have, and "changes no
gameplay" is false. `Poll` promoting due datagrams before the client reads, within the same tick, is
what makes that true. Get the ordering wrong and every test still passes while the game feels worse.

---

## 6. Decision records due

Two, at least:

- **The wire format lives in `GameLogic`** (§2.2). Someone will propose `NeuronCore` on the grounds
  that a transport is engine work; the record needs the three-way elimination in it.
- **The snapshot carries a view record, not `ShipState`** (§2.3), with the four omitted fields named
  and the reason — a client should not be told what every ship intends next.

A third if the tick-counted latency (§2.1) is argued: it is the sort of thing that looks like a
limitation until someone tries to reproduce a measurement with a wall clock in the loop.

---

## 7. Acceptance

**The transport**, in `NeuronCoreTests`:

- A datagram sent at tick `T` with `latencyTicks = 0` is receivable at tick `T`, and one sent with
  `latencyTicks = 7` is not receivable at `T + 6` and is at `T + 7`.
- Datagrams arrive in send order.
- A full queue returns `false` from `Send` and loses nothing already queued.
- `dropOneInN = 3` drops exactly every third datagram, and the same sequence twice drops the same
  ones.
- A datagram of exactly `MAX_DATAGRAM_BYTES` round-trips; one larger is refused rather than
  truncated.

**The format**, in `GameLogicTests`:

- A world of one ship round-trips: every field in §2.3's table comes back bit-identical.
- A world of 200 ships fragments across several datagrams and reassembles to the same 200, in order.
- A snapshot missing one fragment yields nothing — not a partial world.
- A snapshot whose `tick` is older than one already applied is ignored.
- A handle that despawned between snapshot and apply resolves to nothing rather than a stranger.
- An order datagram round-trips and applies to the same ships the client selected.

**Behaviour-neutrality**, which is the bar this slice is really judged on:

- With `latencyTicks = 0`, a scripted run through the transport produces positions bit-identical to
  the same run reading `World` directly, tick for tick.
- All existing GameLogic tests pass unchanged; they exercise the simulation, which this must not
  touch.

**The tree:**

- `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass, and the new files are
  registered in both the `.vcxproj` and the `.filters`.
- The dependency guard still passes: `NeuronCore` names no game type, `NeuronServer` names no game
  type, and nothing but `Outpost` names both halves.
- Debug|x64 green in CI including clang-tidy. **CI is the only gate on the client half.**
- **A screenshot at two window sizes is due** and this is the one slice where it genuinely matters:
  the claim is that the game looks identical, and only a picture can carry that.

**The documents:**

- §19 marks 2b `landed`, and the sentence about it wanting the tree to itself becomes past tense.
- `Transport.h`'s "NOTHING IMPLEMENTS THIS YET" is false the moment this lands and changes with it.
- `WorldView.h`'s comment about the class that will stop holding a `World&` becomes true or changes.
- `AGENTS.md`'s "`Transport` is declared and unimplemented (§2)" is false and changes.
- The records from §6 exist and the index lists them.

---

## 8. Assumptions the implementer may make

- **One client.** The loopback has two ends, not N. Multiple clients are a server concern and this
  slice does not have a server, only a half.
- **An ordered, reliable-by-default channel**, with loss only where `dropOneInN` puts it. Modelling
  reordering is worth doing when a socket exists and its absence is worth stating now.
- **No authentication, no connection handshake.** `ConnectionState` goes to `Connected` and stays;
  the state machine earns its keep when there is a network to lose.
- **Latency is uniform.** No jitter model. Jitter is what a real socket brings and a measurement
  against uniform latency is still a measurement.
- **The composition root may see both halves.** It is the only thing in the tree allowed to, and
  every phase here is one executable (§2).
