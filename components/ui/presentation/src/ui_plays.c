#include "presentation/ui_plays.h"
#include "presentation/ui_theme.h"
#include "services/answer_content.h"
#include <stdio.h>
#include <string.h>

typedef enum { MODE_MENU, MODE_ANSWER, MODE_CLICKER, MODE_CLICKER_PICK, MODE_CLICKER_ACTIONS, MODE_CLICKER_CHANGE, MODE_SUSPENDED } ui_plays_mode_t;
static struct {
    lv_obj_t *screen, *previous, *message, *detail, *meter, *status, *count, *feedback;
    lv_obj_t *ring_outer, *ring_inner, *fold;
    ui_plays_mode_t mode;
    unsigned selection;
    unsigned click_selection;
    unsigned click_change_selection;
    bool click_change_help;
    ui_click_view_t click_view;
    char click_name[32];
    ab_state_t answer_state;
    size_t answer_index;
    ui_play_select_cb_t select;
    ui_plays_exit_cb_t exit;
    void *user;
} P;

static const char *const titles[] = {"小智 AI 对话", "答案之书", "蓝牙翻页笔", "障碍躲避游戏", "连接小程序", "返回首页"};
// Display order is independent of the semantic IDs used by the app worker.
static const ui_play_id_t menu_plays[] = {UI_PLAY_XIAOZHI, UI_PLAY_ANSWER,
    UI_PLAY_CLICKER, UI_PLAY_DODGE, UI_PLAY_CONNECT};


static void clear(ui_plays_mode_t mode) {
    lv_obj_clean(P.screen);
    P.message = P.detail = P.meter = P.status = P.count = P.feedback = NULL;
    P.ring_outer = P.ring_inner = P.fold = NULL;
    lv_obj_set_style_bg_color(P.screen, lv_color_hex(UI_BG), 0);
    P.mode = mode;
}
void ui_plays_menu(void) {
    if (!P.screen) return;
    clear(MODE_MENU);
    ui_theme_text(P.screen, "玩法", 14, 17, 120, &ui_font_title, UI_INK);
    char position[16]; snprintf(position, sizeof position, "%u / 6", P.selection + 1);
    lv_obj_t *count = ui_theme_text(P.screen, position, 164, 17, 62, &ui_font_title, UI_INK);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_RIGHT, 0);
    unsigned first = P.selection < 4 ? 0 : P.selection - 3;
    for (unsigned row_index = 0; row_index < 4; ++row_index) {
        unsigned i = first + row_index;
        bool selected = i == P.selection;
        lv_obj_t *row = ui_theme_panel(P.screen, 14, 58 + row_index * 56, 212, 48, selected);
        ui_theme_text(row, titles[i], 12, 12, 170, &ui_font_title, selected ? UI_ACCENT : UI_INK);
        if (selected) ui_theme_text(row, ">", 184, 13, 15, &lv_font_montserrat_14, UI_ACCENT);
    }
    lv_obj_t *hint = ui_theme_text(P.screen, "上下选择  确定进入", 12, 289, 216, &ui_font_title, UI_INK);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_screen_load(ui_theme_screen_obj(P.screen));
}
void ui_plays_open(ui_play_select_cb_t select, ui_plays_exit_cb_t exit, void *user) {
    if (P.screen) return;
    P.previous = lv_screen_active(); P.select = select; P.exit = exit; P.user = user;
    P.selection = 0;
    P.screen = ui_theme_screen();
    ui_plays_menu();
}
void ui_plays_close(void) {
    if (!P.screen) return;
    lv_screen_load(P.previous);
    lv_obj_t *gone = ui_theme_screen_obj(P.screen);
    lv_obj_delete(gone ? gone : P.screen); P.screen = NULL;
    if (P.exit) P.exit(P.user);
}
bool ui_plays_is_active(void) { return P.screen != NULL; }
void ui_plays_menu_key(ui_play_key_t key) {
    if (!P.screen || P.mode != MODE_MENU) return;
    if (key == UI_PLAY_BACK) { ui_plays_close(); return; }
    if (key == UI_PLAY_UP) P.selection = (P.selection + 5) % 6;
    else if (key == UI_PLAY_DOWN) P.selection = (P.selection + 1) % 6;
    else if (key == UI_PLAY_OK) {
        if (P.selection == 5) { ui_plays_close(); return; }
        if (P.select && P.select(menu_plays[P.selection], P.user)) {
            // Prevent double-launch while the app worker switches audio/Bluetooth ownership.
            ui_plays_suspend();
        }
        return;
    }
    ui_plays_menu();
}
void ui_plays_suspend(void) {
    if (P.screen) clear(MODE_SUSPENDED);
}
// The book keeps its own indigo-and-gold atmosphere. All ornaments are small
// native LVGL shapes: no full-screen image, opacity layer or scaled canvas.
#define BOOK_NIGHT  0x100E31
#define BOOK_INDIGO 0x29205D
#define BOOK_VIOLET 0x514284
#define BOOK_GOLD   0xEAC77D
#define BOOK_PAPER  0xFFF1D2
#define BOOK_BROWN  0x695137

