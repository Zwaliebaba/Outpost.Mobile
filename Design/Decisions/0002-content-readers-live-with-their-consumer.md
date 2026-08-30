# 0002 — A content reader lives in the library that consumes what it reads

Status: accepted
Date: 2026-08-29

## Context

NeuronCore began as the home of every file-format parser — OBJ/MTL and DDS — on the reasoning that
a parser has no game semantics and no graphics API, so it belongs in the layer everything shares.
0001 moved the DDS reader out because it speaks `DXGI_FORMAT`. That left the OBJ reader and
`MeshData` in Core on a different argument: a mesh is also collision geometry, which the server
will need.

That argument does not hold against the collision design ([`Design/Archive/Collision.md`](../Archive/Collision.md)
§4–5). Collision shapes are to be **authored** capsules in a `HullSpec` table in GameLogic, and
deriving them from mesh bounds is explicitly rejected there — it would drag wingtips and antennae
into the hull and put the collision shape in the renderer's reach. Today the only consumer of a
parsed mesh is `MeshLibrary`, which uploads it to a vertex buffer, and `WorldView`, which reads its
bounds for picking and thruster points. Both are presentation.

## Decision

A content reader lives next to the thing that consumes what it reads, not in the shared library
by default. Meshes are consumed by the renderer, so `ObjParser` and `MeshData` move to
NeuronClient with their tests, alongside `DdsImage`. NeuronCore keeps what every layer uses:
`FileSys`, diagnostics, easing, the clock, `Transport`.

The test for where a reader goes is "who calls it", not "does it include a graphics header". A
reader with no consumer on the server is client code even when it would compile on the server.

## Alternatives considered

- **Keep `MeshData` in Core for future collision use.** Rejected: the collision design says the
  server never reads a mesh; sizes arrive as authored numbers. Keeping the type "just in case" is
  the reasoning that put `DXGI_FORMAT` in Core.
- **Split `MeshData` into a Core bounds struct and a Client vertex struct.** Rejected for now:
  nothing on the server wants bounds either. Revisit if `HullSpec` is ever generated from meshes
  offline — that would be a tool, and it could link NeuronClient.
- **Leave everything where it is.** Rejected: NeuronCore's own umbrella comment promises "format
  parsers", which is a category, not a reason, and the category was how the DDS mistake happened.

## Consequences

- NeuronCore has no parser left; its "Content" filter is empty and can go when the next file is
  removed from it.
- `NeuronCoreTests` shrinks to easing; mesh and texture tests run under `NeuronClientTests`.
- The AGENTS.md repository map and the §2 rule are updated in the same commit, per the rule that a
  sentence made false by a change changes with it.
