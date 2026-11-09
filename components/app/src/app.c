#include "presentation/companion_resources.h"
#include "presentation/bobo_art.h"
#include "presentation/ui_boot.h"
#include "platform/platform_pet_art.h"
#include "platform/platform_clicker.h"
// components/app/src/app.c —— 唯一知道"具体实现"的地方
#include "app/app.h"
#include "platform/platform_factory.h"
#include "platform/board_config.h"
#if BOARD_M5STACK_STOPWATCH
#include "platform/stopwatch_hardware.h"
#include <driver/i2c_master.h>
#endif
#include "services/ui_model.h"
#include "services/battery.h"
#include "services/factory_test.h"
#include "xiaozhi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static atomic_bool s_factory_command_active;

// 产测按键捕获:置位后 on_btn_raw 把每次按下记入掩码(bit0=上/bit1=下/bit2=确定),
// 期间不走 LVGL 导航/游戏。由 AT+TEST=BTN 开关。
#if PERIPH_BUTTON
static volatile bool    s_btn_test_active = false;
static volatile int     s_btn_test_mask   = 0;
static volatile uint8_t s_btn_test_count[3] = { 0, 0, 0 };   // 自检:各键按下次数(逐键2遍用)
static volatile bool    s_factory_mode    = false;   // 自检模式:业务按键全屏蔽,只喂自检捕获
static hal_button_t     *s_button;
#endif

static const char *TAG = "app";

// 内存诊断:逐节点打印堆,方便定位每个子系统的 RAM 开销与碎片。
//   free    = 当前空闲总量(相邻两次的差 ≈ 该子系统吃掉的内存)
//   min     = 开机至今历史最低空闲(全局低水位,越低越危险)
//   max_blk = 当前最大连续可分配块(大块分配如头像 PNG ~43KB 看这个,揭示碎片)
//   internal= 8 位可用中的内部 DRAM(C3 无 PSRAM,与总量一致,留列便于移植对比)
//   lv_peak  = LVGL 池"历史峰值/池容量"(仅 builtin malloc 模式;走系统堆时恒 0)。
//              定池大小只能看这个峰值 —— 绘制期的临时 layer/draw 缓冲在采样间隙就释放了,
//              看瞬时占用会严重低估,据此砍池会让 LVGL 卡死触发看门狗(已踩过)。
//   music_st = music 任务栈"剩余最小水位"(字节)。远大于 0 说明栈开大了,可按此下调
//              xTaskCreate 的栈参数(留 ~1KB 余量);接近 0 则危险,必须调大。
static TaskHandle_t s_music_task;   // 仅用于读栈水位
static void log_heap(const char *stage) {
    unsigned lu = 0, lf = 0;
#if PERIPH_DISPLAY
    uint32_t u = 0, f = 0;
    platform_lvgl_mem_stats(&u, &f, NULL);
    lu = (unsigned)u; lf = (unsigned)f;
#endif
    // ESP-IDF 的 uxTaskGetStackHighWaterMark 返回**字节**(非 vanilla FreeRTOS 的字),勿再乘 4
    unsigned mst = s_music_task ? (unsigned)uxTaskGetStackHighWaterMark(s_music_task) : 0u;
    ESP_LOGI(TAG, "[HEAP] %-16s free=%6u  min=%6u  max_blk=%6u  lv_peak=%5u/%5u  music_st=%5u",
             stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             lu, lf, mst);
}

// 无动作休眠:任何用户输入都复位计时；超时后进入深睡，由 ADC 功能键节点低电平唤醒。
static atomic_uint s_last_activity_ms;
static atomic_bool s_sleep_preparing, s_deep_sleep_entering;
static portMUX_TYPE s_power_guard = portMUX_INITIALIZER_UNLOCKED;
static void note_activity(void) {
    taskENTER_CRITICAL(&s_power_guard);
    if (!atomic_load(&s_deep_sleep_entering)) s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    taskEXIT_CRITICAL(&s_power_guard);
}

#if PERIPH_DISPLAY
static bool note_portal_activity(void) {
    taskENTER_CRITICAL(&s_power_guard);
    bool awake = !atomic_load(&s_sleep_preparing) && !atomic_load(&s_deep_sleep_entering);
    if (awake) atomic_store(&s_last_activity_ms, (uint32_t)(esp_timer_get_time() / 1000));
    taskEXIT_CRITICAL(&s_power_guard);
    return awake;
}
#endif

#if PERIPH_AUDIO
#include "services/audio_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#if PERIPH_BLE
#include "services/score_rx.h"
#include "services/rtttl.h"
#include "services/square_synth.h"
#endif
#endif
#if PERIPH_DISPLAY
#include "presentation/ui_profile.h"
#include "presentation/ui_xiaozhi.h"
#include "presentation/ui_album.h"
#include "presentation/ui_game.h"
#include "presentation/ui_reward.h"
#include "presentation/ui_factory.h"
#include "presentation/ui_selftest.h"
#include "app/app_plays.h"
#include "presentation/ui_button_guide.h"
#endif
#if PERIPH_DISPLAY && PERIPH_BUTTON
#include "hal/hal_button.h"
#endif
#if PERIPH_BLE
#include "services/profile_ctl.h"
#include "services/config_json.h"
#include "hal/hal_identity.h"
#include "services/ble_match.h"
#include "services/token_bcast.h"
#include "services/profile_bcast.h"
#include "services/profile_batch_bcast.h"
#include "services/tone_presets.h"
#if PERIPH_DISPLAY
#include "services/jpeg_rx.h"
#include "services/screen_capture.h"
#include "platform/platform_screen_capture.h"
#endif
#endif

#if PERIPH_BLE
static profile_data_t s_profile;
#endif

#if PERIPH_BLE
static hal_config_t *s_cfg;
static hal_kv_t *s_kv;
static hal_identity_t *s_id;
static token_bcast_t s_tb;
static profile_bcast_t s_pb;
static profile_batch_bcast_t s_pbb;
#define IDENT_BLOB_SIZE 72
static uint8_t s_ident_blob[IDENT_BLOB_SIZE];

// 组装并推送身份只读 blob(BLE 特征 …0012)。
// v2: [0]ver=2 [1]flags [2..25]SN [26..57]全零 [58..65]HwVer [66..71]保留。
// bit0=已 provision, bit1=Key 已隐藏。绑定须扫描本机二维码，不经 BLE 读取密钥。
// 小程序必须查 flags 判断是否激活,不能用"SN 非空"—— SN 永远非空。
//
// 固定长度和公开字段偏移不变；旧客户端不得将全零 Key 当成未 provision。
static void publish_identity(void) {
    if (!s_cfg || !s_id) return;
    memset(s_ident_blob, 0, sizeof s_ident_blob);
    s_ident_blob[0] = 0x02;
    s_ident_blob[1] = 0x02 | (hal_identity_is_provisioned(s_id) ? 0x01 : 0x00);

    char tmp[40];
    if (hal_identity_get_sn(s_id, tmp, sizeof tmp) > 0)
        memcpy(&s_ident_blob[2], tmp, strnlen(tmp, 24));
    if (hal_identity_get_hw_ver(s_id, tmp, sizeof tmp) > 0)
        memcpy(&s_ident_blob[58], tmp, strnlen(tmp, 8));

    hal_config_set_identity(s_cfg, s_ident_blob, IDENT_BLOB_SIZE);
}
// 游戏积分只读 blob(BLE 特征 …0013): [ver=1][game_total u32 LE][game_best u32 LE] = 9 字节。
static uint8_t s_game_blob[9];
static void publish_gamescore(void) {
    if (!s_cfg) return;
    uint32_t total = (uint32_t)(s_profile.game_total < 0 ? 0 : s_profile.game_total);
    uint32_t best  = (uint32_t)(s_profile.game_best  < 0 ? 0 : s_profile.game_best);
    s_game_blob[0] = 0x01;
    s_game_blob[1] = total & 0xFF; s_game_blob[2] = (total >> 8) & 0xFF;
    s_game_blob[3] = (total >> 16) & 0xFF; s_game_blob[4] = (total >> 24) & 0xFF;
    s_game_blob[5] = best & 0xFF; s_game_blob[6] = (best >> 8) & 0xFF;
    s_game_blob[7] = (best >> 16) & 0xFF; s_game_blob[8] = (best >> 24) & 0xFF;
    hal_config_set_gamescore(s_cfg, s_game_blob, sizeof s_game_blob);
}
// 供 provision_console 在 AT+CARDID= 写入成功后回调,刷新 BLE 身份 blob。
static void on_identity_changed(void *user) { (void)user; publish_identity(); }
static atomic_flag preference_busy = ATOMIC_FLAG_INIT;
static bool save_profile(void) {
    if (!s_kv) return false;
    uint8_t blob[PROFILE_BLOB_SIZE];
    int n = profile_serialize(&s_profile, blob, sizeof blob);
    return n > 0 && hal_kv_set(s_kv, "profile", blob, n) == 0;
}
// 统一回报:每个下发动作最终都必须有一条 DONE 或 0x1X 错误,小程序才能确定状态。
static void notify_status(uint8_t type, uint8_t st) {
    if (s_cfg) hal_config_notify_status(s_cfg, type, st);
    else ESP_LOGE(TAG, "s_cfg 为空,无法回通知 type=0x%02X st=0x%02X", type, st);
}
#if PERIPH_DISPLAY
static jpeg_rx_t s_jpeg_rx;
static int jsink_begin(void *u, uint32_t t) {
    (void)u;
    return jpeg_store_begin(s_profile.img_mode == IMG_MODE_AVATAR ? JPEG_SLOT_AVATAR : JPEG_SLOT_FULL, t);
}
static int jsink_write(void *u, const uint8_t *d, int n) { (void)u; return jpeg_store_write(d, n); }
static int jsink_end(void *u) { (void)u; return jpeg_store_end(); }
static const jpeg_rx_sink_t s_jsink = { jsink_begin, jsink_write, jsink_end, NULL };

// 截图协议由一个专用任务串行化。NimBLE host 回调只入队控制命令,
// 或对已公布的 (capture_id, seq) 做无阻塞 ACK 唤醒;不等待重绘或传输。
#define CAPTURE_CMD_QUEUE_LEN 4u
#define CAPTURE_ACK_TIMEOUT_MS 3000u
#define CAPTURE_RESULT_TTL_MS (5u * 60u * 1000u)
#define CAPTURE_TASK_STACK_BYTES 6144u

typedef enum {
    CAP_WAIT_IDLE = 0,
    CAP_WAIT_ACK,
    CAP_WAIT_ACKED,
    CAP_WAIT_CANCELLED,
} capture_wait_state_t;

typedef enum {
    CAP_CANCEL_NONE = 0,
    CAP_CANCEL_USER,
    CAP_CANCEL_DISCONNECT,
} capture_cancel_reason_t;

typedef struct {
    uint8_t op;
    uint16_t capture_id;
    uint32_t session;
} capture_cmd_t;

static screen_capture_t s_capture;
static QueueHandle_t s_capture_cmd_q;
static SemaphoreHandle_t s_capture_ack_sem;
// Protect the empty-queue / worker-exit handoff. NimBLE never waits on it.
static SemaphoreHandle_t s_capture_worker_lock;
static bool s_capture_worker_running; // accessed only with worker_lock held
static atomic_bool s_capture_ready;
static atomic_bool s_capture_active;
static atomic_bool s_capture_start_pending;
static atomic_bool s_capture_transport_held;
static atomic_bool s_jpeg_upload_active;
static atomic_uint_fast16_t s_capture_current_id;
static atomic_uint_fast16_t s_capture_wait_id;
static atomic_uint_fast16_t s_capture_wait_seq;
static atomic_uint_fast16_t s_capture_result_id;
static atomic_uint_fast32_t s_capture_result_deadline_ms;
static atomic_uint_fast32_t s_capture_session;
static atomic_int s_capture_wait_state;
static atomic_int s_capture_cancel_reason;
static atomic_uint s_profile_ui_pending;
static atomic_bool s_config_connected;
static uint16_t s_capture_id_counter;
static bool s_capture_redraw_done;
static bool s_capture_redraw_ok;
static uint8_t s_capture_failure_status;

// lvgl_port_lock(0) 是无限等待,不是 try-lock。截图同步重绘期间 transfer task
// 持有递归 LVGL 锁并等待 NimBLE ACK,所以 host/input 回调必须在碰锁前退出。
static bool capture_ui_blocked(void) {
    return atomic_load(&s_capture_start_pending) ||
           atomic_load(&s_capture_active) ||
           platform_screen_capture_active();
}

#define PROFILE_UI_REFRESH_NAME  (1u << 0)
#define PROFILE_UI_REFRESH_TITLE (1u << 1)
#define PROFILE_UI_REFRESH_INTRO (1u << 2)

// 档案广播可能在截图任务独占 LVGL 时到达。值已先落 NVS；这里在截图释放
// LVGL 后补刷最终 profile，多个广播只合并 dirty 位，不会丢掉最新值。
static void profile_ui_flush_pending(void) {
    if (capture_ui_blocked()) return;
    unsigned pending = atomic_load(&s_profile_ui_pending);
    if (pending == 0 || !platform_lvgl_lock(10)) return;
    pending = atomic_exchange(&s_profile_ui_pending, 0);
    if (pending & PROFILE_UI_REFRESH_NAME) ui_profile_set_name(s_profile.name);
    if (pending & PROFILE_UI_REFRESH_TITLE) ui_profile_set_title(s_profile.role);
    if (pending & PROFILE_UI_REFRESH_INTRO) ui_profile_set_intro(s_profile.subtitle);
    platform_lvgl_unlock();
}

static void capture_clear_start_pending(void) {
    atomic_store(&s_capture_start_pending, false);
    profile_ui_flush_pending();
}

static uint32_t capture_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool capture_deadline_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void capture_clear_result(void) {
    atomic_store(&s_capture_result_id, 0);
    atomic_store(&s_capture_result_deadline_ms, 0);
}

static bool capture_transport_hold(bool hold) {
    if (hold) {
        if (atomic_load(&s_capture_transport_held)) return true;
        if (!hal_config_capture_transport_hold(s_cfg, true)) return false;
        atomic_store(&s_capture_transport_held, true);
        return true;
    }
    if (!atomic_exchange(&s_capture_transport_held, false)) return true;
    return hal_config_capture_transport_hold(s_cfg, false);
}

static bool capture_result_matches(uint16_t capture_id) {
    uint16_t id = (uint16_t)atomic_load(&s_capture_result_id);
    if (capture_id == 0 || id != capture_id) return false;
    uint32_t deadline = (uint32_t)atomic_load(&s_capture_result_deadline_ms);
    if (capture_deadline_reached(capture_now_ms(), deadline)) {
        uint_fast16_t expected = id;
        (void)atomic_compare_exchange_strong(&s_capture_result_id, &expected, 0);
        return false;
    }
    return true;
}

// Task 3 的同步 region callback 可能正卡在 ACK 等待。顺序不能反:
// 先给本层信号量,再 cancel 平台层,才能让 LVGL flush 安全退栈。
static void capture_request_cancel(capture_cancel_reason_t reason) {
    if (reason == CAP_CANCEL_DISCONNECT) {
        atomic_store(&s_capture_cancel_reason, CAP_CANCEL_DISCONNECT);
    } else {
        int expected = CAP_CANCEL_NONE;
        (void)atomic_compare_exchange_strong(&s_capture_cancel_reason, &expected,
                                             CAP_CANCEL_USER);
    }
    atomic_store(&s_capture_wait_state, CAP_WAIT_CANCELLED);
    if (s_capture_ack_sem) xSemaphoreGive(s_capture_ack_sem);
    platform_screen_capture_cancel();
}

static bool capture_signal_matching_ack(uint16_t capture_id, uint16_t seq) {
    if (!atomic_load(&s_capture_active) ||
        capture_id != (uint16_t)atomic_load(&s_capture_current_id) ||
        capture_id != (uint16_t)atomic_load(&s_capture_wait_id) ||
        seq != (uint16_t)atomic_load(&s_capture_wait_seq)) {
        return false;
    }
    int expected = CAP_WAIT_ACK;
    if (!atomic_compare_exchange_strong(&s_capture_wait_state, &expected,
                                        CAP_WAIT_ACKED)) {
        return false;
    }
    xSemaphoreGive(s_capture_ack_sem);
    return true;
}

static bool capture_publish_wait(uint16_t capture_id, uint16_t seq) {
    while (xSemaphoreTake(s_capture_ack_sem, 0) == pdTRUE) { }
    atomic_store(&s_capture_wait_id, capture_id);
    atomic_store(&s_capture_wait_seq, seq);
    int expected = CAP_WAIT_IDLE;
    return atomic_compare_exchange_strong(&s_capture_wait_state, &expected,
                                          CAP_WAIT_ACK);
}

typedef enum {
    CAP_ACK_MATCHED,
    CAP_ACK_TIMEOUT,
    CAP_ACK_CANCELLED,
} capture_ack_result_t;

static capture_ack_result_t capture_wait_for_ack(void) {
    if (xSemaphoreTake(s_capture_ack_sem,
                       pdMS_TO_TICKS(CAPTURE_ACK_TIMEOUT_MS)) != pdTRUE) {
        int expected = CAP_WAIT_ACK;
        if (atomic_compare_exchange_strong(&s_capture_wait_state, &expected,
                                           CAP_WAIT_IDLE)) {
            return CAP_ACK_TIMEOUT;
        }
        // ACK/CANCEL 在超时边界先抢到状态即算有效;下轮发包会清理滞留 give。
    }
    int state = atomic_exchange(&s_capture_wait_state, CAP_WAIT_IDLE);
    return state == CAP_WAIT_ACKED ? CAP_ACK_MATCHED : CAP_ACK_CANCELLED;
}

