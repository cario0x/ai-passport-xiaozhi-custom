# StopWatch custom UI — Passport visual on official core

## Layout (current)

**Edge-to-edge 466×466 circular composition.** Passport background (`0x0B1118`)
fills every panel pixel. There is **no** 240×320 letterbox stage and **no**
black unused outer ring. Content (face / title / text / CTA) is reflowed inside
the circular safe inset (~56px); the face keeps authentic proportions (144 asset,
uniform scale to ~200 only — no stretch).

## Ported (this build)

| Item | Detail |
|------|--------|
| Welcome character page | Fullscreen circular FoloToy composition |
| Palette | `XZ_UI_BG 0x0B1118`, accent `0xF3B584`, ink `0xF7EBDD` |
| Character art | Embedded `face-00-*.png` (girl, 6 frames), uniform scale |
| Strings | `FoloToy` / `小智` / `hi，我是FoloToy的小智` / CTA `配置网络` |
| Hint (2-btn) | `请按右侧按钮开始` |
| Emotion → face | Official `SetEmotion` → Passport face frames |
| Touch | CST820B → clickable CTA hitbox matching visible pill |
| Buttons | B1 = WiFi-config/chat; B2 = confirm when disconnected else volume |

## Deferred

- Full `ui_xiaozhi` chat/settings/interrupt/avatar-picker pages
- Boy face pack, Bobo, plays, clicker, companion CPR1
- FoloToy networking core (official lifecycle retained)

## History note

- `6852f2e` white blob — superseded  
- `daa9e18` 240×320 centered letterbox — superseded (user: 鋪滿整個屏幕)  
