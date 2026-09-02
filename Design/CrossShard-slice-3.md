# Work order — Cross-shard slice 3: both queues in the state codec

Implements slice 3 of [`CrossShard.md`](CrossShard.md) §8 — the outbox and the inbox join the saved
state, so a shard that dies mid-handoff still holds the ship.

**Layer:** `GameLogic`.
**Depends on:** slice 2, which landed 2026-09-02.
**Blocks:** slice 4. A re-send loop over a queue that does not survive a restart is a re-send loop
that cannot recover from the failure it exists for.

---

## 1. The hole this closes, and why it is the whole slice

[ADR 0065](Decisions/0065-a-handoff-is-at-least-once-delivery-onto-an-idempotent-apply.md) claims a
property the build does not yet have. Its own consequences say so:

> **The outbox is in memory this slice, so a shard that dies loses it.** That is the hole slice 3
> fills by putting both queues in the state codec, and it is named rather than hidden: until then the
> recoverability this record claims is a property of the design and not yet of the build.

The design's §4 is equally specific: an outbox entry is written *in the same tick as the despawn*,
**and part of the saved state** — "a shard that dies after the despawn and before the send still has
the ship, in its file, in the outbox — which is the property that makes 'lost' recoverable rather
than final." Take the file away and at-least-once delivery degrades to at-most-once, which is the
one failure mode the whole scheme was chosen to avoid.

So this slice adds no behaviour. It makes an existing claim true.

## 2. Scope

1. **`UNIVERSE_STATE_FORMAT` 8 → 9**, with both queues written after the fleet block and read behind
   a gate on the format byte (ADR 0061). A format-8 file loads with two empty queues, which is
   exactly what it meant: it was written by a build that could not have had anything in them.

2. **`m_nextHandoff` is saved too**, and that is the field a reader will skip past if it is not
   argued for. It is the sequence an acknowledgement addresses an entry by. Left out, a shard that
   reloaded would restart at 1 and re-use numbers the far side has already acked — so a re-send
   after a reload would be acknowledged by an ack for a *different* ship. It is one `u64` against a
   silent mis-delivery in slice 4.

3. **A `Handoff` is written field for field, not memcpy'd.** It holds an `EntityId`, two
   `FactionId`s, a `u8`, three `u32`s and two `u64`s; a struct write would put its padding in the
   file and make two identical universes compare unequal, which is the argument `Route` already
   makes about waypoints past `count`.

4. **Both queues are bounded on read** against `in.Remaining()`, like every other count in this
   format, so a corrupt length asks for one allocation of a bounded size.

5. **The fixture for format 8**, `UniverseFormat8.sav`, committed with its row in `FIXTURES` — the
   rule AGENTS.md §12 states for every bump. **This one can be produced honestly**, unlike the file
   format 1 → 2 bump slice 1 had to bend: the tool writes format 9 after this change, so the format-8
   file is what the tool at the parent commit writes, and the parent commit is on this branch.

6. **Prose in the same commit**: `CrossShard.md` §4's outbox bullet stops saying "as of slice 2 and
   joins the state codec in slice 3", ADR 0065's consequence is superseded by naming it rather than
   edited, and §8's slice 3 row records what landed.

7. **No decision record.** Putting an existing field in the format it belongs in decides nothing;
   ADR 0061 already decided how a format grows.

## 3. Out of scope

- **Anything on a wire.** Slice 4.
- **The re-send loop.** It needs a transport that can lose a message. `AcknowledgeHandoffs` stays
  called by nothing.
- **Trimming the outbox.** An entry stays until acknowledged, which is the design, and there is
  nothing to acknowledge it yet. A queue that grew without bound would be a bug in slice 4's loop,
  not in this format.
- **The inbox surviving a *delivery* it never drained.** It does, and that falls out of saving it
  rather than being a separate thing to build.

## 4. How it must behave

1. A universe with entries in either queue, saved and reloaded, comes back with the same entries in
   the same order, and drains to the same universe as one that never saved.
2. **A format-8 fixture loads, keeps its census, and replays to byte equality** — the row slice 1
   built the migration reader for, now with a second format behind it.
3. A universe with both queues empty writes the same bytes it did before this slice, plus the two
   counts and the sequence. Nothing else in the format moves.