static bool capture_send_current_packet(void) {
    uint8_t packet[SC_PACKET_CACHE_BYTES];
    for (;;) {
        size_t packet_len = 0;
        uint16_t mtu = hal_config_get_mtu_payload(s_cfg);
        sc_packet_result_t packet_result = screen_capture_next_packet(
            &s_capture, mtu, packet, sizeof packet, &packet_len);
        if (packet_result != SC_PACKET_READY) {
            s_capture_failure_status = CFG_ST_ERR_SEQ;
            return false;
        }

        uint16_t capture_id = s_capture.capture_id;
        uint16_t seq = s_capture.seq;
        if (!capture_publish_wait(capture_id, seq)) return false;
        if (!hal_config_notify_capture(s_cfg, packet, packet_len)) {
            int expected = CAP_WAIT_ACK;
            (void)atomic_compare_exchange_strong(&s_capture_wait_state, &expected,
                                                 CAP_WAIT_IDLE);
            s_capture_failure_status = CFG_ST_ERR_NOT_READY;
            return false;
        }

        capture_ack_result_t ack = capture_wait_for_ack();
        if (ack == CAP_ACK_MATCHED) {
            if (!screen_capture_accept_ack(&s_capture, capture_id, seq)) {
                s_capture_failure_status = CFG_ST_ERR_SEQ;
                return false;
            }
            return true;
        }
        if (ack == CAP_ACK_CANCELLED) return false;
        if (!screen_capture_timeout(&s_capture)) {
            s_capture_failure_status = CFG_ST_ERR_TIMEOUT;
            return false;
        }
        // screen_capture_timeout 授权一次缓存包重发;这里精确实现 3s + 最多 3 次。
    }
}

static bool on_capture_region(int x, int y, int w, int h,
                              const uint8_t *rgb565, void *user) {
    (void)user;
    if (atomic_load(&s_capture_cancel_reason) != CAP_CANCEL_NONE ||
        x < 0 || y < 0 || w <= 0 || h <= 0) {
        return false;
    }
    size_t rgb565_len = (size_t)w * (size_t)h * 2u;
    if (!screen_capture_begin_region(&s_capture, (uint16_t)x, (uint16_t)y,
                                     (uint16_t)w, (uint16_t)h, rgb565,
                                     rgb565_len)) {
        s_capture_failure_status = CFG_ST_ERR_DECODE;
        return false;
    }
    while (s_capture.region_active) {
        if (!capture_send_current_packet()) return false;
    }
    return true;
}

static void on_capture_redraw_done(bool ok, void *user) {
    (void)user;
    s_capture_redraw_done = true;
    s_capture_redraw_ok = ok;
}

static void capture_abort_active(uint8_t status, bool send_status) {
    // 所有失败路径都重复执行安全的唤醒→平台 cancel→核心 cancel顺序。
    atomic_store(&s_capture_wait_state, CAP_WAIT_CANCELLED);
    if (s_capture_ack_sem) xSemaphoreGive(s_capture_ack_sem);
    platform_screen_capture_cancel();
    screen_capture_cancel(&s_capture);
    (void)capture_transport_hold(false);
    atomic_store(&s_capture_active, false);
    profile_ui_flush_pending();
    atomic_store(&s_capture_current_id, 0);
    atomic_store(&s_capture_wait_id, 0);
    atomic_store(&s_capture_wait_seq, 0);
    atomic_store(&s_capture_wait_state, CAP_WAIT_IDLE);
    atomic_store(&s_capture_cancel_reason, CAP_CANCEL_NONE);
    capture_clear_result();
    if (send_status) notify_status(CFG_MSG_SCREEN_CAPTURE, status);
}

// END 已 ACK 但结果记录尚在发布时,CANCEL 仍会走 active 分支。
// 这个检查必须把已接受的用户 CANCEL 映射为 DONE,不能像断连一样静默。
static bool capture_end_interrupted(uint32_t session) {
    capture_cancel_reason_t reason =
        (capture_cancel_reason_t)atomic_load(&s_capture_cancel_reason);
    if (session == (uint32_t)atomic_load(&s_capture_session) &&
        reason == CAP_CANCEL_NONE) {
        return false;
    }
    uint8_t status = reason == CAP_CANCEL_USER ? CFG_ST_DONE : CFG_ST_ERR_NOT_READY;
    capture_abort_active(status, reason == CAP_CANCEL_USER);
    return true;
}

static uint16_t capture_next_id(void) {
    if (++s_capture_id_counter == 0) ++s_capture_id_counter;
    return s_capture_id_counter;
}

static void capture_run_start(uint32_t session) {
    if (session != (uint32_t)atomic_load(&s_capture_session)) {
        capture_clear_start_pending();
        return;
    }
    if (!hal_config_capture_subscribed(s_cfg)) {
        capture_clear_start_pending();
        notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_NOT_READY);
        return;
    }
    if (atomic_load(&s_capture_active) || platform_screen_capture_active() ||
        atomic_load(&s_jpeg_upload_active) || jpeg_view_decode_busy()) {
        capture_clear_start_pending();
        notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
        return;
    }

    uint16_t capture_id = capture_next_id();
    if (!screen_capture_start(&s_capture, capture_id)) {
        capture_clear_start_pending();
        notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
        return;
    }
    if (!capture_transport_hold(true)) {
        capture_clear_start_pending();
        capture_abort_active(CFG_ST_ERR_NOT_READY, true);
        return;
    }
    capture_clear_result();
    atomic_store(&s_capture_cancel_reason, CAP_CANCEL_NONE);
    atomic_store(&s_capture_wait_state, CAP_WAIT_IDLE);
    atomic_store(&s_capture_current_id, capture_id);
    atomic_store(&s_capture_active, true);
    capture_clear_start_pending();
    s_capture_failure_status = CFG_ST_ERR_DECODE;
    if (session != (uint32_t)atomic_load(&s_capture_session)) {
        capture_abort_active(CFG_ST_ERR_NOT_READY, false);
        return;
    }
    notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ACK);

    // META 必须在强制重绘前完成 ACK,否则第一个 region 还不能进入 DATA。
    if (!capture_send_current_packet()) {
        capture_cancel_reason_t reason =
            (capture_cancel_reason_t)atomic_load(&s_capture_cancel_reason);
        capture_abort_active(reason == CAP_CANCEL_USER ? CFG_ST_DONE : s_capture_failure_status,
                             reason != CAP_CANCEL_DISCONNECT);
        return;
    }

    s_capture_redraw_done = false;
    s_capture_redraw_ok = false;
    bool start_called = platform_screen_capture_start();
    capture_cancel_reason_t reason =
        (capture_cancel_reason_t)atomic_load(&s_capture_cancel_reason);
    if (!start_called || !s_capture_redraw_done || !s_capture_redraw_ok ||
        reason != CAP_CANCEL_NONE) {
        uint8_t status = reason == CAP_CANCEL_USER ? CFG_ST_DONE
                         : !start_called ? CFG_ST_ERR_BUSY
                                         : s_capture_failure_status;
        capture_abort_active(status, reason != CAP_CANCEL_DISCONNECT);
        return;
    }
    if (!screen_capture_finish(&s_capture, NULL) || !capture_send_current_packet()) {
        reason = (capture_cancel_reason_t)atomic_load(&s_capture_cancel_reason);
        capture_abort_active(reason == CAP_CANCEL_USER ? CFG_ST_DONE : s_capture_failure_status,
                             reason != CAP_CANCEL_DISCONNECT);
        return;
    }
    (void)capture_transport_hold(false);

    // 先公布结果记录,再撤 active; END ACK 后紧跟的 FINISH/CANCEL 不会掉进窗口。
    atomic_store(&s_capture_result_deadline_ms, capture_now_ms() + CAPTURE_RESULT_TTL_MS);
    atomic_store(&s_capture_result_id, capture_id);
    if (capture_end_interrupted(session)) return;
    // END ACK 到达时 Task 3 已完成重绘退栈并恢复原导航状态。
    atomic_store(&s_capture_active, false);
    profile_ui_flush_pending();
    if (capture_end_interrupted(session)) return;
    atomic_store(&s_capture_current_id, 0);
    atomic_store(&s_capture_wait_id, 0);
    atomic_store(&s_capture_wait_seq, 0);
    atomic_store(&s_capture_wait_state, CAP_WAIT_IDLE);
    atomic_store(&s_capture_cancel_reason, CAP_CANCEL_NONE);
    notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_DONE);
}

static void capture_transfer_task(void *arg) {
    (void)arg;
    for (;;) {
        capture_cmd_t cmd;
        xSemaphoreTake(s_capture_worker_lock, portMAX_DELAY);
        bool have_command = xQueueReceive(s_capture_cmd_q, &cmd, 0) == pdTRUE;
        if (!have_command) s_capture_worker_running = false;
        xSemaphoreGive(s_capture_worker_lock);
        if (!have_command) {
            // The result is only two atomics; capture_result_matches expires it
            // on access. Do not retain a 6 KiB stack for a five-minute receipt.
            vTaskDelete(NULL);
            return;
        }
        // 断连前已入队的 START/FINISH 不能在新连接上重放。
        if (cmd.session != (uint32_t)atomic_load(&s_capture_session)) {
            if (cmd.op == SC_OP_START) capture_clear_start_pending();
            continue;
        }
        if (cmd.op == SC_OP_START) {
            capture_run_start(cmd.session);
        } else if ((cmd.op == SC_OP_FINISH || cmd.op == SC_OP_CANCEL) &&
                   capture_result_matches(cmd.capture_id)) {
            // END ACK 后 FINISH/CANCEL 只清理 5 分钟结果记录,不再碰显示。
            capture_clear_result();
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_DONE);
        } else {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_SEQ);
        }
    }
}

static bool capture_queue_command(uint8_t op, uint16_t capture_id) {
    capture_cmd_t cmd = {
        .op = op,
        .capture_id = capture_id,
        .session = (uint32_t)atomic_load(&s_capture_session),
    };
    if (!s_capture_cmd_q || !s_capture_worker_lock ||
        xSemaphoreTake(s_capture_worker_lock, 0) != pdTRUE) return false;
    bool queued = xQueueSend(s_capture_cmd_q, &cmd, 0) == pdTRUE;
    if (queued && !s_capture_worker_running) {
        s_capture_worker_running = true;
        if (xTaskCreate(capture_transfer_task, "capture", CAPTURE_TASK_STACK_BYTES,
                        NULL, 5, NULL) != pdPASS) {
            // With no worker, this was the only queued command. Never leave a
            // failed START behind to run unexpectedly on a later request.
            xQueueReset(s_capture_cmd_q);
            s_capture_worker_running = false;
            queued = false;
        }
    }
    xSemaphoreGive(s_capture_worker_lock);
    return queued;
}

static bool capture_screen_allowed(void) {
    // Same LVGL lock as redraw: no check-to-capture race exposing the QR Key.
    return !ui_button_guide_active();
}

static void capture_orchestration_init(struct _lv_display_t *lvdisp) {
    screen_capture_init(&s_capture, SC_WIDTH, SC_HEIGHT);
    atomic_init(&s_capture_ready, false);
    atomic_init(&s_capture_active, false);
    atomic_init(&s_capture_start_pending, false);
    atomic_init(&s_capture_transport_held, false);
    atomic_init(&s_jpeg_upload_active, false);
    atomic_init(&s_capture_current_id, 0);
    atomic_init(&s_capture_wait_id, 0);
    atomic_init(&s_capture_wait_seq, 0);
    atomic_init(&s_capture_result_id, 0);
    atomic_init(&s_capture_result_deadline_ms, 0);
    atomic_init(&s_capture_session, 1);
    atomic_init(&s_capture_wait_state, CAP_WAIT_IDLE);
    atomic_init(&s_capture_cancel_reason, CAP_CANCEL_NONE);
    atomic_init(&s_profile_ui_pending, 0);
    s_capture_cmd_q = xQueueCreate(CAPTURE_CMD_QUEUE_LEN, sizeof(capture_cmd_t));
    s_capture_ack_sem = xSemaphoreCreateBinary();
    s_capture_worker_lock = xSemaphoreCreateMutex();
    s_capture_worker_running = false;
    if (!s_capture_cmd_q || !s_capture_ack_sem || !s_capture_worker_lock || !lvdisp) {
        ESP_LOGE(TAG, "截图调度资源创建失败 queue=%p ack=%p display=%p",
                 (void *)s_capture_cmd_q, (void *)s_capture_ack_sem, (void *)lvdisp);
        return;
    }
    platform_screen_capture_set_policy(capture_screen_allowed);
    platform_screen_capture_init(lvdisp, on_capture_region, on_capture_redraw_done, NULL);
    atomic_store(&s_capture_ready, true);
    ESP_LOGI(TAG, "截图调度 queue=%p ack=%p task=按需创建 state=%uB",
             (void *)s_capture_cmd_q, (void *)s_capture_ack_sem,
             (unsigned)sizeof s_capture);
}
#endif
#if PERIPH_AUDIO
// 1024 足够:RTTTL 是极紧凑的文本(实测整首马里奥才几百字节),原来 4096×2 是过度分配。
#define SCORE_MAX 1024
#define BOOT_RTTTL_KV_KEY "boot_rtttl"
static uint8_t s_score_rx_buf[SCORE_MAX];
static uint8_t s_score_play_buf[SCORE_MAX];
static int s_score_play_len;
// 静态初始化 buf/cap:不依赖 app_run 里的 score_rx_init 执行时机,
// 避免万一初始化未跑到导致 cap=0、任何长度都被判"超长"。
static score_rx_t s_score_rx = { .buf = s_score_rx_buf, .cap = SCORE_MAX };
static SemaphoreHandle_t s_music_sig;
static SemaphoreHandle_t s_music_idle_sig;
static SemaphoreHandle_t s_audio_owner;
static volatile int s_music_interrupt;
static atomic_bool s_music_playing;
static audio_service_t s_au;         // 音频业务(on_cfg_message 的音量处理要用,故提前声明)
#endif

static bool has_user_configured_content(void) {
    bool name = strcmp(s_profile.name, "FoloToy") != 0;
    bool intro = s_profile.subtitle[0] != '\0';
    bool avatar = false;
    bool photo = false;
    bool audio = false;
#if PERIPH_DISPLAY
    if (s_profile.avatar_name[0] == '\0') {
        avatar = jpeg_store_has_valid(JPEG_SLOT_AVATAR);
    } else {
        char initial_avatar[16] = {0};
        bool is_initial = strcmp(s_profile.avatar_name, "default") == 0;
        if (avatar_store_first_name(initial_avatar) == 0 &&
            strcmp(s_profile.avatar_name, initial_avatar) == 0) {
            is_initial = true;
        }
        avatar = !is_initial;
    }
    photo = jpeg_store_has_valid(JPEG_SLOT_FULL);
#else
    avatar = strcmp(s_profile.avatar_name, "default") != 0;
#endif
#if PERIPH_AUDIO
    int got = 0;
    if (s_kv && hal_kv_get(s_kv, BOOT_RTTTL_KV_KEY, s_score_play_buf,
                           sizeof s_score_play_buf, &got) == 0 && got > 0) {
        audio = rtttl_validate((const char *)s_score_play_buf, got) == 0;
    }
#endif
    ESP_LOGI(TAG, "用户内容判定 avatar=%d name=%d intro=%d audio=%d photo=%d",
             avatar, name, intro, audio, photo);
    return avatar || name || intro || audio || photo;
}

#if PERIPH_DISPLAY
static void on_ble_connected(void *user) {
    (void)user;
    atomic_store(&s_config_connected, true);
    ui_profile_set_ble_connected(true);
    app_plays_config_connected();
}
// NimBLE host task: ACK 信号量必须先唤醒,再取消同步 LVGL 捕获。
static void on_ble_disconnected(void *user) {
    (void)user;
    atomic_store(&s_config_connected, false);
    ui_profile_set_ble_connected(false);
    (void)atomic_fetch_add(&s_capture_session, 1);
    capture_clear_result();
    // JPEG 分片与断连回调同在 NimBLE host task,可直接丢弃半包状态。
    // begin 已擦除 header,所以未 END 的 flash 数据本来就不可见,无额外存储资源要释放。
    jpeg_rx_init(&s_jpeg_rx);
    atomic_store(&s_jpeg_upload_active, false);
    if (atomic_load(&s_capture_active)) {
        capture_request_cancel(CAP_CANCEL_DISCONNECT);
    }
}

static void on_capture_control(const sc_control_t *control) {
    if (!atomic_load(&s_capture_ready)) {
        notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_NOT_READY);
        return;
    }
    if (control->op == SC_OP_START) {
        if (atomic_load(&s_capture_active)) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
            return;
        }
        bool expected = false;
        if (!atomic_compare_exchange_strong(&s_capture_start_pending, &expected, true)) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
            return;
        }
        if (!capture_queue_command(SC_OP_START, 0)) {
            capture_clear_start_pending();
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
        }
        return;
    }
    if (control->op == SC_OP_ACK) {
        if (!capture_signal_matching_ack(control->capture_id, control->seq)) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_SEQ);
        }
        return;
    }
    if ((control->op == SC_OP_FINISH || control->op == SC_OP_CANCEL) &&
        capture_result_matches(control->capture_id)) {
        if (!capture_queue_command(control->op, control->capture_id)) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_BUSY);
        }
        return;
    }
    if (control->op == SC_OP_CANCEL && atomic_load(&s_capture_active)) {
        if (control->capture_id != (uint16_t)atomic_load(&s_capture_current_id)) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_SEQ);
        } else {
            capture_request_cancel(CAP_CANCEL_USER);
        }
        return;
    }
    notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_SEQ);
}
#endif

