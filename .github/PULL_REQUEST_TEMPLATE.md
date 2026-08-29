<!-- One slice per pull request. Delete lines that do not apply; do not leave them unanswered. -->

## What this is

<!-- One or two sentences. Which work order or design this implements, or what it fixes. -->

Work order / design:
Layer(s) touched: <!-- NeuronCore / NeuronClient / NeuronServer / GameLogic / Outpost / Tests / Build -->

## What was done

-

## Out of scope, and left as it was

-

## Assumptions and placeholders

<!-- Anything the work order allowed you to stub, and anything you decided that it did not settle. -->

-

## Evidence

- Built: <!-- Debug|x64 at minimum; say which configurations -->
- Tests run: <!-- which suites, and the result, e.g. NeuronClientTests 27/27 -->
- Build checks: <!-- CheckProjectFiles.py, CheckFormat.py (clang-format 18.1.3) -->
- Screenshots: <!-- required for anything visual, at two window sizes -->

## Decisions

<!-- A type moved between libraries, a dependency rule or guard changed, a project or dependency added or removed, an approach turned down that someone will propose again? Name the decision record, or state that none is due. -->

Decision record:

## Checklist

- [ ] Naming conforms: `_` on parameters, `m_` on class state, `UPPER_CASE` on constants, no `I`/`C`/`Base`/`Impl` prefixes or suffixes, units in names
- [ ] Files are PascalCase, flat, unique repo-wide, and registered in both the `.vcxproj` and the `.filters`
- [ ] `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass
- [ ] Dependency rules hold: no engine project names the game, client and server do not name each other, nothing names the executable, no graphics header reaches NeuronCore or NeuronServer
- [ ] No `argv`, no environment reads, no stored `XMVECTOR`, no `RH` call, nothing under `CompiledShaders/` committed
- [ ] GameLogic touched? The replay-equality test still passes, and nothing added reads a clock, draws unseeded randomness, or keys on a pointer
- [ ] Any sentence in the rulebook or a design that this change made false has been changed in this pull request
- [ ] A decision record is included where one is due, and the index lists it
