#pragma once
#include "lvgl.h"
#include "presentation/companion_resources.h"
#include "presentation/ui_play_key.h"
#include "xiaozhi.h"
typedef enum { UI_XZ_NONE, UI_XZ_TALK, UI_XZ_RETRY, UI_XZ_WIFI, UI_XZ_EXIT, UI_XZ_AVATAR,
               UI_XZ_INTERRUPT, UI_XZ_INTERRUPT_MODE, UI_XZ_VOLUME, UI_XZ_RESUME,
               UI_XZ_BEGIN_SETUP, UI_XZ_BRIGHTNESS_PREVIEW, UI_XZ_BRIGHTNESS_CANCEL,
               UI_XZ_BRIGHTNESS, UI_XZ_SLEEP } ui_xz_action_t;
void ui_xiaozhi_open(const xz_view_t *view,const companion_portrait_t *portrait);
void ui_xiaozhi_update(const xz_view_t *view,const companion_portrait_t *portrait);
ui_xz_action_t ui_xiaozhi_key(ui_play_key_t key,unsigned *avatar);
void ui_xiaozhi_setting_result(bool saved);
void ui_xiaozhi_set_device_settings(unsigned brightness, unsigned sleep_minutes);
void ui_xiaozhi_set_battery(int percent, int level);
bool ui_xiaozhi_welcome_drawn(void);
void ui_xiaozhi_close(void);
// Optional: app registers this so LVGL control taps synthesize confirm without
// presentation depending on the app component.
typedef void (*ui_xiaozhi_touch_fn)(void *user);
void ui_xiaozhi_set_touch_confirm(ui_xiaozhi_touch_fn fn, void *user);
