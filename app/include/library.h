#ifndef LIBRARY_H
#define LIBRARY_H

#include <stddef.h>
#include <wchar.h>

#include "ui_types.h"

typedef struct {
    uint32_t rate;
    uint8_t channels;
    uint32_t total;
    uint8_t loop;
    uint32_t loop_start;
} LibraryTrackInfo;

/* Add one validated track. The library copies both strings and metadata, so
 * the caller may release its input after the call. If a copy fails, the row
 * is not added and count does not change. Returns 0 or -1 on OOM. */
int library_add(LibraryState *library, const wchar_t *path,
                const wchar_t *name, const LibraryTrackInfo *info);

/* Remove a row. selected_index becomes -1 when the removed row was selected.
 * Returns the replacement row the caller should select, or -1 if none. */
int library_remove(LibraryState *library, int index);

/* Release every row string but retain the allocated row capacity for reuse. */
void library_clear(LibraryState *library);

#endif /* LIBRARY_H */
