# Functional Coverage — Emulator vs Real Nivona App

**Question asked:** "If I point the real Nivona Android app at this
emulator, will every feature work, or are there gaps?"

**Short answer:** the core flow works end-to-end. The handshake,
status notifications, brew commands, manipulation prompts,
maintenance gauges and per-family counters are all implemented
against the app's actual wire format. There are six identified
**functional gaps** in non-core or speculation-only flows, each
catalogued below with impact and mitigation.

This document was produced by parallel inventories of (a) what the
app expects (sources: `melitta-ha-integration/docs/NIVONA.md`,
`NIVONA_RE_NOTES.md`, `NIVONA_EMULATOR_AUDIT_V2.md`, and the
HA-side parser `brands/nivona.py`) and (b) what the emulator
currently implements (sources: `main/*.c`, `main/*.h`).

## Coverage matrix

Legend: ✓ = match, ⚠ = partial / gap (see notes), ✗ = missing.

| Area | What app expects | Emulator status | Notes |
|------|------------------|-----------------|-------|
| **Local name pattern** | `<10-digit>-----` per family (e.g. `8107000001-----`) | ✓ | `nivona_families.c:111-118` per family |
| **company_id in mfg_data** | `0x0319` (Eugster) | ✓ | `nivona_ble.c` MFR_DATA |
| **customerId in mfg_data** | `0xFFFF` (ushort.MaxValue) | ✓ | `nivona_ble.c` MFR_DATA |
| **mfg_data tail bytes 4-7** | Decompile-obfuscated; not directly extractable | ⚠ G1 | Emulator emits zeros; see G1 below |
| **Service UUID** | `0000ad00-b35c-11e4-9813-0002a5d5c51b` | ✓ | `nivona_gatt.c:23` |
| **AD02 (notify)** | notify only | ✓ | `nivona_gatt.c:122-134` |
| **AD03 (write)** | write + write-no-rsp | ✓ | `nivona_gatt.c` |
| **AD01 / AD04 / AD05** | discovered but unused | ✓ stub | OK — app doesn't drive them |
| **AD06 (device name)** | r/w | ✓ | `nivona_gatt.c:92-108` |
| **DIS service** | NOT used by Nivona app (model from `local_name`) | ✓ implemented anyway | `nivona_dis.c` — harmless extra |
| **HU handshake (verifier)** | 2-round NIVONA_HU_TABLE CRC fold | ✓ | `nivona_crypto.c:54-67` |
| **HU session_key prefix** | 2 bytes returned in response, prepended on all subsequent encrypted frames | ✓ | `nivona_dispatch.c:92-102` |
| **RC4 master key** | `NIV_060616_V10_1*9#3!4$6+4res-?3` | ✓ | `nivona_crypto.c` |
| **HX layout** | `process(i16) sub_process(i16) message(i16) progress(i16)` BE | ✓ | `nivona_fsm.c:99-111` — info=high, manip=low byte of message |
| **HX `process` codes** | family-aware (8000: 3/4 ready/brewing; others: 8/11) | ✓ | `nivona_families.c` |
| **HX `message` byte 11** | move-cup-to-frother | ✓ | `MANIP_MOVE_CUP=11` |
| **HX `message` byte 20** | flush-required | ✓ | `MANIP_FLUSH_REQUIRED=20` |
| **HX `info` byte** | always 0 (verified app-side) | ✓ | warning emitted on non-zero |
| **HE start brew** | `[0, mode, 0, selector, 0, flags, 0…]` 18 bytes | ✓ | `nivona_dispatch.c:235-293`, validates mode + flags |
| **HE flag 0x00 (ChilledBrew)** | accepted on NICR 1040 firmware | ⚠ G2 | accepted but no chilled-specific behavior |
| **HW temp-recipe register** | strength/coffee/water/milk amounts written to reg 9001 before HE | ✓ (since emu-v0.8.3) | per-family field layouts in `nivona_family_t.recipe_layout`; `brew_task` reads + applies strength + volume scaling |
| **HZ cancel** | abort active brew | ✓ | `nivona_brew_cancel` |
| **HY confirm prompt** | clears manipulation, re-evaluates | ✓ | `nivona_dispatch.c:322-333` |
| **HD reset-default** | erase a single HR id | ✓ | `nivona_store_erase_num` |
| **HV version** | opaque ack | ✓ stub | "NIVONA v1.0" |
| **HI features** | byte 0 bit 0 = IMAGE_TRANSFER | ✓ | 10 zero bytes (no OTA) |
| **HA / HB alpha r/w** | profile names, machine name | ✓ | `nivona_store_set_alpha` |
| **HC read recipe** | NOT used by Nivona app per audit | ✓ stub | empty 66 bytes |
| **HJ write recipe** | NOT used by Nivona app per audit | ✓ stub | ACK only |
| **HR per-family recipe counters (200..212)** | each family has its own subset | ✓ | `nivona_stats.c` `recipe_id_mask` per family |
| **HR total counter** | id 213 (8000/900), 215 (1000/1030/1040), absent on 600/700/79x | ✓ | `nivona_stats.c` `total_id` |
| **HR maintenance counters (214..224)** | per family | ✓ | clean_coffee / clean_frother / rinse / rinse_frother / rinse_filter / filter_change / descale / via_app / pot in stats table |
| **HR universal gauges** | 600 (descale %), 610 (BU clean %), 620 (frother %), 640 (filter %) | ✓ | seeded to 100 % at first boot |
| **HR universal warnings** | 601 / 611 / 621 / 641 | ✓ | seeded to 0 at first boot |
| **HR filter_dependency** | 642 (8000), 101 (900/1000), 105 (600/700/79x) | ✓ | `filter_dep_id` per family |
| **HW per-family settings (101..119)** | language, clock, brightness, filter type, etc. — varies per family | ⚠ G4 | round-trips (HW writes, HR reads back) but not seeded with defaults |
| **Alpha IDs for profile names** | not documented for Nivona | ⚠ G5 | emulator seeds IDs 0x0100..0x0103 as placeholders |
| **Maintenance cycle trigger** | unknown — app may use HE 40+ (descale) or wait for machine | ⚠ G6 | emulator only triggers via CLI `maint`, NACKs HE selectors > family.recipe_count |
| **Brew counters update** | per-recipe / total / via_app HR ids bumped on success | ✓ | `apply_wear_after_brew`, batched NVS |
| **Wear gauges degradation** | gauges drop per brew, raise warning flags | ✓ | `apply_wear_after_brew` — filter −1 %/brew, BU −1 %/2 brews, descale −1 %/5 brews |
| **JustWorks pairing** | Just-Works SMP | ✓ | NimBLE SM with sm_bonding=1 |
| **BLE bond persistence** | bond survives reboot | ✓ | NVS BLE bond store |
| **Random static address (F1:..)** | matches real machine | ✓ | `main.c:42-59` |

