#include "presentation/ui_bobo.h"
#include "presentation/ui_theme.h"
#include "presentation/bobo_art.h"
#include "presentation/pixel_sprite.h"
#include "services/bobo_content.h"
#include <stdio.h>
#include <string.h>

/* One page tree and one home-only animation timer. Persistent state is still
 * acknowledged by the worker; visual randomness never changes the save. */
typedef enum {
    PAGE_HOME, PAGE_CARE, PAGE_CARE_DETAIL, PAGE_BAG, PAGE_ITEM,
    PAGE_DESTINATIONS, PAGE_DESTINATION, PAGE_TOOLS, PAGE_PREVIEW,
    PAGE_TRIP, PAGE_CANCEL, PAGE_READY, PAGE_RECEIPT,
    PAGE_BOOKS, PAGE_CARDS, PAGE_CARD, PAGE_NOTICE, PAGE_GROWTH,
    PAGE_FRIENDS, PAGE_WISH_INVITE, PAGE_WISH_LISTEN, PAGE_WISH_ERROR, PAGE_GIFT, PAGE_ART_UNAVAILABLE, PAGE_FAREWELL
} page_t;
static struct {
    lv_obj_t *screen, *previous;
    bobo_state_t state, before;
    ui_bobo_command_cb_t command;
    ui_bobo_exit_cb_t exit;
    void *user;
    page_t page, notice_return, command_page;
    unsigned selection, home_selection, item, destination, book, card, care, card_side;
    unsigned pending_argument;
    ui_bobo_command_t pending_command;
    bool pending, save_blocked;
    lv_timer_t *animation;
    lv_obj_t *pet, *room, *home_caption, *gift, *gift_pet_window, *gift_caption, *gift_action;
    lv_obj_t *wish_ribbon, *wish_caption, *wish_prompt;
    pet_wish_view_t wish;
    unsigned gift_frame;
    bool wish_running, gift_replay;
    uint32_t animation_random;
    uint8_t beat, frame, wait, previous_beat;
    bool introduced, suspended;
    char notice_title[64], notice_body[384];
    const lv_image_dsc_t *notice_pose;
} ui;

typedef enum { POSE_IDLE, POSE_WAVE, POSE_HAPPY, POSE_SLEEP } pose_t;
typedef enum { BEAT_SUMMON, BEAT_BREATHE, BEAT_WAVE, BEAT_HOP,
               BEAT_LOOK, BEAT_REST, BEAT_TRAVEL } beat_t;
typedef struct { uint8_t pose; int8_t x, y; } pet_frame_t;
/* Native indexed poses, 6.25 fps. No full-image copies, transforms or
 * opacity layers. The shared neutral frames make each gesture settle. */
static const pet_frame_t pet_frames[][16] = {
    [BEAT_SUMMON] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
        {0,0,9},{2,0,-7},{0,0,1},{1,0,-1},{1,0,-1},{0,0,0},
        {1,0,-1},{1,0,-1},{2,0,-2},{2,0,-1},{0,0,0}},
    [BEAT_BREATHE] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,-1},{0,0,-1},
        {0,0,-1},{0,0,-1},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
        {0,0,0},{0,0,0},{0,0,0},{0,0,0}},
    [BEAT_WAVE] = {{0,0,0},{1,1,-1},{1,2,-1},{0,1,0},{1,0,-1},{1,-1,-1},
        {0,0,0},{1,1,-1},{1,2,-1},{0,1,0},{1,0,-1},{1,-1,-1},
        {2,0,0},{2,0,0},{0,0,0},{0,0,0}},
    [BEAT_HOP] = {{0,0,1},{2,0,-3},{2,0,-8},{2,0,-5},{0,0,1},{0,0,0},
        {2,0,-4},{2,0,-9},{2,0,-5},{0,0,1},{2,0,0},{2,0,0},
        {2,0,0},{0,0,0},{0,0,0},{0,0,0}},
    [BEAT_LOOK] = {{0,0,0},{0,-1,0},{0,-3,-1},{0,-4,-1},{0,-4,-1},{0,-3,0},
        {0,0,0},{0,2,0},{1,3,-1},{1,4,-1},{1,4,-1},{0,2,0},
        {0,0,0},{0,0,0},{0,0,0},{0,0,0}},
    [BEAT_REST] = {{0,0,0},{3,0,1},{3,0,1},{3,0,1},{3,0,0},{3,0,0},
        {3,0,0},{3,0,1},{3,0,1},{3,0,1},{3,0,0},{3,0,0},
        {3,0,0},{3,0,1},{3,0,1},{0,0,0}},
    [BEAT_TRAVEL] = {{0,-4,0},{1,-3,-2},{0,-2,0},{1,-1,-2},{0,0,0},{1,1,-2},
        {0,2,0},{1,3,-2},{0,4,0},{1,3,-2},{0,2,0},{1,1,-2},
        {0,0,0},{1,-1,-2},{0,-2,0},{0,0,0}}
};
#define AGE_POSES(age, size) { &bobo_##age##_##size##_idle, &bobo_##age##_##size##_wave, \
    &bobo_##age##_##size##_happy, &bobo_##age##_##size##_sleep }
#define PET_POSES(species, age, size) { &pet_##species##_##age##_##size##_idle, &pet_##species##_##age##_##size##_wave, \
    &pet_##species##_##age##_##size##_happy, &pet_##species##_##age##_##size##_sleep }
#define PET_AGES(species) { {PET_POSES(species, baby, detail), PET_POSES(species, baby, home)}, \
    {PET_POSES(species, junior, detail), PET_POSES(species, junior, home)}, \
    {PET_POSES(species, grown, detail), PET_POSES(species, grown, home)} }
static const lv_image_dsc_t *pet_pose(pose_t pose, bool home) {
    static const lv_image_dsc_t *const images[BOBO_PET_COUNT][3][2][4] = {{
        {AGE_POSES(baby, detail), AGE_POSES(baby, home)},
        {AGE_POSES(junior, detail), AGE_POSES(junior, home)},
        {AGE_POSES(grown, detail), AGE_POSES(grown, home)}
    }, PET_AGES(sunflower), PET_AGES(cactus), PET_AGES(panda), PET_AGES(lele)};
    return images[ui.state.active_pet][bobo_growth_stage(&ui.state)][home ? 1 : 0][pose];
}
#undef AGE_POSES
#undef PET_POSES
#undef PET_AGES
static const char *pet_name(void) { return bobo_pet_name(ui.state.active_pet); }

