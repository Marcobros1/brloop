#ifndef UI_LIBRARY_H
#define UI_LIBRARY_H

#include "ui_state.h"

/* Library rows: insert, refresh the loop column, add/remove/clear tracks. */
void list_refresh_loop_column(void);
int add_track(const wchar_t *path, wchar_t *errbuf, size_t errlen);
void remove_track(int idx);
void remove_all(void);

/* Info panel: clear, fill from a snapshot, and select a track. */
void clear_info_panel(void);
void fill_info_panel(const PlayerSnapshot *s);
void select_track(int idx);

/* UI refresh and track change helpers (called by main.c's command router). */
void update_ui(void);
void change_track(int idx, int wasPlaying);

#endif /* UI_LIBRARY_H */