// BLE 消息回调(NimBLE host task)。type=JSON → 部分更新 → 存 NVS → 刷新 UI → notify;
// type=JPG(0x02) → 分片写 flash → 收齐则请求全屏看图。
static void on_cfg_message(uint8_t type, const uint8_t *payload, int len, void *user) {
    (void)user;
    if (atomic_load(&s_sleep_preparing) || atomic_load(&s_factory_command_active)) {
        notify_status(type, CFG_ST_ERR_BUSY);
        return;
    }
    note_activity();   // 收到蓝牙连接数据:算活动,复位息屏计时(并唤醒)
    // 批量分片(图片0x02/乐谱0x03/音频0x04/0x05)逐帧刷屏 → 降 DEBUG;仅配置(0x01)保留 INFO。
    // 传输的最终结果由后续"JSON 解析结果 / JPG 显示结果 / 乐谱收齐"等日志给出。
    if (type == 0x01) ESP_LOGI(TAG, "◀ 消息 type=0x01 payload=%d 字节", len);
    else              ESP_LOGD(TAG, "◀ 消息 type=0x%02X payload=%d 字节", type, len);
#if PERIPH_DISPLAY
    if (type == CFG_MSG_SCREEN_CAPTURE) {
        sc_control_t control;
        if (screen_capture_parse_control(payload, (size_t)(len < 0 ? 0 : len), &control) !=
            SC_PARSE_OK) {
            notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_ERR_FRAME);
        } else {
            on_capture_control(&control);
        }
        return;
    }
    if (type == CFG_MSG_JPEG) {   // JPG 分片(BEGIN/DATA/END)
        // BEGIN 会擦掉整个 imgstore 分区,而解码任务可能正 mmap 着它读。
        // 两者跑在不同任务(解码在 LVGL 任务,本函数在 NimBLE host 任务)且无锁,
        // 放行的话就是把解码器脚下的数据抽走 —— 现象是 JPEG SOS 段解析失败,
        // 看起来像"图片损坏",但数据在解码开始时明明是好的,极难定位。
        // 宁可让发送方等一下重试。解码通常几百毫秒。
        if (len >= 1 && payload[0] == JPEG_RX_OP_BEGIN &&
            (jpeg_view_decode_busy() || atomic_load(&s_capture_active) ||
             atomic_load(&s_capture_start_pending))) {
            ESP_LOGW(TAG, "显示解码/截图正忙,拒绝新的图片传输(请稍后重试)");
            notify_status(CFG_MSG_JPEG, CFG_ST_ERR_BUSY);
            return;
        }
        int done = 0;
        int st = jpeg_rx_frame(&s_jpeg_rx, payload, len, &s_jsink, &done);
        atomic_store(&s_jpeg_upload_active, s_jpeg_rx.receiving != 0);
        uint8_t ns = (st == 0) ? CFG_ST_ACK
                   : (st == 1) ? CFG_ST_ERR_SEQ
                   : (st == 2) ? CFG_ST_ERR_STORAGE
                               : CFG_ST_ERR_TOO_LONG;
        if (st != 0) ESP_LOGW(TAG, "JPG 分片错误 rx=%d → 状态0x%02X", st, ns);
        notify_status(CFG_MSG_JPEG, ns);
        if (done) {
            if (s_profile.img_mode == IMG_MODE_AVATAR) {
                s_profile.avatar_name[0] = '\0';
                if (!save_profile()) ESP_LOGW(TAG, "用户头像来源标记保存失败");
                ESP_LOGI(TAG, "头像图片已收齐,请求头像位显示");
                jpeg_view_request_mode(JPEG_VIEW_AVATAR);
            } else {
                ESP_LOGI(TAG, "全屏图片已收齐,请求全屏显示");
                jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
            }
        }
        return;
    }
#endif
#if PERIPH_AUDIO
    if (type == CFG_MSG_SCORE) {   // RTTTL 乐谱分片
        int done = 0;
        int st = score_rx_frame(&s_score_rx, payload, len, &done);
        uint8_t ns = (st == 0) ? CFG_ST_ACK
                   : (st == 1) ? CFG_ST_ERR_SEQ
                               : CFG_ST_ERR_TOO_LONG;
        if (done && ns == CFG_ST_ACK) {
            if (rtttl_validate((const char *)s_score_rx_buf, (int)s_score_rx.written) < 0) {
                ns = CFG_ST_ERR_PARSE;
                ESP_LOGW(TAG, "RTTTL 格式非法,拒绝播放");
            } else if (!s_music_sig) {
                ns = CFG_ST_ERR_STORAGE;
                ESP_LOGE(TAG, "播放任务未就绪");
            } else if (!s_kv || hal_kv_set(s_kv, BOOT_RTTTL_KV_KEY,
                                           s_score_rx_buf, (int)s_score_rx.written) != 0) {
                ns = CFG_ST_ERR_STORAGE;
                ESP_LOGE(TAG, "RTTTL 持久化失败,拒绝更新开机音乐");
            } else {
                memcpy(s_score_play_buf, s_score_rx_buf, s_score_rx.written);
                s_score_play_len = (int)s_score_rx.written;
                s_music_interrupt = 1;                 // 抢占当前曲
                xSemaphoreGive(s_music_sig);
                ns = CFG_ST_DONE;                      // 已开始播放
                ESP_LOGI(TAG, "乐谱收齐(%d 字节),已保存为开机音乐并开始播放", s_score_play_len);
            }
        }
        if (ns == CFG_ST_ERR_TOO_LONG && len >= 3 && payload[0] == 0x00) {
            uint32_t le = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8);   // 按协议(小端)解析
            uint32_t be = (uint32_t)payload[2] | ((uint32_t)payload[1] << 8);   // 若对方写成大端
            ESP_LOGW(TAG, "乐谱 BEGIN 声明长度=%u(大端解则 %u);实际缓冲 cap=%d buf=%p。"
                          "cap 若为 0 说明缓冲未初始化;否则请检查小程序 total_len 是否为【2 字节小端】",
                     (unsigned)le, (unsigned)be, s_score_rx.cap, (void *)s_score_rx.buf);
        } else if (ns >= CFG_ST_ERR_FRAME) {
            ESP_LOGW(TAG, "乐谱错误 状态0x%02X", ns);
        }
        notify_status(CFG_MSG_SCORE, ns);
        return;
    }
#endif
    if (type != CFG_MSG_JSON) {
        ESP_LOGW(TAG, "不支持的消息类型 0x%02X", type);
        notify_status(type, CFG_ST_ERR_UNSUPPORTED);   // 未知类型也要回应,不能静默丢弃
        return;
    }
    ESP_LOGI(TAG, "JSON 内容: %.*s", len, (const char *)payload);
    cfg_changed_t chg;
    profile_data_t old_profile = s_profile;
    char old_avatar[sizeof s_profile.avatar_name];
    memcpy(old_avatar, s_profile.avatar_name, sizeof old_avatar);
    int n = config_json_apply(&s_profile, (const char *)payload, len, &chg);
    uint8_t status = (n > 0) ? CFG_ST_DONE
                   : (n == 0) ? CFG_ST_ERR_NO_FIELD
                              : CFG_ST_ERR_PARSE;
    ESP_LOGI(TAG, "JSON 解析结果: 生效字段=%d%s", n,
             n < 0 ? "(解析失败)" : (n == 0 ? "(无已知键)" : ""));
    if (n > 0) {
        bool avatar_ok = true;
        if (chg.avatar_name && !avatar_store_has(s_profile.avatar_name)) {
            memcpy(s_profile.avatar_name, old_avatar, sizeof s_profile.avatar_name);
            avatar_ok = false;
            status = CFG_ST_ERR_AVATAR;
            ESP_LOGW(TAG, "内置头像不存在,保留 '%s'", old_avatar);
        }
        if (chg.game_clear) {
            s_profile.game_total = 0;
            s_profile.game_best = 0;
        }
        // 仅当有需持久化的字段变化时才写 NVS —— 避免频繁 time 同步磨损 flash。
        bool persist = chg.name || chg.role || chg.subtitle || chg.battery || chg.level ||
                       chg.online || chg.token || chg.token_max || chg.sleep_min ||
                       chg.volume || chg.game_clear || chg.img_mode ||
                       (chg.avatar_name && avatar_ok);
        if (persist && !save_profile()) {
            // A failed command must not leave unsaved values active or publish
            // them through UI/audio/score callbacks. Let the client retry.
            s_profile = old_profile;
            ESP_LOGW(TAG, "NVS 保存失败，保留原设置");
            notify_status(CFG_MSG_JSON, CFG_ST_ERR_STORAGE);
            return;
        }
        // 时间同步:写系统时钟(由 RTC 计时,跨深度休眠保留;精度受内部 RC 限制,详见协议)。
        // 时间是实时值,**不持久化**(存进 NVS 也会过期)。
        if (chg.time) {
            struct timeval tv = { .tv_sec = (time_t)chg.time_epoch, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "系统时钟已同步: epoch=%lld", (long long)chg.time_epoch);
        }
        // 游戏积分清零:小程序读走 …0013 后下发,清零工牌端累计/最高,并刷新只读特征。
        if (chg.game_clear) {
            publish_gamescore();
            ESP_LOGI(TAG, "游戏积分已清零(小程序同步后)");
        }
#if PERIPH_AUDIO
        // 音量:立即应用到 codec(播放时 configure 也会按 s_au.volume 重新应用),并持久化。
        if (chg.volume) {
            audio_service_set_volume(&s_au, s_profile.volume);
            ESP_LOGI(TAG, "codec 音量已设为 %d%%", s_profile.volume);
        }
#endif
#if PERIPH_DISPLAY
        if (chg.avatar_name && avatar_ok)
            jpeg_view_request_avatar(s_profile.avatar_name);
        unsigned ui_pending = (chg.name ? PROFILE_UI_REFRESH_NAME : 0) |
                              (chg.role ? PROFILE_UI_REFRESH_TITLE : 0) |
                              (chg.subtitle ? PROFILE_UI_REFRESH_INTRO : 0);
        atomic_fetch_or(&s_profile_ui_pending, ui_pending);
        profile_ui_flush_pending();
#endif
    }
    notify_status(CFG_MSG_JSON, status);
}
#endif

#if PERIPH_DISPLAY
static hal_display_t *s_disp;
#if PERIPH_BLE
// LVGL-owned one-shot: cold boot and wake wait for the saved avatar (or its
// definitive failure fallback) before lighting the first visible frame.
static struct _lv_display_t *s_wake_display;
static atomic_bool s_boot_avatar_ready;
static void restore_boot_avatar(void);
#endif
#define BRIGHT_PCT 100                    // 产测/无配置时的安全亮度

// 全屏退出时刻(ms)。退出用的"长按确定键"会被 LVGL 导航当成对焦点磁贴(ALBUM)的一次
// 点击释放,从而立刻又请求全屏 → 死循环。用它给全屏重入去抖:退出后极短时间内的
// ALBUM 点击判为误触忽略。跨任务(on_btn_raw 写 / on_dock_action 读)只读写一个 32 位值。
#define VIEW_REENTER_GUARD_MS 1000u
static volatile uint32_t s_view_exit_ms;
static volatile bool s_view_nav_restore_pending;
static volatile bool s_button_guide_active;
static volatile bool s_button_guide_exit;

// Keep navigation disabled until the exit key is physically released.
static void on_plays_exit(void *user) {
    (void)user;
    s_view_exit_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_view_nav_restore_pending = true;
}
static void on_image_screen(void *screen, void *user) {
    (void)user;
    platform_lvgl_nav_enable(false);
    ui_album_attach(screen);
}
static bool can_show_fullscreen(void *user) {
    (void)user;
    return !s_button_guide_active && !app_plays_active() &&
           (ui_profile_is_active() || ui_album_active());
}

// 普通冷启动且五类用户内容均为默认时显示扫码绑定页；按键只负责进入主页。
static void show_boot_button_guide(void) {
    if (atomic_load(&s_config_connected)) { restore_boot_avatar(); return; }
    char sn[HAL_IDENTITY_SN_CAP] = {0}, key[HAL_IDENTITY_KEY_CAP] = {0};
    if (hal_identity_get_sn(s_id, sn, sizeof sn) <= 0 ||
        hal_identity_get_key(s_id, key, sizeof key) <= 0) {
        ESP_LOGW(TAG, "扫码绑定页缺少完整身份,跳过");
        restore_boot_avatar();
        return;
    }
    s_button_guide_exit = false;
    if (platform_lvgl_lock(300)) {
        platform_lvgl_nav_enable(false);
        ui_button_guide_open(sn, key);
        s_button_guide_active = ui_button_guide_active();
        memset(key, 0, sizeof key);
        if (!s_button_guide_active) {
            platform_lvgl_nav_enable(true);
            platform_lvgl_unlock();
            ESP_LOGW(TAG, "扫码绑定页创建失败,保留主页");
            restore_boot_avatar();
            return;
        }
        platform_lvgl_unlock();
    } else {
        ESP_LOGW(TAG, "扫码绑定页打开失败:LVGL 锁超时");
        restore_boot_avatar();
        return;
    }
    // All subsystems are ready. Decode behind the QR, and never expose its
    // underlying home until the stored avatar has a terminal result.
    restore_boot_avatar();
    while (!s_button_guide_exit && !atomic_load(&s_config_connected))
        vTaskDelay(pdMS_TO_TICKS(50));
    while (!atomic_load(&s_boot_avatar_ready)) vTaskDelay(pdMS_TO_TICKS(20));
    bool close_wait_logged = false;
    while (!platform_lvgl_lock(500)) {
        if (!close_wait_logged) {
            ESP_LOGI(TAG, "扫码绑定页已请求退出,等待截图/绘制释放 LVGL 后关闭");
            close_wait_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ui_button_guide_close();
    platform_lvgl_unlock();
    while (platform_button_any_pressed(s_button)) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(80));
    while (!platform_lvgl_lock(500)) vTaskDelay(pdMS_TO_TICKS(50));
    s_button_guide_active = false;
    platform_lvgl_nav_enable(true);
    platform_lvgl_unlock();
    if (close_wait_logged) ESP_LOGI(TAG, "LVGL 已释放,扫码绑定页关闭完成");
    ESP_LOGI(TAG, "扫码绑定页已关闭;下次是否显示由用户内容是否为默认值决定");
    note_activity();
}
#if PERIPH_BLE
// Worker drains completed rounds independently of the lossy raw-input queue.
static void on_game_result(uint32_t total, uint32_t best, uint32_t rounds, void *user) {
    (void)user; (void)rounds;
#if GAME_DEBUG_FIXED_SCORE
    total = rounds > 99999999u / GAME_DEBUG_FIXED_SCORE ? 99999999u : rounds * GAME_DEBUG_FIXED_SCORE;
    best = GAME_DEBUG_FIXED_SCORE;
#endif
    if (!total && !best) return;
    uint32_t previous = s_profile.game_total > 0 ? (uint32_t)s_profile.game_total : 0;
    s_profile.game_total = total > 99999999u - previous ? 99999999 : (int)(previous + total);
    if (best > 999999) best = 999999;
    if (best > (uint32_t)s_profile.game_best) s_profile.game_best = (int)best;
    if (save_profile())
        ESP_LOGI(TAG, "游戏积分 +%lu → 累计 %d / 最高 %d(已存本地)",
                 (unsigned long)total, s_profile.game_total, s_profile.game_best);
    else ESP_LOGW(TAG, "游戏积分落盘失败");
    publish_gamescore();
}
#endif

// dock 某项被"确定"(编码器点击,运行在 LVGL 任务内,无需再加锁)
static void on_dock_action(int id, void *user) {
    (void)user;
    if (s_view_nav_restore_pending || atomic_load(&s_factory_command_active)) return;
    switch (id) {
        case 2: {  // Four plays share one navigation and input lifetime.
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_view_exit_ms && (uint32_t)(now - s_view_exit_ms) < VIEW_REENTER_GUARD_MS) break;
#if PERIPH_BLE
            int best = s_profile.game_best;
#else
            int best = 0;
#endif
            if (app_plays_open(best)) platform_lvgl_nav_enable(false);
            break;
        }
        case 3: {  // 看图:全屏无覆盖文字，确定打开返回/取消菜单
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (s_view_exit_ms && (uint32_t)(now - s_view_exit_ms) < VIEW_REENTER_GUARD_MS) {
                // 刚退出全屏,这次多半是退出长按释放被当成的误触点击 —— 忽略,避免死循环。
                ESP_LOGI(TAG, "忽略退出后 %ums 内的看图点击(误触去抖)", VIEW_REENTER_GUARD_MS);
                break;
            }
            if (jpeg_store_has_valid(JPEG_SLOT_FULL)) {
                platform_lvgl_nav_enable(false);   // 全屏期间冻结不可见主页的 Dock 焦点与点击
                jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
            } else {
                platform_lvgl_nav_enable(false);
                ui_album_no_photo(NULL);
            }
            break;
        }
        case 4:  // Compatibility action; Bobo is always available inside Plays.
            if (app_plays_open(0)) platform_lvgl_nav_enable(false);
            break;
        default: break;
    }
}

#if PERIPH_BUTTON
// 原始按键转发:仅在游戏进行中把 上=0/下=1/确定=2 喂给游戏。
// 运行在 iot_button 任务:只调用线程安全的 ui_game_key(仅置标志位)。
// 非游戏态直接返回,按键照常走 LVGL 导航。
static void on_btn_raw(int index, hal_btn_event_t e, void *user) {
    (void)user;
#if PERIPH_BLE
    if (capture_ui_blocked()) return;
#endif
    taskENTER_CRITICAL(&s_power_guard);
    if (atomic_load(&s_deep_sleep_entering)) { taskEXIT_CRITICAL(&s_power_guard); return; }
    s_last_activity_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool cancelled_sleep = atomic_exchange(&s_sleep_preparing, false);
    taskEXIT_CRITICAL(&s_power_guard);
    if (cancelled_sleep) app_plays_cancel_sleep();
    if (s_button_guide_active) {
        if (e == HAL_BTN_PRESS && index >= 0 && index < 3) s_button_guide_exit = true;
        return;   // 扫码页吞掉本次功能键；右侧任意按键均可进入主页
    }
    if (s_factory_mode || s_btn_test_active || atomic_load(&s_factory_command_active)) {   // 自检模式:业务按键(导航/游戏/看图)全屏蔽,只喂自检捕获
        // 只按 PRESS(按下)计数:一次物理按下=一次 PRESS;若也计 CLICK 会一次按变两次。
        if (s_btn_test_active && e == HAL_BTN_PRESS && index >= 0 && index < 3) {
            if (s_btn_test_count[index] < 255) s_btn_test_count[index]++;
            s_btn_test_mask |= (1 << index);
        }
        return;
    }
    if (app_plays_active()) {
        app_plays_button(index, e);
        return;
    }
    if (s_view_nav_restore_pending) return;
    if (ui_album_active() || jpeg_view_is_fullscreen()) {
        if (e != HAL_BTN_PRESS && e != HAL_BTN_CLICK && e != HAL_BTN_LONG) return;
        if (!platform_lvgl_lock(100)) return;
        ui_album_action_t action = ui_album_button(index,
            e == HAL_BTN_PRESS ? UI_ALBUM_PRESS : e == HAL_BTN_LONG ? UI_ALBUM_LONG : UI_ALBUM_CLICK);
        platform_lvgl_unlock();
        if (action == UI_ALBUM_RETURN_HOME || action == UI_ALBUM_DISMISSED) {
            s_view_nav_restore_pending = true;
            if (action == UI_ALBUM_RETURN_HOME) jpeg_view_exit();
            s_view_exit_ms = (uint32_t)(esp_timer_get_time() / 1000);
        }
        return;
    }
    // Home has no hidden long-press page. A held confirm key must not open
    // an unrelated screen when a child view has just returned home.
    if (!ui_game_is_active()) return;
    switch (index) {
        case 0: if (e == HAL_BTN_PRESS) ui_game_key(UI_GAME_KEY_UP);   break;  // 上
        case 1: if (e == HAL_BTN_PRESS) ui_game_key(UI_GAME_KEY_DOWN); break;  // 下
        case 2:  // 确定:短按=冲刺/开始/重来,长按=退出
            if (e == HAL_BTN_LONG)       ui_game_key(UI_GAME_KEY_EXIT);
            else if (e == HAL_BTN_CLICK) ui_game_key(UI_GAME_KEY_ENTER);
            break;
        default: break;
    }
}
#endif // PERIPH_BUTTON
#endif // PERIPH_DISPLAY

