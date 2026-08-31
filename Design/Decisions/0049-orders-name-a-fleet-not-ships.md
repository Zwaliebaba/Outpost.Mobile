# 0049 — Orders name a fleet, not ships

Status: accepted
Date: 2026-08-31

## Context

[`Design/Fleets.md`](../Fleets.md) §6 makes the fleet the unit of command. The tree already has two
order messages and they are both ship lists: `MoveOrder` and `DockOrder` each carry up to
`MaxShipsPerOrder()` entity ids, and `World` gates them by filtering the list down to the issuer's
own ships (ADR 0014).

A fleet order could be a third message of that shape — the client expanding a fleet into its members
and sending them — and that is the smaller change. It is also the wrong one, in a way that is easier
to argue now than to unpick later, because the shape of this message is what the client's selection
logic, the order budget and the whole of slice 4's defense are built against.

## Decision

One message that names a **slot and a kind**, and carries no ship list at all:

```
FleetOrder { u8 slot; u8 kind; WorldPos point; f32 facing; u8 hasFacing; EntityId station }
```

`World::IssueFleetOrder(FactionId, slot, FleetCommand)` is the entry point. The authority gate is one
comparison — does the issuer's faction own a live fleet in that slot — and the order is then
*lowered* onto `IssueMoveOrder` and `IssueDockOrder`, which are untouched. The standing order is kept
on the fleet's row, so it outlives the tick it was given on.

## Alternatives considered

- **A third ship-list message: the client expands the fleet and sends its members.** The smallest
  change, and wrong three ways. The client would be *stating* membership the server already owns,
  which is the client asserting server state — the thing every other part of this wire is arranged to
  prevent (ADR 0013, ADR 0039). The message would grow with the fleet, so ordering eight ships would
  cost eight times what ordering one costs, on a wire whose order budget is counted in messages. And
  the gate would stay a filter, so an order could be *partly* accepted: a fleet of eight moving as
  six is a fleet nobody ordered, and nothing would say which six.
- **A fleet order that also carries the member list, as a confirmation.** Two sources of truth for
  membership, disagreeing on exactly the ticks that matter — a launch, a loss, a docking.
- **Keeping both forms permanently.** Two ways to say one thing on a wire that gates in one place is
  a path nobody tests, which is ADR 0028's argument at message grain. They do coexist for now, and
  only because the client still sends the old ones: they retire with the slice that stops sending
  them, and nothing new is built on them.
- **Lowering in the adapter — `Publisher` expanding a fleet and calling `IssueMoveOrder`.** ADR 0014
  again: the adapter has no test suite, and every future host would have to remember to do it. The
  simulation refusing is a property; an adapter refusing is a convention.

## Consequences

- **An order stops scaling with the group it moves.** One fixed-size message whatever the fleet's
  size, so a subscriber's `ordersPerTick` budget now bounds *fleets ordered per tick* rather than
  ships — which is the number a player actually generates.
- **There is no partial acceptance.** A fleet order is refused whole or lowered whole. The five
  results (`Ordered`, `NoSuchFleet`, `NotAStation`, `RefusedStanding`, `Unsupported`) are the whole
  vocabulary, and every refusal changes nothing.
- **Membership never travels upward.** The client learns which ships are in which fleet from the
  roster the server states (design §8.1) and never asserts it.
- **The standing order is state, so behavior can be built on it.** A hull launched after the order
  joins it rather than the rally; slice 4's defense suspends and resumes it; and a fleet holds it
  through traffic. None of that is expressible about a message that has already been consumed.
- **Two kinds are reserved and refused.** `Attack` waits for the pursuit chassis and a `combatant`
  flag; `Mine` waits for a mining design and for something to mine. The bytes are spent either way,
  which is the point of reserving them — a later design adds behavior, never a renumbering.
- **`World::IssueMoveOrder` and `IssueDockOrder` keep their ship-list shape**, because the NPC passes
  and the launch use them. What retires is the *wire* form, not the call.
- **`Stop` becomes expressible.** A fleet can be told to stop rather than told to go where it already
  is, which a destination-shaped order cannot say.
