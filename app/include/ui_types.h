#ifndef UI_TYPES_H
#define UI_TYPES_H

#include <windows.h>
#include <stdint.h>

/* This header is the shared vocabulary between the Win32 window and the UI
 * selftest. Control IDs are kept here so a test can address the same commands
 * as a real click without importing main.c's implementation. */
#define IDC_LIST          1001
#define IDC_BTN_OPEN      1002
#define IDC_BTN_REMOVE    1003
#define IDC_BTN_CLEAR     1004
#define IDC_BTN_PLAY      1005
#define IDC_BTN_PREVIOUS  1006
#define IDC_BTN_NEXT      1007
#define IDC_BTN_LANG      1009
#define IDC_CHECK_LOOP    1008
#define IDC_BTN_SPEED     1010
#define IDC_BTN_PAIR      1012
#define IDC_PROGRESS      1014
#define IDC_VAL_STATE     1015
#define IDC_VAL_TIME      1016
#define IDC_VAL_SPEED     1017
#define IDC_VAL_DEVRATE   1018
#define IDC_VAL_NAME      1020
#define IDC_VAL_PATH      1021
#define IDC_VAL_RATE      1022
#define IDC_VAL_CH        1023
#define IDC_VAL_SAMPLES   1024
#define IDC_VAL_DUR       1025
#define IDC_VAL_LOOP      1026
#define IDC_VAL_LOOPST    1027
#define IDC_CAP_LOOPEND   1028
#define IDC_VAL_LOOPEND   1029
#define IDC_CAP_NAME      1030
#define IDC_CAP_CH        1031
#define IDC_CAP_RATE      1032
#define IDC_CAP_DUR       1033
#define IDC_CAP_SAMPLES   1034
#define IDC_CAP_LOOP      1035
#define IDC_CAP_LOOPST    1036
#define IDC_CAP_PATH      1037
#define IDC_CAP_VER       1038
#define IDM_LANG_EN       1050
#define IDM_LANG_ES       1051
#define IDC_BTN_THEME     1011
#define IDM_THEME_SYSTEM  1060
#define IDM_THEME_LIGHT   1061
#define IDM_THEME_DARK    1062

#define TIMER_UI 1
#define UI_TICK_MS 100
#define PROGRESS_CLASS_NAME L"MkwProgress"

/* A library row owns both heap strings. Numeric fields are metadata copied
 * from the validated BRSTM and are not pointers into the file. */
typedef struct {
    wchar_t *path;
    wchar_t *name;
    uint32_t rate;
    uint8_t  channels;
    uint32_t total;
    uint8_t  loop;
    uint32_t loop_start;
} TrackRow;

/* UI-thread-owned library state. `rows[0..count)` are initialized; the
 * remaining capacity is reserved for future additions. selected_index is -1
 * when no row is selected and otherwise always falls within that range. */
typedef struct {
    TrackRow *rows;
    int count;
    int capacity;
    int selected_index;
} LibraryState;

/* Handles are created once during WM_CREATE and remain valid until WM_DESTROY.
 * The struct stores no ownership of the HWND objects themselves. */
typedef struct {
    HWND list, status;
    HWND groupLib, groupInfo, groupPlay, groupSpeed;
    HWND btnOpen, btnRemove, btnClear;
    HWND btnPlay, btnPrevious, btnNext, btnLang, btnTheme;
    HWND btnSpeed, btnPair;
    HWND checkLoop, progress;
    HWND capName, capCh, capRate, capDur, capSamples, capLoop, capLoopSt;
    HWND capLoopEnd;
    HWND capPath, capVer;
    HWND valName, valPath, valRate, valCh, valSamples, valDur;
    HWND valLoop, valLoopSt, valState, valTime, valSpeed, valDevRate;
    HWND valLoopEnd;
} Controls;

/* Sepia palette (option C). Normal text is at least 4.5:1 on its panel colour. */

/* Light theme */
#define UI_THEME_LIGHT_FONDO      RGB(0xFD,0xF6,0xEE)
#define UI_THEME_LIGHT_SUPERFICIE RGB(0xF3,0xE6,0xD4)
#define UI_THEME_LIGHT_PRIMARIO   RGB(0x3E,0x2A,0x20)
#define UI_THEME_LIGHT_SECUNDARIO RGB(0x7A,0x5E,0x4F)
#define UI_THEME_LIGHT_ACENTO     RGB(0xA6,0x3A,0x00)
#define UI_THEME_LIGHT_BORDE      RGB(0xD8,0xC3,0xA8)

/* Dark theme */
#define UI_THEME_DARK_FONDO       RGB(0x2B,0x1E,0x18)
#define UI_THEME_DARK_SUPERFICIE  RGB(0x3A,0x28,0x20)
#define UI_THEME_DARK_PRIMARIO    RGB(0xF3,0xE6,0xD4)
#define UI_THEME_DARK_SECUNDARIO  RGB(0xC9,0xA6,0x87)
#define UI_THEME_DARK_ACENTO      RGB(0xDD,0x79,0x35)
#define UI_THEME_DARK_BORDE       RGB(0x4A,0x39,0x2F)

/* Status text colours, each readable on its theme's panel colour. */
#define UI_STATE_LIGHT_LOADING   RGB(0x5C,0x4A,0x3F)
#define UI_STATE_LIGHT_PLAYING   RGB(0x2E,0x6B,0x2E)
#define UI_STATE_LIGHT_PAUSED    RGB(0x8A,0x4A,0x00)
#define UI_STATE_LIGHT_FINISHED  RGB(0x1F,0x4E,0x8C)
#define UI_STATE_DARK_LOADING    RGB(0xD3,0xC2,0xAE)
#define UI_STATE_DARK_PLAYING    RGB(0x9F,0xD2,0x9F)
#define UI_STATE_DARK_PAUSED     RGB(0xF2,0xB3,0x6B)
#define UI_STATE_DARK_FINISHED   RGB(0xA9,0xC8,0xF0)

/* Active theme (g_ui_theme_dark is defined in main.c). */
#define UI_C_FONDO      (g_ui_theme_dark ? UI_THEME_DARK_FONDO      : UI_THEME_LIGHT_FONDO)
#define UI_C_SUPERFICIE (g_ui_theme_dark ? UI_THEME_DARK_SUPERFICIE : UI_THEME_LIGHT_SUPERFICIE)
#define UI_C_PRIMARIO   (g_ui_theme_dark ? UI_THEME_DARK_PRIMARIO   : UI_THEME_LIGHT_PRIMARIO)
#define UI_C_SECUNDARIO (g_ui_theme_dark ? UI_THEME_DARK_SECUNDARIO : UI_THEME_LIGHT_SECUNDARIO)
#define UI_C_ACENTO     (g_ui_theme_dark ? UI_THEME_DARK_ACENTO     : UI_THEME_LIGHT_ACENTO)
#define UI_C_BORDE      (g_ui_theme_dark ? UI_THEME_DARK_BORDE      : UI_THEME_LIGHT_BORDE)

#endif /* UI_TYPES_H */
