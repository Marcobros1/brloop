#include "library.h"

#include <stdlib.h>
#include <string.h>

static wchar_t *duplicate_wide(const wchar_t *text)
{
    size_t length = wcslen(text) + 1;
    wchar_t *copy = (wchar_t *)malloc(length * sizeof *copy);
    if (copy)
        memcpy(copy, text, length * sizeof *copy);
    return copy;
}

int library_add(LibraryState *library, const wchar_t *path,
                const wchar_t *name, const LibraryTrackInfo *info)
{
    TrackRow *row;

    if (library->count == library->capacity) {
        int capacity = library->capacity ? library->capacity * 2 : 8;
        TrackRow *rows = (TrackRow *)realloc(
            library->rows, (size_t)capacity * sizeof *rows);
        if (!rows)
            return -1;
        library->rows = rows;
        library->capacity = capacity;
    }

    row = &library->rows[library->count];
    memset(row, 0, sizeof *row);
    row->path = duplicate_wide(path);
    row->name = duplicate_wide(name);
    if (!row->path || !row->name) {
        free(row->path);
        free(row->name);
        row->path = NULL;
        row->name = NULL;
        return -1;
    }
    row->rate = info->rate;
    row->channels = info->channels;
    row->total = info->total;
    row->loop = info->loop;
    row->loop_start = info->loop_start;
    library->count++;
    return 0;
}

int library_remove(LibraryState *library, int index)
{
    int replacement = -1;

    if (index < 0 || index >= library->count)
        return -1;
    free(library->rows[index].path);
    free(library->rows[index].name);
    memmove(&library->rows[index], &library->rows[index + 1],
            (size_t)(library->count - index - 1) * sizeof *library->rows);
    library->count--;

    if (library->selected_index == index) {
        /* The caller selects and opens the returned replacement. */
        library->selected_index = -1;
        if (library->count > 0)
            replacement = index < library->count ? index : library->count - 1;
    } else if (library->selected_index > index) {
        library->selected_index--;
    }
    return replacement;
}

void library_clear(LibraryState *library)
{
    for (int i = 0; i < library->count; i++) {
        free(library->rows[i].path);
        free(library->rows[i].name);
    }
    library->count = 0;
    library->selected_index = -1;
}

