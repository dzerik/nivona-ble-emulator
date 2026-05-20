#include "nivona_maint_cycle.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nivona_brew.h"         // for collision check (brew_active)
#include "nivona_consumables.h"
#include "nivona_families.h"
#include "nivona_frame.h"
#include "nivona_fsm.h"
#include "nivona_maint.h"
#include "nivona_stats.h"
#include "nivona_store.h"

static const char *TAG = "nivona_cycle";

// Maintenance gauges (percent + warning) are universal across all 5
// Nivona families per StatisticsFactory maintenance lists
// (EugsterMobileApp.decompiled.cs:9173-9178, 9215, 9239, 9263, 9304).
#define STAT_DESCALE_PCT         600
#define STAT_DESCALE_WARN        601
#define STAT_BU_CLEAN_PCT        610
#define STAT_BU_CLEAN_WARN       611
#define STAT_FROTHER_CLEAN_PCT   620
#define STAT_FROTHER_CLEAN_WARN  621
#define STAT_FILTER_PCT          640
#define STAT_FILTER_WARN         641
// Cumulative counters are FAMILY-SPECIFIC — fetched at runtime from
// nivona_stats to avoid writing IDs the family doesn't have
// (e.g. descale = 220 on 8000/900, 222 on 1000, absent on 700/600).

// Mirrors nivona_brew.c — all mutations of s_task / s_active / s_cancel
// / s_kind go through s_lock. The mutex also acts as a release/acquire
// fence so cycle_task observes s_kind after nivona_maint_cycle_start
// wrote it (volatile alone does not imply that on dual-core Xtensa).
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static bool s_active = false;
static volatile bool s_cancel = false;
static nivona_cycle_kind_t s_kind = NIVONA_CYCLE_GENERIC_CLEAN;

static void push_status(void) {
    uint8_t payload[8];
    nivona_fsm_pack_status(payload);
    nivona_frame_send("HX", payload, sizeof(payload), false, true);
}

// A cycle is modelled as a small sequence of (sub_process, duration_ms,
// optional manipulation prompt) tuples. The family-specific counter
// ID is resolved at runtime via `resolve_stat_counter` — writing a
// hardcoded 214/220 here would be wrong for 1000-family and absent
// for 700/600 (see audit V2 Focus 5).
typedef enum {
    STAT_COUNTER_NONE = 0,
    STAT_COUNTER_CLEAN_COFFEE,
    STAT_COUNTER_RINSE,
    STAT_COUNTER_FILTER_CHANGE,
    STAT_COUNTER_DESCALE,
} stat_counter_kind_t;

static int16_t resolve_stat_counter(stat_counter_kind_t k) {
    const nivona_stats_t *s = nivona_stats_current();
    switch (k) {
        case STAT_COUNTER_CLEAN_COFFEE:  return s->clean_coffee_id;
        case STAT_COUNTER_RINSE:         return s->rinse_id;
        case STAT_COUNTER_FILTER_CHANGE: return s->filter_change_id;
        case STAT_COUNTER_DESCALE:       return s->descale_id;
        default:                         return 0;
    }
}

typedef struct {
    nivona_cycle_kind_t kind;
    uint32_t            total_ms;
    uint8_t             prep_manip;     // prompt raised at start
                                        // (0 = none)
    stat_counter_kind_t stat_counter;   // resolve_stat_counter()
    int16_t             stat_pct;       // HR id reset to 100 % on completion
    int16_t             stat_warn;      // HR id cleared (0) on completion
    const char         *label;
} cycle_plan_t;