/* Ink-blue night room: text and surfaces have distinct tokens. */
#define PET_BG 0x1B3441
#define PET_PANEL 0x203D4B
#define PET_TEXT 0xF2F1E8
#define PET_MUTED 0xAEC4CD
#define PET_LINE 0x42616F
#define PET_ACCENT 0xA8D6C4
#define PET_FOCUS 0x365766
#define PET_BLUE 0x9DBECB
#define PET_GREEN 0xA0C7B6
#define PET_SLATE 0xB0C7D0
#define PET_GOLD 0xE8D2A0
static uint32_t pet_color(uint32_t color) {
    if (color == UI_INK) return PET_TEXT;
    if (color == UI_ACCENT) return PET_ACCENT;
    if (color == UI_MUTED) return PET_MUTED;
    if (color == UI_WARNING) return 0xE5B8A0;
    return color;
}
static lv_obj_t *pet_text(lv_obj_t *parent, const char *value, int x, int y,
                          int width, const lv_font_t *font, uint32_t color) {
    /* Every selected keycap, including its subtitle, stays legible on deep blue. */
    bool dark_key = lv_obj_get_style_border_width(parent, 0) == 2 &&
                    lv_color_eq(lv_obj_get_style_bg_color(parent, 0), lv_color_hex(PET_FOCUS));
    return ui_theme_text(parent, value, x, y, width, font, dark_key ? PET_TEXT : pet_color(color));
}
static lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y); lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, bool focused) {
    lv_obj_t *obj = shape(parent, x, y, w, h, focused ? PET_FOCUS : PET_PANEL, 5);
    lv_obj_set_style_border_width(obj, focused ? 2 : 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(focused ? PET_ACCENT : PET_LINE), 0);
    return obj;
}
static void icon(lv_obj_t *parent, const lv_image_dsc_t *image, int x, int y) {
    lv_obj_t *obj = lv_image_create(parent);
    lv_image_set_src(obj, image); lv_obj_set_pos(obj, x, y);
}
static void render(void);
static unsigned choice_count(void) {
    switch (ui.page) {
        case PAGE_CARDS: return 5;
        case PAGE_HOME: case PAGE_BAG: return 6;
        case PAGE_FRIENDS: return 1;
        case PAGE_WISH_LISTEN: case PAGE_ART_UNAVAILABLE: return 1;
        case PAGE_CARE: case PAGE_DESTINATIONS: case PAGE_TOOLS: case PAGE_BOOKS: return 4;
        case PAGE_NOTICE: return ui.save_blocked ? 2 : 1;
        case PAGE_RECEIPT: return ui.state.receipt.card_id < BOBO_CARD_COUNT ? 2 : 1;
        case PAGE_DESTINATION: case PAGE_PREVIEW: {
            bobo_trip_plan_t plan;
            return bobo_preview(&ui.state, ui.destination, &plan) == BOBO_OK ? 2 : 1;
        }
        default: return 2;
    }
}
static bool list_page(void) {
    return ui.page == PAGE_CARE || ui.page == PAGE_BAG || ui.page == PAGE_DESTINATIONS ||
           ui.page == PAGE_TOOLS || ui.page == PAGE_BOOKS || ui.page == PAGE_CARDS;
}
static void go(page_t page, unsigned selection) {
    if ((page == PAGE_HOME || page == PAGE_FRIENDS) && ui.state.health == BOBO_DEAD) page = PAGE_FAREWELL;
    if (page == PAGE_HOME && ui.state.pending_reveal) page = PAGE_GIFT;
    if (page == PAGE_HOME && !ui.state.owned_pets) page = PAGE_WISH_INVITE;
    if (page == PAGE_GIFT && ui.page != PAGE_GIFT) {
        ui.gift_frame = 0; ui.gift_replay = !ui.state.pending_reveal;
    }
    ui.page = page;
    ui.selection = page == PAGE_CANCEL || page == PAGE_FAREWELL ? 1 : selection;
    render();
}
static lv_obj_t *text(const char *value, int x, int y, int w, uint32_t color) {
    return pet_text(ui.screen, value, x, y, w, &ui_font_body, color);
}
static lv_obj_t *sprite(const lv_image_dsc_t *image, int x, int y) {
    lv_obj_t *obj = lv_image_create(ui.screen);
    lv_image_set_src(obj, image); lv_obj_set_pos(obj, x, y);
    return obj;
}
static void row(const char *title, const char *detail, unsigned index, int y, int h) {
    lv_obj_t *p = panel(ui.screen, 14, y, 212, h, ui.selection == index);
    pet_text(p, title, 10, 5, 190, &ui_font_body, ui.selection == index ? UI_ACCENT : UI_INK);
    if (detail && detail[0])
        pet_text(p, detail, 10, 28, 190, &ui_font_caption, UI_MUTED);
}
static lv_obj_t *action(const char *title) {
    if (choice_count() == 1) {
        row("返回", NULL, 0, 250, 35);
        return NULL;
    }
    lv_obj_t *primary = panel(ui.screen, 14, 250, 154, 35, ui.selection == 0);
    lv_obj_t *label = pet_text(primary, title, 10, 5, 132, &ui_font_body, ui.selection == 0 ? UI_ACCENT : UI_INK);
    lv_obj_t *return_button = panel(ui.screen, 172, 250, 54, 35, ui.selection == 1);
    pet_text(return_button, "返回", 9, 5, 36, &ui_font_body, ui.selection == 1 ? UI_ACCENT : UI_INK);
    return label;
}
static void header(const char *title) {
    pet_text(ui.screen, title, 14, 17, 212, &ui_font_title, UI_INK);
    shape(ui.screen, 14, 49, 212, 1, PET_LINE, 0);
}
static void illustrated_row(const char *title, const char *detail, const lv_image_dsc_t *image,
                            unsigned index, int y) {
    lv_obj_t *p = panel(ui.screen, 14, y, 212, 54, ui.selection == index);
    icon(p, image, 8, 9);
    pet_text(p, title, 50, 5, 148, &ui_font_body, UI_INK);
    pet_text(p, detail, 50, 29, 148, &ui_font_caption, UI_MUTED);
}
static bool owns(unsigned card) { return card < BOBO_CARD_COUNT && (ui.state.collected & (1u << card)); }
static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }
static const char *card_name(unsigned card) {
    return card < BOBO_CARD_COUNT ? bobo_cards[card].name : "本馆已集齐";
}
static void card_paragraph(const char *value) {
    /* Break at the original comma, keeping the source words and punctuation.
     * This avoids an isolated final punctuation glyph with LVGL CJK wrapping. */
    char lines[256];
    const char *comma = strstr(value, "，");
    if (comma) {
        size_t split = (size_t)(comma - value) + strlen("，");
        snprintf(lines, sizeof(lines), "%.*s\n%s", (int)split, value, value + split);
    } else {
        snprintf(lines, sizeof(lines), "%s", value);
    }
    text(lines, 26, 156, 188, UI_INK);
}
static const char *result_text(bobo_result_t result) {
    switch (result) {
        case BOBO_LOCKED: return "还未解锁这个小馆。\n先收集其他观察卡吧。";
        case BOBO_BUSY: return "先完成眼前的小旅行\n或待揭晓的礼物吧。";
        case BOBO_AWAY: return "伙伴正在旅行。\n回来再一起整理。";
        case BOBO_COOLDOWN: return "刚刚照顾过伙伴。\n稍等一会儿再来吧。";
        case BOBO_FULL: return "现在已经满足啦。\n不消耗物品和冷却。";
        case BOBO_EMPTY: return "点心吃完啦。\n小旅行会带回补给。";
        case BOBO_NOT_READY: return "当前没有可领取的发现。";
        case BOBO_ALREADY_OWNED: return "这张观察卡已经收好啦。";
        default: return "这次操作没有完成。\n请返回后再试一次。";
    }
}
static void notice(const char *title, const char *body, page_t back,
                   const lv_image_dsc_t *pose) {
    snprintf(ui.notice_title, sizeof(ui.notice_title), "%s", title);
    snprintf(ui.notice_body, sizeof(ui.notice_body), "%s", body);
    ui.notice_return = back; ui.notice_pose = pose; go(PAGE_NOTICE, 0);
}
static void send_command(ui_bobo_command_t command, unsigned argument) {
    if (ui.pending) return;
    if (ui.save_blocked && command != UI_BOBO_RETRY_SAVE && command != UI_BOBO_WISH_CANCEL) {
        notice("存档暂不可用", "进度保留在当前状态。\n请先重试读取存档。", ui.page, NULL);
        return;
    }
    ui.pending = true; ui.pending_command = command;
    ui.pending_argument = argument; ui.command_page = ui.page; ui.before = ui.state;
    if (!ui.command || !ui.command(command, argument, ui.user)) {
        ui.pending = false;
        notice("请再试一次", "操作暂时排不上队。\n尚未完成，请返回重试。",
               ui.page == PAGE_NOTICE ? ui.notice_return : ui.page, NULL);
    } else render();
}
static const char *care_name(unsigned action_id) {
    static const char *const names[] = {"喝水", "整理熊帽", "聊一聊", "吃一份点心"};
    if (action_id == BOBO_TIDY && ui.state.active_pet != BOBO_PET_BOBO) return "整理一下";
    return names[action_id < 4 ? action_id : 0];
}
static void care_detail(unsigned action_id, bool item_page) {
    char buf[192];
    bobo_state_t candidate = ui.state;
    bobo_result_t result = bobo_care(&candidate, (bobo_care_t)action_id);
    header(item_page ? bobo_item_names[ui.item] : care_name(action_id));
    sprite(pet_pose(action_id == BOBO_TIDY ? POSE_HAPPY : action_id == BOBO_CHAT ? POSE_WAVE : POSE_IDLE, false), 80, 62);
    if (action_id == BOBO_SNACK) snprintf(buf, sizeof(buf), "点心 %u/9 · 消耗 1 份", ui.state.snacks);
    else snprintf(buf, sizeof(buf), "%s", "可重复使用 · 不消耗");
    text(buf, 14, 148, 212, UI_MUTED);
    if (result == BOBO_OK) {
        if (action_id == BOBO_SNACK)
            snprintf(buf, sizeof(buf), "精神 +%u  心情 +%u\n冷却 10 秒",
                     candidate.energy - ui.state.energy, candidate.mood - ui.state.mood);
        else {
            unsigned delta = action_id == BOBO_WATER ? candidate.energy - ui.state.energy :
                             action_id == BOBO_TIDY ? candidate.tidy - ui.state.tidy : candidate.mood - ui.state.mood;
            snprintf(buf, sizeof(buf), "%s +%u\n冷却 %u 分钟 · 无经验奖励",
                     action_id == BOBO_WATER ? "精神" : action_id == BOBO_TIDY ? "整洁" : "心情",
                     delta, action_id == BOBO_WATER ? 10 : 5);
        }
    } else if (result == BOBO_COOLDOWN) {
        snprintf(buf, sizeof(buf), "还需等 %u 分 %02u 秒\n当前不会消耗物品", ui.state.cooldown_s[action_id] / 60,
                 ui.state.cooldown_s[action_id] % 60);
    } else snprintf(buf, sizeof(buf), "%s", result_text(result));
    text(buf, 14, 178, 212, result == BOBO_OK ? UI_INK : UI_MUTED);
    action(result == BOBO_OK ? care_name(action_id) : "确定查看提示");
}
/* One draw-event object paints the room; rectangles are draw commands, not
 * individual LVGL objects or a decoded background bitmap. */
