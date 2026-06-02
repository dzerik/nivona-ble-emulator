# Emulator Changelog

**Independent from the HA integration's `manifest.json` version.**
The emulator under `esp_emulator/` evolves at its own cadence — bug
fixes, new brand-emulation coverage, and protocol-fidelity work happen
here without touching the integration's release cycle, and vice versa.
Emulator releases are tagged `emu-v<MAJOR>.<MINOR>.<PATCH>`.

## [0.12.0] — 2026-06-03 — Touchscreen front panel (Waveshare board)

Turns the **Waveshare ESP32-C6-Touch-LCD-1.47** into a coffee-machine
front panel: the 1.47″ 172×320 touchscreen shows live status and drives
the same actions as the CLI/BLE. Built on the multi-board foundation from
0.11.0 — the display stack compiles only for boards whose profile sets
`CONFIG_NIVONA_BOARD_HAS_DISPLAY=y`, so the headless boards are unchanged.

### Added

- **Display + touch bring-up** in `boards/waveshare_c6_lcd_1_47/board.c`:
  JD9853 LCD on SPI2 (with the 34-px column offset for the 172/240
  visible window) + LEDC PWM backlight, and the AXS5106L capacitive touch
  on I²C. Espressif's Apache-2.0 `esp_lcd_jd9853` and
  `esp_lcd_touch_axs5106` drivers are vendored under `components/`. New
  HAL accessors `board_lcd_panel()`, `board_lcd_panel_io()`,
  `board_touch()`, `board_set_backlight()`.
- **`nivona_ui` component** — LVGL v9 via `esp_lvgl_port` (20-line double
  draw buffer, no PSRAM) rendering a tile dashboard: live state tile
  (READY / BREWING + progress / prompt banner), Brew/Cancel/Confirm/
  Refill/Family tiles, a consumables strip, plus brew-recipe, refill and
  family sub-screens. A ~7 Hz timer polls `nivona_fsm_get_status` and the
  buttons call the same internal APIs as the CLI.

### Fixed

- **Brew now clears a pending soft prompt at start.** `brew_task` left the
  cold-start `FLUSH_REQUIRED` (HX `message=20`) set for the whole of the
  first brew after boot, so the brewing status carried a stale "flush
  required". It now sets `manipulation = NONE` when the brew begins (a
  machine actively brewing does not also ask for a flush);
  `nivona_maint_reevaluate()` at brew end re-surfaces any real prompt.

## [0.11.0] — 2026-06-02 — Multi-board build (board profiles + HAL)

### Added

- **Build-time board profiles** under `boards/<name>/`, selected with
  `idf.py -DBOARD=<name>` (default `xiao_c6`). Each profile carries its
  own `sdkconfig.board`, `partitions.csv`, and `board.c` implementing the
  new `board_hal` interface (`board_info()`, `board_early_init()`).
- **Three first-class boards:** `xiao_c6` (ESP32-C6, 4 MB — unchanged
  default), `xiao_s3` (ESP32-S3, 8 MB), `waveshare_c6_lcd_1_47`
  (ESP32-C6, 8 MB, headless for now — touchscreen UI in a later release).
- **CI matrix** builds all three profiles; a regression on any board
  fails the build.

### Changed

- `main.c` no longer contains board-specific RF-switch code; it calls
  `board_early_init()`. The XIAO-C6 RF-switch logic moved verbatim into
  `boards/xiao_c6/board.c`.
- Target / flash size / partition table moved out of `sdkconfig.defaults`
  into each board's `sdkconfig.board`. The generated `sdkconfig` now lives
  in the build directory, so switching boards (after `fullclean`)
  regenerates it cleanly with no stale target/flash/partition values.

## [0.10.0] — 2026-06-02 — Sync protocol with the HA integration's nivona package

Cross-checked the emulator's protocol surface against the HA
integration's `brands/nivona/` package and `protocol.py` (the
documented source of truth). The crypto core (RC4 master key, the
256-byte HU table, the 2-round verifier), the frame format, the
family process codes, the recipe/temp-recipe layouts and the
per-family stat-ID tables were already in lockstep. Three deltas
were found and closed.

### Added

