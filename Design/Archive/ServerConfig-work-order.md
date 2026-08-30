# Work order — slice 24: the server configuration file

Cut from [`MmoScalabilityPlan.md`](MmoScalabilityPlan.md) §6 slice 24, against the tree at `6350c7b`.
It is what §4 decision 3 — "how a dedicated server is told what to be", taken 2026-08-30 as *a
configuration file read by the composition root alone* — turns into code.

Layer: `Outpost` only.

---

## 1. Scope

**A text format, a hand-written parser for it in `Outpost`, and the composition root reading one
file at boot.** The file ships as `Outpost/Assets/Server.cfg` and is deployed beside the executable
like every other asset.

The format is `key = value`, one per line; `#` begins a comment; blank lines are ignored:

```
# Outpost server configuration.
port = 30081
backlog = 1
interestRadiusMetres = 2000
interestUpdateEveryTicks = 6
ordersPerTick = 8
```

Five keys, each with a real consumer today (the `shard` key returns with slice 16, its only consumer):

| Key | Type | Consumer |
|---|---|---|
| `port` | u16 | `QuicListener::Start`, and the client end that dials it |
| `backlog` | u32 | `QuicListener::Desc::backlog` |
| `interestRadiusMetres` | float | `InterestSet::Desc::radiusMetres`, through `Publisher::Desc` |
| `interestUpdateEveryTicks` | u32 | `InterestSet::Desc::updateEveryTicks` |
| `ordersPerTick` | u32 | `Publisher::Desc::ordersPerTick` |

Every default is taken from the library's own `Desc{}` rather than spelled again, so a default that
moves in `SimTuning.h` or `Publisher.h` moves here without anybody remembering to follow it. The one
exception is `port`, whose default was a constant in `OutpostApp.cpp` and moves into `ServerConfig.h`
to become one.

**`OUTPOST_QUIC_PORT` stops being a literal in three places** and becomes `m_config.port`. That is
the point of the slice made visible: the root reads a value rather than compiling one in.

## 2. The decisions this order takes

### 2.1 What "fails closed" means for a configuration file

AGENTS.md §5 has two sentences that both apply and appear to pull apart: *"anything parsing content
or configuration reports what was wrong and fails closed; it never throws on malformed input and
never asserts"*, and *"a missing hull logs and is skipped — it does not fail boot"*.

The reconciliation is the split between the parser and the root:

- **The parser is a pure function and decides nothing.** `ParseServerConfig(text, outConfig,
  outError)` returns false with a message naming the line and the reason, and **applies nothing**:
  the caller's `ServerConfig` is untouched on failure. So a half-applied configuration — half the
  admin's file and half the defaults, with nothing saying which — cannot happen. That is the
  "closed" the rule is about.
- **The root decides what to do about it.** `Outpost.exe` is a game with a window and a person in
  front of it: it logs the message and boots on the defaults, because a typo in a tuning file should
  not be a black screen. A headless root would print and exit non-zero, and the parser is the same
  function in both. This is stated here rather than left to be argued at the second root.

**A missing file is not a failure.** The defaults are the values the executable compiles in today, so
a boot with no file is byte-for-byte the boot before this slice.

### 2.2 Every error is an error, including an unknown key

A parser that ignores what it does not recognise turns `prot = 30081` into a silent revert to 30081
— which looks identical to it working. So: an unknown key, a duplicate key, a missing `=`, an empty
key, a value that is not a number, a number outside the key's range, and trailing rubbish after a
number are each a refusal naming the line. There is nothing a configuration file can contain that
this accepts without understanding.

Ranges are per key and are the ones the consumer actually needs: `port` ≤ 65535, `backlog` ≥ 1,
`interestRadiusMetres` > 0 and finite, `interestUpdateEveryTicks` ≥ 1, `ordersPerTick` ≥ 1.

### 2.3 Why the parser is hand-written and in `Outpost`

