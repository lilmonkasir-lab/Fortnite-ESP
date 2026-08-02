/*
 * game_logic.h — platform-independent logic for the Lua Injector Test Game.
 *
 * The interesting part for lua-injector users is `game_state_t`: it is a
 * plain, documented struct living in the game's .data section, found at
 * runtime by scanning for GAME_MARKER (a unique 31-byte ASCII string).
 * Scripts locate the marker with host.scan(), subtract 4 bytes, and then
 * read/write fields with host.peek/host.poke using the fixed offsets
 * below (there is no padding: every field is 4-byte aligned).
 *
 * Memory map (offsets relative to the struct base, see GAME.md):
 *
 *   0x00  int32  magic        == GAME_STATE_MAGIC ("LUAL")
 *   0x04  char   marker[32]   == GAME_MARKER
 *   0x24  int32  score        current score
 *   0x28  int32  lives        lives remaining
 *   0x2C  int32  level        current level
 *   0x30  int32  game_over    1 = game over
 *   0x34  int32  paused       1 = paused
 *   0x38  float  player_x     player top-left x (pixels)
 *   0x3C  float  player_y     player top-left y (pixels)
 *   0x40  float  speed        alien descent speed multiplier
 *   0x44  int32  alien_count  aliens still alive
 *   0x48  int32  bullet_count bullets in flight
 *   0x4C  int32  lua_msg_mode 1 = draw lua_msg in the window
 *   0x50  char   lua_msg[64]  text Lua scripts can display (NUL-terminated)
 *   0x90  float  target_x     nearest alive alien center (game-updated)
 *   0x94  float  target_y     nearest alive alien center (game-updated)
 *   0x98  int32  reserved[2]
 *   = 0xA0 bytes total
 */
#ifndef LUA_INJECTOR_TEST_GAME_LOGIC_H
#define LUA_INJECTOR_TEST_GAME_LOGIC_H

#include <stdint.h>
#include <stddef.h>

#define GAME_W              640
#define GAME_H              480
#define GAME_STATE_MAGIC    0x4C55414CUL   /* "LUAL" little-endian */
#define GAME_MARKER         "LUA-INJECTOR-TEST-GAME-4C2F9A1E"

#define PLAYER_W            40
#define PLAYER_H            16
#define ALIEN_W             24
#define ALIEN_H             14

#define MAX_ALIENS          30
#define MAX_BULLETS         16
#define MAX_BOMBS           16

typedef struct game_state {
    int32_t  magic;         /* 0x00 */
    char     marker[32];    /* 0x04 */
    int32_t  score;         /* 0x24 */
    int32_t  lives;         /* 0x28 */
    int32_t  level;         /* 0x2C */
    int32_t  game_over;     /* 0x30 */
    int32_t  paused;        /* 0x34 */
    float    player_x;      /* 0x38 */
    float    player_y;      /* 0x3C */
    float    speed;         /* 0x40 */
    int32_t  alien_count;   /* 0x44 */
    int32_t  bullet_count;  /* 0x48 */
    int32_t  lua_msg_mode;  /* 0x4C */
    char     lua_msg[64];   /* 0x50 */
    float    target_x;      /* 0x90 */
    float    target_y;      /* 0x94 */
    int32_t  reserved[2];   /* 0x98 */
} game_state_t;             /* 0xA0 */

/* compile-time layout checks (clang/gcc C11+; layout is padding-free so
 * it holds for MSVC too) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(game_state_t) == 0xA0, "game_state_t size must be 0xA0");
_Static_assert(offsetof(game_state_t, magic)       == 0x00, "magic offset");
_Static_assert(offsetof(game_state_t, marker)      == 0x04, "marker offset");
_Static_assert(offsetof(game_state_t, score)       == 0x24, "score offset");
_Static_assert(offsetof(game_state_t, lives)       == 0x28, "lives offset");
_Static_assert(offsetof(game_state_t, level)       == 0x2C, "level offset");
_Static_assert(offsetof(game_state_t, game_over)   == 0x30, "game_over offset");
_Static_assert(offsetof(game_state_t, paused)      == 0x34, "paused offset");
_Static_assert(offsetof(game_state_t, player_x)    == 0x38, "player_x offset");
_Static_assert(offsetof(game_state_t, player_y)    == 0x3C, "player_y offset");
_Static_assert(offsetof(game_state_t, speed)       == 0x40, "speed offset");
_Static_assert(offsetof(game_state_t, alien_count) == 0x44, "alien_count offset");
_Static_assert(offsetof(game_state_t, bullet_count)== 0x48, "bullet_count offset");
_Static_assert(offsetof(game_state_t, lua_msg_mode)== 0x4C, "lua_msg_mode offset");
_Static_assert(offsetof(game_state_t, lua_msg)     == 0x50, "lua_msg offset");
_Static_assert(offsetof(game_state_t, target_x)    == 0x90, "target_x offset");
_Static_assert(offsetof(game_state_t, target_y)    == 0x94, "target_y offset");
#endif

typedef struct {
    float x, y;
    int   alive;
} alien_t;

typedef struct {
    float x, y;
    int   alive;
} bullet_t;

typedef struct {
    float x, y;
    int   alive;
} bomb_t;

typedef struct {
    int left;         /* hold left arrow  */
    int right;        /* hold right arrow */
    int fire;         /* hold space       */
    int toggle_pause; /* edge on P        */
    int reset;        /* edge on R when game over */
} game_input_t;

typedef struct game {
    game_state_t state;          /* the documented, script-visible struct */
    alien_t      aliens[MAX_ALIENS];
    bullet_t     bullets[MAX_BULLETS];
    bomb_t       bombs[MAX_BOMBS];
    int          fire_cooldown;
    int          alien_tick;
    int          bomb_timer;
    int          hit_flash;
} game_t;

/* fresh game: resets state, spawns the alien formation */
void game_init(game_t *g);

/* advance the simulation by one frame (16 ms); updates state.* fields */
void game_update(game_t *g, const game_input_t *in);

#endif /* LUA_INJECTOR_TEST_GAME_LOGIC_H */
