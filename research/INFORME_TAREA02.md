# Informe de la Tarea T2 — Reproductor BRSTM funcional (Windows)

Estado: completado y verificado (ver sección 7).

## 1. Objetivo y alcance

Construir sobre el núcleo verificado en T1 (`core/`, 176/176 PASS) un
reproductor de escritorio Windows capaz de:

- abrir varios archivos `.brstm` y mostrar sus metadatos reales,
- reproducir con audio en tiempo real (waveOut), con bucle `loop` respetando
  los puntos del archivo,
- acelerar con el factor observado en Final Lap Stream Maker (el tono sube en
  proporción, igual que en FLSM), sin generar archivos modificados,
- y validar todo ello con pruebas automatizadas repetibles.

Fuera de alcance en T2: Android (T3) y web (T5).

## 2. Arquitectura implementada

```
main.c        interfaz Win32 (lista, panel de metadatos, transporte, controles de velocidad);
              nunca toca muestras ni el lector BRSTM; modo --selftest que
              conduce la ventana real y sus manejadores reales
player.c      cursor de decodificación, wrap del loop, velocidad, máquina de
              estados (DETENIDO/REPRODUCIENDO/PAUSADO/TERMINADO); hilo de
              motor propio con 4 ranuras de 512 frames (CALLBACK_EVENT)
audio_out.c   dispositivo waveOut: apertura, plegado estéreo, buffers
core/         lector BRSTM + decodificador DSP-ADPCM, SIN CAMBIOS respecto a T1
```

Principios mantenidos:

- **Sin precodificación**: el audio se produce por bloques pequeños bajo
  demanda (`player_render`), igual que en las tools de T1.
- **UI no bloqueante**: la interfaz consulta `player_get_state` en su temporizador
  (100 ms); ninguna llamada al motor superó ~35 ms (medido en las pruebas).
- **Ciclo de vida correcto**: abrir/cerrar dispositivo por reproducción;
  `player_destroy` devuelve el proceso a su nivel de reposo de manejadores.

## 3. Velocidad y mapeo con FLSM

Modelo (heredado de T0/T1): el dispositivo reproduce el PCM original a

```
device_rate = floor(sample_rate * speed_ppm / 1_000_000)
```

que es exactamente lo que hace FLSM al reescribir el campo de frecuencia del
archivo (2 bytes @0x64), pero en tiempo real y sin tocar el archivo. Al leer
las mismas muestras más rápido, el tono sube en proporción — el efecto pedido.

| Control UI            | speed_ppm | device_rate 44100 | device_rate 48000 |
|-----------------------|-----------|--------------------|--------------------|
| 1.0× (botón)          | 1000000   | 44100              | 48000              |
| FLSM (botón)          | 1068120   | 47104              | 51269              |

- `PLAYER_FLSM_SPEED_PPM = 1068120` reproduce las conversiones **medidas**:
  44100 → 47104 y 48000 → 51269 (T0 y colección).
- Solo existen dos caminos de UI para la velocidad: los botones **1,0×** y
  **Final Lap** (el slider 50–200 % y los botones 1,5×/2,0× se retiraron tras
  la entrega de T2). La API `player_set_speed(ppm)` sigue admitiendo cualquier
  razón de velocidad.
- **1.06812 es una cota observada en los archivos MKW estudiados, no una
  constante universal**; cualquier otro ppm es una relación de velocidad
  simple. Así queda documentado en `player.h`.

Verificación medida (grupo `realtime`): a 1.0× un archivo de 20,22 s duró
20,28 s; a FLSM durió 18,98 s (esperado 18,93 s; speedup medido 1,0682 vs
teórico 1,0681).

## 4. Loop en tiempo real

El problema abierto en T1 era de dónde obtener el estado ADPC exacto en
`loop_start`: los `loop_hist` de la sección HEAD parte 3 NO coinciden con el
estado real transportado, y la tabla ADPC está vaciada en algunos archivos.

Solución implementada en `player.c`: **capturar el estado del decodificador
al atravesar por primera vez el bloque que contiene `loop_start`** y
restaurarlo en cada wrap:

1. `loop_block = block_for_sample(loop_start)` al abrir.
2. En la primera decodificación secuencial que entra a ese bloque se guarda
   `loop_entering` (hist por canal) y el estado de bloque.
