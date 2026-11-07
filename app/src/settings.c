#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define SETTINGS_FOLDER L"BRLoop"
#define SETTINGS_FILE L"settings.json"

/* Folder is %APPDATA%\BRLoop, created on demand by the save path.
 * MKW_SETTINGS_DIR, when set, replaces the whole folder: test runs point it at
 * a scratch directory so they can never touch the user's real preference. */
static int settings_folder(wchar_t *out, size_t out_len)
{
    wchar_t override[MAX_PATH];
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"MKW_SETTINGS_DIR", override, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        if (swprintf(out, out_len, L"%s", override) < 0)
            return 0;
        return 1;
    }
    n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return 0;
    if (swprintf(out, out_len, L"%s\\%s", appdata, SETTINGS_FOLDER) < 0)
        return 0;
    return 1;
}

int settings_path(wchar_t *out, size_t out_len)
{
    wchar_t folder[MAX_PATH];
    if (!settings_folder(folder, MAX_PATH))
        return 0;
    if (swprintf(out, out_len, L"%s\\%s", folder, SETTINGS_FILE) < 0)
        return 0;
    return 1;
}

/* Reads the whole file into buf. Returns the byte count, -2 when the file
 * does not exist (first run), or -1 when it exists but cannot be read. */
static long read_file(const wchar_t *path, char *buf, size_t cap)
{
    FILE *f;
    size_t n;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return -2;
    f = _wfopen(path, L"rb");
    if (!f)
        return -1;
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/* Accepts exactly {"language":"en"} or {"language":"es"}, with optional
 * whitespace. Anything else is treated as an invalid file. */
static int parse_language(const char *text, StrLanguage *language)
{
    const char *p = strstr(text, "\"language\"");
    if (!p)
        return 0;
    p += strlen("\"language\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (strncmp(p, "\"en\"", 4) == 0) {
        *language = STR_LANG_EN;
        return 1;
    }
    if (strncmp(p, "\"es\"", 4) == 0) {
        *language = STR_LANG_ES;
        return 1;
    }
    return 0;
}

/* Accepts "theme":"system", "light" or "dark". Anything else is not a theme. */
static int parse_theme(const char *text, ThemeChoice *theme)
{
    const char *p = strstr(text, "\"theme\"");
    if (!p)
        return 0;
    p += strlen("\"theme\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (strncmp(p, "\"system\"", 8) == 0) {
        *theme = THEME_SYSTEM;
        return 1;
    }
    if (strncmp(p, "\"light\"", 7) == 0) {
        *theme = THEME_LIGHT;
        return 1;
    }
    if (strncmp(p, "\"dark\"", 6) == 0) {
        *theme = THEME_DARK;
        return 1;
    }
    return 0;
}

/* Palette names used before the descriptors were dropped (Q-146). A saved
 * old name is translated on read; the next save writes the new name. */
static const struct {
    const char *old_name;
    const char *new_name;
} palette_renames[] = {
    { "Animal Crossing (verdes)", "Animal Crossing" },
    { "Mario Kart Wii (aro azul)", "Mario Kart Wii" },
    { "Metroid Prime (visor)", "Metroid Prime" },
    { "Metroid Prime 2 (Dark Visor)", "Metroid Prime 2" },
    { "Metroid Prime 3 (Phazon)", "Metroid Prime 3" },
    { "New Super Mario Bros. Wii (cielo)", "New Super Mario Bros. Wii" },
    { "Resident Evil 4 (apagado)", "Resident Evil 4" },
    { "Smash Brawl (modos)", "Smash Brawl" },
    { "Super Mario Galaxy (oscura)", "Super Mario Galaxy" },
    { "Super Mario Galaxy 2 (clara)", "Super Mario Galaxy 2" },
    { "Super Mario Sunshine (mar)", "Super Mario Sunshine" },
    { "Wii Fit (verde salud)", "Wii Fit" },
    { "Wii Sports (azul y gris)", "Wii Sports" },
    { "Wii Sports Resort (resort)", "Wii Sports Resort" },
    { "Zelda Skyward Sword (acuarela)", "Zelda Skyward Sword" },
    { "Zelda Twilight Princess (apagado)", "Zelda Twilight Princess" },
    { "Zelda Wind Waker (mar)", "Zelda Wind Waker" },
};

/* Translates a pre-Q-146 name in place. Names that are already current, or
 * unknown, are left as they are. */
static void migrate_palette_name(char *name, size_t name_len)
{
    size_t i;

    for (i = 0; i < sizeof palette_renames / sizeof palette_renames[0]; i++) {
        if (strcmp(name, palette_renames[i].old_name) == 0) {
            strncpy(name, palette_renames[i].new_name, name_len - 1);
            name[name_len - 1] = '\0';
            return;
        }
    }
}

/* Accepts "palette":"<name>". The name is copied without quotes. Names with
 * escapes, empty names or names that do not fit are not palettes. */