static const cycle_plan_t PLANS[] = {
    { NIVONA_CYCLE_DESCALE,         180000, MANIP_FILL_WATER,      STAT_COUNTER_DESCALE,       STAT_DESCALE_PCT,   STAT_DESCALE_WARN,   "descale"        },
    { NIVONA_CYCLE_EASY_CLEAN,       45000, MANIP_FLUSH_REQUIRED,  STAT_COUNTER_RINSE,         0,                  0,                   "easy_clean"     },
    { NIVONA_CYCLE_INTENSIVE_CLEAN,  90000, MANIP_FILL_POWDER,     STAT_COUNTER_CLEAN_COFFEE,  STAT_BU_CLEAN_PCT,  STAT_BU_CLEAN_WARN,  "intensive_clean"},
    { NIVONA_CYCLE_FILTER_INSERT,    30000, MANIP_FLUSH_REQUIRED,  STAT_COUNTER_FILTER_CHANGE, STAT_FILTER_PCT,    STAT_FILTER_WARN,    "filter_insert"  },
    { NIVONA_CYCLE_FILTER_REPLACE,   30000, MANIP_FLUSH_REQUIRED,  STAT_COUNTER_FILTER_CHANGE, STAT_FILTER_PCT,    STAT_FILTER_WARN,    "filter_replace" },
    { NIVONA_CYCLE_FILTER_REMOVE,    20000, 0,                     STAT_COUNTER_NONE,          STAT_FILTER_PCT,    STAT_FILTER_WARN,    "filter_remove"  },
    { NIVONA_CYCLE_EVAPORATING,      60000, 0,                     STAT_COUNTER_RINSE,         0,                  0,                   "evaporating"    },
    { NIVONA_CYCLE_GENERIC_CLEAN,    20000, 0,                     STAT_COUNTER_RINSE,         0,                  0,                   "rinse"          },
};

static const cycle_plan_t *find_plan(nivona_cycle_kind_t k) {
    for (size_t i = 0; i < sizeof(PLANS)/sizeof(PLANS[0]); i++) {
        if (PLANS[i].kind == k) return &PLANS[i];
    }
    return NULL;
}

int nivona_maint_cycle_from_name(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < sizeof(PLANS)/sizeof(PLANS[0]); i++) {
        if (strcmp(name, PLANS[i].label) == 0) {
            return (int)PLANS[i].kind;
        }
    }
    // Aliases
    if (!strcmp(name, "clean"))  return NIVONA_CYCLE_EASY_CLEAN;
    if (!strcmp(name, "rinse"))  return NIVONA_CYCLE_GENERIC_CLEAN;
    return -1;
}

const char *nivona_maint_cycle_name(nivona_cycle_kind_t k) {
    const cycle_plan_t *p = find_plan(k);
    return p ? p->label : "?";
}

