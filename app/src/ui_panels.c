#include "ui_panels.h"

#include "ui_types.h"

/* Mirrors the ctl_show(..., bib) calls in set_active_mode() (main.c), bib =
 * (mode == 0). Keep the two lists in step when a control changes mode. */
static const int g_library_ids[] = {
    IDC_BTN_OPEN, IDC_BTN_REMOVE, IDC_BTN_CLEAR, IDC_GROUP_LIB, IDC_LIST,
    IDC_GROUP_INFO, IDC_VAL_NAME, IDC_VAL_PATH, IDC_CAP_RATE, IDC_VAL_RATE,
    IDC_CAP_CH, IDC_VAL_CH, IDC_CAP_DUR, IDC_VAL_DUR, IDC_CAP_LOOP,
    IDC_VAL_LOOP, IDC_GROUP_SPEED, IDC_VAL_SPEED, IDC_VAL_DEVRATE,
    IDC_BTN_SPEED, IDC_BTN_PAIR, IDC_CAP_VER
};

/* Mirrors the ctl_show(..., settings) calls in set_active_mode(), settings =
 * (mode == 2). */
static const int g_settings_ids[] = {
    IDC_CAP_LANG, IDC_CAP_THEME, IDC_CAP_PALETTE, IDC_BTN_LANG_EN,
    IDC_BTN_LANG_ES, IDC_BTN_THEME_SYSTEM, IDC_BTN_THEME_LIGHT,
    IDC_BTN_THEME_DARK, IDC_LST_PALETTE
};

int panel_ids_count(PanelId panel)
{
    if (panel == PANEL_LIBRARY)
        return (int)(sizeof g_library_ids / sizeof g_library_ids[0]);
    if (panel == PANEL_SETTINGS)
        return (int)(sizeof g_settings_ids / sizeof g_settings_ids[0]);
    return 0;
}

int panel_ids_get(PanelId panel, int index)
{
    if (index < 0 || index >= panel_ids_count(panel))
        return 0;
    if (panel == PANEL_LIBRARY)
        return g_library_ids[index];
    return g_settings_ids[index];
}
