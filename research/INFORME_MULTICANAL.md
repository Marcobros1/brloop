# INFORME MULTICANAL — Selección de versión por pares de canales

Fecha: 2026-10-20. Fase: posterior a la entrega de T2, antes de T3 (Android).
Alcance: investigación del significado de los canales en los BRSTM multicanal de
Mario Kart Wii (originales y del mod Metal Kart Wii Retro Rewind) e implementación
de la elección de versión con un botón, a petición del usuario.

## 1. Objetivo

El usuario pidió poder **elegir pares de canales que suenan a una versión u otra**
de la música, con un botón que muestre la versión correspondiente, en lugar de
mezclar todos los canales a la vez. Antes de tocar el motor hubo que determinar,
con datos, qué representa cada canal en los ficheros reales.

## 2. Metodología

- **Herramienta nueva**: `core/tools/brstm_channels.exe` (añadida a `core/build.ps1`;
  `brstm.c`/`dsp_adpcm.c` sin cambios). Decodifica con **nuestro decodificador T1
  verificado** (no una reimplementación) y para cada canal calcula:
  - RMS y pico; actividad (ventanas de 1024 muestras por encima del 8 % de su propio pico);
  - correlación de Pearson a nivel de muestra entre todos los pares de canales;
  - correlación de envolvente (mismas ventanas de 1024);
  - **duplicados bit a bit** (canal idéntico muestra a muestra, con salida anticipada);
  - **track table del propio fichero** (HEAD parte 2, especificación pública de
    wiibrew.org/wiki/BRSTM_file): qué canales define el archivo como cada «track».
- Uso: `core\build\brstm_channels.exe <fichero> [--wav <dir>]` (los WAV salen
  siempre a un directorio temporal propio, nunca a la colección).
- Muestra: los **19 ficheros con >2 canales** de la colección (8ch: `STRM_N_KOOPA_N/_f`,
  `STRM_N_WATER_N/_f`; 4ch: `STRM_N_{RIDGEHIGHWAY,FACTORY,TRUCK,DESERT}_N/_f`,
  `o_Crs_In_Fan{,_Wifi,_battle}`, `o_FanfareGP1_32`, `o_Start32_fan`,
  `o_Start2_32_fan`, `o_Ending_PartA_32`).

## 3. Resultados de los 19 ficheros

### 3.1 La track table confirma el patrón en 19/19

Cada fichero define sus tracks como **pares estéreo adyacentes exactos**
(track 0 = ch0+ch1, track 1 = ch2+ch3, …). Fanfares originales `o_*`: descripción
tipo 0; `STRM_*`: tipo 1 con volumen/pan (`pan=64` = centro en todos).

| Fichero | Canales | Tracks | Volumen de track |
|---|---|---|---|
| `STRM_N_KOOPA_N` | 8 | 4 × estéreo adyacente | 90 / 80 / 80 / 80 |
| `STRM_N_WATER_N` | 8 | 4 × estéreo adyacente | (tipo 1) |
| `STRM_N_{DESERT,RIDGEHIGHWAY,…}_N` | 4 | 2 × estéreo adyacente | 127 / 88 |
| `o_Crs_In_Fan*`, `o_*_32*` | 4 | 2 × estéreo adyacente | tipo 0 (sin volumen) |

### 3.2 Contenido por correlación (qué suena en cada par)

- **KOOPA (8ch, Castillo de Bowser)**: correlación intra-par 0,38–0,72 a nivel de
  muestra y 0,62–0,86 de envolvente; **inter-par ≈ 0,00–0,02** → **4 pares
  independientes** (cada par, una versión/mix distinto de la pista).
- **WATER (8ch)**: ch0≡ch6 y ch1≡ch7 con Pearson 1,00 (e idem para ch2/ch4, ch3/ch5),
  pero **no** bit idénticos → **2 versiones únicas duplicadas** en los 4 pares.
- **Circuitos 4ch (DESERT, RIDGEHIGHWAY, FACTORY, TRUCK)**: intra-par 0,44–0,75,
  **inter-par ≈ 0,00** (envolvente 0,07–0,34) → **2 versiones independientes** por
  fichero. Es la fuente del efecto «a toda pastilla» del plegado actual.
- **Fanfares `o_*` (4ch)**: `ch0 == ch2` y `ch1 == ch3` **bit a bit** → una sola
  estéreo duplicada; el plegado actual ya suena correcto ahí (media de copias iguales).

### 3.3 Qué dice la documentación de la comunidad