static void cycle_task(void *arg) {
    (void)arg;
    // Snapshot s_kind under the lock — the acquire fence here pairs
    // with the release in nivona_maint_cycle_start so the kind value
    // is guaranteed visible regardless of which core scheduled us.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    nivona_cycle_kind_t kind = s_kind;
    xSemaphoreGive(s_lock);

    const cycle_plan_t *plan = find_plan(kind);
    if (plan == NULL) {
        ESP_LOGE(TAG, "no plan for kind=%d", (int)kind);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_active = false;
        s_task = NULL;
        xSemaphoreGive(s_lock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "cycle start: %s (%ums)", plan->label, (unsigned)plan->total_ms);

    // Prep phase: raise the prompt (if any) and wait for HY — nivona_maint
    // handles soft clears. For the emulator we just announce and proceed
    // after a short wait, matching "user pressed OK".
    if (plan->prep_manip != 0) {
        nivona_fsm_set_manipulation(plan->prep_manip);
        push_status();
        vTaskDelay(pdMS_TO_TICKS(1500));
        // If a real user hasn't HY'd, we clear manipulation ourselves —
        // emulator convenience so CI flows don't hang.
        nivona_fsm_set_manipulation(MANIP_NONE);
    }

    // Set the process code = cycle kind. Progress ramps 0→100.
    //
    // Substitute a "busy/maintenance" sentinel (MELITTA_PROC_BUSY = 99)
    // if the cycle's wire value collides with the current family's
    // brewing code. The only documented collision is
    // NIVONA_CYCLE_FILTER_INSERT = 11 vs family.process_brewing = 11
    // for every non-8000 Nivona family — without this guard HA's
    // process sensor reads filter-insert as "brewing" mid-cycle and
    // any automation gating on is_brewing misbehaves. (Audit-V3 C2.)
    const nivona_family_t *fam_now = nivona_family_current();
    int16_t emit_code = (int16_t)kind;
    if ((int16_t)kind == fam_now->process_brewing ||
        (int16_t)kind == fam_now->process_ready) {
        ESP_LOGW(TAG, "cycle code %d collides with brew/ready on family %s "
                      "— substituting BUSY sentinel 99 for HX emit",
                 (int)kind, fam_now->key);
        emit_code = 99;  // MELITTA_PROC_BUSY
    }
    nivona_fsm_set_process(emit_code, 0);
    push_status();

    const uint32_t TICK_MS = 500;
    uint32_t elapsed = 0;
    while (elapsed < plan->total_ms && !s_cancel) {
        uint32_t step = plan->total_ms - elapsed;
        if (step > TICK_MS) step = TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
        int16_t pct = (int16_t)((elapsed * 100) / plan->total_ms);
        if (pct > 100) pct = 100;
        nivona_fsm_set_progress(pct);
        push_status();
    }

    if (!s_cancel) {
        // Tick the family-specific cumulative counter (if the current
        // family actually has one — 700/79X/600 have NO cumulative
        // stats per StatisticsFactory).
        int16_t counter_id = resolve_stat_counter(plan->stat_counter);
        if (counter_id != 0) {
            nivona_store_set_num(counter_id,
                nivona_store_get_num(counter_id) + 1);
            ESP_LOGI(TAG, "counter bump: HR %d = %d",
                     (int)counter_id,
                     (int)nivona_store_get_num(counter_id));
        } else if (plan->stat_counter != STAT_COUNTER_NONE) {
            const nivona_family_t *fam = nivona_family_current();
            ESP_LOGI(TAG, "counter kind=%d has no HR id on family %s — skipped",
                     (int)plan->stat_counter,
                     fam ? fam->key : "?");
        }
        // Universal maintenance-gauge reset (600/610/620/640 exist on
        // every family's MaintenanceItems list).
        if (plan->stat_pct != 0) {
            nivona_store_set_num(plan->stat_pct, 100);
        }
        if (plan->stat_warn != 0) {
            nivona_store_set_num(plan->stat_warn, 0);
        }
        // Specific side-effects: filter_insert / replace → filter
        // consumable = fresh; descale → water consumable unchanged
        // (real machines consume a lot of water during descale, we
        // don't simulate that yet).
        if (kind == NIVONA_CYCLE_FILTER_INSERT ||
            kind == NIVONA_CYCLE_FILTER_REPLACE) {
            nivona_consumable_set(NIVONA_CONSUM_FILTER, 100);
        }
        if (kind == NIVONA_CYCLE_FILTER_REMOVE) {
            nivona_consumable_set(NIVONA_CONSUM_FILTER, 0);
        }
    }

    // Back to READY.
    const nivona_family_t *fam = nivona_family_current();
    nivona_fsm_set_process(fam->process_ready, 0);
    nivona_fsm_set_progress(0);
    nivona_maint_reevaluate();
    push_status();

    ESP_LOGI(TAG, "cycle done: %s (%s)", plan->label,
             s_cancel ? "cancelled" : "ok");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_active = false;
    s_cancel = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

void nivona_maint_cycle_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

bool nivona_maint_cycle_start(nivona_cycle_kind_t kind) {
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "nivona_maint_cycle_start called before init");
        return false;
    }
    if (nivona_brew_active()) {
        ESP_LOGW(TAG, "brew active — cycle rejected");
        return false;
    }
    if (find_plan(kind) == NULL) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "cycle already active");
        return false;
    }
    s_kind = kind;
    s_active = true;
    s_cancel = false;
    TaskHandle_t new_task = NULL;
    BaseType_t rc = xTaskCreate(cycle_task, "nivona_cycle", 4096,
                                NULL, 5, &new_task);
    if (rc != pdPASS) {
        s_active = false;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "xTaskCreate failed: %d", (int)rc);
        return false;
    }
    s_task = new_task;
    xSemaphoreGive(s_lock);
    return true;
}

void nivona_maint_cycle_cancel(void) {
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) s_cancel = true;
    xSemaphoreGive(s_lock);
}

bool nivona_maint_cycle_active(void) {
    if (s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_active;
    xSemaphoreGive(s_lock);
    return active;
}