3. Al llegar a `total_samples` con loop activo: `cursor = loop_start`,
   `carried = loop_entering`, se invalida el buffer y se sigue decodificando
   desde el bloque de loop — la continuidad es exacta porque es el mismo
   camino de decodificación que habría hecho la reproducción lineal.

La sección `[loop_start, total)` se repite indefinidamente; archivos sin loop
o con el flag desactivado terminan en `total_samples` y pasan a TERMINADO.

Verificación (grupo `loop`): el PCM producido por el reproductor durante
8.406.092 frames (tKD_N: `total + (total − loop_start)`, un wrap completo) es
**byte a byte idéntico** a la referencia de vgmstream (`test.exe -l 2 -f 0`).
El wrap en tiempo real saltó exactamente a `loop_start` (573440) a las 18,99 s
y la posición permaneció dentro de `[loop_start, total)` durante la observación
de 1 s posterior.

## 5. Multicanal: plegado estéreo (fallback)

Medición: en esta máquina `WAVE_MAPPER` rechaza PCM de 4 y 8 canales con
`WAVERR_BADFORMAT` (= 32) — sonda aislada: 2 canales r=0, 4/8 canales r=32.

Comportamiento de `audio_open`:

1. Intento nativo con el formato del archivo.
2. Si falla por formato y hay >2 canales → reintentar como estéreo y marcar
   `folded=1`; `audio_write` pliega: promedio de canales de índice par →
   izquierda, impar → derecha (disposición estándar L R L2 R2; **heurística
   documentada**, ver §8).
3. La caché `native_rejected` recuerda por (rate, canales) los formatos ya
   rechazados para no repetir la apertura fallida (ver §6).

El UI muestra «Dispositivo: N Hz · estéreo» cuando se pliega; el snapshot
expone `audio_folded`. Un archivo de 4 canales se reprodujo con `folded=1`
y la posición avanzó (grupo `ta`).

## 6. Hallazgos medidos (winmm y CRT de Windows)

- **`waveOutOpen` fallido con formato >2c deja ~3 manejadores huerfanos en
  winmm**: sonda aislada (10 intentos fallidos + 10 éxitos) = +30 manejadores
  netos, sin pasar por nuestro código. Mitigación: caché `native_rejected`
  (solo el primer intento por formato paga el impuesto) + comprobaciones de
  fuga calibradas sobre ciclos estables, no sobre el absoluto tras destruir.
- **`fputs`/`fprintf` estrecho sobre un `FILE*` abierto con `ccs=UTF-8`
  aborta en esta CRT** (`0xC0000409`, verificado con sonda mínima:
  `fwrite` y `fwprintf` sí funcionan). Los informes de pruebas se abren en
  modo texto llano.
- `waveOutClose` es asíncrono: tras destruir, unos manejadores del driver
  siguen en vuelo unos ms; las comprobaciones permiten asentarse.
- Primer `waveOutOpen` del proceso: inicialización única de winmm (~100
  manejadores) que sobrevive al cierre; los ciclos posteriores son net-cero.
  Las pruebas hacen calentamiento antes de medir balances.

## 7. Verificación

