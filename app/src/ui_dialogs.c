/* BRLoop desktop prototype (T2): modal dialogs and popup menus.
 *
 * They wait for user interaction (open-file dialog, language/theme menus)
 * and block the selftest: unreachable without a human click. Their own file
 * can be excluded from coverage by path instead of excluding main.c or
 * ui_library.c wholesale. No behaviour changes: moved verbatim.
 */
#include "ui_dialogs.h"

#include <commctrl.h>
#include <commdlg.h>

#include <string.h>
#include <wchar.h>

#include "ui_library.h"
#include "strings.h"

/* ------------------------------------------------ open file dialog -- */

void browse_files(void)
{
    static wchar_t buf[65536];
    static wchar_t dir[33000];
    static wchar_t full[33000];
    OPENFILENAMEW ofn;
    wchar_t err[1024], msg[1400], sum[128];
    int added = 0, failed = 0;

    memset(&ofn, 0, sizeof ofn);
    memset(buf, 0, sizeof buf);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    /* File-dialog filter: each name is followed by its pattern, and the list
     * ends with a double NUL, so the pieces are joined by hand. */
    {
        wchar_t filter[512];
        size_t fi = 0;
        const wchar_t *parts[4] = { tr(STR_FILTER_BRSTM), L"*.brstm", tr(STR_FILTER_ALL), L"*.*" };
        size_t need = 1; /* final NUL that terminates the filter list */
        int pi;
        for (pi = 0; pi < 4; pi++)
            need += wcslen(parts[pi]) + 1; /* each part plus its own NUL */
        /* Copy each filter part (plus its NUL) with an explicit bound, so the
         * analyzer sees the copy stays inside the 512-wchar buffer; too long
         * translations fall back to no filter instead of writing out of bounds. */
        if (need <= 512) {
            for (pi = 0; pi < 4; pi++) {
                size_t len = wcslen(parts[pi]);
                wcsncpy_s(filter + fi, 512 - fi, parts[pi], len + 1);
                fi += len + 1;
            }
            filter[fi] = L'\0';
            ofn.lpstrFilter = filter;
        } else {
            ofn.lpstrFilter = NULL;
        }
    }
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER
              | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn))
        return;

    msg[0] = L'\0';
    wchar_t *p = buf;
    if (p[0] && p[wcslen(p) + 1] != L'\0') {
        wcscpy_s(dir, 33000, p);
        p += wcslen(p) + 1;
        while (*p) {
            swprintf(full, 33000, L"%s\\%s", dir, p);
            if (add_track(full, err, 1024) == 0)
                added++;
            else {
                if (failed == 0)
                    wcscpy_s(msg, 1400, err);
                failed++;
            }
            p += wcslen(p) + 1;
        }
    } else {
        if (add_track(buf, err, 1024) == 0)
            added++;
        else {
            wcscpy_s(msg, 1400, err);
            failed++;
        }
    }

    if (failed > 0) {
        wchar_t box[2048];
        if (failed > 1)
            swprintf(box, 2048, tr(STR_INVALID_FILES_MORE), msg, failed - 1);
        else
            swprintf(box, 2048, L"%s", msg);
        MessageBoxW(g_hwnd, box, tr(STR_INVALID_FILE_TITLE),
                    MB_OK | MB_ICONWARNING);
    }
    if (added > 0 && g_library.selected_index < 0 && g_library.count > 0)
        ListView_SetItemState(g_controls.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    if (added > 0) {
        swprintf(sum, 128, tr(STR_LIST_COUNT), g_library.count);
        set_status(sum);
    }
}