- **NICR 8107 chilled-brew selectors (8/9/10).** `RECIPES_8000` now
  carries Chilled Espresso (8), Chilled Lungo (9) and Chilled
  Americano (10), mirroring `RECIPES_8000_CHILLED` in
  `brands/nivona/_family_8000.py`. The app lists these as separate
  recipes and sends them with the HE chilled flag byte
  (`payload[5] = 0x00`). Previously the emulator — which advertises
  the 8107 serial — NACKed any HE with selector ≥ 8, so chilled
  brews could not start at all. They now complete using the base
  category ramp (`STATS_8000.recipe_id_mask` already enabled
  8/9/10). This refines gap **G2** in `docs/FUNCTIONAL_COVERAGE.md`:
  the chilled *path* now works; the chilled *temperature ramp* is
  still unmodelled (no thermal model on the emulator).

### Changed

- **`fluid_scale` field corrected for 900-light / 1030 / 1040.** The
  integration declares `fluid_scale_factor=10` only on the NICR 9xx
  (`CAPABILITIES_900`); 900-light, 1030 and 1040 carry no ×10
  marker, and the actual HW write-path scaling
  (`RecipeFieldLayout.fluid_write_scale_10`) is currently `False`
  everywhere. The emulator's table had `fluid_scale = 10` on all
  four; reverted 900-light/1030/1040 to `1` so the (currently
  unused) marker matches the integration. NICR 9xx stays at 10.

### Docs

- Refreshed every `brands/nivona.py` reference (single module) to the
  current `brands/nivona/` package layout — in `nivona_families.c/.h`,
  `README.md`, `CLAUDE.md`, and `docs/FUNCTIONAL_COVERAGE.md`.
  Historical CHANGELOG entries are left untouched.

## [0.9.0] — 2026-05-21 — Honor per-brew overrides from the app

Closes finding **G3** from `docs/FUNCTIONAL_COVERAGE.md`. Before
this release, the Nivona Android app's per-cup customisation UI
(strength dial, ml sliders) had no visible effect on emulator
brews — the app writes the picks into the temp-recipe register
(HW 9001 + field offset) before HE, but `brew_task` used a fixed
ramp table driven only by the recipe category.

### Added

- **Per-family temp-recipe field layout.** New
  `nivona_recipe_layout_t` member on `nivona_family_t` carries the
  byte offsets the app uses for `strength`, `two_cups`,
  `coffee_amount`, `water_amount`, `milk_amount`, `milk_foam_amount`.
  Layouts mirror the HA integration's `_STANDARD_RECIPE_LAYOUTS`
  for each of the 8 known families. Temperature offsets exist on
  real hardware but the emulator has no thermal model so they
  aren't tracked.
- **`apply_override_scale` heuristic** in `nivona_brew.c`. Reads
  the temp-recipe slot at the start of every brew and, if at
  least one field is set, scales `total_ms`:
  - **strength** adds 15 % per unit above zero.
  - **fluid volume** (coffee + water + milk + foam) is linear
    against an 80 ml reference, clamped to [0.5 ×, 4.0 ×].
- Override values + before/after scaling are logged at INFO so
  testing against the real app is observable on the serial console.

### Notes

- The override values are NOT cleared from NVS after the brew. The
  real machine consumes them once and discards; the emulator leaves
  them stale. The app re-writes before every HE so this is invisible
  in normal use. Triggering a brew via CLI `brew <n>` after the app
  has written overrides will pick up those overrides — by design,
  useful for testing.
- No wire-format change. Only the brew ramp duration changes, and
  only when the app (or a CLI override) has populated reg 9001.

## [0.8.2] — 2026-05-21 — V3 pending items + reconstructed audit doc

Closes the three "accepted-for-later" items from 0.8.1 and lands a
written audit record so future sessions can verify coverage from
disk instead of context.

### Fixed

- **Session key no longer leaks via `/diag`** (I3). The HTTP
  diagnostic endpoint exposed `last_decrypt` — first 32 plaintext
  bytes of the most recent incoming frame, including the 2-byte
  session key prefix that authenticates every subsequent frame.
  Now skipped at capture time; the diagnostic still shows the cmd
  opcode and payload (the actually-useful debug content).
