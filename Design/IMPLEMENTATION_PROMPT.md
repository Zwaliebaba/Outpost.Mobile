# Outpost — Main Game Screen HUD: Implementation Prompt

> Feed this file to your coding agent (or use it as the work order) alongside the
> reference screenprint in `screenprints/main-screen.png` and the icon DDS files in `icons/`.

## Task

Implement the in-game HUD for Outpost's main game screen as mocked in
`screenprints/main-screen.png` (1280×800 reference, tablet landscape, pure touch).
Build on the existing engine: text via `Neuron::TextRenderer` (FontId::Ui = `Fonts\EditorFont.dds`),
quads via the text pipeline's solid glyph (`BitmapFont::SOLID_GLYPH`, DEL byte draws a block),
tuning constants in `Outpost/ViewTuning.h`, HUD drawing grows in `Outpost/Hud.*` per its header
comment ("it is the thing that grows every week"). Do not let any of this feed back into
`GameLogic` — HUD reads `Game::World` / `WorldView` state only.

## Visual language (Darwinia-inspired, flat)

- Square corners everywhere. No blur, no glow, no gradients.
- Panels: fill rgba(11,13,16,0.82); 1px outline rgba(72,92,112,0.45) — derive from GRID_COLOUR.
- Text: EditorFont atlas, square-cell fixed pitch (its natural wide tracking IS the look).
- All-caps labels. Label colour #5F7488; value colour = HUD_COLOUR (0.78,0.87,0.96).
- Accents: selection green SEL_RING_COLOUR (0.35,0.95,0.55) for active/positive;
  marker amber MARKER_COLOUR (0.95,0.78,0.28) for alerts/orders; hostile red ~(0.95,0.43,0.35).
- Optional CRT scanlines over HUD only: 1px dark line every 3px, ~16% black.

Suggested new constants for ViewTuning.h:
HUD_PANEL_FILL, HUD_PANEL_OUTLINE, HUD_LABEL_COLOUR, HUD_ACCENT_GREEN, HUD_ACCENT_AMBER, HUD_ALERT_RED.

## Layout (px at 1280×800; multiply by dpiScale like Hud.cpp does)

1. **Resources — top-left (16,14)**
   Two panels side by side, gap 10, padding 10×16:
   `CR 12,480 +42/m` and `ALLOY 3,215 +8/m`. Label small, value large, income in accent green.

2. **Sim readout — top-centre (optional/debug)**
   `TICK n · TIME 1.00x`. Hidden by default; the existing debug Hud stats feed it.

3. **Minimap — top-right (right:16, top:14), 212 wide**
   Header strip (h≈30, bottom rule): left `SECTOR 7-K`, right `CONTACTS n` in amber.
   Map area h=140: faint 28px grid, friendly dots 4px green, hostile dots 4px red,
   camera frustum as a 1px light trapezoid (project the view corners onto the ground plane).

4. **Function rail — left edge, vertically centred (left:16)**
   Four 60×60 buttons, gap 10: RSRCH / WALLET / STORE / UNIVRS.
   Icon 22px centred + label beneath. Pressed/active state: outline + fill shift to accent green
   (rgba(89,242,140,0.7) outline, 8% green fill). Each opens its screen (out of scope here).
   Icons: `icons/Icon{Research,Wallet,Storage,Universe}.dds` — 64×64, uncompressed 32-bit RGBA
   (byte-aligned masks, parses with `NeuronCore/DdsImage`), white-on-transparent coverage masks:
   sample and multiply by vertex colour exactly like the font atlas, so one file serves every tint.

5. **Event log — bottom-left (16, above bottom bar by 16)**
   Up to 3 rows, newest on top. Row: 2px left rule in severity colour (amber alert /
   green friendly / grey info), timestamp `MM:SS` in label colour, message text.

6. **Bottom bar — full width, h=96, top rule**
   Left→right, 1px vertical separators:
   - Control groups: five 48×56 buttons `1..5` with ship count `×n` (or `—` when empty).
     Active group: green outline + 12% green fill. Tap = select group; long-press = assign.
   - Selection summary: group name in green + `7 SELECTED · 2 FRIGATE · 4 INTERCEPTOR · 1 CRUISER`.
   - Stats (selection aggregate): HULL bar (grey fill SHIP_COLOUR) + %, SHIELD bar (green) + %,
     SPEED value `38 m/s`, ORDER state in amber (`IDLE / MOVING / ALIGNING` from `OrderState`).
   Bars: 6px tall, track rgba(255,255,255,0.07), squared ends.

## Touch rules

- Hit targets ≥44px (rail buttons 60, group tabs 48 wide).
- HUD consumes pointer events over its panels so taps there never reach PointerTracker
  (no accidental ground orders through the bottom bar).

## Acceptance

- Renders over the live scene at any resolution; layout anchored to corners/edges, scaled by dpiScale.
- No allocation per frame in the HUD draw path (fixed buffers like Hud.cpp's `char line[512]`).
- All colours/paddings sourced from ViewTuning.h constants, not literals at the call site.
- Icon DDS files load through DdsImage without a trace warning.