#if PERIPH_AUDIO
#if AUDIO_MIC_PASSTHROUGH_TEST
// 麦克风直通扬声器测试(开关在 board_config.h):对着麦说话,扬声器实时放出,
// 用于排查 ES8311 输入/输出通路。独占音频且不返回,故此模式下乐谱播放不可用。
static void mic_passthrough_task(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "麦克风直通测试已开启:音频被独占,乐谱播放不可用");
    platform_audio_passthrough(s_au.audio);   // 进入后不返回
}
#elif PERIPH_BLE
// 乐谱播放任务:等信号 → RTTTL 逐音符 → 分块方波合成 → 播放。新曲通过 s_music_interrupt 抢占。
// 方波合成缓冲放静态区(2KB),不占任务栈:放栈上会与 esp_codec_dev/i2s 驱动的
// 调用链一起把栈撑爆(实测 4096 栈 + 栈上 block 会触发 Stack protection fault)。
static int16_t s_music_block[1024];
static void music_player_task(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_music_sig, portMAX_DELAY);
        if (s_audio_owner) xSemaphoreTake(s_audio_owner, portMAX_DELAY);
        if (atomic_load(&s_sleep_preparing)) {
            if (s_audio_owner) xSemaphoreGive(s_audio_owner);
            atomic_store(&s_music_playing, false);
            if (s_music_idle_sig) xSemaphoreGive(s_music_idle_sig);
            continue;
        }
        atomic_store(&s_music_playing, true);
        s_music_interrupt = 0;
        if (audio_service_configure(&s_au, 8000, 16, 1) != 0) {
            if (s_audio_owner) xSemaphoreGive(s_audio_owner);
            atomic_store(&s_music_playing, false);
            if (s_music_idle_sig) xSemaphoreGive(s_music_idle_sig);
            continue;
        }
        rtttl_t rt;
        if (rtttl_init(&rt, (const char *)s_score_play_buf, s_score_play_len) < 0) {
            if (s_audio_owner) xSemaphoreGive(s_audio_owner);
            atomic_store(&s_music_playing, false);
            if (s_music_idle_sig) xSemaphoreGive(s_music_idle_sig);
            continue;
        }
        int freq, ms;
        while (!s_music_interrupt && rtttl_next(&rt, &freq, &ms) == 1) {
            int total = 8000 * ms / 1000;       // 采样数
            int phase = 0;
            while (total > 0 && !s_music_interrupt) {
                int n = total < 1024 ? total : 1024;
                square_fill(s_music_block, n, freq, 8000, &phase, 6000);
                audio_service_play(&s_au, s_music_block, (size_t)n * sizeof(int16_t));
                total -= n;
            }
        }
        if (s_audio_owner) xSemaphoreGive(s_audio_owner);
        atomic_store(&s_music_playing, false);
        if (s_music_idle_sig) xSemaphoreGive(s_music_idle_sig);
    }
}