- **Telnet log no longer races into the wrong fd** (I2).
  `telnet_vprintf` snapshotted `s_client_fd` under the mutex but
  did the `send()` after releasing — lwIP could recycle the fd
  number between snapshot and send. Restructured so `send()`
  happens inside the mutex; paired with also holding the mutex
  across `close()` in `client_task` cleanup.
- **Fresh connection always requires fresh HU** (C3 defense-in-
  depth). `nivona_frame_reset` (called on disconnect) now clears
  `s_handshake_done` and zeroes `s_key_prefix`, so a new peer can
  never inherit the previous peer's session key. After tracing the
  reviewer's flagged race, the cross-task race itself was a false
  positive — every writer ends up on the NimBLE host task — but
  clearing on disconnect is the right thing regardless.

### Added

- `docs/AUDIT_V3.md` — reconstructed table of every V3 finding
  (Phases 1-5) plus Phase 6 follow-up and Phase 7 closure. Maps
  every ID to its fix commit so future reviewers can spot-check
  coverage without spelunking `git log`.

No wire-format / NVS layout change vs 0.8.1.

## [0.8.1] — 2026-05-21 — V3 follow-up patch

Independent code review post-0.8.0 (no audit context) flagged four
real high-confidence findings the V3 sweep missed — including one
regression introduced by 0.8.0 itself. Patched as Phase 6.

### Fixed

- **Wi-Fi reconnect no longer stalls the event loop** (regression
  from 0.8.0). The disconnect handler was calling `vTaskDelay` for
  up to 30 s on the system event-loop task, which dispatches *every*
  Wi-Fi/IP event including `IP_EVENT_STA_GOT_IP`. So a failed retry
  would mask the success of a subsequent successful retry until the
  delay elapsed. Replaced with a one-shot `esp_timer` so the
  back-off fires on the esp_timer task and the event loop stays
  responsive.
- **`nivona_frame_send` no longer races on a shared buffer.** The
  `static uint8_t frame[MAX_FRAME_BYTES]` was concurrently written
  by the NimBLE host task (ACK/NACK/dispatch), `brew_task` and
  `cycle_task` (push_status every 500 ms), and the CLI task. With
  overlap the buffer was torn → corrupt frames on the wire. Moved
  to a stack-allocated buffer; 512 B fits in every caller's ≥ 4 kB
  stack with margin.
- **Alpha load path no longer re-persists at boot.** Mirror of the
  num-store fix from 0.8.0 that was missed for the alpha store.
  `nvs_load_all` called the public `nivona_store_set_alpha` for
  every blob it read, opening `NVS_READWRITE` on the same namespace
  where it already held an `NVS_READONLY` iterator (UB per ESP-IDF
  NVS docs) and recommitting every value to flash. Added
  `set_alpha_mem` parallel to `set_num_mem`.
- **Batched NVS commits in `apply_wear_after_brew`.** Per-brew flash
  wear was 9× the necessary minimum because each `set_num` cycled
  its own open/set/commit/close. Added a small
  `nivona_store_batch_*` API that opens NVS once and commits once
  at the end. Memory state is still updated immediately so HX
  readers see the new values without waiting for commit.

### Known issues still pending

- `/diag` HTTP endpoint exposes `last_decrypt` (first 32 plaintext
  bytes of the most recent incoming frame, which once the handshake
  completes includes the 2-byte session key prefix). Acceptable for
  a dev-only emulator on a trusted LAN; will gate behind a compile
  flag in a future release.
- `nivona_telnet.c`: narrow TOCTOU window between `s_client_fd`
  snapshot and the `send()`. Worst case: one log line addressed to
  a freshly-recycled fd. Low impact.

No wire-format / NVS layout change vs 0.8.0.

## [0.8.0] — 2026-05-21 — Audit V3 sweep (concurrency, safety, clarity)

First post-split release. The emulator was extracted from
`melitta-ha-integration/esp_emulator/` into its own repo, and we used
the move as an opportunity to land a deep-review pass on the C
codebase. Closes **39 findings** from the V3 audit (8 Critical, 18
Important, 13 Minor) across five themed batches.

### Fixed — Phase 1: concurrency hygiene

