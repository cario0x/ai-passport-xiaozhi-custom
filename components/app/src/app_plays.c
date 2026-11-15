#include "xiaozhi.h"
#include "presentation/ui_xiaozhi.h"
#include "presentation/companion_resources.h"
#include "app/app_plays.h"
#include "platform/board_config.h"
#include "platform/platform_factory.h"
#include "platform/platform_clicker.h"
#include "presentation/ui_plays.h"
#include "presentation/ui_game.h"
#include "presentation/ui_button_guide.h"
#include "services/answer_picker.h"
#include "services/answer_content.h"
#include "services/play_runtime.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

enum { PLAY_CLOSED = -2, PLAY_MENU = -1 };
typedef enum { EVENT_KEY, EVENT_SELECT, EVENT_VOICE_CHECK } event_kind_t;
typedef struct { event_kind_t kind; int value, argument; uint64_t at_ms; uint32_t generation; } play_event_t;
static const char *TAG = "plays";
static app_plays_config_t C;
static void ui_device_preferences(void);
static QueueHandle_t queue;
static atomic_int active = PLAY_CLOSED;
static atomic_bool abort_microphone;
static play_sleep_gate_t sleep_gate;
static play_scores_t pending_scores;
static portMUX_TYPE score_guard = portMUX_INITIALIZER_UNLOCKED;
static atomic_int best_score;
static atomic_uint input_generation;
static atomic_uint config_connections;
static unsigned connect_open_generation;
// Worker-owned intent belongs to the page where a use was selected. Sleeping
// temporarily disables transport without turning the use-selection page on.
static bool clicker_enabled, clicker_sleep_paused;
static bool xz_confirm_armed;
static bool xz_restart, xz_setup, xz_leaving, xz_exit_fault_shown;
static void xz_touch_confirm(void *user) {
    (void)user;
    // Same confirm index the 3-key Passport UI uses; works with StopWatch remap.
    app_plays_button(2, HAL_BTN_PRESS);
    app_plays_button(2, HAL_BTN_RELEASE);
}
// Only the plays worker writes this bit. The old Wi-Fi profile stays intact
// if a card in manual setup sleeps before the user submits a new network.
RTC_DATA_ATTR static bool resume_setup_after_sleep;
static atomic_bool voice_check_pending;
static uint64_t voice_check_deadline;
static bool voice_check_return_home;
static uint32_t clicker_generation;
// Only the raw-button producer accesses these. RELEASE/CLICK/LONG belong to
// the page where their PRESS began, even if the callback arrives after a page
// transition. In particular, LVGL opens Plays on release before iot_button
// finishes its delayed single-click classification on the homepage gesture.
static uint32_t key_generation[3];
static ab_session_t session;
static ab_audio_meter_t meter;
static ab_answer_picker_t picker;
static bool mic_owned;
static uint32_t mic_generation;
static int16_t mic_frame[320];

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
void app_plays_config_connected(void) { atomic_fetch_add(&config_connections, 1); }
static bool post(event_kind_t kind, int value, int argument) {
    play_event_t event = {.kind = kind, .value = value, .argument = argument, .at_ms = now_ms(),
                          .generation = atomic_load(&input_generation)};
    return queue && xQueueSend(queue, &event, 0) == pdTRUE;
}
static bool select_play(ui_play_id_t play, void *user) {
    (void)user;
    if (play == UI_PLAY_CONNECT && !C.identity) return false;
    return post(EVENT_SELECT, play, 0);
}
bool app_plays_voice_check(void) {
    int mode = atomic_load(&active);
    if (!queue || (mode != PLAY_MENU && mode != UI_PLAY_XIAOZHI) ||
        play_sleep_blocked(&sleep_gate) || !xiaozhi_stopped()) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&voice_check_pending, &expected, true)) return false;
    if (post(EVENT_VOICE_CHECK, 0, 0)) return true;
    atomic_store(&voice_check_pending, false);
    return false;
}
static void exited(void *user) {
    (void)user;
    atomic_store(&active, PLAY_CLOSED);
    atomic_fetch_add(&input_generation, 1);
    atomic_store(&abort_microphone, true);
    if (C.exit) C.exit(C.user);
}
static void child_exited(void *user) {
    (void)user;
    atomic_store(&active, PLAY_MENU);
    atomic_fetch_add(&input_generation, 1);
    atomic_store(&abort_microphone, true);
    ui_plays_menu();
}
static void score_event(int score, void *user) {
    (void)user;
    taskENTER_CRITICAL(&score_guard);
    play_scores_add(&pending_scores, score > 0 ? (unsigned)score : 0);
    taskEXIT_CRITICAL(&score_guard);
}
static void drain_scores(void) {
    taskENTER_CRITICAL(&score_guard);
    play_scores_t results = play_scores_take(&pending_scores);
    taskEXIT_CRITICAL(&score_guard);
    if (!results.rounds) return;
    if (results.best > (unsigned)atomic_load(&best_score)) best_score = results.best;
    if (C.game_result) C.game_result(results.total, results.best, results.rounds, C.user);
}
static void mic_stop(void) {
    memset(mic_frame, 0, sizeof(mic_frame));
    if (!mic_owned) return;
    mic_owned = false;
    if (C.release_audio) C.release_audio(C.user);
}
static void cancel_microphone(void) {
    mic_stop();
    ab_session_audio_error(&session, now_ms());
}
static void sync_microphone_owner(void) {
    // The UI/producer can invalidate capture while acquire/read is blocked.
    // Recheck after those calls before consuming even the final PCM frame.
    if (atomic_exchange(&abort_microphone, false)) {
        cancel_microphone();
        return;
    }
    if (mic_owned && (play_sleep_blocked(&sleep_gate) ||
        atomic_load(&active) != UI_PLAY_ANSWER || mic_generation != atomic_load(&input_generation))) {
        cancel_microphone();
    }
}
static void mic_start(uint64_t started_ms) {
    const uint32_t generation = atomic_load(&input_generation);
    if (!ab_session_press(&session, 0, started_ms)) return;
    ab_audio_meter_reset(&meter);
    if (!C.audio || !C.acquire_audio || !C.acquire_audio(C.user)) {
        ab_session_audio_error(&session, now_ms());
        return;
    }
    mic_owned = true;
    mic_generation = generation;
    sync_microphone_owner();
}
static void mic_finish(uint64_t ended_ms) {
    mic_stop();
    ab_audio_summary_t summary = ab_audio_meter_snapshot(&meter);
    if (ab_session_release(&session, 0, ended_ms, summary) == AB_RELEASE_REVEAL) {
        size_t answer = ab_answer_picker_next(&picker, answer_text_count,
                                             esp_random() ^ (uint32_t)ended_ms);
        if (answer != SIZE_MAX) ab_session_set_answer(&session, answer);
    }
}
static bool launch(int play) {
    if (atomic_load(&active) != PLAY_MENU || play < 0 || play > UI_PLAY_CONNECT) return true;
    cancel_microphone();
    char sn[24] = {0}, key[HAL_IDENTITY_KEY_CAP] = {0};
    bool identity_ready = play != UI_PLAY_CONNECT;
    unsigned connection_generation = atomic_load(&config_connections);
    if (play == UI_PLAY_CONNECT && C.identity) {
        int sn_len = hal_identity_get_sn(C.identity, sn, sizeof sn);
        int key_len = hal_identity_get_key(C.identity, key, sizeof key);
        identity_ready = sn_len > 0 && sn_len < (int)sizeof sn &&
                         key_len > 0 && key_len < (int)sizeof key;
    }
    if (play == UI_PLAY_ANSWER) ab_session_init(&session);
    // The worker retains this transition until the lock is available. It never
    // competes with raw input for another queue slot after the UI was suspended.
    if (!platform_lvgl_lock(300)) return false;
    if (!identity_ready) {
        // Selection already suspended the menu. Restore it, but never publish
        // a seemingly valid onboarding QR with missing/truncated credentials.
        ui_plays_menu();
        platform_lvgl_unlock();
        return true;
    }
    atomic_store(&active, play);
    atomic_fetch_add(&input_generation, 1);
    if (play == UI_PLAY_XIAOZHI) {
        xz_confirm_armed = false;
        xz_restart = true; xz_setup = resume_setup_after_sleep; xz_leaving = xz_exit_fault_shown = false;
        resume_setup_after_sleep = false;
        xz_view_t initial;
        xiaozhi_snapshot(&initial); initial.phase = XZ_BOOT; initial.detail[0] = 0;
        initial.output_active = false;
        ui_xiaozhi_open(&initial, companion_avatar(initial.avatar));
        // Touch controls call back here (no presentation→app CMake edge).
        ui_xiaozhi_set_touch_confirm(xz_touch_confirm, NULL);
        if (C.voice_opened) C.voice_opened(C.user);
    } else if (play == UI_PLAY_DODGE) {
        ui_game_set_best(best_score);
        ui_game_open(child_exited, NULL);
    } else if (play == UI_PLAY_CONNECT) {
        // An existing connection must not hide a QR explicitly opened by the
        // user. A new connection after entry returns home just like cold boot.
        connect_open_generation = connection_generation;
        ui_button_guide_open_menu(sn, key);
    }
    platform_lvgl_unlock();
    return true;
}
static bool clicker_phone, clicker_touch, clicker_scroll_reversed;
static void sync_clicker_owner(void) {
    if (clicker_enabled && (atomic_load(&active) != UI_PLAY_CLICKER ||
        clicker_generation != atomic_load(&input_generation))) {
        clicker_enabled = clicker_sleep_paused = false;
        platform_clicker_set_active(false);
    } else if (clicker_sleep_paused && !play_sleep_blocked(&sleep_gate)) {
        // Page callbacks use this lock too. Revalidate after waiting so an exit
        // or a new selector session cannot race a stale resume into advertising.
        if (!platform_lvgl_lock(100)) return;
        if (atomic_load(&active) != UI_PLAY_CLICKER ||
            clicker_generation != atomic_load(&input_generation)) {
            clicker_enabled = clicker_sleep_paused = false;
        } else if (!play_sleep_blocked(&sleep_gate)) {
            clicker_sleep_paused = false;
            if (clicker_enabled) platform_clicker_set_active(true);
        }
        platform_lvgl_unlock();
    }
}
static void button(const play_event_t *event) {
    int mode = atomic_load(&active), index = event->value;
    hal_btn_event_t kind = (hal_btn_event_t)event->argument;
    if (play_sleep_blocked(&sleep_gate) || mode == PLAY_CLOSED ||
        event->generation != atomic_load(&input_generation)) return;
    if (mode == UI_PLAY_ANSWER) {
        if (index == 0 && kind == HAL_BTN_PRESS) mic_start(event->at_ms);
        if (index == 0 && kind == HAL_BTN_RELEASE) mic_finish(event->at_ms);
    }
    if (mode == UI_PLAY_CLICKER) {
        ui_play_key_t click_key;
        if (kind == HAL_BTN_CLICK && index == 0) click_key = UI_PLAY_UP;
        else if (kind == HAL_BTN_CLICK && index == 1) click_key = UI_PLAY_DOWN;
        else if (kind == HAL_BTN_CLICK && index == 2) click_key = UI_PLAY_OK;
        else if (kind == HAL_BTN_LONG && index == 2) click_key = UI_PLAY_BACK;
        else return;
        if (!platform_lvgl_lock(300)) return;
        ui_click_action_t selected = ui_plays_clicker_key(click_key);
        if (selected == UI_CLICK_ACTION_EXIT) {
            clicker_enabled = clicker_sleep_paused = false;
            platform_clicker_set_active(false);
            child_exited(NULL);
        } else if (selected == UI_CLICK_ACTION_CHANGE_COMPUTER) {
            ui_plays_clicker_feedback("正在断开旧设备", platform_clicker_change_computer());
        } else if (selected == UI_CLICK_ACTION_REVERSE_SCROLL) {
            if (clicker_phone && !clicker_touch) clicker_scroll_reversed = !clicker_scroll_reversed;
        } else if (selected == UI_CLICK_ACTION_POINTER_TEST) {
            if (clicker_phone && !clicker_touch)
                ui_plays_clicker_feedback("观察手机圆点是否出现", platform_clicker_send(CLICKER_POINTER_TEST));
        } else if (selected == UI_CLICK_ACTION_TOUCH_MODE || selected == UI_CLICK_ACTION_PHONE_MODE || selected == UI_CLICK_ACTION_COMPUTER_MODE) {
            clicker_touch = selected == UI_CLICK_ACTION_TOUCH_MODE;
            clicker_phone = clicker_touch || selected == UI_CLICK_ACTION_PHONE_MODE;
            clicker_enabled = true;
            clicker_generation = event->generation;
            clicker_sleep_paused = false;
            platform_clicker_set_active(true);
        } else if (selected != UI_CLICK_ACTION_NONE) {
            clicker_action_t action = clicker_touch ? CLICKER_TOUCH_PREV : clicker_phone ? CLICKER_PHONE_PREV : CLICKER_PREV;
            const char *label = clicker_phone ? "上一条" : "上一页";
            switch (selected) {
                case UI_CLICK_ACTION_NEXT: action = clicker_touch ? CLICKER_TOUCH_NEXT : clicker_phone ? CLICKER_PHONE_NEXT : CLICKER_NEXT; label = clicker_phone ? "下一条" : "下一页"; break;
                case UI_CLICK_ACTION_START: action = CLICKER_START; label = "开始演示"; break;
                case UI_CLICK_ACTION_END: action = CLICKER_END; label = "结束演示"; break;
                case UI_CLICK_ACTION_BLANK: action = CLICKER_BLANK; label = "隐藏画面 / 再按恢复"; break;
                default: break;
            }
            if (clicker_phone && !clicker_touch && clicker_scroll_reversed &&
                (action == CLICKER_PHONE_PREV || action == CLICKER_PHONE_NEXT))
                action = action == CLICKER_PHONE_PREV ? CLICKER_PHONE_NEXT : CLICKER_PHONE_PREV;
            ui_plays_clicker_feedback(label, platform_clicker_send(action));
        }
        platform_lvgl_unlock();
        return;
    }
    if (mode == UI_PLAY_XIAOZHI) {
        if (xz_leaving) return;
        ui_play_key_t key;
#if BOARD_M5STACK_STOPWATCH
        // Official v2.6.0 StopWatch: Button1=GPIO2 chat/wifi, Button2=GPIO1 volume.
        // Passport Xiaozhi UI expects 3-key UP/DOWN/OK (OK is index 2). Map the
        // two physical keys so OK works: Button2 (index 1) = confirm; Button1
        // (index 0) = UP. Synthetic touch still uses index 2.
        // Welcome copy "请按右侧最下方的按钮" → physical bottom-right = Button2.
        if (index == 1) {
            if (kind == HAL_BTN_PRESS) { xz_confirm_armed = true; return; }
            if (xz_confirm_armed && (kind == HAL_BTN_LONG || kind == HAL_BTN_RELEASE)) {
                key = kind == HAL_BTN_LONG ? UI_PLAY_BACK : UI_PLAY_OK;
                xz_confirm_armed = false;
            } else return;
        } else if (index == 0 && kind == HAL_BTN_PRESS) {
            key = UI_PLAY_UP;
        } else if (index == 0 && kind == HAL_BTN_LONG) {
            key = UI_PLAY_BACK;
        } else if (index == 2 && kind == HAL_BTN_PRESS) {
            xz_confirm_armed = true;
            return;
        } else if (index == 2 && xz_confirm_armed && (kind == HAL_BTN_LONG || kind == HAL_BTN_RELEASE)) {
            key = kind == HAL_BTN_LONG ? UI_PLAY_BACK : UI_PLAY_OK;
            xz_confirm_armed = false;
        } else return;
#else
        if (index < 2 && kind == HAL_BTN_PRESS) {
            key = index == 0 ? UI_PLAY_UP : UI_PLAY_DOWN;
        } else if (index == 2 && kind == HAL_BTN_PRESS) {
            xz_confirm_armed = true;
            return;
        } else if (index == 2 && xz_confirm_armed && (kind == HAL_BTN_LONG || kind == HAL_BTN_RELEASE)) {
            key = kind == HAL_BTN_LONG ? UI_PLAY_BACK : UI_PLAY_OK;
            xz_confirm_armed = false;
        } else return;
#endif
        if (!platform_lvgl_lock(300)) return;
        unsigned avatar = 0;
        ui_device_preferences();
        ui_xz_action_t action = ui_xiaozhi_key(key, &avatar);
        if (action == UI_XZ_EXIT) { xiaozhi_stop(); xz_restart = false; xz_leaving = true; resume_setup_after_sleep = false; }
        if (action == UI_XZ_AVATAR) {
            xiaozhi_avatar(avatar);
            xz_view_t view; xiaozhi_snapshot(&view);
            ui_xiaozhi_update(&view, companion_avatar(view.avatar));
        }
        platform_lvgl_unlock();
        if (action == UI_XZ_TALK) xiaozhi_talk();
        else if (action == UI_XZ_BEGIN_SETUP) xiaozhi_begin_setup();
        else if (action == UI_XZ_INTERRUPT) xiaozhi_interrupt();
        else if (action == UI_XZ_RESUME) xiaozhi_resume();
        else if (action == UI_XZ_BRIGHTNESS_PREVIEW || action == UI_XZ_BRIGHTNESS_CANCEL) {
            unsigned value = action == UI_XZ_BRIGHTNESS_CANCEL && C.get_brightness ? C.get_brightness(C.user) : avatar;
            if (C.preview_brightness) (void)C.preview_brightness(C.user, value);
        }
        else if (action == UI_XZ_BRIGHTNESS || action == UI_XZ_SLEEP) {
            bool saved = action == UI_XZ_BRIGHTNESS ?
                C.set_brightness && C.set_brightness(C.user, avatar) :
                C.set_sleep_minutes && C.set_sleep_minutes(C.user, avatar);
            if (platform_lvgl_lock(100)) {
                ui_device_preferences();
                ui_xiaozhi_setting_result(saved);
                platform_lvgl_unlock();
            }
        }
        else if (action == UI_XZ_INTERRUPT_MODE || action == UI_XZ_VOLUME) {
            bool saved = action == UI_XZ_VOLUME ? xiaozhi_set_volume(avatar) :
                         xiaozhi_set_natural_interrupt(avatar != 0);
            xz_view_t current; xiaozhi_snapshot(&current);
            if (saved && action == UI_XZ_INTERRUPT_MODE && current.phase != XZ_STANDBY) {
                xiaozhi_stop(); xz_restart = true; xz_setup = false;
            }
            if (platform_lvgl_lock(100)) {
                xz_view_t view; xiaozhi_snapshot(&view);
                ui_xiaozhi_update(&view, companion_avatar(view.avatar));
                ui_xiaozhi_setting_result(saved);
                platform_lvgl_unlock();
            }
        }
        else if (action == UI_XZ_RETRY || action == UI_XZ_WIFI) {
            xiaozhi_stop(); xz_restart = true; xz_setup = action == UI_XZ_WIFI;
        }
        return;
    }
    bool back = index == 2 && kind == HAL_BTN_LONG;
    ui_play_key_t key;
    if (back) key = UI_PLAY_BACK;
    else if (index == 0 && kind == HAL_BTN_PRESS) key = UI_PLAY_UP;
    else if (index == 1 && kind == HAL_BTN_PRESS) key = UI_PLAY_DOWN;
    else if (index == 2 && kind == HAL_BTN_CLICK) key = UI_PLAY_OK;
    else return;
    if (mode == UI_PLAY_DODGE) {
        ui_game_key((ui_game_key_t)key);
        return;
    }
    bool answer_return = mode == UI_PLAY_ANSWER && (back || key == UI_PLAY_OK);
    if (answer_return) mic_stop();
    if (!platform_lvgl_lock(300)) return;
    if (mode == PLAY_MENU) ui_plays_menu_key(key);
    else if (mode == UI_PLAY_CONNECT && (back || key == UI_PLAY_OK)) {
        ui_button_guide_close();
        child_exited(NULL);
    }
    else if (answer_return) child_exited(NULL);
    platform_lvgl_unlock();
}
static void ui_device_preferences(void) {
    ui_xiaozhi_set_device_settings(C.get_brightness ? C.get_brightness(C.user) : 100,
                                  C.get_sleep_minutes ? C.get_sleep_minutes(C.user) : 5);
}
static void refresh(uint64_t now) {
    int mode = atomic_load(&active);
    if (mode == PLAY_CLOSED || mode == PLAY_MENU || mode == UI_PLAY_DODGE) return;
    if (!platform_lvgl_lock(100)) return;
    mode = atomic_load(&active); // A child may have exited while this worker waited.
    if (mode == UI_PLAY_ANSWER) {
        ab_session_view_t view = ab_session_view(&session);
        ui_plays_answer(&view, ab_audio_level_units(meter.summary.current_centidbfs), now);
    } else if (mode == UI_PLAY_CLICKER) {
        platform_clicker_snapshot_t snapshot; platform_clicker_snapshot(&snapshot);
        if (snapshot.status == PLATFORM_CLICKER_READY &&
            !(clicker_touch ? snapshot.touch_ready : clicker_phone ? snapshot.mouse_ready : snapshot.keyboard_ready))
            snapshot.status = PLATFORM_CLICKER_PAIRING;
        ui_click_view_t view = {.phone_mode = clicker_phone, .touch_mode = clicker_touch, .scroll_reversed = clicker_scroll_reversed, .status = (ui_click_status_t)snapshot.status,
            .sent = snapshot.sent, .failed = snapshot.failed, .name = snapshot.name};
        ui_plays_clicker(&view);
    } else if (mode == UI_PLAY_XIAOZHI) {
        ui_device_preferences();
        xz_view_t view; xiaozhi_snapshot(&view);
        if (xz_leaving) { view.phase = XZ_CLOUD; strcpy(view.detail, "正在返回玩法，请稍候"); view.revision++; }
        ui_xiaozhi_update(&view, companion_avatar(view.avatar));
        if (view.phase == XZ_WELCOME && ui_xiaozhi_welcome_drawn()) xiaozhi_welcome_shown();
    } else if (mode == UI_PLAY_CONNECT &&
               atomic_load(&config_connections) != connect_open_generation) {
        ui_button_guide_close();
        ui_plays_close();
    }
    platform_lvgl_unlock();
}
static void pause_voice_for_sleep(void) {
    if (atomic_load(&active) == UI_PLAY_XIAOZHI && !xz_leaving && !voice_check_return_home) {
        if (!resume_setup_after_sleep && !xiaozhi_stopped()) {
            xz_view_t current;
            xiaozhi_snapshot(&current);
            if (current.phase == XZ_SETUP) resume_setup_after_sleep = true;
            if (current.phase == XZ_WELCOME || current.phase == XZ_STANDBY) {
                xz_restart = true; xz_setup = false;
            }
        }
        // If a key cancels the sleep handshake, reopen the same setup page.
        if (resume_setup_after_sleep) { xz_restart = true; xz_setup = true; }
    } else {
        xz_restart = false;
    }
    xiaozhi_stop();
}

