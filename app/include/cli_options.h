#ifndef CLI_OPTIONS_H
#define CLI_OPTIONS_H

#include <stddef.h>
#include <wchar.h>

/* Called for each positional argument. The callback owns the path only for
 * the duration of the call and must copy anything it needs to retain. Return
 * zero on success and write a user-facing error on failure. */
typedef int (*CliAddTrack)(void *context, const wchar_t *path,
                           wchar_t *error, size_t error_length);

/* Receives parser errors such as an overlong option path or a rejected track.
 * The callback decides whether to show a status message or a dialog. */
typedef void (*CliReportError)(void *context, const wchar_t *message);

/* Parsed command-line values. The path arrays are bounded storage owned by
 * this struct; the caller may safely use them after parsing returns. */
typedef struct {
    int selftest;                 /* --selftest was present */
    int has_invalid_path;         /* invalid_path contains a supplied path */
    wchar_t invalid_path[1024];   /* path passed to --invalid */
    int has_report_path;          /* report_path contains a supplied path */
    wchar_t report_path[1024];    /* path passed to --report */
} CliOptions;

/* Adapters keep this parser independent of Win32 controls and Player. Both
 * callbacks are optional; without add_track, positional paths are ignored. */
typedef struct {
    CliAddTrack add_track;
    CliReportError report_error;
    void *callback_context;       /* passed unchanged to both callbacks */
} CliParseCallbacks;

/* Parse options and preload positional BRSTM paths through callbacks. The
 * parser never opens report_path and never owns callback-created resources. */
int cli_options_parse(int argc, wchar_t **argv, CliOptions *options,
                     const CliParseCallbacks *callbacks);

#endif /* CLI_OPTIONS_H */
