#include "wait.h"

/* Jugador cuyas lecturas de estado usa wait_until_for_test. Se inyecta con
 * wait_set_player; el selftest pasa el suyo, los tests unitarios pueden pasar
 * un mock o dejarlo NULL. */
static Player *g_player;

void wait_pump(DWORD ms)
{
    DWORD deadline = GetTickCount() + ms;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    } while (GetTickCount() < deadline);
}

int wait_until_pred(int (*cond)(void), DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    do {
        if (cond())
            return 1;
        wait_pump(10);
    } while (GetTickCount() < deadline);
    return cond();
}

void wait_set_player(Player *player)
{
    g_player = player;
}

int wait_until_for_test(PlayerState want, DWORD timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    do {
        if (g_player && player_get_state(g_player).state == want)
            return 1;
        wait_pump(10);
    } while (GetTickCount() < deadline);
    return g_player && player_get_state(g_player).state == want;
}
