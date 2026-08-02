/*
 * game_logic.c — see game_logic.h. Pure C, no OS dependencies, so the
 * exact same logic runs inside the Win32 window (win_game.c) and in the
 * Linux headless simulator (game_sim.c).
 */
#include "game_logic.h"

#include <stdlib.h>
#include <string.h>

static void spawn_aliens(game_t *g) {
    game_state_t *s = &g->state;
    int n = 0;
    for (int i = 0; i < MAX_ALIENS; i++) g->aliens[i].alive = 0;
    /* 6 columns x 4 rows */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 6; c++) {
            alien_t *a = &g->aliens[n++];
            a->x = 60.0f + c * 90.0f;
            a->y = 50.0f + r * 28.0f;
            a->alive = 1;
        }
    }
    s->alien_count = n;
}

void game_init(game_t *g) {
    memset(g, 0, sizeof *g);
    game_state_t *s = &g->state;
    s->magic    = (int32_t)GAME_STATE_MAGIC;
    strcpy(s->marker, GAME_MARKER);
    s->score     = 0;
    s->lives     = 3;
    s->level     = 1;
    s->game_over = 0;
    s->paused    = 0;
    s->player_x  = (float)((GAME_W - PLAYER_W) / 2);
    s->player_y  = 440.0f;
    s->speed     = 1.0f;
    s->bullet_count = 0;
    s->lua_msg_mode = 0;
    s->lua_msg[0]   = '\0';
    s->target_x  = 0.0f;
    s->target_y  = 0.0f;
    g->fire_cooldown = 0;
    g->alien_tick    = 0;
    g->bomb_timer    = 30;
    g->hit_flash     = 0;
    spawn_aliens(g);
}

static int aabb(float ax, float ay, float aw, float ah,
                float bx, float by, float bw, float bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

void game_update(game_t *g, const game_input_t *in) {
    game_state_t *s = &g->state;

    if (in->toggle_pause) s->paused = !s->paused;
    if (s->paused) return;
    if (in->reset && s->game_over) { game_init(g); return; }
    if (s->game_over) return;

    /* ---- player ---- */
    if (in->left)  s->player_x -= 7.0f;
    if (in->right) s->player_x += 7.0f;
    if (s->player_x < 8.0f) s->player_x = 8.0f;
    if (s->player_x > GAME_W - PLAYER_W - 8.0f)
        s->player_x = (float)(GAME_W - PLAYER_W - 8);

    /* ---- firing ---- */
    if (g->fire_cooldown > 0) g->fire_cooldown--;
    if (in->fire && g->fire_cooldown == 0) {
        for (int i = 0; i < MAX_BULLETS; i++) {
            if (!g->bullets[i].alive) {
                g->bullets[i].alive = 1;
                g->bullets[i].x = s->player_x + PLAYER_W / 2.0f - 2.0f;
                g->bullets[i].y = s->player_y - 8.0f;
                s->bullet_count++;
                g->fire_cooldown = 10;
                break;
            }
        }
    }

    /* ---- bullets ---- */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!g->bullets[i].alive) continue;
        g->bullets[i].y -= 12.0f;
        if (g->bullets[i].y < -10.0f) {
            g->bullets[i].alive = 0;
            s->bullet_count--;
            continue;
        }
        for (int j = 0; j < MAX_ALIENS; j++) {
            if (!g->aliens[j].alive) continue;
            alien_t *a = &g->aliens[j];
            if (aabb(g->bullets[i].x, g->bullets[i].y, 4.0f, 8.0f,
                     a->x, a->y, ALIEN_W, ALIEN_H)) {
                a->alive = 0;
                s->alien_count--;
                s->score += 10;
                g->bullets[i].alive = 0;
                s->bullet_count--;
                break;
            }
        }
    }

    /* ---- aliens descend ---- */
    if (--g->alien_tick <= 0) {
        int interval = 26 - s->level * 2;
        if (interval < 6) interval = 6;
        g->alien_tick = interval;
        for (int i = 0; i < MAX_ALIENS; i++) {
            if (!g->aliens[i].alive) continue;
            g->aliens[i].y += 8.0f * s->speed;
            if (g->aliens[i].y + ALIEN_H >= s->player_y)
                s->game_over = 1;
        }
    }

    /* ---- alien bombs ---- */
    if (--g->bomb_timer <= 0) {
        g->bomb_timer = 45;
        int pool[MAX_ALIENS], n = 0;
        for (int i = 0; i < MAX_ALIENS; i++)
            if (g->aliens[i].alive) pool[n++] = i;
        if (n > 0) {
            int pick = pool[rand() % n];
            for (int i = 0; i < MAX_BOMBS; i++) {
                if (!g->bombs[i].alive) {
                    g->bombs[i].alive = 1;
                    g->bombs[i].x = g->aliens[pick].x + ALIEN_W / 2.0f - 2.0f;
                    g->bombs[i].y = g->aliens[pick].y + ALIEN_H;
                    break;
                }
            }
        }
    }
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!g->bombs[i].alive) continue;
        g->bombs[i].y += 5.0f;
        if (g->bombs[i].y > GAME_H) { g->bombs[i].alive = 0; continue; }
        if (aabb(g->bombs[i].x, g->bombs[i].y, 4.0f, 10.0f,
                 s->player_x, s->player_y, PLAYER_W, PLAYER_H)) {
            g->bombs[i].alive = 0;
            if (--s->lives <= 0) {
                s->lives = 0;
                s->game_over = 1;
            } else {
                g->hit_flash = 25;
            }
        }
    }
    if (g->hit_flash > 0) g->hit_flash--;

    /* ---- level up ---- */
    if (s->alien_count == 0 && !s->game_over) {
        s->level++;
        s->speed *= 1.12f;
        if (s->lives < 9) s->lives++;
        spawn_aliens(g);
    }

    /* ---- nearest alive alien (feeds target_x/target_y for autopilot) ---- */
    float best = 1e9f;
    int have = 0;
    for (int i = 0; i < MAX_ALIENS; i++) {
        if (!g->aliens[i].alive) continue;
        float cx = g->aliens[i].x + ALIEN_W / 2.0f;
        float cy = g->aliens[i].y + ALIEN_H / 2.0f;
        float dx = cx - s->player_x, dy = cy - s->player_y;
        float d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            s->target_x = cx;
            s->target_y = cy;
            have = 1;
        }
    }
    if (!have) { s->target_x = 0.0f; s->target_y = 0.0f; }
}
