# Shard server — a universe with no window

**Status: drafted and agreed with the owner on 2026-09-02. All three of §5's decisions were put and
taken the same day — two roots, the in-process server stays, and a client is redirected — and §5
records each with what it costs. Slice 1 is cut ([`ShardServer-slice-1.md`](ShardServer-slice-1.md));
slices 2 to 5 are listed in §7 and are cut one at a time, when each is next.**

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

## 5. Decisions, put to the owner and taken

All three were put on 2026-09-02 and answered the same day, each as the draft recommended. They are
kept as questions with their answers beneath, because the cost of each is what a later reader will
want and it does not survive being rewritten into a statement.

1. **One root or two?** A separate `Server/` executable beside `Outpost`, or one root with the
   window made optional. The draft says two, for ADR 0008's reason and because "optional window" is
   a condition that ends up threaded through boot, input, render and shutdown. The cost of two is a
   dozen lines of ordering duplicated, and a discipline that they must not drift.

   → **Two.** The duplication is accepted and is the thing §6 says to watch: if a change to boot
   ordering ever has to be made twice, this decision was wrong. Slice 1's decision record carries the
   argument.

2. **Does the game executable keep its in-process server?** Today `Outpost` hosts its own universe
   and talks to it over QUIC on loopback (ADR 0021, 0028). It could keep that for single player, or
   single player could become "launch a shard server and connect to it". Keeping it is simpler now
   and means two ways to start a game for ever; dropping it makes every session identical and makes
   the first run harder.

   → **It keeps it.** So there are two ways to start a game, deliberately: `Outpost` alone is single
   player and needs nothing installed, and `Server` plus `Outpost` is everything else. The standing
   cost is that a change to session setup has two callers, and the standing benefit is that a first
   checkout still runs the game by running the game.

3. **Which shard does a client connect to first, and who tells it?** §7 says the shard its camera is
   in — but a client that has not connected has no camera position it trusts. Options: a fixed
   home shard in the client's config; the save's shard; or a redirect from any shard to the right
   one. The draft leans to the last, because it is the only one that stays true when a player's
   fleet has moved and it is what a reconnect on a crossing needs anyway.

   → **A redirect.** A client connects to whichever shard it knows and is told where to go; the same
   message is what a crossing sends. It costs one message kind and a reconnect path that would have
   been needed for crossings regardless, and it is the only answer that is still correct after a
   player's fleet has moved — which a config file and a save's shard both stop being.

4. **How many sessions does a shard serve, when there is no login?** There is no account and no
   authentication, so every session would be `OWNER_LOCAL` and `FACTION_PLAYER` — the constants the
   game's one client uses. Two clients would therefore give orders to one fleet and see one set of
   ships. Options: serve them all and let them collide; mint an owner per connection so each has its
   own fleets; or serve one and refuse the rest.

   → **One, and the rest are refused with a stated reason.** Taken 2026-09-02, with slice 2. Minting
   an owner is the tempting one and is worse than it looks: the server would be inventing an identity
   backed by nothing, and a client that reconnects gets a different one and loses its fleets — so the
   feature would be "multiplayer" that silently deletes a player's things. The refusal costs one
   sentence, is honest about what the tree has, and is lifted by a login rather than replaced by one.
   `backlog` stays what it always was: how many connections the listener can *hold*.

5. **Where does a session's interest centre come from?** `Publisher::SetCentre` takes a point per
   subscriber and the game's root reads it off the camera each frame. A server has no camera.
   Options: derive it from the subscriber's own ships; add a wire message so the client says where it
   is looking; or leave it at the universe origin.

   → **The centroid of the session's own fleets.** Taken 2026-09-02, with slice 2. It is the number
   the status block already derives for every update (`UniverseSnapshot`'s fleet block), so it costs
   no new machinery, no new message kind and no new determinism surface — and `Universe` now derives
   it once for both callers rather than twice. Measured rather than assumed: with the player's fleet
   ordered 15 km out, a 2 km interest set at the derived centre holds all three of its ships and one
   pinned at the origin holds none. The known failure is a camera over empty space away from the
   fleet, which is documented in `Outpost/UniverseSimulation.h` and is **slice 5's**, where the
   session gains a camera along with the redirect.

6. **Does `Server` get a test project, when no composition root in this tree has one?** Slice 2's
   session logic — open, refuse, close, reopen — is ordinary testable code, and it was verified by a
   scratch harness CI never runs. The tree's convention is one suite per *library*: neither `Outpost`
   nor `Tools/UniverseGen` has one.

   → **Yes, a twelfth project.** Taken 2026-09-02, with slice 3. The convention's *reason* does not
   transfer: `Outpost` has no suite because its logic is D3D12- and WinRT-bound and untestable off a
   device, and `Server`'s is not. What makes it cheap is that it shares no source file with `Server` —
   the testable half of that project is header-only by construction and `ShardApp.cpp` is the root
   that is run rather than tested, so the suite reaches it by include path and
   `CheckProjectFiles.py`'s unique-name rule is not bent. **Keeping that split is now a rule**: a
   testable thing that lands in a `.cpp` under `Server/` has landed in the wrong file.

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
| 1 | [The executable: config, save, run loop, clean shutdown. **One shard, no clients, no links**](ShardServer-slice-1.md) | `Server` | M | — | [0067](Decisions/0067-the-tree-has-a-second-composition-root.md) — **landed 2026-09-02** |
| 2 | [Sessions: the listener, one publisher subscriber per connection](ShardServer-slice-2.md) | `Server` | M | 1 | §5.4, §5.5 |
| 3 | [Links: a `ShardLink` per neighbour, derived from the save's own gates; `NoteDurableThrough` after each save, and a twelfth project to test it in](ShardServer-slice-3.md) | `Server`+`GameLogic`+`Tests` | M | 1, 2 | §5.6 |
| 4 | Two shards, two processes, a fleet crossing between them — **`CrossShard.md`'s slice 5, server half** | `Server` | M | 2, 3 | — |
| 5 | The client follows its camera across: the redirect §5.3 chose, and a reconnect on a crossing | `Server`+`Outpost` | L | 4 | ADR: a client is redirected, not configured |

Slice 1 is deliberately useless on its own — a process that loads a universe, ticks it and saves it,
with nobody watching. That is the point: it is the smallest thing that proves the engine's headless
claim, and everything after it is an addition rather than a rewrite.

**Slice 1 landed, and the claim held.** A universe booted by `Server`, ticked 8276 times and saved is
byte-for-byte the universe ticked 8276 times through `Universe::Step` directly — the run loop changes
no outcome — and the loop allocates nothing across 6100 steady-state ticks after boot. `ServerHost`
needed no change to run without a window, which is the answer this slice was cut to get. Two things
did change on contact and are recorded in [the work order](ShardServer-slice-1.md) §8: `ServerConfig`
moved from `Outpost/` to `GameLogic/` so one deployment's file has one parser, and AGENTS.md §5's
`argv` exemption widened from "a tool under `Tools/`" to "a program that is its own caller".

`CrossShard.md`'s slice 5 is **this design's slices 4 and 5**, and that design's row is amended to
point here rather than restating them. The unreachable-shard refusal `CrossShard.md` §6 asks for
lands with slice 4, where a link first has a real connection to lose — the owner's decision of
2026-09-02.
