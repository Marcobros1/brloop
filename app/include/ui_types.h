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
    HWND btnPlay, btnPrevious, btnNext;
    HWND btnSpeed, btnPair;
    HWND checkLoop, progress;
    HWND capName, capCh, capRate, capDur, capSamples, capLoop, capLoopSt;
    HWND capLoopEnd;
    HWND capPath, capVer;
    HWND valName, valPath, valRate, valCh, valSamples, valDur;
    HWND valLoop, valLoopSt, valState, valTime, valSpeed, valDevRate;
    HWND valLoopEnd;
} Controls;

#endif /* UI_TYPES_H */
