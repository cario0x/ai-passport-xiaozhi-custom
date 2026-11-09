// components/ui/presentation/include/presentation/ui_round_layout.h
// Round display safe-area layout helpers for M5Stack StopWatch
#pragma once

#include "platform/board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if SCREEN_IS_ROUND

// Apply horizontal safe-area padding for round display
// Prevents content from being cut off by circular mask
static inline void ui_round_set_horizontal_safe(lv_obj_t *obj) {
    if (!obj) return;
    // 20% horizontal inset to stay within circular visible area
    lv_coord_t inset = LV_HOR_RES * 20 / 100;
    lv_obj_set_style_pad_left(obj, inset, 0);
    lv_obj_set_style_pad_right(obj, inset, 0);
}

// Align object to top with safe-area offset
// Keeps content below the top arc of the circular display
static inline void ui_round_align_top(lv_obj_t *obj, lv_coord_t y_offset) {
    if (!obj) return;
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, DISPLAY_ROUND_EDGE_INSET + y_offset);
}

// Align object to bottom with safe-area offset
// Keeps content above the bottom arc of the circular display
static inline void ui_round_align_bottom(lv_obj_t *obj, lv_coord_t y_offset) {
    if (!obj) return;
    lv_obj_align(obj, LV_ALIGN_BOTTOM_MID, 0, -(DISPLAY_ROUND_EDGE_INSET + y_offset));
}

// Get safe content width for round display
// Returns width that fits within circular visible area at given height
static inline lv_coord_t ui_round_safe_width_at_height(lv_coord_t y_from_center) {
    // Circular display: r^2 = x^2 + y^2
    // At vertical distance y from center, safe width = 2 * sqrt(r^2 - y^2)
    // For 466x466 display, r = 233
    const lv_coord_t radius = LV_HOR_RES / 2;
    const lv_coord_t y = y_from_center;
    
    if (y >= radius || y <= -radius) return 0;
    
    // Safe width = 2 * sqrt(r^2 - y^2) * 0.85 (15% margin)
    lv_coord_t y_sq = y * y;
    lv_coord_t r_sq = radius * radius;
    lv_coord_t safe_sq = r_sq - y_sq;
    
    // Integer sqrt approximation
    lv_coord_t safe_x = 0;
    for (lv_coord_t i = radius; i > 0; i--) {
        if (i * i <= safe_sq) {
            safe_x = i;
            break;
        }
    }
    
    return (safe_x * 2 * 85) / 100;  // 85% of calculated width
}

#else  // Square display

// No-op for square displays
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