Un único comando (requerido por T2):

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_t2_verification.ps1
```

Ejecuta, en orden: T1 antes → SHA-256 de la colección → build → mutantes
inválidos → batería `player_tests` completa → autotest de la UI real →
SHA-256 después (los originales no cambian) → T1 después.

Batería `player_tests` (grupos y casos obligatorios):

| Grupo     | Cubre |
|-----------|-------|
| metadata  | 44100/48000/51269 estéreo, 4ch, 8ch, sin loop vs valores esperados |
| speed     | mapeo device_rate = floor(rate·k) con k=1068120 |
| invalid   | 36 mutantes rechazados (info + decodificación + modo audio) |
| loop      | PCM byte a byte vs vgmstream (`-l 2 -f 0`) con wrap |
| noloop    | archivo sin loop termina exactamente en total |
| loopoff   | archivo con loop, loop desactivado → termina en total |
| memory    | 16 M frames en loop con memoria plana (+0,0 KB) |
| th        | transporte sin audio: pausa/reanudar/parar, latencia <500 ms |
| probe     | manejadores equilibrados en ciclos open/play/stop/close |
| ta        | dispositivo real: transporte, cambio de canción, 4ch plegado, rechazo de inválidos |
| realtime  | duración 1.0× vs FLSM, wrap en vivo, posición en [loop_start, total) |

Autotest UI (`brstm_player --selftest`): ventana y manejadores reales —
carga de 2 pistas, rechazo de archivo inválido con mensaje, selección con
metadatos, play con dispositivo sin error, latencia de la API <100 ms,
pausa con posición congelada, velocidad FLSM en vivo, conmutador de loop
(sincronía checkbox↔motor), cambio de canción reproduciendo, ciclos sin
crecimiento de manejadores/memoria y cierre de ventana.

Resultados de la ejecución de referencia (2026-10-19):

```text
=== 1. T1 (antes) ===              RESULT: PASS (176 checks)
=== 5. player_tests ===            RESULT: PASS (121 checks, 0 failed)
=== 6. autotest UI ===             RESULT: PASS (17 checks, 0 failed)
=== 7. SHA-256 ===                 colección sin cambios tras toda la reproducción
=== 8. T1 (después, reconstruido)  RESULT: PASS (176 checks)
=== resumen del script ===         RESULT: PASS (19 checks) — exit 0
```

Evidencias destacadas dentro de la batería:

- Loop: PCM **byte a byte idéntico a vgmstream** durante 8.406.092 frames
  (pasada inicial + sección de loop).
- Tiempo real (archivo o_Crs_In_Fan_Wifi, 48000 Hz, 4ch, 20,22 s):
  1,0× → 20,34 s medido; FLSM → 19,07 s (esperado 18,93 s);
  speedup medido 1,0665 vs teórico 1,0681; `device_rate` FLSM = 51269
  exacto; wrap en vivo saltó exactamente a `loop_start` = 573440.
- Multicanal: 4 canales reproduce vía `stereo fold (device rejected 4ch)`.
- Memoria: 16.003.072 frames en loop con crecimiento de working set
  de +0,0 KB.
- Manejadores: ciclos `open/play/stop/close` net-cero; tras destruir el
  proceso vuelve a su nivel de reposo (impuesto de winmm aislado en §6).

## 8. Problemas conocidos y limitaciones

1. **Plegado estéreo = heurística**: se asume la disposición estándar
   L R L2 R2 (par → izquierda). No está verificado por oído ni por un
   análisis de contenido cuál es la disposición real de cada archivo.
2. **Calidad audible del loop**: la identidad byte a byte con vgmstream cubre
   la corrección de muestras; no se ha hecho una prueba auditiva humana de
   ausencia de clics en el borde de loop (la transición es exactamente la
   misma decodificación continua, por lo que no debería haberlo).
3. **Impuesto de winmm** en aperturas fallidas: no es de nuestro código, pero
   una solución definitiva requeriría otro API de audio o aceptarlo.
4. El factor 1,06812 no está probado sobre un ejecutable de FLSM en esta
   tarea (se usó la constante medida en T0/T1).
5. Sin repo git: todos los cambios son creación/modificación de archivos.

## 9. Próximos pasos

- T3 (Android) y T5 (web) sobre el núcleo C ya verificado; el plegado y el
  modelo de velocidad deben reutilizarse, no reimplementarse.

## 10. Cambios posteriores a la entrega (T2+)

En la UI, tras la entrega de T2 y a petición del usuario:

- Se retiraron los botones 1,5× y 2,0× y el slider 50–200 %: la velocidad solo
  cambia con **1,0×** y **Final Lap ×1,06812**. La API `player_set_speed(ppm)`
  sigue admitiendo cualquier razón de velocidad; el motor no cambió.
- La barra de progreso pasó de `msctls_progress32` a un control propio
  (`MkwProgress`): sombrea la región `[loop_start, total)` en tono ámbar y
  dibuja una marca naranja en `loop_start`, de modo que los dos puntos de loop
  se ven mientras suena la canción (p. ej. tKD_N: marca al ~27,6 %,
  1347584 de 4876838 muestras). Verificado con reproducción real.
- El panel de información añade la fila «Fin de loop»: en los archivos MKW
  estudiados es el final del audio (diseño del modelo de loop `[start, total)`).
- El autotest de la UI pasó de 17 a 18 comprobaciones (verifica que la barra
  usa la clase personalizada `MkwProgress`).
