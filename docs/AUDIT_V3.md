# Nivona BLE Emulator — Audit V3 Report

**Audit window:** 2026-05-19 / 2026-05-21
**Releases:** emu-v0.8.0 (Phases 1-5) → emu-v0.8.1 (Phase 6) → emu-v0.8.2 (Phase 7)
**Status:** all findings closed or explicitly accepted as known-issue.

This is the recovered written record of the V3 audit. The original
review was conversational; this document was reconstructed from the
fix commits and is the authoritative source for cross-checking that
nothing slipped through.

The numbering uses the original V3 phase prefix (`C` = Critical,
`I` = Important, `M` = Minor). Phase 6 follow-up items kept their
reviewer-assigned letters (`F1-F4`). Phase 7 closed `I2`, `I3` and
the `C3` false-positive.

---

## Phase 1 — Concurrency hygiene (commit `f3a34c9`)

Every FSM module shared mutable state across the NimBLE host task,
brew/cycle workers and the CLI task without synchronisation.

| ID | What | Where | Resolution |
|----|------|-------|------------|
| C1 | `s_active` / `s_arg` race in brew start | `nivona_brew.c` | `SemaphoreHandle_t s_lock`; check-and-set under one mutex |
| C2 | `s_active` / `s_kind` race in cycle start | `nivona_maint_cycle.c` | Same pattern as brew |
| C3 | `s_conn_handle` / `s_notify_subscribed` flipped between host task and senders | `nivona_gatt.c` | `portMUX_TYPE s_state_mux` spinlock around the read/write pair |
| C4 | `s_current` / `s_restored` non-atomic lazy init | `nivona_families.c` | `portMUX_TYPE s_restore_mux` with fast-path then double-check |
| I1 | brew/cycle arg snapshot read by worker after release | both | `arg_snapshot` copied while still holding the lock |

## Phase 2 — Memory safety (commit `9f86a23`)

| ID | What | Where | Resolution |
|----|------|-------|------------|
| C5 | Frame builder could overrun the static buffer on long payloads | `nivona_frame.c:nivona_frame_send` | Bounds check on `1 + cmd_len + kp_len + payload_len + 2` before any write |
| C6 | `uint8_t << 24` is UB by C11 6.5.7 if `int` is 16-bit; raises sign bit even at 32-bit | `nivona_dispatch.c` `be16_i` / `put_be32` | Cast each byte to `uint32_t` before the shift |
| I2 | `ble_gap_adv_start` could fail with `BLE_HS_EALREADY` after reconnect | `nivona_ble.c` | Always `ble_gap_adv_stop` first |
| I3 | Telnet `close(fd)` could race with another task's `recv()`; lwIP could recycle the fd | `nivona_telnet.c` | `shutdown(fd, SHUT_RDWR)` signals EOF; old `client_task` closes |
| I4 | `vsnprintf` truncation accounting dropped the NUL byte | `nivona_telnet.c` | Clamp `n` to `sizeof(buf)-1` |

## Phase 3 — Domain correctness (commit `7d87379`)

| ID | What | Where | Resolution |
|----|------|-------|------------|
| C7 | `MELITTA_PROC_FILTER_INSERT = 11` collided with non-8000 brewing code | `nivona_maint_cycle.c` | Substitute `MELITTA_PROC_BUSY = 99` for FILTER_INSERT on cycle write |
| I5 | Wear-tick parity inconsistent after CLI `family <key>` switch | `nivona_brew.c` | Use `stats->total_id` when available, sync local fallback counter mid-session |
| I6 | Cup counters wrote HR IDs the real machine doesn't expose on that family | `nivona_brew.c` | Gate every store write on `nivona_stats_has_recipe_counter` / `total_id != 0` |
| I7 | Family default resolved by array index `[7]`; reordering silently shifts default | `nivona_families.c` | `nivona_family_find("8000")` resolves by name |

## Phase 4 — Encapsulation & log hygiene (commit `4c2a9db`)

| ID | What | Where | Resolution |
|----|------|-------|------------|
| I8 | `g_own_addr_type` extern global | `nivona_ble.c/h` | File-static with getter/setter |
| I9 | `NIVONA_RC4_KEY` extern global; both crypto paths touched key bytes directly | `nivona_crypto.c/h`, `nivona_frame.c` | File-static; new `nivona_rc4_with_master_key()` wrapper |
| I10 | Session-key bytes logged at `INFO` (visible on serial console by default) | `nivona_dispatch.c` | Downgraded to `DEBUG` |
| I11 | Unencrypted `A` / `N` accepted even after handshake (forgery surface) | `nivona_frame.c` | Pre-handshake gate (`if (!s_handshake_done) …`) |
| I12 | `try_parse` reentrant; static `plain[]` exposed mid-decrypt | `nivona_frame.c` | `s_in_parse` guard with single `goto out:` epilogue |
| I13 | AD02 GATT char had `BLE_GATT_CHR_F_READ`; real machine doesn't allow READ | `nivona_gatt.c` | Removed READ flag (notify-only) |
| I14 | `nvs_load_all` re-persisted every numerical value at boot (~50 commit cycles + flash wear) | `nivona_store.c` | Split `nivona_store_set_num` into memory-only `set_num_mem` + persisting public API; load path uses memory-only |
| I15 | `WIFI_EVENT_STA_DISCONNECTED` re-connected immediately; flapping AP filled the log buffer | `nivona_wifi.c` | Reason-aware reconnect with exponential back-off (capped 30 s) + log throttle |
| I16 | Dead `nvs_flash.h` include in wifi | `nivona_wifi.c` | Removed |

