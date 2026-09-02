# Shard server — a universe with no window

**Status: drafted 2026-09-02, not yet agreed. §5 puts three decisions to the owner and none of them
should be taken by an implementer. Nothing is cut.**

The player-facing sentence is that there isn't one: **nobody sees this, and that is the point.**
What a player eventually gets from it is a universe that is still there when they close the game,
and a frontier larger than one machine.

---

## 1. Why this document exists at all

Two designs and eight slices now end at the same sentence.

- `CrossShard.md` §8: *"Slice 5 waits on something no design in this tree owns yet: a headless
  process. The seam, the configuration file and the shard in the save are all in place, and what is
  missing is a root that runs a universe without a window."*
- `GameDesignPlan.md` phase 2: *"Every slice here waits on the headless run loop and the
  dedicated-server root, which belong to no plan yet."* — seven slices, including the governor, the
  session layer, publish-off-the-tick-thread and recovery.

Building it inside one of those slices would take a decision eight slices depend on as a side effect
of one, with no record. So it gets a design, and the owner agreed that on 2026-09-02.

## 2. What is already there, and it is nearly everything

This is a **composition root**, not a system. Almost every part exists and has existed for slices:

- **`Neuron::Simulation`** — the engine's whole knowledge of the game: `Step` and `Tick`, no
  `GameLogic` header anywhere near it (`Simulation.h` says so at length).
- **`Neuron::ServerHost`** — the fixed-rate accumulator, with a catch-up bound so a stall does not
  spiral. It already *is* the headless run loop; nothing about it wants a window.
- **`Outpost/UniverseSimulation`** — the adapter that makes a `Universe` a `Simulation`, with a
  comment that has been waiting for this document since it was written: *"A dedicated server would
  call `Publisher::Add` once per session instead, which is the whole of what changes there."*
- **`ServerConfig` and `Server.cfg`** — port, backlog, interest, orders per tick, save cadence,
  read by the composition root alone (ADR 0043).
- **`UniverseGen` writing one file per shard**, and a save that carries its shard and refuses a file
  whose header and body disagree (ADR 0057, 0058, 0063).
- **`ShardLink`**, and a handoff that crosses a transport, acknowledges when durable, and re-sends
  until it does (ADR 0065, 0066).
- **QUIC, a listener, a reliable lane**, and a boot that fails rather than falling back (ADR 0028).

**What is missing is an executable and the ordering inside it.** That is genuinely all — which is
why this design is short and why its risk is not "can it be built" but "does it duplicate `Outpost`".

## 3. The shape

**A console executable, `Server/`, that is a composition root and nothing else.**

It owns, in this order, and the order is the design:

1. Read `Server.cfg`. Refuse to start on a bad one, naming the line — `Outpost` already does this.
2. Read `Universe.<shard>.sav`. **No genesis, no fallback**: ADR 0058 gave authoring to a tool, and a
   server that could invent a universe is a second thing that can author one.
3. Open the listener. Accept sessions; one `Publisher::Add` per session, which is `UniverseSimulation`'s
   own prediction.
4. Open a `ShardLink` per neighbouring shard. Which shards those are is a function of the layout and
   the partition, not a list somebody maintains (ADR 0063) — the same argument, one level up.
5. Run: `ServerHost::Advance` → `Step` per tick → drain the inbox at the tick boundary → publish →
   pump each link → save on the cadence → `NoteDurableThrough` after the save, and only after
   (ADR 0066).
6. Stop on a signal, saving once on the way out.

**`Outpost` keeps its own root and is not refactored to share one.** §5.1 puts that choice; the
draft's position is that two roots with one library under them is the tree's existing shape and
the duplication is a dozen lines of ordering, where a shared root would need to abstract over "has a
window" — which is exactly the seam ADR 0008 cut and ADR 0037 defended.

## 4. What it deliberately is not

- **Not a matchmaker, a gateway or a lobby.** One client, one connection, to the shard its camera is
  in (`CrossShard.md` §7). A gateway multiplexing several shards is the answer the day a player wants
  to watch two at once, and nothing here forecloses it.
- **Not authoritative about content.** It runs a universe a tool wrote.
- **Not a place for game rules.** Every rule is `GameLogic`'s; this is ordering and lifetime.
- **Not multi-threaded.** Publish-off-the-tick-thread is phase 2's slice 15 and wants this to exist
  first, not to be designed into it now.
- **Not a Windows service, and not a container image.** How it is deployed is a question for the day
  something deploys it.

## 5. Decisions this design puts to the owner

**None of these should be taken by an implementer**, and each is needed before the slice that names
it.

1. **One root or two?** A separate `Server/` executable beside `Outpost`, or one root with the
   window made optional. The draft says two, for ADR 0008's reason and because "optional window" is
   a condition that ends up threaded through boot, input, render and shutdown. The cost of two is a
   dozen lines of ordering duplicated, and a discipline that they must not drift.

2. **Does the game executable keep its in-process server?** Today `Outpost` hosts its own universe
   and talks to it over QUIC on loopback (ADR 0021, 0028). It could keep that for single player, or
   single player could become "launch a shard server and connect to it". Keeping it is simpler now
   and means two ways to start a game for ever; dropping it makes every session identical and makes
   the first run harder.

3. **Which shard does a client connect to first, and who tells it?** §7 says the shard its camera is
   in — but a client that has not connected has no camera position it trusts. Options: a fixed
   home shard in the client's config; the save's shard; or a redirect from any shard to the right
   one. The draft leans to the last, because it is the only one that stays true when a player's
   fleet has moved and it is what a reconnect on a crossing needs anyway.

## 6. What would make this design wrong

- **If the run loop turns out to want a different shape without a window** — if `ServerHost`'s
  accumulator only makes sense driven by a frame — then the engine's headless claim was never true
  and slice 1 will say so before anything else is built on it.
- **If two composition roots drift**, the duplication argument in §3 fails and the shared-root option
  in §5.1 was right. The check is whether a change to boot ordering ever has to be made twice and is
  made once.
- **If a shard's neighbours cannot be derived from the layout**, ADR 0063's argument does not extend
  upward and links become configuration — which is a table somebody maintains, and the thing that
  record refused.

## 7. Slices

Cut one at a time, when each is next.

| # | Slice | Layer | Size | Depends on | ADR |
|---|---|---|---|---|---|
| 1 | The executable: config, save, run loop, clean shutdown. **One shard, no clients, no links** | `Server` | M | §5.1 | ADR: the tree's second composition root |
| 2 | Sessions: the listener, one publisher subscriber per connection | `Server` | M | 1, §5.2 | — |
| 3 | Links: a `ShardLink` per neighbour, derived from the layout; `NoteDurableThrough` after each save | `Server`+`GameLogic` | M | 1 | — |
| 4 | Two shards, two processes, a fleet crossing between them — **`CrossShard.md`'s slice 5, server half** | `Server` | M | 2, 3 | — |
| 5 | The client follows its camera across: reconnect on a crossing, and §5.3's answer | `Outpost` | L | 4, §5.3 | ADR |

Slice 1 is deliberately useless on its own — a process that loads a universe, ticks it and saves it,
with nobody watching. That is the point: it is the smallest thing that proves the engine's headless
claim, and everything after it is an addition rather than a rewrite.

`CrossShard.md`'s slice 5 is **this design's slices 4 and 5**, and that design's row is amended to
point here rather than restating them. The unreachable-shard refusal `CrossShard.md` §6 asks for
lands with slice 4, where a link first has a real connection to lose — the owner's decision of
2026-09-02.
