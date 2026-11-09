// components/presentation/src/ui_profile.c —— 档案页 + 底部横向 dock(头像位 + 按键导航)
// 首页:状态栏 + 头像 + 昵称/头衔/四行简介 + dock。
#include "presentation/ui_profile.h"
#include "presentation/ui_licenses.h"
#include "presentation/ui_round_layout.h"
#include "lvgl.h"
#include "presentation/ui_theme.h"
#include "presentation/bobo_art.h"
#include "presentation/pixel_sprite.h"
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

// 中文字体(tools/gen_cn_font.py 生成:24px 昵称字体含完整 GB2312 汉字,2bpp,数据在 flash)
LV_FONT_DECLARE(lv_font_cn_16);
LV_FONT_DECLARE(lv_font_cn_24);

// ---- 屏幕布局参数(ST7789P3 T200H7-C14-05:240×320 竖屏)----
// 状态栏顶部对齐、dock 底部对齐,中部内容块(头像+身份栏)在两者之间垂直居中。
#define SCREEN_W  240
#define SCREEN_H  320
#define STATUS_H  40    // 顶部状态栏占高
#define DOCK_H    64    // 两个入口，无额外底部提示行

// ---- 调色板 ----
#define COL_BG      lv_color_hex(UI_BG)
#define COL_PANEL   lv_color_hex(UI_PANEL)
#define COL_PANEL_F lv_color_hex(0x12261C)   // original demo focused green
#define COL_BORDER  lv_color_hex(UI_LINE)
#define COL_GREEN   lv_color_hex(UI_ACCENT)
#define COL_DISCONNECTED lv_color_hex(0xE0463A)
#define COL_GREEN2  lv_color_hex(0x46C77D)
#define COL_WHITE   lv_color_hex(UI_INK)
#define COL_GRAY    lv_color_hex(UI_MUTED)

// ---- 头像位尺寸(上传的头像图缩放到此矩形内显示;没有传过图则此区域留空)----
#define AVA_W 96
#define AVA_H 156

typedef struct {
    lv_obj_t *time_lbl, *date_lbl;
    lv_obj_t *online_dot;
    ui_battery_icon_t battery;
    bool ble_shown;
    lv_obj_t *name_lbl, *title_lbl, *intro_lbl;
    lv_timer_t *clock;
    lv_obj_t *img_cont, *img;    // 头像位图片(裁剪容器+lv_image);NULL=当前空着
    lv_obj_t *ava_ph;            // 头像位默认占位(黑色半身剪影);有真实头像时隐藏
    int       ava_x, ava_y;      // 头像位左上角坐标(建界面时算好,给 show_image 定位用)
    lv_obj_t *dock_val[8];
    lv_obj_t *dock;              // dock 容器
    lv_obj_t *tile[2];           // 玩法与相册；博博始终在玩法内
} profile_t;
static profile_t P;
static atomic_bool s_ble_connected = ATOMIC_VAR_INIT(false);

static ui_action_cb_t s_action_cb;
static void          *s_action_user;

static lv_obj_t *make_label(lv_obj_t *p, const char *txt, const lv_font_t *f, lv_color_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}
static lv_obj_t *make_box(lv_obj_t *p, int w, int h, lv_color_t bg) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static lv_obj_t *make_bluetooth(lv_obj_t *parent) {
    static const lv_point_precise_t rune[] = {{1,5},{12,14},{6,19},{6,1},{12,6},{1,15}};
    static const lv_point_precise_t slash[] = {{1,2},{13,18}};
    lv_obj_t *box = make_box(parent, 14, 20, COL_BG);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_t *line = lv_line_create(box);
    lv_line_set_points(line, rune, 6);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, COL_GRAY, 0);
    line = lv_line_create(box);
    lv_line_set_points(line, slash, 2);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_color(line, COL_GRAY, 0);
    return box;
}