## Phase 5 — Clarity refactor (commit `403874c`)

| ID | What | Where | Resolution |
|----|------|-------|------------|
| M1 | Decompiler attribution in file header | `nivona_families.h` | Removed |
| M6 | Missing `extern "C"` guards on public headers | 11 headers | Added |
| M7 | `strcmp` chain on family key to detect powder-lid presence | `nivona_maint.c` | Added `has_powder_lid` field to `nivona_family_t`, table-driven |
| M8 | `brew_task` held 70 lines of wear / counter side-effects inline | `nivona_brew.c` | Extracted into `apply_wear_after_brew` helper |
| M9 | Magic literal `cfg.server_port = 80` | `nivona_ota.c` | `#define OTA_HTTP_PORT 80` |
| M13 | `be16_i` doc didn't explain why the helper is signed | `nivona_dispatch.c` | Expanded doc — HR/HW IDs encode write complements as negative on the wire |

---

## Phase 6 — Independent review follow-up (commit `166888a`, emu-v0.8.1)

After 0.8.0 shipped, a fresh code-reviewer pass without audit
context flagged four real high-confidence findings the V3 sweep
missed — including one regression Phase 4 itself introduced.

| ID | What | Where | Resolution |
|----|------|-------|------------|
| F1 | (Regression of I15) `vTaskDelay` inside the Wi-Fi event handler — stalls the system event loop for up to 30 s | `nivona_wifi.c` | One-shot `esp_timer` so back-off fires on the esp_timer task |
| F2 | (Missed parallel to I12) `nivona_frame_send` used `static uint8_t frame[]` shared across NimBLE host task + brew/cycle/CLI | `nivona_frame.c` | Stack-allocated; 512 B fits in every caller's ≥ 4 kB stack |
| F3 | (Missed parallel to I14) Alpha load path re-persisted at boot + RW/RO handle conflict on same namespace | `nivona_store.c` | Added `set_alpha_mem`; forward-declared both `set_*_mem` helpers |
| F4 | `apply_wear_after_brew` did 9 `set_num` cycles → 9 flash erase+write per brew | `nivona_brew.c`, `nivona_store.c/h` | Added `nivona_store_batch_{begin,set_num,end}` API; brew uses it |

---

## Phase 7 — Pending items closed (commit `d89abbe`, emu-v0.8.2)

| ID | What | Where | Resolution |
|----|------|-------|------------|
| I2 (P7) | `telnet_vprintf` send() raced with `client_task` close() — lwIP could recycle fd between snapshot and send | `nivona_telnet.c` | Format outside mutex, send inside; pair `close()` with the s_client_fd reset under the same mutex |
| I3 (P7) | `/diag` HTTP exposed `last_decrypt` whose first 2 bytes are the session-key prefix (auth token for every subsequent frame) | `nivona_frame.c` | Skip prefix when capturing — the diagnostic still shows cmd + payload |
| C3 (P7) | Reviewer flagged parser-state globals as raced | `nivona_frame.c` | After tracing: false positive (every writer ends up on the NimBLE host task). Defense in depth: `nivona_frame_reset` now also clears `s_handshake_done` / `s_key_prefix` so a fresh connection requires a fresh HU |

---

## Known issues — accepted, not fixed

- **`s_local_wear_tick` not persisted across reboots** when the
  family lacks a `total_id` (600 family only). The local counter
  resets to 0 on boot, so parity-and-modulo gauges (`BU clean`,
  `descale`) effectively reset their phase. Acceptable for an
  emulator; the real machine derives wear from a firmware-internal
  counter we don't have access to.
- **Brew/cycle "one batch at a time" contract**. `nivona_store_batch_*`
  uses a single static slot. The contract is enforced by the
  caller-side mutexes (`brew` and `cycle` never overlap), but a
  future caller would have to re-evaluate. Not enforced by the
  API itself.
- **`nivona_frame_reset` re-clears `s_handshake_done` on disconnect** —
  this is defense in depth but does mean the existing HU dispatcher
  must re-run from scratch on every reconnect. Documented in code.

---

## How to verify in a future session

If a future reviewer wants to spot-check this audit:

1. `git log --grep="audit-V3\|Phase [1-7]"` lists every fix commit.
2. Each commit message names the IDs it closes.
3. Cross-check this file's table against the commit list — every
   row should map to exactly one commit.
4. To spot *new* gaps, run an independent code-reviewer pass on
   `main/*.c` and `main/*.h`; flag anything not already on this
   table. Phase 6 was found exactly this way.
