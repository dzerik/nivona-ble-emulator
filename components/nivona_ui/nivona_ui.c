// components/nivona_ui/nivona_ui.c — LVGL v9 UI for Nivona BLE Emulator.
//
// Phase 4: "Variant C" tile dashboard + sub-screens on Waveshare
// ESP32-C6-Touch-LCD-1.47 (172×320 portrait, JD9853 / AXS5106L).
//
// Architecture:
//   - Home screen: header, state tile, 2-col action grid, bottom strip.
//   - Brew picker sub-screen: list of current-family recipes.
//   - Refill sub-screen: Water / Beans / Empty-tray big buttons.
//   - Family picker sub-screen: list of all known families.
//   - lv_timer at 150 ms polls FSM, consumables, BLE state and updates
//     Home-screen widgets in-place (no widget rebuild).
//
// Circular-dependency avoidance:
//   main → nivona_ui (REQUIRES),  nivona_ui -X-> main.
//   We break the cycle by declaring the needed functions `extern` here
//   (they are defined in main component .c files). The relevant
//   self-contained headers (nivona_fsm.h, nivona_families.h,
//   nivona_consumables.h) are pulled in via PRIV_INCLUDE_DIRS in
//   CMakeLists.txt; no header from main/ that #includes other
//   components is included here.

#include "nivona_ui.h"

#include "board.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"

// Self-contained headers from main/ (safe to include directly)
#include "nivona_fsm.h"
#include "nivona_families.h"
#include "nivona_consumables.h"

// ---------------------------------------------------------------------------
// extern declarations for main-component functions without a circular-safe
// header.  These symbols are resolved at link time by the main component.
// ---------------------------------------------------------------------------
extern bool  nivona_brew_start(int16_t selector, bool two_cups);
extern void  nivona_brew_cancel(void);
extern bool  nivona_brew_active(void);

extern bool  nivona_maint_handle_confirm(void);
extern void  nivona_maint_reevaluate(void);

extern bool  nivona_frame_handshake_complete(void);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char *TAG = "nivona_ui";

#define UI_HRES      172
#define UI_VRES      320
#define UI_BUF_LINES  20
#define UI_BUF_SIZE  (UI_HRES * UI_BUF_LINES)

// Colors (dark theme)
#define CLR_BG         0x111318   // near-black background
#define CLR_SURFACE    0x1e2130   // card/tile surface
#define CLR_HEADER     0x0d0f18   // top header bar
#define CLR_GREEN      0x27ae60
#define CLR_RED        0xc0392b
#define CLR_AMBER      0xe67e22
#define CLR_BLUE       0x2980b9
#define CLR_GREY       0x4a4a5a
#define CLR_TEXT_PRI   0xecf0f1
#define CLR_TEXT_SEC   0x95a5a6
#define CLR_TILE_BREW  0x1a5c2e   // brew button dark-green
#define CLR_TILE_CANCL 0x5c1a1a   // cancel dark-red
#define CLR_TILE_CONF  0x1a3d5c   // confirm dark-blue
#define CLR_TILE_REF   0x2e2a12   // refill dark-amber
#define CLR_TILE_FAM   0x1e2a3a   // family dark

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void screen_home_build(void);
static void screen_brew_picker_build(void);
static void screen_refill_build(void);
static void screen_family_picker_build(void);
static void poll_timer_cb(lv_timer_t *t);

// ---------------------------------------------------------------------------
// Screen handles
// ---------------------------------------------------------------------------
static lv_obj_t *g_scr_home         = NULL;
static lv_obj_t *g_scr_brew         = NULL;
static lv_obj_t *g_scr_refill       = NULL;
static lv_obj_t *g_scr_family       = NULL;