static unsigned plays_get_volume(void *user) {
    (void)user;
    return s_profile.volume;
}
static bool apply_volume(void *user, unsigned volume) {
    (void)user;
    if (volume > 100) return false;
    if (volume == s_profile.volume) return true;
    unsigned previous = s_profile.volume;
    s_profile.volume = volume;
    if (!save_profile()) {
        s_profile.volume = previous;
        return false;
    }
    audio_service_set_volume(&s_au, volume);
    return true;
}
static bool plays_set_volume(void *user,unsigned value) {
    if(atomic_flag_test_and_set(&preference_busy))return false;
    bool ok=apply_volume(user,value);atomic_flag_clear(&preference_busy);return ok;
}
static bool plays_acquire_audio(void *user) {
    (void)user;
    if (!s_audio_owner || !s_au.audio || atomic_load(&s_sleep_preparing)) return false;
    s_music_interrupt = 1;
    if (xSemaphoreTake(s_audio_owner, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    if (atomic_load(&s_sleep_preparing) || audio_service_configure(&s_au, 16000, 16, 1) != 0 ||
        hal_audio_set_out_mute(s_au.audio, true) != 0) {
        xSemaphoreGive(s_audio_owner);
        return false;
    }
    return true;
}
static void plays_release_audio(void *user) {
    (void)user;
    (void)hal_audio_set_out_mute(s_au.audio, false);
    xSemaphoreGive(s_audio_owner);
}

// 开机只播放小程序最后一次成功下发的 RTTTL。无存档或存档损坏都静默跳过。
static void play_saved_boot_rtttl(void) {
    if (!s_kv || !s_music_sig || !s_music_task) return;
    int got = 0;
    if (hal_kv_get(s_kv, BOOT_RTTTL_KV_KEY, s_score_play_buf,
                   sizeof s_score_play_buf, &got) != 0 || got <= 0) {
        ESP_LOGI(TAG, "未配置 RTTTL 开机音乐,跳过播放");
        return;
    }
    if (rtttl_validate((const char *)s_score_play_buf, got) < 0) {
        ESP_LOGW(TAG, "已保存的 RTTTL 开机音乐无效,跳过播放");
        return;
    }
    s_score_play_len = got;
    s_music_interrupt = 1;
    xSemaphoreGive(s_music_sig);
    ESP_LOGI(TAG, "播放已保存的 RTTTL 开机音乐(%d 字节)", got);
}
#endif
#endif

static unsigned plays_get_brightness(void *user) {
    (void)user;
    return s_profile.brightness >= BRIGHTNESS_MIN && s_profile.brightness <= 100 ?
           (unsigned)s_profile.brightness : BRIGHTNESS_DEFAULT;
}
static bool plays_preview_brightness(void *user, unsigned brightness) {
    (void)user;
#if PERIPH_DISPLAY
    if (brightness < BRIGHTNESS_MIN || brightness > 100 || !s_disp) return false;
    hal_display_set_backlight(s_disp, (uint8_t)brightness);
    return true;
#else
    (void)brightness;
    return false;
#endif
}
static bool apply_brightness(void *user, unsigned brightness) {
    if (brightness < BRIGHTNESS_MIN || brightness > 100) return false;
    int previous = s_profile.brightness;
    if (!plays_preview_brightness(user, brightness)) return false;
    if (previous == (int)brightness) return true;
    s_profile.brightness = (int)brightness;
    if (!save_profile()) {
        s_profile.brightness = previous;
        (void)plays_preview_brightness(user, plays_get_brightness(user));
        return false;
    }
    return true;
}
static bool plays_set_brightness(void *user,unsigned value) {
    if(atomic_flag_test_and_set(&preference_busy))return false;
    bool ok=apply_brightness(user,value);atomic_flag_clear(&preference_busy);return ok;
}
static unsigned plays_get_sleep_minutes(void *user) {
    (void)user;
    return (unsigned)s_profile.sleep_min;
}
static bool plays_set_sleep_minutes(void *user, unsigned minutes) {
    (void)user;
    if (minutes > SLEEP_MIN_MAX) return false;
    int previous = s_profile.sleep_min;
    if (previous != (int)minutes) {
        s_profile.sleep_min = (int)minutes;
        if (!save_profile()) { s_profile.sleep_min = previous; return false; }
    }
    note_activity();
    return true;
}

#if PERIPH_AUDIO && PERIPH_BLE
// 播一段内置提示音。运行在 NimBLE host 任务:只做复制 + 置标志 + 给信号量,
// **绝不能在这里合成播放** —— audio_service_play 阻塞写 I2S,会卡死 BLE 协议栈。
static void play_tone(const char *rtttl) {
    if (!s_music_sig || !rtttl) return;
    int n = (int)strlen(rtttl);
    if (n <= 0 || n > SCORE_MAX) return;
    memcpy(s_score_play_buf, rtttl, (size_t)n);
    s_score_play_len   = n;
    s_music_interrupt  = 1;      // 抢占正在播的乐谱:短提示音优先于背景音乐
    xSemaphoreGive(s_music_sig);
}

#endif

#if PERIPH_BLE
static hal_ble_scan_t *s_scan;
#endif

#if PERIPH_BLE && PERIPH_DISPLAY
// 图片解码显示结果(运行于 LVGL 任务)。收齐时已回过 ACK,这里补发终结性回应。
// 失败时尽量给出**可操作**的原因:笼统的"解码失败"会让小程序那边完全无从下手。
static void on_jpeg_result(int ok, void *user) {
    (void)user;
    if (ok) {
        if (s_view_nav_restore_pending && jpeg_view_is_fullscreen()) jpeg_view_exit();
        ESP_LOGI(TAG, "JPG 显示结果: 成功");
        notify_status(CFG_MSG_JPEG, CFG_ST_DONE);
        return;
    }
    // Restore only after the current screen and any avatar recovery are settled.
    // An avatar failure can arrive while a play, binding page or photo is visible.
    s_view_nav_restore_pending = true;
    jpeg_probe_t p = jpeg_view_last_probe();
    uint8_t st;
    switch (p) {
        case JPEG_PROBE_PROGRESSIVE:
        case JPEG_PROBE_UNSUPPORTED: st = CFG_ST_ERR_JPEG_FORMAT; break;
        case JPEG_PROBE_TRUNCATED:
        case JPEG_PROBE_NOT_JPEG:    st = CFG_ST_ERR_JPEG_BROKEN; break;
        default:                     st = CFG_ST_ERR_DECODE;      break;  // 预检过了但解码器仍失败
    }
    ESP_LOGW(TAG, "JPG 显示结果: 失败 —— %s (回报状态 0x%02X)", jpeg_probe_str(p), st);
    notify_status(CFG_MSG_JPEG, st);
}
// Caller holds LVGL. The existing REFR_READY gate waits for a complete draw;
// installing a descriptor is not enough to safely illuminate the LCD.
static void finish_wake_frame(void) {
    if (s_wake_display && platform_lvgl_arm_first_frame(s_disp, s_wake_display, (uint8_t)plays_get_brightness(NULL)))
        s_wake_display = NULL;
}
static void on_boot_voice_opened(void *user) {
    (void)user;
    // Flash-mapped XiaoZhi portraits require no PNG decode. Arm the same
    // complete-frame gate only once the actual startup screen is present.
    ui_boot_close();
    atomic_store(&s_boot_avatar_ready, true);
    finish_wake_frame();
}
static void show_startup_brand(void) {
    if (!PASSPORT_XIAOZHI_EDITION) return;
    s_button_guide_active = true;
    if (!platform_lvgl_lock(500)) { s_button_guide_active = false; return; }
    bool shown = ui_boot_open();
    if (shown) finish_wake_frame();
    platform_lvgl_unlock();
    uint32_t started = (uint32_t)(esp_timer_get_time()/1000);
    // Wait for a real drawn overlay before the chime, without holding LVGL.
    while (shown && (uint32_t)(esp_timer_get_time()/1000)-started < 500u) {
        bool drawn = false;
        if (platform_lvgl_lock(50)) { drawn = ui_boot_drawn(); platform_lvgl_unlock(); }
        if (drawn) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
#if PERIPH_AUDIO && !AUDIO_MIC_PASSTHROUGH_TEST
    unsigned samples = xiaozhi_play_startup_sound();
    ESP_LOGI(TAG, "startup_chime samples=%u", samples);
#endif
    while (shown && (uint32_t)(esp_timer_get_time()/1000)-started < UI_BOOT_DURATION_MS)
        vTaskDelay(pdMS_TO_TICKS(10));
    // The overlay is removed by on_boot_voice_opened after the real page exists.
    ESP_LOGI(TAG, "startup_brand complete=%d", shown);
    s_button_guide_active = false;
}
static bool start_boot_xiaozhi(void) {
    if (!PASSPORT_XIAOZHI_EDITION) return false;
    s_button_guide_active = true; // Consume the wake/boot gesture through release.
    while (!platform_lvgl_lock(500)) vTaskDelay(pdMS_TO_TICKS(50));
    platform_lvgl_nav_enable(false);
    bool opened = app_plays_open_xiaozhi(s_profile.game_best);
    if (!opened) { ui_boot_close(); platform_lvgl_nav_enable(true); }
    platform_lvgl_unlock();
    if (opened) {
        while (platform_button_any_pressed(s_button)) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(80));
        note_activity();
    }
    s_button_guide_active = false;
    return opened;
}
// 头像位模式解码完成(运行于 LVGL 任务):把图片显示到头像位;dsc=NULL(失败)则显示兜底。
static void on_jpeg_avatar(const void *dsc, void *user) {
    (void)user;
    ui_profile_show_image(dsc);
    atomic_store(&s_boot_avatar_ready, true);
    finish_wake_frame();
}
// Called only after all boot-time subsystem allocations have completed. Keep
// PNG decoding out of the startup allocation peak on this no-PSRAM board.
static void restore_boot_avatar(void) {
    if (s_profile.avatar_name[0] == '\0' && jpeg_store_has_valid(JPEG_SLOT_AVATAR))
        jpeg_view_request_mode(JPEG_VIEW_AVATAR);
    else if (avatar_store_has(s_profile.avatar_name))
        jpeg_view_request_avatar(s_profile.avatar_name);
    else {
        // No stored avatar is a terminal result too; do not leave the LCD dark.
        atomic_store(&s_boot_avatar_ready, true);
        while (!platform_lvgl_lock(500)) vTaskDelay(pdMS_TO_TICKS(50));
        finish_wake_frame();
        platform_lvgl_unlock();
    }
}
#endif

#if PERIPH_BLE
// 收到一条 token 广播(运行于 NimBLE host 任务)。
// 纯决策交给 token_bcast_handle,这里只负责落地副作用。
static void on_token_bcast(const uint8_t *mfg, int len) {
    token_result_t r;
    // 单调时钟,单位毫秒。不用 esp_log_timestamp():那是日志用途的 API,其单位取决于
    // configTICK_RATE_HZ(实现是 tick * (1000/HZ)),改 FreeRTOS 频率就会悄悄变;
    // 而这个值喂给的是签名失败限速器,时钟语义必须是明确的。
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    token_bcast_handle(&s_tb, mfg, len, now_ms, &r);

    if (r.action == TOKEN_ACT_IGNORE) {
        // 只有"已验签但字段非法"才值得喊 —— 那是网关的真实 bug。
        // 别人的广播、重复包都静默(场地里这是常态流量,不是异常)。
        if (r.reason == TOKEN_R_BAD_BALANCE || r.reason == TOKEN_R_BAD_OP) {
            ESP_LOGW(TAG, "token 广播字段非法 reason=%d(balance/op),请查网关", (int)r.reason);
        }
        return;
    }

    if (r.action == TOKEN_ACT_FAIL && r.reason == TOKEN_R_BAD_MAC) {
        // 签名不对:整个包不可信,一个字节都不采信,只响音
        ESP_LOGW(TAG, "token 广播签名校验失败(可能有人在伪造,或网关密钥不一致)");
#if PERIPH_AUDIO
        play_tone(TONE_TOKEN_FAIL);
#endif
        return;
    }

    // Retired pet commands must never mutate balances or stored profile data.
    if (r.action == TOKEN_ACT_PET) return;
    note_activity(); // 仅本机、已验签且非重复的有效业务报文刷新空闲时间。

    // 到这里是 ADD / SUB / SYNC / FAIL(网关下发的业务失败) —— 都已验签,都要落地
    int prev_token = s_profile.token;                 // 记旧值,给奖励动画算增减量
    s_profile.token = (int)r.balance;

    // SYNC 是周期性对账,只刷数字不响音 —— 否则每次同步工牌都会叫一声。
    const char *tone = (r.action == TOKEN_ACT_ADD)  ? TONE_TOKEN_ADD
                     : (r.action == TOKEN_ACT_SUB)  ? TONE_TOKEN_SUB
                     : (r.action == TOKEN_ACT_SYNC) ? NULL
                                                    : TONE_TOKEN_FAIL;

    // 先落盘再响成功音:反过来会出现"响了加分音但值没存住",
    // 用户听到成功、重启后余额退回,比直接听到失败音更难排查。
    if (!save_profile()) {
        ESP_LOGW(TAG, "token 余额落盘失败,本次改动撑不过重启");
        tone = TONE_TOKEN_FAIL;
    }

    ESP_LOGI(TAG, "token 广播生效: op=%d 余额=%u seq=%u",
             (int)r.action, (unsigned)r.balance, (unsigned)r.seq);

#if PERIPH_DISPLAY
    // capture gate 关闭时才允许走普通 LVGL 锁;截图期间直接跳过刷新。
    // 这安全是因为 balance 是绝对值:屏幕短暂显示旧值,下一条广播会带出正确值。
    // 绝不能持着截图 ACK 所需的 NimBLE host 任务等待 LVGL 锁。
    if (!capture_ui_blocked() && platform_lvgl_lock(0)) {
        ui_profile_set_title(s_profile.role);
        ui_profile_set_intro(s_profile.subtitle);
        // 加/扣分:播放礼盒+金币奖励动画(SYNC/FAIL 不触发)。delta 取绝对值。
        if (r.action == TOKEN_ACT_ADD || r.action == TOKEN_ACT_SUB) {
            int delta = s_profile.token - prev_token;
            ui_reward_play(r.action == TOKEN_ACT_ADD, delta < 0 ? -delta : delta);
        }
        platform_lvgl_unlock();
    }
#endif
#if PERIPH_AUDIO
    play_tone(tone);
#endif
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// v2 档案广播:静默更新绝对值,不播放 token 奖励音。
static void on_profile_bcast(const uint8_t *mfg, int len) {
    if (!s_tb.enabled || len != PROFILE_BCAST_LEN) return;
    pb_result_t r;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pb_status_t st = profile_bcast_feed(&s_pb, mfg, now_ms, &r);
    if (st != PB_COMPLETE) {
        static uint32_t last_error_ms;
        if (st == PB_ERROR && now_ms - last_error_ms >= 3000u) {
            last_error_ms = now_ms;
            ESP_LOGW(TAG, "档案广播无效,已忽略");
        }
        return;
    }

    bool persist = false, refresh_token = false, refresh_name = false;
    bool refresh_avatar = false;
    switch (r.field) {
        case PB_FIELD_TOKEN: {
            uint32_t v = read_u32_le(r.data);
            s_profile.token = (int)(v > 1000000000u ? 1000000000u : v);
            persist = refresh_token = true;
            break;
        }
        case PB_FIELD_TOKEN_MAX: {
            uint32_t v = read_u32_le(r.data);
            if (v < 1u) v = 1u;
            s_profile.token_max = (int)(v > 1000000000u ? 1000000000u : v);
            persist = refresh_token = true;
            break;
        }
        case PB_FIELD_TIME: {
            struct timeval tv = { .tv_sec = (time_t)read_u32_le(r.data), .tv_usec = 0 };
            settimeofday(&tv, NULL);
            break;
        }
        case PB_FIELD_NICKNAME:
            memcpy(s_profile.name, r.data, r.len);
            s_profile.name[r.len] = '\0';
            persist = refresh_name = true;
            break;
        case PB_FIELD_AVATAR_NAME: {
            char name[16];
            memcpy(name, r.data, r.len);
            name[r.len] = '\0';
            if (!avatar_store_has(name)) {
                ESP_LOGW(TAG, "档案广播头像 '%s' 不存在,保留当前头像", name);
                return;
            }
            memcpy(s_profile.avatar_name, name, r.len + 1u);
            persist = refresh_avatar = true;
            break;
        }
        default:
            return;
    }
    if (persist && !save_profile()) {
        ESP_LOGW(TAG, "档案广播落盘失败");
        return;
    }
    note_activity();
    ESP_LOGI(TAG, "档案广播生效 field=%u txn=%u", (unsigned)r.field, (unsigned)r.txn);
#if PERIPH_DISPLAY
    if (refresh_avatar) jpeg_view_request_avatar(s_profile.avatar_name);
    unsigned ui_pending = refresh_name ? PROFILE_UI_REFRESH_NAME : 0u;
    if (ui_pending) {
        atomic_fetch_or(&s_profile_ui_pending, ui_pending);
        profile_ui_flush_pending();
    }
#endif
}

// v3 快速档案广播:常用字段单包、昵称最多 5 片。两类消息分别完整验签/重组后才落地。
static void on_profile_batch_bcast(const uint8_t *mfg, int len) {
    if (!s_tb.enabled || len != PBB_FRAME_LEN) return;
    pbb_result_t r;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pbb_status_t st = profile_batch_bcast_feed(&s_pbb, mfg, now_ms, &r);
    if (st == PBB_IGNORE || st == PBB_MORE) return;
    if (st == PBB_ERROR) {
        static uint32_t last_error_ms;
        if ((uint32_t)(now_ms - last_error_ms) >= 3000u) {
            last_error_ms = now_ms;
            ESP_LOGW(TAG, "v3 档案广播字段非法,已忽略");
        }
        return;
    }

    bool persist = false, refresh_token = false, refresh_name = false, refresh_avatar = false;
    uint32_t time_epoch = 0;
    profile_data_t old_profile = s_profile;
    if (st == PBB_COMMON_READY) {
        // 先验证所有会失败的资源引用,确保常用字段包是全有或全无。
        char avatar_name[16] = {0};
        if ((r.fields & PBB_FIELD_AVATAR) &&
            avatar_store_name_at((uint8_t)(r.avatar_id - 1u), avatar_name) != 0) {
            ESP_LOGW(TAG, "v3 档案广播 avatar_id=%u 不存在,整包拒绝", (unsigned)r.avatar_id);
            return;
        }
        if (r.fields & PBB_FIELD_TOKEN) {
            s_profile.token = (int)r.token;
            persist = refresh_token = true;
        }
        if (r.fields & PBB_FIELD_TOKEN_MAX) {
            s_profile.token_max = (int)r.token_max;
            persist = refresh_token = true;
        }
        if (r.fields & PBB_FIELD_TIME) {
            time_epoch = r.time_epoch;
        }
        if (r.fields & PBB_FIELD_AVATAR) {
            memcpy(s_profile.avatar_name, avatar_name, sizeof avatar_name);
            persist = refresh_avatar = true;
        }
    } else if (st == PBB_NICKNAME_READY) {
        memcpy(s_profile.name, r.nickname, r.nickname_len);
        s_profile.name[r.nickname_len] = '\0';
        persist = refresh_name = true;
    }

    if (persist && !save_profile()) {
        s_profile = old_profile;
        ESP_LOGW(TAG, "v3 档案广播落盘失败");
        return;
    }
    if (time_epoch != 0) {
        struct timeval tv = { .tv_sec = (time_t)time_epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
    }
    profile_batch_bcast_commit(&s_pbb,
        st == PBB_COMMON_READY ? PBB_HDR_COMMON : PBB_HDR_NICKNAME, r.txn);
    note_activity();
    ESP_LOGI(TAG, "v3 档案广播生效 type=0x%02X txn=%u",
             st == PBB_COMMON_READY ? PBB_HDR_COMMON : PBB_HDR_NICKNAME, (unsigned)r.txn);
#if PERIPH_DISPLAY
    if (refresh_avatar) jpeg_view_request_avatar(s_profile.avatar_name);
    unsigned ui_pending = refresh_name ? PROFILE_UI_REFRESH_NAME : 0u;
    if (ui_pending) {
        atomic_fetch_or(&s_profile_ui_pending, ui_pending);
        profile_ui_flush_pending();
    }
#endif
}

// 扫描命中回调(NimBLE host 任务)。粗筛已在 ble_scan.c 做过,这里按类型分流。
static void on_ble_match(const uint8_t *data, int len, void *user) {
    (void)user;
    if (atomic_load(&s_sleep_preparing)) return;
    ble_match_kind_t kind = ble_match_classify(data, len);
    if (kind == BLE_MATCH_TOKEN) { on_token_bcast(data, len); return; }
    if (kind == BLE_MATCH_PROFILE) { on_profile_bcast(data, len); return; }
    if (kind == BLE_MATCH_PROFILE_BATCH) { on_profile_batch_bcast(data, len); return; }
    // 旧版心跳广播保留识别但静默忽略，不再弹出 HELLO WORLD 页面或播放提示音。
    (void)kind;
}
#endif

// 未同步过时间时的默认起点(本地秒当 UTC):2026-01-01 00:00。BLE 对时后会被覆盖。
// 这样状态栏始终显示一个走动的默认时间,而不是 --:--。
#define DEFAULT_TIME_EPOCH 1767225600L
static void seed_default_time(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= 1600000000L) return;  // 已有/已同步,不覆盖
    tv.tv_sec = DEFAULT_TIME_EPOCH; tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "时间未同步,置默认起点 2026-01-01 00:00(BLE 对时后覆盖)");
}

// 尚未取得真实电量或电量计不可用时显示未知轮廓,不使用虚拟百分比。
static void show_unknown_battery(void) {
#if PERIPH_DISPLAY
    if (platform_lvgl_lock(0)) {
        ui_profile_set_battery(-1, 0);
        ui_xiaozhi_set_battery(-1, 0);
        platform_lvgl_unlock();
    }
#endif
}

#if PERIPH_BATTERY
static hal_battery_t   *s_batt;
static battery_filter_t s_batt_f;
#endif

#if PERIPH_BUTTON
// 空闲 sleep_min 分进入深度睡眠。三个 ADC 功能键共用 GPIO0，按下电压均低于数字低阈值，
// 因而配置该节点低电平唤醒即可覆盖上/下/确定键。深睡唤醒属于一次复位启动。
static int s_button_wakeup_gpio = -1;

static bool sleep_async_work_finished(void) {
#if PERIPH_DISPLAY && PERIPH_BLE
    // 分片写是同步的；清状态后已写但未 END 的图片没有有效 header，下次启动不会使用。
    if (atomic_load(&s_jpeg_upload_active)) {
        jpeg_rx_init(&s_jpeg_rx);
        atomic_store(&s_jpeg_upload_active, false);
    }
#if PERIPH_AUDIO
    if (s_score_rx.receiving) score_rx_init(&s_score_rx, s_score_rx_buf, SCORE_MAX);
#endif

    // 截图任务可能正持 LVGL 锁并等待 ACK。先唤醒取消，下一轮确认完全退栈后再关屏。
    if (atomic_load(&s_capture_start_pending)) {
        (void)atomic_fetch_add(&s_capture_session, 1);
        capture_clear_start_pending();
    }
    if (atomic_load(&s_capture_active) || platform_screen_capture_active()) {
        capture_request_cancel(CAP_CANCEL_DISCONNECT);
        return false;
    }
    if (jpeg_view_decode_busy()) return false;
#endif
#if PERIPH_DISPLAY
    if (!app_plays_prepare_sleep()) return false;
#endif
    return true;
}

#if PERIPH_AUDIO
static int audio_stop_for_deep_sleep(void) {
    s_music_interrupt = 1;
#if PERIPH_BLE && !AUDIO_MIC_PASSTHROUGH_TEST
    if (s_music_sig) (void)xSemaphoreTake(s_music_sig, 0); // 丢弃尚未开始的播放请求
    if (s_music_idle_sig) {
        while (xSemaphoreTake(s_music_idle_sig, 0) == pdTRUE) { }
    }
    if (atomic_load(&s_music_playing) && s_music_idle_sig) {
        (void)xSemaphoreTake(s_music_idle_sig, pdMS_TO_TICKS(300));
    }
    if (atomic_load(&s_music_playing)) {
        ESP_LOGE(TAG, "音乐任务未能及时停止，取消本次深睡");
        return -1;
    }
#endif
    // READY normally guarantees the play worker released recording ownership.
    // Take the same mutex as a final guard around the physical codec shutdown.
    if (s_audio_owner && xSemaphoreTake(s_audio_owner, pdMS_TO_TICKS(500)) != pdTRUE) return -1;
    int result = audio_service_shutdown(&s_au);
    if (s_audio_owner) xSemaphoreGive(s_audio_owner);
    return result;
}
#endif

static void idle_check_cb(void *arg) {
    (void)arg;
#if PERIPH_DISPLAY && PERIPH_BLE
    profile_ui_flush_pending();
#endif
    static uint16_t hb;                       // 心跳:每 ~60s(240×250ms)打一次堆,盯运行期泄漏/碎片
    if (++hb >= 240) { hb = 0; log_heap("hb-60s"); }

    // 图片退出由 LVGL timer 异步销毁 screen。等销毁/头像恢复完成且长按键已释放后，
    // 再恢复主页导航，彻底吞掉退出长按的释放事件。
#if PERIPH_DISPLAY && PERIPH_BLE
    if (s_view_nav_restore_pending && !atomic_load(&s_factory_command_active) &&
        !jpeg_view_is_active() && !jpeg_view_decode_busy() &&
        !ui_album_active() && !app_plays_active() && !capture_ui_blocked() &&
        !platform_button_any_pressed(s_button) && platform_lvgl_lock(10)) {
        // State can change while waiting for LVGL. Recheck before waking Dock.
        if (!jpeg_view_is_active() && !jpeg_view_decode_busy() && !ui_album_active() &&
            !app_plays_active() && !capture_ui_blocked() && !platform_button_any_pressed(s_button) &&
            !s_button_guide_active && ui_profile_is_active()) {
            s_view_exit_ms = (uint32_t)(esp_timer_get_time() / 1000);
            s_view_nav_restore_pending = false;
            platform_lvgl_nav_enable(true);
            note_activity();
        }
        platform_lvgl_unlock();
    }
#endif

#if DEEP_SLEEP_FORCE_DISABLED
    return;
#endif

    uint32_t now  = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t idle = now - s_last_activity_ms;

    if (s_factory_mode || atomic_load(&s_factory_command_active) || s_deep_sleep_entering) return;
#if DEEP_SLEEP_TEST_SECONDS > 0
    uint32_t thr = (uint32_t)DEEP_SLEEP_TEST_SECONDS * 1000u;
#else
#if PERIPH_BLE
    int lmin = s_profile.sleep_min;
#else
    int lmin = 5;
#endif
    if (lmin <= 0) return;
    uint32_t thr = (uint32_t)lmin * 60u * 1000u;
#endif
    if (app_plays_voice_busy() && !atomic_load(&s_sleep_preparing)) {
        atomic_store(&s_last_activity_ms, now);
        return;
    }
    if (idle >= thr) {
        taskENTER_CRITICAL(&s_power_guard);
        // A key press or phone request can arrive after the first idle sample.
        uint32_t check_now = (uint32_t)(esp_timer_get_time() / 1000);
        if (atomic_load(&s_factory_command_active) ||
            check_now - atomic_load(&s_last_activity_ms) < thr) {
            taskEXIT_CRITICAL(&s_power_guard);
            return;
        }
        atomic_store(&s_sleep_preparing, true);
        taskEXIT_CRITICAL(&s_power_guard);
        if (!sleep_async_work_finished()) return;

        if (s_button_wakeup_gpio < 0) {
            ESP_LOGE(TAG, "无法进入深睡:未配置功能键唤醒 GPIO");
            atomic_store(&s_sleep_preparing, false);
#if PERIPH_DISPLAY
            app_plays_cancel_sleep();
#endif
            note_activity();
            return;
        }
        
        // 配置GPIO唤醒:根据芯片特性选择合适的API
        esp_err_t err;
#if SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP
        // ESP32-C3 等支持直接GPIO深睡唤醒的芯片
        err = esp_deep_sleep_enable_gpio_wakeup(
            1ULL << s_button_wakeup_gpio, ESP_GPIO_WAKEUP_GPIO_LOW);
#elif SOC_PM_SUPPORT_EXT0_WAKEUP
        // ESP32-S3 等使用EXT0唤醒的芯片 (单GPIO,低电平唤醒)
        err = esp_sleep_enable_ext0_wakeup((gpio_num_t)s_button_wakeup_gpio, 0);
#else
#error "No deep sleep GPIO wakeup method available for this chip"
#endif
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "配置 GPIO%d 深睡唤醒失败:%s",
                     s_button_wakeup_gpio, esp_err_to_name(err));
            atomic_store(&s_sleep_preparing, false);
#if PERIPH_DISPLAY
            app_plays_cancel_sleep();
#endif
            note_activity();
            return;
        }
#if PERIPH_BUTTON
        if (!platform_button_prepare_deep_sleep(s_button)) {
            ESP_LOGW(TAG, "功能键节点未稳定,取消本次深睡并重新计时");
            atomic_store(&s_sleep_preparing, false);
#if PERIPH_DISPLAY
            app_plays_cancel_sleep();
#endif
            note_activity();
            return;
        }
#endif
        bool committed = false;
        taskENTER_CRITICAL(&s_power_guard);
        uint32_t commit_now = (uint32_t)(esp_timer_get_time() / 1000);
        if (atomic_load(&s_sleep_preparing) && commit_now - atomic_load(&s_last_activity_ms) >= thr) {
#if PERIPH_DISPLAY
            committed = app_plays_commit_sleep();
#else
            committed = true;
#endif
            if (committed) atomic_store(&s_deep_sleep_entering, true);
        }
        taskEXIT_CRITICAL(&s_power_guard);
        if (!committed) {
            atomic_store(&s_sleep_preparing, false);
#if PERIPH_DISPLAY
            app_plays_cancel_sleep();
#endif
#if PERIPH_BUTTON
            platform_button_cancel_deep_sleep(s_button);
#endif
            return;
        }
#if PERIPH_BATTERY
        // CW2017 和 ES8311 共用 I2C：先休眠电量计，再由音频关闭流程最后释放 SDA/SCL。
        if (s_batt && hal_battery_shutdown(s_batt) != 0)
            ESP_LOGW(TAG, "CW2017 未能进入休眠，继续关闭其他外设");
#endif
#if PERIPH_AUDIO
        // 等乐谱任务退出 I2S 写入后再关 codec，避免 close/write 跨任务竞争。
        int audio_off = audio_stop_for_deep_sleep();
        if (audio_off != 0) {
            ESP_LOGW(TAG, "深睡前关闭 ES8311 失败:%d，继续进入深睡", audio_off);
        }
#endif
#if PERIPH_BLE
        (void)hal_config_shutdown(s_cfg);
#endif
#if PERIPH_DISPLAY
        if (s_disp) {
            if (platform_lvgl_lock(1000)) {
                if (hal_display_shutdown(s_disp) != 0)
                    ESP_LOGW(TAG, "显示面板未完整进入休眠，继续进入深睡");
                platform_lvgl_unlock();
            } else {
                hal_display_set_backlight(s_disp, 0);
                ESP_LOGW(TAG, "LVGL 锁超时，仅关闭背光后进入深睡");
            }
        }
#endif
        s_deep_sleep_entering = true;
#if DEEP_SLEEP_TEST_SECONDS > 0
        ESP_LOGI(TAG, "空闲 %d 秒(临时测试) → 进入深度睡眠(GPIO%d 任意功能键唤醒)",
                 DEEP_SLEEP_TEST_SECONDS, s_button_wakeup_gpio);
#else
        ESP_LOGI(TAG, "空闲 %d 分钟 → 进入深度睡眠(GPIO%d 任意功能键唤醒)",
                 lmin, s_button_wakeup_gpio);
#endif
        esp_deep_sleep_start();
    }
}
#endif

#if PERIPH_BATTERY
// 采一次:CW2017 原始 SOC → 直接分档 → 刷状态栏。定时运行时位于 esp_timer 任务。
// 返回 true 仅表示已有有效 SOC 且本次已成功写入界面(无屏幕配置时表示 SOC 有效)。
static bool battery_sample(void) {
    int soc = hal_battery_read_soc(s_batt);   // CW2017 电量计:直接给 SOC%
    // 无效读数只保留缓存,不回退到电压估算；首次就绪前保持未知状态。
    battery_feed_pct(&s_batt_f, soc);
    if (s_batt_f.level == 0) return false;
    int pct = battery_percent(&s_batt_f);
    int lvl = battery_level(&s_batt_f);
    platform_clicker_set_battery(pct);
#if PERIPH_DISPLAY
    // SDK timeout0 means an infinite wait. This callback runs on esp_timer's
    // high-priority task; waiting through a full LVGL render boosts that task
    // above voice via mutex inheritance. Bound the wait to1ms and retry the
    // cached SOC next cycle, even if the next sensor read fails.
    if (platform_lvgl_lock(1)) {
        ui_profile_set_battery(pct, lvl);
        ui_xiaozhi_set_battery(pct, lvl);
        platform_lvgl_unlock();
        return true;
    }
    return false;
#else
    return true;
#endif
}
static void battery_timer_cb(void *arg) { (void)arg; battery_sample(); }

