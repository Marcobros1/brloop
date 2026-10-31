#ifndef ICONS_H
#define ICONS_H

#include <windows.h>

/* Transport glyphs drawn in code. The caller owns the device context and the
 * rectangle; the glyph is centred inside that rectangle. */
typedef enum {
    ICON_PLAY,
    ICON_PAUSE,
    ICON_PREVIOUS,
    ICON_NEXT
} IconKind;

void icons_draw(HDC dc, RECT box, IconKind kind, COLORREF color);

#endif /* ICONS_H */
