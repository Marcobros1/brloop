#ifndef UI_DIALOGS_H
#define UI_DIALOGS_H

#include <windows.h>

/* Modal dialogs and popup menus: they wait for user interaction and block the
 * selftest, so they live in their own file, excluded from coverage by path.
 */
void browse_files(void);
void show_language_menu(HWND owner);
void show_theme_menu(HWND owner);

#endif /* UI_DIALOGS_H */
