#include "ui_layout.h"

#include <windows.h>
#include <commctrl.h>

#include "strings.h"

/* DPI of the window and the text line height of g_font, set once by
 * layout_init(). Every size in this file goes through scale() or text_h(). */
static UINT g_layout_dpi = 96;
static int g_line_h = 20;

/* Design pixels (sized at 96 DPI) to pixels at the window DPI. */
static int scale(int v)
{
    return MulDiv(v, (int)g_layout_dpi, 96);
}

/* Height of one text row: the real line height of g_font plus `extra`
 * design pixels for padding or a frame. */
static int text_h(int extra)
{
    return g_line_h + scale(extra);
}

/* Reads the DPI and the line height of g_font. Called from WM_CREATE after
 * the font exists and before any control is positioned. */
void layout_init(UINT dpi)
{
    HDC dc;
    HGDIOBJ old;
    TEXTMETRICW tm;

    g_layout_dpi = dpi ? dpi : 96;
    if (!g_font) {
        g_line_h = scale(20);
        return;
    }
    dc = GetDC(NULL);
    old = SelectObject(dc, (HGDIOBJ)g_font);
    if (GetTextMetricsW(dc, &tm))
        g_line_h = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
}

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
 * WM_SIZE and once after creation. Every size is scaled by the window DPI
 * (scale), and text rows take their height from the font (text_h). The
 * window enforces a 760x570 minimum at the same DPI in WM_GETMINMAXINFO.
 * Left column = library (60 % of the usable width), right column = info
 * card (fills the free height) over the speed box (content height).
 * Settings mode centres its block in the whole central panel. */
