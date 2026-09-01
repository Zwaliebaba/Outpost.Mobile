# 0057 — The save is a versioned file, and a refused one stops the boot

Status: accepted
Date: 2026-09-01

## Context

`Design/Universe.md` §8 gives the state codec a file. Two questions come with it that a commit
message cannot settle, because both are about what happens when something is wrong.

The first is what a save file *is*. `WriteUniverseState` already produces a complete universe as
bytes, magic and format byte included, and a file could be exactly those bytes and nothing else.

The second is what a boot does with a file it cannot read. There are only two answers — start a new
universe, or stop — and they are not close to each other.

## Decision

**A save file is a header this codec owns in front of the state bytes**: magic, a format byte, the
galaxy seed, the shard, and the length of the state. `Game::WriteSaveFile` and `Game::ReadSaveFile`
in `GameLogic`, beside the state codec, taking and returning bytes; the file itself is opened by the
composition root, because nothing in `GameLogic` or `NeuronCore` opens a file.

**A file that is present and cannot be read stops the boot, naming what refused.** It never falls
through to genesis. `Neuron::FileSys::Exists` is what makes that expressible: a reader answers "no
file" and "I could not read the file" with the same empty buffer, and here those must lead to
opposite places.

The write is atomic — `Neuron::BinaryFile::WriteFileAtomic`, a flushed sibling temporary renamed over
the target — and the cadence is `saveEveryTicks` in `Server.cfg`, because how much progress a
deployment will lose to a power cut is a property of the deployment (ADR 0043).

## Alternatives considered

**For the format:**

- **The state bytes alone, no header.** The simplest thing, and it works until the first time
  anything else needs to be known. It cannot carry the galaxy seed, and without the seed a binary
  whose compiled `GALAXY_SEED` has moved on loads a saved fleet into a galaxy that has quietly
  rearranged itself around it — stations inside stars, gates leading nowhere. Rejected on that
  alone; the seed is the reason the header exists.
- **One format byte for both.** Bump the state's byte and let it cover the file too. Rejected
  because they answer different questions — the file's says how to *find* the state, the state's
  says how to *read* it — and a wire change that bumps the state's should not invalidate every save
  on disk.
- **A checksum instead of a length.** Stronger against corruption, and it was not chosen because the
  failure being defended against is a torn write, not a flipped bit: the length catches truncation
  and it catches a file with something appended, which is the one damage `ReadUniverseState` cannot
  see. A checksum is a later addition to a format that already has a version byte to add it under.

**For a refused file:**

- **Fall through to genesis.** The forgiving option, and the one this record exists to reject. A
  refused save silently replaced by a fresh universe is a player's game deleted by a bug in reading
  it — and the deletion completes at the next periodic save, which lands on top of the file nobody
  could read. The failure is total, silent, and unrecoverable, and the cost of the alternative is a
  message box.
- **Rename the bad file aside and start fresh.** Keeps the bytes, so it is not the previous option.
  Rejected because it decides for the player, at boot, that this is a new game — and it is a
  mechanism that runs exactly when something is already wrong, which is the worst moment to have one
  that has never been tested. Moving a file aside is something a person can do, and the refusal
  message says so.
- **Repair what parses.** Load the ships and drop the fleets. Rejected on `ParseServerConfig`'s rule,
  for `ParseServerConfig`'s reason: half a universe with nothing saying which half is a state that
  should not be able to exist.

## Consequences

- The boot has two failure modes now and one sentence for both. `ThrowLinkFailure` generalised to
  `ThrowBootFailure`: the wire and the saved universe refuse for the same reason, which is that
  there is no acceptable other thing to run instead.
- **The save at clean shutdown is at the end of `Run()`, not in `Shutdown()`.** `wWinMain` calls
  `Shutdown` from its catch blocks too, so a save there would fire after a failed boot — over a good
  file, or over the file that had just been refused. "The loop returned rather than threw" is what
  clean means, and it is structural rather than a flag.
- The shard is in the file twice, header and state, and `ReadSaveFile` refuses a file where the two
  disagree. That is the price of a header that can be read without the body, and it is a row in the
  suite rather than a comment.
- `m_simulation.Connect` moved behind the universe. The subscriber's despawn cursor opens at
  `DespawnHead()`, which on a restored boot is the saved one; connected first, it would have opened
  at zero and walked the whole despawn log on the first tick. The ordering was already wrong and
  already documented as right — it cost nothing only because every boot began at head zero.
- The file lands under `Assets\`, which is where `ResolvePath` puts a relative name and is wrong for
  a real install. One constant, `UNIVERSE_SAVE_FILE`, so the day there is a writable data directory
  that line moves and nothing else does.
- Genesis is still the game's, on a first boot. It should not stay that way — a headless shard server
  has no business running it either — and the slice that moves it into a tool is
  `Design/Universe.md` §13's 5b. This record does not decide that; it is what makes it cheap.