R7 bans generators, so there is no schema library and no `.ini` dependency; the format is small
enough that a reader for it is fifty lines. It lives in `Outpost` because ADR 0002's test says code
lives with its consumer and the consumer of a server configuration is the composition root — the
same test that put `DdsImage` in `NeuronClient` and `WorldSnapshot` in `GameLogic`. `NeuronCore` may
hold no game semantics and this struct names an interest radius; `NeuronServer` knows
nothing about the game it ticks.

### 2.4 What the file does *not* carry

**The world seed**, which the plan's §6 sentence names. Nothing reads one: `UniverseLayout` exists
(ADR 0037) and is not yet wired into the root, and the two seeds the root does hold —
`BODY_START_SEED` and `SKY_SEED` — are presentation, live in `ViewTuning.h`, and are tunable at any
time by the rule that separates that header from `SimTuning.h`. A knob with no reader is a knob to
explain and then remove. The day the root lays out a system, `worldSeed` is one row in the table
above and one line in the parser.

## 3. Out of scope

- **The headless executable itself**, and whatever runs it. The file is what a second root would
  read; it is not the second root.
- **`argv` and the environment.** Still banned, still by §5, and this slice does not weaken that: a
  file read by the composition root is exactly what §5 already describes.
- **Live reload.** Read once at boot, and the struct is a value from then on.
- **Anything a library reads for itself.** `QuicListener`, `Publisher` and `World` go on taking
  plain `Desc` structs and plain arguments; only the root has ever seen a file and that does not
  change.
- **The presentation tuning in `ViewTuning.h`**, per §2.4.
- **A schema version.** One format, one reader, one executable; a version byte is a promise to
  support the old shape and nothing yet asks for it.

## 4. What to build on

- `NeuronCore/FileSys.h` — `TextFile::ReadFileA`, and `ResolvePath`, which makes a bare name
  relative to `<exe>\Assets\`. That is why the file goes in `Outpost/Assets` and is named without a
  path at the call site.
- `Outpost/OutpostApp.cpp` — `OUTPOST_QUIC_PORT` and its three uses; `OpenLink`.
- `Outpost/WorldSimulation.h` — `Connect`, which builds the one `Publisher::Desc`.
- `GameLogic/InterestSet.h` and `GameLogic/Publisher.h` — the `Desc` defaults the config's own
  defaults are taken from.
- `Outpost/Outpost.vcxproj` — the `<None Include="Assets\…">` + `DeploymentContent` shape every
  mesh and font already uses.

## 5. Acceptance

- [ ] **A malformed file reports what was wrong and fails closed**: the message names the line
      number and the reason, nothing is applied, and neither a throw nor an assert happens. Swept
      over every refusal in §2.2.
- [ ] **Every value the current `Outpost.exe` hard-codes for the link and the subscriber can be
      expressed**, and a file stating exactly the defaults parses to exactly the defaults.
- [ ] **The existing boot is unchanged when no file is present** — the defaults are what the
      executable compiled in, taken from the libraries' own `Desc{}` where one exists.
- [ ] **A decision record** for the format and for why a file does not bend §5.
- [ ] `python Build/CheckFormat.py` and `python Build/CheckProjectFiles.py` green, with the asset
      registered in both the `.vcxproj` and the `.filters`.

## 6. Assumptions the implementer may make

- **`Outpost` has no test suite**, which is ADR 0014's standing assumption and the one this plan
  already made for slices 1, 8, 9 and 10. The parser is therefore verified by a harness that
  compiles the shipped translation unit and sweeps it, and the sweep is reported with the change —
  measured rather than read, which is the tree's standard where a suite cannot reach. Nothing about
  the parser needs Windows, so the harness runs the real code and not a copy of it.
- **No screenshot is owed.** Nothing visual changes; the boot log line already prints the port and
  now prints the one that was read.
- **One subscriber.** The file can express more; the executable still opens one link.