static void refresh_ble_status(void) {
    bool connected = atomic_load(&s_ble_connected);
    if (!P.online_dot || connected == P.ble_shown) return;
    lv_obj_set_style_line_color(lv_obj_get_child(P.online_dot, 0),
                                connected ? lv_color_hex(0x79BFFF) : COL_GRAY, 0);
    if (connected) lv_obj_add_flag(lv_obj_get_child(P.online_dot, 1), LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(lv_obj_get_child(P.online_dot, 1), LV_OBJ_FLAG_HIDDEN);
    P.ble_shown = connected;
}

void ui_profile_set_ble_connected(bool connected) {
    atomic_store(&s_ble_connected, connected);
}

// 在头像位置(96×156)显示图片,等比缩放完整装下(contain),常驻。
// dsc=NULL 则移除图片,头像位留空。须在 LVGL 锁内(或 LVGL 任务)调用。
static lv_obj_t *s_scr_ref;

// 头像位默认占位:黑色人物上半身剪影(头 + 肩,底部与两侧被头像框裁掉 → 半身)。
// show=false 隐藏(有真实头像时)。须在 LVGL 锁内调用。
static void avatar_placeholder(bool show) {
    if (!s_scr_ref) return;
    if (!show) { if (P.ava_ph) lv_obj_add_flag(P.ava_ph, LV_OBJ_FLAG_HIDDEN); return; }
    if (P.ava_ph) { lv_obj_remove_flag(P.ava_ph, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_t *box = make_box(s_scr_ref, AVA_W, AVA_H, COL_PANEL);
    lv_obj_set_pos(box, P.ava_x, P.ava_y);
    lv_obj_set_style_radius(box, 10, 0);
    pixel_sprite_init();
    lv_obj_t *pet = lv_image_create(box); lv_image_set_src(pet, &bobo_art_wave);
    lv_obj_set_pos(pet, 8, 30);
    ui_theme_text(box, "你好，旅伴", 8, 123, 83, &ui_font_body, UI_MUTED);
    P.ava_ph = box;
}

void ui_profile_show_image(const void *dsc_) {
    const lv_image_dsc_t *dsc = dsc_;
    if (!s_scr_ref) return;
    if (dsc == NULL) {                       // 清空头像位 → 恢复默认剪影占位
        if (P.img_cont) { lv_obj_delete(P.img_cont); P.img_cont = NULL; P.img = NULL; }
        avatar_placeholder(true);
        return;
    }
    avatar_placeholder(false);               // 有真实头像:隐藏占位
    if (!P.img_cont) {                       // 首次:建裁剪容器(超出边界的图会被容器裁掉)
        P.img_cont = lv_obj_create(s_scr_ref);
        lv_obj_remove_style_all(P.img_cont);
        lv_obj_set_size(P.img_cont, AVA_W, AVA_H);
        lv_obj_set_pos(P.img_cont, P.ava_x, P.ava_y);
        lv_obj_set_style_bg_opa(P.img_cont, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(P.img_cont, LV_OBJ_FLAG_SCROLLABLE);
        P.img = lv_image_create(P.img_cont);
    }
    /* Portrait contrast belongs in the clothing palette. Keep transparent
     * pixels open to the actual homepage, with no photo plate or halo. */
    lv_image_set_src(P.img, dsc);
    int w = dsc->header.w, h = dsc->header.h;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    // contain:整幅完整可见、一个像素不裁,装不满的方向留背景色边。**向下取整**避免大出 1px 被裁。
    int sx = AVA_W * 256 / w;
    int sy = AVA_H * 256 / h;
    // JPEG 传输尺寸为硬件可解码的 96×160；保持 1:1 后由容器居中裁掉上下各 2px。
    int scale = (w == 96 && h == 160) ? 256 : (sx < sy ? sx : sy);
    if (scale < 1) scale = 1;
    lv_image_set_pivot(P.img, w / 2, h / 2);
    lv_image_set_scale(P.img, scale);
    lv_obj_center(P.img);
}

bool ui_profile_open_licenses(void) {
    return s_scr_ref && lv_screen_active() == s_scr_ref && ui_licenses_open();
}
bool ui_profile_is_active(void) {
    return s_scr_ref && lv_screen_active() == s_scr_ref && !ui_licenses_is_active();
}
static void profile_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) != s_scr_ref) return;
    ui_licenses_close();
    if (P.clock) lv_timer_delete(P.clock);
    memset(&P, 0, sizeof(P)); s_scr_ref = NULL;
}

// ---- dock ----
static void dock_clicked(lv_event_t *e) {
    lv_obj_t *tile = lv_event_get_target(e);
    int id = (int)(intptr_t)lv_obj_get_user_data(tile);
    if (s_action_cb) s_action_cb(id, s_action_user);
}
static lv_obj_t *make_dock_tile(lv_obj_t *parent, int x, int w, int id, const char *name, const char *val) {
    lv_obj_t *t = lv_button_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w, 51);
    lv_obj_set_pos(t, x, 5);
    lv_obj_set_style_radius(t, 8, 0);
    lv_obj_set_style_bg_color(t, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t, COL_BORDER, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, COL_GREEN, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(t, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(t, COL_PANEL_F, LV_STATE_FOCUSED);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(t, (void *)(intptr_t)id);
    lv_obj_add_event_cb(t, dock_clicked, LV_EVENT_CLICKED, NULL);

    lv_group_t *g = lv_group_get_default();   // 加入默认导航 group(按键切换焦点)
    if (g) lv_group_add_obj(g, t);

    lv_obj_t *nl = make_label(t, name, &ui_font_body, COL_WHITE);
    lv_obj_set_style_text_letter_space(nl, 0, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 40, 0);
    lv_obj_t *vl = make_label(t, val, &lv_font_montserrat_24, COL_GREEN);   // 图标放大到 28(2 倍)
    lv_obj_align(vl, LV_ALIGN_LEFT_MID, 10, 0);
    if (id >= 0 && id < 8) P.dock_val[id] = vl;
    return t;
}

// 两个稳定入口，底部始终显示按键用途。
#define DOCK_TW 100
static void dock_relayout(void) {
    for (int i = 0; i < 2; ++i) lv_obj_set_pos(P.tile[i], 14 + i * 112, 5);
}

void ui_profile_set_on_action(ui_action_cb_t cb, void *user) {
    s_action_cb = cb; s_action_user = user;
}

const void *ui_font_cn16(void) { return &ui_font_body; }
const void *ui_font_cn24(void) { return &ui_font_title; }

// 读系统时钟(由 BLE 下发 "time" 经 settimeofday 设定,RTC 计时、跨深睡保留)刷新状态栏。
// 未同步(时间还停在 1970,epoch < 2020)时显示占位 --:-- / --- 。
// 时间以"本地秒当作 UTC"存(见协议),故用 gmtime 得到本地挂钟。
#define TIME_SYNCED_EPOCH 1600000000L   // 2020-09,判定是否已同步的阈值
static const char *WDAY[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static void refresh_time(void) {
    time_t now = time(NULL);
    if (now < TIME_SYNCED_EPOCH) {
        if (P.time_lbl) lv_label_set_text(P.time_lbl, "--:--");
        if (P.date_lbl) lv_label_set_text(P.date_lbl, "---");
        return;
    }
    struct tm tm;
    gmtime_r(&now, &tm);
    if (P.time_lbl) lv_label_set_text_fmt(P.time_lbl, "%02d:%02d", tm.tm_hour, tm.tm_min);
    if (P.date_lbl) lv_label_set_text(P.date_lbl, WDAY[tm.tm_wday % 7]);
}
static void clock_cb(lv_timer_t *t) {
    (void)t;
    refresh_time();   // 每秒读一次 RTC(便宜);同步后自动跳到真实时间
    refresh_ble_status();
}

void ui_profile_create(void) {
    lv_obj_t *scr = lv_screen_active();
    if (s_scr_ref == scr) return;
    if (P.clock) lv_timer_delete(P.clock);
    memset(&P, 0, sizeof(P));
    lv_obj_add_event_cb(scr, profile_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 状态栏 ----
    // 时间来自系统时钟/BLE 对时；电量来自采样，状态点反映真实 BLE 连接。
    P.time_lbl = make_label(scr, "--:--", &lv_font_montserrat_24, COL_WHITE);
    lv_obj_set_pos(P.time_lbl, 12, 8);
    P.date_lbl = make_label(scr, "---", &lv_font_montserrat_12, COL_GRAY);
    lv_obj_set_style_text_letter_space(P.date_lbl, 2, 0);
    lv_obj_set_pos(P.date_lbl, 92, 16);
    // 电量只用图标表达；尚未就绪时保持灰色空轮廓。
    ui_theme_battery_create(&P.battery, scr);
    P.online_dot = make_bluetooth(scr);
    P.ble_shown = false;
    lv_obj_align_to(P.online_dot, P.battery.icon, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    refresh_ble_status();

    // 中部内容块(头像+身份栏)整体高度以头像为准(AVA_H),在状态栏与 dock 之间垂直居中。
    // 居中后整体上移 16px:昵称离顶部状态栏更近,多出的空白让到底部 dock 一侧。
    const int mid_y0 = STATUS_H + ((SCREEN_H - DOCK_H - STATUS_H) - AVA_H) / 2;

    // ---- 头像位(左)----
    // 无默认像素小人:默认留空(背景色),仅在传过头像图后由 show_image 建图片容器填入。
    s_scr_ref = scr;
    P.ava_x = 12;
    P.ava_y = mid_y0;
    avatar_placeholder(true);   // 默认显示黑色半身剪影(传过头像后会被隐藏)

    // ---- 身份(右):昵称 + 可下发头衔 + 四行一句话简介 ----
    const int TEXT_X = 118, TEXT_W = SCREEN_W - 118 - 6;
    P.name_lbl = make_label(scr, "FoloToy", &ui_font_title, COL_WHITE);
    lv_obj_set_size(P.name_lbl, TEXT_W, 28);
    lv_label_set_long_mode(P.name_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);   // 过长循环滚动,不换行撑高
    lv_obj_set_pos(P.name_lbl, TEXT_X, mid_y0 + 4);
    P.title_lbl = make_label(scr, "NETRUNNER", &ui_font_body, COL_GREEN2);
    lv_obj_set_size(P.title_lbl, TEXT_W, 20);
    lv_label_set_long_mode(P.title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(P.title_lbl, TEXT_X, mid_y0 + 40);
    P.intro_lbl = make_label(scr, "探索AI的\n无限可能", &ui_font_body, COL_WHITE);
    lv_obj_set_size(P.intro_lbl, TEXT_W, lv_font_get_line_height(&ui_font_body) * 4);
    lv_label_set_long_mode(P.intro_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(P.intro_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(P.intro_lbl, TEXT_X, mid_y0 + 64);

    // ---- 底部横向 dock(2 格居中:GAME / ALBUM)----
    lv_obj_t *dock = lv_obj_create(scr);
    lv_obj_remove_style_all(dock);
    lv_obj_set_size(dock, SCREEN_W, DOCK_H);
    ui_round_align_bottom(dock, 0);  // Safe-area aware bottom dock
    lv_obj_set_style_bg_color(dock, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(dock, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dock, COL_BORDER, 0);
    lv_obj_set_style_border_width(dock, 1, 0);
    lv_obj_remove_flag(dock, LV_OBJ_FLAG_SCROLLABLE);
    P.dock = dock;
    P.tile[0] = make_dock_tile(dock, 0, DOCK_TW, 2, "玩法", LV_SYMBOL_PLAY);
    P.tile[1] = make_dock_tile(dock, 0, DOCK_TW, 3, "相册", LV_SYMBOL_IMAGE);
    dock_relayout();

    P.clock = lv_timer_create(clock_cb, 1000, NULL);
    refresh_time();
}

// ================= 运行时 setter(请在 LVGL 锁内调用)=================

// profile_data_t.name 最多 47 字节。显示前按 UTF-8 码点查询昵称实际使用的字体：
// 缺失汉字替换为 '*'，缺失非汉字丢弃；仅改变屏幕文本，不改 NVS 原始昵称。
#define PROFILE_NAME_MAX_BYTES 47
static bool nickname_utf8_next(const char *text, uint32_t *offset, uint32_t *codepoint) {
    const unsigned char *s = (const unsigned char *)text + *offset;
    uint32_t cp;
    uint32_t min;
    uint32_t count;

    if (s[0] < 0x80) {
        *codepoint = s[0];
        (*offset)++;
        return true;
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        cp = s[0] & 0x1F; min = 0x80; count = 2;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        cp = s[0] & 0x0F; min = 0x800; count = 3;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        cp = s[0] & 0x07; min = 0x10000; count = 4;
    } else {
        (*offset)++;
        return false;
    }

    for (uint32_t i = 1; i < count; i++) {
        if (s[i] == '\0' || (s[i] & 0xC0) != 0x80) {
            (*offset)++;
            return false;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        (*offset)++;
        return false;
    }
    *offset += count;
    *codepoint = cp;
    return true;
}

static bool nickname_is_han(uint32_t codepoint) {
    return (codepoint >= 0x3400 && codepoint <= 0x4DBF)    // CJK 扩展 A
        || (codepoint >= 0x4E00 && codepoint <= 0x9FFF)    // CJK 基本区
        || (codepoint >= 0xF900 && codepoint <= 0xFAFF)    // CJK 兼容汉字
        || (codepoint >= 0x20000 && codepoint <= 0x323AF); // CJK 扩展 B-H
}

static void nickname_display_text(const char *name, char out[PROFILE_NAME_MAX_BYTES + 1]) {
    uint32_t read = 0;
    size_t write = 0;
    while (name[read] != '\0' && write < PROFILE_NAME_MAX_BYTES) {
        uint32_t start = read;
        uint32_t letter = 0;
        bool valid = nickname_utf8_next(name, &read, &letter);

        lv_font_glyph_dsc_t glyph;
        if (!valid) continue;
        if (!lv_font_get_glyph_dsc(&ui_font_title, &glyph, letter, 0)) {
            // 缺失汉字保留一个明确占位；Emoji、外文和其它符号直接隐藏。
            if (nickname_is_han(letter)) out[write++] = '*';
            continue;
        }

        size_t encoded_len = read - start;
        if (encoded_len > PROFILE_NAME_MAX_BYTES - write) break;
        memcpy(out + write, name + start, encoded_len);
        write += encoded_len;
    }
    out[write] = '\0';
}

void ui_profile_set_name(const char *name) {
    if (!P.name_lbl || !name) return;
    char display[PROFILE_NAME_MAX_BYTES + 1];
    nickname_display_text(name, display);
    lv_label_set_text(P.name_lbl, display);
}
// 图标填充直接使用 CW2017 实际 SOC；未知值不冒充有效电量。
void ui_profile_set_battery(int percent, int level) {
    ui_theme_battery_set(&P.battery, percent, level);
}
// Keep the legacy configuration API; Bobo is now always under Plays.

void ui_profile_set_title(const char *title) {
    if (P.title_lbl && title) lv_label_set_text(P.title_lbl, title);
}
void ui_profile_set_intro(const char *intro) {
    if (P.intro_lbl && intro)
        lv_label_set_text(P.intro_lbl, *intro ? intro : "探索AI的\n无限可能");
}
