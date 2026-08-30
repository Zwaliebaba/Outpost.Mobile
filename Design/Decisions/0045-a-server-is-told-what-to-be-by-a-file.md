# 0045 — A server is told what to be by a file the composition root reads

Status: accepted
Date: 2026-08-30

## Context

AGENTS.md §5: *"No argv, no environment variables. Configuration is loaded by the composition root
only; libraries receive plain config structs and never read files or the registry themselves."*

That rule was written for a game that is its own server. `MmoScalabilityPlan.md` §4 decision 3 asked
how a *dedicated* server would be told what to be — its port, how many connections it carries, which
shard it mints identities for, what one subscriber is sent — and the owner took it on 2026-08-30: **a
configuration file, read by the composition root alone.**

That does not bend §5, and saying so is half the reason this record exists. The rule bans `argv` and
the environment, and says configuration is *loaded by the composition root* — which is exactly what
a file read there is. What §5 forbids is a library opening a file, and no library does.

The other half is what the rule's own second sentence demands of it: *"anything parsing content or
configuration reports what was wrong and fails closed; it never throws on malformed input and never
asserts."* A format needs a reader, and R7 bans generators, so the reader is written by hand.

## Decision

**`Outpost/Assets/Server.cfg`, `key = value`, one per line, `#` for comments, read once at boot.**
The parser is `Outpost/ServerConfig.{h,cpp}` — a pure function over text — and the root does the file
read and decides what a refusal means.

Six keys, each with a consumer that exists today: `port`, `backlog`, `shard`,
`interestRadiusMetres`, `interestUpdateEveryTicks`, `ordersPerTick`. Every default is taken from the
consuming library's own `Desc{}` rather than restated, so a default that moves moves here with it.
The one exception is `port`, which had no library to belong to: `OUTPOST_QUIC_PORT` was a constant
in `OutpostApp.cpp` and is now `ServerConfig::port`'s default.

**"Fails closed" is split between the parser and the root**, and this is the part worth writing down:

- `ParseServerConfig(text, outConfig, outError)` returns false with a message naming the line and the
  reason, and **applies nothing** — the caller's struct is untouched. So half the admin's file and
  half the defaults, with nothing saying which, is a state that cannot exist. It never throws and
  never asserts.
- The root decides. `Outpost.exe` has a window and a person in front of it, so it logs
  `CONFIG REFUSED` and boots on the defaults; a headless root would print the same message and exit
  non-zero. Same parser, two roots, and neither decision is baked into the parser.

**Every error is an error, including an unknown key.** A reader that ignores what it does not
recognise turns `prot = 40000` into a silent revert to 30081, which looks exactly like the file
working. An unknown key, a key set twice, a missing `=`, an empty key or value, a value that is not a
number, one outside the key's range, and trailing rubbish after a number are each a refusal.

**A missing file is not a failure.** The defaults are what the executable compiled in before the file
existed, so a boot without one is byte-for-byte the boot before this slice.

## Alternatives considered

- **Leave it compiled in.** What the tree did. Rejected because it is what gates a second process:
  two servers on one machine need two ports, and two shards minting identities need two shard ids
  (ADR 0044) or they issue colliding ones. Neither is a rebuild.
- **`argv`, or environment variables.** The usual answer for a server, and the reason §5 bans both:
  configuration that arrives through the process's launch is configuration that is invisible in the
  tree, different on every machine, and impossible to review. A file is a thing you can read, diff
  and ship.
- **JSON, TOML, or YAML.** Every one of them is a dependency (AGENTS.md §5's approval rule) or a
  generator (R7), for a format that has six scalar keys. The hand-written reader is fifty lines and
  its failure messages name the line, which no schema library gives for free.
- **Put the parser in a library.** `NeuronCore` may hold no game semantics and this struct names an
  interest radius and a shard; `NeuronServer` knows nothing about the game it ticks. ADR 0002's
  test — code lives with its consumer — puts it in the root, which is the only consumer a
  configuration file has ever had. The cost is that it lands where there is no test suite; see the
  consequences.
- **Repair a malformed file rather than refuse it** — take the lines that parse and default the
  rest. Rejected as the failure mode the rule is about: an operator who mistyped one line would get
  a server running on a configuration nobody wrote and no message tying the behaviour to the typo.
- **A schema version byte.** One format, one reader, one executable. A version is a promise to keep
  supporting the old shape, and nothing has asked for it. The day the format changes
  incompatibly, the key that says so is one row.
- **Carry the world seed**, which the plan's slice sentence named. Nothing reads one:
  `UniverseLayout` exists (ADR 0037) and is not yet wired into the root, and the two seeds the root
  does hold — `BODY_START_SEED` and `SKY_SEED` — are presentation, live in `ViewTuning.h`, and are
  tunable at any time by the rule that separates that header from `SimTuning.h`. A knob with no
  reader is a knob to explain and then remove; when the root lays out a system it is one row here
  and one branch in the parser.

## Consequences

- **`OUTPOST_QUIC_PORT` is gone** as a literal in three places. The port is read, and the boot log
  line and the "port was refused" failure both name the one that was read.
- **`World::ConfigureShard` takes the configured shard**, which is the value ADR 0044 said a second
  machine must not leave alone. The shipped file says so beside the key.
- **`WorldSimulation::Connect` takes the config**, so the one subscriber's interest radius, update
  period and order budget are properties of the deployment rather than of the build. They are per
  subscriber, which is the shape `Publisher::Desc` already had.
- **The file ships stating exactly the defaults.** That makes it safe to ship and safe to delete —
  the game with the file and the game without it are the same game — and it means the format is
  documented by an example that is checked every run.
- **`Outpost` still has no test suite**, which is ADR 0014's standing assumption and the one this
  plan already made for slices 1, 8, 9 and 10. The parser is therefore verified by a harness that
  compiles the shipped translation unit — nothing in it needs Windows — and sweeps it: every
  refusal above, the shipped file parsing to exactly the defaults, and 200,000 fuzzed inputs under
  ASan and UBSan without a throw, an abort or a silent refusal. Measured rather than read, which is
  what this tree does where a suite cannot reach. The day `Outpost` gains a suite, those cases move
  into it unchanged.
- **Libraries are untouched.** `QuicListener`, `Publisher`, `InterestSet` and `World` go on taking
  plain `Desc` structs and plain arguments, and none of them has ever seen a file. That is the
  sentence §5 was protecting, and it is still true.
- **No live reload.** Read once, a value from then on. A server that re-read a file mid-tick would
  be changing the replay contract's inputs from another thread's timeline.
