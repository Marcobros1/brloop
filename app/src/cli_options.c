#include "cli_options.h"

#include <string.h>

/* Keep reporting behind the adapter so this module has no UI dependency. */
static void report(CliParseCallbacks const *callbacks, const wchar_t *message)
{
    if (callbacks && callbacks->report_error)
        callbacks->report_error(callbacks->callback_context, message);
}

/* Parse argv[1..argc-1]. The caller must keep argv valid for the call only;
 * option paths are copied into options, while positional paths are passed to
 * add_track immediately. A missing option value is ignored safely. */
int cli_options_parse(int argc, wchar_t **argv, CliOptions *options,
                     const CliParseCallbacks *callbacks)
{
    int i;

    if (!options || argc < 0 || !argv)
        return -1;
    memset(options, 0, sizeof *options);

    for (i = 1; i < argc; i++) {
        const wchar_t *argument = argv[i];
        if (wcscmp(argument, L"--selftest") == 0) {
            options->selftest = 1;
        } else if (wcscmp(argument, L"--invalid") == 0 && i + 1 < argc) {
            const wchar_t *path = argv[++i];
            if (wcslen(path) >= sizeof options->invalid_path
                                      / sizeof *options->invalid_path) {
                report(callbacks, L"Ruta de --invalid demasiado larga: ignorada.");
                continue;
            }
            wcscpy_s(options->invalid_path,
                     sizeof options->invalid_path
                         / sizeof *options->invalid_path,
                     path);
            options->has_invalid_path = 1;
        } else if (wcscmp(argument, L"--report") == 0 && i + 1 < argc) {
            const wchar_t *path = argv[++i];
            if (wcslen(path) >= sizeof options->report_path
                                      / sizeof *options->report_path)
                continue;
            wcscpy_s(options->report_path,
                     sizeof options->report_path
                         / sizeof *options->report_path,
                     path);
            options->has_report_path = 1;
        } else if (callbacks && callbacks->add_track) {
            wchar_t error[1024];
            if (callbacks->add_track(callbacks->callback_context, argument,
                                     error, sizeof error / sizeof *error) != 0
                && !options->selftest)
                report(callbacks, error);
        }
    }
    return 0;
}