- **All FSM modules now synchronise shared state.** `nivona_brew`,
  `nivona_maint_cycle`, `nivona_gatt`, `nivona_families` previously
  did unprotected check-then-act / read-modify-write on shared flags
  and pointers. On dual-core ESP32 a callback running on the NimBLE
  host task can preempt the app task between the check and the act —
  this manifests as a race on `s_conn_handle` / `s_active` /
  `s_current`. Each module now takes either a `SemaphoreHandle_t`
  mutex (for blocking sections — brew start, cycle start) or a
  `portMUX_TYPE` spinlock (for O(1) sections — GATT state, family
  restore).
- **Brew/cycle arg snapshot under the lock.** `s_arg` / `s_kind`
  are now copied into a task-local snapshot before the task releases
  the mutex, so a late writer can't corrupt the in-flight ramp.

### Fixed — Phase 2: memory safety

- **Frame-builder bounds check.** `nivona_frame_send` now refuses to
  start serialising if `payload_len + KEY_PREFIX_LEN + header/footer
  > MAX_FRAME_BYTES`, where previously it would silently truncate
  past the static buffer.
- **UB-free big-endian shifts.** `put_be16`/`put_be32` and friends
  cast each `uint8_t` to `uint32_t` before the shift to dodge the
  C11 6.5.7 left-shift-into-sign-bit UB.
- **BLE advertising lifecycle.** Always `ble_gap_adv_stop()` before
  re-starting advertising; previous code could double-start and
  return BLE_HS_EALREADY from inside the host task callback.
- **Telnet fd lifecycle.** Replace `close(fd)` from one task while
  another may be reading with `shutdown(SHUT_RDWR)`; fix
  `vsnprintf` truncation accounting so we don't drop the null
  terminator on long log lines.

### Fixed — Phase 3: domain correctness

- **Process-code collision resolved.** `MELITTA_PROC_FILTER_INSERT = 11`
  collided with the non-8000 Nivona brewing code (11). The cycle
  task now substitutes `MELITTA_PROC_BUSY = 99` for FILTER_INSERT
  so the HX stream never confuses a filter cycle for a brew.
- **Wear-tick consistency across family switches.** Maintenance gauge
  parity (BU clean every 2 brews, descale every 5) now reads
  `stats->total_id` when available and syncs to a local fallback
  counter, so a mid-session `family` switch doesn't reset parity to
  zero or pick up stale parity from the previous family.
- **Family-aware stats writes.** Every `nivona_store_set_num` for a
  cup/total/via-app counter is gated on the family's authoritative
  stat-ID table; we never persist HR ids the real machine doesn't
  expose for the selected family.

### Fixed — Phase 4: encapsulation and log hygiene

- `g_own_addr_type` → file-static with getter/setter.
- `NIVONA_RC4_KEY` → file-static, exposed via
  `nivona_rc4_with_master_key()` wrapper — both crypto call sites
  now route through it instead of touching the key bytes directly.
- Session-key logging downgraded `INFO → DEBUG` so a serial-attached
  developer doesn't see the live key in their console by default.
- Frame parser pre-handshake gate: unencrypted single-char ACK/NACK
  are only honoured before the handshake completes.
- Frame parser reentrancy guard: `try_parse` enters via a single
  `goto out:` epilogue so the static plaintext buffer can't be
  observed mid-decrypt.
- AD02 GATT char: removed `BLE_GATT_CHR_F_READ` (notify-only on
  the real machine; the stub READ was returning empty payloads).
- NVS load-path no longer re-persists the values it just read.
  `nivona_store_set_num` was split into a memory-only setter
  (`set_num_mem`, used by `nvs_load_all`) and the public persisting
  version, eliminating ~50 open/commit/close cycles + flash wear
  per cold start.
- WiFi reason-aware reconnect with exponential backoff (capped at
  30 s) and log throttling so a flapping AP can't fill the log
  buffer.

### Refactored — Phase 5: clarity

- New `has_powder_lid` field on `nivona_family_t` replaces the
  strcmp-chain in `nivona_maint_family_mask`. Adding a new family
  is one struct row.
- `apply_wear_after_brew` helper extracted from `brew_task` — the
  task body is now ramp-only; wear policy lives in one place.
- `OTA_HTTP_PORT` constant for the `cfg.server_port = 80` literal.
- `extern "C"` guards added to all remaining public headers.
- `be16_i` doc expanded to explain *why* it returns signed (HR/HW
  ids encode write-complements as negative on the wire).

