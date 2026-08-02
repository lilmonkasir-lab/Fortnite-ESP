/*
 * win_game.c — Win32 shell for the Lua Injector Test Game.
 *
 * A tiny GDI game: move with the arrow keys, shoot with space, pause with
 * P, restart with R after game over.  All gameplay lives in game_logic.c;
 * this file only owns the window, the timer, the back buffer and the
 * drawing.
 *
 * The window title includes the process id so it is easy to find with
 * `lua-injector-x64.exe --list`.
 *
 * Build: see the Makefile (`make bin/lua-test-game-x64.exe`) or build.bat.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_logic.h"

#define WINDOW_CLASS L"LuaInjectorTestGameW"

static game_t  g_game;
static HDC     g_bufDC;
static HBITMAP g_bmp;
static HFONT   g_font;
static HFONT   g_fontBig;
static int     g_p_prev;
static int     g_r_prev;

static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF rgb) {
    HBRUSH br = CreateSolidBrush(rgb);
    RECT r = { x, y, x + w, y + h };
    FillRect(dc, &r, br);
    DeleteObject(br);
}

static void draw_frame(HWND hwnd) {
    game_state_t *s = &g_game.state;

    fill_rect(g_bufDC, 0, 0, GAME_W, GAME_H, RGB(10, 10, 16));
    SetBkMode(g_bufDC, TRANSPARENT);

    /* aliens */
    for (int i = 0; i < MAX_ALIENS; i++) {
        if (!g_game.aliens[i].alive) continue;
        fill_rect(g_bufDC, (int)g_game.aliens[i].x, (int)g_game.aliens[i].y,
                  ALIEN_W, ALIEN_H, RGB(255, 70, 70));
    }
    /* bombs */
    for (int i = 0; i < MAX_BOMBS; i++) {
        if (!g_game.bombs[i].alive) continue;
        fill_rect(g_bufDC, (int)g_game.bombs[i].x, (int)g_game.bombs[i].y,
                  4, 10, RGB(255, 140, 0));
    }
    /* bullets */
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!g_game.bullets[i].alive) continue;
        fill_rect(g_bufDC, (int)g_game.bullets[i].x, (int)g_game.bullets[i].y,
                  4, 8, RGB(255, 220, 60));
    }
    /* player (flashes white briefly after being hit) */
    fill_rect(g_bufDC, (int)s->player_x, (int)s->player_y, PLAYER_W, PLAYER_H,
              g_game.hit_flash > 0 ? RGB(255, 255, 255) : RGB(80, 255, 120));

    /* HUD */
    SelectObject(g_bufDC, g_font);
    SetTextColor(g_bufDC, RGB(230, 230, 230));
    char line[256];
    snprintf(line, sizeof line, "score %d    lives %d    level %d",
             s->score, s->lives, s->level);
    TextOutA(g_bufDC, 8, 6, line, (int)strlen(line));
    snprintf(line, sizeof line, "state 0x%llX    marker \"%s\"",
             (unsigned long long)(uintptr_t)(void *)s, GAME_MARKER);
    TextOutA(g_bufDC, 8, 26, line, (int)strlen(line));
    snprintf(line, sizeof line,
             "attach: lua-injector-x64.exe --pid %lu",
             (unsigned long)GetCurrentProcessId());
    TextOutA(g_bufDC, 8, GAME_H - 24, line, (int)strlen(line));

    /* text written by a Lua script */
    if (s->lua_msg_mode && s->lua_msg[0]) {
        SetTextColor(g_bufDC, RGB(60, 220, 255));
        TextOutA(g_bufDC, 8, 46, s->lua_msg, (int)strlen(s->lua_msg));
    }

    /* overlays */
    RECT center = { 0, GAME_H / 2 - 60, GAME_W, GAME_H / 2 + 60 };
    if (s->paused) {
        SelectObject(g_bufDC, g_fontBig);
        SetTextColor(g_bufDC, RGB(255, 255, 255));
        DrawTextA(g_bufDC, "PAUSED - P", -1, &center,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(g_bufDC, g_font);
    }
    if (s->game_over) {
        SelectObject(g_bufDC, g_fontBig);
        SetTextColor(g_bufDC, RGB(255, 60, 60));
        DrawTextA(g_bufDC, "GAME OVER - PRESS R", -1, &center,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(g_bufDC, g_font);
    }

    (void)hwnd;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        g_bufDC = CreateCompatibleDC(dc);
        g_bmp = CreateCompatibleBitmap(dc, GAME_W, GAME_H);
        if (g_bufDC && g_bmp) SelectObject(g_bufDC, g_bmp);
        ReleaseDC(hwnd, dc);
        g_font = CreateFontW(-18, 0, 0, 0, FW_BOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        g_fontBig = CreateFontW(-44, 0, 0, 0, FW_BOLD, 0, 0, 0,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        srand((unsigned)GetTickCount());
        game_init(&g_game);
        SetTimer(hwnd, 1, 16, NULL);
        return 0;
    }
    case WM_TIMER: {
        game_input_t in;
        memset(&in, 0, sizeof in);
        in.left  = (GetAsyncKeyState(VK_LEFT)  & 0x8000) != 0;
        in.right = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
        in.fire  = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        int p = (GetAsyncKeyState('P') & 0x8000) != 0;
        if (p && !g_p_prev) in.toggle_pause = 1;
        g_p_prev = p;
        int r = (GetAsyncKeyState('R') & 0x8000) != 0;
        if (r && !g_r_prev) in.reset = 1;
        g_r_prev = r;
        game_update(&g_game, &in);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        draw_frame(hwnd);
        BitBlt(dc, 0, 0, GAME_W, GAME_H, g_bufDC, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* avoid flicker; we redraw everything in WM_PAINT */
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_font) DeleteObject(g_font);
        if (g_fontBig) DeleteObject(g_fontBig);
        if (g_bmp) DeleteObject(g_bmp);
        if (g_bufDC) DeleteDC(g_bufDC);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassW(&wc)) return 1;

    char title[128];
    snprintf(title, sizeof title, "Lua Injector Test Game [pid %lu]",
             (unsigned long)GetCurrentProcessId());

    HWND hwnd = CreateWindowW(WINDOW_CLASS, L"", WS_CAPTION | WS_SYSMENU |
                              WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                              GAME_W + 16, GAME_H + 39, NULL, NULL, hInst,
                              NULL);
    if (!hwnd) return 1;
    SetWindowTextA(hwnd, title);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