- mkwiiki.org/wiki/Custom_Music: 6 pistas oficiales de MKW usan BRSTM multicanal
  (Toad's Factory, Wario's Gold Mine, Koopa Cape, Dry Dry Ruins, Moonview Highway,
  Bowser's Castle); «*the amount of channels on a track is how many different types
  of music can play on it but doubled*».
- Hilos de la comunidad (Smashboards/mkwiiki): las porciones indoor/outdoor de la
  pista «utilizan canales de audio diferentes»; los `.brstm` originales extraídos de
  MKW son de 4 canales (2 estéreo) y con 2 canales «media pista se queda en silencio»
  → el juego **busca el par por zona y suena uno solo**.

**Conclusión**: cada par = **una versión estéreo completa**, el juego **nunca** reproduce
varios pares a la vez. Nuestros datos (pares independientes + track table) son
consistentes con esa semántica: mezclar todos los pares (modo actual) no reproduce
ningún estado real del juego.

## 4. Decisión de implementación

- **Par = versión.** Selección por pares adyacentes (validado por la track table en
  19/19; si algún fichero futuro define algo distinto, el analizador lo reportará).
- **Motor** (`player.h`/`player.c`):
  - `channel_pair` (−1 = «Todas (mezcla)», **valor por defecto** para no alterar el
    comportamiento ni las pruebas de T2); `pair_count`/`pair_sel` en el snapshot.
  - `render_locked` con par activo emite **solo `ch[2p]/ch[2p+1]` como estéreo**
    (bit exacto, verificable sin dispositivo).
  - Con par activo el dispositivo **abre estéreo** (evita el plegado y la apertura
    multicanal nativa); `player_set_channel_pair()` valida el rango y, si hay audio
    sonando, **reaplica el dispositivo en vivo** (mismo camino que el cambio de velocidad).
  - Se resetea a −1 al abrir/cerrar fichero.
- **UI** (`main.c`): fila «Versión:» en el panel de información con un **botón** que
  cicla `Todas (mezcla) → Versión 1 · canales 1-2 → Versión 2 · canales 3-4 → …`;
  deshabilitado (texto «Estéreo»/«—») en ficheros sin pares; el estado también se
  refleja en la barra de estado. El sufijo «· estéreo» del dispositivo aparece tanto
  en plegado como con par activo.
- **Volumen de track (hipótesis abierta)**: la track table declara 127/88 (y 90/80 en
  KOOPA). Si el motor del juego aplica ese volumen al sonar cada track, la versión 2
  sonaría ~3 dB más baja in-game. **Decisión**: reproducir el par en bruto, sin aplicar
  volumen — es la interpretación directa de «elegir una versión» y es reversible si se
  comprueba en el juego que el volumen se aplica.

## 5. Pruebas y verificación

| Nivel | Resultado |
|---|---|
| Nuevo grupo `pair` (batería `player_tests`) | **33/33** — cada par render **bit exacto** vs canales aislados (4ch), por defecto = todos los canales, validación de rangos, `pair_count` 8ch=4 / estéreo=0, seek con par activo alineado con la referencia |
| Batería completa | **185/185** (152 previos + 33) |
| Autotest de UI (`--selftest`) | **29/29** (9 nuevos: pares del fichero 4ch, ciclado del botón, caption «Versión 2», cambio en vivo reproduciendo, vuelta a mezcla sin errores de audio) |
| `tests/run_t2_verification.ps1` | **PASS 19/19** — T1 176/176 antes/después, batería, autotest, **SHA-256 de la colección sin cambios** |
| Captura visual | `t3_multi/ui_version.png`: KOOPA seleccionado, reproduciendo, botón en «Versión 1 · canales 1-2» |
| Escucha manual (pendiente del usuario) | WAVs por par en `%LOCALAPPDATA%\Temp\opencode\t3_multi\wav\` (WATER + 4 circuitos 4ch) y `t3_multi\koopa\` (KOOPA: canales mono, 4 pares y mezcla) |

## 6. Lecciones de la verificación visual externa

Para drivear la UI desde fuera del proceso (scripts en `Temp\opencode\t3_multi\`):
- **Nunca enviar punteros entre procesos**: `LVM_SETITEMSTATE` con `LVITEM*` propio
  mata la app ajena con 0xC000041D (excepción en su wndproc). Selección válida =
  `WM_LBUTTONDOWN/UP` con coordenadas empaquetadas (misma técnica que el seek de T2).
- `WM_GETTEXT` crudo por `SendMessage` no marshala el buffer → texto vacío;
  usar `GetWindowTextW`.
- Enviar con `SendMessageTimeout` (anti-cuelgue) y **matar siempre la app al final**:
  un hijo vivo hereda el pipe del shell y cuelga la orden.
- El selftest interno selecciona intra-proceso (`ListView_SetItemState`), por eso
  sus 29 checks no reproducían el fallo: era artefacto del arnés, no del reproductor.

## 7. Próximo paso

Multicanal queda cerrado a nivel de código y pruebas automatizadas. Queda la
**prueba auditiva del usuario** con los WAVs por par (identificar qué par es qué
cover en KOOPA y confirmar que las versiones suenan bien en la app), y verificar en
el juego si aplica el volumen de track. Después: **T3 (Android)**, reutilizando
`core/`, el modelo de velocidad y la lógica de pares.