4. `UNIVERSE_STATE_FORMAT_OLDEST` does not move. Format 7 is still readable, which is the claim
   ADR 0061 makes about retiring a format only by record.
5. Both replay gates stay green: the handoff queues are state, so a universe that saves mid-handoff
   and reloads must step identically to one that did not.

## 5. Acceptance

- `UniverseStateTests`: a mid-handoff save and reload, byte-identical on re-save; the format-8
  fixture row; the empty-queue case unchanged.
- `HandoffTests`: a crossing interrupted by a save and a reload still arrives, whole, in its slot —
  which is the slice's title claim as a test.
- The whole suite green; `CheckProjectFiles.py`, `CheckFormat.py`, `CheckViewAccess.py`,
  clang-tidy over `GameLogic`.

## 6. Assumptions the implementer may make

- **The migration reader already works.** Slice 1 built it and format 8 exercised it; this is the
  second field-adding bump and needs no new mechanism.
- **A `Handoff` holds no handle.** Every field is a value that means the same thing in another
  process, which is what lets it be written flat and put on a wire unchanged in slice 4.

---

## 7. What changed on contact

- **Nothing, in the design.** This is the rare slice that added no shape: two vectors and a `u64`
  joined a format that already knew how to grow, behind the gate ADR 0061 built. The interesting work
  was in the tests, which is what a slice whose whole purpose is "make an existing claim true" should
  look like.
- **`m_nextHandoff` is saved, and §2.2 is why.** It is the field a reader skips past. Without it a
  reloaded shard restarts its sequence at 1 and re-uses numbers the far side has already
  acknowledged, so a re-send after a reload would be cleared by an ack for a *different* ship. One
  `u64` against a silent mis-delivery in slice 4.
- **A `Handoff` is written field by field**, never as a struct. It has a `u8` between two `u32`s and
  would carry three bytes of padding into the file, which two identical universes could differ in —
  the argument `Route` already makes about waypoints past its count.

## 8. What was verified, and how — and the honest gap

Eleven rows **run**, not merely parsed: eight in `UniverseStateTests` and the three from
`HandoffTests` that this slice could have broken, all executed against the real `GameLogic` through
an asserting stand-in for the test framework.

```
AUniverseMidHandoffSurvivesItsOwnSaveFile                pass
AnUndeliveredInboxSurvivesTooAndDrainsToTheSameUniverse  pass
AnEmptyQueueCostsTheFormatOnlyItsCounts                  pass
AnOlderFixtureLoadsAndReplays                            pass    <- both fixtures, 7 and 8
TheNewestFixtureIsTheToolsOutput                         pass
AMalformedSaveFileIsRefusedAndChangesNothing             pass
ASavedUniverseReplaysToTheSameRun                        pass
TheShardCountSurvivesTheHeader                           pass
```

The slice's title claim, as a row: a four-shard galaxy, the fleet ordered through a gate that leads
out, stepped until the outbox holds three — then the shard is *killed*, reloaded from its bytes, and
the recovered outbox is delivered. Three ships spawn on the far side and the fleet re-forms in its
slot. The inbox half is tested from the other end: delivered but not drained, saved, reloaded, and
both copies drain to byte-identical universes.

**The fixture is a clang artifact, and that is a bend worth stating.** AGENTS.md §12 wants
`UniverseFormat8.sav` to be what the tool writes at the parent commit, and the tool only runs on
Windows. What makes it sound here rather than merely convenient: no row compares a *retired* format's
fixture to the tool's output — `TheNewestFixtureIsTheToolsOutput` skips any fixture behind the current
format, which format 8 now is. What is asserted about it is its size, its two format bytes and its
census, and every one of those is fixed-width or a count. A compiler difference in this format can
only be a float value, never a length. Both fixtures were inspected to confirm the shape:

```
format 7: file byte 1, header 23, state byte 7, 124 438 bytes   <- MSVC, from slice 1
format 8: file byte 2, header 27, state byte 8, 126 906 bytes   <- clang, this slice
```

**The gap:** `AcknowledgeHandoffs` is still called by nothing, so an outbox grows without bound in a
build that hands off and never acks. That is not a defect in this format — there is no transport to
acknowledge anything yet — but it is the first thing slice 4 must not forget, and a queue that only
ever grows is now a queue that only ever grows *in the save file*.
