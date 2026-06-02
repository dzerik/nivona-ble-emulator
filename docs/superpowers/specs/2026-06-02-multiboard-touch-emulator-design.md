# Design — Multi-board support + touchscreen UI for the Nivona emulator

**Date:** 2026-06-02
**Status:** approved (brainstorming) → ready for implementation plan
**Scope:** one feature — introduce a board-abstraction layer so the firmware
builds first-class for three boards, and add a touchscreen front-panel UI on the
Waveshare board, without changing the BLE wire protocol or regressing the
headless boards.

## Goal

Run the existing Nivona BLE emulator on a **Waveshare ESP32-C6-Touch-LCD-1.47**
and use its 172×320 touchscreen as a "front panel" (status + control), while
keeping **XIAO ESP32-C6** and **XIAO ESP32-S3** building and working with no
manual edits.

Today the project is mono-board: a single `sdkconfig.defaults` targeting
`esp32c6`, and the only board-specific code is `xiao_c6_rf_init()` (GPIO3/14 RF
switch) called unconditionally in `app_main`. There is no board abstraction.

## Decisions (locked during brainstorming)

| Question | Decision |
|---|---|
| Touchscreen role | **Status + control** — front panel that shows state and exposes the same actions the CLI has (brew, cancel, confirm prompt, refill, switch family). |
| First-class boards | **XIAO C6**, **XIAO S3**, **Waveshare C6-Touch-LCD-1.47** — all build without manual edits. |
| Build mechanism | **Approach A** — build-time board profile (`-DBOARD=<name>`) + Board HAL + conditional UI compilation. |
| Display stack | **LVGL widgets + our own thin esp_lcd/esp_lcd_touch bring-up** behind a board hook (no BSP lock-in). |
| Screen layout | **Variant C** — tile dashboard + sub-screens (5-screen flow below). |
| Family switch from UI | Takes effect **after reboot** (matches current emulator behavior; writes NVS via `nivona_family_set`). |
| Refill from UI | **Sub-screen** with separate items: water / beans / empty tray. |

## Section 1 — Board abstraction (HAL + build profiles)

### Directory layout (new `boards/`, partition tables move out of repo root)

```
boards/
  board.h                              # common board_hal interface
  xiao_c6/                { board.c, sdkconfig.board, partitions.csv }   # esp32c6, 4MB, RF switch GPIO3/14
  xiao_s3/                { board.c, sdkconfig.board, partitions.csv }   # esp32s3, PSRAM, no display
  waveshare_c6_lcd_1_47/  { board.c, sdkconfig.board, partitions.csv }   # esp32c6, 8MB, JD9853 + CST816
```

The existing root `partitions.csv` becomes `boards/xiao_c6/partitions.csv`
unchanged (4 MB OTA layout). Waveshare gets an 8 MB layout.

### `board.h` — the only thing `main.c` knows about hardware

```c
typedef struct { const char *name; bool has_display; } board_info_t;

const board_info_t *board_info(void);
void board_early_init(void);          // RF switch OR display bring-up; called first in app_main

#if CONFIG_NIVONA_BOARD_HAS_DISPLAY
esp_lcd_panel_handle_t board_lcd_panel(void);
esp_lcd_touch_handle_t board_touch(void);
void board_set_backlight(uint8_t pct);
#endif
```

`main.c` loses `xiao_c6_rf_init()`; it calls `board_early_init()` instead. The
XIAO-C6 RF-switch logic moves verbatim into `boards/xiao_c6/board.c`.

### Build wiring

- **Root `CMakeLists.txt`**: read cache var `BOARD` (**default `xiao_c6`** → exact
  backward compatibility); set
  `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/${BOARD}/sdkconfig.board"`; set
  the partition CSV from `boards/${BOARD}/partitions.csv`; propagate `BOARD` down.
- **`main/CMakeLists.txt`**: add `boards/${BOARD}/board.c` to SRCS; conditionally
  append `nivona_ui` to `REQUIRES` only when the board has a display.
- **Root `sdkconfig.defaults`**: keeps the common config (BLE / WiFi / console /
  FreeRTOS). Per-board `sdkconfig.board` sets `CONFIG_IDF_TARGET`, flash size,
  PSRAM, and `CONFIG_NIVONA_BOARD_HAS_DISPLAY`.

### Build commands (documented in README/CONTRIBUTING)

```
idf.py -DBOARD=xiao_c6 build                 # default — identical to today
idf.py -DBOARD=waveshare_c6_lcd_1_47 build
idf.py -DBOARD=xiao_s3 build
# switching board (target/flash changes): idf.py -DBOARD=<new> fullclean
```

### Invariant guarantee

Boards without a display compile **zero** UI/LVGL/esp_lcd code — it is excluded
at the CMake level (conditional `REQUIRES`), not via `#ifdef` inside shared
files. The `xiao_c6` default reproduces the current build 1:1.

## Section 2 — UI subsystem (`nivona_ui`)

### Placement & conditional build

A separate IDF component `components/nivona_ui/`, added to the build **only** when
`CONFIG_NIVONA_BOARD_HAS_DISPLAY=y` (conditional `REQUIRES` in `main`). Its
managed deps (LVGL, CST816 touch, JD9853 panel) are therefore pulled only for
the Waveshare build; headless boards never see them.

### Threading model (LVGL is not thread-safe)

- One dedicated `nivona_ui` task runs the `lv_timer_handler()` loop; **all** LVGL
  calls happen only on that task, at a priority **below** the NimBLE host task.
- No external module calls into the UI. Instead of callbacks, the UI **polls**: an
  `lv_timer` at ~5–10 Hz reads the thread-safe `nivona_fsm_get_status()` snapshot
  and redraws. Simple and race-free.

