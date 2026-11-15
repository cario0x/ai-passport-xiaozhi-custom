#include "presentation/ui_licenses.h"
#include "presentation/font_notices.h"
#include "presentation/ui_theme.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_BYTES 1024
#define LINE_BYTES 256
#define BODY_WIDTH 216
#define BODY_HEIGHT 210
#define LINE_SPACE 2

static struct {
    lv_obj_t *screen, *previous, *focus, *body, *counter, *buttons[3];
    lv_group_t *group, *previous_group;
    size_t length, offset, end;
    unsigned page, pages;
    bool opening, closing;
    char text[PAGE_BYTES], line[LINE_BYTES];
} L;
static void open_deferred(void *unused);
static void close_deferred(void *unused);

static bool group_valid(lv_group_t *group) {
    for (uint32_t i = 0; i < lv_group_get_count(); ++i)
        if (lv_group_by_index(i) == group) return true;
    return false;
}
static void move_inputs(lv_group_t *from, lv_group_t *to) {
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) != from) continue;
        bool held = lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED;
        lv_indev_reset(input, NULL);
        lv_indev_set_group(input, to);
        if (held) lv_indev_wait_release(input);
    }
}
static size_t utf8_next(size_t offset, uint32_t *codepoint) {
    const unsigned char *s = (const unsigned char *)passport_font_notices + offset;
    unsigned bytes = s[0] < 0x80 ? 1 : s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : 4;
    uint32_t cp = bytes == 1 ? s[0] : s[0] & ((1u << (7 - bytes)) - 1);
    if (bytes > L.length - offset) bytes = 1;
    for (unsigned i = 1; i < bytes; ++i) {
        if ((s[i] & 0xC0) != 0x80) { bytes = 1; cp = s[0]; break; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *codepoint = cp;
    return offset + bytes;
}
static size_t previous_codepoint(size_t start, size_t end) {
    if (end <= start) return start;
    --end;
    while (end > start && ((unsigned char)passport_font_notices[end] & 0xC0) == 0x80) --end;
    return end;
}
/* Measures real glyph advances in linear time, then validates the chosen line
 * with LVGL's own measurement. Soft breaks prefer spaces; long URLs hard-wrap.
 * All source bytes, including whitespace, belong to exactly one page range. */
static size_t line_end(size_t start, size_t *visible_bytes) {
    size_t cursor = start, last_space = start;
    int32_t width = 0;
    while (cursor < L.length) {
        uint32_t cp, next_cp = 0;
        size_t next = utf8_next(cursor, &cp);
        if (cp == '\n' || cp == '\r') {
            cursor = next;
            if (cp == '\r' && cursor < L.length && passport_font_notices[cursor] == '\n') ++cursor;
            break;
        }
        if (next - start >= LINE_BYTES) break;
        if (next < L.length) (void)utf8_next(next, &next_cp);
        width += lv_font_get_glyph_width(&ui_font_caption, cp, next_cp);
        if (width > BODY_WIDTH && cursor > start) {
            if (last_space > start) cursor = last_space;
            break;
        }
        cursor = next;
        if (cp == ' ' || cp == '\t') last_space = cursor;
    }
    size_t visible_end = cursor;
    while (visible_end > start && (passport_font_notices[visible_end - 1] == '\n' || passport_font_notices[visible_end - 1] == '\r')) --visible_end;
    for (;;) {
        size_t length = visible_end - start;
        memcpy(L.line, passport_font_notices + start, length); L.line[length] = '\0';
        lv_point_t extent;
        lv_text_get_size(&extent, L.line, &ui_font_caption, 0, LINE_SPACE, BODY_WIDTH, LV_TEXT_FLAG_EXPAND);
        if (extent.x <= BODY_WIDTH || length == 0) break;
        visible_end = previous_codepoint(start, visible_end);
        cursor = visible_end;
    }
    *visible_bytes = visible_end - start;
    return cursor;
}
static size_t make_page(size_t start, bool write) {
    size_t cursor = start, used = 0;
    unsigned lines = (BODY_HEIGHT + LINE_SPACE) / (lv_font_get_line_height(&ui_font_caption) + LINE_SPACE);
    if (!lines) lines = 1;
    for (unsigned line = 0; line < lines && cursor < L.length; ++line) {
        size_t bytes;
        size_t end = line_end(cursor, &bytes);
        if (end <= cursor || used + bytes + (line ? 1 : 0) >= PAGE_BYTES) break;
        if (line) { if (write) L.text[used] = '\n'; ++used; }
        if (write) memcpy(L.text + used, L.line, bytes);
        used += bytes; cursor = end;
    }
    if (write) L.text[used] = '\0';
    return cursor;
}
static void show_page(void) {
    L.end = make_page(L.offset, true);
    lv_label_set_text_static(L.body, L.text);
    lv_label_set_text_fmt(L.counter, "%u / %u", L.page, L.pages);
    if (L.offset == 0) lv_obj_add_state(L.buttons[0], LV_STATE_DISABLED);
    else lv_obj_remove_state(L.buttons[0], LV_STATE_DISABLED);
    if (L.end >= L.length) lv_obj_add_state(L.buttons[1], LV_STATE_DISABLED);
    else lv_obj_remove_state(L.buttons[1], LV_STATE_DISABLED);
    lv_obj_t *focused = lv_group_get_focused(L.group);
    if (focused && lv_obj_has_state(focused, LV_STATE_DISABLED))
        lv_group_focus_obj(L.buttons[L.end >= L.length ? 2 : 1]);
}
static void navigation(lv_event_t *event) {
    if (L.closing || !L.screen) return;
    unsigned action = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (action == 2) { ui_licenses_close(); return; }
    if (action == 1 && L.end < L.length) {
        L.offset = L.end; ++L.page; show_page();
    } else if (action == 0 && L.offset > 0) {
        size_t cursor = 0, prior = 0;
        while (cursor < L.offset) {
            prior = cursor; cursor = make_page(cursor, false);
        }
        L.offset = prior; --L.page; show_page();
    }
}
static lv_obj_t *button(const char *title, unsigned index) {
    lv_obj_t *button = lv_button_create(L.screen);
    lv_obj_remove_style_all(button); lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(button, 12 + (int)index * 75, 284); lv_obj_set_size(button, 66, 29);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_PANEL), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(button, lv_color_hex(UI_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_group_add_obj(L.group, button);
    lv_obj_add_event_cb(button, navigation, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_t *label = ui_theme_text(button, title, 0, 0, 54, &ui_font_caption, UI_INK);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(label);
    return button;
}
static void owner_deleted(lv_event_t *event) {
    (void)event;
    L.previous = NULL; L.focus = NULL;
    ui_licenses_close();
}
bool ui_licenses_open(void) {
    if (L.opening || L.screen || L.closing) return false;
    L.previous = lv_screen_active(); L.previous_group = lv_group_get_default();
    L.focus = L.previous_group ? lv_group_get_focused(L.previous_group) : NULL;
    if (!L.previous) return false;
    L.opening = true;
    lv_obj_add_event_cb(L.previous, owner_deleted, LV_EVENT_DELETE, NULL);
    if (lv_async_call(open_deferred, NULL) != LV_RESULT_OK) {
        lv_obj_remove_event_cb(L.previous, owner_deleted); memset(&L, 0, sizeof(L)); return false;
    }
    /* A long-OK can be released before open_deferred changes the screen/group.
     * Consume that held gesture now, while its homepage target is still live. */
    for (lv_indev_t *input = lv_indev_get_next(NULL); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) == L.previous_group &&
            lv_indev_get_state(input) == LV_INDEV_STATE_PRESSED)
            lv_indev_wait_release(input);
    }
    return true;
}
static void open_deferred(void *unused) {
    (void)unused;
    if (!L.opening) return;
    L.opening = false;
    if (!L.previous || !lv_obj_is_valid(L.previous) || lv_screen_active() != L.previous) {
        if (L.previous && lv_obj_is_valid(L.previous)) lv_obj_remove_event_cb(L.previous, owner_deleted);
        memset(&L, 0, sizeof(L)); return;
    }
    L.length = strlen(passport_font_notices); L.pages = 0;
    for (size_t cursor = 0; cursor < L.length;) {
        size_t end = make_page(cursor, false);
        if (end <= cursor) { /* Font/data failure cannot expose a partial notice as complete. */
            lv_obj_remove_event_cb(L.previous, owner_deleted); memset(&L, 0, sizeof(L)); return;
        }
        cursor = end; ++L.pages;
    }
    if (!L.pages) L.pages = 1;
    L.page = 1;
    L.group = lv_group_create(); lv_group_set_default(L.group); lv_group_set_wrap(L.group, true);
    L.screen = ui_theme_screen();
    ui_theme_text(L.screen, "字体授权", 12, 13, 126, &ui_font_title, UI_INK);
    L.counter = ui_theme_text(L.screen, "", 138, 16, 90, &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(L.counter, LV_TEXT_ALIGN_RIGHT, 0);
    L.body = ui_theme_text(L.screen, "", 12, 48, BODY_WIDTH, &ui_font_caption, UI_INK);
    lv_obj_set_style_text_line_space(L.body, LINE_SPACE, 0); lv_obj_set_height(L.body, BODY_HEIGHT);
    lv_obj_t *hint = ui_theme_text(L.screen, "上下选择 · 确定", 12, 262, 216, &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    L.buttons[0] = button("上一页", 0); L.buttons[1] = button("下一页", 1); L.buttons[2] = button("返回", 2);
    show_page(); lv_group_focus_obj(L.buttons[L.pages > 1 ? 1 : 2]);
    move_inputs(L.previous_group, L.group); lv_screen_load(ui_theme_screen_obj(L.screen));
}
void ui_licenses_close(void) {
    if (L.closing) return;
    if (L.opening) {
        lv_async_call_cancel(open_deferred, NULL);
        if (L.previous && lv_obj_is_valid(L.previous)) lv_obj_remove_event_cb(L.previous, owner_deleted);
        memset(&L, 0, sizeof(L)); return;
    }
    if (!L.screen) return;
    L.closing = true;
    if (lv_async_call(close_deferred, NULL) != LV_RESULT_OK) L.closing = false;
}
static void close_deferred(void *unused) {
    (void)unused;
    if (!L.screen) return;
    lv_obj_t *screen = L.screen;
    lv_group_t *group = L.group;
    lv_group_t *previous_group = group_valid(L.previous_group) ? L.previous_group : NULL;
    lv_obj_t *previous = L.previous && lv_obj_is_valid(L.previous) ? L.previous : NULL;
    if (previous) lv_obj_remove_event_cb(previous, owner_deleted);
    move_inputs(group, previous_group); lv_group_set_default(previous_group);
    lv_obj_t *to_delete = ui_theme_screen_obj(screen);
    if (!to_delete) to_delete = screen;
    if (previous) lv_screen_load(previous);
    else if (lv_screen_active() == to_delete) lv_screen_load(lv_obj_create(NULL));
    if (L.focus && lv_obj_is_valid(L.focus) && lv_obj_get_group(L.focus) == previous_group)
        lv_group_focus_obj(L.focus);
    L.screen = NULL;
    lv_obj_delete(to_delete); lv_group_delete(group);
    memset(&L, 0, sizeof(L));
}
bool ui_licenses_is_active(void) { return L.opening || L.screen != NULL; }
bool ui_licenses_get_view(ui_licenses_view_t *view) {
    if (!view || !L.screen) return false;
    *view = (ui_licenses_view_t){.offset = L.offset, .end = L.end, .page = L.page, .pages = L.pages};
    return true;
}