static void worker(void *arg) {
    (void)arg;
    uint64_t last_render = 0;
    bool last_output_active = false;
    int pending_launch = -1;
    uint32_t pending_launch_generation = 0;
    for (;;) {
        xiaozhi_service();
        if (voice_check_return_home && atomic_load(&active) == PLAY_CLOSED) {
            voice_check_deadline = 0; voice_check_return_home = false;
            atomic_store(&voice_check_pending, false);
            xiaozhi_stop();
        }
        if (voice_check_deadline &&
            (now_ms() >= voice_check_deadline ||
             (atomic_load(&active) == UI_PLAY_XIAOZHI && !xz_restart && xiaozhi_stopped()) ||
             play_sleep_blocked(&sleep_gate))) {
            // This deadline lives on the device, so loss of the serial host
            // cannot leave an automatically started conversation running.
            xiaozhi_stop(); xz_restart = false;
            pending_launch = -1; voice_check_deadline = 0;
            if (atomic_load(&active) == UI_PLAY_XIAOZHI) xz_leaving = true;
        }
        if (play_sleep_frozen(&sleep_gate)) { vTaskDelay(pdMS_TO_TICKS(25)); continue; }
        sync_clicker_owner();
        if (xz_leaving && xiaozhi_stopped() && platform_lvgl_lock(100)) {
            xz_view_t view; xiaozhi_snapshot(&view);
            if (view.restart_required && !xz_exit_fault_shown) {
                // Show a restore failure once before leaving. A subsequent
                // return press remains available; the radio refuses retries.
                xz_exit_fault_shown = true;
                ui_xiaozhi_update(&view, companion_avatar(view.avatar));
            } else { ui_xiaozhi_close(); child_exited(NULL); }
            xz_leaving = false;
            platform_lvgl_unlock();
        }
        if (voice_check_return_home && !voice_check_deadline && xiaozhi_stopped() &&
            atomic_load(&active) == PLAY_MENU && platform_lvgl_lock(100)) {
            ui_plays_close();
            voice_check_return_home = false;
            atomic_store(&voice_check_pending, false);
            platform_lvgl_unlock();
        }
        if (atomic_load(&active) != UI_PLAY_XIAOZHI) {
            xiaozhi_stop(); xz_restart = false;
        } else if (play_sleep_blocked(&sleep_gate)) {
            pause_voice_for_sleep();
        } else if (xz_restart && xiaozhi_stopped()) {
            xz_restart = false; resume_setup_after_sleep = false;
            (void)xiaozhi_start(xz_setup);
        }
        if (atomic_exchange(&abort_microphone, false)) {
            cancel_microphone();
        }
        sync_microphone_owner();
        play_event_t event;
        // Queue input wakes this task immediately. Home/menu only need the
        // 500 ms model refresh; keep existing timing inside plays and for
        // transitions and sleep preparation. PCM reads provide their own
        // pacing while recording.
        int mode = atomic_load(&active);
        bool timely = (mode != PLAY_CLOSED && mode != PLAY_MENU) || pending_launch >= 0 ||
                      play_sleep_blocked(&sleep_gate);
        TickType_t wait = mic_owned ? 0 : pdMS_TO_TICKS(timely ? 25 : 500);
        if (xQueueReceive(queue, &event, wait) == pdTRUE) {
            switch (event.kind) {
                case EVENT_KEY: button(&event); break;
                case EVENT_SELECT:
                    if (event.generation == atomic_load(&input_generation)) {
                        pending_launch = event.value; pending_launch_generation = event.generation;
                    }
                    break;
                case EVENT_VOICE_CHECK:
                    if (event.generation != atomic_load(&input_generation) ||
                        play_sleep_blocked(&sleep_gate) || !xiaozhi_stopped() ||
                        (atomic_load(&active) != PLAY_MENU && atomic_load(&active) != UI_PLAY_XIAOZHI) ||
                        now_ms() >= event.at_ms + 45000u) {
                        atomic_store(&voice_check_pending, false);
                        break;
                    }
                    voice_check_deadline = event.at_ms + 45000u;
                    voice_check_return_home = true;
                    if (atomic_load(&active) == PLAY_MENU) {
                        pending_launch = UI_PLAY_XIAOZHI;
                        pending_launch_generation = event.generation;
                    } else { xz_restart = true; xz_setup = false; xz_leaving = false; }
                    break;
            }
        }
        if (pending_launch_generation != atomic_load(&input_generation)) pending_launch = -1;
        if (pending_launch >= 0 && !play_sleep_blocked(&sleep_gate) && launch(pending_launch)) pending_launch = -1;
        drain_scores();
        sync_microphone_owner();
        uint64_t now = now_ms();
        if (mic_owned) {
            size_t got = 0;
            int error = hal_audio_read(C.audio, mic_frame, sizeof(mic_frame), &got);
            now = now_ms(); // PCM belongs to its frame-end clock, not the time before read.
            sync_microphone_owner();
            if (mic_owned) {
                if (error != 0 || got != sizeof(mic_frame)) {
                    mic_stop();
                    ab_session_audio_error(&session, now);
                } else {
                    ab_audio_meter_push(&meter, mic_frame, 320, 20);
                }
                if (mic_owned && now >= session.view.started_ms &&
                    now - session.view.started_ms >= 15000) mic_finish(now);
            }
            // Only the derived level/duration leaves this iteration. Keep no PCM history.
            memset(mic_frame, 0, sizeof(mic_frame));
        }
        ab_session_tick(&session, now);
        unsigned sleep_ticket = play_sleep_ticket(&sleep_gate);
        if ((sleep_ticket & 3) == 1) {
            cancel_microphone();
            pause_voice_for_sleep();
            if (!xiaozhi_stopped()) continue;
            // Set the marker before the platform call: cancellation may arrive
            // either side of disabling HID, including while PREPARING (not READY).
            if (clicker_enabled) clicker_sleep_paused = true;
            platform_clicker_set_active(false);
            // Freeze the timer-driven game before draining its final result.
            if (!platform_lvgl_lock(100)) continue;
            ui_game_pause_for_sleep();
            platform_lvgl_unlock();
            drain_scores();
            (void)play_sleep_publish(&sleep_gate, sleep_ticket);
            continue;
        }
        int page = atomic_load(&active);
        unsigned interval = page == UI_PLAY_ANSWER ? 50 : page == UI_PLAY_XIAOZHI ? 100 : 500;
        bool output_active = page == UI_PLAY_XIAOZHI && xiaozhi_output_active();
        if (output_active != last_output_active || now >= last_render + interval) {
            refresh(now); last_render = now; last_output_active = output_active;
        }
    }
}
bool app_plays_init(const app_plays_config_t *config) {
    if (!config || queue) return false;
    C = *config;
    xz_config_t voice = {.audio = C.audio, .acquire_audio = C.acquire_audio,
        .release_audio = C.release_audio, .portal_activity = C.portal_activity,
        .pause_radio = platform_ble_voice_pause,
        .resume_radio = platform_ble_voice_resume, .get_volume = C.get_volume,
        .set_volume = C.set_volume, .user = C.user, .get_brightness = C.get_brightness,
        .set_brightness = C.set_brightness};
    xiaozhi_init(&voice);
    ab_session_init(&session); ab_audio_meter_reset(&meter); ab_answer_picker_init(&picker);
    mic_owned = false;
    clicker_enabled = clicker_sleep_paused = false;
    queue = xQueueCreate(16, sizeof(play_event_t));
    if (!queue) return false;
    if (xTaskCreate(worker, "plays", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(queue); queue = NULL; return false;
    }
    ui_game_set_result_cb(score_event, NULL);
    ESP_LOGI(TAG, "玩法就绪");
    return true;
}
bool app_plays_open(int best) {
    if (!queue || atomic_load(&active) != PLAY_CLOSED) return false;
    best_score = best;
    atomic_fetch_add(&input_generation, 1);
    atomic_store(&active, PLAY_MENU);
    ui_plays_open(select_play, exited, NULL);
    return true;
}
bool app_plays_active(void) { return atomic_load(&active) != PLAY_CLOSED; }
bool app_plays_open_xiaozhi(int best) {
    if (!app_plays_open(best)) return false;
    // Reuse the worker's normal, generation-checked launch. No network work
    // runs under LVGL; the hidden menu releases its children immediately.
    ui_plays_suspend();
    if (post(EVENT_SELECT, UI_PLAY_XIAOZHI, 0)) return true;
    ui_plays_close();
    return false;
}
bool app_plays_voice_busy(void) {
    if (atomic_load(&active) != UI_PLAY_XIAOZHI) return false;
    xz_view_t view; xiaozhi_snapshot(&view);
    return view.phase == XZ_FAREWELL || view.phase == XZ_WAKING || view.phase == XZ_LISTEN || view.phase == XZ_THINK || view.phase == XZ_SPEAK ||
        view.phase == XZ_BOOT || (view.phase == XZ_WELCOME && view.output_active) ||
        view.phase == XZ_BIND || view.phase == XZ_WIFI || view.phase == XZ_CLOUD;
}
bool app_plays_clicker_active(void) { return atomic_load(&active) == UI_PLAY_CLICKER; }
void app_plays_button(int index, hal_btn_event_t event) {
    if (index < 0 || index >= 3 || !app_plays_active() || play_sleep_blocked(&sleep_gate)) return;
    uint32_t generation = atomic_load(&input_generation);
    if (event == HAL_BTN_PRESS) key_generation[index] = generation;
    if (key_generation[index] != generation) return;
    play_event_t key = {.kind = EVENT_KEY, .value = index, .argument = event,
                        .at_ms = now_ms(), .generation = key_generation[index]};
    if (xQueueSend(queue, &key, 0) != pdTRUE) atomic_store(&abort_microphone, true);
}
bool app_plays_prepare_sleep(void) {
    return !queue || play_sleep_request(&sleep_gate);
}
bool app_plays_commit_sleep(void) {
    return !queue || play_sleep_commit(&sleep_gate);
}
void app_plays_cancel_sleep(void) {
    (void)play_sleep_cancel(&sleep_gate);
}
