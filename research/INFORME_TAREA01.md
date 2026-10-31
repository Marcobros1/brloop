# Informe de Tarea 01 — Investigación técnica y viabilidad del reproductor BRSTM

Fecha: 2026-10-19. Alcance: investigación y experimentación; sin implementación del
reproductor ni de la interfaz. Ningún archivo de la colección original fue modificado
(todas las pruebas usaron copias en `%LOCALAPPDATA%\Temp\opencode\` o decodificaciones
de solo lectura).

---

## A. Informe de investigación

### A.1 Entorno y herramientas disponibles (comprobado)

| Herramienta | Ubicación | Estado |
|---|---|---|
| Final Lap Stream Maker v2.0 | `MKW_FLSM` (ruta local al ejecutable) | Encontrado; .NET WinForms (GUI, .NET 4.5.2), se lanza y responde |
| foobar2000 | `C:\Program Files\foobar2000` | Encontrado; solo componentes de fábrica (sin componente BRSTM instalado; `foo_input_std` no lista BRSTM) |
| LoopingAudioConverter 3.2 | Carpeta local de LAC | Encontrado; incluye `tools\vgmstream\test.exe` (CLI decodificador), ffmpeg y VGAudio |
| Colección de referencia | `%APPDATA%\CT-MKWII\Mods\Metal Kart Wii Retro Rewind` (o `MKW_COLLECTION`) | 301 archivos `.brstm` + `revo_kart.brsar`; solo lectura durante todo el trabajo |
| Copias de trabajo | Carpeta local de trabajo de FLSM | Verificado por SHA-256: idénticos a la colección |
| Toolchain | Python 3.9, Node, .NET SDK, JDK 21, git; sin gradle ni ffmpeg en PATH | Comprobado por ejecución |

Límite documentado: foobar2000 no sirvió para inspeccionar BRSTM (no tiene componente
instalado); la inspección se hizo con vgmstream CLI (legítimo, incluido en LAC) y con
un parser propio de solo lectura.

### A.2 Fase 1 — Inspección de la colección (experimental)

Script: `research/brstm_inventory.py` (+ `brstm_header_probe.py`, `loop_alignment.py`).

- 301/301 archivos con magia `RSTM` válida y tamaño declarado = tamaño real (0 desajustes).
- Códec: siempre DSP ADPCM de 4 bits (codec=2). Canales: 282×2ch, 15×4ch, 4×8ch.
- Frecuencias: 149×44100 Hz, 112×48000 Hz, **40×51269 Hz** (las 40 son variantes `_f`).
- Solo 1 archivo sin loop: `o_Wi-Fi_waiting32.brstm` (loop flag=0). Los otros 300 tienen loop.
- 40 pares `_f`/`_n` (final-lap vs normal): en 39 de ellos `loop_start`, `total` y canales
  son idénticos y **solo cambia la frecuencia** (48000→51269, ratio 1.068104).
  La excepción (`r_sfc_battle`) son dos archivos distintos, no un par de velocidad.
- `loop_start` es múltiplo exacto de `samples per block` (14336 muestras, interleave
  0x2000) en **300/300** archivos con loop (propiedad del corpus, no de la especificación).

### A.3 Fase 2 — Final Lap Stream Maker

1. **Opciones visibles (experimental/UIA + textos del recurso)**: la v2.0 tiene un único
   botón ("Click here or drag and drop a BRSTM/BCSTM/BFSTM to make a Final Lap version"),
   después `OpenFileDialog` → `SaveFileDialog` → mensaje "BRSTM successfully saved in …".
   **No hay control de velocidad, tono ni porcentaje** en la interfaz (revisados todos los
   textos UTF-16/ASCII del exe; método: `research/flsm_strings.py`, solo cadenas visibles,
   sin descompilar IL).
2. **Documentación (wiki oficial enlazada por el propio programa)**: "generates a
   speeded up version of any BRSTM/BCSTM/BFSTM" para final lap de MKW/7/8. Uso: abrir →
   guardar. Sin parámetros ni factor documentado. v1.0 2016-10-02, v2.0 2016-10-06.
3. **Prueba de conversión (T0, ejecutada)**: conversión real sobre `5_1_N.brstm`
   (44100 Hz) → `5_1_F.brstm` (carpeta de trabajo de FLSM) (8 625 568 bytes, mismo tamaño que la entrada).
   Diff binario completo (`flsm_compare.py`): **un único rango distinto: 2 bytes en
   0x64–0x65**, el campo de frecuencia 0xAC44 (44100) → 0xB800 (47104). Todo lo demás
   byte a byte idéntico: cabecera, offsets, loop flag, loop_start (3555328), total
   (7543345) y **todo el payload ADPCM de DATA**.
4. **Decodificación cruzada** (vgmstream `-i` sobre ambos): PCM **byte-idéntico**
   (30 173 380 bytes). Duración 171.05 s → 160.14 s (ratio 1.068118 = relación de
   frecuencias): acelera **y sube el tono proporcionalmente**, con loop intacto.
   → Confirmado que FLSM = cambio del campo de frecuencia declarado; sin reescritura
   de audio ni nuevo procesamiento audible.
5. **Factor**: proporcional, **no** un 51269 fijo. Medido: 44100→47104 (×1.068118) y,
   en la colección, 48000→51269 (×1.068104). Ambos son consistentes con una única
   constante k ∈ [1.0681179, 1.068125) (p. ej. k≈1.06812) aplicada con **truncado
   entero** (k×44100→47104, k×48000→51269.x→51269). El valor exacto de k no está
   determinado; no afecta al diseño del reproductor.

### A.4 Fase 3 — Formato BRSTM y loop (experimental + documentación)

Documentación: wiibrew.org/wiki/BRSTM_file (recuperada). Verificación empírica con 6
archivos de la colección (`brstm_header_probe.py`): todos los offsets cuadran.

Cabecera RSTM (0x00–0x3F, big-endian): magia `RSTM`, BOM `FEFF`, tamaño de fichero (0x08),
offsets/tamaños de HEAD (0x10/0x14), ADPC (0x18/0x1C) y DATA (0x20/0x24).

HEAD + "parte 1" (offset relativo a HEAD+0x08, apuntado en HEAD+0x0C):

| Campo | Offset (absoluto típico) | Verificado en |
|---|---|---|
| codec (2 = ADPCM 4 bits) | part1+0x00 (0x60) | 301 archivos |
| **loop flag** (0/1) | part1+0x01 (0x61) | 301 archivos |
| canales | part1+0x02 (0x62) | 301 archivos |
| **sample rate** (u16) | part1+0x04 (**0x64**) | 301 archivos; coincide con vgmstream |
| **loop start** (u32, muestras) | part1+0x08 (0x68) | coincide con vgmstream |
| **total samples** (u32, muestras) | part1+0x0C (0x6C) | coincide con vgmstream |
| offset datos ADPCM / nº bloques / tamaño bloque / muestras por bloque | part1+0x10.. | 0x1420 / n / 0x2000 / 14336 |

Conclusiones sobre el loop:

- **No existe campo "loop end": la sección de loop es `[loop_start, total_samples)`**.
  Coincide con vgmstream, que reporta `loop end == stream total samples` en los 300.
- Unidades: muestras PCM por canal (frames), big-endian. Relación con el audio decodificado
  = índice directo de frames decodificados.
- Transición: vgmstream produce, con `-l 2 -f 0`, exactamente `total + (total-loop_start)`
  frames, y el tramo añadido es **byte-idéntico** al `[loop_start, total)` original
  (`loop_verify.py`, archivo uTTC_N). Es decir, el loop es un reinicio exacto de la
  posición, no una mezcla.
- Calidad de la costura (uTTC_N): salto en la frontera = 474 de amplitud, frente a
  delta media local 1753 y p95 5305 → la discontinuidad del contenido es **menor que el
  movimiento habitual de la señal** (ratio 0.089): sin clic evitable. (Depende del
  contenido, no de la implementación; el contenido del corpus tiene bucles bien elegidos.)
- Precisión ADPCM: el codec es predictivo; al saltar hay que restaurar el estado DSP.
  HEAD parte 3 guarda por canal coeficientes, predictor/escala iniciales, historial
  (samples −1/−2) **y estado "loop"** (predictor/escala/historial de loop); el chunk ADPC
  da historial en cada frontera de bloque. Un decodificador propio debe (a) decodificar
  desde el inicio del bloque que contiene el objetivo y descartar prefijo, y (b) al hacer
  wrap, reinstaurar el estado de loop de HEAD. Esto es lo que vgmstream hace
  (inferencia razonada sobre su comportamiento observable; la estructura de datos está
  verificada).
- Alineación: en este corpus `loop_start` cae siempre en frontera de bloque (300/300),
  lo que simplifica el seek; **no debe asumirse para archivos de terceros**.
- Sin loop: `o_Wi-Fi_waiting32` → vgmstream no informa de loop y la duración es un único
  paso (7870464 muestras): reproducción única. Comportamiento propuesto: si loop flag=0,
  reproducir hasta el final y detener/fade-out.
- Metadatos inválidos: no se encontró ningún archivo inconsistente en el corpus (las
  comprobaciones de tamaño y offsets cuadran en 301/301). Restricción propuesta del motor:
  validar magia, offsets dentro del fichero, `loop_start <= total`, `rate > 0`, codec
  conocido; si algo falla, error claro (la propia FLSM muestra "The file you tried to
  open is not a correct BRSTM/BCSTM/BFSTM file").

### A.5 Fase 4 — Aceleración en tiempo real (experimental)

Experimento 1 — variante `_f` (par `n_rainbow32`, `wav_compare.py` + decodificación con
vgmstream `-i`):

- Los dos WAV decodificados tienen **PCM byte-idéntico** (34 285 700 bytes).
- Solo difiere la frecuencia del `fmt`: 48000 vs 51269 (ratio 1.068104).
- Duración: 178.57 s → 167.19 s (misma relación). El loop en muestras no cambia.
- → La "versión final lap" acelera **y sube el tono proporcionalmente** (modo
  resampleo/tape), sin reescribir audio. Reproducir los mismos PCM a otra frecuencia
  produce exactamente este efecto.

Experimento 2 — prototipo mínimo de lector con loop + velocidad variable en tiempo real
(`speed_loop_experiment.py`, solo Python stdlib, sin dependencias nuevas):

- Lectura por índice de muestra con interpolación lineal, wrap `[loop_start, total)`,
  avance por muestra = velocidad (cambiable en caliente = cambio de velocidad en vivo).
- Costura a velocidad 1.0: salto 474 vs p95 5581 (0.085); a velocidad 1.30: salto 1676
  vs p95 6660 (0.252) → por debajo del movimiento típico: **sin clic evitable**.
- Cambiar la velocidad en vivo no introduce estado discontinuo (solo cambia el avance).
- Rendimiento: 10 s de audio en 1.35 s = **~7.4× tiempo real en Python puro por
  muestra**; un decodificador en C/Kotlin nativo estará órdenes de magnitud por encima
  (GPU/CPU: el ADPCM 4 bits es barato). No se creó ningún archivo BRSTM.
- Audición: `%LOCALAPPDATA%\Temp\opencode\brstm_exp\xpt_speed100_wrap.wav` y
  `xpt_speed130_wrap.wav`.

Implicación: la aceleración en tiempo real **no exige decodificar toda la canción**:
basta procesar por bloques (el formato ya es por bloques de 0x2000 bytes/14336 muestras)
y mantener la posición de lectura como índice de muestra con wrap.

### A.6 Fase 5 — APIs de salida (documentación oficial)

- **Android** (`PlaybackParams`, API 23+, docs oficiales citadas):
  `AudioTrack.setPlaybackParams()` controla `speed` y `pitch` **en tiempo real**:
  - `pitch = 1.0` → cambio de velocidad **conservando tono** (timestretching del sistema).
  - `pitch = speed` → cambio por **resampleo** (tono sube con la velocidad), idéntico en
    efecto al campo de frecuencia de FLSM.
  → Ambos modos de la Fase 4 están soportados nativamente por Android sin generar archivos.
- **Web** (MDN, AudioBufferSourceNode): `loop`, `loopStart`, `loopEnd` (en segundos)
  permiten loop nativo; `playbackRate` (AudioParam, modificable en caliente) acelera
  **sin corrección de tono** (el tono escala). Para streaming se recomienda
  `AudioWorkletNode`. El modo "conservar tono" en web **no es nativo** para buffers:
  requeriría un estirador temporal propio en el worklet (futuro, solo si se decide).

---

## B. Comparativa técnica

### B.1 Opciones de aceleración

| Opción | Loop correcto | Cambio en vivo | Calidad | Coste CPU | Cobertura | Riesgo |
|---|---|---|---|---|---|---|
| 1. Cambio de frecuencia/avance (tono sube) | Sí: posiciones en muestras intactas | Sí (solo cambia el avance) | Idéntica al original (es re-muestreo del propio PCM) | Muy bajo | Android ✓ nativo (`pitch=speed`), Web ✓ nativo (`playbackRate`) | El tono sube (decisión de producto) |
| 2. Timestretch conservando tono | Sí en origen; cuidado: la ventana del estirador cruza la costura | Sí en Android (`pitch=1.0`), en web requiere DSP propio | Buena, pero con artefactos posibles cerca de la costura | Medio-alto | Android ✓ nativo, Web ✗ (implementación propia) | Calidad/latencia del estirador del sistema sin verificar en dispositivo |
| 3. Motor existente (MediaPlayer/ExoPlayer/ffmpeg) | N/A | N/A | N/A | N/A | **No decodifican BRSTM** → descartados como decodificador; sirven solo como referencia | No resuelven el formato |

Recomendación funcional (pendiente de aprobación): implementar primero la opción 1
(reproduce el comportamiento medido en las referencias), dejando la opción 2 como
conmutador posterior vía `pitch=1.0` en Android.

### B.2 Tecnologías

| Criterio | Kotlin + APIs nativas (AudioTrack) + núcleo C/C++ (NDK) | Web (JS + Web Audio + WASM) | Híbrido (Flutter/RN + motor nativo) |
|---|---|---|---|
| BRSTM | Parser+ADPCM propios o vgmstream vía NDK (licencia permisiva verificada) | Igual núcleo en WASM | Igual que Android, más puente FFI |
| Precisión de loop | Muestras exactas (índice entero + estado DSP de loop) | Exacta si el loop va en el worklet | Igual que Android |
| Velocidad en tiempo real | `setPlaybackParams` (ambos modos) | `playbackRate` (tono sube) + worklet | Depende del puente |
| Latencia/continuidad | AudioTrack streaming, buffers pequeños | AudioWorklet (quantums de 128) | Peor por puente de mensajes |
| Rendimiento/memoria | Núcleo nativo, decodificación por bloques (poca RAM) | WASM nativo, buffers en memoria o streaming | Igual, con sobrecarga del framework |
| Plataformas | Android ✓ | Navegadores ✓ | Android +iOS potencial |
| Complejidad/mantenimiento | 1 núcleo + 1 UI | 1 núcleo + UI web | 2 frameworks + puente |
| Licencias | vgmstream: permisiva (COPYING verificado); VGAudio: MIT (About.html de LAC); evitar LAC/BrawlLib sin revisar (LGPL/GPL indicados) | Mismas del núcleo | Depende de framework |

**Recomendación razonada (requiere aprobación según AGENTS.md)**: Kotlin + Android
nativo con **núcleo compartido en C/C++** (parser/decodificador DSP-ADPCM/StreamController)
compilado vía NDK para Android y vía WASM+Emscripten para la web, con AudioTrack en
Android y AudioWorklet en web. Motivos: el requisito dominante es control de muestras y
baja latencia, no la UI; React/RN/Flutter no aportan nada al motor de audio y añaden un
puente que perjudica el tiempo real; el núcleo C/C++ da código común real entre
plataformas. La UI puede ser Kotlin (Jetpack Compose) en Android y web plain después.
No se instala ni adopta nada todavía.

---

## C. Registro de evidencias

| # | Conclusión | Tipo |
|---|---|---|
| C1 | 301 BRSTM válidos; codec ADPCM 4b; rates 44100/48000/51269; 300 con loop, 1 sin loop | **Experimental** (`brstm_inventory.py`) |
| C2 | Offset exacto de loop flag/rate/loop_start/total (0x61/0x64/0x68/0x6C) | **Experimental** + **Documentación** (wiibrew) |
| C3 | La sección de loop es `[loop_start, total)`; no hay loop end propio | **Experimental** (vgmstream en 300 archivos) + docs |
| C4 | Par `_n`/_f` difiere solo en 2 bytes (frecuencia); PCM idéntico; +6.81% velocidad y tono | **Experimental** (`wav_compare.py`) |
| C5 | 39/40 pares `_f`/`_n` con ratio 1.068104 idéntico y mismo loop | **Experimental** |
| C6 | Loop muestramente exacto al reconcatenar; costura por debajo del ruido de la señal | **Experimental** (`loop_verify.py`) |
| C7 | Velocidad variable + wrap funciona sin artefactos evitables ni archivos intermedios | **Experimental** (`speed_loop_experiment.py`) |
| C8 | `loop_start` alineado a bloque en 300/300 (corpus, no garantía de formato) | **Experimental** |
| C9 | FLSM v2.0: sin opciones de velocidad; abrir→guardar; "speeded up version" | **Experimental** (UIA/textos) + **Documentación** (wiki oficial) |
| C10 | FLSM solo modifica el campo de frecuencia (2 bytes @0x64); PCM, loop y total intactos | **Experimental** (diff completo de 5_1_N/5_1_F + decodificación cruzada) |
| C11 | Android permite ambos modos (tono proporcional o conservado) en tiempo real | **Documentación** (PlaybackParams, API 23+) |
| C12 | Web: loop nativo sample-accurate + `playbackRate` con tono proporcional | **Documentación** (MDN) |
| C13 | Decodificar por bloques basta; no hace falta precodificar la canción | **Experimental** (formato + prototipo) |
| C14 | vgmstream licencia permisiva; VGAudio MIT; LAC frontend MIT (BrawlLib/GPL: revisar) | **Documentación local** (COPYING/About.html) |
| C15 | Factor de FLSM: **proporcional** ≈×1.06812 con truncado entero (44100→47104), no 51269 fijo | **Experimental** (medición T0 + colección); k exacto sin determinar (rango acotado) |
| C16 | Calidad/latencia real del timestretch del sistema en Android | **Pendiente de prueba en dispositivo** |

---

## D. Arquitectura propuesta (borrador, no adoptado)

```
[Archivo BRSTM]
   │  BrstmReader: valida magia/offsets, lee HEAD (loop flag, rate,
   │  loop_start, total, bloques), ADPC (historial), localiza DATA
   ▼
[DSP-ADPCM Decoder]  ← decodifica por bloques (0x2000 B/14336 muestras por canal)
   │  seek(n): decodifica desde el inicio de bloque y descarta prefijo
   │  restore_loop_state(): reinstaura predictor/escala/historial de loop de HEAD
   ▼
[StreamController]  ← posición (índice de muestras), wrap [loop_start, total),
   │                    factor de velocidad en caliente, modo (tono sube / conservado)
   ▼
[Búfer circular PCM16]  (por bloques; sin precodificar la canción)
   ▼
[Salida]  Android: AudioTrack streaming + setPlaybackParams(speed/pitch)
          Web: AudioWorklet (wrap y velocidad en el worklet) + playbackRate
          
[Interfaz]  Kotlin/Compose (Android) y web UI, hablando con el motor vía una
            API mínima: open(file), play/pause/stop, speed(x), state(), position()
```

Separación estricta: el núcleo (lector, decodificador, loop, velocidad) no conoce la
UI; la UI no toca muestras. El núcleo se compila para Android (NDK) y web (WASM).

---

## E. Plan de implementación (ordenado por riesgo)

**T0 — Experimento FLSM de referencia** ✔ **completado**
- Ejecutado sobre `5_1_N.brstm` (44100 Hz) → `5_1_F.brstm`; diff binario completo y
  decodificación cruzada con vgmstream (`flsm_compare.py`).
- Resultado: FLSM solo cambia el campo de frecuencia (0x64–0x65), PCM byte-idéntico,
  loop intacto, factor proporcional ≈×1.06812 (truncado). C10 y C15 cerrados.
- Método: conversión manual aprobada por el usuario (`flsm_drive_msg.ps1` /
  `flsm_t0.ps1` quedan como utilidad; la automatización del clic no abrió el diálogo y
  no se persigue más: el conocimiento ya está obtenido).

**T1 — Parser BRSTM propio + decodificador DSP-ADPCM** (riesgo técnico máximo)
- Objetivo: `parse → PCM16` por bloques, con seek y estado de loop.
- Dependencias: especificación wiibrew + pruebas de oro con vgmstream.
- Aceptación: PCM **byte-idéntico** al de vgmstream (`test.exe -i`) en ≥3 archivos
  (2ch/44100, 2ch/48000, 4ch o 8ch), y seek a `loop_start` igual que vgmstream.
- Pruebas: comparación byte a byte de WAV; casos sin loop y cabeceras corruptas.

**T2 — Motor de loop + velocidad sin salida de audio**
- Objetivo: StreamController con wrap y velocidad en caliente (modo tono proporcional).
- Dependencias: T1.
- Aceptación: la salida concatenada con `-l 2 -f 0` de vgmstream es byte-idéntica a la
  del motor a velocidad 1.0; a velocidad 1.3 la costura mantiene salto < p95 local;
  cambio de velocidad en caliente no desincroniza el loop.
- Pruebas: `loop_verify.py`-style sobre la salida del motor; métrica de costura.

**T3 — Corte vertical Android en dispositivo** (riesgo de integración)
- Objetivo: AudioTrack streaming desde el motor + `setPlaybackParams` con cambios de
  velocidad en vivo, loop continuo.
- Dependencias: T2, dispositivo/emulador, aprobación de arquitectura.
- Aceptación: 10 min de reproducción con 3 cambios de velocidad y ≥5 vueltas de loop sin
  underruns ni clics; latencia del cambio < 200 ms; documentar si `pitch=speed` suena
  como las referencias y si `pitch=1.0` es aceptable.
- Pruebas: grabación de salida, logs de AudioTrack, medición de CPU/batería.

**T4 — Robustez de formato**
- Objetivo: sin loop, offsets inválidos, 4/8 canales, rates mixtos, `loop_start` no
  alineado a bloque (archivo sintético), tamaño declarado ≠ real.
- Dependencias: T1. Aceptación: errores claros, nunca crash; reproducción correcta de
  los 301 archivos del corpus en modo headless (decodificación completa sin divergencias).

**T5 — Corte vertical web**
- Objetivo: núcleo en WASM + AudioWorklet con loop y `playbackRate` en caliente.
- Dependencias: T2, aprobación de arquitectura.
- Aceptación: Chrome y Firefox de escritorio, costura sin clics, cambio de velocidad en
  vivo, consumo razonable; iOS queda fuera del alcance inicial (marcar como límite).
- Pruebas: métricas de costura, prueba auditiva, prueba de memoria a 30 min.

**T6 — Modo conservar tono (opcional, tras validar producto)**
- Objetivo: conmutador tono proporcional/conservado (Android nativo; web con estirador
  propio o renunciar). Dependencias: T3. Aceptación: definir umbral de artefactos
  aceptable en la costura con escucha comparativa.

**T7 — Rendimiento y estabilidad**: decodificación > tiempo real sostenida, memoria
acotada, reproducción prolongada. **T8 — Interfaz** (fuera del alcance de esta tarea).

---

## F. Actualización de memoria

Ver `MEMORY.md` (estado, decisiones pendientes, límites y siguiente paso).
