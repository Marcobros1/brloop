# Especificación de MOTION

**Fecha:** 2026-11-12
**Decisión:** Q-160 (variante 4)
**Respuesta:** motion-preview.html (prototipo descartable)

## Resumen

Nivel de movimiento: "momentos coreografiados, sin bucles". Cuatro efectos de un solo ciclo que acompañan los cambios de estado. No bloquean la entrada. Los tiempos son del prototipo; en Win32 se revisan en la ventana real.

## Decisiones

- **Q-161:** Encogido de botones al 94% del tamaño original, aplicado al botón entero (fondo, borde e icono). Zona clicable sin cambios. Sustituye al encogido solo del icono al 96%.
- **Q-162:** Estilo de botones: A (Ficha). Esquinas rectas, borde negro de 2 px, bisel inferior y hundimiento de 2 px al pulsar. Opción B preferida en el prototipo, pero se implementa A por decisión del usuario.
- **Q-163/Q-164:** Halo de Play. Tres versiones probadas; ninguna funciona bien en Win32 (botón owner-draw, controles hijos). Se quita.
- **Q-170:** Desplazamiento horizontal de 200 ms, de derecha a izquierda: panel viejo sale por la izquierda, nuevo entra por la derecha. Clicks aceptados durante el movimiento. Sustituye al fundido con AlphaBlend (Q-165/Q-166/Q-169) y a la regla de Q-160 de no mover controles.

## Efectos implementados

### 1. Encogido de botones (paso 2, Q-161)

- Al pulsar un botón owner-draw, se aplica una transformación de escala al 94% durante 120 ms.
- Incluye fondo, borde e icono.
- La zona clicable no cambia (el clic se detecta antes de la animación).
- Se conecta a WM_DRAWITEM: al detectar ODS_SELECTED, se dibuja el botón al 94%.

### 2. Desplazamiento de panel (paso 4, Q-170)

- Al cambiar entre modes (Library ↔ Settings), los paneles se desplazan horizontalmente 200 ms.
- Dirección: panel antiguo sale por la izquierda, nuevo entra por la derecha.
- Se usa MoveWindow para mover los controles hijos.
- El estado cambia al instante; el desplazamiento solo acompaña.
- Clicks aceptados durante el desplazamiento (en controles del modo nuevo).

### 3. Barrido de progreso (paso 5)

- Al cambiar de pista, la barra de progreso se barres 500 ms.
- Efecto de "sweep": la barra cambia de brillo (brightness) de 1.4 a 1.0 en 500 ms.
- Implementado en el paint de la barra de progreso personalizada.

## No implementado (decidido explícitamente)

- **Halo de Play (Q-163/Q-164):** No funciona en Win32 con controles owner-draw. Se quita.
- **Fila desplazada (Q-160 opción 3):** Sustituido por desplazamiento de panel completo.
- **Transición de barra de progreso (Q-160):** Sigue la posición al instante; sin interpolación.
- **Transiciones de color:** Cambios de estado inmediatos; sin animación.
- **Deslizamiento de paneles:** Sustituido por desplazamiento horizontal.

## Especificaciones técnicas

### Win32

- Animaciones implementadas con temporizador (SetTimer) y invalidate de la ventana.
- Botones owner-draw: la animación de encogido se aplica en WM_DRAWITEM.
- Desplazamiento de panel: MoveWindow con SetTimer para el desplazamiento en pasos.
- Barrido de progreso: invalidate de la barra de progreso en cada frame de la animación.

### Multiplataforma (futuro)

- Módulo `anim_fade.c` diseñado para encapsular el fundido (Q-166).
- En sistemas sin animación, el cambio es instantáneo.
- Main.c solo llama a la interfaz, sin #ifdef.

## Criterios de aceptación

- La transición entre modes (Library ↔ Settings) muestra desplazamiento horizontal.
- Los botones owner-draw se encogen al 94% al pulsar.
- La barra de progreso hace el efecto de barrido al cambiar de pista.
- No hay bucles en las animaciones; todos los efectos son de un solo ciclo.
- El estado cambia al instante; las animaciones solo acompañan.
