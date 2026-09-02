# Work order — Shard server slice 3: links

Implements slice 3 of [`ShardServer.md`](ShardServer.md) §7 — a `ShardLink` per neighbouring shard,
and `NoteDurableThrough` after each save. **Still one process; slice 4 is what makes it two.**

**Layer:** `Server`, `GameLogic`, and **`Tests/ServerTests` (new)**.
**Depends on:** slices 1 and 2.
**Blocks:** slice 4 (two shards, two processes) and, through it, slice 5.

---

## 1. What this slice is for

`ShardLink` has existed since [`CrossShard-slice-4.md`](CrossShard-slice-4.md) and has never been
owned by anything. It was built and tested against a pair of `LoopbackTransport`s driven by a test;
this slice gives it a home in a running program, which means answering three questions the tests
never had to:

1. **Which shards does this one link to?** Not a list somebody maintains (ADR 0063).
2. **When is a save durable, and who says so?** [ADR 0066](Decisions/0066-an-acknowledgement-means-durable-not-delivered.md)
   says the composition root does, because a universe does not know it is ever saved.
3. **Where in the pass does a link pump?** The tick order is already argued; this adds to it.

## 2. Scope

1. **`Game::NeighbourShardsOf(const Universe&, std::span<ShardId> _out)`** — the shards this one has
   a gate to, derived from the universe's own gates: `EntityShardOf(gate.destination)` for every
   gate, deduplicated, sorted, excluding itself.

   **It is derived from the save, not from the layout**, and that is stronger than what
   `ShardServer.md` §3.4 asked for. A server that re-derived the partition would need the galaxy seed,
   the pins and `GalaxyDesc`, and would disagree with its own file the day any of them drifted. The
   gates are already in the file and already say where they lead (ADR 0056).

   Measured before writing this order, on the shipped galaxy at one to five shards:

   ```
   2 shards: 0↔1
   3 shards: 0↔1↔2
   4 shards: 0↔1↔2↔3
   5 shards: 0↔1↔2↔3↔4
   ```

   **The neighbour graph is a path, not a mesh**, and every shard has at most two neighbours. That
   falls out of ADR 0063's contiguous cut and is worth stating because of what it costs: links scale
   **linearly** with shard count, not quadratically. The row that guards it should assert the
   property links actually depend on — **symmetry**, that if A names B then B names A — rather than
   the path shape, which is a fact about this galaxy and not about the partition.

2. **`Shard::ShardLinks`**, a header beside `ShardSimulation.h`: one `Game::ShardLink` and one
   transport pointer per neighbour, built once at boot from §2.1. `Pump` each of them once per pass,
   beside `PumpSessions` and for the same reason — it is not a tick's work.

3. **`NoteDurableThrough` after the save, and only after.** `ShardApp::SaveUniverse` calls it on every
   link with the tick it just wrote, and on the path where `WriteFileAtomic` **refused** it does not.
   That refusal path is the whole of ADR 0066: a save that did not happen makes nothing durable, and a
   link told otherwise would ack a fleet into a file that does not contain it.

