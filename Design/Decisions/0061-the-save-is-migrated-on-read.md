# 0061 — The save is migrated on read, and a format is retired only by record

Status: accepted
Date: 2026-09-02

## Context

ADR 0057 gave the universe a versioned file and ruled that a file in a format the build does not
know stops the boot. That was right, and it was complete while there was nothing to migrate from:
the format byte moved six times in the three days after the file arrived, and each move was a wipe
nobody had to care about because no deployment held a file.

`Design/GameDesignPlan.md` schedules a dozen more moves — a wallet, items, sites, a journal, a build
queue, a ship record, effects, a stance on the fleet row — and from the first one that lands on a
universe anyone has played in, "refused" means "deleted at the next boot". The plan's phase 0
exists to make that impossible before any of those tables is designed.

## Decision

**A file in an older format is migrated on read, inside `ReadUniverseState`, by gating every field a
later format added on the format byte the file carries.** The writer writes one format,
`UNIVERSE_STATE_FORMAT`; the reader accepts any from `UNIVERSE_STATE_FORMAT_OLDEST` to it. A field
added at format N is read as `format >= N ? read : the value it had before it existed`, at the point
in the stream where it lives, and nowhere else. A universe read from an older format is a current
universe the moment the read returns, and the next save writes it forward. The file header's byte
gets the same window.

**The reader is proven against a file, not against itself.** The commit that bumps the format to N+1
commits `Tests/GameLogicTests/Assets/UniverseFormatN.sav`, written by `UniverseGen` at its parent
commit — the shipped galaxy at tick zero, byte-identical on any machine (ADR 0058) — with its census
recorded beside it, and the row that loads, counts, re-saves and replays every fixture is green in
the same commit.

**A format is retired only by a decision record**, which moves `OLDEST`, names the fixtures it
deletes, and says why the gates they exercised may go.

**A boot that migrated a file keeps the file it read**, beside the save under its format's name,
once and never overwritten. A copy of an *accepted* file decides nothing for the player; it is there
because a gate that misreads an older field produces a universe that loads, saves thirty seconds
later, and has by then destroyed the only file that could show what the field was.

## Alternatives considered

- **Keep refusing.** ADR 0057 as it stands. Rejected because it makes every table in the plan a
  wipe, and the plan cannot afford one per slice.
- **A forward-rewrite step in `UniverseGen`** that reads format N and writes N+1. Rejected because
  it needs exactly the same old reader this record adds, and then keeps it in a tool nobody runs on
  a live shard; the game is what has the file, so the game migrates.
- **A reader per format**, as separate functions. Rejected because a format that moved one field
  would duplicate the whole stream, and two readers that must agree on every other field is the
  drift ADR 0058 exists to prevent, one layer down.
- **A transform table inside the file**, describing how to read itself. Rejected as a format for
  the format: it has to be versioned too, and it moves the gates from code a test can read into data
  a test can only trust.
- **Keep every format forever.** The gates accumulate in the one codec the tick trusts, each one a
  branch on every read of a field that no live file has carried for a year. Rejected in favour of
  retirement by record: possible, deliberate, and visible in the index.
- **Rename a refused file aside.** ADR 0057 rejected it, and this record does not reopen it. The
  sidecar here is written for a file the reader *accepted*, which is the opposite case.

## Consequences

- **Every bump now costs a fixture and a gate**, and the rulebook's checklist says so. A bump that
  arrives without its fixture fails `TheNewestFixtureIsTheToolsOutput` on the next commit that does
  not move the format, which is every other one.
- **The reader carries a `format` local it does not yet consult.** There is no older format to
  gate for at the commit this record lands in; the first gate lands with the first bump after it,
  and the slice that adds it changes one line per field.
- **Formats 2 to 6 are not migrated from**, and never will be: no file was ever on disk in any of
  them. `OLDEST` starts at 7, where a file first existed to keep.
- **The header reports what it was read at** — `SaveHeader::fileFormat` and `stateFormat`, filled by
  the reader and ignored by the writer — so a boot can say `SAVE | FORMAT 7` and a test can prove a
  fixture is in the format its name claims.
- **A refused boot names the bytes.** The reader changes nothing when it refuses, so the composition
  root peeks the two format bytes for the sentence rather than reporting "not a universe this build
  can read" and leaving the player to guess which build.
- **The sidecar lands under `Assets\`** beside the save, wrong for a real install for the reason
  `UNIVERSE_SAVE_FILE`'s comment already gives, and moves with it.
- **The fixture is a genesis file.** Every table `Step` reads is present at tick zero and several are
  empty — no route in flight, no docking, no protector duty. The gated default of a mid-flight field
  is exercised by the replay the row runs on the loaded fixture, not by the file; a richer fixture
  would need a program that steps and saves, which the tool is not.
