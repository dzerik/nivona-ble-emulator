// Nivona family-specific tuning table.
//
// Phase A of the Nivona-emulation roadmap (see
// docs/NIVONA_RE_NOTES.md). Centralises per-family values that the FSM
// and the brew task need in order to emulate different Nivona machines
// convincingly enough for both Home Assistant and the official Nivona
// Android app to work against whichever family the CLI `family <key>`
// command has selected.

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sub-process codes emitted during brew ramp. Must match SubProcess
// enum in the HA integration (const.py:113) and the values the real
// Nivona firmware reports in the HX sub_process field during brew.
typedef enum {
    NIVONA_SUB_IDLE     = 0,
    NIVONA_SUB_GRINDING = 1,
    NIVONA_SUB_COFFEE   = 2,
    NIVONA_SUB_STEAM    = 3,
    NIVONA_SUB_WATER    = 4,
    NIVONA_SUB_PREPARE  = 5,
} nivona_sub_process_t;

// Recipe category → brew ramp shape. Each stage executes in sequence,
// each gets an even share of the total brew time.
typedef enum {
    NIVONA_CAT_ESPRESSO,   // GRINDING → COFFEE                 (short)
    NIVONA_CAT_COFFEE,     // GRINDING → COFFEE                 (medium)
    NIVONA_CAT_AMERICANO,  // GRINDING → COFFEE → WATER
    NIVONA_CAT_MILK_DRINK, // GRINDING → COFFEE → STEAM         (cappuccino, latte, …)
    NIVONA_CAT_MILK_ONLY,  // STEAM                             (hot milk / foam)
    NIVONA_CAT_WATER,      // WATER                             (hot water)
    NIVONA_CAT_UNKNOWN,    // Unknown selector — reject with NACK
} nivona_recipe_category_t;

typedef struct {
    uint8_t                  selector;   // HE payload byte[3]
    const char              *name;       // Display
    nivona_recipe_category_t category;
} nivona_recipe_t;

// Per-family field offsets inside the temp-recipe register block.
//
// The real machine exposes a single HW register (TEMP_RECIPE_REG, 9001)
// that the app writes BEFORE issuing HE — per-brew strength / volumes /
// temperatures. Different model families have slightly different field
// layouts; the offsets here mirror the HA integration's
// `_STANDARD_RECIPE_LAYOUTS` (brands/nivona/, assembled in __init__.py
// from each _family_*.py's RecipeFieldLayout). A value of 0 means
// "field not exposed by this family" — read via `temp_recipe_offset()`
// which returns -1 in that case.
//
// We track only the fields the emulator actually uses for brew-ramp
// scaling: strength, two_cups, and the four fluid volumes. The
// temperature offsets exist on real hardware but the emulator has no
// thermal model, so they're not stored here.
typedef struct {
    int8_t strength;            // -1 = field absent
    int8_t two_cups;
    int8_t coffee_amount;
    int8_t water_amount;
    int8_t milk_amount;         // -1 on 600 (no milk system)
    int8_t milk_foam_amount;
} nivona_recipe_layout_t;

typedef struct {
    const char *key;            // "600" / "700" / "79x" / … / "8000"
    const char *ble_name;       // Advertised local_name — bare serial
                                // with 5 trailing dashes. App takes
                                // Substring(0, 4) as model code.
    const char *model;          // Display model name (e.g. "NIVO 8107")

    // Phase A — HX FSM process codes
    int16_t process_ready;      // NIVO 8000 = 3, others = 8
    int16_t process_brewing;    // NIVO 8000 = 4, others = 11

    // Phase C-lite — brew payload scaling (populated but not yet
    // consumed; HW override handler will read it in a later slice).
    uint8_t fluid_scale;        // ml ×N marker. Mirrors the surviving
                                // fluid_scale_factor=10 on NICR 9xx
                                // (brands/nivona/_family_900.py:
                                // CAPABILITIES_900); every other family
                                // is 1. NOTE: the integration's actual
                                // HW write-path scaling (RecipeFieldLayout
                                // .fluid_write_scale_10) is currently
                                // False everywhere — reverted pending a
                                // live trace — so this marker is unused
                                // by the emulator too.
    uint8_t has_milk_system;    // 900/1030/1040/8000 = 1, others = 0
    uint8_t has_powder_lid;     // 1030/1040/8000 = 1, others = 0
                                // (machines with a ground-coffee chute
                                //  for decaf shots — affects which
                                //  MANIP_* prompts the family can raise)

    // HE payload byte[1] expected "brew command mode" —
    // EugsterMobileApp.decompiled.cs:6463. Real machine rejects
    // HE with wrong mode.
    //   NIVO 8000 → 0x04
    //   all other → 0x0B (= 11; note this shares the brewing Process
    //                     code on non-8000 families by coincidence)
    uint8_t brew_command_mode;

    // Phase C-lite — per-family recipe table (selector → category).
    // NULL-terminated semantics: iterate up to recipe_count.
    const nivona_recipe_t *recipes;
    size_t                 recipe_count;

    // Temp-recipe (HW 9001 + offset) field layout — see comment on
    // `nivona_recipe_layout_t`. Driven by `nivona_brew_task` to scale
    // ramps according to the app's per-brew overrides.
    nivona_recipe_layout_t recipe_layout;
} nivona_family_t;

// Convenience: HW register id for a temp-recipe field on the active
// family. Returns -1 if the family doesn't expose the requested field.
// `offset` is one of nivona_recipe_layout_t's members.
#define NIVONA_TEMP_RECIPE_BASE 9001
static inline int16_t nivona_temp_recipe_reg(int8_t offset) {
    return (offset < 0) ? -1
                        : (int16_t)(NIVONA_TEMP_RECIPE_BASE + offset);
}

// All known Nivona families. Size via NIVONA_FAMILIES_COUNT.
extern const nivona_family_t NIVONA_FAMILIES[];
extern const size_t NIVONA_FAMILIES_COUNT;

// Returns the currently active family. Defaults to the "8000" entry
// at boot. Never NULL.
const nivona_family_t *nivona_family_current(void);

// Switches the active family by key. Returns 0 on success, non-zero
// if the key is unknown. Caller is responsible for pushing the new
// ble_name / DIS values; this call just updates the table pointer so
// the FSM and brew task read the right codes.
int nivona_family_set(const char *key);

// Convenience lookup (read-only). Returns NULL on miss.
const nivona_family_t *nivona_family_find(const char *key);

// Resolve an HE-selector byte to a recipe in the current family.
// Returns NULL if the selector is not known for this family — the
// HE handler should NACK in that case.
const nivona_recipe_t *nivona_family_recipe_by_selector(
    const nivona_family_t *fam, uint8_t selector);

#ifdef __cplusplus
}
#endif
