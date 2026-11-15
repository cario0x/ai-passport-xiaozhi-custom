# Flash safety — official-core vs Passport device

**Status:** documentation only. No flash authorized in the migration turn.

## Incompatibility summary

| Region | Passport StopWatch (on device) | Official v2.6.0 | Risk if flashed blindly |
|--------|--------------------------------|-----------------|-------------------------|
| NVS 0x9000/0x4000 | yes | yes | Low if not erased; key schema may differ |
| 0xd000 | phy_init | **otadata** | Overwrites phy_init |
| 0xf000 | (inside cardid/phy area) | phy_init | Layout diverge |
| 0x20000 | factory app ~3–4MB | ota_0 ~4MB | App replace OK size-wise |
| 0x406000+ | imgava/assets/petart/companion | (inside ota_1 / free until 0x800000) | Resource loss if ota_1 written |
| 0x800000 | (often unused / beyond companion) | **assets 8MB** | Wipes anything mapped there |

## Recommended future flash (when authorized)

1. Backup NVS (user-owned; agent must not read secrets).
2. Write **only**:
   - bootloader @ 0x0
   - partition-table @ 0x8000 (official v2/16m)
   - otadata @ 0xd000 (accept phy_init loss at old address)
   - app ota_0 @ 0x20000
   - assets @ 0x800000
3. **Omit** `erase_region` on NVS `0x9000`.
4. Expect Passport companion/petart UI assets to be unavailable under official core.
5. Re-test WiFi; rebind **only** if user confirms binding is actually broken.

## Do not

- `esptool erase_flash`
- Automatic NVS migration scripts
- Force Xiaozhi cloud rebind
- Claim hardware success from compile-only artifacts