typedef struct { uint8_t x, y, w, h; uint32_t color; } room_rect_t;
static void draw_room(lv_event_t *event) {
    static const room_rect_t pieces[] = {
        {0, 0, 216, 132, 0x263F50},       /* ink-blue wall */
        {0, 98, 216, 34, 0x47636E},       /* quiet floor */
        {0, 95, 216, 3, 0x304D5C},
        {0, 109, 216, 1, 0x587480},
        {0, 123, 216, 1, 0x587480},
        {48, 98, 1, 11, 0x587480},
        {167, 110, 1, 13, 0x587480},
        {14, 16, 46, 56, 0x5F8090},       /* left window */
        {17, 19, 40, 50, 0x86AABD},
        {20, 22, 34, 44, 0x152C3E},
        {20, 48, 34, 18, 0x1C3948},
        {24, 27, 8, 5, 0xE8D2A0},        /* small warm moonlight */
        {35, 21, 3, 46, 0x86AABD},
        {19, 43, 36, 3, 0x86AABD},
        {11, 72, 52, 3, 0x6D8995},       /* sill */
        {173, 47, 29, 51, 0x536F7B},      /* slender shelf */
        {176, 50, 23, 45, 0x263F4C},
        {176, 65, 23, 3, 0x536F7B},
        {176, 82, 23, 3, 0x536F7B},
        {178, 54, 4, 11, 0x88A899},
        {183, 52, 5, 13, 0xB8C5C1},
        {190, 55, 6, 10, 0x9DADBC},
        {178, 75, 15, 3, 0x799C96},
        {178, 78, 17, 4, 0xB5C3C9},
        {181, 89, 12, 5, 0xC6BBA3},
        {184, 39, 10, 8, 0x82968E},       /* small plant */
        {188, 25, 2, 14, 0x8AAC9A},
        {182, 26, 6, 5, 0x91B5A2},
        {190, 20, 5, 8, 0xACC6A7},
        {191, 32, 6, 4, 0x8AAC9A},
        {62, 108, 92, 18, 0x2D4B55},      /* simple woven rug */
        {65, 111, 86, 12, 0x547679},
        {69, 111, 2, 12, 0x6F8E8C},
        {145, 111, 2, 12, 0x6F8E8C}
    };
    lv_obj_t *object = lv_event_get_target(event);
    lv_area_t origin; lv_obj_get_coords(object, &origin);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_draw_rect_dsc_t rect; lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_COVER;
    for (unsigned i = 0; i < sizeof(pieces) / sizeof(pieces[0]); ++i) {
        const room_rect_t *piece = &pieces[i];
        lv_area_t area = {.x1 = origin.x1 + piece->x, .y1 = origin.y1 + piece->y,
                          .x2 = origin.x1 + piece->x + piece->w - 1,
                          .y2 = origin.y1 + piece->y + piece->h - 1};
        rect.bg_color = lv_color_hex(piece->color);
        lv_draw_rect(layer, &rect, &area);
    }
    /* A few tiny draw commands give the anticipation and gestures a visible
     * rhythm without creating particle objects or allocating a draw layer. */
    if (object != ui.room || ui.page != PAGE_HOME || ui.suspended) return;
    if (ui.beat == BEAT_SUMMON && ui.frame < 8) {
        int pulse = ui.frame < 5 ? ui.frame : 7 - ui.frame;
        int width = 28 + pulse * 10, height = 8 + pulse * 2;
        lv_area_t ring = {.x1 = origin.x1 + 108 - width / 2, .y1 = origin.y1 + 111 - height / 2,
                          .x2 = origin.x1 + 108 + width / 2, .y2 = origin.y1 + 111 + height / 2};
        rect.bg_opa = LV_OPA_TRANSP; rect.border_opa = LV_OPA_COVER;
        rect.border_width = 2; rect.border_color = lv_color_hex(PET_ACCENT); rect.radius = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &rect, &ring);
        rect.bg_opa = LV_OPA_COVER; rect.border_width = 0; rect.radius = 0;
    }
    for (int i = 0; i < 3; ++i) {
        int x, y, size = 3;
        if (ui.beat == BEAT_SUMMON && ui.frame < 8) {
            int reach = 28 - (int)ui.frame * 3;
            x = 106 + (i - 1) * reach;
            y = 96 - (i == 1 ? 8 : 0) - ui.frame * 2;
        } else if ((ui.beat == BEAT_HOP || ui.beat == BEAT_WAVE) && ui.frame < 12) {
            x = 65 + i * 40; y = 49 - (ui.frame % 4) * 3;
            if (i == (int)(ui.frame % 3)) continue;
            size = 2;
        } else if (ui.beat == BEAT_REST && ui.frame > 2 && ui.frame < 15) {
            x = 143 + i * 5; y = 62 - i * 8 - (ui.frame % 4); size = i == 0 ? 3 : 2;
        } else break;
        lv_area_t area = {.x1 = origin.x1 + x, .y1 = origin.y1 + y,
                          .x2 = origin.x1 + x + size - 1, .y2 = origin.y1 + y + size - 1};
        rect.bg_color = lv_color_hex(i == 1 ? 0xE8D2A0 : PET_ACCENT);
        lv_draw_rect(layer, &rect, &area);
    }
}
static lv_obj_t *room_scene(void) {
    lv_obj_t *room = lv_obj_create(ui.screen);
    lv_obj_remove_style_all(room); lv_obj_remove_flag(room, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(room, 12, 60); lv_obj_set_size(room, 216, 132);
    lv_obj_add_event_cb(room, draw_room, LV_EVENT_DRAW_MAIN, NULL);
    return room;
}
static int home_pose_y(void) {
    /* The native 120px sprites retain the age-specific scale. Transparent
     * margins are already native pixels; visible feet share y=180. */
    static const uint8_t margins[] = {18, 10, 4};
    return 180 - (120 - margins[bobo_growth_stage(&ui.state)]);
}
static uint32_t animation_random(void) {
    uint32_t value = ui.animation_random;
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return ui.animation_random = value;
}
static void home_caption(char *buf, size_t capacity) {
    if (ui.state.health == BOBO_ILL) snprintf(buf, capacity, "好好休息，等你来照顾");
    else if (ui.beat == BEAT_SUMMON)
        snprintf(buf, capacity, ui.frame < 5 ? "%s，快来呀" : "来啦，今天也一起玩", pet_name());
    else if (ui.state.phase == BOBO_TRAVELLING)
        snprintf(buf, capacity, "小旅行中 · 还剩 %u 分钟", (ui.state.trip.remaining_s + 59) / 60);
    else if (ui.state.phase == BOBO_READY) snprintf(buf, capacity, "新发现回来啦，去旅行看看");
    else if (bobo_growth_remaining(&ui.state))
        snprintf(buf, capacity, "再领取 %u 次旅行就长大", bobo_growth_remaining(&ui.state));
    else snprintf(buf, capacity, "%s", bobo_count(&ui.state) == 12 ? "我们的观察册收集完成啦" : "长大了，好奇心还在呀");
}
static void animation_apply(void) {
    if (!ui.pet) return;
    const pet_frame_t *frame = &pet_frames[ui.beat][ui.frame];
    const lv_image_dsc_t *source = pet_pose(ui.state.health == BOBO_ILL ? POSE_SLEEP : (pose_t)frame->pose, true);
    if (lv_image_get_src(ui.pet) != source) lv_image_set_src(ui.pet, source);
    lv_obj_set_pos(ui.pet, 60 + (ui.state.health == BOBO_ILL ? 0 : frame->x),
                   home_pose_y() + (ui.state.health == BOBO_ILL ? 0 : frame->y));
    if (ui.state.health != BOBO_ILL && ui.beat == BEAT_SUMMON && ui.frame < 5) lv_obj_add_flag(ui.pet, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(ui.pet, LV_OBJ_FLAG_HIDDEN);
    if (ui.room) {
        lv_area_t sparks; lv_obj_get_coords(ui.room, &sparks);
        sparks.x2 = sparks.x1 + 156; sparks.x1 += 64;
        sparks.y2 = sparks.y1 + 121; sparks.y1 += 35;
        lv_obj_invalidate_area(ui.room, &sparks);
    }
    if (ui.home_caption) {
        char buf[96]; home_caption(buf, sizeof(buf));
        if (strcmp(lv_label_get_text(ui.home_caption), buf)) lv_label_set_text(ui.home_caption, buf);
    }
}
static beat_t animation_choose(void) {
    static const uint8_t normal[] = {BEAT_WAVE, BEAT_HOP, BEAT_LOOK, BEAT_REST};
    static const uint8_t quiet[] = {BEAT_REST, BEAT_BREATHE, BEAT_REST, BEAT_LOOK};
    static const uint8_t subdued[] = {BEAT_LOOK, BEAT_REST, BEAT_BREATHE, BEAT_WAVE};
    static const uint8_t ready[] = {BEAT_HOP, BEAT_WAVE, BEAT_LOOK, BEAT_HOP};
    static const uint8_t travel[] = {BEAT_TRAVEL, BEAT_WAVE, BEAT_LOOK, BEAT_TRAVEL};
    const uint8_t *choices = ui.state.phase == BOBO_TRAVELLING ? travel :
        ui.state.energy < 40 ? quiet : ui.state.phase == BOBO_READY ? ready :
        ui.state.mood < 40 ? subdued : normal;
    unsigned index = animation_random() % 4;
    while (choices[index] == ui.previous_beat) index = (index + 1) % 4;
    ui.previous_beat = choices[index];
    return (beat_t)choices[index];
}
static const lv_image_dsc_t *gift_pose(unsigned frame) {
    static const lv_image_dsc_t *const images[] = {&pet_gift_0, &pet_gift_1, &pet_gift_2, &pet_gift_3,
        &pet_gift_4, &pet_gift_5, &pet_gift_6, &pet_gift_7};
    return images[frame < 8 ? frame : 7];
}
static void label_change(lv_obj_t *label, const char *value) {
    if (label && strcmp(lv_label_get_text(label), value)) lv_label_set_text(label, value);
}
static void gift_apply(void) {
    if (!ui.gift || !ui.pet) return;
    unsigned frame = ui.gift_frame;
    const lv_image_dsc_t *box = gift_pose(frame < 4 ? 5 : frame < 8 ? 6 : 7);
    if (lv_image_get_src(ui.gift) != box) lv_image_set_src(ui.gift, box);
    lv_obj_set_pos(ui.gift, frame < 12 ? 60 : 60 + (int)(frame - 12) * 24, 89);
    if (frame >= 20) lv_obj_add_flag(ui.gift, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(ui.gift, LV_OBJ_FLAG_HIDDEN);
    if (frame < 5) lv_obj_add_flag(ui.pet, LV_OBJ_FLAG_HIDDEN);
    else {
        lv_obj_remove_flag(ui.pet, LV_OBJ_FLAG_HIDDEN);
        const lv_image_dsc_t *pet = pet_pose(frame < 12 ? POSE_IDLE : frame % 8 < 4 ? POSE_WAVE : POSE_HAPPY, true);
        if (lv_image_get_src(ui.pet) != pet) lv_image_set_src(ui.pet, pet);
        int rise = frame < 12 ? (int)(12 - frame) * 7 : 0;
        static const uint8_t margins[] = {18, 10, 4};
        int resting_y = 30 - margins[bobo_growth_stage(&ui.state)];
        lv_obj_set_pos(ui.pet, 0, resting_y + rise - (frame >= 20 && frame % 8 >= 4 ? 2 : 0));
        if (frame >= 12 && ui.gift_pet_window) {
            lv_obj_set_height(ui.gift_pet_window, 156);
            lv_obj_move_foreground(ui.gift_pet_window);
        }
    }
    if (ui.gift_caption) {
        char line[96];
        if (frame < 12) snprintf(line, sizeof(line), "礼盒里有一位小伙伴");
        else snprintf(line, sizeof(line), "%s，来啦", pet_name());
        label_change(ui.gift_caption, line);
    }
    label_change(ui.gift_action,
        frame < 20 ? "礼盒正在打开" : ui.gift_replay ? "回到小屋" : "带它回家");
}
static void animate_pet(lv_timer_t *timer) {
    (void)timer;
    if (ui.screen && ui.page == PAGE_GIFT && !ui.suspended && lv_screen_active() == ui.screen) {
        if (ui.gift_frame < 27) ++ui.gift_frame;
        else ui.gift_frame = 20;
        gift_apply(); return;
    }
    if (!ui.screen || ui.page != PAGE_HOME || ui.suspended || !ui.pet ||
        lv_screen_active() != ui.screen) return;
    if (ui.wait && --ui.wait == 0) {
        ui.beat = animation_choose(); ui.frame = 0;
    } else if (++ui.frame == 16) {
        ui.frame = 0;
        if (!ui.wait) {
            ui.beat = ui.state.energy < 40 && ui.state.phase == BOBO_HOME ? BEAT_REST : BEAT_BREATHE;
            ui.wait = 12 + animation_random() % 17;
        }
    }
    animation_apply();
}
static void render_home(void) {
    char buf[96];
    if (!ui.introduced) { ui.introduced = true; ui.beat = BEAT_SUMMON; ui.frame = ui.wait = 0; }
    snprintf(buf, sizeof(buf), "%s的小屋", pet_name());
    pet_text(ui.screen, buf, 12, 11, 168, &ui_font_title, UI_INK);
    lv_obj_t *level = shape(ui.screen, 186, 12, 42, 22, PET_FOCUS, 4);
    snprintf(buf, sizeof(buf), "Lv.%u", (unsigned)bobo_growth_stage(&ui.state) + 1);
    lv_obj_t *label = pet_text(level, buf, 2, 2, 38, &ui_font_caption, UI_INK);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    pet_text(ui.screen, bobo_growth_name(&ui.state), 12, 39, 96, &ui_font_caption, UI_MUTED);
    snprintf(buf, sizeof(buf), "观察卡 %u/12", bobo_count(&ui.state));
    label = pet_text(ui.screen, buf, 116, 39, 112, &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
    ui.room = room_scene();
    ui.pet = sprite(pet_pose(POSE_IDLE, true), 60, home_pose_y());
    static const char *const stats[] = {"精神", "心情", "整洁"};
    const unsigned values[] = {ui.state.energy, ui.state.mood, ui.state.tidy};
    const uint32_t colors[] = {PET_BLUE, PET_GREEN, PET_SLATE};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *stat = shape(ui.screen, 12 + (int)i * 74, 198, 68, 32, PET_PANEL, 5);
        pet_text(stat, stats[i], 5, 3, 30, &ui_font_caption, UI_MUTED);
        snprintf(buf, sizeof(buf), "%u", values[i]);
        label = pet_text(stat, buf, 37, 3, 26, &ui_font_caption, UI_INK);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        shape(stat, 5, 25, 58, 3, PET_LINE, 0);
        shape(stat, 5, 25, (int)(58 * values[i] / 100), 3, colors[i], 0);
    }
    home_caption(buf, sizeof(buf));
    ui.home_caption = label = pet_text(ui.screen, buf, 12, 234, 216, &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    static const char *const entries[] = {"照顾", "背包", "旅行", "图鉴", "伙伴", "返回"};
    const lv_image_dsc_t *const images[] = {&pet_icon_water, &pet_icon_backpack, &pet_icon_map, &pet_icon_album, &pet_icon_gift};
    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *p = panel(ui.screen, 8 + (int)i * 38, 255, 35, 54, ui.selection == i);
        if (i < 5) icon(p, images[i], 0, 1);
        else {
            static const lv_point_precise_t arrow[] = {{22, 8}, {11, 17}, {22, 26}};
            lv_obj_t *line = lv_line_create(p); lv_line_set_points(line, arrow, 3);
            lv_obj_set_style_line_width(line, 3, 0);
            lv_obj_set_style_line_color(line, lv_color_hex(ui.selection == i ? PET_ACCENT : PET_TEXT), 0);
            lv_obj_set_style_line_rounded(line, true, 0);
        }
        label = pet_text(p, entries[i], 0, 34, 32, &ui_font_caption, UI_INK);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    animation_apply();
    if (!ui.animation) ui.animation = lv_timer_create(animate_pet, 160, NULL);
    if (!ui.animation) {
        /* A scarce timer allocation must never leave an invisible pet. */
        ui.beat = BEAT_BREATHE; ui.frame = 0; animation_apply();
    } else if (!ui.suspended) lv_timer_resume(ui.animation);
}
static void render_wish(void) {
    header(ui.page == PAGE_GIFT ? "愿望收到啦" : "许愿小礼盒");
    const char *prompt_text = ui.page == PAGE_GIFT ? "小伙伴正在来到你身边" :
        ui.page == PAGE_WISH_INVITE ? "按下开始，等待提示" :
        ui.page == PAGE_WISH_ERROR ? "礼盒暂时听不到你" :
        ui.wish.phase == PET_WISH_CALIBRATING ? "先安静片刻" : "现在说出你的愿望";
    lv_obj_t *prompt = ui.wish_prompt = pet_text(ui.screen, prompt_text,
        14, 66, 212, &ui_font_body, PET_GOLD);
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, 0);
    if (ui.page == PAGE_GIFT) {
        // Clip only the small emerging sprite at the box baseline: feet never
        // appear beneath the closed box. This creates no opacity or scale layer.
        ui.gift_pet_window = lv_obj_create(ui.screen);
        lv_obj_remove_style_all(ui.gift_pet_window);
        lv_obj_remove_flag(ui.gift_pet_window, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(ui.gift_pet_window, 60, 60); lv_obj_set_size(ui.gift_pet_window, 120, 136);
        ui.pet = lv_image_create(ui.gift_pet_window);
        lv_image_set_src(ui.pet, pet_pose(POSE_IDLE, true)); lv_obj_set_pos(ui.pet, 0, 61);
        ui.gift = sprite(gift_pose(5), 60, 89);
        ui.gift_caption = pet_text(ui.screen, "礼盒里有一位小伙伴", 14, 219, 212, &ui_font_caption, UI_MUTED);
        lv_obj_set_style_text_align(ui.gift_caption, LV_TEXT_ALIGN_CENTER, 0);
        ui.gift_action = action("礼盒正在打开");
        gift_apply();
        if (!ui.animation) ui.animation = lv_timer_create(animate_pet, 160, NULL);
        if (ui.animation && !ui.suspended) lv_timer_resume(ui.animation);
        else if (!ui.animation) { ui.gift_frame = 20; gift_apply(); }
        return;
    }
    unsigned progress = ui.page == PAGE_WISH_LISTEN ? min_u(ui.wish.progress, 1000) : 0;
    ui.gift = sprite(gift_pose(progress * 4 / 1000), 60, 89);
    const char *caption = ui.page == PAGE_WISH_INVITE ? "礼盒准备好，再轻声许愿" :
        ui.page == PAGE_WISH_ERROR ? (ui.wish.phase == PET_WISH_MIC_ERROR ? "麦克风暂时不可用" : "愿望还没说完，再试一次吧") :
        ui.wish.phase == PET_WISH_CALIBRATING ? "正在听周围的声音" : "慢慢说，丝带会轻轻松开";
    ui.wish_caption = pet_text(ui.screen, caption, 14, 211, 212, &ui_font_caption, UI_MUTED);
    lv_obj_set_style_text_align(ui.wish_caption, LV_TEXT_ALIGN_CENTER, 0);
    shape(ui.screen, 26, 234, 188, 5, PET_LINE, 2);
    ui.wish_ribbon = shape(ui.screen, 26, 234, (int)(188 * progress / 1000), 5, PET_GOLD, 2);
    if (!progress) lv_obj_add_flag(ui.wish_ribbon, LV_OBJ_FLAG_HIDDEN);
    action(ui.page == PAGE_WISH_ERROR ? "再说一次" : "开始许愿");
}
static void render(void) {
    if (!ui.screen) return;
    if (ui.animation) lv_timer_pause(ui.animation);
    ui.pet = ui.room = ui.home_caption = ui.gift = ui.gift_pet_window = ui.gift_caption = ui.gift_action = NULL;
    ui.wish_ribbon = ui.wish_caption = ui.wish_prompt = NULL;
    if (ui.page != PAGE_HOME) { ui.beat = BEAT_BREATHE; ui.frame = 0; ui.wait = 12; }
    if (ui.selection >= choice_count()) ui.selection = choice_count() - 1;
    lv_obj_clean(ui.screen);
    lv_obj_set_style_bg_color(ui.screen, lv_color_hex(PET_BG), 0);
    char buf[256];
    switch (ui.page) {
        case PAGE_ART_UNAVAILABLE:
            header("小伙伴还在准备");
            text("礼盒画面暂时打不开。\n请返回后再试一次。", 14, 94, 212, UI_INK);
            action("返回"); break;
        case PAGE_WISH_INVITE: case PAGE_WISH_LISTEN: case PAGE_WISH_ERROR: case PAGE_GIFT:
            render_wish(); break;
        case PAGE_FRIENDS: {
            header("我的小伙伴");
            sprite(pet_pose(ui.state.health == BOBO_ILL ? POSE_SLEEP : POSE_IDLE, true), 60, 55);
            snprintf(buf, sizeof(buf), "%s · %s", pet_name(), bobo_growth_name(&ui.state));
            lv_obj_t *line = pet_text(ui.screen, buf, 14, 175, 212, &ui_font_body, UI_INK);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
            line = pet_text(ui.screen, ui.state.health == BOBO_ILL ? "生病了，照顾一下吧" : "一直陪着你",
                            14, 202, 212, &ui_font_caption, UI_MUTED);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
            line = pet_text(ui.screen, "未照顾 6h 生病 · 12h 离世", 14, 225, 212, &ui_font_caption, UI_MUTED);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
            action("返回"); break;
        }
        case PAGE_FAREWELL: {
            header("和伙伴说再见");
            sprite(pet_pose(POSE_SLEEP, true), 60, 55);
            snprintf(buf, sizeof(buf), "谢谢你，%s", pet_name());
            lv_obj_t *line = pet_text(ui.screen, buf, 14, 177, 212, &ui_font_body, UI_INK);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
            line = pet_text(ui.screen, "这段陪伴，留在心里。\n准备好后，可以再次许愿。",
                            14, 209, 212, &ui_font_caption, UI_MUTED);
            lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
            action("再次许愿"); break;
        }
        case PAGE_HOME: render_home(); break;
        case PAGE_CARE:
            snprintf(buf, sizeof(buf), "陪%s待一会儿", pet_name()); header(buf);
            for (unsigned i = 0; i < 3; ++i) {
                snprintf(buf, sizeof(buf), "%s", i == 0 ? "精神 · 喝口水" : i == 1 ? "整洁 · 整理一下" : "心情 · 说说今天");
                illustrated_row(care_name(i), buf, i == 0 ? &pet_icon_water : i == 1 ? &pet_icon_towel : &pet_icon_album,
                                i, 72 + (int)i * 58);
            }
            break;
        case PAGE_CARE_DETAIL: care_detail(ui.care, false); break;
        case PAGE_BAG: {
            snprintf(buf, sizeof(buf), "%s的小背包", pet_name()); header(buf);
            unsigned last_item = min_u(ui.selection, 4);
            unsigned first = last_item > 2 ? last_item - 2 : 0;
            for (unsigned i = first; i < first + 3 && i < 5; ++i) {
                if (i == 2) snprintf(buf, sizeof(buf), "%u/9 份 · 旅行带回", ui.state.snacks);
                else snprintf(buf, sizeof(buf), "%s", i >= 3 && ui.state.tool == i - 2 ? "可重复使用 · 已带上" : "常用品 · 可重复使用");
                const lv_image_dsc_t *const images[] = {&pet_icon_water, &pet_icon_towel, &pet_icon_snack,
                                                       &pet_icon_sketchbook, &pet_icon_lens};
                illustrated_row(bobo_item_names[i], buf, images[i], i, 72 + (int)(i - first) * 58);
            }
            break;
        }
        case PAGE_ITEM:
            if (ui.item < 3) care_detail(ui.item == 2 ? BOBO_SNACK : ui.item, true);
            else {
                header(bobo_item_names[ui.item]); sprite(pet_pose(POSE_WAVE, false), 80, 62);
                text("常用品 · 可重复使用", 14, 146, 212, UI_MUTED);
                text(ui.item == 3 ? "下次优先找形状和线条。\n这类已齐会找其他新卡。" : "下次优先找细节和结构。\n这类已齐会找其他新卡。", 14, 173, 212, UI_INK);
                text("收藏目标优先于工具", 14, 221, 212, UI_MUTED);
                action(ui.state.tool == ui.item - 2 ? "已带上" : "下次带上");
            } break;
        case PAGE_DESTINATIONS:
            header("选一处想象小旅行");
            for (unsigned i = 0; i < 3; ++i) {
                if (bobo_destination_unlocked(&ui.state, i))
                    snprintf(buf, sizeof(buf), "已发现 %u/4", bobo_destination_count(&ui.state, i));
                else snprintf(buf, sizeof(buf), "收集 %u/%u 张后解锁", bobo_count(&ui.state), i == 1 ? 1 : 3);
                illustrated_row(bobo_destinations[i], buf, ui.page == PAGE_BOOKS ? &pet_icon_album : &pet_icon_map, i, 72 + (int)i * 58);
            } break;
        case PAGE_DESTINATION: {
            header(bobo_destinations[ui.destination]);
            text("想象小旅行", 14, 67, 212, UI_ACCENT);
            bobo_trip_plan_t plan;
            bobo_result_t result = bobo_preview(&ui.state, ui.destination, &plan);
            snprintf(buf, sizeof(buf), "本馆观察卡 %u/4", bobo_destination_count(&ui.state, ui.destination));
            text(buf, 14, 102, 212, UI_INK);
            if (result == BOBO_OK) {
                snprintf(buf, sizeof(buf), "约 %u 分钟\n下一张：%s", plan.duration_s / 60, card_name(plan.card_id));
                text(buf, 14, 140, 212, UI_INK);
                text("先选工具，再确认出发", 14, 211, 212, UI_MUTED);
                action("准备行囊");
            } else {
                text(result_text(result), 14, 142, 212, UI_MUTED); action("返回选小馆");
            } break;
        }
        case PAGE_TOOLS:
            header("这次带什么");
            for (unsigned i = 0; i < 3; ++i)
                illustrated_row(bobo_tools[i], i == 0 ? "按顺序寻找新发现" : i == 1 ? "优先看形状和线条" : "优先看细节和结构",
                                i == 0 ? &pet_icon_backpack : i == 1 ? &pet_icon_sketchbook : &pet_icon_lens, i, 72 + (int)i * 58);
            break;
        case PAGE_PREVIEW: {
            header("出发前看一眼");
            bobo_trip_plan_t plan;
            bobo_result_t result = bobo_preview(&ui.state, ui.destination, &plan);
            if (result != BOBO_OK) { text(result_text(result), 14, 92, 212, UI_MUTED); action("返回"); break; }
            snprintf(buf, sizeof(buf), "%s · %u 分钟", bobo_destinations[ui.destination], plan.duration_s / 60);
            text(buf, 14, 65, 212, UI_INK);
            snprintf(buf, sizeof(buf), "%s：%s", plan.card_id < BOBO_CARD_COUNT ? "必得新卡" : "回访", card_name(plan.card_id));
            text(buf, 14, 94, 212, UI_ACCENT);
            snprintf(buf, sizeof(buf), "点心实际可装 %u 份\n当前 %u/9 · 计划带回 %u", plan.capacity_snacks, ui.state.snacks, plan.snacks);
            text(buf, 14, 123, 212, UI_INK);
            bobo_state_t candidate = ui.state;
            (void)bobo_start(&candidate, ui.destination);
            snprintf(buf, sizeof(buf), "精神 -%u · 整洁 -%u", ui.state.energy - candidate.energy, ui.state.tidy - candidate.tidy);
            text(buf, 14, 171, 212, UI_INK);
            text(ui.state.target_card < BOBO_CARD_COUNT && ui.state.target_card / 4 == ui.destination ? "已优先选择收藏目标" : bobo_tools[ui.state.tool], 14, 196, 212, UI_MUTED);
            text("提前回来无奖励，不退消耗", 14, 222, 212, UI_WARNING);
            action("确认出发"); break;
        }
        case PAGE_TRIP: {
            header(bobo_destinations[ui.state.trip.destination]);
            unsigned total = ui.state.trip.duration_s;
            unsigned progress = total ? (total - ui.state.trip.remaining_s) * 100 / total : 0;
            sprite(progress < 25 ? pet_pose(POSE_WAVE, false) : pet_pose(POSE_WAVE, false), 80, 61);
            text(progress < 25 ? "在路上" : progress < 75 ? "找找有趣的地方" : "整理新发现", 14, 145, 212, UI_ACCENT);
            snprintf(buf, sizeof(buf), "%u%% · 还剩 %u 分 %02u 秒", progress, ui.state.trip.remaining_s / 60, ui.state.trip.remaining_s % 60);
            text(buf, 14, 174, 212, UI_INK);
            text(progress < 25 ? "拐个弯，看看新风景。" : progress < 75 ? "发现了一点小秘密。" : "把好奇装好，准备回家。", 14, 202, 212, UI_MUTED);
            action("提前回来");
            break;
        }
        case PAGE_CANCEL:
            header("现在就回来吗"); sprite(pet_pose(POSE_WAVE, false), 80, 66);
            text("本次不获得观察卡和点心。\n出发消耗不会退还。", 14, 158, 212, UI_WARNING);
            action("确认提前回来"); break;
        case PAGE_READY: {
            snprintf(buf, sizeof(buf), "%s带回新发现", pet_name()); header(buf); sprite(pet_pose(POSE_HAPPY, false), 80, 61);
            snprintf(buf, sizeof(buf), "%s", card_name(ui.state.trip.card_id));
            text(buf, 14, 145, 212, UI_ACCENT);
            unsigned capacity = BOBO_MAX_SNACKS - ui.state.snacks;
            snprintf(buf, sizeof(buf), "当前背包 %u/9\n点心实际入包 %u 份", ui.state.snacks, min_u(capacity, ui.state.trip.snacks));
            text(buf, 14, 175, 212, UI_INK);
            text("返回会保留待领取", 14, 222, 212, UI_MUTED);
            action("收进观察册"); break;
        }
        case PAGE_RECEIPT:
            header("这次发现收好啦"); sprite(pet_pose(POSE_HAPPY, false), 80, 61);
            text(card_name(ui.state.receipt.card_id), 14, 145, 212, UI_ACCENT);
            snprintf(buf, sizeof(buf), "点心 +%u · 现有 %u/9\n精神 +%u · 心情 +%u", ui.state.receipt.snacks,
                     ui.state.snacks, ui.state.receipt.energy, ui.state.receipt.mood);
            text(buf, 14, 175, 212, UI_INK);
            text(bobo_count(&ui.state) == 12 ? "12/12 · 伙伴的好搭档" : "想象小旅行 · 一起留作回忆", 14, 223, 212, UI_MUTED);
            action(ui.state.receipt.card_id < 12 ? "看看这张观察卡" : "回到小屋"); break;
        case PAGE_GROWTH:
            snprintf(buf, sizeof(buf), "%s长大啦", pet_name()); header(buf); room_scene(); sprite(pet_pose(POSE_HAPPY, true), 60, home_pose_y());
            snprintf(buf, sizeof(buf), "Lv.%u · %s", (unsigned)bobo_growth_stage(&ui.state) + 1, bobo_growth_name(&ui.state));
            { lv_obj_t *name = text(buf, 14, 198, 212, UI_INK);
              lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0); }
            { lv_obj_t *line = pet_text(ui.screen, "一起走过的旅行，让我长大", 14, 220, 212, &ui_font_caption, UI_MUTED);
              lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0); }
            action("看看这次收获"); break;
        case PAGE_BOOKS:
            snprintf(buf, sizeof(buf), "%s的观察册", pet_name());
            header(bobo_count(&ui.state) == 12 ? "观察册已收集完成" : buf);
            for (unsigned i = 0; i < 3; ++i) {
                snprintf(buf, sizeof(buf), "%u/4 张观察卡", bobo_destination_count(&ui.state, i));
                illustrated_row(bobo_destinations[i], buf, ui.page == PAGE_BOOKS ? &pet_icon_album : &pet_icon_map, i, 72 + (int)i * 58);
            } break;
        case PAGE_CARDS:
            header(bobo_destinations[ui.book]);
            for (unsigned i = 0; i < 4; ++i) {
                unsigned card = ui.book * 4 + i;
                snprintf(buf, sizeof(buf), "%s  %s", owns(card) ? "已得" : ui.state.target_card == card ? "目标" : "未得", card_name(card));
                row(buf, NULL, i, 72 + (int)i * 42, 36);
            } break;
        case PAGE_CARD:
            header(card_name(ui.card));
            panel(ui.screen, 14, 63, 212, 178, false);
            shape(ui.screen, 26, 108, 188, 1, PET_LINE, 0);
            pet_text(ui.screen, "博博的想象观察卡", 26, 218, 188, &ui_font_caption, UI_MUTED);
            snprintf(buf, sizeof(buf), "来自%s · 想象旅行", bobo_destinations[ui.card / 4]);
            pet_text(ui.screen, buf, 26, 79, 188, &ui_font_caption, UI_MUTED);
            if (owns(ui.card)) {
                text(ui.card_side ? "博博的回忆 2/2" : "观察发现 1/2", 26, 121, 188, UI_ACCENT);
                card_paragraph(ui.card_side ? bobo_cards[ui.card].memory : bobo_cards[ui.card].observation);
                action("下一页");
            } else {
                text("还没发现这张观察卡", 26, 120, 188, UI_INK);
                pet_text(ui.screen, "设为目标，去对应小馆寻找。\n目标优先于工具偏好。", 26, 153, 188, &ui_font_caption, UI_MUTED);
                if (!bobo_destination_unlocked(&ui.state, ui.card / 4)) {
                    snprintf(buf, sizeof(buf), "需先收集 %u 张解锁小馆", ui.card / 4 == 1 ? 1 : 3);
                    pet_text(ui.screen, buf, 26, 196, 188, &ui_font_caption, UI_WARNING);
                }
                action(ui.state.target_card == ui.card ? "取消目标" : "设为当前目标");
            } break;
        case PAGE_NOTICE:
            header(ui.notice_title);
            if (ui.notice_pose) { sprite(ui.notice_pose, 80, 62); text(ui.notice_body, 14, 153, 212, UI_INK); }
            else text(ui.notice_body, 14, 83, 212, UI_INK);
            action(ui.save_blocked ? "重试存档" : "知道了"); break;
    }
    if (list_page()) row("返回", NULL, choice_count() - 1, 250, 35);
    if (ui.page != PAGE_HOME) {
        lv_obj_t *hint = pet_text(ui.screen, ui.pending ?
            (ui.pending_command == UI_BOBO_WISH_START || ui.pending_command == UI_BOBO_WISH_CANCEL ? "礼盒准备中，请稍候" : "正在保存，请稍候") : ui.page == PAGE_FRIENDS ? "只计开机 · 关机休眠暂停" : "上下选择 · 确定", 12, 296, 216,
                                 &ui_font_caption, UI_MUTED);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void ui_bobo_open(const bobo_state_t *state, bobo_load_result_t load_status,
                  bool clock_trusted, ui_bobo_command_cb_t command,
                  ui_bobo_exit_cb_t exit, void *user) {
    if (!state || !bobo_valid(state)) return;
    pixel_sprite_init();
    if (ui.screen) ui_bobo_close();
    memset(&ui, 0, sizeof(ui)); ui.state = *state; ui.command = command; ui.exit = exit; ui.user = user;
    ui.animation_random = lv_tick_get() ^ state->trip_sequence ^ ((uint32_t)state->collected << 16) ^ 0xB0B01234u;
    if (!ui.animation_random) ui.animation_random = 1;
    ui.previous = lv_screen_active(); ui.screen = ui_theme_screen();
    lv_screen_load(ui_theme_screen_obj(ui.screen));
    if (!bobo_art_ready()) { go(PAGE_ART_UNAVAILABLE, 0); return; }
    ui.save_blocked = load_status == BOBO_SAVE_UNAVAILABLE;
    (void)clock_trusted;
    char message[384] = "";
    if (ui.save_blocked) snprintf(message, sizeof(message), "存档暂不可用。\n请重试后再操作。\n");
    else if (load_status == BOBO_SAVE_RECOVERED) snprintf(message, sizeof(message), "已恢复上一次有效进度。\n");
    else if (load_status == BOBO_SAVE_RESET) snprintf(message, sizeof(message), "伙伴存档未能恢复。\n已准备新的小旅行。\n");
    if (message[0]) notice("和伙伴一起出发", message, PAGE_HOME, NULL);
    else go(PAGE_HOME, 0);
}
void ui_bobo_close(void) {
    if (!ui.screen) return;
    if ((ui.wish_running || (ui.pending && ui.pending_command == UI_BOBO_WISH_START)) && ui.command)
        (void)ui.command(UI_BOBO_WISH_CANCEL, 0, ui.user);
    if (ui.animation) { lv_timer_delete(ui.animation); ui.animation = NULL; }
    lv_obj_t *screen = ui.screen, *previous = ui.previous;
    ui_bobo_exit_cb_t exit = ui.exit; void *user = ui.user;
    ui.screen = NULL; ui.pending = false;
    if (previous && lv_obj_is_valid(previous)) lv_screen_load(previous);
    lv_obj_t *to_delete = ui_theme_screen_obj(screen);
    lv_obj_delete(to_delete ? to_delete : screen);
    memset(&ui, 0, sizeof(ui));
    if (exit) exit(user);
}
bool ui_bobo_is_active(void) { return ui.screen != NULL; }
void ui_bobo_suspend(bool suspended) {
    if (!ui.screen || ui.suspended == suspended) return;
    ui.suspended = suspended;
    if (suspended) {
        if (ui.animation) lv_timer_pause(ui.animation);
        ui.beat = BEAT_BREATHE; ui.frame = 0; ui.wait = 12;
        if (ui.page == PAGE_HOME) animation_apply();
    } else if ((ui.page == PAGE_HOME || ui.page == PAGE_GIFT) && ui.animation) {
        lv_timer_reset(ui.animation); lv_timer_resume(ui.animation);
    }
}

static void back(void) {
    switch (ui.page) {
        case PAGE_ART_UNAVAILABLE: ui_bobo_close(); break;
        case PAGE_GIFT:
            if (ui.state.pending_reveal) ui_bobo_close();
            else go(PAGE_HOME, 4);
            break;
        case PAGE_WISH_LISTEN:
            send_command(UI_BOBO_WISH_CANCEL, 0);
            if (ui.pending) ui.wish_running = false;
            break;
        case PAGE_WISH_INVITE: case PAGE_WISH_ERROR:
            if (ui.state.health == BOBO_DEAD) go(PAGE_FAREWELL, 1);
            else if (ui.state.owned_pets) go(PAGE_HOME, 4);
            else ui_bobo_close();
            break;
        case PAGE_FRIENDS: go(PAGE_HOME, ui.home_selection); break;
        case PAGE_FAREWELL: ui_bobo_close(); break;
        case PAGE_HOME: ui_bobo_close(); break;
        case PAGE_CARE: case PAGE_BAG: case PAGE_DESTINATIONS: case PAGE_TRIP:
        case PAGE_READY: case PAGE_RECEIPT: case PAGE_GROWTH: case PAGE_BOOKS: go(PAGE_HOME, ui.home_selection); break;
        case PAGE_CARE_DETAIL: go(PAGE_CARE, ui.care); break;
        case PAGE_ITEM: go(PAGE_BAG, ui.item); break;
        case PAGE_DESTINATION: go(PAGE_DESTINATIONS, ui.destination); break;
        case PAGE_TOOLS: go(PAGE_DESTINATION, 0); break;
        case PAGE_PREVIEW: go(PAGE_TOOLS, ui.state.tool); break;
        case PAGE_CANCEL: go(ui.state.phase == BOBO_READY ? PAGE_READY : PAGE_TRIP, 0); break;
        case PAGE_CARDS: go(PAGE_BOOKS, ui.book); break;
        case PAGE_CARD: go(PAGE_CARDS, ui.card % 4); break;
        case PAGE_NOTICE:
            if (ui.save_blocked) ui_bobo_close();
            else go(ui.notice_return, ui.notice_return == PAGE_HOME ? ui.home_selection : 0);
            break;
    }
}
void ui_bobo_key(ui_play_key_t key) {
    if (!ui.screen || ui.pending || ui.suspended) return;
    if (key == UI_PLAY_BACK) { back(); return; }
    if (key == UI_PLAY_UP || key == UI_PLAY_DOWN) {
        unsigned count = choice_count();
        ui.selection = (ui.selection + (key == UI_PLAY_DOWN ? 1 : count - 1)) % count;
        if (ui.page == PAGE_HOME) ui.home_selection = ui.selection;
        render(); return;
    }
    if (key != UI_PLAY_OK) return;
    if (ui.selection == choice_count() - 1) { back(); return; }
    switch (ui.page) {
        case PAGE_HOME:
            ui.home_selection = ui.selection;
            go(ui.selection == 0 ? PAGE_CARE : ui.selection == 1 ? PAGE_BAG : ui.selection == 3 ? PAGE_BOOKS : ui.selection == 4 ? PAGE_FRIENDS :
               ui.state.phase == BOBO_READY ? PAGE_READY : ui.state.phase == BOBO_TRAVELLING ? PAGE_TRIP : PAGE_DESTINATIONS, 0); break;
        case PAGE_FRIENDS: break;
        case PAGE_FAREWELL: go(PAGE_WISH_INVITE, 0); break;
        case PAGE_WISH_INVITE: case PAGE_WISH_ERROR: send_command(UI_BOBO_WISH_START, 0); break;
        case PAGE_GIFT:
            if (ui.gift_frame >= 20) {
                if (ui.state.pending_reveal) send_command(UI_BOBO_REVEAL_ACK, 0);
                else go(PAGE_HOME, 4);
            }
            break;
        case PAGE_WISH_LISTEN: case PAGE_ART_UNAVAILABLE: break;
        case PAGE_CARE: ui.care = ui.selection; go(PAGE_CARE_DETAIL, 0); break;
        case PAGE_CARE_DETAIL: send_command(UI_BOBO_CARE, ui.care); break;
        case PAGE_BAG: ui.item = ui.selection; go(PAGE_ITEM, 0); break;
        case PAGE_ITEM:
            send_command(ui.item < 3 ? UI_BOBO_CARE : UI_BOBO_EQUIP,
                         ui.item < 2 ? ui.item : ui.item == 2 ? BOBO_SNACK : ui.item - 2); break;
        case PAGE_DESTINATIONS: ui.destination = ui.selection; go(PAGE_DESTINATION, 0); break;
        case PAGE_DESTINATION:
            if (bobo_destination_unlocked(&ui.state, ui.destination)) go(PAGE_TOOLS, ui.state.tool);
            else go(PAGE_DESTINATIONS, ui.destination);
            break;
        case PAGE_TOOLS:
            if (ui.state.tool == ui.selection) go(PAGE_PREVIEW, 0);
            else send_command(UI_BOBO_EQUIP, ui.selection);
            break;
        case PAGE_PREVIEW: send_command(UI_BOBO_START, ui.destination); break;
        case PAGE_TRIP: go(PAGE_CANCEL, 1); break;
        case PAGE_CANCEL:
            send_command(UI_BOBO_CANCEL, 0); break;
        case PAGE_READY: send_command(UI_BOBO_CLAIM, 0); break;
        case PAGE_RECEIPT:
            if (ui.state.receipt.card_id < 12) {
                ui.card = ui.state.receipt.card_id; ui.book = ui.card / 4; ui.card_side = 0; go(PAGE_CARD, 0);
            } else go(PAGE_HOME, ui.home_selection); break;
        case PAGE_GROWTH: go(PAGE_RECEIPT, 0); break;
        case PAGE_BOOKS: ui.book = ui.selection; go(PAGE_CARDS, 0); break;
        case PAGE_CARDS: ui.card = ui.book * 4 + ui.selection; ui.card_side = 0; go(PAGE_CARD, 0); break;
        case PAGE_CARD:
            if (!owns(ui.card)) send_command(UI_BOBO_TARGET, ui.state.target_card == ui.card ? BOBO_NO_CARD : ui.card);
            else { ui.card_side ^= 1; render(); } break;
        case PAGE_NOTICE:
            if (ui.save_blocked) send_command(UI_BOBO_RETRY_SAVE, 0);
            else go(ui.notice_return, ui.notice_return == PAGE_HOME ? ui.home_selection : 0);
            break;
    }
}
void ui_bobo_update(const bobo_state_t *state, bobo_result_t result,
                    bool save_ok, bool acknowledgement, bool clock_trusted) {
    if (!ui.screen || !state || !bobo_valid(state)) return;
    bool changed = memcmp(&ui.state, state, sizeof(*state)) != 0;
    ui.state = *state;
    (void)clock_trusted;
    if (!acknowledgement) {
        if (!save_ok && !ui.save_blocked && !ui.pending) {
            ui.save_blocked = true;
            notice("这次还没保存好", "进度还没确认好。\n请重试读取存档。", ui.page, NULL);
            return;
        }
        if (!changed) return;
        if (!ui.pending && state->health == BOBO_DEAD && ui.page != PAGE_FAREWELL &&
            ui.page != PAGE_WISH_INVITE && ui.page != PAGE_WISH_LISTEN && ui.page != PAGE_WISH_ERROR)
            go(PAGE_FAREWELL, 1);
        else if (!ui.pending && (ui.page == PAGE_TRIP || ui.page == PAGE_CANCEL) && state->phase == BOBO_READY)
            go(PAGE_READY, 0);
        else render();
        return;
    }
    if (!ui.pending) { render(); return; }
    ui.pending = false;
    if (!save_ok) {
        ui.save_blocked = true;
        page_t return_page = ui.pending_command == UI_BOBO_RETRY_SAVE ? ui.notice_return : ui.command_page;
        notice("这次还没保存好", "进度还没确认好。\n请重试读取存档。", return_page, NULL);
        return;
    }
    if (result != BOBO_OK) {
        notice("伙伴的小提示", result_text(result),
               ui.command_page == PAGE_NOTICE ? ui.notice_return : ui.command_page, NULL); return;
    }
    ui.save_blocked = false;
    switch (ui.pending_command) {
        case UI_BOBO_WISH_START:
            ui.wish_running = true;
            ui.wish = (pet_wish_view_t){.phase = PET_WISH_CALIBRATING};
            go(PAGE_WISH_LISTEN, 0); break;
        case UI_BOBO_WISH_CANCEL:
            ui.wish_running = false;
            if (state->pending_reveal) ui_bobo_close();
            else if (state->health == BOBO_DEAD) go(PAGE_FAREWELL, 1);
            else if (state->owned_pets) go(PAGE_HOME, 4);
            else ui_bobo_close();
            break;
        case UI_BOBO_SELECT_PET: ui.introduced = false; go(PAGE_HOME, 4); break;
        case UI_BOBO_REVEAL_ACK: ui.introduced = true; go(PAGE_HOME, 4); break;
        case UI_BOBO_RETRY_SAVE: {
            // An uncertain write may have committed. Restore a page matching
            // the reloaded state, never re-offer an already completed action.
            page_t return_page = state->health == BOBO_DEAD ? PAGE_FAREWELL : state->pending_reveal ? PAGE_GIFT : state->phase == BOBO_TRAVELLING ? PAGE_TRIP :
                                 state->phase == BOBO_READY ? PAGE_READY : PAGE_HOME;
            if (state->health != BOBO_DEAD && !state->pending_reveal && state->phase == BOBO_HOME && state->receipt.sequence != ui.before.receipt.sequence)
                return_page = bobo_growth_stage(state) > bobo_growth_stage(&ui.before) ? PAGE_GROWTH : PAGE_RECEIPT;
            notice("存档可以用了", "已恢复当前进度。\n接着陪伙伴玩吧。", return_page, NULL); break;
        }
        case UI_BOBO_START: go(PAGE_TRIP, 0); break;
        case UI_BOBO_CANCEL: notice("伙伴回来啦", "本次没有领取奖励。\n歇一会儿，再一起出发。", PAGE_HOME, pet_pose(POSE_WAVE, false)); break;
        case UI_BOBO_CLAIM:
            go(bobo_growth_stage(state) > bobo_growth_stage(&ui.before) ? PAGE_GROWTH : PAGE_RECEIPT, 0); break;
        case UI_BOBO_EQUIP:
            if (ui.command_page == PAGE_TOOLS) go(PAGE_PREVIEW, 0);
            else notice("行囊准备好了", "工具已带上，下次旅行生效。\n收藏目标会优先选择。", PAGE_ITEM, pet_pose(POSE_WAVE, false));
            break;
        case UI_BOBO_TARGET:
            notice(ui.state.target_card == BOBO_NO_CARD ? "目标已取消" : "记住这个发现啦",
                   ui.state.target_card == BOBO_NO_CARD ? "下次按工具偏好找新卡。" : "去对应小馆时优先寻找。\n工具偏好不会覆盖目标。", PAGE_CARD, NULL); break;
        case UI_BOBO_CARE: {
            char message[192];
            snprintf(message, sizeof(message), "精神 +%u · 心情 +%u\n整洁 +%u%s",
                     state->energy >= ui.before.energy ? state->energy - ui.before.energy : 0,
                     state->mood >= ui.before.mood ? state->mood - ui.before.mood : 0,
                     state->tidy >= ui.before.tidy ? state->tidy - ui.before.tidy : 0,
                     ui.pending_argument == BOBO_SNACK ? " · 点心 -1" : "");
            notice("伙伴舒服多啦", message, ui.command_page,
                   ui.pending_argument == BOBO_SNACK || ui.pending_argument == BOBO_WATER ? pet_pose(POSE_IDLE, false) : pet_pose(POSE_HAPPY, false)); break;
        }
    }
}

void ui_bobo_wish_update(const pet_wish_view_t *view, bool save_ok) {
    if (!ui.screen || !view || !ui.wish_running || ui.page != PAGE_WISH_LISTEN) return;
    ui.wish = *view;
    if (view->phase == PET_WISH_ACCEPTED) {
        ui.wish_running = false;
        if (!save_ok) {
            ui.save_blocked = true;
            notice("礼物还没保存好", "这次相遇还没有确认。\n请先重试读取存档。", PAGE_HOME, NULL);
        } else if (ui.state.pending_reveal) {
            go(PAGE_GIFT, 0);
        } else {
            ui.wish.phase = PET_WISH_MIC_ERROR; go(PAGE_WISH_ERROR, 0);
        }
    } else if (view->phase == PET_WISH_TIMED_OUT || view->phase == PET_WISH_MIC_ERROR) {
        ui.wish_running = false; go(PAGE_WISH_ERROR, 0);
    } else if (view->phase == PET_WISH_CANCELLED) {
        ui.wish_running = false;
        if (ui.state.health == BOBO_DEAD) go(PAGE_FAREWELL, 1);
            else if (ui.state.owned_pets) go(PAGE_HOME, 4); else ui_bobo_close();
    } else if (view->phase == PET_WISH_LISTENING || view->phase == PET_WISH_CALIBRATING) {
        unsigned progress = min_u(view->progress, 1000);
        const lv_image_dsc_t *box = gift_pose(progress * 4 / 1000);
        if (ui.gift && lv_image_get_src(ui.gift) != box) lv_image_set_src(ui.gift, box);
        if (ui.wish_ribbon) {
            lv_obj_set_width(ui.wish_ribbon, 188 * progress / 1000);
            if (progress) lv_obj_remove_flag(ui.wish_ribbon, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(ui.wish_ribbon, LV_OBJ_FLAG_HIDDEN);
        }
        label_change(ui.wish_caption,
            view->phase == PET_WISH_CALIBRATING ? "正在听周围的声音" : "慢慢说，丝带会轻轻松开");
        label_change(ui.wish_prompt,
            view->phase == PET_WISH_CALIBRATING ? "先安静片刻" : "现在说出你的愿望");
    }
}