// ---------------------------------------------------------------------------
// Home-screen widgets that the poll timer updates
// ---------------------------------------------------------------------------
typedef struct {
    lv_obj_t *ble_dot;          // small colored label "●"
    lv_obj_t *model_lbl;        // header model name

    lv_obj_t *state_tile;       // parent container
    lv_obj_t *state_title;      // "READY" / "BREWING" / prompt text
    lv_obj_t *state_sub;        // sub-label (stage, family key, or prompt hint)
    lv_obj_t *state_bar;        // lv_bar for brew progress (hidden when idle)

    lv_obj_t *btn_brew;         // green BREW tile
    lv_obj_t *btn_cancel;       // red CANCEL tile
    lv_obj_t *btn_confirm;      // blue CONFIRM tile

    lv_obj_t *bottom_strip;     // text label at the bottom
} home_widgets_t;

static home_widgets_t g_hw;

// Remembered state for minimal redraws
static bool     g_last_ble      = false;
static bool     g_last_brewing  = false;
static uint8_t  g_last_manip    = 0xff; // force first update
static int16_t  g_last_progress = -1;
static int16_t  g_last_sub      = -1;
static int16_t  g_last_process  = -999;
static uint8_t  g_last_water    = 0xff;
static uint8_t  g_last_beans    = 0xff;
static uint8_t  g_last_tray     = 0xff;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static const char *manip_label(uint8_t m)
{
    switch (m) {
    case 1:  return "REMOVE GROUNDS";
    case 2:  return "CHECK TRAYS";
    case 3:  return "EMPTY TRAYS";
    case 4:  return "FILL WATER";
    case 5:  return "CLOSE LID";
    case 6:  return "FILL BEANS";
    case 11: return "MOVE CUP";
    case 20: return "FLUSH";
    default: return "CHECK MACHINE";
    }
}

static const char *sub_process_label(int16_t sp)
{
    switch (sp) {
    case 0: return "Idle";
    case 1: return "Grinding";
    case 2: return "Brewing";
    case 3: return "Steaming";
    case 4: return "Hot water";
    case 5: return "Prepare";
    default: return "—";
    }
}

// Create a flat button tile with given bg color and label text.
static lv_obj_t *make_tile_btn(lv_obj_t *parent,
                                const char *text,
                                uint32_t bg_hex)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg_hex), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    // Disabled state: dim the bg
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_DISABLED | LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl);

    return btn;
}

// ---------------------------------------------------------------------------
// HOME SCREEN
// ---------------------------------------------------------------------------

static void on_brew_open(lv_event_t *e)
{
    (void)e;
    screen_brew_picker_build();
    lv_screen_load(g_scr_brew);
}
static void on_cancel(lv_event_t *e)
{
    (void)e;
    nivona_brew_cancel();
}
static void on_confirm(lv_event_t *e)
{
    (void)e;
    nivona_maint_handle_confirm();
}
static void on_refill_open(lv_event_t *e)
{
    (void)e;
    screen_refill_build();
    lv_screen_load(g_scr_refill);
}
static void on_family_open(lv_event_t *e)
{
    (void)e;
    screen_family_picker_build();
    lv_screen_load(g_scr_family);
}

