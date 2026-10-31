#ifndef STRINGS_H
#define STRINGS_H

#include <wchar.h>

/* User-visible text lookup. Every label, caption and message in the window is
 * requested by key, so the language can change without touching the layout
 * or the self-test.
 *
 * The keys, the Spanish text and the English text are defined in ONE list
 * (STRING_TABLE below). The enum and both tables are generated from that list,
 * so a key can never drift away from its texts. To add a message, add one
 * line here. Template entries use swprintf placeholders filled by the caller. */
typedef enum {
    STR_LANG_EN = 0,
    STR_LANG_ES = 1
} StrLanguage;

#define STRING_TABLE(X) \
    X(STR_LIBRARY_GROUP,        L"Biblioteca",                 L"Library") \
    X(STR_BUTTON_OPEN,          L"Abrir…",                     L"Open…") \
    X(STR_BUTTON_REMOVE,        L"Quitar",                     L"Remove") \
    X(STR_BUTTON_CLEAR,         L"Limpiar",                    L"Clear") \
    X(STR_COL_NAME,             L"Nombre",                     L"Name") \
    X(STR_COL_RATE,             L"Hz",                         L"Hz") \
    X(STR_COL_CHANNELS,         L"Canales",                    L"Channels") \
    X(STR_COL_DURATION,         L"Duración",                   L"Duration") \
    X(STR_COL_LOOP,             L"Loop",                       L"Loop") \
    X(STR_FILE_INFO_GROUP,      L"Información del archivo",    L"File information") \
    X(STR_CAP_NAME,             L"Nombre:",                    L"Name:") \
    X(STR_CAP_CHANNELS,         L"Canales:",                   L"Channels:") \
    X(STR_CAP_RATE,             L"Frecuencia:",                L"Sample rate:") \
    X(STR_CAP_DURATION,         L"Duración:",                  L"Duration:") \
    X(STR_CAP_SAMPLES,          L"Muestras:",                  L"Samples:") \
    X(STR_CAP_LOOP,             L"Loop:",                      L"Loop:") \
    X(STR_CAP_LOOP_START,       L"Inicio de loop:",            L"Loop start:") \
    X(STR_CAP_LOOP_END,         L"Fin de loop:",               L"Loop end:") \
    X(STR_CAP_PATH,             L"Ruta:",                      L"Path:") \
    X(STR_CAP_VERSION,          L"Versión:",                   L"Version:") \
    X(STR_PLAY_GROUP,           L"Reproducción",               L"Playback") \
    X(STR_BUTTON_PLAY,          L"Reproducir",                 L"Play") \
    X(STR_BUTTON_PAUSE,         L"Pausar",                     L"Pause") \
    X(STR_BUTTON_RESUME,        L"Reanudar",                   L"Resume") \
    X(STR_BUTTON_REPLAY,        L"Volver a reproducir",        L"Play again") \
    X(STR_BUTTON_PREVIOUS,      L"Anterior",                   L"Previous") \
    X(STR_BUTTON_NEXT,          L"Siguiente",                  L"Next") \
    X(STR_CHECK_LOOP,           L"Repetir loop",               L"Repeat loop") \
    X(STR_SPEED_GROUP,          L"Velocidad",                  L"Speed") \
    X(STR_SPEED_FLSM,           L"Final Lap ×1,06812",         L"Final Lap ×1,06812") \
    X(STR_SPEED_NORMAL,         L"1,0×",                       L"1.0×") \
    X(STR_STATE_STOPPED,        L"Estado: Detenido",           L"Status: Stopped") \
    X(STR_STATE_PLAYING,        L"Estado: Reproduciendo",      L"Status: Playing") \
    X(STR_STATE_PAUSED,         L"Estado: Pausado",            L"Status: Paused") \
    X(STR_STATE_FINISHED,       L"Estado: Terminado",          L"Status: Finished") \
    X(STR_STATUS_READY,         L"Listo.",                     L"Ready.") \
    X(STR_LOOP_YES,             L"Sí",                         L"Yes") \
    X(STR_LOOP_NO,              L"No",                         L"No") \
    X(STR_LOOP_UNAVAILABLE,     L"No disponible",              L"Not available") \
    X(STR_VALUE_NONE,           L"—",                          L"—") \
    X(STR_VERSION_STEREO,       L"Estéreo",                    L"Stereo") \
    X(STR_VERSION_CAPTION,      L"Versión %d · canales %d-%d", L"Version %d · channels %d-%d") \
    X(STR_NO_TRACK_VALUE,       L"—",                          L"—") \
    X(STR_TRACK_OPENED,         L"Archivo abierto: %s",        L"File opened: %s") \
    X(STR_TRACK_OPEN_FAILED,    L"No se pudo abrir «%s»: %s",  L"Could not open «%s»: %s") \
    X(STR_PATH_NOT_ANSI,        L"La ruta no es representable en la codificación ANSI del sistema.", L"The path cannot be represented in the system ANSI code page.") \
    X(STR_PATH_NOT_ANSI_NAMED,  L"«%s»: ruta no representable en ANSI", L"«%s»: path cannot be represented in ANSI") \
    X(STR_TRACK_INVALID_NAMED,  L"«%s»: %s",                   L"«%s»: %s") \
    X(STR_TRACK_NO_MEMORY,      L"«%s»: sin memoria",          L"«%s»: out of memory") \
    X(STR_LIST_EMPTY,           L"Lista vacía.",               L"List is empty.") \
    X(STR_LIST_COUNT,           L"%d archivo(s) en la lista.", L"%d file(s) in the list.") \
    X(STR_DEVICE_RATE,          L"Dispositivo: %u Hz",         L"Device: %u Hz") \
    X(STR_DEVICE_RATE_STEREO,   L"Dispositivo: %u Hz · estéreo", L"Device: %u Hz · stereo") \
    X(STR_DEVICE_RATE_EMPTY,    L"Dispositivo: —",             L"Device: —") \
    X(STR_TIME_SEPARATOR,       L":",                          L":") \
    X(STR_LOOP_END_POSITION,    L"%llu (fin del archivo)",     L"%llu (end of file)") \
    X(STR_LOOP_POSITION,        L"%u (%s)",                    L"%u (%s)") \
    X(STR_STATE_DECODE_ERROR,   L"Error de decodificación; reproducción detenida.", L"Decoding error; playback stopped.") \
    X(STR_AUDIO_OUTPUT_ERROR,   L"Error de salida de audio: %s", L"Audio output error: %s") \
    X(STR_RESUMING,             L"Reanudando…",                L"Resuming…") \
    X(STR_RESTARTING,           L"Volviendo al inicio de la canción.", L"Going back to the start of the song.") \
    X(STR_PLAYING_FROM_START,   L"Reproduciendo desde el inicio…", L"Playing from the start…") \
    X(STR_PLAY_NO_AUDIO,        L"No se puede reproducir (sin salida de audio).", L"Cannot play (no audio output).") \
    X(STR_PAUSED,               L"Pausado.",                   L"Paused.") \
    X(STR_PLAYING,              L"Reproduciendo…",             L"Playing…") \
    X(STR_PLAY_NO_FILE,         L"No se puede reproducir (sin archivo o sin salida de audio).", L"Cannot play (no file or no audio output).") \
    X(STR_BACK_TO_START,        L"Volviendo al inicio de la canción.", L"Going back to the start of the song.") \
    X(STR_PREVIOUS_TRACK,       L"Canción anterior.",          L"Previous track.") \
    X(STR_NEXT_TRACK,           L"Siguiente canción.",         L"Next track.") \
    X(STR_VERSION_SELECTED,     L"Versión %d: canales %d-%d.", L"Version %d: channels %d-%d.") \
    X(STR_FILTER_BRSTM,         L"Archivos BRSTM (*.brstm)",   L"BRSTM files (*.brstm)") \
    X(STR_FILTER_ALL,           L"Todos los archivos (*.*)",   L"All files (*.*)") \
    X(STR_INVALID_FILE_TITLE,   L"Archivo no válido o ilegible", L"Invalid or unreadable file") \
    X(STR_INVALID_FILES_MORE,   L"%s\n(y otros %d archivos no válidos)", L"%s\n(and %d other invalid files)") \
    X(STR_FATAL_ERROR_TITLE,    L"Error fatal",                L"Fatal error") \
    X(STR_ENGINE_START_FAILED,  L"No se pudo iniciar el motor de reproducción.", L"Could not start the playback engine.") \
    X(STR_WINDOW_TITLE,         L"BRLoop — Reproductor BRSTM", L"BRLoop — BRSTM Player") \
    X(STR_MENU_LANGUAGE,        L"Idioma",                     L"Language") \
    X(STR_MENU_ENGLISH,         L"English",                    L"English") \
    X(STR_MENU_SPANISH,         L"Español",                    L"Español") \
    X(STR_SETTINGS_SAVE_FAILED, L"No se pudo guardar el idioma.", L"Could not save the language.") \
    X(STR_SETTINGS_INVALID,     L"El archivo de configuración no es válido; se usa inglés.", L"The settings file is invalid; using English.") \
    X(STR_TRACK_ALREADY_ADDED,  L"«%s» ya está en la lista.", L"\"%s\" is already in the list.") \
    X(STR_FONT_MISSING,         L"No se encontró la fuente de la interfaz; se usa la del sistema.", L"The interface font was not found; using the system font.")

#define STRING_KEY_ENUM(key, es, en) key,
typedef enum {
    STRING_TABLE(STRING_KEY_ENUM)
    STR_KEY_COUNT
} StrKey;
#undef STRING_KEY_ENUM

void strings_set_language(StrLanguage language);
StrLanguage strings_language(void);

/* Returns the text for the active language. Never returns NULL. */
const wchar_t *tr(StrKey key);

#endif /* STRINGS_H */
