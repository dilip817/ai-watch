/**
 * splash_screen.c — Cue animated splash screen
 *
 * Target: 410 x 502 AMOLED (Waveshare ESP32-S3 2.06")
 *
 * Wordmark: "C" arc fills 0->75%, then "ue" beside it.
 * Sequence: arc fill (1.2s) -> hold (1.5s) -> fade out (0.4s) ~ 3.1s total
 *
 * Arc stroke width = 2, logo 50% larger than original.
 */

#include "splash_screen.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "splash";

/* ── Display ──────────────────────────────────────────────── */
#define DISPLAY_W           410
#define DISPLAY_H           502

/* ── Logo geometry (50% larger: radius 17 -> 26) ─────────── */
/*
 * Arc radius = 26, stroke = 2.
 * Arc diameter = 52, roughly matches Montserrat 48 cap-height (~34px * 1.5 = 51).
 *
 * Wordmark block width: arc_diam(52) + gap(10) + ue_width(~62) = ~124px
 * Center: x_start = (410 - 124) / 2 = 143
 * Arc cx = 143 + 26 + 2 = 171  (radius + stroke keeps left edge in frame)
 * Arc cy = DISPLAY_H/2 - 10 = 241  (slightly above center)
 *
 * "ue" label: x = 143 + 52 + 10 = 205
 * baseline y = cy + radius = 241 + 26 = 267, top of label = 267 - 38 = 229
 *
 * Rotation: 210 degrees puts the gap at ~1 o'clock, C opens to the right.
 */
#define ARC_RADIUS          26
#define ARC_STROKE          4
#define ARC_CX              171
#define ARC_CY              241
#define ARC_ROTATION        45          /* gap centered at 3 o'clock, C opens right */
#define ARC_SWEEP           270         /* 270 deg = 75% of circle */
#define ARC_OBJ_SIZE        ((ARC_RADIUS + ARC_STROKE) * 2)

#define TEXT_X              205
#define TEXT_Y              (ARC_CY + ARC_RADIUS - 38)  /* baseline-align with arc bottom */

/* ── Colors ───────────────────────────────────────────────── */
#define COLOR_BG            lv_color_hex(0x0A0A0A)
#define COLOR_TEAL          lv_color_hex(0x1D9E75)
#define COLOR_TRACK         lv_color_hex(0x1A3530)

/* ── Timing ───────────────────────────────────────────────── */
#define ANIM_FILL_DELAY_MS      300
#define ANIM_FILL_DURATION_MS   1200
#define HOLD_DURATION_MS        1500
#define FADE_DURATION_MS        400

/* ── State ────────────────────────────────────────────────── */
static lv_obj_t             *s_screen       = NULL;
static lv_obj_t             *s_arc          = NULL;
static lv_timer_t           *s_hold_timer   = NULL;
static splash_done_cb_t      s_done_cb      = NULL;

/* ── Prototypes ───────────────────────────────────────────── */
static void arc_anim_cb(void *obj, int32_t val);
static void fade_anim_cb(void *obj, int32_t val);
static void hold_timer_cb(lv_timer_t *t);
static void fade_done_cb(lv_anim_t *a);

/* ── Public ───────────────────────────────────────────────── */

void splash_screen_set_done_cb(splash_done_cb_t cb)
{
    s_done_cb = cb;
}

void splash_screen_show(void)
{
    /* Screen */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, DISPLAY_W, DISPLAY_H);
    lv_obj_set_style_bg_color(s_screen, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Ghost track ring (subtle background circle) */
    lv_obj_t *track = lv_arc_create(s_screen);
    lv_obj_set_size(track, ARC_OBJ_SIZE, ARC_OBJ_SIZE);
    lv_obj_set_pos(track, ARC_CX - ARC_RADIUS - ARC_STROKE,
                          ARC_CY - ARC_RADIUS - ARC_STROKE);
    lv_arc_set_bg_angles(track, 0, 360);
    lv_arc_set_angles(track, 0, 0);
    lv_obj_set_style_arc_color(track, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(track, ARC_STROKE, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_remove_style(track, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);

    /* Animated "C" arc */
    s_arc = lv_arc_create(s_screen);
    lv_obj_set_size(s_arc, ARC_OBJ_SIZE, ARC_OBJ_SIZE);
    lv_obj_set_pos(s_arc, ARC_CX - ARC_RADIUS - ARC_STROKE,
                          ARC_CY - ARC_RADIUS - ARC_STROKE);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_TEAL, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, ARC_STROKE, LV_PART_INDICATOR);
    /* Rounded caps cause stray pixels during animation (caps draw outside
     * the arc's invalidation rect). Use square caps for clean redraws. */
    lv_obj_set_style_arc_rounded(s_arc, false, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(s_arc, ARC_ROTATION);
    lv_arc_set_bg_angles(s_arc, 0, ARC_SWEEP);
    lv_arc_set_range(s_arc, 0, ARC_SWEEP);
    lv_arc_set_value(s_arc, 0);

    /* "ue" label — baseline-aligned with arc bottom */
    lv_obj_t *lbl_ue = lv_label_create(s_screen);
    lv_label_set_text(lbl_ue, "ue");
    lv_obj_set_style_text_color(lbl_ue, COLOR_TEAL, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_ue, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_pos(lbl_ue, TEXT_X, TEXT_Y);

    /* Load screen */
    lv_scr_load(s_screen);

    /* Arc fill animation: 0 -> 270 (= 75%) */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_arc);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, 0, ARC_SWEEP);
    lv_anim_set_duration(&a, ANIM_FILL_DURATION_MS);
    lv_anim_set_delay(&a, ANIM_FILL_DELAY_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* Hold timer -> fade out */
    uint32_t hold_ms = ANIM_FILL_DELAY_MS + ANIM_FILL_DURATION_MS + HOLD_DURATION_MS;
    s_hold_timer = lv_timer_create(hold_timer_cb, hold_ms, NULL);
    lv_timer_set_repeat_count(s_hold_timer, 1);
}

/* ── Animation callbacks ──────────────────────────────────── */

static void arc_anim_cb(void *obj, int32_t val)
{
    lv_arc_set_value((lv_obj_t *)obj, (int16_t)val);
}

static void hold_timer_cb(lv_timer_t *t)
{
    (void)t;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_screen);
    lv_anim_set_exec_cb(&a, fade_anim_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, fade_done_cb);
    lv_anim_start(&a);
}

static void fade_anim_cb(void *obj, int32_t val)
{
    /* Set opacity on the screen object — selector 0 covers all parts */
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)val, 0);
}

static void fade_done_cb(lv_anim_t *a)
{
    (void)a;
    ESP_LOGI(TAG, "Splash animation complete");

    /*
     * This callback runs inside lv_timer_handler() — LVGL mutex already held.
     * Call done_cb which uses ui_show_screen_nolock() to load the home screen.
     * Do NOT call lv_obj_del(s_screen) — lv_screen_load_anim() in the done_cb
     * takes ownership of the screen transition and will handle cleanup.
     */
    if (s_done_cb) s_done_cb();
    s_screen = NULL;
}
