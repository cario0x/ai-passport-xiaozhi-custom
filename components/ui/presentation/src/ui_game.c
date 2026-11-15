#include "presentation/ui_game.h"
#include "presentation/ui_theme.h"
#include "presentation/bobo_art.h"
#include "presentation/pixel_sprite.h"
#include "services/dodge.h"
#include <stdatomic.h>
#include <stdio.h>
#include <limits.h>

static struct {
    lv_obj_t *screen, *previous, *player, *obstacles[DODGE_OBSTACLES], *score, *footer, *overlay;
    lv_timer_t *timer;
    dodge_t game;
    dodge_mode_t drawn_mode;
    unsigned drawn_selection;
    unsigned drawn_score;
    unsigned drawn_best;
    unsigned drawn_player_frame;
    int drawn_player_lane;
    uint8_t drawn_art[DODGE_OBSTACLES];
    bool drawn_running;
    uint32_t open_tick;
    ui_game_exit_cb_t exit;
    void *user;
} G;
static atomic_bool active;
// Input producers never touch LVGL or game state. Exchange prevents losing an
// event when the button and LVGL tasks meet in the same scheduler interval.
static atomic_int pending_move;
static atomic_bool pending_ok, pending_back;
static ui_game_result_cb_t result_cb;
static void *result_user;
static unsigned saved_best;
static const lv_image_dsc_t *const art[] = {
    &dodge_art_luggage, &dodge_art_robot, &dodge_art_plant, &dodge_art_books
};