## Functional gaps

### G1 — Manufacturer-data tail bytes 4-7 are zero placeholders

**Severity:** Low / unknown
**Location:** `nivona_ble.c` `MFR_DATA`
**What the app does:** During scan, the Nivona app validates only
`company_id == 0x0319` and `customerId == 0xFFFF` before reading the
rest. The remaining 4 bytes may encode hardware version / serial /
provisioning bits but the relevant decompile method
(`EFLibrary.CheckDiscovered`) is control-flow-flattened with
encrypted string tables, so the exact expected bytes can't be
recovered without dynamic instrumentation against a real machine.
**Symptom if app rejects:** scan never surfaces emulator → app shows
"machine not found".
**Current observation:** the app accepts the emulator's scan packet,
so this is not blocking in practice.
**Action:** keep zeros; revisit if a future Android version starts
filtering on these bytes.

### G2 — ChilledBrew flag (HE flags=0x00) has no per-recipe effect

**Severity:** Low
**Location:** `nivona_dispatch.c:handle_he` (accepts 0x00 / 0x01),
`nivona_brew.c:brew_task` (ramp depends only on recipe category).
**What the app does:** On NICR 1040 with firmware `1040A015G15` the
app can issue `HE flags=0x00` to request a "ChilledBrew" — different
temperature ramp, lower wear on heating element.
**Symptom:** the brew completes at the same speed as a regular brew;
wear counters tick at the same rate.
**Action:** add a `chilled` field on `brew_arg_t` and a faster ramp
in `RAMPS` table when set. Only NICR 1040 is affected. Easy fix; not
implemented because we have no way to verify the actual chilled
ramp duration against real hardware.

### G3 — Temp-recipe HW overrides (register 9001) — CLOSED in emu-v0.8.3

**Status:** ✓ closed.
**Location:** `nivona_brew.c::read_overrides` + `apply_override_scale`,
field offsets in `nivona_family_t.recipe_layout`
(`nivona_families.c`).
**What the app does:** Before issuing `HE`, the app writes user-
selected strength / coffee_amount / water_amount / milk_amount /
temperature into a temporary-recipe register (`SendTemporaryRecipe`,
APK:5103). The machine consumes these for the upcoming brew, then
the values are discarded.
**Resolution:** `brew_task` now reads the temp-recipe slot at the
start of every brew via per-family field offsets that mirror
`brands/nivona.py:_STANDARD_RECIPE_LAYOUTS`. A heuristic scaling
factor is applied to `total_ms`:
- **strength** — each unit above zero adds 15 % to brew duration.
- **fluid volumes** — total of coffee + water + milk + milk_foam,
  scaled against an 80 ml reference, clamped to [0.5 ×, 4.0 ×].
