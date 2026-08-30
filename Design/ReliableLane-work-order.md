# Work order — the reliable lane

Slice 3a of [`QuicTransport.md`](QuicTransport.md) §14, scheduled on 2026-08-30 when the owner
lifted §12 decision 4. Also slice 5 of [`MmoScalabilityPlan.md`](MmoScalabilityPlan.md), which is
where the cost of waiting was priced: finding E1 in the review — a lost leave or destroyed list is a
client that is permanently wrong, and a 13-fragment update completes 77% of the time at 2% loss.

One slice, `NeuronCore`. It adds a lane and proves it. It does not put anything on it: that is 3b.

## Scope

1. **`Transport.h`** — the contract gains a second lane, as non-pure virtuals:

   ```cpp
   [[nodiscard]] virtual bool SendReliable(const std::uint8_t* _bytes, std::uint32_t _count);
   [[nodiscard]] virtual std::uint32_t ReceiveReliable(std::uint8_t* _outBytes, std::uint32_t _capacity);
   ```

   Defaults refuse — `false` and `0` — so every existing implementation of `Transport` still
   compiles and still behaves, `CaptureTransport` in `GameLogicTests` included. A refusing default
   is the honest answer for a transport that has no lane, and it is the same answer `Send` already
   gives for a full queue, so callers have a shape they know.

   `MAX_RELIABLE_BYTES` beside `MAX_DATAGRAM_BYTES`, for one framed message. It is not the same
   number and must not be spelled as if it were: a datagram is bounded by the path's MTU, a
   reliable message by what the receiver is willing to buffer for one frame.

2. **`LoopbackTransport`** — the lane never drops and never reorders. `dropOneInN` does not apply
   to it, and neither does anything else that models a lossy path; a queue that is full still
   refuses, because that is backpressure and not loss.

3. **`QuicTransport`** — the lane is the one bidirectional stream `PeerBidiStreamCount = 1` already
   reserves at handshake:
   - opened when the connection reaches `Connected`, on the owning thread, never in a callback;
   - each message framed with a 2-byte little-endian length, because a stream is bytes and the
     lane's contract is messages;
   - a third ring for the stream's receive side, filled from `STREAM_EVENT_RECEIVE` on the worker
     and drained in `Poll` on the owning thread — ADR 0022's rule holds without exception, and the
     reassembly of a partial frame belongs to the owning thread, not to MsQuic's.

4. **Tests**, in `NeuronCoreTests`, over both implementations:
   - order is preserved across many messages;
   - nothing is lost under `dropOneInN = 1` — every datagram dropped, every reliable message
     arriving;
   - a message larger than `MAX_RELIABLE_BYTES` is refused rather than truncated, which is
     `Send`'s rule and the one `QuicTransportTests` already asserts for datagrams;
   - a partial frame arriving in two `STREAM_EVENT_RECEIVE`s reassembles into one message.

## Out of scope

- **Anything choosing the lane.** No `WorldSnapshot` change, no adapter change, no ALPN bump: the
  format is ADR 0008's territory and moves in 3b. `ShipsPerSnapshotFragment()` is untouched, and
  that is an acceptance item rather than a note.
- **Unidirectional streams, more than one stream, or stream-per-message.** One lane, reserved at
  handshake. A second is a new decision.
- **Flow control tuning, or a policy for a peer that never reads.** The ring fills and the lane
  refuses; what a session does about a subscriber that has stopped reading is the session's, and
  ADR 0027's consequences already name it.

## What to build on

`QuicTransport`'s existing ring pair and its `m_lock`-per-connection discipline — the third ring is
the same arena shape, taken once at `Reserve`, and it must not introduce a second lock ordering.
`LoopbackTransport::Connect`'s pairing, and its `dropOneInN` and `latencyTicks` knobs, which the
lane ignores. `QuicApi.cpp`'s settings block, where `PeerBidiStreamCount` is already 1 — read the
comment there before changing anything, because that number was reserved for this.

`Design/QuicTransport.md` §4.2 and ADR 0022 are the threading contract. Note that the ADR
under-reports the worker-side API surface today (`ConnectionSetConfiguration` in `Adopt` and
`ConnectionShutdown` in `ReconsiderConnected` are both worker-side); if this slice adds a worker-side
call, the ADR gets a sentence in the same commit rather than a third undocumented one.

## Acceptance

- The four test groups above, green on both implementations.
- Every existing suite green and unchanged — in particular `QuicTransportTests` and
  `LoopbackTransportTests`, whose datagram rows must not have moved, and `GameLogicTests`, which
  must compile without `CaptureTransport` gaining a line.
- `ShipsPerSnapshotFragment()` returns what it returned before this slice.
- No allocation on the send or receive path once a connection is up: the third ring is taken at
  `Reserve` with the other two. A code read decides this, and the pull request says so.
- Debug|x64 builds; `python Build/CheckProjectFiles.py` and `python Build/CheckFormat.py` pass.
- A decision record if the lane's shape turns out to need one — a second stream, a different framing,
  or a worker-side call ADR 0022 does not cover. If none of those happen, no record is due: this
  slice is the design's §8 as written.