static lv_obj_t *book_shape(lv_obj_t *parent, int x, int y, int w, int h,
                            uint32_t fill, uint32_t edge, int border, int radius) {
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_remove_style_all(shape);
    lv_obj_set_pos(shape, x, y); lv_obj_set_size(shape, w, h);
    lv_obj_set_style_bg_color(shape, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(shape, lv_color_hex(edge), 0);
    lv_obj_set_style_border_width(shape, border, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_remove_flag(shape, LV_OBJ_FLAG_SCROLLABLE);
    return shape;
}
static lv_obj_t *book_text(lv_obj_t *parent, const char *text, int x, int y,
                           int width, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = ui_theme_text(parent, text, x, y, width, font, color);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}
static void book_star(lv_obj_t *parent, int x, int y, int r, uint32_t color) {
    book_shape(parent, x - r, y, 2 * r + 1, 1, color, color, 0, 0);
    book_shape(parent, x, y - r, 1, 2 * r + 1, color, color, 0, 0);
}
static void book_mic(lv_obj_t *parent, int x, int y) {
    book_shape(parent, x + 10, y, 16, 29, BOOK_GOLD, BOOK_PAPER, 1, 8);
    lv_obj_t *cup = book_shape(parent, x + 3, y + 14, 30, 24, BOOK_INDIGO, BOOK_GOLD, 2, 13);
    lv_obj_set_style_bg_opa(cup, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(cup, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
    book_shape(parent, x + 17, y + 37, 2, 9, BOOK_GOLD, BOOK_GOLD, 0, 0);
    book_shape(parent, x + 9, y + 45, 18, 2, BOOK_GOLD, BOOK_GOLD, 0, 1);
}
static void answer_layout(const ab_session_view_t *view) {
    clear(MODE_ANSWER);
    lv_obj_set_style_bg_color(P.screen, lv_color_hex(BOOK_NIGHT), 0);
    book_text(P.screen, "THE BOOK OF ANSWERS", 16, 17, 208, &lv_font_montserrat_10, BOOK_GOLD);
    // A sparse constellation keeps the small screen calm and legible.
    static const uint8_t stars[][3] = {{20,55,3},{219,77,2},{26,148,2},{213,170,3},{21,229,2},{216,237,2}};
    for (unsigned i = 0; i < sizeof(stars) / sizeof(stars[0]); ++i)
        book_star(P.screen, stars[i][0], stars[i][1], stars[i][2], i % 2 ? BOOK_VIOLET : BOOK_GOLD);
    const char *hint = "按住上键提问，说完松开";
    if (view->state == AB_STATE_IDLE) {
        book_shape(P.screen, 49, 56, 149, 183, 0x090923, 0x090923, 0, 7);
        book_shape(P.screen, 46, 48, 146, 182, 0xC8AA76, BOOK_GOLD, 1, 5);
        book_shape(P.screen, 44, 46, 144, 178, BOOK_INDIGO, BOOK_GOLD, 1, 5);
        book_shape(P.screen, 51, 47, 1, 175, BOOK_VIOLET, BOOK_VIOLET, 0, 0);
        lv_obj_t *frame = book_shape(P.screen, 58, 59, 116, 149, BOOK_INDIGO, 0x9E814E, 1, 2);
        book_star(frame, 58, 17, 5, BOOK_GOLD);
        P.message = book_text(frame, "答案\n之书", 17, 35, 82, &ui_font_book, BOOK_PAPER);
        lv_obj_set_style_text_line_space(P.message, 5, 0);
        book_text(frame, "ASK THE STARS", 5, 121, 106, &lv_font_montserrat_10, BOOK_GOLD);
        book_star(P.screen, 43, 43, 4, BOOK_GOLD);
        book_star(P.screen, 187, 225, 4, BOOK_GOLD);
    } else if (view->state == AB_STATE_LISTENING) {
        P.ring_outer = book_shape(P.screen, 52, 64, 136, 136, BOOK_NIGHT, BOOK_VIOLET, 1, LV_RADIUS_CIRCLE);
        P.ring_inner = book_shape(P.screen, 69, 81, 102, 102, BOOK_INDIGO, BOOK_GOLD, 1, LV_RADIUS_CIRCLE);
        book_mic(P.screen, 102, 108);
        book_text(P.screen, "我在听，慢慢说", 20, 218, 200, &ui_font_title, BOOK_PAPER);
        hint = "至少两秒 · 说完松开上键";
    } else if (view->state == AB_STATE_PAGE_TURN || view->state == AB_STATE_ANSWER) {
        // Native paper, a fine spine and an inset gilt border reveal real text.
        book_shape(P.screen, 29, 63, 190, 177, 0x080721, 0x080721, 0, 7);
        book_shape(P.screen, 24, 57, 192, 177, 0xCCAF7E, BOOK_GOLD, 1, 6);
        lv_obj_t *page = book_shape(P.screen, 22, 52, 192, 177, BOOK_PAPER, BOOK_GOLD, 1, 6);
        lv_obj_t *frame = book_shape(page, 10, 10, 172, 156, BOOK_PAPER, 0xD8BD8B, 1, 2);
        book_text(frame, "A LITTLE SIGN FOR YOU", 8, 15, 156, &lv_font_montserrat_10, BOOK_BROWN);
        book_star(frame, 86, 44, 4, 0xAD874D);
        if (view->state == AB_STATE_ANSWER) {
            const char *message = view->answer_ready && view->answer_index < answer_text_count ?
                answer_texts[view->answer_index] : "再试一次";
            if (view->answer_ready) switch (view->answer_index) {
                case 4: message = "现在\n还不是时候"; break;
                case 7: message = "答案就在\n你心里"; break;
                case 13: message = "事情会\n变得明朗"; break;
                case 14: message = "听从\n内心的声音"; break;
                case 18: message = "你已经\n知道答案"; break;
                case 21: message = "很快就会\n有结果"; break;
                case 23: message = "相信正在\n发生的一切"; break;
                default: break;
            }
            P.message = book_text(frame, message, 8, 60, 156, &ui_font_answer, BOOK_BROWN);
            lv_obj_set_style_text_line_space(P.message, 7, 0);
            lv_obj_update_layout(P.message);
            lv_obj_set_y(P.message, 86 - lv_obj_get_height(P.message) / 2);
            book_star(frame, 86, 130, 3, 0xAD874D);
            hint = "把这一页，留给心里的你";
        } else {
            P.fold = book_shape(page, 96, 1, 95, 175, BOOK_INDIGO, BOOK_GOLD, 1, 4);
            hint = "星光正在翻开这一页…";
        }
    } else {
        book_shape(P.screen, 76, 70, 88, 88, BOOK_INDIGO, BOOK_GOLD, 1, LV_RADIUS_CIRCLE);
        book_mic(P.screen, 102, 89);
        bool too_short = view->state == AB_STATE_TOO_SHORT;
        book_text(P.screen, too_short ? "再说完整一点" : "暂时没听见声音", 18, 186, 204, &ui_font_title, BOOK_PAPER);
        book_text(P.screen, too_short ? "按住上键至少两秒" : "稍等一下，再试一次", 18, 219, 204, &ui_font_body, BOOK_GOLD);
    }
    P.detail = book_text(P.screen, hint, 8, 250, 224, &ui_font_caption, 0xC7BCD8);
    lv_obj_t *ask = book_shape(P.screen, 14, 281, 103, 29, BOOK_INDIGO, BOOK_VIOLET, 1, 8);
    book_text(ask, "按住上键", 3, 5, 97, &ui_font_body, BOOK_PAPER);
    lv_obj_t *back = book_shape(P.screen, 123, 281, 103, 29, BOOK_NIGHT, 0x9E814E, 1, 8);
    book_text(back, "确定·返回", 3, 5, 97, &ui_font_body, BOOK_GOLD);
    P.answer_state = view->state; P.answer_index = view->answer_index;
}
void ui_plays_answer(const ab_session_view_t *view, uint16_t level, uint64_t now_ms) {
    if (!P.screen || !view) return;
    if (P.mode != MODE_ANSWER || view->state != P.answer_state || view->answer_index != P.answer_index)
        answer_layout(view);
    if (view->state == AB_STATE_LISTENING && P.ring_outer && P.ring_inner) {
        unsigned strength = level > 1000 ? 1000 : level;
        int pulse = (int)((now_ms / 40) % 20);
        if (pulse > 10) pulse = 20 - pulse;
        int diameter = 126 + (int)(strength * 18 / 1000) + pulse;
        lv_obj_set_size(P.ring_outer, diameter, diameter);
        lv_obj_set_pos(P.ring_outer, 120 - diameter / 2, 132 - diameter / 2);
        lv_obj_set_style_border_color(P.ring_outer, lv_color_hex(strength > 300 ? BOOK_GOLD : BOOK_VIOLET), 0);
    } else if (view->state == AB_STATE_PAGE_TURN && P.fold) {
        uint64_t remaining = view->deadline_ms > now_ms ? view->deadline_ms - now_ms : 0;
        if (remaining > AB_PAGE_TURN_MS) remaining = AB_PAGE_TURN_MS;
        int width = (int)(remaining * 190 / AB_PAGE_TURN_MS);
        lv_obj_set_width(P.fold, width > 0 ? width : 1);
        lv_obj_set_x(P.fold, 191 - width);
    }
}
static void clicker_pick(void) {
    clear(MODE_CLICKER_PICK);
    ui_theme_header(P.screen, "选择用途", "BLUETOOTH / REMOTE");
    static const char *const titles[] = {"安卓刷视频", "iPhone 刷视频", "电脑翻演示", "返回玩法"};
    static const char *const details[] = {"屏幕中间 · 触控滑动", "辅助触控 · 拖动翻页", "演示文稿 · 上下翻页", "回到玩法列表"};
    for (unsigned i = 0; i < sizeof titles / sizeof titles[0]; ++i) {
        bool selected = P.click_selection == i;
        lv_obj_t *row = ui_theme_panel(P.screen, 14, 62 + i * 51, 212, 46, selected);
        ui_theme_text(row, titles[i], 12, 3, 188, &ui_font_title, selected ? UI_ACCENT : UI_INK);
        ui_theme_text(row, details[i], 12, 26, 188, &ui_font_caption, UI_MUTED);
    }
    ui_theme_text(P.screen, "选好用途后，再连接蓝牙", 16, 274, 208, &ui_font_caption, UI_MUTED);
    ui_theme_footer(P.screen, "上下选择   确定进入");
}
static void clicker_layout(void) {
    clear(MODE_CLICKER);
    ui_theme_header(P.screen, "蓝牙翻页笔", P.click_view.phone_mode ? "PHONE / BLUETOOTH" : "PRESENT / BLUETOOTH");
    lv_obj_t *panel = ui_theme_panel(P.screen, 14, 65, 212, 73, false);
    P.status = ui_theme_text(panel, "正在开启蓝牙", 10, 9, 192, &ui_font_title, UI_GOLD);
    P.detail = ui_theme_text(panel, "", 10, 42, 192, &lv_font_montserrat_14, UI_INK);
    P.message = ui_theme_text(P.screen, "", 14, 148, 212, &ui_font_body, UI_INK);
    P.meter = ui_theme_text(P.screen, "", 14, 174, 212, &ui_font_caption, UI_MUTED);
    P.count = ui_theme_text(P.screen, "", 14, 239, 212, &ui_font_caption, UI_MUTED);
    P.feedback = ui_theme_text(P.screen, "", 14, 258, 212, &ui_font_caption, UI_MUTED);
    lv_obj_t *actions = ui_theme_panel(P.screen, 14, 281, 212, 29, true);
    ui_theme_text(actions, "确定 · 操作菜单 / 返回", 10, 4, 195, &ui_font_body, UI_ACCENT);
}
void ui_plays_clicker(const ui_click_view_t *view) {
    if (!P.screen || !view) return;
    ui_click_status_t previous_status = P.click_view.status;
    if (view != &P.click_view) {
        snprintf(P.click_name, sizeof(P.click_name), "%s", view->name ? view->name : "AI PASSPORT");
        P.click_view = *view; P.click_view.name = P.click_name;
    }
    if (P.mode == MODE_CLICKER_ACTIONS || P.mode == MODE_CLICKER_CHANGE || P.mode == MODE_CLICKER_PICK) return;
    if (P.mode != MODE_CLICKER) { P.click_selection = 0; clicker_pick(); return; }
    if (view->status == UI_CLICK_READY && previous_status != UI_CLICK_READY)
        P.click_change_help = false;
    bool phone = view->phone_mode;
    const char *status[] = {"正在开启蓝牙", phone ? "请连接手机蓝牙：" : "请连接电脑蓝牙：",
        phone ? "正在连接手机" : "正在连接电脑",
        phone ? (view->touch_mode ? "已连接，可以滑动" : "已连接，拖动模式") : "已连接，可以翻页", "蓝牙暂时不可用", "正在断开旧设备"};
    unsigned value = (unsigned)view->status;
    lv_obj_set_style_text_font(P.status, view->status == UI_CLICK_PAIRING ? &ui_font_body : &ui_font_title, 0);
    lv_label_set_text(P.status, status[value < 6 ? value : 4]);
    bool ready = view->status == UI_CLICK_READY;
    bool available = view->status != UI_CLICK_UNAVAILABLE && value < 6;
    lv_obj_set_style_text_color(P.status, lv_color_hex(ready ? UI_ACCENT : UI_GOLD), 0);
    bool switching = view->status == UI_CLICK_SWITCHING;
    lv_label_set_text(P.message, ready ? (phone ? (view->touch_mode ? "安卓模式 · 触控滑动" : "请先开启辅助触控") : "电脑模式 · 按键翻页") : switching ? "换一台设备" : available ? (phone ? (view->touch_mode ? "安卓模式 · 连接步骤" : "1. 请先开启辅助触控") : "电脑模式 · 连接步骤") : "蓝牙尚未就绪");
    lv_obj_set_style_text_color(P.message, lv_color_hex(phone && !view->touch_mode && !switching && available ? UI_GOLD : UI_INK), 0);
    lv_obj_set_style_text_font(P.meter, ready ? &ui_font_body : &ui_font_caption, 0);
    lv_label_set_text(P.meter, ready ? (phone ? "上键：上一条  下键：下一条\n手机竖屏，打开\n抖音/小红书/视频号。" : "上键：上一页\n下键：下一页") : switching ?
        "正在释放按键并断开连接\n完成后会重新显示配对步骤\n旧设备请先断开或关闭蓝牙" : available ?
        (phone ? (view->touch_mode ? "安卓：在系统蓝牙中配对\n选择上方名称，完成连接\n手机竖屏，打开\n抖音/小红书/视频号。" : "设置 → 辅助功能 → 触控\n→ 辅助触控，打开开关\n2. 打开手机系统蓝牙\n选择上方名称，完成配对") : "Mac：系统设置 / 蓝牙\nWin：设置 / 蓝牙和设备\nWin 先点添加设备 / 蓝牙\n选上方名称，点配对或连接") :
        "返回玩法后，再进入试试\n仍不可用时，请重启设备");
    char counts[64] = "";
    if (ready) snprintf(counts, sizeof(counts), "已发送 %lu 次", (unsigned long)view->sent);
    lv_label_set_text(P.count, counts);
    lv_label_set_text(P.detail, view->name ? view->name : "AI PASSPORT");
    if (view->status != previous_status || !*lv_label_get_text(P.feedback))
        lv_label_set_text(P.feedback, ready ? (phone ? (view->touch_mode ? "画面中间滑动，避开屏幕边缘" : "圆点在画面中间时，按上下键") : "先打开电脑上的演示文稿") :
                          available ? (P.click_change_help ? "旧设备请先断开或关闭蓝牙" : phone ? (view->touch_mode ? "连接后，手机竖屏打开视频" : "同时开启执行触控手势") : "确定打开菜单，可切手机模式") : "确定打开菜单，可返回玩法");
}
static void clicker_actions(void) {
    clear(MODE_CLICKER_ACTIONS);
    ui_theme_header(P.screen, "翻页笔操作", P.click_view.phone_mode ? "PHONE / ACTIONS" : "PRESENT / ACTIONS");
    static const char *const computer_options[] = {
        "继续翻页", "选择其他用途", "开始演示", "结束演示", "暂时隐藏画面", "换一台设备", "返回玩法"
    };
    static const char *const computer_details[] = {
        "上下键翻页，确定打开菜单", "安卓 / iPhone / 电脑",
        "从第一页开始 · Win PPT / F5", "回到编辑界面 · Esc",
        "再按一次恢复 · 放映中 / B", "断开当前设备，重新配对", "退出翻页笔，回到玩法列表"
    };
    static const char *const phone_options[] = {"继续滑动", "选择其他用途", "换一台设备", "返回玩法"};
    static const char *const phone_details[] = {"上键上一条，下键下一条", "安卓 / iPhone / 电脑",
        "断开当前设备，重新配对", "退出翻页笔，回到玩法列表"};
    static const char *const iphone_options[] = {"继续滑动", "选择其他用途", "测试指针", "反转滚动方向", "换一台设备", "返回玩法"};
    const char *iphone_details[] = {"上键上一条，下键下一条", "安卓 / iPhone / 电脑",
        "只移动圆点，不点击",
        P.click_view.scroll_reversed ? "当前反向 · 点击切回正向" : "当前正向 · 上下颠倒时切换",
        "断开当前设备，重新配对", "退出翻页笔，回到玩法列表"};
    bool phone = P.click_view.phone_mode;
    bool iphone = phone && !P.click_view.touch_mode;
    unsigned count = phone ? (iphone ? 6 : 4) : 7;
    const char *const *options = iphone ? iphone_options : phone ? phone_options : computer_options;
    const char *const *details = iphone ? iphone_details : phone ? phone_details : computer_details;
    unsigned page = P.click_selection / 3;
    char page_text[32];
    snprintf(page_text, sizeof(page_text), "操作菜单  %u / %u", page + 1, (count + 2) / 3);
    ui_theme_text(P.screen, page_text, 16, 65, 210, &ui_font_caption, UI_MUTED);
    for (unsigned row_index = 0; row_index < 3; ++row_index) {
        unsigned i = page * 3 + row_index;
        if (i >= count) break;
        bool selected = i == P.click_selection;
        lv_obj_t *row = ui_theme_panel(P.screen, 14, 88 + row_index * 54, 212, 49, selected);
        ui_theme_text(row, options[i], 12, 5, 190, &ui_font_body, selected ? UI_ACCENT : UI_INK);
        ui_theme_text(row, details[i], 12, 28, 190, &ui_font_caption, UI_MUTED);
    }
    ui_theme_text(P.screen, phone ? (P.click_view.touch_mode ? "安卓使用屏幕中间触控\n手机竖屏，一次按键滑动一条" : "辅助触控内开启执行触控手势\n圆点保持在画面中间") :
                  "演示功能以 PowerPoint 为准\n其他软件的快捷键可能不同", 16, 258, 210,
                  &ui_font_caption, UI_MUTED);
    ui_theme_footer(P.screen, "上下选择   确定执行");
}
static void clicker_change_help(void) {
    clear(MODE_CLICKER_CHANGE);
    ui_theme_header(P.screen, "换一台设备", "BLUETOOTH / PAIR");
    ui_theme_text(P.screen, "将断开现在的设备\n清除本机对它的配对记录", 16, 78, 208, &ui_font_body, UI_INK);
    char instructions[224];
    snprintf(instructions, sizeof instructions,
        "旧设备若自动重连：\n请先断开或忽略此设备\n也可以关闭旧设备蓝牙\n随后在新设备蓝牙中选择\n%s", P.click_name);
    ui_theme_text(P.screen, instructions, 16, 129, 208, &ui_font_caption, UI_MUTED);
    ui_theme_text(P.screen, "上下选择   确定执行", 16, 219, 208, &ui_font_caption, UI_MUTED);
    static const char *const options[] = {"断开并重新配对", "取消，返回菜单"};
    for (unsigned i = 0; i < 2; ++i) {
        bool selected = i == P.click_change_selection;
        lv_obj_t *action = ui_theme_panel(P.screen, 14, 241 + i * 34, 212, 30, selected);
        ui_theme_text(action, options[i], 10, 5, 195, &ui_font_body, selected ? UI_ACCENT : UI_INK);
    }
}
ui_click_action_t ui_plays_clicker_key(ui_play_key_t key) {
    if (!P.screen || (P.mode != MODE_CLICKER && P.mode != MODE_CLICKER_ACTIONS &&
                      P.mode != MODE_CLICKER_CHANGE && P.mode != MODE_CLICKER_PICK)) return UI_CLICK_ACTION_NONE;
    if (P.mode == MODE_CLICKER_PICK) {
        if (key == UI_PLAY_BACK) return UI_CLICK_ACTION_EXIT;
        if (key == UI_PLAY_UP) P.click_selection = (P.click_selection + 3) % 4;
        else if (key == UI_PLAY_DOWN) P.click_selection = (P.click_selection + 1) % 4;
        else if (key == UI_PLAY_OK) {
            if (P.click_selection == 3) return UI_CLICK_ACTION_EXIT;
            P.click_view.phone_mode = P.click_selection < 2;
            P.click_view.touch_mode = P.click_selection == 0;
            clicker_layout(); ui_plays_clicker(&P.click_view);
            return P.click_view.touch_mode ? UI_CLICK_ACTION_TOUCH_MODE : P.click_view.phone_mode ? UI_CLICK_ACTION_PHONE_MODE : UI_CLICK_ACTION_COMPUTER_MODE;
        }
        clicker_pick(); return UI_CLICK_ACTION_NONE;
    }
    if (P.mode == MODE_CLICKER_CHANGE) {
        if (key == UI_PLAY_UP || key == UI_PLAY_DOWN) {
            P.click_change_selection = 1 - P.click_change_selection;
            clicker_change_help();
            return UI_CLICK_ACTION_NONE;
        }
        if (key == UI_PLAY_BACK) { clicker_actions(); return UI_CLICK_ACTION_NONE; }
        if (key == UI_PLAY_OK) {
            if (P.click_change_selection == 1) {
                clicker_actions();
                return UI_CLICK_ACTION_NONE;
            }
            P.click_change_help = true;
            clicker_layout(); ui_plays_clicker(&P.click_view);
            return UI_CLICK_ACTION_CHANGE_COMPUTER;
        }
        return UI_CLICK_ACTION_NONE;
    }
    if (P.mode == MODE_CLICKER) {
        if (key == UI_PLAY_UP) return UI_CLICK_ACTION_PREV;
        if (key == UI_PLAY_DOWN) return UI_CLICK_ACTION_NEXT;
        if (key == UI_PLAY_BACK) return UI_CLICK_ACTION_EXIT;
        if (key == UI_PLAY_OK) { P.click_selection = 0; clicker_actions(); }
        return UI_CLICK_ACTION_NONE;
    }
    bool phone = P.click_view.phone_mode;
    unsigned count = phone ? (P.click_view.touch_mode ? 4 : 6) : 7;
    if (key == UI_PLAY_UP) P.click_selection = (P.click_selection + count - 1) % count;
    else if (key == UI_PLAY_DOWN) P.click_selection = (P.click_selection + 1) % count;
    else if (key == UI_PLAY_OK || key == UI_PLAY_BACK) {
        unsigned selection = key == UI_PLAY_BACK ? 0 : P.click_selection;
        if (selection == count - 1) return UI_CLICK_ACTION_EXIT;
        if (selection == count - 2) {
            P.click_change_selection = 1;
            clicker_change_help();
            return UI_CLICK_ACTION_NONE;
        }
        if (selection == 1) { P.click_selection = 0; clicker_pick(); return UI_CLICK_ACTION_NONE; }
        if (phone && !P.click_view.touch_mode && selection == 2) {
            clicker_layout(); ui_plays_clicker(&P.click_view);
            return UI_CLICK_ACTION_POINTER_TEST;
        }
        if (phone && !P.click_view.touch_mode && selection == 3) {
            P.click_view.scroll_reversed = !P.click_view.scroll_reversed;
            clicker_layout(); ui_plays_clicker(&P.click_view);
            return UI_CLICK_ACTION_REVERSE_SCROLL;
        }
        clicker_layout(); ui_plays_clicker(&P.click_view);
        return selection == 2 ? UI_CLICK_ACTION_START : selection == 3 ? UI_CLICK_ACTION_END :
               selection == 4 ? UI_CLICK_ACTION_BLANK : UI_CLICK_ACTION_NONE;
    }
    clicker_actions();
    return UI_CLICK_ACTION_NONE;
}
void ui_plays_clicker_feedback(const char *action, bool queued) {
    if (!P.screen || P.mode != MODE_CLICKER) return;
    lv_label_set_text(P.feedback, queued ? action : P.click_view.status == UI_CLICK_READY ?
                      "未发送，稍后再试" : P.click_view.phone_mode ? "请先完成手机配对" : "请先完成电脑配对");
}