static int parse_palette(const char *text, char *out, size_t out_len)
{
    const char *p = strstr(text, "\"palette\"");
    size_t len = 0;
    if (!p)
        return 0;
    p += strlen("\"palette\"");
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != ':')
        return 0;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p != '"')
        return 0;
    p++;
    while (*p != '"' && *p != '\0' && *p != '\\') {
        if (len + 1 >= out_len)
            return 0;
        out[len++] = *p++;
    }
    if (*p != '"' || len == 0)
        return 0;
    out[len] = '\0';
    return 1;
}

/* Writes all preferences in one file. Each save keeps the others, so the
 * language, the theme and the palette never overwrite each other. */
static int write_settings(StrLanguage language, ThemeChoice theme, const char *palette)
{
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    FILE *f;
    const char *lang_code = language == STR_LANG_ES ? "es" : "en";
    const char *theme_code = theme == THEME_LIGHT ? "light"
                           : theme == THEME_DARK  ? "dark"
                           : "system";

    if (!settings_folder(folder, MAX_PATH))
        return -1;
    if (!settings_path(path, MAX_PATH))
        return -1;
    /* CreateDirectoryW fails with ERROR_ALREADY_EXISTS when the folder is
     * already there, which is fine; any other failure stops the save. */
    if (!CreateDirectoryW(folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return -1;
    f = _wfopen(path, L"wb");
    if (!f)
        return -1;
    fprintf(f, "{\"language\":\"%s\",\"theme\":\"%s\",\"palette\":\"%s\"}\n",
            lang_code, theme_code, palette);
    if (fclose(f) != 0)
        return -1;
    return 0;
}

SettingsStatus settings_load_language(StrLanguage *language)
{
    wchar_t path[MAX_PATH];
    char text[512];
    long n;

    *language = STR_LANG_EN;
    if (!settings_path(path, MAX_PATH))
        return SETTINGS_NOT_FOUND;
    n = read_file(path, text, sizeof text);
    if (n == -2)
        return SETTINGS_NOT_FOUND;
    if (n < 0)
        return SETTINGS_INVALID;
    if (!parse_language(text, language)) {
        *language = STR_LANG_EN;
        return SETTINGS_INVALID;
    }
    return SETTINGS_OK;
}

int settings_save_language(StrLanguage language)
{
    ThemeChoice theme = THEME_SYSTEM;
    char palette[128];
    settings_load_theme(&theme);
    settings_load_palette(palette, sizeof palette);
    return write_settings(language, theme, palette);
}

SettingsStatus settings_load_theme(ThemeChoice *theme)
{
    wchar_t path[MAX_PATH];
    char text[512];
    long n;

    *theme = THEME_SYSTEM;
    if (!settings_path(path, MAX_PATH))
        return SETTINGS_NOT_FOUND;
    n = read_file(path, text, sizeof text);
    if (n == -2)
        return SETTINGS_NOT_FOUND;
    if (n < 0)
        return SETTINGS_INVALID;
    if (!parse_theme(text, theme)) {
        *theme = THEME_SYSTEM;
        return SETTINGS_INVALID;
    }
    return SETTINGS_OK;
}

int settings_save_theme(ThemeChoice theme)
{
    StrLanguage language = STR_LANG_EN;
    char palette[128];
    settings_load_language(&language);
    settings_load_palette(palette, sizeof palette);
    return write_settings(language, theme, palette);
}

/* Reads the palette name. Missing or invalid values give "Wii Blanco". */
SettingsStatus settings_load_palette(char *out, size_t out_len)
{
    wchar_t path[MAX_PATH];
    char text[512];
    long n;

    strcpy(out, "Wii Blanco");
    if (!settings_path(path, MAX_PATH))
        return SETTINGS_NOT_FOUND;
    n = read_file(path, text, sizeof text);
    if (n == -2)
        return SETTINGS_NOT_FOUND;
    if (n < 0)
        return SETTINGS_INVALID;
    if (!parse_palette(text, out, out_len)) {
        strcpy(out, "Wii Blanco");
        return SETTINGS_INVALID;
    }
    migrate_palette_name(out, out_len);
    return SETTINGS_OK;
}

/* Writes the palette name (UTF-8), keeping the saved language and theme.
 * Returns 0 or -1. */
int settings_save_palette(const wchar_t *name)
{
    char palette[128];
    StrLanguage language = STR_LANG_EN;
    ThemeChoice theme = THEME_SYSTEM;
    if (!WideCharToMultiByte(CP_UTF8, 0, name, -1, palette, sizeof palette, NULL, NULL))
        return -1;
    if (strchr(palette, '"') || strchr(palette, '\\'))
        return -1;
    settings_load_language(&language);
    settings_load_theme(&theme);
    return write_settings(language, theme, palette);
}
