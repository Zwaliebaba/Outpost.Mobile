# Work order — Shard server slice 2: sessions

Implements slice 2 of [`ShardServer.md`](ShardServer.md) §7 — the listener, and one `Publisher`
subscriber per connection. **One shard, one session, no links.**

**Layer:** `Server`, reaching `NeuronCore`, `NeuronServer` and `GameLogic`.
**Depends on:** slice 1 ([`ShardServer-slice-1.md`](ShardServer-slice-1.md)), landed 2026-09-02.
**Blocks:** slice 4 (two shards, two processes) and slice 5 (the client redirect).

---

## 1. What this slice is for

Slice 1 proved the run loop does not want a window. It also produced a server **nobody can watch**,
which is the one thing about it that is not a design choice. This slice makes it watchable: a client
dials the port, the shard adds a subscriber, and the same snapshots the game already sends itself
over loopback go over a real wire to another process.

The claim it tests is narrower than it looks, and it is worth stating precisely, because almost
everything here already works:

> **`Game::Publisher` does not know how many subscribers it has, and the seam does not know whether
> its far end is in this process.**

Both have been asserted since ADR 0030 and ADR 0008 and neither has been tested, because the only
thing that had ever driven a `Publisher` was an executable with exactly one subscriber, living in the
same process as its client. If either claim is false, it fails here.

## 2. Scope

1. **`ShardSession`** — what one connection is: the `QuicTransport*` the listener accepted, the
   `Publisher::Handle` it was given, and nothing else. It is a struct, not a class: it owns nothing,
   because the listener owns the transport and the publisher owns the subscriber.

2. **The publisher wiring MOVES into `ShardSimulation`**, it is not copied. `ShardSimulation.h` says
   so in its own comment and slice 1 §7 says it is the first place this design could rot: the game's
   `UniverseSimulation` owns a `Publisher` because it has one client, and the shard's must own one
   because it has zero or more. The tick becomes, in this order and the order is the design:

   ```
   ApplyOrders  ->  DrainInbox  ->  Step  ->  SetCentre per session  ->  Publish
   ```

   The drain stays where slice 1 put it — between the orders and the step — because ADR 0065's
   determinism argument is about the step, not about the orders.

3. **`ShardApp::Boot` opens the listener** after the universe and before the run loop: `QuicApi::Open`
   then `QuicListener::Start(port, {backlog})`, both out of `Server.cfg` (ADR 0043). A refused port
   is a **third boot failure with no fallback**, said the way slice 1's two are said and exiting
   non-zero. `Outpost` falls back to a loopback because there is a window and a person in front of
   it; a server with no port serves nobody and should say so and stop.

4. **The run loop polls the listener once per pass**, not once per tick: accepting is not a tick's
   work and a pass that ran no ticks must still accept. A newly accepted transport gets
   `Publisher::Add`; a transport the listener has recycled gets `Publisher::Remove`. Both are said on
   stdout with the tick they happened on, because a server's log is the only thing anybody can watch.

5. **One session, and the second is refused.** The owner's decision of 2026-09-02: there is no login,
   so every session would be `OWNER_LOCAL` and two clients would fight over one fleet. The shard
   accepts the first connection and closes any other with a stated reason. `backlog` stays
   configurable and stays honest — it is how many the listener can *hold*, and this slice is what
   decides how many it will *serve*.