// 首页点亮前进行有限快速首读。循环每次主动让出 CPU，不会饿死 LVGL/IDLE 任务。
static void battery_prime_on_boot(void) {
    const int attempts = BATT_BOOT_TIMEOUT_MS / BATT_BOOT_RETRY_MS + 1;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (battery_sample()) {
            ESP_LOGI(TAG, "开机电量首读完成 attempt=%d soc=%d%%", attempt,
                     battery_percent(&s_batt_f));
            return;
        }
        if (attempt < attempts) vTaskDelay(pdMS_TO_TICKS(BATT_BOOT_RETRY_MS));
    }
    ESP_LOGW(TAG, "开机电量首读在 %dms 内未就绪,保持空图标并由定时采样继续重试",
             BATT_BOOT_TIMEOUT_MS);
}
#endif

// ============================ 产测执行(AT+TEST 系列)============================
// 运行于 USB 串口任务(provision_console 的 console_task)。每个 line_* 把一行
// "+TEST:...,PASS|FAIL\r\n" 写进缓冲并返回是否通过;AUTO 汇总全部 + RESULT 行。
// 判据阈值与解析在 services/factory_test.c(纯逻辑,已单测)。
#if PERIPH_BLE

static bool ft_health_pass(void) {
    esp_reset_reason_t rr = esp_reset_reason();
    bool rst_ok = !(rr == ESP_RST_PANIC || rr == ESP_RST_BROWNOUT ||
                    rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT);
    return ft_heap_pass((int)esp_get_free_heap_size()) && rst_ok;
}

static bool ft_line_info(char *o, int n, int *w) {
    const esp_app_desc_t *d = esp_app_get_description();
    char sn[24] = {0};
    hal_identity_get_sn(s_id, sn, sizeof sn);
    bool pass = ft_health_pass();
    *w = snprintf(o, n, "+TEST:INFO,ver=%s,sn=%s,heap=%u,rst=%d,%s\r\n",
                  d ? d->version : "?", sn[0] ? sn : "-",
                  (unsigned)esp_get_free_heap_size(), (int)esp_reset_reason(),
                  pass ? "PASS" : "FAIL");
    return pass;
}

// 纯格式化(不碰硬件),便于 AUTO 复用一次采集的结果、避免重复配 codec。
static int ft_fmt_i2c(char *o, int n, int es8311, int cw2017, bool *pass) {
    *pass = es8311 && cw2017;
    return snprintf(o, n, "+TEST:I2C,es8311=%d,cw2017=%d,%s\r\n",
                    es8311, cw2017, *pass ? "PASS" : "FAIL");
}
static int ft_fmt_audio(char *o, int n, int ret, int mag, int peak, bool *pass) {
    *pass = ft_audio_pass(ret, mag);
    return snprintf(o, n, "+TEST:AUDIO,ret=%d,tone_mag=%d,peak=%d,%s\r\n",
                    ret, mag, peak, *pass ? "PASS" : "FAIL");
}

// 声学自回环:边放 1kHz 300ms 边录,测 1kHz 处能量。ret==0 说明 codec 在 I2C 上有响应,
// 故 AUTO 里可用它的 ret 直接判 ES8311 在位,省掉一次单独的 codec 重配。
static int ft_run_audio(int *ret, int *mag, int *peak) {
    *ret = -1; *mag = 0; *peak = 0;
#if PERIPH_AUDIO
    *ret = audio_service_loopback_test(&s_au, 1000, 300, peak, mag);
#endif
    return *ret;
}
static int ft_batt_present(void) {
#if PERIPH_BATTERY
    return s_batt != NULL;
#else
    return 0;
#endif
}

static bool ft_line_i2c(char *o, int n, int *w) {
    int es8311 = 0;
#if PERIPH_AUDIO
    es8311 = (audio_service_configure(&s_au, 16000, 16, 1) == 0);   // 单发:重配 codec 走 I2C,不出声
#endif
    bool pass; *w = ft_fmt_i2c(o, n, es8311, ft_batt_present(), &pass);
    return pass;
}

static bool ft_line_audio(char *o, int n, int *w) {
    int ret, mag, peak;
    ft_run_audio(&ret, &mag, &peak);
    bool pass; *w = ft_fmt_audio(o, n, ret, mag, peak, &pass);
    return pass;
}

static bool ft_line_batt(char *o, int n, int *w) {
    bool present = false; int soc = -1, mv = -1;
#if PERIPH_BATTERY
    present = (s_batt != NULL);
    if (present) {
        // 芯片唤醒/更新 profile 后 SOC 有短暂启动保护期。只有产测允许有界等待;
        // 周期采样保持非阻塞,不把尚未就绪误判为电池故障,也不替换真实 0%。
        const int64_t deadline = esp_timer_get_time() + 4000000LL;
        soc = hal_battery_read_soc(s_batt);
        while ((soc < 0 || soc > 100) && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(100));
            soc = hal_battery_read_soc(s_batt);
        }
        mv = hal_battery_read_mv(s_batt);
    }
#endif
    bool pass = ft_batt_pass(present, soc);
    *w = snprintf(o, n, "+TEST:BATT,present=%d,soc=%d,mv=%d,%s\r\n",
                  present, soc, mv, pass ? "PASS" : "FAIL");
    return pass;
}

static bool ft_line_ble(char *o, int n, int *w) {
    bool started = (s_scan != NULL) && (s_cfg != NULL);
    char sn[24] = {0};
    hal_identity_get_sn(s_id, sn, sizeof sn);
    *w = snprintf(o, n, "+TEST:BLE,started=%d,sn=%s,%s\r\n",
                  started, sn[0] ? sn : "-", started ? "PASS" : "FAIL");
    return started;
}

static bool ft_line_id(char *o, int n, int *w) {
    // 逐字段检测 cardid:DeviceKey(sn)/DeviceSecret(key)/ProductKey(pk)/HardwareVersion(hw)。
    // 四字段齐 → PASS;缺任一 → FAIL(SN 恒由 MAC 回补,通常总在,真正可能缺的是 key/pk/hw)。
    bool sn, key, pk, hw;
    identity_fields_present(&sn, &key, &pk, &hw);
    char fp[9]; identity_pk_fingerprint(fp);
    bool pass = sn && key && pk && hw;
    *w = snprintf(o, n, "+TEST:ID,sn=%d,key=%d,pk=%d,hw=%d,pk_fp=%s,%s\r\n",
                  sn, key, pk, hw, fp, pass ? "PASS" : "FAIL");
    return pass;
}

