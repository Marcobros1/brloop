#include "ui_layout.h"

#include <windows.h>
#include <commctrl.h>

#include "strings.h"

/* Widest state-text pixel width (all states, both languages), measured
 * with g_font. The box keeps its size across language changes because
 * set_language() does not re-run layout. */
int measure_state_text_width(void)
{
    static const StrKey stateKeys[] = {
        STR_STATE_STOPPED, STR_STATE_PLAYING, STR_STATE_PAUSED, STR_STATE_FINISHED
    };
    StrLanguage saved = strings_language();
    StrLanguage langs[2] = { STR_LANG_ES, STR_LANG_EN };
    HDC hdc;
    HFONT old;
    int widest = 0, li, ki;

    if (!g_font)
        return 0;
    hdc = GetDC(NULL);
    old = (HFONT)SelectObject(hdc, g_font);
    for (li = 0; li < 2; li++) {
        strings_set_language(langs[li]);
        for (ki = 0; ki < (int)(sizeof stateKeys / sizeof stateKeys[0]); ki++) {
            SIZE sz;
            const wchar_t *text = tr(stateKeys[ki]);
            if (GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz) && sz.cx > widest)
                widest = sz.cx;
        }
    }
    strings_set_language(saved);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
    return widest;
}

/* Writes g_nameFull into the banner box. If it does not fit on one line at the
 * box width, it is cut and ends in "..." (Q-088). Measured with the real font. */
void name_fit_to_box(void)
{
    static const wchar_t ellipsis[] = L"...";
    HWND ctl = g_controls.valName;
    RECT box;
    HDC dc;
    HGDIOBJ oldFont;
    SIZE sz;
    size_t len, lo, hi;
    wchar_t buf[512];

    if (!ctl)
        return;
    len = wcslen(g_nameFull);
    if (!GetClientRect(ctl, &box) || box.right <= 0) {
        SetWindowTextW(ctl, g_nameFull);
        return;
    }
    dc = GetDC(ctl);
    oldFont = SelectObject(dc, (HGDIOBJ)g_font);
    GetTextExtentPoint32W(dc, g_nameFull, (int)len, &sz);
    if (sz.cx <= box.right) {
        SelectObject(dc, oldFont);
        ReleaseDC(ctl, dc);
        SetWindowTextW(ctl, g_nameFull);
        return;
    }
    /* Binary search for the longest prefix whose text plus "..." fits. */
    lo = 0;
    hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        memcpy(buf, g_nameFull, mid * sizeof(wchar_t));
        memcpy(buf + mid, ellipsis, sizeof ellipsis);
        GetTextExtentPoint32W(dc, buf, (int)(mid + 3), &sz);
        if (sz.cx <= box.right)
            lo = mid;
        else
            hi = mid - 1;
    }
    memcpy(buf, g_nameFull, lo * sizeof(wchar_t));
    memcpy(buf + lo, ellipsis, sizeof ellipsis);
    SelectObject(dc, oldFont);
    ReleaseDC(ctl, dc);
    SetWindowTextW(ctl, buf);
}

void set_name_fitted(const wchar_t *name)
{
    wcsncpy(g_nameFull, name ? name : L"", sizeof g_nameFull / sizeof g_nameFull[0] - 1);
    g_nameFull[sizeof g_nameFull / sizeof g_nameFull[0] - 1] = L'\0';
    name_fit_to_box();
}

/* Recompute every control position from the client size. Called on
 * WM_SIZE and once after creation. Fixed pixel geometry (no auto-scale):
 * the window enforces a 760x560 minimum in WM_GETMINMAXINFO, and the
 * layout is simple enough that proportional scaling would add complexity
 * without visible benefit at this stage. Left column = library (width
 * clamped 300..440), right column = info / transport / speed panels. */