6. **A session's interest centre is the centroid of its own fleets.** The owner's decision of the
   same day. The server cannot read a camera, and the publisher already computes exactly this number
   for the status block it stamps on every update (`UniverseSnapshot.h`'s `FleetStatus::position`),
   so this costs no new machinery, no new wire message and no new determinism surface. Its known
   failure — a player whose camera is over empty space away from their fleets — is documented in
   `Outpost/UniverseSimulation.h` and is slice 5's, where the session gains a camera.

## 3. Out of scope

- **Links between shards.** Slice 3. Nothing here opens a `ShardLink` or names a neighbour.
- **A second client.** Refused, per §2.5, and the refusal is the feature.
- **A login, an account, or an authenticated owner.** `OWNER_LOCAL` is still the one player, and the
  refusal above is what keeps that honest rather than a comment saying it.
- **Publishing off the tick thread.** `GameDesignPlan.md` phase 2 slice 15, and it wants this to
  exist first.
- **A new wire message of any kind.** `UNIVERSE_STATE_FORMAT` does not move this slice. If it has
  to, that is a finding and this order was wrong.
- **The client connecting to it.** `Outpost` still hosts its own universe (`ShardServer.md` §5.2) and
  is not touched. A client that dials this server is slice 5.

## 4. How it must behave

1. A shard with no client **ticks exactly as slice 1 did**, to the byte. A publisher with zero
   subscribers must cost nothing but a branch, and the replay gate is what says so.
2. A client that connects is added, and **what it receives is what the game receives itself** — the
   same snapshots, the same interest filtering, the same status block.
3. A client that disconnects is removed, its slot is recycled, and the shard goes on ticking. A
   server that dies when a client leaves is the failure this is most likely to have.
4. A second client is refused, with a sentence naming why, and the first is undisturbed.
5. A refused port stops the boot, non-zero, naming the port.
6. It still allocates nothing per tick after boot, with a session attached.
7. Nothing in `Server/` names a graphics type. `CheckProjectFiles.py` holds it.

## 5. Acceptance

- **Run it, and connect to it.** The acceptance is two processes: the shard on its port, and a client
  that receives snapshots from it. Until slice 5 gives `Outpost` a `--connect`, the client is a test
  harness — and that is not a weaker acceptance, because what it proves is that the far end of the
  seam is a socket and not a pointer.
- The replay gate, unchanged and green: a shard with no session must tick identically to slice 1's.
- `GameLogicTests` green. `Publisher` gains no behaviour this slice; if it gains any, that is scope
  §3 said no to.
- `CheckProjectFiles.py`, `CheckFormat.py`, clang-tidy over the new sources.
- The decision record, if §6 turns up something worth one.

## 6. What to watch for

- **The publisher wiring duplicating instead of moving.** The first and most likely rot. If
  `ShardSimulation` and `UniverseSimulation` end this slice both owning a `Publisher` with the same
  five lines of `Desc` filling, that is two implementations of ADR 0030 and the day one changes is
  the day they disagree.
- **A pointer held across a `QuicListener::Poll`.** The listener's own header warns of it: an
  accepted transport belongs to the pool again once the Poll that finds it closed has run, so a
  `ShardSession` holding one must be reconciled against `Accepted()` every pass rather than trusted.
- **The refusal leaking.** Closing a second connection must recycle its slot, or a server refuses
  one client and then has one fewer slot for ever.
- **A per-tick cost that scales with sessions times ships.** The centroid is per session and the
  interest set is per session; at one session and 307 ships neither is measurable, and the slice
  should say what it measured rather than assume it stays that way.

## 7. Assumptions the implementer may make

- **`QuicListener` needs no change.** It pre-allocates its transports, recycles closed slots, and
  reports what it bound. If that turns out to be false it is this slice's finding.
- **`Publisher` needs no change.** `Add`, `Remove`, `SetCentre` and per-subscriber interest are all
  there and were built for exactly this (ADR 0030).
- **`ServerConfig` needs no new field.** Port, backlog, interest radius, interest cadence and orders
  per tick are all already in it and all already read by slice 1.

## 8. What changed on contact

- **The centroid moved into `Universe` rather than being written twice.** §2.6 said the number the
  status block derives is the number a session should follow, and the only honest way to say that is
  to have one function derive it. `Universe::TryCentreOfFleet` and `TryCentreOfOwnedFleets` are new;
  `WriteFleetBlock` calls the first instead of averaging inline. **The wire did not move**: 172,851
  bytes over 4,000 ticks with the same FNV-1a hash before and after, compared against a worktree of
  the commit this slice started from.
- **`live` had to be counted separately after that.** The status block's count byte is
  live-members-plus-manifest, and the loop that used to produce the centroid also produced the count.
  They are different facts about a fleet and the block now asks for them separately — four lines,
  and clearer than the coincidence it replaced.
- **Both publisher cursors are opened at the head, and the game only opens one.**
  `UniverseSimulation::Connect` sets `openingDespawnCursor` and leaves `openingShotCursor` at zero,
  which is harmless there because the game's one client connects on tick 0 and both logs are empty.
  A client joining a **running** shard at zero would receive the entire shot log the universe still
  holds in its first update. `ShardSimulation::OpenSession` opens both, per ADRs 0027 and 0053. This
  is not a defect in the game and is not fixed there; it is a difference the slice found and states.
- **The listener is opened last in `Boot` and closed first in `Run`'s exit.** A port bound before the
  universe is read is a port held by a process about to exit; and a session publishing into a
  transport being torn down is the shutdown bug this ordering exists to not have.
- **A refused connection is still polled.** Every transport the listener calls live is polled,
  including one this server has refused and asked to close — because a transport nobody polls never
  finishes its close, and its slot is never recycled. That would make a server that refuses one
  client permanently one slot smaller.
- **A refusal had to be remembered, or it repeats sixty times a second.** The first version logged
  `SESSION REFUSED` and called `Close()` on every pass, because a refused transport stays in the
  listener's `Accepted()` for several passes while its close finishes. Caught by reading the diff
  rather than by running it — the harness below drives `ShardSimulation` and never reaches this
  loop, and on CI it would have been a log with thousands of identical lines in it. The refused
  transports are now held in a scratch list reconciled against `Accepted()` exactly as the sessions
  are, and reserved from the same backlog so it still allocates nothing per pass.
- **`Server` needed the MsQuic package, and CI is what said so.** Slice 1 included `QuicListener.h`
  transitively (through `ServerConfig.h`) and never called anything behind it, so nothing pulled
  `QuicApi.obj` out of `NeuronCore.lib`. Slice 2's `QuicApi::Open` does, and `Server.exe` failed to
  link with two unresolved MsQuic symbols after compiling cleanly and passing every check in
  `Build/`. `CheckProjectFiles.py` now holds it: a project that links -- static libraries are exempt,
  because a `.lib` is not linked -- and that **directly** includes `QuicApi.h`, `QuicListener.h` or
  `QuicTransport.h` must carry the package and import its targets. Transitive includes are
  deliberately not counted, or `GameLogic` and every suite that reaches the umbrella would be told to
  carry MsQuic they never call. Both halves of the failure were planted and confirmed caught.
- **`Server/` has no test project, and slice 2 is the first slice where that costs something.**
  Slice 1's logic was a boot sequence and a run loop, and running the program *was* the test. The
  session logic is ordinary testable code — open, refuse, close, reopen — and it is verified below by
  a scratch harness rather than by a suite that runs in CI. A twelfth project is not in this order's
  scope; it should be in slice 3's, where links add more of the same kind of logic.

## 9. What was verified, and how

Everything below was **run** against the real `ShardSimulation` and the real `GameLogic`, with
`LoopbackTransport` where CI has QUIC — which is exactly the substitution `Neuron::Transport` exists
to make, and the reason the seam was cut there (ADR 0008).

**Fifteen rows, all passing:**

```
a shard with no session ticks byte-identically to slice 1's      pass
the first session is taken                                       pass
and the shard says it has one                                    pass
the second session is refused                                    pass
and the refusal left the first alone                             pass
the shard knows which wire its session is on                     pass
and knows the refused one is not a session                       pass
the session receives what the game receives itself               pass
and the refused connection receives nothing at all               pass
a tick with a session attached allocates nothing                 pass
closing the session removes it                                   pass
and the shard is back to none                                    pass
closing it twice says so rather than corrupting anything         pass
and the shard went on ticking after the client left              pass
a new client after the first left is served                      pass
```

The session received **86,439 bytes over 2,000 ticks** and the refused connection received **zero**.
A tick with a session attached made **0 allocations over 2,000 ticks**, which is §4.6.

**The centre decision does real work, and the shipped universe at rest cannot show it.** The first
version of this check was vacuous: the player has one fleet, it starts on the origin, and with no
order it is still there 4,000 ticks later — so the derived centre and the origin were the same
number and every comparison passed for the wrong reason. Ordered 15 km out and ticked 40,000:

```
after 40000 ticks the centre moved 11975 m east and 8981 m north, to 11975, 8981
interest radius 2000 m: the derived centre holds 3 of the player's 3 ships, the origin holds 0
```

**`GameLogicTests` unchanged and green**: 339 rows run, and the one that differs is the same
clang-versus-MSVC divergence slice 1 recorded — a 5,500-tick fleet fight ending 103 ticks earlier
under clang, identical on the commit where CI was last green.

**The gap, stated.** `ShardApp::OpenListener` and `ShardApp::PumpSessions` are the two functions here
that were compiled but **not run**: MsQuic is a Windows package in this tree, so `QuicApi`,
`QuicListener` and `QuicTransport` link on CI and nowhere else. What that leaves untested is the
accept path, the handshake gate, the slot recycling and the close — which is to say, everything
between a socket and a `Neuron::Transport&`. Everything above that line is exercised above. This is a
larger gap than slice 1's console handler and it is named rather than waved past: the acceptance §5
asks for — two processes, one connecting to the other — is a Windows manual check, and it is the
first thing slice 4 will do for real.