void layout(void)
{
    RECT rc;
    int cw, ch;
    int m = scale(12), g = scale(10), gBottom = scale(18), pad = scale(12);
    int lineH = text_h(0);     /* one line of text */
    int btnH = text_h(6);      /* text push button */
    int edH = text_h(2);       /* read-only edit */
    int statusH = text_h(8);
    int modeBarH = text_h(10);
    int playH = scale(134);
    int leftW, rightX, rightW, topH, contentY;
    int bannerH, speedH;
    int x0, col2, y, gy, sy;
    int prevW = scale(84), playW = scale(104), nextW = scale(96), btnGap = scale(12);

    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < scale(100) || ch < scale(100))
        return;

    SendMessageW(g_controls.status, WM_SIZE, 0, 0);
    move_ctl(g_controls.status, 0, ch - statusH, cw, statusH);
    /* Status text cut at the edge, no "...", and not under the size grip
     * (SBARS_SIZEGRIP, ~16 px right at 96 DPI). */
    {
        int statusPart = cw - scale(16);
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

    /* Library takes 60 % of the usable width; the right column keeps at
     * least 340 design px and has no upper limit for the library (Q-144). */
    leftW = (cw - 2 * m - g) * 60 / 100;
    if (cw - 2 * m - g - leftW < scale(340))
        leftW = cw - 2 * m - g - scale(340);
    rightX = m + leftW + g;
    rightW = cw - rightX - m;
    x0 = rightX + pad;
    col2 = rightX + rightW / 2 + scale(2);

    /* gBottom: real gap between the left/right column and the playback box. */
    topH = ch - statusH - 2 * m - playH - modeBarH - g - gBottom;

    /* Speed box has the height of its content (title, label, button).
     * The banner card takes the rest of the column (Q-145). */
    /* Room for the speed button, the version button and its label. */
    speedH = scale(60) + btnH + scale(8) + btnH + scale(8) + text_h(1) + scale(14);
    bannerH = topH - speedH - g;

    /* Central block: only the active mode is shown. */
    if (g_active_mode == 0) { /* Library: list + banner card (B1) */
        /* Library (left column). */
        move_ctl(g_controls.groupLib, m, contentY, leftW, topH);
        move_ctl(g_controls.btnOpen, m + pad, contentY + scale(22), scale(96), btnH);
        move_ctl(g_controls.btnRemove, m + scale(116), contentY + scale(22), scale(84), btnH);
        move_ctl(g_controls.btnClear, m + scale(208), contentY + scale(22), scale(84), btnH);
        move_ctl(g_controls.list, m + pad, contentY + scale(56), leftW - scale(24),
                 topH - scale(56) - pad);

        /* Banner card (right column): name, path below, grid of 4 data
         * fields (Hz, Channels, Duration, Loop). Each row is one line of
         * text, so its height comes from the font. */
        move_ctl(g_controls.groupInfo, rightX, contentY, rightW, bannerH);
        /* Name and path move down to leave the group title free (top). */
        move_ctl(g_controls.valName, rightX + pad, contentY + scale(22), rightW - scale(24), text_h(2));
        name_fit_to_box();
        move_ctl(g_controls.valPath, rightX + pad, contentY + scale(46), rightW - scale(24), edH);
        gy = contentY + scale(72);
        move_ctl(g_controls.capRate, rightX + pad, gy, scale(84), lineH);
        move_ctl(g_controls.valRate, rightX + pad + scale(88), gy, rightW / 2 - scale(104), lineH);
        move_ctl(g_controls.capCh, col2, gy, scale(74), lineH);
        move_ctl(g_controls.valCh, col2 + scale(78), gy, rightW - (col2 - rightX) - scale(90), lineH);
        gy += scale(22);
        move_ctl(g_controls.capDur, rightX + pad, gy, scale(84), lineH);
        move_ctl(g_controls.valDur, rightX + pad + scale(88), gy, rightW / 2 - scale(104), lineH);
        move_ctl(g_controls.capLoop, col2, gy, scale(74), lineH);
        move_ctl(g_controls.valLoop, col2 + scale(78), gy, rightW - (col2 - rightX) - scale(90), lineH);

        /* Speed below the card, with the version button right under it. */
        sy = contentY + bannerH + g;
        move_ctl(g_controls.groupSpeed, rightX, sy, rightW, speedH);
        move_ctl(g_controls.valSpeed, rightX + pad, sy + scale(28), scale(84), lineH);
        move_ctl(g_controls.valDevRate, rightX + rightW - pad - scale(170), sy + scale(28), scale(170), lineH);
        move_ctl(g_controls.btnSpeed, rightX + (rightW - scale(200)) / 2, sy + scale(60), scale(200), btnH);
        /* Version button right under the speed button (restored, Q-149). */
        move_ctl(g_controls.btnPair, rightX + (rightW - scale(200)) / 2, sy + scale(60) + btnH + scale(8), scale(200), btnH);
        move_ctl(g_controls.capVer, rightX + pad, sy + scale(60) + btnH + scale(8) + btnH + scale(8), rightW - scale(24), text_h(1));
    }
    /* Settings (mode 2): language, theme and palette in the central block
     * (Q-121, Q-122). Three fieldsets stacked vertically; controls are
     * created in create_controls() and only positioned here. */
    {
        /* Block centred in the central panel, at most 440 design px wide. */
        int areaW = cw - 2 * m;
        int sw = areaW - 2 * pad;
        int sx;
        if (sw > scale(440))
            sw = scale(440);
        sx = m + (areaW - sw) / 2;
        int fy = contentY;
        int fh = scale(44);
        int gap = scale(14);

        /* Language. */
        move_ctl(g_controls.capLang, sx, fy, scale(120), lineH);
        move_ctl(g_controls.btnLangEn, sx, fy + scale(20), scale(110), btnH);
        move_ctl(g_controls.btnLangEs, sx + scale(118), fy + scale(20), scale(110), btnH);

        /* Theme. */
        int ty = fy + fh + gap;
        move_ctl(g_controls.capTheme, sx, ty, scale(120), lineH);
        move_ctl(g_controls.btnThemeSystem, sx, ty + scale(20), scale(100), btnH);
        move_ctl(g_controls.btnThemeLight, sx + scale(108), ty + scale(20), scale(100), btnH);
        move_ctl(g_controls.btnThemeDark, sx + scale(216), ty + scale(20), scale(100), btnH);

        /* Palette: list of catalogue entries. */
        int py = ty + fh + gap;
        move_ctl(g_controls.capPalette, sx, py, scale(120), lineH);
        move_ctl(g_controls.lstPalette, sx, py + scale(20), sw, topH - (py - contentY) - scale(30));
    }

    /* Bottom bar, always visible: transport controls, progress, state,
     * and system message to its right. */
    y = ch - statusH - m - playH;
    move_ctl(g_controls.groupPlay, m, y, cw - 2 * m, playH);
    /* Previous, play and next form a row, centered in the box. */
    x0 = m + ((cw - 2 * m) - (prevW + btnGap + playW + btnGap + nextW)) / 2;
    move_ctl(g_controls.btnPrevious, x0, y + scale(22), prevW, scale(28));
    move_ctl(g_controls.btnPlay, x0 + prevW + btnGap, y + scale(22), playW, scale(28));
    move_ctl(g_controls.btnNext, x0 + prevW + btnGap + playW + btnGap, y + scale(22), nextW, scale(28));
    /* Progress: its own row, up to the state/system-message area. */
    {
        int leftX = m + pad;
        int rightEdge = cw - m - pad;
        int stateW = measure_state_text_width() + scale(16);
        int sysX = leftX + scale(180) + stateW + scale(12);
        int sysW = rightEdge - sysX;
        if (sysW < 0)
            sysW = 0;
        move_ctl(g_controls.progress, leftX, y + scale(56), rightEdge - leftX, scale(16));
        /* Time below the progress bar (as before the change). */
        move_ctl(g_controls.valTime, leftX, y + scale(78), scale(170), lineH);
        /* State to the right of time, same row, box sized to its text. */
        move_ctl(g_controls.valState, leftX + scale(180), y + scale(78), stateW, lineH);
        /* System message to the right of state; hidden when there is no text. */
        move_ctl(g_controls.valSystem, sysX, y + scale(78), sysW, lineH);
        /* Bottom row: loop. */
        move_ctl(g_controls.checkLoop, leftX, y + scale(106), scale(170), lineH);
    }
}
