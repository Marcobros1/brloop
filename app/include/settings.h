#ifndef SETTINGS_H
#define SETTINGS_H

#include "strings.h"

/* User preferences kept between sessions. Only the interface language is
 * stored today. The file lives in %APPDATA%, never next to the music. */

typedef enum {
    SETTINGS_OK = 0,
    SETTINGS_NOT_FOUND,     /* first run: no file yet, defaults apply */
    SETTINGS_INVALID        /* file exists but cannot be parsed */
} SettingsStatus;

/* Full path of settings.json, or 0 when %APPDATA% is unset. */
int settings_path(wchar_t *out, size_t out_len);

/* Reads the language. On anything other than SETTINGS_OK the caller gets
 * STR_LANG_EN, and the status says why, so the UI can report it. */
SettingsStatus settings_load_language(StrLanguage *language);

/* Writes the language, creating the folder when needed. Returns 0 on
 * success, -1 on any failure. */
int settings_save_language(StrLanguage language);

#endif /* SETTINGS_H */
