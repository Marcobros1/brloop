#ifndef WAIT_H
#define WAIT_H

#include <windows.h>

#include "player.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Procesa los mensajes de Win32 durante `ms` ms (en lugar de dormir) para
 * que la UI bajo prueba siga recibiendo callbacks mientras espera.
 * No depende de audio ni de una ventana real: solo de la cola de mensajes. */
void wait_pump(DWORD ms);

/* Espera hasta que `cond` devuelva verdadero o expire `timeout_ms`.
 * Devuelve el valor final de `cond`: 1 si se cumplió, 0 si hubo timeout
 * (una falla, nunca un cuelgue). Es la función que se prueba con mocks. */
int wait_until_pred(int (*cond)(void), DWORD timeout_ms);

/* Asigna el jugador cuyas lecturas de estado usa wait_until_for_test.
 * Permite inyectar un jugador de prueba (o NULL) sin depender del selftest. */
void wait_set_player(Player *player);

/* Espera hasta que el jugador alcance el estado `want` o expire el timeout.
 * Devuelve 1 si lo alcanzó, 0 si no. */
int wait_until_for_test(PlayerState want, DWORD timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WAIT_H */
