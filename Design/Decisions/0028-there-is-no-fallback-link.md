# 0028 — There is no fallback link

Status: accepted
Date: 2026-08-30

Supersedes in part: [0021](0021-the-network-transport-is-msquic.md), whose consequences made the
loopback the fallback, and `Design/QuicTransport.md` §12 decision 2, which chose that arrangement.

## Context

`Outpost.exe` opened a QUIC connection across `127.0.0.1` at boot and, when it could not — MsQuic
missing, the port taken, the handshake timing out — connected a `LoopbackTransport` pair instead and
logged an amber `LINK | LOOPBACK`. The game then ran, identically, on a wire that was not the one
being shipped.

The argument for it was that a taken port is a diagnostic and not a failed boot. That was reasonable
while QUIC was new in the tree and nobody knew how often it would refuse. It has now booted over
QUIC as a matter of course, and the fallback has become the thing the original decision was written
to prevent: `Design/QuicTransport.md` §12 decision 2 chose QUIC-by-default over loopback-by-default
because "a path nobody runs is a path nobody notices breaking", and the fallback is a path nobody
runs.

It also costs more than it looks. Two `LoopbackTransport` members sat in `OutpostApp` whether or not
they carried anything, at 288 KB of rings each; the frame loop advanced their tick clocks every
frame and every tick for a latency model that only applied to them; and every reader of `Init` had
to hold two wires in mind to follow one. The failure it was protecting against, meanwhile, was
reported as one 64-character amber row in an event log, which is the least likely place for a player
to notice that the thing they are testing is not the thing that will ship.

## Decision

`Outpost.exe` opens QUIC or it does not start. `OpenQuicLink()` returns having connected both ends
or throws `winrt::hresult_error(E_FAIL, …)` naming the stage that refused and the reason string that
stage kept — the library, the port, the dial, or the handshake with both ends' states.

`LoopbackTransport` stays in `NeuronCore`, unchanged, with its test suite. It is not the fallback
any more; it is the instrument. It remains the only implementation of `Transport` that can drop a
datagram or delay one by a counted number of ticks on purpose, which is what the reliable lane's
tests need and what a real connection cannot be asked to do.

## Alternatives considered

- **Keep the fallback and make it louder** — a modal dialog at boot rather than a log row. Rejected:
  it treats the symptom. The game would still be running on the untested wire afterward, and a
  dialog that a developer learns to click through is a fallback with extra steps.
- **Keep the fallback behind a constant**, off by default, on for whoever wants it. Rejected: that
  is the loopback-by-default arrangement §12 decision 2 already turned down, with the default
  inverted. A constant nobody flips is a path nobody runs.
- **Delete `LoopbackTransport` as well**, leaving one implementation of `Transport`. Considered and
  turned down deliberately: the class is not dead code, it is the loss-and-latency instrument.
  Slice 3a's acceptance is "nothing lost under `dropOneInN`", which needs a transport that can be
  told to drop; QUIC on localhost drops nothing on demand. Deleting it would trade a tested
  instrument for a smaller file count, and would leave the reliable lane with no way to prove it is
  reliable. It also keeps `Transport` honest as an abstraction: an interface with one implementation
  is a class wearing a costume.
- **Fall back only in Debug.** Rejected: it makes the shipped configuration the one with the least
  testing, which is the wrong way round, and it means a defect in the QUIC path is invisible on
  exactly the builds developers run.

## Consequences

- A taken port now stops the game with a message box naming the port and the reason, rather than
  starting it on a different wire. That is a louder failure, and it is the intended one: the reason
  strings `QuicApi`, `QuicListener` and `QuicTransport` each keep exist to be read by a person, and
  until now the only place they were shown was a log row inside a game that had already started.
- `OutpostApp` loses two members, two `AdvanceTo` calls per frame plus two per tick, and one branch
  in `Init` and `Shutdown` each. `m_linkIsQuic` becomes `m_linkOpen`, which is what `Shutdown`
  actually needs to know now that there is only one kind of link.
- `wWinMain` is unchanged. It already caught `hresult_error` and showed `message()`, which is why
  that exception type was chosen over `Neuron::Fatal` — `Fatal` discards its formatted arguments and
  reports "Fatal Error", which for a taken port is a support ticket rather than a diagnostic.
- The day a dedicated server exists it inherits this: no silent degradation, and a boot that cannot
  open its wire says so and exits, which is what a process under a supervisor should do.
- Nothing below the composition root changes. The simulation, the format, the interest set and the
  renderer still cannot tell which `Transport` they were handed, and `NeuronCoreTests` still runs
  the same round-trip over both implementations.
