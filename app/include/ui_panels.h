#ifndef UI_PANELS_H
#define UI_PANELS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Panel membership: which control ids belong to each mode. Pure table, no
 * HWND. The sets mirror set_active_mode() in main.c. */
typedef enum {
    PANEL_LIBRARY = 0,
    PANEL_SETTINGS = 1
} PanelId;

/* Number of control ids of a panel. */
int panel_ids_count(PanelId panel);

/* Control id number `index` of a panel (0 <= index < count), or 0 if out of range. */
int panel_ids_get(PanelId panel, int index);

#ifdef __cplusplus
}
#endif

#endif /* UI_PANELS_H */
