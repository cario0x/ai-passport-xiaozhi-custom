// components/ui/presentation/include/presentation/ui_round_layout.h
// Round display layout helpers for M5Stack StopWatch.
// Passport UI is authored for 240×320. On the 466×466 round panel that content
// must be centered as a stage; arc insets apply only to full-bleed round UIs.
#pragma once

#include "lvgl.h"
#include "platform/board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

// Canonical passport content size used across profile / xiaozhi / theme screens.
#define UI_PASSPORT_W  240
#define UI_PASSPORT_H  320

#if SCREEN_IS_ROUND

// Center a 240×320 passport stage on the active round display.
// Parent should be the full-resolution screen; letterbox remains parent bg.
static inline lv_obj_t *ui_round_passport_stage(lv_obj_t *parent, lv_color_t bg) {
    if (!parent) return NULL;
    lv_obj_t *stage = lv_obj_create(parent);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage, UI_PASSPORT_W, UI_PASSPORT_H);
    lv_obj_center(stage);
    lv_obj_set_style_bg_color(stage, bg, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    // Keep clicks on stage children; stage itself need not swallow empty taps.
    return stage;
}

// Center a 240×320 LVGL screen object on the round panel (for lv_screen_load).
static inline void ui_round_center_passport_screen(lv_obj_t *screen) {
    if (!screen) return;
    lv_obj_set_size(screen, UI_PASSPORT_W, UI_PASSPORT_H);
    lv_obj_align(screen, LV_ALIGN_CENTER, 0, 0);
}

static inline void ui_round_set_horizontal_safe(lv_obj_t *obj) {
    if (!obj) return;
    lv_coord_t inset = LV_HOR_RES * 20 / 100;
    lv_obj_set_style_pad_left(obj, inset, 0);
    lv_obj_set_style_pad_right(obj, inset, 0);
}

// Full-bleed round UI only. Passport stages use normal TOP/BOTTOM coords.
static inline void ui_round_align_top(lv_obj_t *obj, lv_coord_t y_offset) {
    if (!obj) return;
    lv_obj_t *parent = lv_obj_get_parent(obj);
    lv_coord_t ph = parent ? lv_obj_get_height(parent) : LV_VER_RES;
    if (ph >= LV_VER_RES - 2) {
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET + y_offset);
    } else {
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y_offset);
    }
}

static inline void ui_round_align_bottom(lv_obj_t *obj, lv_coord_t y_offset) {
    if (!obj) return;
    lv_obj_t *parent = lv_obj_get_parent(obj);
    lv_coord_t ph = parent ? lv_obj_get_height(parent) : LV_VER_RES;
    if (ph >= LV_VER_RES - 2) {
        lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, -(DISPLAY_ROUND_EDGE_INSET + y_offset));
    } else {
        lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, -y_offset);
    }
}

static inline lv_coord_t ui_round_safe_width_at_height(lv_coord_t y_from_center) {
    const lv_coord_t radius = LV_HOR_RES / 2;
    const lv_coord_t y = y_from_center;
    if (y >= radius || y <= -radius) return 0;
    lv_coord_t y_sq = y * y;
    lv_coord_t r_sq = radius * radius;
    lv_coord_t safe_sq = r_sq - y_sq;
    lv_coord_t safe_x = 0;
    for (lv_coord_t i = radius; i > 0; i--) {
        if (i * i <= safe_sq) {
            safe_x = i;
            break;
        }
    }
    return (safe_x * 2 * 85) / 100;
}

#else  // Square display

static inline lv_obj_t *ui_round_passport_stage(lv_obj_t *parent, lv_color_t bg) {
    (void)bg;
    return parent;  // Passport fills the square screen; no extra stage.
}

static inline void ui_round_center_passport_screen(lv_obj_t *screen) {
    if (!screen) return;
    lv_obj_set_size(screen, UI_PASSPORT_W, UI_PASSPORT_H);
}

static inline void ui_round_set_horizontal_safe(lv_obj_t *obj) { (void)obj; }
static inline void ui_round_align_top(lv_obj_t *obj, lv_coord_t y_offset) {
    if (obj) lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y_offset);
}
static inline void ui_round_align_bottom(lv_obj_t *obj, lv_coord_t y_offset) {
    if (obj) lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, -y_offset);
}
static inline lv_coord_t ui_round_safe_width_at_height(lv_coord_t y_from_center) {
    (void)y_from_center;
    return LV_HOR_RES;
}

#endif  // SCREEN_IS_ROUND

#ifdef __cplusplus
}
#endif
