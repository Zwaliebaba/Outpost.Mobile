# 0036 — A liveried surface is declared by its material, and the combine is a multiply

Status: accepted
Date: 2026-08-30

## Context

A hull is painted by two authorities. Its structure — plating, canopy glass — is the model's,
identical on every ship of that class whoever flies it. Its livery — the panels and trim that say
whose ship this is — is the faction's, and the same model has to wear azure for Core Vanguard
Command, red for the Vandal Collective, or whatever a player picked.

Until this slice the client had neither half of that. Every hull was tinted as one object:
`albedo = lerp(tint, the mesh's own vertex colour, mix)`, with a ship colour and a material mix per
faction. Two things had to be decided to replace it — which surfaces the faction owns, and what
"the faction paints it" arithmetically means.

## Decision

**A liveried surface is declared by its material**, as a bit in the mesh file
(`NmoRenderFlags::RaceTinted`, [`Design/Archive/NmoFormat.md`](../Archive/NmoFormat.md) §5.5), carried per vertex
into the shader as `MeshVertex::race`. `Art/Meshes/GlbToNmo.py` sets it, by material name, once, at
conversion, from §13.1's table; nothing in the loader or the renderer knows a material name.

**The combine is a multiply**: `albedo = lerp(col, livery * col, race)`. On a flagged material the
authored `baseColour` is a *shade* rather than a colour — the converter writes the corpus's liveried
materials greyscale, and an `Exhaust` marker's authored colour is replaced by its luminance for the
same reason.

`MeshInstance::tint` keeps its shape and changes meaning: `rgb` is the livery, `w` is a highlight
lift applied after lighting, which is what selection becomes.

## Alternatives considered

- **Recognise a liveried material by its name in the loader** — "the material called `plate` is the
  liveried one". Rejected: it is a convention a rename breaks silently, in content, at run time,
  where a flag is authored, round-tripped and visible in the file. The name convention still exists,
  but it lives in the tool that runs once and turns names into flags.
- **Keep the lerp and give each faction a ship colour and a mix**, which is what the tree did for a
  year. Rejected, and this is the alternative someone will propose again, so the numbers are
  here: the rule mixed a tint against whatever the model was painted, so the two hues argued. The
  corpus is authored with bright green panels (`Kd 0.50 0.93 0.13` on the Interceptor); a friendly's
  panel came out `#86C75E`, and the *same panel under a red tint at the same mix* came out `#B0A932`
  — olive, with red barely ahead of green, which is not a red ship. The old `ViewTuning.h` worked
  that through and answered it by dropping the hostile mix to a fifth, which bought a red enemy by
  throwing away four fifths of the model's paint. A multiply cannot argue, because the shade it
  multiplies has no hue to contribute: the faction's colour survives exactly, and the shade ladder
  (plate 0.45 < accent 0.80 < thruster 1.00) survives with it at every livery.
- **A second livery colour, or a per-submesh override.** Rejected as not free: it is a second flag
  bit and a second instance channel, and no content wants one.
- **Tinting the whole hull and accepting it.** Rejected: it is what made a canopy indistinguishable
  from plating, and it is why `hull` was authored at `0.024` (near black) — the tint lifted it to a
  mid grey on screen, so the authored number meant nothing on its own.

## Consequences

- The corpus's liveried materials are greyscale and *must stay* greyscale. A hue that survives into
  a flagged material is an asset bug: the multiply discards it, so the file would state an intent
  the renderer throws away. The loader does not police this, because content it can render is not
  content it should refuse.
- The rendered brightness of the unliveried materials had to be re-authored, not preserved as
  numbers. `hull` went from `0.024` to `0.27` and `glass` to `0.12`: nothing lifts them now, so the
  authored value is the final one, and it is the *rendered* result that had to be kept.
- Selection stops being a colour and becomes a lift towards white. Under liveries a mint-green
  selected hull reads as a different faction, and the player's own livery might be mint.
- The HUD stays a *relation* language where the scene is an identity one. Liveries never reach the
  minimap: a player who picks a red-ish livery must not turn their own dots into the colour the HUD
  uses for hostiles. `HUD_ALERT_RED` re-derives from `LIVERY_VANDAL`, which is the same
  relation-follows-the-faction rule it always had.
- A hostile's plume is no longer a faction signal in its own right; it is the flying faction's
  livery, which is the same answer arrived at honestly.
- `SELECTED_COLOUR` is deleted. It was the hull tint and nothing else — the ring and the HUD's green
  come from `SEL_RING_COLOUR` and always did — so once selection became a lift it had no readers.