### Notes for users coming from 0.7.x

- No protocol/wire-format change. Existing HA integrations and the
  Nivona Android app see byte-identical BLE traffic.
- NVS layout unchanged.
- Build: still ESP-IDF v5.x against ESP32-WROOM-32. Source files
  shuffled internally; `idf.py build` from the repo root just works.

## [0.7.0] — 2026-04-14 — Audit V2 fixes (decompile-grounded)

Closes **10 prioritised findings** from
`docs/NIVONA_EMULATOR_AUDIT_V2.md`, where every change is backed by
a file:line citation in the decompiled Nivona Android app
(`/home/dzerik/Development/esp-coffee-bridge/apk_study/decompiled/`).
The V1 audit had no access to this decompile — V2 resolves ~80 % of
previous "TBD" tags.

### Fixed (decompile-grounded)

- **HX Message byte is BE int16, not (info, manip) pair.** The
  Nivona Android app decodes bytes 4–5 as a single 16-bit Message
  (`EugsterMobileApp.Droid.decompiled.cs:28601-28623`). The
  emulator's `nivona_fsm_set_info` now logs a warning on non-zero
  `info` because it turns Message into an unrecognised value
  (neither 0/11/20 nor within the app's error range ≤6), silently
  disabling flush / error dialogs.
- **HY always ACKs.** Real app fire-and-forgets HY with 4 zero
  bytes and polls HX for status change
  (`EugsterMobileApp.decompiled.cs:6447-6451`,
  `Droid:26054-26097`). Emulator's previous NACK-on-hard-prompt
  contradicted the app model.
- **HD resets ONE setting by id** (previously silently ACKed
  without any state change). Real handler is
  `SetDefaultNumericValue(short id)` per `Droid:28692-28701`.
  New `nivona_store_erase_num(id)` called with the 2-byte BE id
  from payload.
- **Per-family stat tables.** New `nivona_stats.{h,c}` ported
  line-by-line from `StatisticsFactory.GetAvailableStatisticsFor*`
  (`EugsterMobileApp.decompiled.cs:9146-9306`) — five family groups
  with recipe-counter masks, cumulative-counter IDs, and
  "dependent setting" IDs. No two families agree: 600 and 700/79X
  have NO cumulative counters; 1000-family uses 216 for
  clean_coffee where 8000/900 use 214; 1000 uses 222 for descale
  where 8000/900 use 220.
- **Cup counter gates on family's stat map** — emulator no longer
  ticks `200+selector` when that selector has no counter on the
  current family (e.g. 79X selector 4 → no HR id; 600 selector 2
  or 5 → absent).
- **Cycle counters are family-resolved** — `resolve_stat_counter()`
  in `nivona_maint_cycle.c` picks the right HR id for descale /
  clean_coffee / rinse / filter_change per family. Writing 220 for
  descale on a 1000-family machine would have been wrong (real id
  is 222); writing any counter on 600/700/79X would have been
  wrong (they have no cumulative counters at all).
- **Filter dependent-setting ID seeded per family** — 642 for 8000,
  101 for 1000/900, 105 for 700/79X/600
  (`:9178, :9215, :9239, :9263, :9304`).
- **HE mode byte verified** — NACK on mismatch between
  `payload[1]` and the family's `brew_command_mode` (0x04 for
  8000, 0x0B for others; `:6463`). Also validates `flags` in
  {0x00 (ChilledBrew), 0x01 (normal)} with citation of
  `MakeStandardRecipeFallback` (`:6491-6526`).
- **ADV manufacturer-data tail bytes** marked TBD —
  `EFLibrary.CheckDiscovered` is control-flow-flattened with
  encrypted strings; exact values cannot be extracted from
  decompile. Confirmed only `customerId = 0xFFFF`
  (`Droid:28189, 28407`).

### Changed (housekeeping)

- **Melitta-derived `PROC_READY=2 / PROC_PRODUCT=4` enum removed.**
  Never used directly (families[] overrides at runtime) —
  misleading dead weight. Remaining cleaning-cycle codes renamed
  `MELITTA_PROC_*` to clearly signal they are not Nivona-verified.
  (`Droid:25934-25935` confirms 3/4/8/11 as authoritative.)