4. **A twelfth project, `Tests/ServerTests`** — the owner's decision of 2026-09-02, reversing the note
   in [`ShardServer-slice-2.md`](ShardServer-slice-2.md) §8 that left it open. The tree's convention
   is one suite per *library* and neither `Outpost` nor `Tools/UniverseGen` has one — but the reason
   `Outpost` has none is that its logic is D3D12- and WinRT-bound and untestable off a device, and
   that reason does not apply here.

   **It shares no source file with `Server`.** `ShardSimulation.h`, `ShardSession.h` and the new
   `ShardLinks.h` are header-only, so the suite includes them with `Server\` on its include path and
   `CheckProjectFiles.py`'s unique-name rule is not bent. That is a design principle this slice makes
   explicit and later slices must keep: **the testable half of `Server/` is header-only, and
   `ShardApp.cpp` is the root that is run rather than tested.**

   Slice 2's fifteen scratch rows move into it first, unchanged, so the project lands carrying
   something known to pass.

## 3. Out of scope

- **Two processes, and any address at all.** How a shard finds where its neighbour is listening —
  a port convention, a table in `Server.cfg`, or something else — is **slice 4's decision** and must
  not be taken here. This slice builds the links and pumps them over transports it is handed.
- **A gate that leads to a shard nobody is running.** `CrossShard.md` §6's unreachable-shard refusal
  lands with slice 5, by the owner's decision of 2026-09-02.
- **Changing `ShardLink`.** It was built for this and needs nothing. If it turns out to, that is this
  slice's finding.
- **`Outpost`.** Untouched, and still its own in-process server.

## 4. How it must behave

1. A shard whose galaxy has one shard **builds no links at all** and ticks byte-identically to slice
   2's. One shard has no neighbours, and the count is what says so.
2. Neighbours are **symmetric**: if shard A's gates name B, B's name A. At every shard count the
   partition supports.
3. A save calls `NoteDurableThrough` on every link with the tick written; **a refused save calls it on
   none of them.**
4. A fleet crossing between two universes in one process, over a pair of loopback transports, arrives
   exactly once and is not acknowledged until the arriving side has saved — which is slices 2, 3 and 4
   of `CrossShard.md` re-proved through the server's own wiring rather than through a test's.
5. Still nothing per tick after boot.
6. `ServerTests` runs in CI, on the same terms as the other four suites.

## 5. Acceptance

- `ServerTests` green in CI, carrying slice 2's fifteen rows plus this slice's.
- `GameLogicTests` green, with the neighbour rows added there — `NeighbourShardsOf` is `GameLogic`'s
  and is tested where `GameLogic` is tested.
- `CheckProjectFiles.py` with the twelfth project registered and its settings agreeing with the other
  eleven, `CheckFormat.py`, `CheckViewAccess.py`, clang-tidy over the new sources.
- The replay gate unchanged.

## 6. What to watch for

- **Adding the project is the risky part, not the code.** The eleventh cost this branch two CI cycles
  — a `Tests\`-depth package path and a missing MsQuic package, both now held by
  `CheckProjectFiles.py`. Land `ServerTests` carrying only slice 2's known-passing rows, get it green,
  and add slice 3's logic after.
- **`NoteDurableThrough` on the refusal path.** The easy mistake is to call it once at the end of
  `SaveUniverse` regardless, which reads fine and quietly undoes ADR 0066.
- **A link pumped inside the tick.** `Pump` is per pass, like the listener. Pumping it per tick would
  put a transport's timing inside the replay contract.
- **The neighbour derivation reaching for the layout.** It does not need one, and a version that takes
  a `GalaxyLayout` would make the server able to disagree with its own save.

## 7. Assumptions the implementer may make

- **`QuicListener` and `Publisher` need no change**, as slices 1 and 2 found.
- **The neighbour set is stable for the life of a process.** A shard's gates do not change while it
  runs, so the links are built once at boot and never rebuilt. The day a gate can be built or
  destroyed, this is the sentence that stops being true.
- **A link with no transport is a link that does nothing**, not an error. Slice 3 builds the links;
  slice 4 is what gives them somewhere to send.

## 8. What changed on contact

- **`ShardLink` sent the whole outbox to every peer, and that is wrong the moment a shard has two
  neighbours.** §7 of this order assumed `ShardLink` needed no change and said that if it did, it was
  this slice's finding. It did.

  `Universe::Outbox()` is **one queue for every destination**, and `ShardLink::Pump` passed all of it
  to `WriteHandoffs`. With two shards that is correct by accident — every entry is for the one peer
  there is — which is why four slices of `CrossShard` tests, all written against two shards, never
  saw it. With three the middle shard borders two others, and each would have been handed the other's
  entries: handoffs naming a gate that shard does not have.

  The fix is one argument and four lines. `Pump` takes the peer's `ShardId` and sends only the entries
  whose `EntityShardOf(gate)` matches — a handoff already names the gate it **arrives** at, in the
  arriving shard's table (ADR 0056), so nothing new had to be carried to know where an entry is for.
  The peer is handed in per pump rather than held, for the same reason the universe and the transport
  are.

  **The guard bites.** `HandoffTests::ALinkCarriesNothingThatIsNotForItsOwnPeer` flies a real
  crossing, then pumps two links out of the departing shard — one to the destination and one to a
  shard that is not it — and asserts the second sends nothing and puts nothing on its wire. Restoring
  the old behaviour fails that row and only that row:

  ```
  with the pre-slice-3 send-everything:  342 rows run, 2 failed
  with the filter:                       342 rows run, 1 failed   (the pre-existing clang matchup row)
  ```

  It was tempting to write this row by putting two synthesised entries in one outbox, and that would
  have meant adding a mutator to `Universe` for a test to use. The outbox is written by `StepJumps`
  and by nothing else, and it should stay that way — so the row states the property instead: a real
  outbox, a link to the shard it is for and a link to a shard it is not.
- **`NeighbourShards` is `Universe`'s, not the layout's.** §2.1 asked for the neighbour set to be
  derived from the layout, as `ShardServer.md` §3.4 did. It is derived from the **gates**, which is
  strictly better: the server never loads a layout, so it cannot disagree with its own save. The
  design's sentence is the one that was imprecise, not this order's.
