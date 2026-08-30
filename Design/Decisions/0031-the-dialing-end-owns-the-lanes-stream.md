# 0031 — The dialing end owns the reliable lane's stream

Status: accepted
Date: 2026-08-30

## Context

The reliable lane (ADR 0028) is carried on the one bidirectional stream the handshake reserves —
`QUIC_PEER_BIDI_STREAMS = 1`, set in `QuicApi.cpp` before the lane existed, precisely so the lane
would need no configuration change when it arrived.

A bidirectional stream carries both directions, so one stream serves both halves and only one end
may open it. Slice 3a said so in a comment — "the dialing end only" — and enforced nothing. The
accepting end's `Poll` therefore ran, found no stream (its `PEER_STREAM_STARTED` had not arrived
yet), found the connection `Connected`, and opened a second bidirectional stream against a peer that
had negotiated room for one. The handle was also a plain `void*` written by an MsQuic worker in
`AdoptReliableStream` and read by the owning thread in `SendReliable` and `Close`, so the two raced
over it.

It took the test host down, and it took three CI runs to reach because earlier failures masked it.
The lesson worth recording is not the race: it is that a comment stating a rule is not the rule. The
same shape had already appeared twice in this branch — `Publisher::Add` promising a cursor it did not
set, and an order budget counting drops that never happened.

## Decision

`QuicTransport` knows which end it is. `Connect` sets `m_isDialer`; `Reserve` — the pool's call, and
the only way an accepting end is prepared — clears it. `OpenReliableStream` returns immediately
unless this end dialed, so exactly one stream is ever opened and the other end always receives it
through `PEER_STREAM_STARTED`.

The handle becomes `std::atomic<void*>`, because it genuinely crosses threads, and
`AdoptReliableStream` claims it with a compare-exchange: a second `PEER_STREAM_STARTED` is refused
rather than overwriting a handle this side is required to close exactly once.

`StreamStart` takes `QUIC_STREAM_START_FLAG_IMMEDIATE`. Without it MsQuic defers the start until the
first byte is sent, so the accepting end sees no `PEER_STREAM_STARTED` until traffic flows and its
lane never comes up — anything waiting on `ReliableReady()` waits for ever.

## Alternatives considered

- **Two unidirectional streams, one opened by each end.** Symmetric, and it needs no rule about who
  opens what. Rejected: it doubles the stream count and the negotiated limits, and it means each end
  manages a send stream and a receive stream with different lifetimes — more surface for the same
  guarantee a single bidirectional stream already gives.
- **Let either end open it and close the loser.** Rejected: two streams briefly exist, the flow
  control limit is one, and which one survives depends on timing — a race resolved by luck is the
  thing being fixed, not a fix.
- **Have the accepting end open it and the dialer wait.** Works equally well in principle. Not
  taken because the dialer is the end that already knows the connection exists at a definite moment
  — it returned from `Connect` — whereas the accepting end learns of it inside a worker callback,
  and `StreamOpen` there would be an MsQuic call from a worker that ADR 0022 keeps clear.
- **Keep the handle non-atomic and take the accept lock around it.** Rejected: a lock for a single
  pointer that one side writes once, when the load is on the send path of every reliable message.

## Consequences

- Exactly one bidirectional stream per connection, opened by the end that dialed, matching the one
  the handshake reserved.
- `ReliableReady()` becomes true on the dialer at `START_COMPLETE` and on the accepting end when it
  adopts the stream. It stays a separate question from `State()`, because the lane needs one round
  trip more than the connection does.
- A recycled listener slot (ADR 0030) comes back as an accepting end, because `Reserve` clears the
  role. A transport cannot silently keep a dialing role it acquired in a previous life.
- The rule is now enforced by code rather than by a comment. Where this tree writes a rule into a
  comment, the next question is which line makes it true.
