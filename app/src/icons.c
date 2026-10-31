#include "icons.h"

/* Filled glyphs drawn with GDI polygons, so they need no image files or
 * third-party icon sets. Coordinates are relative to the button's client
 * rectangle; each glyph is centred in the rectangle it is given. */

static void fill_polygon(HDC dc, const POINT *pts, int count, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    HBRUSH old = (HBRUSH)SelectObject(dc, brush);
    HPEN pen = (HPEN)GetStockObject(NULL_PEN);
    HPEN old_pen = (HPEN)SelectObject(dc, pen);
    Polygon(dc, pts, count);
    SelectObject(dc, old_pen);
    SelectObject(dc, old);
    DeleteObject(brush);
}

static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF color)
{
    RECT r = { x, y, x + w, y + h };
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}

/* Play: a right-pointing triangle. */
static void draw_play(HDC dc, RECT box, COLORREF color)
{
    int h = box.bottom - box.top;
    int tri_h = h * 6 / 10;
    int w = tri_h * 9 / 10;
    int left = box.left + (box.right - box.left - w) / 2;
    int top = box.top + (h - tri_h) / 2;
    int mid = top + tri_h / 2;
    POINT pts[3];
    pts[0].x = left;      pts[0].y = top;
    pts[1].x = left;      pts[1].y = top + tri_h;
    pts[2].x = left + w;  pts[2].y = mid;
    fill_polygon(dc, pts, 3, color);
}

/* Pause: two vertical bars. */
static void draw_pause(HDC dc, RECT box, COLORREF color)
{
    int h = box.bottom - box.top;
    int bar_w = h / 4;
    int bar_h = h * 6 / 10;
    int cx = box.left + (box.right - box.left) / 2;
    int top = box.top + (h - bar_h) / 2;
    fill_rect(dc, cx - bar_w * 3 / 2 - bar_w / 3, top, bar_w, bar_h, color);
    fill_rect(dc, cx + bar_w / 2 + bar_w / 3, top, bar_w, bar_h, color);
}

/* Previous and next: a triangle pointing toward the direction plus a bar at
 * the end it moves away from. */
static void draw_skip(HDC dc, RECT box, COLORREF color, int forward)
{
    int h = box.bottom - box.top;
    int w = h * 6 / 10;
    int bar_w = h / 6;
    int cx = box.left + (box.right - box.left) / 2;
    int top = box.top + h / 2 - h * 3 / 10;
    int bottom = top + h * 6 / 10;
    int mid = top + h * 3 / 10;
    POINT pts[3];
    if (forward) {
        int x0 = cx - w / 2 - bar_w / 2;
        pts[0].x = x0;          pts[0].y = top;
        pts[1].x = x0;          pts[1].y = bottom;
        pts[2].x = x0 + w;      pts[2].y = mid;
        fill_polygon(dc, pts, 3, color);
        fill_rect(dc, x0 + w + bar_w / 2, top, bar_w, bottom - top, color);
    } else {
        int x0 = cx + w / 2 + bar_w / 2;
        pts[0].x = x0;          pts[0].y = top;
        pts[1].x = x0;          pts[1].y = bottom;
        pts[2].x = x0 - w;      pts[2].y = mid;
        fill_polygon(dc, pts, 3, color);
        fill_rect(dc, x0 - w - bar_w / 2 - bar_w, top, bar_w, bottom - top, color);
    }
}

void icons_draw(HDC dc, RECT box, IconKind kind, COLORREF color)
{
    int saved = SaveDC(dc);
    SetBkMode(dc, TRANSPARENT);
    switch (kind) {
    case ICON_PLAY:     draw_play(dc, box, color); break;
    case ICON_PAUSE:    draw_pause(dc, box, color); break;
    case ICON_PREVIOUS: draw_skip(dc, box, color, 0); break;
    case ICON_NEXT:     draw_skip(dc, box, color, 1); break;
    default: break;
    }
    RestoreDC(dc, saved);
}
