# 0001 — NeuronCore and NeuronServer are headless

Status: accepted
Date: 2026-08-28

## Context

The engine is split so that the authoritative half can one day leave the player's machine and run
in a container: no GPU, no window, no screen. Today both halves run in one process, which makes it
easy to let a graphics type drift into a shared library without noticing — the build still links,
because the client is always there to satisfy it.

The DDS reader rewrite brought this to a head. It resolves file formats to `DXGI_FORMAT`, and its
first draft included `<dxgiformat.h>` from NeuronCore on the argument that the enum alone is not
"a graphics API". True as far as it goes, and still the wrong direction: a header that exists to
name what a device can consume is graphics, whatever it links.

## Decision

NeuronCore and NeuronServer include no graphics header — no `<d3d12.h>`, no `<dxgi*.h>`, not
`<dxgiformat.h>` — and hold no device, swapchain, window, descriptor or other data only a GPU can
use. Everything client-specific lives in NeuronClient. DirectXMath is mathematics and stays.
`Build/CheckProjectFiles.py` enforces the include rule, so the day the server is built alone the
list of things to untangle is empty rather than discovered.

## Alternatives considered

- **Mirror `DXGI_FORMAT` as a `TexelFormat` enum in NeuronCore**, same numbers, no header, with
  the client static-asserting the two agree. Keeps the DDS reader shared. Rejected: it keeps a
  graphics vocabulary in the headless library under another name, and it is a 120-entry table to
  regenerate each SDK release. Nothing on the server needs a texture's format in the first place.
- **Rely on review to keep graphics out of Core.** Rejected: the in-process build cannot fail on
  the mistake, so review is the only thing catching it, and this tree already prefers a guard that
  runs before the compiler to a rule that lives in a document (see the shared-settings check).

## Consequences

- The DDS reader moved to NeuronClient with its tests (0002 generalises this).
- A future headless build of NeuronCore + NeuronServer + GameLogic has no graphics dependency to
  strip; the Windows SDK's Win32 and Winsock headers remain, which a Windows container supplies.
- Anything that wants to be shared and is graphics-shaped has to be split: the data the server
  needs (an extent, a capsule, a format-independent size) goes in Core, the GPU half in Client.