#if PERIPH_BUTTON
// 逐键捕获:开捕获态,最多 10s;每识别到新键即时 printf 一行;集齐 上/下/确定 判 PASS。
// 直接 printf 到同一 USB 串口(与框架同任务,无交错),返回 0 表示无需框架再写。
static int ft_btn_test(void) {
    s_btn_test_mask = 0;
    s_btn_test_active = true;
    printf("+TEST:BTN,capturing,press UP DOWN OK within 10s\r\n"); fflush(stdout);
    const char *names[3] = { "UP", "DOWN", "OK" };
    int64_t start = esp_timer_get_time() / 1000;
    int prev = 0;
    while (((esp_timer_get_time() / 1000) - start) < 10000 && s_btn_test_mask != 0x7) {
        int m = s_btn_test_mask;
        if (m != prev) {
            for (int i = 0; i < 3; i++)
                if ((m & (1 << i)) && !(prev & (1 << i)))
                    printf("+TEST:BTN,key=%s\r\n", names[i]);
            fflush(stdout); prev = m;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    s_btn_test_active = false;
    int m = s_btn_test_mask;
    if (m == 0x7) printf("+TEST:BTN,PASS\r\n");
    else          printf("+TEST:BTN,FAIL,mask=%d\r\n", m);
    fflush(stdout);
    return 0;
}
#endif

static int run_factory_command(ft_cmd_t cmd, char *out, int n, void *user) {
    (void)user;
    int w = 0, dw = 0;
    switch (cmd) {
        case FT_DSP: {
            xz_dsp_stats_t stats = {0};
            bool ok = xiaozhi_dsp_check(&stats);
            w = snprintf(out, n,
                "+TEST:DSP,frames16=%u,avg16_us=%u,peak16_us=%u,"
                "frames24=%u,avg24_us=%u,peak24_us=%u,cpu_mhz=%u,stack_free=%u,%s\r\n",
                stats.frames16, stats.avg16_us, stats.peak16_us,
                stats.frames24, stats.avg24_us, stats.peak24_us,
                stats.cpu_mhz, stats.stack_free, ok ? "OK" : "FAIL");
            return w < 0 ? 0 : w < n ? w : n > 0 ? n - 1 : 0;
        }
        case FT_INFO:  ft_line_info(out, n, &w); break;
        case FT_I2C:   ft_line_i2c(out, n, &w);  break;
        case FT_AUDIO: ft_line_audio(out, n, &w);break;
        case FT_BATT:  ft_line_batt(out, n, &w); break;
        case FT_BLE:   ft_line_ble(out, n, &w);  break;
        case FT_ID:    ft_line_id(out, n, &w);   break;
#if PERIPH_DISPLAY
        case FT_DISP:
            if (platform_lvgl_lock(0)) { ui_factory_disp_test(); platform_lvgl_unlock(); }
            w = snprintf(out, n, "+TEST:DISP,shown,OK\r\n");   // 目视判定,由治具记录
            break;
#endif
#if PERIPH_BUTTON
        case FT_BTN:   return ft_btn_test();
#endif
        case FT_AUTO: {
            int fails = 0; bool p;
            // 音频回环只跑一次(唯一一次 codec 配置);ES8311 在位复用其 ret,不再单独重配。
            int a_ret, a_mag, a_peak;
            ft_run_audio(&a_ret, &a_mag, &a_peak);
            int es8311 = (a_ret != -1 && a_ret != -2);   // codec 配置成功=在 I2C 上有响应

            if (!ft_line_info(out + w, n - w, &dw)) { fails++; } w += dw;
            dw = ft_fmt_i2c(out + w, n - w, es8311, ft_batt_present(), &p);   if (!p) fails++; w += dw;
            dw = ft_fmt_audio(out + w, n - w, a_ret, a_mag, a_peak, &p);      if (!p) fails++; w += dw;
            if (!ft_line_batt(out + w, n - w, &dw)) { fails++; } w += dw;
            if (!ft_line_ble (out + w, n - w, &dw)) { fails++; } w += dw;
            if (!ft_line_id  (out + w, n - w, &dw)) { fails++; } w += dw;
            w += snprintf(out + w, n - w, "+TEST:RESULT,%s,fails=%d\r\n",
                          fails ? "FAIL" : "PASS", fails);
            break;
        }
        default: w = snprintf(out, n, "+ERR=test_unhandled\r\n"); break;
    }
    return w;
}

// Serial self-test owns keys/audio until completion. Ordinary gameplay and
// asynchronous music must not operate those peripherals at the same time.
static int ft_voice_status(char *out, int n) {
    if (!out || n <= 0) return 0;
    xz_view_t view;
    xiaozhi_snapshot(&view);
    // Numeric, read-only telemetry: no transcript, QR/binding code, Wi-Fi name
    // or password. OK means the query succeeded, not that audio passed QA.
    int w = snprintf(out, n,
        "+TEST:VOICE,phase=%u,stage=%u,revision=%u,wifi_saved=%u,stopped=%u,"
        "heap=%u,largest=%u,min=%u,uptime_ms=%u,OK\r\n",
        (unsigned)view.phase, (unsigned)view.stage, view.revision,
        (unsigned)view.wifi_saved, (unsigned)xiaozhi_stopped(), (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
        (unsigned)esp_get_minimum_free_heap_size(),
        (unsigned)(esp_timer_get_time() / 1000));
    return w < 0 ? 0 : w < n ? w : n - 1;
}

static int on_factory_test(ft_cmd_t cmd, char *out, int n, void *user) {
    // Status remains available in a play or on the QR screen, without taking
    // audio ownership, changing navigation, or extending the sleep timeout.
    if (cmd == FT_VOICE) return ft_voice_status(out, n);
#if PERIPH_DISPLAY
    if (app_plays_active() || ui_album_active() || jpeg_view_is_active() ||
        capture_ui_blocked() || (s_button_guide_active && cmd != FT_DSP) || s_view_nav_restore_pending)
        return snprintf(out, n, "+ERR=busy,return_home_before_test\r\n");
#endif
    taskENTER_CRITICAL(&s_power_guard);
    bool busy = atomic_load(&s_sleep_preparing) || atomic_load(&s_deep_sleep_entering);
    if (!busy) atomic_store(&s_factory_command_active, true);
    taskEXIT_CRITICAL(&s_power_guard);
    if (busy) return snprintf(out, n, "+ERR=busy\r\n");
#if PERIPH_AUDIO && !AUDIO_MIC_PASSTHROUGH_TEST
    bool audio_test = cmd == FT_AUTO || cmd == FT_AUDIO || cmd == FT_I2C;
    if (audio_test && (!s_audio_owner || xSemaphoreTake(s_audio_owner, 0) != pdTRUE)) {
        atomic_store(&s_factory_command_active, false);
        return snprintf(out, n, "+ERR=busy,audio\r\n");
    }
#endif
#if PERIPH_DISPLAY
    if (cmd != FT_DSP) platform_lvgl_nav_enable(false);
#endif
    int result = run_factory_command(cmd, out, n, user);
#if PERIPH_AUDIO && !AUDIO_MIC_PASSTHROUGH_TEST
    if (audio_test) xSemaphoreGive(s_audio_owner);
#endif
#if PERIPH_DISPLAY
    // Existing release gate consumes the tail of the last captured key.
    if (!s_factory_mode && cmd != FT_DSP) s_view_nav_restore_pending = true;
#endif
    if (cmd != FT_DSP) note_activity();
    atomic_store(&s_factory_command_active, false);
    return result;
}

// ===================== 设备端自助自检(上电按住确定键触发)=====================
// 结果/进度/交互提示全画在设备屏上,工人只碰设备;同时把 +TEST 行 printf 到串口,
// 旁边的 PC 治具被动记台账。运行于 app_run 主任务(阻塞到测完,停在结果屏)。
#if PERIPH_DISPLAY && PERIPH_BUTTON
// 等按键确认:确定键=通过(返回 2)/ 上键=不通过(返回 0);超时返回 -1。
static int st_wait_confirm(int timeout_ms) {
    s_btn_test_mask = 0; s_btn_test_active = true;
    int64_t t0 = esp_timer_get_time() / 1000, res = -1;
    while ((esp_timer_get_time() / 1000 - t0) < timeout_ms) {
        int m = s_btn_test_mask;
        if (m & (1 << 2)) { res = 2; break; }
        if (m & (1 << 0)) { res = 0; break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    s_btn_test_active = false;
    return (int)res;
}
// 单个键的进度段:达标(>=2)显示绿色 "键 OK",未达标显示 "键 x/2"。
// 绿色靠 LVGL label recolor 语法 #RRGGBB ...#(该 label 已开启 recolor)。
static void st_seg(char *o, int n, const char *name, int c) {
    if (c >= 2) snprintf(o, n, "#39FF88 %s OK#", name);
    else        snprintf(o, n, "%s %d/2", name, c);
}
// 逐键捕获:上/下/确定 各按 2 遍,屏上实时显示每键进度;达标段变绿 OK;全部达 2 返回 true。
static bool st_capture_twice(int timeout_ms) {
    for (int i = 0; i < 3; i++) s_btn_test_count[i] = 0;
    s_btn_test_active = true;
    int64_t t0 = esp_timer_get_time() / 1000;
    int last0 = -1, last1 = -1, last2 = -1;
    bool ok = false;
    while ((esp_timer_get_time() / 1000 - t0) < timeout_ms) {
        int c0 = s_btn_test_count[0], c1 = s_btn_test_count[1], c2 = s_btn_test_count[2];
        if (c0 > 2) c0 = 2;
        if (c1 > 2) c1 = 2;
        if (c2 > 2) c2 = 2;
        if (c0 != last0 || c1 != last1 || c2 != last2) {   // 进度变了才刷屏
            last0 = c0; last1 = c1; last2 = c2;
            char s0[24], s1[24], s2[24], line[96];
            st_seg(s0, sizeof s0, "上", c0);
            st_seg(s1, sizeof s1, "下", c1);
            st_seg(s2, sizeof s2, "确定", c2);
            snprintf(line, sizeof line, "%s  %s  %s", s0, s1, s2);
            if (platform_lvgl_lock(200)) { ui_selftest_prompt(line); platform_lvgl_unlock(); }
        }
        if (c0 >= 2 && c1 >= 2 && c2 >= 2) { ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    s_btn_test_active = false;
    return ok;
}
#endif

#if PERIPH_DISPLAY && PERIPH_BUTTON && PERIPH_BLE
static int s_st_fails;
static void st_emit_row(const char *name, char *buf, bool pass) {
    fputs(buf, stdout); fflush(stdout);                       // 串口:供 PC 被动记台账
    if (platform_lvgl_lock(200)) {
        ui_selftest_set_item(name, pass ? UI_ST_PASS : UI_ST_FAIL);
        platform_lvgl_unlock();
    }
    if (!pass) s_st_fails++;
}

static void run_onboard_selftest(void) {
    char sn[24] = {0};
    hal_identity_get_sn(s_id, sn, sizeof sn);
    printf("\r\n+TEST:SELFTEST,begin,sn=%s\r\n", sn[0] ? sn : "-"); fflush(stdout);

    s_st_fails = 0;
    if (platform_lvgl_lock(300)) {
        ui_selftest_open(sn);
        const char *pend[] = { "INFO", "I2C", "AUDIO", "BATT", "BLE", "ID", "DISP", "BTN" };
        for (int i = 0; i < 8; i++) ui_selftest_set_item(pend[i], UI_ST_RUN);
        platform_lvgl_unlock();
    }

    char buf[128]; int w; bool p;
    // 音频回环只跑一次;I2C 的 ES8311 复用其 codec 响应(与 AT+TEST? 同口径,免二次配置)
    int a_ret, a_mag, a_peak;
    ft_run_audio(&a_ret, &a_mag, &a_peak);
    int es8311 = (a_ret != -1 && a_ret != -2);

    p = ft_line_info(buf, sizeof buf, &w);                 st_emit_row("INFO", buf, p);
    ft_fmt_i2c(buf, sizeof buf, es8311, ft_batt_present(), &p);  st_emit_row("I2C", buf, p);
    ft_fmt_audio(buf, sizeof buf, a_ret, a_mag, a_peak, &p);     st_emit_row("AUDIO", buf, p);
    p = ft_line_batt(buf, sizeof buf, &w);                st_emit_row("BATT", buf, p);
    // 电量醒目显示(右上角大字,分色),整场自检可见
#if PERIPH_BATTERY
    if (platform_lvgl_lock(200)) { ui_selftest_battery(hal_battery_read_soc(s_batt)); platform_lvgl_unlock(); }
#endif
    p = ft_line_ble(buf, sizeof buf, &w);                 st_emit_row("BLE", buf, p);
    {   // ID:逐字段检测 cardid,屏上状态列用 S/K/P/H 标记(缺的显示 '-'),缺任一判 FAIL
        bool _sn, _key, _pk, _hw;
        identity_fields_present(&_sn, &_key, &_pk, &_hw);
        p = ft_line_id(buf, sizeof buf, &w);
        fputs(buf, stdout); fflush(stdout);               // 串口:供 PC 被动记台账
        char idf[5] = { _sn ? 'S' : '-', _key ? 'K' : '-', _pk ? 'P' : '-', _hw ? 'H' : '-', 0 };
        if (platform_lvgl_lock(200)) {
            ui_selftest_set_item_text("ID", p ? UI_ST_PASS : UI_ST_FAIL, idf);
            platform_lvgl_unlock();
        }
        if (!p) s_st_fails++;
    }

    // 屏幕自检:逐个纯色全屏(红/绿/蓝三原色),纯色无文字。确定=下一个/完成,上键=标记不良。
    // 三色都按确定通过 → DISP PASS;任一色按上键(或超时无人确认)→ FAIL。
    static const uint32_t DISP_COLORS[] = { 0xFF0000, 0x00FF00, 0x0000FF };
    bool disp_ok = true;
    if (platform_lvgl_lock(200)) { ui_selftest_prompt(""); platform_lvgl_unlock(); }   // 清掉残留提示
    for (int ci = 0; ci < 3; ci++) {
        if (platform_lvgl_lock(300)) { ui_selftest_color(DISP_COLORS[ci]); platform_lvgl_unlock(); }
        if (st_wait_confirm(30000) != 2) { disp_ok = false; break; }   // 上键/超时 → 不良
    }
    if (platform_lvgl_lock(300)) {
        ui_selftest_pattern(false);   // 清除纯色层
        ui_selftest_set_item("DISP", disp_ok ? UI_ST_PASS : UI_ST_FAIL);
        ui_selftest_prompt("");
        platform_lvgl_unlock();
    }
    snprintf(buf, sizeof buf, "+TEST:DISP,%s\r\n", disp_ok ? "PASS" : "FAIL");
    fputs(buf, stdout); fflush(stdout);
    if (!disp_ok) s_st_fails++;

    // 按键自检:上/下/确定 各按 2 遍,屏上实时显示每键进度(达标变绿 OK)
    if (platform_lvgl_lock(300)) {
        ui_selftest_prompt("各按两下: 上 0/2  下 0/2  确定 0/2");
        platform_lvgl_unlock();
    }
    bool btn_ok = st_capture_twice(20000);
    if (platform_lvgl_lock(300)) {
        ui_selftest_set_item("BTN", btn_ok ? UI_ST_PASS : UI_ST_FAIL);
        ui_selftest_prompt("");
        platform_lvgl_unlock();
    }
    snprintf(buf, sizeof buf, "+TEST:BTN,%s\r\n", btn_ok ? "PASS" : "FAIL");
    fputs(buf, stdout); fflush(stdout);
    if (!btn_ok) s_st_fails++;

    // 终判
    bool overall = (s_st_fails == 0);
    snprintf(buf, sizeof buf, "+TEST:RESULT,%s,fails=%d\r\n", overall ? "PASS" : "FAIL", s_st_fails);
    fputs(buf, stdout); fflush(stdout);
    if (platform_lvgl_lock(300)) { ui_selftest_result(overall); platform_lvgl_unlock(); }
    ESP_LOGI(TAG, "产测自检完成: %s (fails=%d)", overall ? "PASS" : "FAIL", s_st_fails);

#if PERIPH_AUDIO
    if (overall) {
        play_tone(TONE_PASS_OK);      // 良品:短促一声
    } else {
        // Keep FAIL visible, but sound only once. Accidental factory entry or
        // unattended manual checks must not leave an endless audible alarm.
        play_tone(TONE_FAIL_ALARM);
    }
#endif
}
#endif
#endif // PERIPH_BLE

#if PERIPH_BLE
// FoloToy 风格分组 AT 指令(全小写,大小写不敏感):
//   at+config=?                          查询配置
//   at+config=common,volume,<0-100>      设音量(profile + codec,持久化)
//   at+config=common,standby_time,<秒>   设空闲深睡时间(秒;内部按分钟存,粒度 60s;0=永不)
//   at+command=?                         列出支持的指令
//   at+command=binding,reset             头像/姓名/简介/音频/照片恢复默认并重启
//   at+command=restart,now / at+reboot   重启设备
// 说明:AT+CARDID(身份)/AT+TEST(产测)是本项目产线专用指令,FoloToy 无对应,保持原样。

#include <limits.h>
#include <stdarg.h>

// 大小写不敏感前缀:命中返回前缀之后的指针,否则 NULL。
static const char *at_after(const char *s, const char *pfx) {
    for (;;) {
        if (*pfx == '\0') return s;
        char a = *s, b = *pfx;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return NULL;
        s++; pfx++;
    }
}

// Match a complete command/token; prefixes alone cannot authorize reset/reboot.
static bool at_is(const char *text, const char *expected) {
    const char *tail = at_after(text, expected);
    return tail && *tail == '\0';
}

static bool at_decimal(const char *text, int maximum, int *out) {
    if (!text || !*text) return false;
    int value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        int digit = *text - '0';
        if (value > maximum / 10 ||
            (value == maximum / 10 && digit > maximum % 10)) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

// The console writes the returned byte count directly, so report actual bytes
// present rather than snprintf's untruncated length.
static int at_reply(char *out, int capacity, const char *format, ...) {
    if (!out || capacity <= 0) return 0;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, (size_t)capacity, format, args);
    va_end(args);
    if (written < 0) return 0;
    return written < capacity ? written : capacity - 1;
}

static int at_reboot(char *out, int n) {
    if (!out || n <= 0) return 0;
    int w = at_reply(out, n, "+OK reboot\r\n");
    fwrite(out, 1, (size_t)w, stdout); fflush(stdout);   // 应答先落串口再复位
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return 0;   // 不会到达
}

static int at_binding_reset(char *out, int n) {
    if (!out || n <= 0) return 0;
#if PERIPH_DISPLAY
    if (jpeg_view_decode_busy() || atomic_load(&s_jpeg_upload_active) || capture_ui_blocked())
        return at_reply(out, n, "+ERR=busy\r\n");
#endif
    profile_data_t defaults;
    profile_ctl_init(&defaults);
    memcpy(s_profile.name, defaults.name, sizeof s_profile.name);
    memcpy(s_profile.subtitle, defaults.subtitle, sizeof s_profile.subtitle);
    memcpy(s_profile.avatar_name, defaults.avatar_name, sizeof s_profile.avatar_name);
    s_profile.img_mode = defaults.img_mode;

    bool profile_ok = save_profile();
    bool audio_ok = true;
    bool images_ok = true;
#if PERIPH_AUDIO
    audio_ok = s_kv && hal_kv_erase(s_kv, BOOT_RTTTL_KV_KEY) == 0;
#endif
#if PERIPH_DISPLAY
    int avatar_clear = jpeg_store_clear(JPEG_SLOT_AVATAR);
    int photo_clear = jpeg_store_clear(JPEG_SLOT_FULL);
    images_ok = avatar_clear == 0 && photo_clear == 0;
#endif
    if (!profile_ok || !audio_ok || !images_ok)
        return at_reply(out, n, "+ERR=binding_reset,profile=%d,audio=%d,images=%d\r\n",
                        profile_ok, audio_ok, images_ok);
    int w = at_reply(out, n, "+OK binding_reset,reboot\r\n");
    fwrite(out, 1, (size_t)w, stdout); fflush(stdout);   // 应答先落串口再复位
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return 0;   // 不会到达
}

// Local, physically connected fixture entry. Uses the ordinary home/plays
// lifetime; no debugger, hidden voice worker, credentials or settings writes.
static int at_test_entry(bool voice, char *out, int n) {
#if PERIPH_DISPLAY
    taskENTER_CRITICAL(&s_power_guard);
    bool busy = atomic_load(&s_sleep_preparing) || atomic_load(&s_deep_sleep_entering) ||
                atomic_load(&s_factory_command_active);
    if (!busy) atomic_store(&s_factory_command_active, true);
    taskEXIT_CRITICAL(&s_power_guard);
    if (busy) return at_reply(out, n, "+ERR=busy\r\n");
    int result;
    bool touched = false;
    if (capture_ui_blocked() || jpeg_view_is_active() || ui_album_active() ||
        s_factory_mode || s_view_nav_restore_pending || !xiaozhi_stopped()) {
        result = at_reply(out, n, "+ERR=busy\r\n");
    } else if (s_button_guide_active) {
        s_button_guide_exit = true;
        touched = true;
        result = at_reply(out, n, "+ERR=busy,home_opening\r\n");
    } else if (!platform_lvgl_lock(100)) {
        result = at_reply(out, n, "+ERR=busy,display\r\n");
    } else {
        bool accepted = false;
        if (voice) {
            if (!app_plays_active() && ui_profile_is_active() && app_plays_open(s_profile.game_best))
                platform_lvgl_nav_enable(false);
            accepted = app_plays_voice_check();
        } else accepted = !app_plays_active() && ui_profile_is_active();
        platform_lvgl_unlock();
        touched = accepted;
        result = at_reply(out, n, accepted ?
            (voice ? "+OK voice_check_requested,limit_ms=45000\r\n" : "+OK test_home\r\n") :
            "+ERR=busy\r\n");
    }
    if (touched) note_activity();
    atomic_store(&s_factory_command_active, false);
    return result;
#else
    (void)voice;
    return at_reply(out, n, "+ERR=unsupported\r\n");
#endif
}

static int on_at_command(const char *line, int len, char *out, int n, void *user) {
    (void)user;
    if (!out || n <= 0) return 0;
    if (!line || len < 0) return at_reply(out, n, "+ERR=arg\r\n");
    // Trim before the bounded copy; never execute a truncated or embedded-NUL command.
    while (len > 0 && (*line == ' ' || *line == '\t')) { line++; len--; }
    while (len > 0 && (line[len-1] == ' ' || line[len-1] == '\t' ||
                       line[len-1] == '\r' || line[len-1] == '\n')) --len;
    char cmd[160];
    if (len >= (int)sizeof(cmd) || memchr(line, '\0', (size_t)len))
        return at_reply(out, n, "+ERR=arg(command length)\r\n");
    memcpy(cmd, line, (size_t)len); cmd[len] = '\0';

    const char *a, *b, *c;

    if (at_is(cmd, "at+reboot")) return at_reboot(out, n);

    if ((a = at_after(cmd, "at+command="))) {
        if (a[0] == '?' && a[1] == '\0')
            return at_reply(out, n,
                "+COMMAND: at+config=? | at+config=common,volume,<0-100> | "
                "at+config=common,standby_time,<秒> | at+command=restart,now | at+reboot | "
                "at+command=binding,reset | at+command=voice,stop/check/interrupt | "
                "at+config=common,natural_interrupt,<?/0/1> | at+command=test,home | "
                "AT+CARDID?/= | AT+TEST?/=<项>\r\n");
        if (at_is(a, "binding,reset")) return at_binding_reset(out, n);
        if (at_is(a, "test,home")) return at_test_entry(false, out, n);
        if (at_is(a, "voice,check")) return at_test_entry(true, out, n);
        if (at_is(a, "voice,end") || at_is(a, "voice,resume")) {
            xz_view_t view; xiaozhi_snapshot(&view);
            if (xiaozhi_stopped()) return at_reply(out, n, "+ERR=busy\r\n");
            if (at_is(a, "voice,resume") && view.phase == XZ_STANDBY) {
                xiaozhi_resume(); return at_reply(out, n, "+OK voice_resuming\r\n");
            }
            if (at_is(a, "voice,end") && (view.phase == XZ_LISTEN || view.phase == XZ_SPEAK || view.phase == XZ_THINK || view.phase == XZ_READY)) {
                xiaozhi_talk(); return at_reply(out, n, "+OK voice_ending\r\n");
            }
            return at_reply(out, n, "+ERR=busy\r\n");
        }
        if (at_is(a, "voice,interrupt")) {
            xiaozhi_interrupt();
            return at_reply(out, n, "+OK voice_interrupt_requested\r\n");
        }
        if (at_is(a, "voice,stop")) {
            xiaozhi_stop();
            return at_reply(out, n, "+OK voice_stop_requested\r\n");
        }
        if (at_is(a, "restart") || at_is(a, "restart,now")) return at_reboot(out, n);   // restart / restart,now
        return at_reply(out, n, "+ERR=command(未知,用 at+command=? 查看)\r\n");
    }

    if ((a = at_after(cmd, "at+config="))) {
        if (a[0] == '?' && a[1] == '\0') {
            char sn[16] = {0}, hw[16] = {0}, key[65] = {0};
            hal_identity_get_sn(s_id, sn, sizeof sn);
            hal_identity_get_hw_ver(s_id, hw, sizeof hw);
            // 回显每设备 DeviceSecret 明文,便于产线核对台账。注意:这是串口(需物理接触)、
            // 且该值本就存在 cardid_ledger.csv 里;仍属敏感——勿把此输出转贴到公共渠道。
            // (ProductKey 不回显,与 AT+CARDID? 一致,只在需要时给 pk 指纹。)
            hal_identity_get_key(s_id, key, sizeof key);
            return at_reply(out, n,
                "+CONFIG: sn=%s,key=%s,hw=%s,provisioned=%d,volume=%d,standby_time=%d,name=%s\r\n",
                sn[0] ? sn : "-", key[0] ? key : "-", hw[0] ? hw : "-",
                hal_identity_is_provisioned(s_id), s_profile.volume,
                s_profile.sleep_min * 60, s_profile.name);
        }
        if (!(b = at_after(a, "common,")))
            return at_reply(out, n, "+ERR=arg(仅支持 common 组)\r\n");

        if ((c = at_after(b, "natural_interrupt,"))) {
            if (at_is(c, "?")) {
                xz_view_t view; xiaozhi_snapshot(&view);
                return at_reply(out, n, "+OK natural_interrupt=%d\r\n", view.natural_interrupt);
            }
            int value;
            if (!at_decimal(c, 1, &value)) return at_reply(out, n, "+ERR=arg(natural_interrupt 0..1)\r\n");
            if (!xiaozhi_stopped()) return at_reply(out, n, "+ERR=busy\r\n");
            bool saved = xiaozhi_set_natural_interrupt(value != 0);
            return at_reply(out, n, saved ? "+OK natural_interrupt=%d\r\n" : "+ERR=save\r\n", value);
        }

        if ((c = at_after(b, "volume,"))) {
            int v;
            if (!at_decimal(c, 100, &v)) return at_reply(out, n, "+ERR=arg(volume 0..100)\r\n");
            int previous = s_profile.volume;
            s_profile.volume = v;
            if (!save_profile()) {
                s_profile.volume = previous;
                return at_reply(out, n, "+ERR=save\r\n");
            }
#if PERIPH_AUDIO
            audio_service_set_volume(&s_au, v);
#endif
            return at_reply(out, n, "+OK volume=%d\r\n", v);
        }
        if ((c = at_after(b, "brightness,"))) {
            if (at_is(c, "?")) return at_reply(out, n, "+OK brightness=%u\r\n", plays_get_brightness(NULL));
            int value;
            if (!at_decimal(c, 100, &value) || value < BRIGHTNESS_MIN)
                return at_reply(out, n, "+ERR=arg(brightness 10..100)\r\n");
            bool saved = plays_set_brightness(NULL, (unsigned)value);
            return at_reply(out, n, saved ? "+OK brightness=%d\r\n" : "+ERR=save\r\n", value);
        }
        if ((c = at_after(b, "standby_time,"))) {
            if (at_is(c, "?")) return at_reply(out, n, "+OK standby_time=%u\r\n", plays_get_sleep_minutes(NULL) * 60u);
            int sec;
            if (!at_decimal(c, INT_MAX, &sec)) return at_reply(out, n, "+ERR=arg(standby_time>=0,单位秒,0=永不)\r\n");
            int mn = sec / 60 + (sec % 60 >= 30);                       // 内部分钟,四舍五入(粒度 60s)
            if (mn > SLEEP_MIN_MAX) mn = SLEEP_MIN_MAX;
            int previous = s_profile.sleep_min;
            s_profile.sleep_min = mn;
            if (!save_profile()) {
                s_profile.sleep_min = previous;
                return at_reply(out, n, "+ERR=save\r\n");
            }
            note_activity();                                // 改配置复位空闲计时,避免立刻触发
            return at_reply(out, n, "+OK standby_time=%d\r\n", mn * 60);
        }
        return at_reply(out, n, "+ERR=key(未知配置项,用 at+config=? 查看)\r\n");
    }

    return at_reply(out, n, "+ERR=unknown\r\n");
}
#endif

void app_run(void) {
    static const board_config_t board = BOARD_CONFIG_DEFAULT();
    const bool woke_from_deep_sleep =
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;

#if BOARD_M5STACK_STOPWATCH
    // Initialize StopWatch hardware early: M5IOE1 and M5PM1 must be ready
    // before display, audio, and battery initialization
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = (gpio_num_t)board.i2c_sda,
        .scl_io_num = (gpio_num_t)board.i2c_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));
    
    if (stopwatch_hardware_init(i2c_bus) != 0) {
        ESP_LOGE(TAG, "Failed to initialize StopWatch hardware");
        abort();
    }
#endif

    // ui_model 始终存在,作为各业务的共享状态。
    static ui_model_t model; ui_model_init(&model);

    seed_default_time();   // 时钟先有个默认起点(未同步也走时),BLE 对时后覆盖
    log_heap("00-boot");   // 内存基线:各子系统初始化前

    // 产测触发:上电按住确定键 → 进设备自助自检(须在建按键前读,ADC unit 用完即释放)。
    bool factory_mode = false;
    bool show_boot_guide = false;
#if PERIPH_BUTTON
    // 深睡唤醒时确定键可能仍被按住，不能把它误判成上电自检手势。
    factory_mode = !woke_from_deep_sleep && (platform_button_boot_index(&board) == 2);
    s_factory_mode = factory_mode;   // 置早:on_btn_raw 从第一下按键起就屏蔽业务
    if (factory_mode) ESP_LOGW(TAG, "检测到上电按住确定键 → 进入产测自检模式");
    hal_button_t *btn = platform_create_button(&board);
    s_button = btn;
#endif

#if PERIPH_DISPLAY
    companion_resources_init();
    s_disp = platform_create_display(&board);
    /* Validate the entire read-only art pack once, before UI creation and
     * outside the LVGL lock. Missing/mismatched packs leave safe empty sources. */
    const void *pet_art_data = NULL;
    size_t pet_art_bytes = 0;
    if (!platform_pet_art_map(&pet_art_data, &pet_art_bytes) ||
        !bobo_art_bind(pet_art_data, pet_art_bytes)) {
        ESP_LOGW(TAG, "Shared artwork unavailable: install the matching petart resource for this build");
    }
    struct _lv_display_t *lvdisp = platform_lvgl_init(s_disp, &board);
    bool first_frame_armed = false;
#if PERIPH_BUTTON
    // 自检模式不挂 LVGL 导航:否则底层 dock 会跟着按键动焦点/误触发。自检只经 on_btn_raw 捕获。
    if (!factory_mode) {
        // 深睡唤醒也必须在建主页前创建默认 group，否则主页控件不会加入导航组。
        // 唤醒键尚未释放时先禁用 indev，避免确定键释放被识别为主页点击。
        if (woke_from_deep_sleep || PASSPORT_XIAOZHI_EDITION) platform_lvgl_nav_enable(false);
        platform_lvgl_attach_buttons(btn, lvdisp);   // 上=prev 下=next 确定=enter(设默认导航 group)
    }
    hal_button_on_event(btn, on_btn_raw, NULL);      // 原始按键→游戏/自检捕获
#endif
    if (platform_lvgl_lock(0)) {                 // 建界面(dock 磁贴自动入导航 group)
        // Factory boot stays on its result screen. Keeping an unused home
        // underneath it consumes the layer workspace needed by rounded rendering.
        if (!factory_mode) {
            ui_profile_create();
            ui_profile_set_on_action(on_dock_action, NULL);
        }
        // 2=GAME / 3=ALBUM 的值文本在 ui_profile.c 建磁贴时就用图标填好了,这里不覆盖
        // Cold boot and deep wake share the same first-home-frame condition.
        // Factory self-test still lights immediately without waiting for an avatar.
#if PERIPH_BLE
        if (!factory_mode) {
            s_wake_display = lvdisp;
            first_frame_armed = true; // Registered for the avatar-completion gate.
        } else
#endif
            first_frame_armed = platform_lvgl_arm_first_frame(s_disp, lvdisp, BRIGHT_PCT);
        platform_lvgl_unlock();
    }
    if (!first_frame_armed) ESP_LOGE(TAG, "首页首帧事件未就绪，保持背光关闭以避免白屏");
    log_heap("10-display+lvgl");   // 含 LVGL 绘制缓冲 + 主界面对象,通常是最大头之一
#endif

#if PERIPH_BLE
    // 身份要在 BLE 之前建立:BLE 特征和后续的 token 广播都依赖它。
    s_id = platform_create_identity(&board);
    {
        char sn[16] = {0};
        hal_identity_get_sn(s_id, sn, sizeof sn);
        if (!hal_identity_is_provisioned(s_id)) {
            ESP_LOGW(TAG, "未写入 Key/ProductKey,小程序绑定与 token 广播不可用。"
                          "请用 AT+CARDID= 指令写入或烧录 cardid 分区。SN=%s", sn);
        } else {
            ESP_LOGI(TAG, "身份自检通过 SN=%s", sn);
        }
    }
    profile_ctl_init(&s_profile);
    s_kv = platform_create_kv(&board);
    {   // 开机从 NVS 载入上次配置(含头像 seed / token);无记录则用出厂默认
        uint8_t blob[PROFILE_BLOB_SIZE]; int got = 0;
        if (s_kv && hal_kv_get(s_kv, "profile", blob, sizeof blob, &got) == 0 &&
            profile_deserialize(&s_profile, blob, got) == 0 && blob[0] != PROFILE_BLOB_VER) {
            if (!save_profile()) ESP_LOGW(TAG, "profile 配置版本迁移落盘失败");
        }
    }
    {   // token 广播:self_target 就是本机 MAC 原始字节(与 SN 是同一个值的两种表示)
        uint8_t self_target[6];
        esp_read_mac(self_target, ESP_MAC_WIFI_STA);
        token_bcast_init(&s_tb, self_target, token_mac_esp, s_id);
        profile_bcast_init(&s_pb, self_target, token_mac_esp, s_id);
        profile_batch_bcast_init(&s_pbb, token_mac_esp, s_id);
        // 身份未完整 provision 时禁用广播。HMAC 密钥实际只使用 DeviceSecret,
        // 但产品激活状态按 DeviceSecret + ProductKey 均存在判定。
        s_tb.enabled = hal_identity_is_provisioned(s_id);
        ESP_LOGI(TAG, "token 广播接收 %s",
                 s_tb.enabled ? "已启用" : "已禁用(身份未完整 provision)");
    }
    log_heap("20-ble-controller");   // NimBLE 控制器+主机栈:BLE 堆的大头,低水位敏感
    s_scan = platform_create_ble_scan(&board);
    hal_ble_scan_on_match(s_scan, on_ble_match, NULL);   // 建在此处:s_tb 已就绪,广播不会打到未初始化的结构体
    provision_console_set_changed_cb(on_identity_changed, NULL);
    provision_console_set_test_cb(on_factory_test, NULL);   // AT+TEST 产测指令
    provision_console_set_at_cb(on_at_command, NULL);       // at+config/at+command/at+reboot
    provision_console_start(s_id);
#if PERIPH_DISPLAY
    if (platform_lvgl_lock(0)) {   // 用载入后的 profile 初始化界面
        ui_profile_set_name(s_profile.name);
        ui_profile_set_title(s_profile.role);
        ui_profile_set_intro(s_profile.subtitle);
        platform_lvgl_unlock();
    }
#endif
    // 设备名用本机 SN:网关扫一下就知道是哪张卡,不必靠台账反查。
    char dev_sn[16] = {0};
    hal_identity_get_sn(s_id, dev_sn, sizeof dev_sn);
    hal_config_t *cfg_src = platform_create_ble_config(&board, dev_sn);
    s_cfg = cfg_src;
#if PERIPH_DISPLAY
    capture_orchestration_init(lvdisp);
    atomic_fetch_or(&s_profile_ui_pending,
                    PROFILE_UI_REFRESH_NAME | PROFILE_UI_REFRESH_TITLE | PROFILE_UI_REFRESH_INTRO);
    profile_ui_flush_pending();
#endif
    hal_config_on_message(cfg_src, on_cfg_message, NULL);
#if PERIPH_DISPLAY
    hal_config_on_connect(cfg_src, on_ble_connected, NULL);
    hal_config_on_disconnect(cfg_src, on_ble_disconnected, NULL);
#endif
    publish_identity();             // 推送身份 blob(BLE 特征 …0012)
    publish_gamescore();            // 推送游戏积分 blob(BLE 特征 …0013)
#if PERIPH_DISPLAY
    if (avatar_store_init() != 0) {
        ESP_LOGE(TAG, "内置头像资源包初始化失败");
    } else if (s_profile.avatar_name[0] != '\0' && !avatar_store_has(s_profile.avatar_name)) {
        char first_name[16] = {0};
        if (avatar_store_first_name(first_name) == 0) {
            ESP_LOGI(TAG, "已保存头像 '%s' 不存在,迁移到 '%s'",
                     s_profile.avatar_name, first_name);
            memcpy(s_profile.avatar_name, first_name, sizeof s_profile.avatar_name);
            if (!save_profile()) ESP_LOGW(TAG, "头像回退落盘失败");
        }
    }
    // Avatar restore now gates the wake frame, so a transient lock collision
    // must not silently skip creation of its decode timer.
    while (!platform_lvgl_lock(500)) vTaskDelay(pdMS_TO_TICKS(50));
    jpeg_view_init(s_disp);
    platform_lvgl_unlock();
    jpeg_view_set_result_cb(on_jpeg_result, NULL);   // 解码结果 → 回报小程序
    jpeg_view_set_avatar_cb(on_jpeg_avatar, NULL);   // 头像位模式 → 显示到头像位
    jpeg_view_set_screen_cb(on_image_screen, NULL);
    jpeg_view_set_can_show_cb(can_show_fullscreen, NULL);
    jpeg_rx_init(&s_jpeg_rx);
    // 注意:开机头像恢复刻意不放这里 —— 头像 PNG 解码
    // 有 43KB 瞬时峰值,若在 boot 尾发起会和 BLE/音频初始化抢内存(实测把 free 压到 8.5KB)。
    // 改到 app_run 末尾、全部子系统就绪、free 回到 ~58KB 之后再请求(见文件末)。
#endif
    show_boot_guide = !PASSPORT_XIAOZHI_EDITION && !factory_mode && !woke_from_deep_sleep && !has_user_configured_content();
    if (show_boot_guide)
        ESP_LOGI(TAG, "用户头像/姓名/简介/音频/照片均为默认,显示扫码绑定页");
    else if (!PASSPORT_XIAOZHI_EDITION && !factory_mode && !woke_from_deep_sleep)
        ESP_LOGI(TAG, "检测到用户配置内容,跳过扫码绑定页直接进入主页");
    log_heap("30-ble-gatt+img");   // GATT 服务 + 图片子系统就绪(头像解码所需大块看 max_blk)
#endif

#if PERIPH_AUDIO
    hal_audio_t *audio = platform_create_audio(&board);
    audio_service_init(&s_au, audio);
    audio_service_set_volume(&s_au, s_profile.volume);   // 从 profile 取音量(默认 95%,BLE 可调)
#if AUDIO_MIC_PASSTHROUGH_TEST
    {   // 诊断模式:麦→扬声器直通。此模式独占音频,乐谱播放不可用(见 board_config.h 注释)
        BaseType_t tok = xTaskCreate(mic_passthrough_task, "mic_pass", 4096, NULL, 5, NULL);
        ESP_LOGW(TAG, "音频模式=麦克风直通诊断(board_config.h 开关=1); task=%s 空闲堆=%u"
                      " —— 此模式下乐谱播放不可用,要用乐谱请把该宏改回 0",
                 (tok == pdPASS ? "OK" : "创建失败"), (unsigned)esp_get_free_heap_size());
    }
#elif PERIPH_BLE
    score_rx_init(&s_score_rx, s_score_rx_buf, SCORE_MAX);
    s_music_sig = xSemaphoreCreateBinary();
    s_music_idle_sig = xSemaphoreCreateBinary();
    s_audio_owner = xSemaphoreCreateMutex();
    atomic_store(&s_music_playing, false);
    // 栈 4096:合成缓冲在静态区,栈只需容纳 esp_codec_dev/i2s 驱动调用链(含换采样率时的
    // close+open 重配)。真机实测峰值约 2624B(看日志 music_st=剩余水位),留 ~1.4KB 余量。
    BaseType_t tok = xTaskCreate(music_player_task, "music", 4096, NULL, 5, &s_music_task);
    ESP_LOGI(TAG, "音频模式=乐谱播放; sig=%p task=%s cap=%d 空闲堆=%u",
             (void *)s_music_sig, (tok == pdPASS ? "OK" : "创建失败"),
             s_score_rx.cap, (unsigned)esp_get_free_heap_size());
    if (!s_music_sig || !s_music_idle_sig)
        ESP_LOGE(TAG, "音频信号量创建失败(堆不足),乐谱播放/休眠同步不可用");
#else
    ESP_LOGW(TAG, "音频已初始化但无播放路径(PERIPH_BLE=0)");
#endif
    log_heap("40-audio");   // 音频 codec + 乐谱/片段任务栈(6144)+ SPIFFS 挂载后
#endif

#if PERIPH_DISPLAY
    app_plays_config_t plays_config = {.exit = on_plays_exit, .portal_activity = note_portal_activity};
    plays_config.get_brightness = plays_get_brightness;
    plays_config.set_brightness = plays_set_brightness;
    plays_config.preview_brightness = plays_preview_brightness;
    plays_config.get_sleep_minutes = plays_get_sleep_minutes;
    plays_config.set_sleep_minutes = plays_set_sleep_minutes;
#if PERIPH_BLE
    plays_config.identity = s_id;
    plays_config.game_result = on_game_result;
    if (PASSPORT_XIAOZHI_EDITION) plays_config.voice_opened = on_boot_voice_opened;
#endif
#if PERIPH_AUDIO && PERIPH_BLE && !AUDIO_MIC_PASSTHROUGH_TEST
    plays_config.audio = s_au.audio;
    plays_config.acquire_audio = plays_acquire_audio;
    plays_config.release_audio = plays_release_audio;
    plays_config.get_volume = plays_get_volume;
    plays_config.set_volume = plays_set_volume;
#endif
    if (!factory_mode && !app_plays_init(&plays_config)) ESP_LOGE(TAG, "玩法任务未能初始化");
#endif
#if PERIPH_BUTTON
    s_button_wakeup_gpio = board.btn_wakeup_gpio;
    note_activity();   // 开机算一次活动,从此刻起计空闲
    {
        const esp_timer_create_args_t ia = { .callback = idle_check_cb, .name = "idle" };
        esp_timer_handle_t ih;
        if (esp_timer_create(&ia, &ih) == ESP_OK)
            esp_timer_start_periodic(ih, 250 * 1000);   // 250ms 巡检,唤醒延迟 ≤0.25s
#if DEEP_SLEEP_FORCE_DISABLED
        ESP_LOGW(TAG, "自动深睡已通过临时调试开关强制禁用");
#elif PERIPH_BLE
        ESP_LOGI(TAG, "空闲深睡:%d 分钟(任意功能键唤醒;0=永不)。"
                      "at+config=common,standby_time 可改", s_profile.sleep_min);
#else
        ESP_LOGI(TAG, "空闲深睡:5 分钟(任意功能键唤醒;BLE 关闭时使用默认值)");
#endif
    }
#endif

#if PERIPH_BATTERY
    s_batt = platform_create_battery(&board);
    if (s_batt) {
        battery_init(&s_batt_f);
        battery_prime_on_boot();               // 首页显示前快速重试，尽量首帧即有真实电量
        const esp_timer_create_args_t bta = { .callback = battery_timer_cb, .name = "batt" };
        esp_timer_handle_t bth = NULL;
        esp_err_t batt_timer_err = esp_timer_create(&bta, &bth);
        if (batt_timer_err == ESP_OK)
            batt_timer_err = esp_timer_start_periodic(bth, (uint64_t)BATT_SAMPLE_MS * 1000);
        if (batt_timer_err == ESP_OK) {
            ESP_LOGI(TAG, "电量检测已启用(每 %dms 直接读取 CW2017 SOC,无软件平滑)", BATT_SAMPLE_MS);
        } else {
            if (bth) esp_timer_delete(bth);
            ESP_LOGE(TAG, "电量定时采样启动失败: %s", esp_err_to_name(batt_timer_err));
        }
    } else {
        ESP_LOGW(TAG, "电量检测创建失败,状态栏显示未知电量");
        show_unknown_battery();
    }
#else
    show_unknown_battery();   // 电量检测关闭:不显示虚拟电量
#endif

    ui_model_set_status(&model, "ready");
    ui_model_set_battery(&model, 100);
    log_heap("99-ready");   // 全部就绪的稳态空闲堆:头像 PNG(~43KB 大块)能否解码看这里的 max_blk

#if PERIPH_DISPLAY && PERIPH_BUTTON && PERIPH_BLE
    if (factory_mode) {
        run_onboard_selftest();   // 自检模式:所有外设已就绪,跑完停在结果屏,不播开机音
        return;
    }
#endif
    (void)factory_mode;
#if PERIPH_DISPLAY && PERIPH_BUTTON && PERIPH_BLE
    // Both cold boot and deep wake take this route. xiaozhi_start(false) uses
    // saved Wi-Fi when available and opens provisioning otherwise; it never
    // clears credentials. Factory boot has already returned above.
    if (!woke_from_deep_sleep) show_startup_brand();
    if (start_boot_xiaozhi()) {
        ESP_LOGI(TAG, "小智版启动：进入 Wi-Fi / 激活 / 对话流程");
        return;
    }
#endif
#if PERIPH_AUDIO && PERIPH_BLE && !AUDIO_MIC_PASSTHROUGH_TEST
    // The QR guide below waits for a key or BLE connection. Queue boot music
    // before entering that wait, so it is heard at startup, including first use.
    if (!woke_from_deep_sleep) {
        play_saved_boot_rtttl();
    } else {
        ESP_LOGI(TAG, "深睡唤醒,跳过 RTTTL 开机音乐");
    }
#endif
#if PERIPH_DISPLAY && PERIPH_BUTTON && PERIPH_BLE
    if (show_boot_guide) show_boot_button_guide();
    if (woke_from_deep_sleep) {
        // Start restore before waiting for the wake key to be released: holding
        // the key must not delay the correct first visible homepage.
        restore_boot_avatar();
        // 导航已在主页创建前挂载但保持禁用；释放并消抖后只需重新启用。
        ESP_LOGI(TAG, "功能键深睡唤醒 → 跳过扫码绑定页,直接进入主页");
        while (platform_button_any_pressed(btn)) vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelay(pdMS_TO_TICKS(80));
        while (!platform_lvgl_lock(500)) vTaskDelay(pdMS_TO_TICKS(50));
        platform_lvgl_nav_enable(true);
        platform_lvgl_unlock();
        note_activity();
    }
#else
    (void)show_boot_guide;
#endif
#if PERIPH_DISPLAY && PERIPH_BLE
    // 开机恢复头像:延后到此处发起(全部子系统已就绪,boot 尾的临时占用已释放,free 回到
    // ~58KB),避免头像 PNG 解码的瞬时内存和 boot 尾/BLE 事件抢内存。
#if PERIPH_BUTTON
    if (!woke_from_deep_sleep && !show_boot_guide) restore_boot_avatar();
#else
    restore_boot_avatar();
#endif
#endif
}
