# Architecture decision records

One file per decision that shaped the tree, written when the decision was made, kept when it is
reversed. AGENTS.md states the rules as they stand; this folder is why they stand, and what was
turned down on the way.

Numbered in order of writing, never renumbered: `NNNN-short-slug.md`. The one thing that can move
a number is a collision, and only in one direction: two branches that both claimed `NNNN` merge,
the record already on the trunk keeps it and the arriving one moves up, citations and all, in the
merge commit. A record is never edited into a different decision — a change of mind is a new record
that names the one it supersedes, and the old one gets `Status: superseded by NNNN`.

## Template

```
# NNNN — Title

Status: accepted | superseded by NNNN
Date: YYYY-MM-DD

## Context
What was true, and what was forcing a choice. Facts, not preferences.

## Decision
One paragraph. What was decided, in the imperative.

## Alternatives considered
Each one, and the reason it lost. This is the part a rulebook cannot hold.

## Consequences
What the decision costs and what it makes easier, including what now has to be done by hand.
```

## Index

| # | Decision | Status |
|---|---|---|
| [0001](0001-headless-core-and-server.md) | NeuronCore and NeuronServer are headless | accepted |
| [0002](0002-content-readers-live-with-their-consumer.md) | A content reader lives in the library that consumes what it reads | accepted |
| [0003](0003-neighbour-list-sorts-by-surface-not-centre.md) | The neighbour list sorts by surface proximity, not centre distance | accepted |
| [0004](0004-separation-clamp-caps-the-pair-not-the-ship.md) | The separation clamp caps what a pair closes, then splits it | accepted |
| [0005](0005-ship-handles-carry-a-slot-not-an-index.md) | A ship handle carries a stable slot, not the ship's array index | accepted |
| [0006](0006-separation-solve-iterates-within-a-tick.md) | The separation solve runs several times per tick | accepted |
| [0007](0007-the-index-stores-a-whole-position.md) | The spatial index stores a whole position, and pays for it | accepted |
| [0008](0008-the-wire-format-lives-in-gamelogic.md) | The wire format lives in GameLogic | accepted |
| [0009](0009-a-snapshot-carries-a-view-record.md) | A snapshot carries a view record, not the simulation's ship | accepted |
| [0010](0010-interest-sets-are-sorted-vectors.md) | Interest sets are sorted vectors, not hash maps | accepted |
| [0011](0011-ship-meshes-are-nmo-and-its-tools-are-python.md) | Ship meshes are NMO v2, and its reference tooling is Python | accepted |
| [0012](0012-randomness-is-one-pcg32-in-neuroncore.md) | Randomness is one seeded PCG32, and it lives in NeuronCore | accepted |
| [0013](0013-allegiance-is-identity-on-the-wire.md) | Allegiance is identity on the wire, not a relation | accepted |
| [0014](0014-command-authority-is-gated-in-the-simulation.md) | Command authority is gated in the simulation, not the adapter | accepted |
| [0015](0015-npc-behavior-lives-in-gamelogic.md) | NPC behavior lives in GameLogic, inside the tick | accepted |
| [0016](0016-bodies-are-presentation.md) | A planet or an asteroid is presentation, not a simulation entity | accepted |
| [0017](0017-the-tree-gains-a-compute-pipeline.md) | The tree gains a compute pipeline, and the CPU generator stays as its reference | accepted |
| [0018](0018-shaders-are-dxil-6-7.md) | Shaders are DXIL for shader model 6.7, compiled by DXC | accepted |
| [0019](0019-fxvertex-is-packed.md) | FxVertex is packed: float position, SNORM16 normal, UNORM8 colour, half uv | accepted |
| [0020](0020-the-bake-is-the-producer.md) | The compute bake is the producer, and three silent defects had to go first | accepted |
| [0021](0021-the-network-transport-is-msquic.md) | The network transport is MsQuic, and the seam stays datagram-shaped | accepted |
| [0022](0022-msquic-workers-enqueue-and-the-owning-thread-delivers.md) | MsQuic's workers enqueue to a ring, and the owning thread delivers | accepted |
| [0023](0023-the-development-credential-is-self-signed-at-boot.md) | The development credential is self-signed at boot, and the client does not validate | accepted |
| [0024](0024-the-sky-is-a-static-catalogue-expanded-on-the-gpu.md) | The sky is a static catalogue, expanded into billboards on the GPU | accepted |
| [0025](0025-there-is-no-ground-plane.md) | There is no ground plane, and the scene pass has no grid | accepted |
| [0026](0026-a-world-is-a-picture-and-a-rock-is-generated.md) | A world is a picture, a rock is generated, and the sea is gone | accepted |
| [0027](0027-despawn-delivery-is-cursored.md) | The despawn log is read by cursor, not drained | accepted |
| [0028](0028-there-is-no-fallback-link.md) | There is no fallback link: Outpost opens QUIC or does not start | accepted |
| [0029](0029-departures-and-orders-take-the-reliable-lane.md) | Departures and orders take the reliable lane; positions stay datagrams | accepted |
| [0030](0030-the-publisher-lives-in-gamelogic.md) | The publisher lives in GameLogic, and serves N subscribers | accepted |
| [0031](0031-listener-slots-are-recycled.md) | Listener slots are recycled, so backlog means concurrency | accepted |
| [0032](0032-the-dialing-end-owns-the-lanes-stream.md) | The dialing end owns the reliable lane's stream | accepted |
| [0033](0033-pathfinding-is-islands-on-a-world-fixed-lattice.md) | Pathfinding is islands of architecture, on a lattice fixed to the world | accepted |
| [0034](0034-a-routes-version-is-the-whole-worlds.md) | A route's version is the whole world's, not its island's | superseded by 0059 |
| [0035](0035-ship-hulls-are-authored-in-glb-and-converted-to-nmo.md) | Ship hulls are authored in GLB and converted to NMO, and ObjParser is deleted | accepted |
| [0036](0036-a-liveried-surface-is-declared-and-the-combine-is-a-multiply.md) | A liveried surface is declared by its material, and the combine is a multiply | accepted |
| [0037](0037-the-universe-layout-is-static-content-in-gamelogic.md) | The universe layout is static content in GameLogic | accepted |
| [0038](0038-stations-are-ships-with-a-side-table.md) | Stations are ships with a side table | accepted |
| [0039](0039-standings-are-simulation-state-stated-per-subscriber.md) | Standings are simulation state, stated per subscriber | accepted |
| [0040](0040-a-departure-carries-a-cause.md) | A departure carries a cause on the wire | accepted |
| [0041](0041-the-protector-response-reacts-to-stated-acts.md) | The protector response reacts to stated acts, not senses | accepted |
| [0042](0042-a-route-never-asks-for-a-point-the-wall-forbids.md) | A route never asks for a point the wall forbids | accepted |
| [0043](0043-a-server-is-told-what-to-be-by-a-file.md) | A server is told what to be by a file the composition root reads | accepted |
| [0044](0044-the-client-gets-a-copy-queue-and-handles-get-generations.md) | GpuDevice gains a copy queue, and a render handle gains a generation | accepted |
| [0045](0045-the-tick-rate-is-fixed-at-60-hz.md) | The tick rate is fixed at 60 Hz, and capacity is bought elsewhere | accepted |
| [0046](0046-the-wires-sector-index-is-32-bits.md) | The wire's sector index is 32 bits, and a position rides a 0.125 m lattice | accepted |
| [0047](0047-identity-is-a-shard-scoped-serial.md) | Identity is a shard-scoped serial, carried for life; the handle stays in-process | accepted |
| [0048](0048-fleets-are-simulation-state-at-fleet-grain.md) | Fleets are simulation state at fleet grain, named by an owner and a slot | accepted |
| [0049](0049-orders-name-a-fleet-not-ships.md) | Orders name a fleet, not ships: one slot, one kind, no ship list | accepted |
| [0050](0050-a-fleet-defends-itself-against-stated-acts.md) | A fleet defends itself against stated acts, at fleet grain | accepted |
| [0051](0051-the-ledger-is-asked-for-not-broadcast.md) | The ledger is asked for, not broadcast | accepted |
| [0052](0052-gunnery-is-deterministic-and-the-fire-pass-states-the-acts.md) | Gunnery is deterministic, and the fire pass states the acts | accepted |
| [0053](0053-fire-events-ride-the-datagram-lane.md) | Fire events ride the datagram lane | accepted |
| [0054](0054-a-design-is-amended-in-place-as-its-slices-land.md) | A design is amended in place as its slices land | accepted |
| [0055](0055-the-galaxy-is-one-seed-and-its-gates-are-the-relative-neighborhood-graph.md) | The galaxy is one seed and a pin table, and its gates are the relative neighborhood graph | accepted |
| [0056](0056-a-jump-is-a-despawn-and-a-spawn-under-one-identity.md) | A jump is a despawn and a spawn under one identity, and a fleet crosses whole | accepted |
| [0057](0057-the-save-is-a-versioned-file-and-a-refused-one-stops-the-boot.md) | The save is a versioned file, and a refused one stops the boot | accepted |
| [0058](0058-a-universe-is-authored-by-a-tool-not-by-the-program-that-runs-it.md) | A universe is authored by a tool, not by the program that runs it | accepted |
| [0059](0059-a-route-is-scoped-to-the-island-that-planned-it.md) | A route is scoped to the island that planned it (supersedes 0034) | accepted |
| [0060](0060-the-game-is-its-own-benchmark.md) | The game is its own benchmark: one persistent plane, at fleet grain, with an economy under it | accepted |
| [0061](0061-the-save-is-migrated-on-read.md) | The save is migrated on read, and a format is retired only by record | accepted |
| [0062](0062-an-owner-is-not-a-faction-and-not-an-entity.md) | An owner is not a faction and not an entity | accepted |
| [0063](0063-the-partition-is-a-function-of-the-layout.md) | The partition is a function of the layout, and contiguity is what decides it | accepted |
| [0064](0064-a-mount-is-bound-to-its-art-by-a-client-table-of-submesh-names.md) | A mount is bound to its art by a client table of submesh names | accepted |