### New module

- `nivona_stats.{h,c}` — 205 LoC, ports five per-family
  `StatisticsFactory` tables plus helper `nivona_stats_has_recipe_counter`.

### Binary

ESP32-C6 clean build, 1.32 MB (+5.9 KB over `emu-v0.6.0`; still
~13 % partition headroom).

---

## [0.6.0] — 2026-04-14 — Phase E (maintenance cycles) + Phase B-lite (stat gauges)

### Added

- **`nivona_maint_cycle.{h,c}`** — long-running maintenance cycle
  runner. Eight cycle kinds (`descale` / `easy_clean` /
  `intensive_clean` / `filter_insert` / `filter_replace` /
  `filter_remove` / `evaporating` / `rinse`) each with their own
  duration, prep-prompt, and on-completion stat updates (counter
  tick + gauge reset to 100 % + warning flag clear).
- **CLI `maint <cycle>`** — start a cycle. Refuses if a brew is
  active or another cycle is running.
- **CLI `stats`** — dump all HR stat counters and gauges.
- **Gauge seeding** — `descale_%` / `brew_unit_clean_%` /
  `frother_clean_%` / `filter_%` seeded to 100 % on first boot
  (preserved across reboots after that).
- **Per-brew gauge degradation** (heuristic; see comments) —
  filter `-1 %`, brew_unit `-1 %/2 brews`, descale `-1 %/5 brews`.
  Warning flags auto-raise under thresholds (< 10 / < 20 / < 20 %).

### Explicitly NOT done

Cycle **process codes** (10 / 17 / 19 / 11 / 12 / 13 / 20 / 9) are
borrowed from the **Melitta** `MachineProcess` IntEnum — the
decompiled Nivona Android app's cleaning-code tables were not
resolved at the time of writing. **These values are TBD and
will likely change** once a real-hw BLE trace becomes available.
Marked as such in the header comment.

The HA integration's `NivonaProfile.parse_status` is **not**
extended to recognise these codes for Nivona — per our
"emulator mimics real machine, not integration" principle we
will not paper over the gap. Real Nivona may use completely
different cycle codes; we'd rather HA show `unknown` than pretend
Melitta codes are correct for Nivona.

Degradation rates, gauge seed values (100 %), and warning
thresholds are also heuristic — none are validated against a
real machine.

### Binary

ESP32-C6 clean build, 1.32 MB (+9 KB over `emu-v0.5.0`; still 13 %
partition headroom).

---

## [0.5.0] — 2026-04-14 — Persistence: state survives reboots + cup counters tick

A real machine remembers its water level and cup counters between power
cycles. The emulator now does too.

### Added

- **`niv_consum` NVS namespace** — `nivona_consumables` persists the
  full state (water / beans / tray / filter levels + brew_unit /
  trays / powder_lid part presence) on every change and restores on
  init. No persisted blob → factory defaults (all tanks full).
- **`niv_fam` NVS namespace** — `nivona_family_set` writes the last-
  selected family key; `nivona_family_current` lazily restores on
  first call. Default on fresh NVS: NIVO 8000. Boot after `family 700`
  now re-advertises as NICR 759 / emits process=8 codes without
  needing to repeat the CLI command.
- **Cup counter tick** — `brew_task` on successful completion
  increments `HR stat_id = 200 + selector` (per-recipe) and `213`
  (total_beverages) via the existing NVS-backed `nivona_store`. HA's
  Nivona stat sensors (declared in `brands/nivona.py::_STATS_*`)
  finally return non-zero values — `sensor.*_espresso`,
  `sensor.*_total_beverages` etc. tick up on every brew.
- **`factory_reset` CLI command** — wipes the four emulator-owned NVS
  namespaces (`niv_num`, `niv_alpha`, `niv_consum`, `niv_fam`) and
  reboots. Does NOT touch WiFi creds or NimBLE bonds — use `forget`
  for the latter.

### Binary

ESP32-C6 clean build, 1.32 MB (+1.6 KB over `emu-v0.4.0`; still 13 %
partition headroom).

---

## [0.4.0] — 2026-04-14 — Phase D: realistic consumables + maintenance FSM

