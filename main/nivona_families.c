#include "nivona_families.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "nivona_fam";

#define NS_FAM "niv_fam"
#define KEY_FAM_CUR "current"

// Per-family table — single source of truth for the emulator.
//
// Process codes (process_ready / process_brewing) come from the
// decompiled Android app's MakeCoffee() switch:
//   NIVO 8000     → 3 / 4
//   All others    → 8 / 11
// See docs/NIVONA_RE_NOTES.md §Phase A.
//
// Recipe tables mirror custom_components/melitta_barista/brands/nivona/
// (the Python source of truth for HA-side, split per-family into
// _family_<key>.py). Selectors are the HE payload byte[3] values the
// app sends. Categories drive the brew ramp shape in nivona_brew_task
// (Phase C-lite).

// ---- Per-family recipe tables (see brands/nivona/_family_*.py) ----

static const nivona_recipe_t RECIPES_600[] = {
    { 0, "Espresso",    NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",      NIVONA_CAT_COFFEE     },
    { 2, "Americano",   NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",  NIVONA_CAT_MILK_DRINK },
    { 4, "Frothy Milk", NIVONA_CAT_MILK_ONLY  },
    { 5, "Hot Water",   NIVONA_CAT_WATER      },
};

static const nivona_recipe_t RECIPES_700[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Cream",           NIVONA_CAT_COFFEE     },
    { 2, "Lungo",           NIVONA_CAT_COFFEE     },
    { 3, "Americano",       NIVONA_CAT_AMERICANO  },
    { 4, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Milk",            NIVONA_CAT_MILK_ONLY  },
    { 7, "Hot Water",       NIVONA_CAT_WATER      },
};

static const nivona_recipe_t RECIPES_79X[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",          NIVONA_CAT_COFFEE     },
    { 2, "Americano",       NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    // NB: selector 4 is deliberately absent in the upstream table.
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Milk",            NIVONA_CAT_MILK_ONLY  },
    { 7, "Hot Water",       NIVONA_CAT_WATER      },
};

static const nivona_recipe_t RECIPES_900[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",          NIVONA_CAT_COFFEE     },
    { 2, "Americano",       NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    { 4, "Caffè Latte",     NIVONA_CAT_MILK_DRINK },
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Hot Milk",        NIVONA_CAT_MILK_ONLY  },
    { 7, "Hot Water",       NIVONA_CAT_WATER      },
};

static const nivona_recipe_t RECIPES_1030[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",          NIVONA_CAT_COFFEE     },
    { 2, "Americano",       NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    { 4, "Caffè Latte",     NIVONA_CAT_MILK_DRINK },
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Hot Water",       NIVONA_CAT_WATER      },
    { 7, "Warm Milk",       NIVONA_CAT_MILK_ONLY  },
    { 8, "Hot Milk",        NIVONA_CAT_MILK_ONLY  },
    { 9, "Frothy Milk",     NIVONA_CAT_MILK_ONLY  },
};

static const nivona_recipe_t RECIPES_1040[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",          NIVONA_CAT_COFFEE     },
    { 2, "Americano",       NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    { 4, "Caffè Latte",     NIVONA_CAT_MILK_DRINK },
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Hot Water",       NIVONA_CAT_WATER      },
    { 7, "Warm Milk",       NIVONA_CAT_MILK_ONLY  },
    { 8, "Frothy Milk",     NIVONA_CAT_MILK_ONLY  },
};

static const nivona_recipe_t RECIPES_8000[] = {
    { 0, "Espresso",        NIVONA_CAT_ESPRESSO   },
    { 1, "Coffee",          NIVONA_CAT_COFFEE     },
    { 2, "Americano",       NIVONA_CAT_AMERICANO  },
    { 3, "Cappuccino",      NIVONA_CAT_MILK_DRINK },
    { 4, "Caffè Latte",     NIVONA_CAT_MILK_DRINK },
    { 5, "Latte Macchiato", NIVONA_CAT_MILK_DRINK },
    { 6, "Milk",            NIVONA_CAT_MILK_ONLY  },
    { 7, "Hot Water",       NIVONA_CAT_WATER      },
    // Chilled-brew selectors — exposed only by the NICR 8107, which is
    // the concrete model this family emulates (ble_name 8107…). The app
    // lists them as separate recipes and sends them with the HE chilled
    // flag byte (payload[5] = 0x00) instead of the normal 0x01. Mirrors
    // RECIPES_8000_CHILLED in brands/nivona/_family_8000.py, applied
    // there for prefix 8107 via capabilities_for_model(). The chilled
    // temperature ramp itself is not modelled — these reuse the base
    // category ramp (the emulator has no thermal model; FUNCTIONAL_COVERAGE
    // G2). STATS_8000.recipe_id_mask already enables 8/9/10.
    { 8,  "Chilled Espresso",  NIVONA_CAT_ESPRESSO  },
    { 9,  "Chilled Lungo",     NIVONA_CAT_COFFEE    },
    { 10, "Chilled Americano", NIVONA_CAT_AMERICANO },
};

#define COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

// Per-family temp-recipe (HW 9001 + offset) layouts. Mirrors the
// `_STANDARD_RECIPE_LAYOUTS` assembled in brands/nivona/__init__.py
// from each _family_*.py's RecipeFieldLayout. Only the fields used
// by the emulator's ramp-scaling heuristic are tracked (strength,
// two_cups, four fluid volumes). Temperatures exist on real hardware
// but the emulator has no thermal model.
//
// Field-naming guide:
//   600  / 700 / 79x / 8000 — single temperature field at offset 3
//                             (we don't read it)
//   900  / 900-light       — separate temps at 5..8, fluids at 9..12
//   1030 / 1040            — same as 900 (fluids at 9..12)
// Milk fields are absent on 600 (no milk system in this family).
static const nivona_recipe_layout_t LAYOUT_600  = { 1, 4, 5, 6, -1,  8 };
static const nivona_recipe_layout_t LAYOUT_700  = { 1, 4, 5, 6,  7,  8 };
static const nivona_recipe_layout_t LAYOUT_79X  = { 1, 4, 5, 6,  7,  8 };
static const nivona_recipe_layout_t LAYOUT_900  = { 1, 4, 9, 10, 11, 12 };
static const nivona_recipe_layout_t LAYOUT_1000 = { 1, 4, 9, 10, 11, 12 };
static const nivona_recipe_layout_t LAYOUT_8000 = { 1, 4, 5, 6,  7,  8 };

const nivona_family_t NIVONA_FAMILIES[] = {
    // key         ble_name              model         ready  brew   scale  milk  lid   mode   recipes           n                          layout
    { "600",       "6801000001-----",   "NICR 680",   8,     11,    1,     0,    0,    0x0B,  RECIPES_600,      COUNT(RECIPES_600),        LAYOUT_600  },
    { "700",       "7591000001-----",   "NICR 759",   8,     11,    1,     0,    0,    0x0B,  RECIPES_700,      COUNT(RECIPES_700),        LAYOUT_700  },
    { "79x",       "7951000001-----",   "NICR 795",   8,     11,    1,     0,    0,    0x0B,  RECIPES_79X,      COUNT(RECIPES_79X),        LAYOUT_79X  },
    { "900",       "9301000001-----",   "NICR 930",   8,     11,    10,    1,    0,    0x0B,  RECIPES_900,      COUNT(RECIPES_900),        LAYOUT_900  },
    { "900-light", "9701000001-----",   "NICR 970",   8,     11,    1,     1,    0,    0x0B,  RECIPES_900,      COUNT(RECIPES_900),        LAYOUT_900  },
    { "1030",      "0301000001-----",   "NICR 1030",  8,     11,    1,     1,    1,    0x0B,  RECIPES_1030,     COUNT(RECIPES_1030),       LAYOUT_1000 },
    { "1040",      "0401000001-----",   "NICR 1040",  8,     11,    1,     1,    1,    0x0B,  RECIPES_1040,     COUNT(RECIPES_1040),       LAYOUT_1000 },
    { "8000",      "8107000001-----",   "NIVO 8107",  3,     4,     1,     1,    1,    0x04,  RECIPES_8000,     COUNT(RECIPES_8000),       LAYOUT_8000 },
};

const size_t NIVONA_FAMILIES_COUNT =
    sizeof(NIVONA_FAMILIES) / sizeof(NIVONA_FAMILIES[0]);

// Default: resolved at restore_once() time via nivona_family_find("8000")
// rather than a magic table index — adding or reordering NIVONA_FAMILIES[]
// would silently shift the default otherwise (audit-V3 finding I7).
static const nivona_family_t *s_current = NULL;
static bool s_restored = false;
// restore_once is called from multiple tasks (brew_task, cycle_task,
// CLI). The spinlock makes the final publish of s_current + s_restored
// atomic across cores (Xtensa/RISC-V bool read-modify-write is not
// atomic across cores). It guards ONLY those two writes — never the NVS
// read, which must run with interrupts enabled (see restore_once).
static portMUX_TYPE s_restore_mux = portMUX_INITIALIZER_UNLOCKED;

static void restore_once(void) {
    // Fast path — already resolved.
    if (s_restored) return;

    // CRITICAL: resolve the family (including the NVS read) WITHOUT holding
    // s_restore_mux. NVS / SPI-flash operations must run with interrupts
    // enabled — calling nvs_open()/nvs_get_str() inside a portMUX critical
    // section makes the flash driver take its mutex in a no-interrupts
    // context, which aborts in lock_acquire_generic and boot-loops the
    // device. The default "8000" has no NVS entry (nvs_open returns
    // NOT_FOUND, the read is skipped), so the crash only showed once a
    // family had actually been switched and persisted. (Fixed 2026-06-03.)
    const nivona_family_t *resolved = nivona_family_find("8000");
    if (resolved == NULL) resolved = &NIVONA_FAMILIES[0];

    nvs_handle_t h;
    if (nvs_open(NS_FAM, NVS_READONLY, &h) == ESP_OK) {
        char key[16] = {0};
        size_t sz = sizeof(key) - 1;
        if (nvs_get_str(h, KEY_FAM_CUR, key, &sz) == ESP_OK) {
            const nivona_family_t *f = nivona_family_find(key);
            if (f != NULL) {
                resolved = f;
                ESP_LOGI(TAG, "restored family=%s from NVS", key);
            }
        }
        nvs_close(h);
    }

    // Publish atomically. The spinlock now guards only the pointer + flag
    // writes (no I/O) — its allowed use. If a concurrent caller already
    // published, it wins and our redundant read is simply dropped.
    portENTER_CRITICAL(&s_restore_mux);
    if (!s_restored) {
        s_current = resolved;
        s_restored = true;
    }
    portEXIT_CRITICAL(&s_restore_mux);
}

const nivona_family_t *nivona_family_current(void) {
    restore_once();
    return s_current;
}

const nivona_family_t *nivona_family_find(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < NIVONA_FAMILIES_COUNT; i++) {
        if (strcmp(key, NIVONA_FAMILIES[i].key) == 0) {
            return &NIVONA_FAMILIES[i];
        }
    }
    return NULL;
}

int nivona_family_set(const char *key) {
    const nivona_family_t *f = nivona_family_find(key);
    if (f == NULL) return -1;
    s_current = f;
    s_restored = true; // prevent a later restore_once from stomping this
    nvs_handle_t h;
    if (nvs_open(NS_FAM, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_FAM_CUR, key);
        nvs_commit(h);
        nvs_close(h);
    }
    return 0;
}

const nivona_recipe_t *nivona_family_recipe_by_selector(
    const nivona_family_t *fam, uint8_t selector) {
    if (fam == NULL || fam->recipes == NULL) return NULL;
    for (size_t i = 0; i < fam->recipe_count; i++) {
        if (fam->recipes[i].selector == selector) return &fam->recipes[i];
    }
    return NULL;
}
