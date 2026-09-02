# 0067 — The tree has a second composition root, and it is not shared with the first

Status: accepted
Date: 2026-09-02

## Context

`Outpost` has been the tree's only composition root since it was written. AGENTS.md §3 states the
privilege that makes it one: it is the only project entitled to see more than one layer, and
[ADR 0043](0043-a-server-is-told-what-to-be-by-a-file.md) states the duty that comes with it — it is
the only place a file is read to decide what the program is.

`Design/ShardServer.md` wants a shard to run without a window. That is not a new capability:
`NeuronServer`'s `ServerHost` has been a fixed-rate loop over a `Simulation` since it was written,
and nothing in it names a device, a swap chain or a frame. But the claim had never been tested,
because the only thing that had ever driven it was a program that also owned a window. A loop that
*happens* to work without a window and a loop that *is* known to is the difference this slice exists
to close, and closing it needs a second program.

Two programs that both boot a universe raises the question this record answers: is the boot a shared
thing they both call, or does each own its own?

## Decision

**`Server` is a second composition root, with its own boot, and the two roots share code only
through the layers below them.**

- `Server/ShardApp` reads its own config, opens its own save, and orders its own startup. It is
  `OutpostApp`'s shape in the same order, deliberately, so a reader who knows one recognises the
  other — but it is not the same code and there is no base class, no shared `Boot()`, and no
  `#ifdef HEADLESS` anywhere.
- What the two roots genuinely share is moved *down* into a layer they may both name, not sideways
  into something they both derive from. `ServerConfig` moved from `Outpost/` to `GameLogic/` for
  exactly this reason: one deployment's `Server.cfg` is read by both ends, and two parsers for one
  file is two programs that disagree about what their own configuration said.
- The rule that made `Outpost` special is now a rule about a *kind* of project, not about one
  project: a composition root may see multiple layers, and `CheckProjectFiles.py` now knows there
  are two of them. Everything else in the tree still sees one.

**`Server` may read `argv`; the game still may not.** AGENTS.md §5's exemption widens from
"a command-line tool under `Tools/`" to "a program that is its own caller". The argument is the one
ADR 0058 made for `UniverseGen` and it transfers unchanged: a library must not reach around its
caller for configuration, and a console program *is* its caller. The alternative was concretely
worse — putting the shard number in `Server.cfg` means one near-identical file per shard differing in
one integer, and the first time one is edited and another is not, two processes believe they are the
same shard and both write `Universe.2.sav`.

## Alternatives considered

- **One root with a headless mode.** `Outpost.exe --headless`, or a `#define`. Rejected: it makes the
  server's correctness depend on a branch inside a program that links D3D12 and WinRT, so "the
  simulation does not want a window" would be tested by a binary that has one. It also breaks §5 in
  a way the widening above does not — the *game* would be reading argv.
- **A shared `CompositionRoot` base class the two roots derive from.** The obvious de-duplication,
  and the one to be most suspicious of. Rejected: the two boots differ in every step that matters
  (one restores a window and a renderer and has a fallback screen; the other has two failures and no
  fallback), so the base class would be a sequence of virtuals with one implementation each — the
  shape of shared code without the substance. The duplication that remains is about thirty lines and
  is visible; a wrong abstraction over it would not be.
- **`Server` as a library that `Outpost` also links.** Rejected for the layer rule: it would have to
  see `GameLogic` and `NeuronServer` together, which makes it a root wearing a library's name, and
  then nothing stops a third thing linking it.
- **Leave `ServerConfig` in `Outpost/` and have `Server` parse its own.** Rejected — see above. This
  is the one place where sharing is not optional, because the thing being shared is a file format
  between two processes in one deployment.

## Consequences

- **The boot ordering now exists twice, and that is the thing to watch.** This record's own falsifier
  is stated in `ShardApp.h`: if a change to boot ordering ever has to be made in both roots, the
  choice above was wrong and a new record should say so. Until then, two thirty-line boots that read
  alike are cheaper than one abstraction that fits neither.
- **"Composition root" is now a category with two members**, so every rule phrased as "`Outpost`
  may…" has to be read as "a composition root may…". AGENTS.md §3 and §5 are amended to say so, and
  `CheckProjectFiles.py` enforces it.
- **`GameLogic` gained a configuration parser**, which is a slight widening of what that project is
  for. It is bounded and stated: `ParseServerConfig` takes a `string_view` and returns a struct.
  Nothing in `GameLogic` opens a file, and ADR 0043 is unchanged — reading the file is still the
  root's, and only the root's.
- **The engine's headless claim is now tested rather than asserted.** A universe booted, ticked and
  saved by `Server` is byte-identical to the same universe ticked the same number of times in
  `Outpost`, and the run loop allocates nothing per tick. Those are measurements, and they are the
  reason this slice was worth its own program.
- The second root is a place a future slice can put a session, a link and a transport without any of
  it touching the game — which is what `Design/ShardServer.md` slices 2 to 5 do.
