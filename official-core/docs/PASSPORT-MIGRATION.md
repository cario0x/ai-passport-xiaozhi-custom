# Passport → official v2.6.0 migration notes

**Decision (Taipei 11:18):** official `78/xiaozhi-esp32` **v2.6.0**
(`4de00aed740595a8713b01f8332b9b2cf30685bc`) is the application core.
Custom UI is ported *into* that core. The prior Passport fork remains at repo
root (non-destructive). This tree lives under `official-core/`.

## What runs as the application core (ported / retained from official)

| Area | Status |
|------|--------|
| `Application` lifecycle | Official — unchanged |
| Voice / audio (ES8311, wake word, Opus, AEC paths) | Official |
| WiFi provisioning (AP / BluFi) | Official |
| Cloud activation / dialogue protocol (MQTT+UDP / WebSocket) | Official |
| MCP device tools | Official |
| StopWatch board pins (I2C 47/48, I2S, CO5300 QSPI, Button1=G2, Button2=G1) | Official |
| Partition table `partitions/v2/16m.csv` | Official (see flash safety) |

## Custom StopWatch adaptations in this tree

| Feature | Status | Notes |
|---------|--------|-------|
| Passport welcome character UI | **Ported** | Edge-to-edge 466 circular; FoloToy brand; face-00 art; hi/CTA strings |
| Round fullscreen (no letterbox) | **Ported** | Passport BG fills panel; content in safe inset (user: 鋪滿整個屏幕) |
| Hint copy for 2 buttons | **Adapted** | `请按右侧按钮开始` (not 3-key “最下方”) |
| Face frames from Passport assets | **Ported** | Embedded `face-00-00..05` (girl); boy pack deferred |
| Touch → CTA | **Source-routed** | CST820B → LVGL indev → peach “配置网络” pill → official `ToggleChatState` |
| Two physical buttons | **Wired to official Application** | B1=WiFi-config/chat; B2=confirm when disconnected else volume |
| Full Passport `ui_xiaozhi` pages (settings/chat bubbles/avatar picker) | **Deferred** | Welcome + emotion→face only — not a full Passport experience claim |
| Bobo / plays / clicker / companion CPR1 partition | **Deferred** | Root Passport tree only |
| FoloToy networking core | **Not ported** | Official voice/WiFi/cloud lifecycle retained |
| Procedural white Grok blob (`6852f2e`) | **Superseded** | User chose Passport interface over blob |

## Asset notes

Passport girl face PNGs reused from `assets/xiaozhi/faces/face-00-*.png`.
Chinese welcome strings embedded as label bitmaps (WenKai-equivalent system font
render at build time) so official core does not depend on Passport `ui_font_*` /
`companion` partition. Full glyph fonts and boy faces remain deferred.

## Flash / partition safety (compile-only this turn — do NOT flash yet)

### Official v2.6.0 (`partitions/v2/16m.csv`)

```
nvs       @ 0x9000   size 0x4000
otadata   @ 0xd000   size 0x2000
phy_init  @ 0xf000   size 0x1000
ota_0     @ 0x20000  size 0x3f0000   (~4MB)
ota_1     @          size 0x3f0000
assets    @ 0x800000 size 8M
```

### Current device firmware (Passport fork, e.g. `6e8cba4b`)

Uses StopWatch-compatible factory layout with `cardid`, `imgava`, `petart`,
`companion` etc. Example (fork `partitions-stopwatch.csv`):

```
nvs       @ 0x9000   size 0x4000     ← SAME offset/size as official
phy_init  @ 0xd000   size 0x1000     ← CONFLICT: official puts otadata here
cardid    @ 0xe000   ...
factory   @ 0x20000  size 0x300000
... imgava @ 0x406000, petart @ 0x500000, companion @ 0x600000 ...
```

### Preservation strategy (before any future authorized flash)

1. **NVS @ 0x9000/0x4000** — offset/size match official. WiFi credentials *may*
   survive an app-only write that does **not** erase NVS. **Not guaranteed** across
   different NVS key namespaces (Passport vs official Xiaozhi).
2. **Do not** `erase_flash` / do not rewrite NVS / do not rebind by default.
3. Flashing official `otadata` @ `0xd000` **overwrites** Passport `phy_init` at that
   address; flashing official `ota_0` app is OK at `0x20000` but official assets at
   `0x800000` **overwrite** Passport `companion` / high resource regions if those
   were written below 16MB.
4. Passport resource partitions (`imgava`, `petart`, `companion`, `cardid`) are
   **not** read by official core — they become dead flash unless a dual-layout
   strategy is designed later.
5. **Binding:** do not assume lost; do not force rebind; never read private backups/keys.

**This agent turn: compile only. No flash, reset, or NVS access.**

## Build

Requires ESP-IDF **≥ 6.0.1** (recommend `release/v6.1`). IDF 5.x will not build v2.6.0.

```bash
source /path/to/esp-idf-v6.1/export.sh
cd official-core
python3 scripts/build.py m5stack/stopwatch --name m5stack-stopwatch-passport
```