void ui_game_set_result_cb(ui_game_result_cb_t cb, void *user) { result_cb = cb; result_user = user; }
void ui_game_set_best(int best) { saved_best = best > 0 ? (unsigned)best : 0; }
bool ui_game_is_active(void) { return atomic_load(&active); }
static int lane_y(unsigned lane) { return 76 + lane * 70; }
static void close_game(void) {
    atomic_store(&active, false);
    lv_timer_delete(G.timer); G.timer = NULL;
    lv_screen_load(G.previous);
    lv_obj_t *gone = ui_theme_screen_obj(G.screen);
    lv_obj_delete(gone ? gone : G.screen); G.screen = NULL;
    if (G.exit) G.exit(G.user);
}
static void draw_overlay(void) {
    if (G.overlay) { lv_obj_delete(G.overlay); G.overlay = NULL; }
    G.drawn_mode = G.game.mode; G.drawn_selection = G.game.selection;
    if (G.game.mode == DODGE_RUNNING) return;
    G.overlay = ui_theme_panel(G.screen, 14, 69, 212, 214, false);
    const char *title = G.game.mode == DODGE_PAUSED ? "休息一下" :
                        G.game.mode == DODGE_OVER ? "下次，再走远一点" : "博博的小小冒险";
    ui_theme_text(G.overlay, title, 12, 12, 186, &ui_font_title, UI_ACCENT);
    char detail[64];
    if (G.game.mode == DODGE_OVER) snprintf(detail, sizeof(detail), "这次躲过了 %u 个障碍", G.game.score);
    else snprintf(detail, sizeof(detail), "%s", G.game.mode == DODGE_PAUSED ?
                  "画面已暂停，慢慢选" : "上 / 下换道，确定暂停");
    ui_theme_text(G.overlay, detail, 12, 45, 187, &ui_font_body, UI_MUTED);
    unsigned count = G.game.mode == DODGE_PAUSED ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
        const char *label = i == count - 1 ? "返回玩法" :
            G.game.mode == DODGE_PAUSED ? (i == 0 ? "继续冒险" : "重新开始") :
            G.game.mode == DODGE_OVER ? "再来一次" : "开始冒险";
        bool selected = i == G.game.selection;
        lv_obj_t *row = ui_theme_panel(G.overlay, 10, 79 + i * 42, 190, 34, selected);
        ui_theme_text(row, label, 12, 6, 166, &ui_font_body, selected ? UI_ACCENT : UI_INK);
    }
}
static void render(void) {
    if (G.drawn_score != G.game.score || G.drawn_best != G.game.best) {
        lv_label_set_text_fmt(G.score, "本局 %u       最好 %u", G.game.score, G.game.best);
        G.drawn_score = G.game.score;
        G.drawn_best = G.game.best;
    }
    bool running = G.game.mode == DODGE_RUNNING;
    if (G.drawn_running != running) {
        lv_label_set_text(G.footer, running ? "上下换道   确定暂停 / 返回" : "上下选择   确定执行");
        G.drawn_running = running;
    }
    unsigned player_frame = (G.game.steps / 5) % 2;
    if (G.drawn_player_frame != player_frame)
        lv_image_set_src(G.player, player_frame ? &bobo_art_walk_left : &bobo_art_walk_right);
    if (G.drawn_player_frame != player_frame || G.drawn_player_lane != (int)G.game.lane) {
        lv_obj_set_pos(G.player, DODGE_PLAYER_X, lane_y(G.game.lane) + 14);
        G.drawn_player_frame = player_frame;
        G.drawn_player_lane = (int)G.game.lane;
    }
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
        const dodge_obstacle_t *o = &G.game.obstacles[i];
        if (o->active) {
            if (G.drawn_art[i] != o->art) {
                lv_image_set_src(G.obstacles[i], art[o->art]);
                G.drawn_art[i] = o->art;
            }
            lv_obj_set_pos(G.obstacles[i], o->x, lane_y(o->lane) + 2);
            lv_obj_remove_flag(G.obstacles[i], LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(G.obstacles[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (G.drawn_mode != G.game.mode || G.drawn_selection != G.game.selection) draw_overlay();
}
static void tick(lv_timer_t *timer) {
    (void)timer;
    if (atomic_exchange(&pending_back, false)) { close_game(); return; }
    int move = atomic_exchange(&pending_move, 0);
    bool ok = atomic_exchange(&pending_ok, false);
    if (move) dodge_key(&G.game, move > 0 ? DODGE_DOWN : DODGE_UP);
    if (ok) dodge_key(&G.game, DODGE_OK);
    if (G.game.mode == DODGE_EXIT) { close_game(); return; }
    dodge_mode_t before = G.game.mode;
    dodge_step(&G.game);
    if (before == DODGE_RUNNING && G.game.mode == DODGE_OVER && result_cb)
        result_cb((int)G.game.score, result_user);
    render();
}
void ui_game_open(ui_game_exit_cb_t exit, void *user) {
    if (atomic_load(&active)) return;
    pixel_sprite_init();
    G.exit = exit; G.user = user; G.previous = lv_screen_active();
    G.open_tick = lv_tick_get(); G.overlay = NULL;
    dodge_init(&G.game, saved_best, G.open_tick ^ 0xa5a5u);
    atomic_store(&pending_move, 0); atomic_store(&pending_ok, false); atomic_store(&pending_back, false);
    G.screen = ui_theme_screen();
    ui_theme_text(G.screen, "障碍躲避", 14, 12, 212, &ui_font_title, UI_INK);
    G.score = ui_theme_text(G.screen, "", 14, 43, 212, &ui_font_body, UI_MUTED);
    for (unsigned lane = 0; lane < DODGE_LANES; ++lane)
        ui_theme_panel(G.screen, 6, lane_y(lane), 228, 66, false);
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
        G.obstacles[i] = lv_image_create(G.screen);
        lv_image_set_src(G.obstacles[i], art[i % 4]);
    }
    G.player = lv_image_create(G.screen);
    G.footer = ui_theme_text(G.screen, "", 10, 298, 220, &ui_font_body, UI_MUTED);
    G.drawn_mode = DODGE_EXIT; G.drawn_selection = 99;
    G.drawn_score = UINT_MAX; G.drawn_best = UINT_MAX;
    G.drawn_player_frame = UINT_MAX; G.drawn_player_lane = -1;
    G.drawn_running = true;
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) G.drawn_art[i] = UINT8_MAX;
    render(); lv_screen_load(ui_theme_screen_obj(G.screen));
    lv_obj_set_style_clip_corner(G.screen, false, 0); lv_obj_set_style_radius(G.screen, 0, 0);
    atomic_store(&active, true);
    G.timer = lv_timer_create(tick, DODGE_STEP_MS, NULL);
}
void ui_game_key(ui_game_key_t key) {
    if (!atomic_load(&active)) return;
    switch (key) {
        case UI_GAME_KEY_UP: atomic_fetch_sub(&pending_move, 1); break;
        case UI_GAME_KEY_DOWN: atomic_fetch_add(&pending_move, 1); break;
        case UI_GAME_KEY_ENTER: atomic_store(&pending_ok, true); break;
        case UI_GAME_KEY_EXIT: atomic_store(&pending_back, true); break;
    }
}
void ui_game_pause_for_sleep(void) {
    if (!atomic_load(&active)) return;
    atomic_store(&pending_move, 0); atomic_store(&pending_ok, false); atomic_store(&pending_back, false);
    if (G.game.mode == DODGE_RUNNING) dodge_key(&G.game, DODGE_OK);
    render();
}