void layout(void)
{
    RECT rc;
    int cw, ch;
    int m = 12, g = 10, statusH = 28, modeBarH = 30, playH = 134, gBottom = 18;
    int leftW, rightX, rightW, topH, contentY;
    int bannerH = 116;
    int x0, col2, y, gy, sy;

    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < 100 || ch < 100)
        return;

    SendMessageW(g_controls.status, WM_SIZE, 0, 0);
    /* statusH matches the control's real height (28 px, normal font). */
    move_ctl(g_controls.status, 0, ch - statusH, cw, statusH);
    /* Status text cut at the edge, no "...", and not under the size grip
     * (SBARS_SIZEGRIP, ~16 px right). */
    {
        int statusPart = cw - 16;
        if (g_controls.status && statusPart > 0)
            SendMessageW(g_controls.status, SB_SETPARTS, 1, (LPARAM)&statusPart);
    }

    /* Mode bar across the top, full width (A2). The two buttons share
     * the width; the active one is painted with color and border (Q-078). */
    {
        int mw = (cw - 2 * m - g) / 2;
        int mx = m;
        move_ctl(g_controls.btnModeLibrary, mx, m, mw, modeBarH);
        mx += mw + g;
        move_ctl(g_controls.btnModeSettings, mx, m, mw, modeBarH);
    }

    contentY = m + modeBarH + g;

    leftW = cw * 46 / 100;
    if (leftW < 300) leftW = 300;
    if (leftW > 440) leftW = 440;
    rightX = m + leftW + g;
    rightW = cw - rightX - m;
    x0 = rightX + 12;
    col2 = rightX + rightW / 2 + 2;

    /* gBottom: real gap between the left/right column and the playback box. */
    topH = ch - statusH - 2 * m - playH - modeBarH - g - gBottom;

    /* Central block: only the active mode is shown. */
    if (g_active_mode == 0) { /* Library: list + banner card (B1) */
        /* Library (left column). */
        move_ctl(g_controls.groupLib, m, contentY, leftW, topH);
        move_ctl(g_controls.btnOpen, m + 12, contentY + 22, 96, 26);
        move_ctl(g_controls.btnRemove, m + 116, contentY + 22, 84, 26);
        move_ctl(g_controls.btnClear, m + 208, contentY + 22, 84, 26);
        move_ctl(g_controls.list, m + 12, contentY + 56, leftW - 24, topH - 56 - 12);

        /* Banner card (right column): name, path below, grid of 4 data
         * fields (Hz, Channels, Duration, Loop). Rows of one normal line
         * of text (16 px). */
        move_ctl(g_controls.groupInfo, rightX, contentY, rightW, bannerH);
        /* Name and path move down to leave the group title free (top). */
        move_ctl(g_controls.valName, rightX + 12, contentY + 22, rightW - 24, 22);
        name_fit_to_box();
        move_ctl(g_controls.valPath, rightX + 12, contentY + 46, rightW - 24, 22);
        gy = contentY + 72;
        move_ctl(g_controls.capRate, rightX + 12, gy, 84, 18);
        move_ctl(g_controls.valRate, rightX + 12 + 88, gy, rightW / 2 - 104, 18);
        move_ctl(g_controls.capCh, col2, gy, 74, 18);
        move_ctl(g_controls.valCh, col2 + 78, gy, rightW - (col2 - rightX) - 90, 18);
        gy += 22;
        move_ctl(g_controls.capDur, rightX + 12, gy, 84, 18);
        move_ctl(g_controls.valDur, rightX + 12 + 88, gy, rightW / 2 - 104, 18);
        move_ctl(g_controls.capLoop, col2, gy, 74, 18);
        move_ctl(g_controls.valLoop, col2 + 78, gy, rightW - (col2 - rightX) - 90, 18);

        /* Speed below the card, with the version button right under it. */
        sy = contentY + bannerH + g;
        move_ctl(g_controls.groupSpeed, rightX, sy, rightW, topH - bannerH - g);
        move_ctl(g_controls.valSpeed, rightX + 12, sy + 28, 84, 20);
        move_ctl(g_controls.valDevRate, rightX + rightW - 12 - 170, sy + 28, 170, 20);
        move_ctl(g_controls.btnSpeed, rightX + (rightW - 200) / 2, sy + 60, 200, 26);
        move_ctl(g_controls.btnPair, rightX + (rightW - 200) / 2, sy + 60 + 26 + 8, 200, 26);
        move_ctl(g_controls.capVer, rightX + 12, sy + 60 + 26 + 8 + 26 + 8, rightW - 24, 18);
    }
    /* Settings (mode 2): language and theme stay visible in the bottom bar;
     * there is no extra central content. */

    /* Bottom bar, always visible: transport controls, progress, state,
     * and system message to its right. */
    y = ch - statusH - m - playH;
    move_ctl(g_controls.groupPlay, m, y, cw - 2 * m, playH);
    /* Previous, play and next form a row (84 + 12 + 104 + 12 + 96 = 308 px),
     * centered in the box. */
    x0 = m + ((cw - 2 * m) - 308) / 2;
    move_ctl(g_controls.btnPrevious, x0, y + 22, 84, 28);
    move_ctl(g_controls.btnPlay, x0 + 96, y + 22, 104, 28);
    move_ctl(g_controls.btnNext, x0 + 212, y + 22, 96, 28);
    /* Language/theme column to the right; everything else in the bottom bar
     * ends before its left edge (no overlap). */
    move_ctl(g_controls.btnLang, cw - m - 12 - 150, y + 62, 150, 26);
    move_ctl(g_controls.btnTheme, cw - m - 12 - 150, y + 98, 150, 26);
    {
        int leftX = m + 12;
        int rightEdge = cw - m - 12 - 150 - 12;
        int stateW = measure_state_text_width() + 16;
        int sysX = leftX + 180 + stateW + 12;
        int sysW = rightEdge - sysX;
        if (sysW < 0)
            sysW = 0;
        /* Progress: its own row, up to the language/theme column. */
        move_ctl(g_controls.progress, leftX, y + 56, rightEdge - leftX, 16);
        /* Time below the progress bar (as before the change). */
        move_ctl(g_controls.valTime, leftX, y + 78, 170, 18);
        /* State to the right of time, same row, box sized to its text. */
        move_ctl(g_controls.valState, leftX + 180, y + 78, stateW, 20);
        /* System message to the right of state; hidden when there is no text. */
        move_ctl(g_controls.valSystem, sysX, y + 78, sysW, 20);
        /* Bottom row: loop. */
        move_ctl(g_controls.checkLoop, leftX, y + 106, 170, 20);
    }
}
