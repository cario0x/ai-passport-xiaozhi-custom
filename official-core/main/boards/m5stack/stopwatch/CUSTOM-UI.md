# StopWatch custom UI — Passport visual on official core

## Ported (this build)

| Item | Detail |
|------|--------|
| Welcome character page | Centered **240×320** Passport stage on **466×466** black letterbox |
| Palette | `XZ_UI_BG 0x0B1118`, accent `0xF3B584`, ink `0xF7EBDD` (from `ui_xiaozhi_theme.h`) |
| Layout coords | Brand (16,16), title (93,12), face (48,49), hi y=205, hint y=237, CTA (12,274) 216×32 r12 |
| Character art | Embedded `assets/xiaozhi/faces/face-00-*.png` (girl, 6 frames) as RGB565A8 |
| Strings | `FoloToy` / `小智` / `hi，我是FoloToy的小智` / CTA `配置网络` |
| Hint (2-btn adapted) | `请按右侧按钮开始` (was Passport 3-key `请按右侧最下方的按钮`) |
| Emotion → face | Official `SetEmotion` maps to Passport face frames (idle/listen/think/talk/happy) |
| Touch | CST820B source path → clickable CTA → `ToggleChatState` |
| Buttons | B1 = WiFi-config (while starting) / chat; B2 = confirm while disconnected, else volume |

## Deferred (still Passport-root only — not claimed as ported)

- Full `ui_xiaozhi` chat/settings/interrupt/volume/avatar-picker pages
- Boy face pack (`face-01-*`), home avatars, Bobo, plays, clicker
- Companion partition CPR1 loader / petart fonts
- FoloToy networking / bind / Passport `xiaozhi` protocol stack
- White mic pill (Passport welcome CTA is peach WiFi “配置网络”, not a white mic)

## Not this UI

Procedural white Grok blob from `6852f2e` was an intermediate baseline and is **removed**.
