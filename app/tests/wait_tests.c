/* Automated unit tests for the wait helpers (wait.c): console, no window,
 * no audio. wait_until_pred() is driven with a mocked condition so each case
 * runs in milliseconds and never depends on a real timer or device.
 *
   Uso: wait_tests.exe
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>

#include "player.h"
#include "wait.h"

static int g_pass, g_fail;

/* ----------------------------------------------------------------- waits --- */

/* Condición mock: devuelve 1 a partir de la llamada `g_true_from`. */
static int g_calls;
static int g_true_from;

static int mock_cond(void)
{
    g_calls++;
    return g_calls >= g_true_from;
}

static void check(int ok, const char *fmt, ...)
{
    va_list ap;
    char msg[1024], line[1100];
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[%s] %s", ok ? "PASS" : "FAIL", msg);
    puts(line);
    fflush(stdout);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

static void test_cond_true_immediately(void)
{
    g_calls = 0;
    g_true_from = 1;
    check(wait_until_pred(mock_cond, 100) == 1, "cond ya verdadero: devuelve 1");
    check(g_calls == 1, "cond ya verdadero: un solo llamado (%d)", g_calls);
}

static void test_cond_true_after_several_polls(void)
{
    g_calls = 0;
    g_true_from = 3;
    check(wait_until_pred(mock_cond, 100) == 1, "cond tras varios polls: devuelve 1");
    check(g_calls >= 3, "cond tras varios polls: llamo suficiente (>=3, fue %d)", g_calls);
}

static void test_cond_timeout_returns_zero(void)
{
    g_calls = 0;
    g_true_from = 100000; /* nunca se cumple en el tiempo dado */
    check(wait_until_pred(mock_cond, 30) == 0, "cond nunca se cumple: devuelve 0 (timeout)");
    check(g_calls > 1, "cond nunca se cumple: hizo varios polls (%d)", g_calls);
}

static void test_wait_pump_headless(void)
{
    /* Sin ventana ni cola de mensajes propia: no debe bloquearse ni fallar. */
    wait_pump(5);
    check(1, "wait_pump sin ventana devuelve sin bloquear");
}

/* ------------------------------------------------------------ player path --- */

static void test_wait_until_for_test_stopped(void)
{
    Player *p = player_create(0); /* headless, sin audio */
    check(p != NULL, "player headless se crea");
    if (p) {
        wait_set_player(p);
        /* Estado inicial es STOPPED y wait_until_for_test lo ve sin esperar. */
        check(wait_until_for_test(PLAYER_STOPPED, 50) == 1,
              "estado inicial STOPPED se cumple de inmediato");
        check(wait_until_for_test(PLAYER_PLAYING, 5) == 0,
              "estado PLAYING no se cumple sin reproducir (timeout)");
        player_destroy(p);
    }
}

int main(void)
{
    puts("=== T2 wait unit tests (headless, sin audio ni ventana) ===");
    fflush(stdout);

    test_cond_true_immediately();
    test_cond_true_after_several_polls();
    test_cond_timeout_returns_zero();
    test_wait_pump_headless();
    test_wait_until_for_test_stopped();

    {
        char line[200];
        snprintf(line, sizeof line, "RESULT: %s (%d checks, %d failed)",
                 g_fail ? "FAIL" : "PASS", g_pass + g_fail, g_fail);
        puts(line);
        fflush(stdout);
    }
    return g_fail ? 1 : 0;
}