Third slice of the Nivona full-emulation roadmap. The emulator now
simulates a real machine's wear: brewing consumes water and beans,
fills the drip tray, and eventually the corresponding prompts
(`FILL_WATER` / `EMPTY_TRAYS` / `BU_REMOVED`) appear in HA —
clearable only by refilling via CLI (or a real physical refill
once community testers have hardware).

End-to-end flow HA users now experience:

1. `brew 3` on a 900-family emulator → GRINDING → COFFEE → STEAM ramp.
2. After ~25 brews the water tank drops under 10 % — emulator raises
   `FILL_WATER` in the next HX.
3. HA's `binary_sensor.*_awaiting_confirmation` turns on; the user
   taps "Confirm Prompt" → emulator receives HY, NACKs (hard prompt).
4. Dev runs `fix water` on the serial CLI → tank back to 100 %,
   `nivona_maint_reevaluate` clears the prompt, next HX reads
   `manipulation = NONE`.
5. Brew resumes normally.

### Added

- **`nivona_consumables.{h,c}`** — simulated levels for `water`,
  `beans`, `tray`, `filter` (each 0–100 %) and three mechanical
  parts (`brew_unit`, `trays`, `powder_lid`). Thresholds in the
  header (e.g. `NIVONA_THR_WATER_LOW = 10`).
- **`nivona_maint.{h,c}`** — maintenance orchestrator. Re-evaluates
  consumables on every state change, picks the highest-priority
  manipulation, handles HY confirm (soft vs hard), exposes
  per-family allowlist (`has_milk_system` → `MOVE_CUP_TO_FROTHER`;
  1030/1040/8000 → `CLOSE_POWDER_LID` + `FILL_POWDER`).
