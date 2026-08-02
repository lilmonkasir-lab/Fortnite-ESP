/*
 * game_sim.c — Linux headless simulator for the Lua Injector Test Game.
 *
 * Runs the exact same game_logic.c that the Windows game uses, prints the
 * live state struct's address + offsets (so you can compare with GAME.md),
 * then plays a scripted autopilot match to prove the logic works.
 *
 * Build/run:
 *     make build/game-sim-test
 *     ./build/game-sim-test
 */
#include <stdio.h>
#include <string.h>

#include "game_logic.h"

int main(void) {
    game_t g;
    game_init(&g);
    game_state_t *s = &g.state;

    printf("Lua Injector Test Game — headless simulation\n");
    printf("state struct @ %p (marker @ %p)\n",
           (void *)s, (void *)s->marker);
    printf("sizeof(game_state_t) = 0x%zX\n", sizeof(game_state_t));
    printf("offsets: magic=0x%02zX marker=0x%02zX score=0x%02zX "
           "lives=0x%02zX level=0x%02zX\n",
           offsetof(game_state_t, magic), offsetof(game_state_t, marker),
           offsetof(game_state_t, score), offsetof(game_state_t, lives),
           offsetof(game_state_t, level));
    printf("offsets: game_over=0x%02zX paused=0x%02zX player_x=0x%02zX "
           "player_y=0x%02zX speed=0x%02zX\n",
           offsetof(game_state_t, game_over), offsetof(game_state_t, paused),
           offsetof(game_state_t, player_x), offsetof(game_state_t, player_y),
           offsetof(game_state_t, speed));
    printf("offsets: alien_count=0x%02zX bullet_count=0x%02zX "
           "lua_msg_mode=0x%02zX lua_msg=0x%02zX target_x=0x%02zX "
           "target_y=0x%02zX\n",
           offsetof(game_state_t, alien_count),
           offsetof(game_state_t, bullet_count),
           offsetof(game_state_t, lua_msg_mode),
           offsetof(game_state_t, lua_msg),
           offsetof(game_state_t, target_x),
           offsetof(game_state_t, target_y));

    /* sanity: magic + marker roundtrip */
    if (s->magic != (int32_t)GAME_STATE_MAGIC) {
        printf("FAIL: magic mismatch 0x%08X\n", (unsigned)s->magic);
        return 1;
    }
    if (strcmp(s->marker, GAME_MARKER) != 0) {
        printf("FAIL: marker mismatch \"%s\"\n", s->marker);
        return 1;
    }
    if (s->alien_count != 24) {
        printf("FAIL: expected 24 aliens, got %d\n", s->alien_count);
        return 1;
    }
    printf("init ok: 24 aliens, lives=%d, level=%d\n", s->lives, s->level);

    /* scripted autopilot: aim the ship so its bullet (which spawns at
     * player_x + 18) lines up with the nearest alien's center, then
     * hold fire */
    int shots_hit = 0, last_score = 0;
    for (int frame = 0; frame < 2000 && !s->game_over; frame++) {
        game_input_t in;
        memset(&in, 0, sizeof in);
        in.fire = 1;
        float aim = s->target_x - 18.0f;   /* bullet spawns at player_x+18 */
        if (aim > s->player_x + 2.0f) in.right = 1;
        else if (aim < s->player_x - 2.0f) in.left = 1;
        game_update(&g, &in);
        if (s->score > last_score) { last_score = s->score; shots_hit++; }
    }

    printf("autopilot after 2000 frames: score=%d lives=%d level=%d "
           "aliens=%d game_over=%d\n",
           s->score, s->lives, s->level, s->alien_count, s->game_over);
    if (s->score <= 0) {
        printf("FAIL: autopilot never scored\n");
        return 1;
    }
    printf("sim passed: %d hits, %d level(s) cleared\n", shots_hit, s->level);
    return 0;
}
