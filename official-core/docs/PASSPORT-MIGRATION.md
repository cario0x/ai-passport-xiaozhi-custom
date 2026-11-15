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
| Touch → CTA | **Fixed semantics** | CST820B → LVGL indev → “配置网络” → `EnterWifiConfigMode` / AP hint (NOT `ToggleChatState`) |
| Two physical buttons | **Fixed semantics** | B1=WiFi/chat (no audio-test while provisioning); B2=confirm when disconnected else volume + toast |
| Status / notification bar | **Unhidden** | Was hidden under fullscreen → volume/WiFi hints invisible despite working events |
| Full Passport `ui_xiaozhi` pages (settings/chat bubbles/avatar picker) | **Deferred** | Welcome + emotion→face only — not a full Passport experience claim |
| Bobo / plays / clicker / companion CPR1 partition | **Deferred** | Root Passport tree only |
| FoloToy networking core | **Not ported** | Official voice/WiFi/cloud lifecycle retained |
| Procedural white Grok blob (`6852f2e`) | **Superseded** | User chose Passport interface over blob |

## Hardware evidence — package `68690f0` (official-core fullscreen)

Physical flash on correct MAC confirmed all five writes; boot project=`xiaozhi` v2.6.0;
ELF `1e1559acd` matching package. User t63u feedback applies to **this** build, not the
old Passport fork. Bounded log ~68s, no panic. Observation ended before 90s (worker
interruption); no further device action after that window.

| t(ms) | Event | Diagnosis |
|------:|-------|-----------|
| 1378 | CST820B RST IOE_G4, I2C 0x15, INT G13 → `lvgl_port_add_touch` | Touch driver path OK |
| 1588 | CST816S IC id 183; indev pointer registered | Chip probe OK |
| 1658 | Fullscreen display | UI path OK |
| 7618 | Passport CTA touch **CLICKED** (LVGL indev) | **Touch not lost** |
| 7618 | CTA while disconnected → `ToggleChatState` | **Wrong callback semantics** |
| 7628 | `wifi_configuring` → `audio_testing` | Official `HandleToggleChatEvent` side-effect |
| 10038–16098 | volume 80/90/100 | Events fired; **source not GPIO-labeled** — do not assume button |
| 16928 / 18658 | audio_testing ↔ wifi_configuring | Same ToggleChat flip |
| 20658–22578 | volume 0/10/20/30 | Same as above |
| 28558 | Audio testing queue full, stop | Stuck in useless audio-test path |
| boot | 3× GPIO0 conflict/overwritten warnings | Open partition/pin conflict; not this CTA bug |
| boot | Missing `robot_2`, `gear` expression resources | Assets gap; Alert/emoji may soft-fail |
| boot | Codec init/open OK | Not physical audio proof |
| — | Provisioning AP/DHCP/webservice up; `wifi_configuring` | No confirmed station/cloud |
| — | NVS not explicitly written by esptool; RF calib invalid then firmware saved itself | Do **not** claim NVS byte-unchanged |

**Verdict:** input hardware and LVGL indev work. User “no useful action” = CTA/`ToggleChatState`
→ audio-test + hidden `status_bar_` (ShowNotification invisible). Fix: CTA/B1/B2 while
disconnected call `HandleProvisionOrChat` (EnterWifiConfigMode or AP SSID/URL toast);
keep status bar visible; labeled `INPUT_*` logs.

## Hardware evidence — package `a9ad19d` / app `d72d8ae8…` (t67u 16:20)

Confirmed flash of input-semantics build. Wi-Fi + MQTT to `api.tenclass.net` connected;
idle + WakeNet in 90s logs. User: **左右按键正常 但是无法触摸屏幕** — B1/B2 work; screen
touch does not. Photo (real device, not simulation): fullscreen Passport welcome still
showing `配置网络` + `请按右侧按钮开始` while already online; status text **聆听中...**
smeared over the avatar forehead.

| Observation | Source diagnosis (not claimed sole root cause) |
|-------------|--------------------------------------------------|
| Buttons work; CTP IC=183 still inits; no `INPUT_*` / CTA click in 90s | Input framework not wholly dead; CTP driver not declared dead |
| Contrast: `68690f0` had one CTA CLICKED | a9ad19d touch init unchanged except status_bar unhide/foreground |
| `status_bar_` over face, default CLICKABLE | Steals upper-screen hits; explains smeared 聆听中 (photo) |
| Fullscreen `stage_` default CLICKABLE, no handler | Empty taps silent (no INPUT_*) |
| INT=G13 → LVGL EVENT mode | Touches only read on INT edge; poll-mode is a reliability lever |
| Stale provision CTA while STA+MQTT up | Welcome layer never gated on `IsConnected()` |

**Follow-up fix:** status pill top-chord + non-clickable; stage non-clickable; CTA only when
`!connected`; online face/chord → ToggleChat; CTP **poll-mode** (`int=NC`) + `INPUT_RAW_TOUCH`
coord logs. B1/B2 paths unchanged when connected.

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
