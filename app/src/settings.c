#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

#define SETTINGS_FOLDER L"MetalKartWiiPlayer"
#define SETTINGS_FILE L"settings.json"

/* Folder is %APPDATA%\MetalKartWiiPlayer. Created on demand by the save path. */
static int settings_folder(wchar_t *out, size_t out_len)
{
    wchar_t appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
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
    wchar_t folder[MAX_PATH];
    wchar_t path[MAX_PATH];
    FILE *f;
    const char *code = language == STR_LANG_ES ? "es" : "en";

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
    fprintf(f, "{\"language\":\"%s\"}\n", code);
    if (fclose(f) != 0)
        return -1;
    return 0;
}