- Combined as a multiplier on the category-default `total_ms`.
The override values are NOT cleared from NVS after the brew (real
machine consumes them; emulator leaves them stale). The app
re-writes them before every HE, so the stale-after-disconnect
window is invisible in normal use.
**Known limitation:** the heuristic doesn't model thermal ramps —
temperature overrides are read but ignored. Adding a thermal model
would require knowing the real machine's flow-rate-vs-temp curve.

### G4 — Per-family settings (HW IDs 101..119) are not pre-seeded

**Severity:** Low
**Location:** `nivona_store.c:seed_defaults` — seeds only the
filter-dependency id and the four maintenance-gauge percentages.
Other settings ids (language, clock, brightness…) are not seeded.
**What the app does:** Reads settings via HR on connect. The first
read after a flash erase returns 0 for every id (the emulator's
"no entry → 0" semantics).
**Symptom:** on first boot the app sees "language = 0 (default)",
"clock = 0", etc. The user immediately writes via HW, and from
then on round-trips are fine.
**Action:** optional — seed per-family setting defaults to make
first-boot UX nicer in the app. Low priority because it auto-
heals after the first edit.

### G5 — Alpha-ID layout for profile names is speculative

**Severity:** Low
**Location:** `nivona_store.c:seed_defaults` — seeds IDs
`0x0100..0x0103` with "Profile 1".."Profile 4".
**What the app does:** The exact alpha-ids Nivona uses for user-
defined profile names are not documented in our decompile. The
HA integration treats this as TBD too.
**Symptom:** if the app reads an alpha-id we didn't seed, it sees
an empty profile name. If the app writes one, the emulator persists
it and reads round-trip. So the worst case is a one-time "blank
names" display until the user renames.
**Action:** revisit when we have an APK packet trace of the profile
screen.

### G6 — Maintenance cycle trigger via HE is NACKed

**Severity:** Unknown (possibly Medium)
**Location:** `nivona_dispatch.c:handle_he` rejects any selector not
in the current family's `recipes` table, and `nivona_families.c`
populates `recipes` with brew selectors only (0..N).
**What the app does:** It is **speculative** that the app sends
`HE 40` (descale), `HE 41` (BU clean), `HE 42` (frother clean), etc.
The decompile of the relevant flow is obfuscated and we have no
APK packet trace of someone actually starting maintenance from the
app.
**Symptom (if app does send HE for maintenance):** emulator NACKs;
app would surface a "machine refused" error.
**Current alternative:** the emulator exposes `maint descale` /
`maint clean_bu` / `maint clean_frother` / `maint filter_change`
via the CLI for manual testing.
**Action:** when (if) we have an APK trace of a maintenance start
from the app, capture the actual HE selector for each cycle and add
to a `maintenance_selectors[]` per-family table. Until then the
gap exists.

## What is fully covered (no gaps)

- BLE discovery + advertising shape
- GATT service & all 6 characteristics with correct flag combos
- Handshake (HU) with full verifier + session-key derivation
- HX 8-byte status frame with the correct int16 layout
- Brew start (HE) with mode + flags validation, recipe selector
  lookup, ACK/NACK contract
- Brew cancel (HZ), prompt confirm (HY), reset-default (HD)
- All numerical (HR/HW) and alpha (HA/HB) read/write paths
- Per-family stat ID tables (recipe counters, totals, maintenance
  counters, gauges, warnings, filter dependency, via-app)
- Maintenance gauge degradation per brew with the universal
  600 / 610 / 620 / 640 ids and 601 / 611 / 621 / 641 warning flags
- All four maintenance cycle types (descale, BU clean, frother
  clean, filter change) via CLI — they pump HX progress notifications
  at the right cadence
- JustWorks pairing + bond persistence + random static address
- NVS-backed persistence of every register so values survive reboot

## How to use this document

- **Before reporting "emulator doesn't work like real machine"**:
  check this table first. If your reported symptom isn't in the gap
  list, the emulator should match — file an issue with the captured
  HX trace.
- **Before adding new emulator features**: confirm the wire-format
  against this table; if a row says ⚠ Gn, the matching G-number
  below tells you what's already known.
- **Before approving a release**: scan the gap list for any "Status:
  Medium" item still marked ⚠. None of the G1-G6 items block a
  release; G3 is the one that would most improve user experience
  if closed.