static void screen_home_build(void)
{
    // Create (or reuse) home screen object
    if (!g_scr_home) {
        g_scr_home = lv_obj_create(NULL);
    } else {
        lv_obj_clean(g_scr_home);
    }
    lv_obj_t *scr = g_scr_home;

    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    const nivona_family_t *fam = nivona_family_current();

    // ---- HEADER BAR --------------------------------------------------------
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, UI_HRES, 28);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(CLR_HEADER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hdr, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hdr, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(hdr, LV_SCROLLBAR_MODE_OFF);

    // Model name (left)
    lv_obj_t *model_lbl = lv_label_create(hdr);
    lv_label_set_text(model_lbl, fam->model);
    lv_obj_set_style_text_color(model_lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
    lv_obj_set_style_text_font(model_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    g_hw.model_lbl = model_lbl;

    // BLE indicator (right) — "● BLE"
    lv_obj_t *ble_dot = lv_label_create(hdr);
    lv_label_set_text(ble_dot, LV_SYMBOL_WIFI " BLE");
    lv_obj_set_style_text_color(ble_dot, lv_color_hex(CLR_GREY), LV_PART_MAIN);
    lv_obj_set_style_text_font(ble_dot, &lv_font_montserrat_14, LV_PART_MAIN);
    g_hw.ble_dot = ble_dot;

    // ---- STATE TILE --------------------------------------------------------
    lv_obj_t *st = lv_obj_create(scr);
    lv_obj_set_size(st, UI_HRES - 10, 92);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 33);
    lv_obj_set_style_bg_color(st, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(st, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(st, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(st, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(st, 6, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(st, LV_SCROLLBAR_MODE_OFF);
    g_hw.state_tile = st;

    lv_obj_t *st_title = lv_label_create(st);
    lv_label_set_text(st_title, "READY");
    lv_obj_set_style_text_font(st_title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(st_title, lv_color_hex(CLR_GREEN), LV_PART_MAIN);
    lv_obj_align(st_title, LV_ALIGN_TOP_MID, 0, 4);
    g_hw.state_title = st_title;

    lv_obj_t *st_sub = lv_label_create(st);
    lv_label_set_text(st_sub, fam->key);
    lv_obj_set_style_text_font(st_sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(st_sub, lv_color_hex(CLR_TEXT_SEC), LV_PART_MAIN);
    lv_obj_align(st_sub, LV_ALIGN_TOP_MID, 0, 30);
    g_hw.state_sub = st_sub;

    lv_obj_t *bar = lv_bar_create(st);
    lv_obj_set_size(bar, (UI_HRES - 10) - 20, 10);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_GREY), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CLR_GREEN), LV_PART_INDICATOR);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    g_hw.state_bar = bar;

    // ---- ACTION GRID -------------------------------------------------------
    // 2-column grid, each cell 76px wide, 58px tall; gap 4px.
    // Layout: row0 = BREW | CANCEL, row1 = CONFIRM | REFILL
    // row2 = FAMILY (full width)
    int grid_top = 33 + 92 + 6;  // below state tile

    // BREW button
    lv_obj_t *btn_brew = make_tile_btn(scr, "BREW", CLR_TILE_BREW);
    lv_obj_set_size(btn_brew, 78, 58);
    lv_obj_align(btn_brew, LV_ALIGN_TOP_LEFT, 5, grid_top);
    lv_obj_add_event_cb(btn_brew, on_brew_open, LV_EVENT_CLICKED, NULL);
    g_hw.btn_brew = btn_brew;

    // CANCEL button
    lv_obj_t *btn_cancel = make_tile_btn(scr, "CANCEL", CLR_TILE_CANCL);
    lv_obj_set_size(btn_cancel, 78, 58);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -5, grid_top);
    lv_obj_add_event_cb(btn_cancel, on_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(btn_cancel, LV_STATE_DISABLED);
    g_hw.btn_cancel = btn_cancel;

    // CONFIRM button
    lv_obj_t *btn_confirm = make_tile_btn(scr, "CONFIRM", CLR_TILE_CONF);
    lv_obj_set_size(btn_confirm, 78, 58);
    lv_obj_align(btn_confirm, LV_ALIGN_TOP_LEFT, 5, grid_top + 62);
    lv_obj_add_event_cb(btn_confirm, on_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(btn_confirm, LV_STATE_DISABLED);
    g_hw.btn_confirm = btn_confirm;

    // REFILL button
    lv_obj_t *btn_refill = make_tile_btn(scr, "REFILL", CLR_TILE_REF);
    lv_obj_set_size(btn_refill, 78, 58);
    lv_obj_align(btn_refill, LV_ALIGN_TOP_RIGHT, -5, grid_top + 62);
    lv_obj_add_event_cb(btn_refill, on_refill_open, LV_EVENT_CLICKED, NULL);

    // FAMILY tile (full width)
    lv_obj_t *btn_fam = lv_button_create(scr);
    lv_obj_set_size(btn_fam, UI_HRES - 10, 42);
    lv_obj_align(btn_fam, LV_ALIGN_TOP_MID, 0, grid_top + 62 + 62);
    lv_obj_set_style_bg_color(btn_fam, lv_color_hex(CLR_TILE_FAM), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn_fam, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_fam, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_fam, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_fam, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_fam, on_family_open, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *fam_lbl = lv_label_create(btn_fam);
        // Build "FAMILY · <model> ›"
        char fam_str[48];
        snprintf(fam_str, sizeof(fam_str), "FAMILY  %s  >", fam->model);
        lv_label_set_text(fam_lbl, fam_str);
        lv_obj_set_style_text_color(fam_lbl, lv_color_hex(CLR_TEXT_SEC), LV_PART_MAIN);
        lv_obj_set_style_text_font(fam_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(fam_lbl);
    }

    // ---- BOTTOM STRIP ------------------------------------------------------
    lv_obj_t *bot = lv_label_create(scr);
    {
        uint8_t w = nivona_consumable_get(NIVONA_CONSUM_WATER);
        uint8_t b = nivona_consumable_get(NIVONA_CONSUM_BEANS);
        uint8_t t = nivona_consumable_get(NIVONA_CONSUM_TRAY);
        char buf[48];
        snprintf(buf, sizeof(buf), "W:%d%%  B:%d%%  T:%d%%", w, b, t);
        lv_label_set_text(bot, buf);
    }
    lv_obj_set_style_text_color(bot, lv_color_hex(CLR_TEXT_SEC), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, -4);
    g_hw.bottom_strip = bot;

    // ---- Start poll timer --------------------------------------------------
    lv_timer_create(poll_timer_cb, 150, NULL);
}

// ---------------------------------------------------------------------------
// POLL TIMER  (runs inside LVGL task — no lock needed)
// ---------------------------------------------------------------------------
static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;

    // Only update if Home screen is active
    if (lv_screen_active() != g_scr_home) return;

    nivona_status_t st;
    nivona_fsm_get_status(&st);
    const nivona_family_t *fam = nivona_family_current();
    bool brewing  = nivona_brew_active();
    bool ble_conn = nivona_frame_handshake_complete();
    uint8_t water = nivona_consumable_get(NIVONA_CONSUM_WATER);
    uint8_t beans = nivona_consumable_get(NIVONA_CONSUM_BEANS);
    uint8_t tray  = nivona_consumable_get(NIVONA_CONSUM_TRAY);

    // ---- BLE dot -----------------------------------------------------------
    if (ble_conn != g_last_ble) {
        g_last_ble = ble_conn;
        lv_obj_set_style_text_color(g_hw.ble_dot,
            lv_color_hex(ble_conn ? CLR_GREEN : CLR_GREY), LV_PART_MAIN);
    }

    // ---- State tile --------------------------------------------------------
    bool state_changed = (st.manipulation != g_last_manip)
                      || (brewing        != g_last_brewing)
                      || (st.sub_process != g_last_sub)
                      || (st.process     != g_last_process);

    if (state_changed) {
        g_last_manip    = st.manipulation;
        g_last_brewing  = brewing;
        g_last_sub      = st.sub_process;
        g_last_process  = st.process;

        if (st.manipulation != 0) {
            // Prompt active
            lv_obj_set_style_bg_color(g_hw.state_tile,
                lv_color_hex(CLR_AMBER), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_title, manip_label(st.manipulation));
            lv_obj_set_style_text_color(g_hw.state_title,
                lv_color_hex(0x1a1a00), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_sub, "Action required");
            lv_obj_set_style_text_color(g_hw.state_sub,
                lv_color_hex(0x3a3a00), LV_PART_MAIN);
            lv_obj_add_flag(g_hw.state_bar, LV_OBJ_FLAG_HIDDEN);
        } else if (brewing) {
            lv_obj_set_style_bg_color(g_hw.state_tile,
                lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_title, "BREWING");
            lv_obj_set_style_text_color(g_hw.state_title,
                lv_color_hex(CLR_BLUE), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_sub,
                sub_process_label(st.sub_process));
            lv_obj_set_style_text_color(g_hw.state_sub,
                lv_color_hex(CLR_TEXT_SEC), LV_PART_MAIN);
            lv_obj_remove_flag(g_hw.state_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_bg_color(g_hw.state_tile,
                lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_title, "READY");
            lv_obj_set_style_text_color(g_hw.state_title,
                lv_color_hex(CLR_GREEN), LV_PART_MAIN);
            lv_label_set_text(g_hw.state_sub, fam->key);
            lv_obj_set_style_text_color(g_hw.state_sub,
                lv_color_hex(CLR_TEXT_SEC), LV_PART_MAIN);
            lv_obj_add_flag(g_hw.state_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ---- Brew progress bar -------------------------------------------------
    if (brewing && st.progress != g_last_progress) {
        g_last_progress = st.progress;
        int16_t prog = st.progress;
        if (prog < 0)   prog = 0;
        if (prog > 100) prog = 100;
        lv_bar_set_value(g_hw.state_bar, prog, LV_ANIM_OFF);
    }

    // ---- Button enable/disable states --------------------------------------
    bool prompt_active = (st.manipulation != 0);

    // BREW: enabled when NOT brewing AND no hard prompt
    if (brewing || prompt_active) {
        lv_obj_add_state(g_hw.btn_brew, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(g_hw.btn_brew, LV_STATE_DISABLED);
    }
    // CANCEL: enabled only while brewing
    if (brewing) {
        lv_obj_remove_state(g_hw.btn_cancel, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_hw.btn_cancel, LV_STATE_DISABLED);
    }
    // CONFIRM: enabled only while prompt is active
    if (prompt_active) {
        lv_obj_remove_state(g_hw.btn_confirm, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(g_hw.btn_confirm, LV_STATE_DISABLED);
    }

    // ---- Bottom strip ------------------------------------------------------
    if (water != g_last_water || beans != g_last_beans || tray != g_last_tray) {
        g_last_water = water;
        g_last_beans = beans;
        g_last_tray  = tray;
        char buf[48];
        snprintf(buf, sizeof(buf), "W:%d%%  B:%d%%  T:%d%%", water, beans, tray);
        lv_label_set_text(g_hw.bottom_strip, buf);
    }
}

// ---------------------------------------------------------------------------
// BREW PICKER SUB-SCREEN
// ---------------------------------------------------------------------------

typedef struct {
    int16_t  selector;
    bool     two_cups;
} brew_ud_t;

// Storage for user-data structs (one per recipe, max 32)
static brew_ud_t g_brew_ud[32];

static void on_brew_back(lv_event_t *e)
{
    (void)e;
    lv_screen_load(g_scr_home);
}

static void on_brew_recipe(lv_event_t *e)
{
    brew_ud_t *ud = (brew_ud_t *)lv_event_get_user_data(e);
    if (!ud) return;
    bool ok = nivona_brew_start(ud->selector, ud->two_cups);
    if (!ok) {
        // Brief visual feedback on the row — change its color transiently.
        // Keep it simple: just log; the poll timer will show "BREWING" or
        // the button stays pressed.
        ESP_LOGW(TAG, "brew_start rejected (selector=%d)", ud->selector);
    }
    lv_screen_load(g_scr_home);
}

static void screen_brew_picker_build(void)
{
    if (!g_scr_brew) {
        g_scr_brew = lv_obj_create(NULL);
    } else {
        lv_obj_clean(g_scr_brew);
    }
    lv_obj_t *scr = g_scr_brew;
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    // Back button row
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_brew_back, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(back);
        lv_label_set_text(lbl, "< Back");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    // Screen title
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Select Recipe");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Scrollable list below header
    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, UI_HRES - 8, UI_VRES - 40);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_color(list, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);

    const nivona_family_t *fam = nivona_family_current();
    size_t count = fam->recipe_count;
    if (count > 32) count = 32;

    for (size_t i = 0; i < count; i++) {
        g_brew_ud[i].selector  = (int16_t)fam->recipes[i].selector;
        g_brew_ud[i].two_cups  = false;

        lv_obj_t *btn = lv_list_add_button(list, NULL, fam->recipes[i].name);
        lv_obj_set_style_bg_color(btn, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_height(btn, 44);
        lv_obj_add_event_cb(btn, on_brew_recipe, LV_EVENT_CLICKED,
                            (void *)&g_brew_ud[i]);
    }
}

// ---------------------------------------------------------------------------
// REFILL SUB-SCREEN
// ---------------------------------------------------------------------------

static void on_refill_back(lv_event_t *e)
{
    (void)e;
    lv_screen_load(g_scr_home);
}

static void on_refill_water(lv_event_t *e)
{
    (void)e;
    nivona_consumable_set(NIVONA_CONSUM_WATER, 100);
    nivona_maint_reevaluate();
    ESP_LOGI(TAG, "Refill: Water -> 100%%");
}
static void on_refill_beans(lv_event_t *e)
{
    (void)e;
    nivona_consumable_set(NIVONA_CONSUM_BEANS, 100);
    nivona_maint_reevaluate();
    ESP_LOGI(TAG, "Refill: Beans -> 100%%");
}
static void on_empty_tray(lv_event_t *e)
{
    (void)e;
    // TRAY: 0% = empty (needs emptying), so set to 0 to "empty" it
    nivona_consumable_set(NIVONA_CONSUM_TRAY, 0);
    nivona_maint_reevaluate();
    ESP_LOGI(TAG, "Refill: Tray emptied -> 0%%");
}

static void screen_refill_build(void)
{
    if (!g_scr_refill) {
        g_scr_refill = lv_obj_create(NULL);
    } else {
        lv_obj_clean(g_scr_refill);
    }
    lv_obj_t *scr = g_scr_refill;
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    // Back button
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_refill_back, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(back);
        lv_label_set_text(lbl, "< Back");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Refill");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    int btn_y = 48;
    int btn_h = 64;
    int gap   = 10;
    int btn_w = UI_HRES - 20;

    // Water 100%
    lv_obj_t *bw = lv_button_create(scr);
    lv_obj_set_size(bw, btn_w, btn_h);
    lv_obj_align(bw, LV_ALIGN_TOP_MID, 0, btn_y);
    lv_obj_set_style_bg_color(bw, lv_color_hex(0x1a3a5c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bw, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bw, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bw, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(bw, on_refill_water, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(bw);
        lv_label_set_text(lbl, "Water 100%");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    // Beans 100%
    lv_obj_t *bb = lv_button_create(scr);
    lv_obj_set_size(bb, btn_w, btn_h);
    lv_obj_align(bb, LV_ALIGN_TOP_MID, 0, btn_y + btn_h + gap);
    lv_obj_set_style_bg_color(bb, lv_color_hex(0x3a2a10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bb, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bb, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bb, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(bb, on_refill_beans, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(bb);
        lv_label_set_text(lbl, "Beans 100%");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    // Empty tray
    lv_obj_t *bt = lv_button_create(scr);
    lv_obj_set_size(bt, btn_w, btn_h);
    lv_obj_align(bt, LV_ALIGN_TOP_MID, 0, btn_y + 2 * (btn_h + gap));
    lv_obj_set_style_bg_color(bt, lv_color_hex(0x3a1a1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bt, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bt, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(bt, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(bt, on_empty_tray, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(bt);
        lv_label_set_text(lbl, "Empty Tray");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }
}

// ---------------------------------------------------------------------------
// FAMILY PICKER SUB-SCREEN
// ---------------------------------------------------------------------------

static void on_family_back(lv_event_t *e)
{
    (void)e;
    lv_screen_load(g_scr_home);
}

static void on_family_select(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    if (!key) return;
    int rc = nivona_family_set(key);
    ESP_LOGI(TAG, "family_set('%s') -> %d", key, rc);
    // Note label: rebuild with a brief "applies after reboot" note
    // For simplicity, navigate back and the model label will update on
    // the next rebuild (user must open home again; poll timer rebuilds model
    // label text on next home load via rebuild). Actually the model_lbl
    // is static text — update it now while we hold the LVGL lock.
    lv_label_set_text(g_hw.model_lbl, nivona_family_current()->model);
    lv_screen_load(g_scr_home);
}

static void screen_family_picker_build(void)
{
    if (!g_scr_family) {
        g_scr_family = lv_obj_create(NULL);
    } else {
        lv_obj_clean(g_scr_family);
    }
    lv_obj_t *scr = g_scr_family;
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    // Back button
    lv_obj_t *back = lv_button_create(scr);
    lv_obj_set_size(back, 60, 28);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back, lv_color_hex(CLR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, on_family_back, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *lbl = lv_label_create(back);
        lv_label_set_text(lbl, "< Back");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_center(lbl);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Family");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(CLR_TEXT_PRI), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_text(note, "* applies after reboot");
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(note, lv_color_hex(CLR_AMBER), LV_PART_MAIN);
    lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *list = lv_list_create(scr);
    lv_obj_set_size(list, UI_HRES - 8, UI_VRES - 50);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(list, lv_color_hex(CLR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);

    const nivona_family_t *cur = nivona_family_current();

    for (size_t i = 0; i < NIVONA_FAMILIES_COUNT; i++) {
        const nivona_family_t *f = &NIVONA_FAMILIES[i];
        bool is_cur = (f == cur);

        // Build display text "key · model"
        // We pass the key pointer as user_data (points into static table —
        // always valid).
        char row_text[48];
        snprintf(row_text, sizeof(row_text), "%s%s  %s",
                 is_cur ? "> " : "  ", f->key, f->model);

        lv_obj_t *btn = lv_list_add_button(list, NULL, row_text);
        lv_obj_set_style_bg_color(btn,
            lv_color_hex(is_cur ? 0x1a3a2a : CLR_SURFACE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_text_color(btn,
            lv_color_hex(is_cur ? CLR_GREEN : CLR_TEXT_PRI), LV_PART_MAIN);
        lv_obj_set_style_text_font(btn, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_height(btn, 44);
        // Pass const char * key as user_data — safe, points into static array.
        lv_obj_add_event_cb(btn, on_family_select, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)f->key);
    }
}

// ---------------------------------------------------------------------------
// nivona_ui_start — public entry point (keeps Phase-3 LVGL init intact)
// ---------------------------------------------------------------------------
void nivona_ui_start(void)
{
    // ---- 1. Init LVGL port ------------------------------------------------
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority     = 4,
        .task_stack        = 8192,  // bumped for Phase-4 UI depth
        .task_affinity     = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // ---- 2. Add display ---------------------------------------------------
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = board_lcd_panel_io(),
        .panel_handle  = board_lcd_panel(),
        .buffer_size   = UI_BUF_SIZE,
        .double_buffer = true,
        .hres          = UI_HRES,
        .vres          = UI_VRES,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    // ---- 3. Add touch input -----------------------------------------------
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp   = disp,
        .handle = board_touch(),
    };
    lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (!touch_indev) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed — touch disabled");
    }

    // ---- 4. Build UI (must hold LVGL lock) --------------------------------
    if (lvgl_port_lock(0)) {
        screen_home_build();
        lv_screen_load(g_scr_home);
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Nivona UI (Phase 4) started — 172x320 portrait");
}
