#include "strings.h"

/* Both tables come from the single STRING_TABLE list in strings.h, so the
 * index of a text is always the index of its key. */
#define STRING_ES(key, es, en) es,
#define STRING_EN(key, es, en) en,

static const wchar_t *const table_es[] = { STRING_TABLE(STRING_ES) };
static const wchar_t *const table_en[] = { STRING_TABLE(STRING_EN) };

#undef STRING_ES
#undef STRING_EN

static StrLanguage current_language = STR_LANG_EN;

void strings_set_language(StrLanguage language)
{
    current_language = language;
}

StrLanguage strings_language(void)
{
    return current_language;
}

const wchar_t *tr(StrKey key)
{
    const wchar_t *const *table = current_language == STR_LANG_ES ? table_es : table_en;
    if ((int)key < 0 || (int)key >= (int)STR_KEY_COUNT)
        return L"";
    return table[key];
}
