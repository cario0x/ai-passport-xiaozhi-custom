# StopWatch custom UI (official-core)

## Ported into official Application

- Black full-bleed 466×466 background
- Centered procedural glossy white blob (glow + core + highlight)
- Opacity-only breathing (period varies with `SetEmotion`)
- Touch: blob clickable → opacity flash + `ToggleChatState` when LVGL indev is live
- Buttons: official B1 chat/WiFi, B2 volume
- CST820B source init (documented pins; not in upstream stopwatch.cc before)

## Deferred (remain in repo-root Passport only)

- FoloToy welcome / `ui_xiaozhi` screens
- Companion face packs / official theme icons as primary avatar
- Bobo pet, plays menu, clicker, answer picker
- Passport ADC 3-key UI model

## Asset gap

**Grok Bot blob artwork was not found** in this repository or upstream.
Procedural LVGL shapes implement the agreed visual direction until an asset is supplied.