### Data flow

```
UI ← (poll 5–10 Hz) ← state                 UI → (from LVGL task) → existing CLI-level actions
  · nivona_fsm_get_status()                    · nivona_brew_start() / nivona_brew_cancel()
  · nivona_family_current()                    · nivona_maint_handle_confirm()        (= HY)
  · nivona_frame_handshake_complete()          · nivona_consumables_* (refill water/beans/tray)
  · nivona_consumables_get() / store counters  · nivona_family_set()                  (writes NVS)
  · [new] nivona_ble_is_connected()
```

The UI is **another frontend to existing actions** — the same functions
`nivona_cli` already calls. No new business logic. Those functions already take
their own locks (designed for the CLI task), so calling them from the LVGL task
is safe.

### Minimal additions to existing modules (read-only)

- `nivona_ble_is_connected()` — connection-state getter (add if absent).
- Small read-only getters for consumables/counters if the current API is
  insufficient for display.

No changes to BLE / brew / FSM logic — only new read-only accessors.

### Lifecycle

In `app_main`, when `board_info()->has_display`, after the rest of init, call
`nivona_ui_start()`, which takes `board_lcd_panel()` / `board_touch()` from the
HAL, initializes LVGL, and starts the UI task. On headless boards this call is
not in the build.

## Section 3 — Screen layout & navigation (Variant C)

Tile dashboard with sub-screens, 172×320 portrait. Five screens:

1. **Home (idle)** — header (model + BLE state); full-width state tile
   (`READY` / family); action tiles `BREW` (enabled), `CANCEL`/`CONFIRM`
   (dimmed when nothing to act on), `REFILL`, full-width `FAMILY · <model> ›`;
   bottom strip with consumables + total counter.
2. **Brew picker** (BREW →) — scrollable list of the current family's recipes;
   on the 8107 this includes the chilled drinks (selectors 8/9/10). Tap →
   `nivona_brew_start(selector)` → back to Home.
3. **Brewing** — state tile shows `BREWING · NN%` + sub-process label + progress
   bar; only `CANCEL` active; `BREW`/`FAMILY` disabled. Updated via HX poll.
4. **Active prompt** — state tile turns into a red warning banner (e.g.
   `⚠ FILL WATER`); `BREW` blocked; `CONFIRM` (= `nivona_maint_handle_confirm`)
   and `REFILL` highlighted.
5. **Family picker** (FAMILY →) — list of the 8 families, current one marked;
   selection calls `nivona_family_set` (NVS), with an honest
   "applies after reboot" note.

**Refill** is its own sub-screen (water / beans / empty tray) per the locked
decision.

## Section 4 — Build, CI, testing

### CI matrix (mechanical invariant guarantee)

Extend `.github/workflows/build.yml` to build three profiles in parallel:

| BOARD | target | guards |
|---|---|---|
| `xiao_c6` (default) | esp32c6 | current headless build not broken |
| `xiao_s3` | esp32s3 | second chip/target alive |
| `waveshare_c6_lcd_1_47` | esp32c6 | display + UI build compiles |

A regression on any board fails CI — that is the enforcement of "don't break
the other boards."

### Dependencies

Declared in `components/nivona_ui/idf_component.yml`, pulled only when the
component is in the graph: `lvgl/lvgl`, `espressif/esp_lcd_touch_cst816s`, a
JD9853 panel driver (component or custom init). Root `idf_component.yml` (mdns)
unchanged.

### Testing

- `tests/test_emulator.py` (BLE/HTTP) stays valid on all boards — the UI does not
  change the wire protocol; it brews through the same APIs.
- UI is not host-unit-testable (LVGL + hardware); verified manually on the
  Waveshare board, with CI providing compile-level safety (boot + render smoke).

### Docs / version

README + CONTRIBUTING gain the board matrix and per-board commands. Bump
`VERSION` (minor) + CHANGELOG. `FUNCTIONAL_COVERAGE.md` unchanged (protocol
identical).

## Risks

1. **RAM on ESP32-C6** — no PSRAM, ~512 KB SRAM already shared with BLE+WiFi. A
   full 172×320×2 ≈ 110 KB framebuffer is not viable; use **partial LVGL
   draw-buffers** (a few lines, double-buffered). Primary technical risk.
2. **JD9853 + CST816 drivers and pin map** — need the exact Waveshare pinout
   (from its demo/schematic); JD9853 may need a community component or a custom
   panel init. Research item before implementation.
3. **Waveshare RF** — PCB antenna, no RF switch, so its `board_early_init` is
   display bring-up with **no** GPIO3/14 (that is XIAO-C6-specific).
4. **LVGL task vs BLE/WiFi coexistence** — task priority and CPU load (HX
   brew-pushes + redraw); mitigated by a modest UI rate (5–10 Hz) and a priority
   below the host task.

## Out of scope (YAGNI)

- Chilled-brew thermal ramp modelling (separate `FUNCTIONAL_COVERAGE` G2 item).
- Instant (no-reboot) family switch — would require re-init of BLE name / DIS.
- Touch UI for XIAO boards (they have no display).
- Any change to the BLE protocol, crypto, or family/stat tables.
- LVGL host-side automated tests.

## Open research items (resolve at start of implementation)

- Exact Waveshare ESP32-C6-Touch-LCD-1.47 pin map (LCD SPI pins, backlight, touch
  I²C, reset) and the correct JD9853 init sequence.
- Whether a maintained JD9853 esp_lcd component exists or we vendor a small init.
- LVGL draw-buffer sizing that fits the C6 RAM budget alongside BLE+WiFi.