- **Cold-start sequencer** — on boot, if no hard prompt, raises
  `FLUSH_REQUIRED` as a soft prompt (cleared by HA's first HY).
- **CLI commands**:
  - `tank <name> <pct>` — set consumable level.
  - `fix <name>` — refill / re-seat (`water`/`beans`/`tray`/
    `filter`/`all`/`brew_unit`/`trays`/`powder_lid`).
  - `part <name> <on|off>` — mechanical part present/absent.
  - `tanks` — dump all levels and parts.

### Changed

- **\[BUG FIX\]** `nivona_fsm.h` `nivona_manipulation_t` enum rewritten
  to match the canonical `Manipulation` IntEnum from
  `const.py:141`. Previously the emulator emitted `MANIP_WATER_EMPTY
  = 1`, which HA parses as `BU_REMOVED = 1` — silently wrong
  manipulation entities. Legacy CLI aliases (`trigger water_empty`
  → `FILL_WATER`) kept working.
- **`brew_task`** consumes resources per stage (GRINDING −3 % beans
  +2 % tray, COFFEE −3 % water, WATER −5 % water, STEAM −3 % water)
  and calls `nivona_maint_reevaluate` at the end so prompts surface
  in the final HX of the brew.
- **`nivona_brew_start`** refuses HE while a hard prompt is active
  (water empty / tray full / BU removed) — matches real-hardware
  behaviour and lets the Nivona app surface a proper error.
- **`handle_he`** NACKs when `nivona_brew_start` refuses — previously
  only NACKed on unknown selector.
- **`handle_hy`** routes through the maintenance orchestrator:
  soft prompts clear, hard prompts NACK until consumables fix
  the underlying condition.

### Requires

- HA integration **v0.46.0+** for brand-aware HX parsing.

### Binary

ESP32-C6 build clean, 1.31 MB (13% partition headroom). +~5 KB
over `emu-v0.3.0` for consumables + maintenance FSM.

---

## [0.3.0] — 2026-04-14 — Phase C-lite: per-family brew recipes + multi-stage ramp

Second slice of the Nivona full-emulation roadmap (see
[`../docs/NIVONA_RE_NOTES.md`](../docs/NIVONA_RE_NOTES.md) §Phase
C-lite). HE brew is now family- and recipe-aware: unknown selectors
are rejected with NACK, and milk-capable recipes walk a proper
GRINDING → COFFEE → STEAM sequence via `sub_process` transitions
instead of a single flat ramp.

### Added

- `nivona_families.{h,c}` gained full per-family recipe tables
  (RECIPES_600 / 700 / 79X / 900 / 1030 / 1040 / 8000), mirroring
  `custom_components/melitta_barista/brands/nivona.py::_RECIPES_*`
  so the C and Python sides stay in sync.
- `nivona_recipe_category_t` enum (ESPRESSO / COFFEE / AMERICANO /
  MILK_DRINK / MILK_ONLY / WATER) and per-category brew ramps
  (stage list + total wall-clock).
- `nivona_family_recipe_by_selector()` — lookup helper used by both
  brew start (resolve category) and HE handler (validate selector).

### Changed

- **`nivona_brew_start(selector)`** now resolves the selector against
  the active family's recipe table. Unknown selector → returns `false`,
  which causes the HE dispatcher to reply NACK. Previously any
  selector byte was silently accepted.
- **`brew_task`** walks recipe-category stages, setting `sub_process`
  per stage:
  - ESPRESSO / COFFEE / milk-less: `GRINDING → COFFEE`
  - AMERICANO: `GRINDING → COFFEE → WATER`
  - MILK_DRINK: `GRINDING → COFFEE → STEAM`
  - MILK_ONLY: `STEAM` only
  - WATER: `WATER` only

  Progress percentage (0–100) is linear across the whole brew.
  Durations are heuristic (espresso 20s, milk drink 45s, etc.) and
  await verification against a real-machine BLE trace.
- **Dispatch `handle_he`** replies NACK when `nivona_brew_start`
  refuses the request (unknown selector / brew in progress).

### Requires

- HA integration `v0.46.0+` (brand-aware HX parsing for `sub_process`
  transitions to render correctly on non-8000 families).

### Binary

ESP32-C6 build clean, +1.8 KB over emu-v0.2.0 (still 13% partition
headroom).

---

## [0.2.0] — 2026-04-14 — Phase A: per-family FSM process codes

First slice of the full Nivona emulation roadmap (see
[`../docs/NIVONA_RE_NOTES.md`](../docs/NIVONA_RE_NOTES.md)).

### Added

- `main/nivona_families.{h,c}` — canonical per-family lookup table
  covering 8 known Nivona families (600/700/79x/900/900-light/1030/
  1040/8000) with `ble_name`, `model`, `process_ready`,
  `process_brewing`, `fluid_scale`, and `has_milk_system` fields.
- `nivona_fsm_reset_to_ready()` — retargets the FSM to the active
  family's READY code without requiring a reboot.

### Changed

- `nivona_fsm_init` reads from the active family entry instead of
  hardcoding `process = 3`.
- `nivona_brew_task` snapshots the current family at brew start and
  uses its `{process_brewing, process_ready}` codes for the ramp.
- CLI `family <key>` command now also calls `nivona_family_set` +
  `nivona_fsm_reset_to_ready` — previously only ADV / DIS were
  updated, FSM codes stayed locked to NIVO 8000 regardless of
  selected family.

### Fixed

- Emulator was functionally broken for any family other than `8000`.
  After `family 700` the ADV identified NICR 759 but HX kept
  returning `process = 3` (NIVO 8000 READY). With HA v0.46.0+
  brand-aware parsing expecting `8` for non-8000 Nivona families,
  status sensors would render as "unknown".

---

## [0.1.0] — 2026-04-13 — Initial emulator (bundled via HA integration v0.44.0)

First working Nivona BLE emulator:

- Byte-exact advertisement matching a real machine (company ID
  `0x0319`, Eugster customer payload, AD00 service, DIS in SR).
- GATT: AD00 (AD01 write, AD02 notify) + DIS (0x180A) with
  manufacturer/model/serial.
- Full Eugster/EFLibrary encrypted protocol: frame parser, RC4 stream
  cipher, AES customer-key bootstrap, HU handshake with per-brand
  verifier, HR/HW/HX/HE/HA/HB opcodes.
- HX FSM (hardcoded NIVO 8000 codes at this stage), HE brew ramp,
  unsolicited HX notifications during brew.
- CLI-driven family switch (ADV/DIS only — FSM codes fixed until
  0.2.0).
- WiFi CLI + telnet + OTA.
- Targets: ESP32-C6 (primary) and ESP32-S3.

Shipped alongside HA integration releases 0.44.0 → 0.48.1 as an
in-tree asset; from 0.2.0 onwards the emulator has its own tag series.
