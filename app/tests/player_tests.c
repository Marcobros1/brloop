/* Automated tests for the T2 BRSTM player (console, no UI involved).
 *
 * Covers, against real collection files:
 *   1. Metadata of 44100/48000/51269 stereo, 4ch, 8ch and no-loop files.
 *   2. Rejection of every invalid mutant from make_invalid_cases.py and
 *      recovery afterwards.
 *   3. Speed: the reported device_rate (FLSM equivalence) and the engine at
 *      conversions (44100 -> 47104, 48000 -> 51269).
 *   4. Loop rendering byte-identical to vgmstream (-l 2 -f 0) over
 *      total + (total - loop_start) frames.
 *   5. End of playback: looped file with the loop off and a no-loop file
 *      both stop exactly at total_samples.
 *   6. Memory stays flat while rendering millions of looped frames.
 *   7. Transport (play/pause/resume/stop, track change, close) on the real
 *      audio device with latency and handle/memory-leak checks.
 *   8. Wall-clock duration at 1.0x vs at the FLSM speed, plus a real-time
 *      loop wrap that must stay inside [loop_start, total_samples).
 *
 * Usage:
 *   player_tests.exe --collection <dir> --invalid <dir> \
 *                    --vgmstream <test.exe> --work <dir> [--report <file>]
 */
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "player.h"
#include "resampler.h"

/* Wall-clock checks can never be exact (scheduler, driver buffering), so a
 * 4% relative tolerance is used with this absolute floor for very short
 * files where 4% would be tighter than the measurement noise. */
#define TOL_SEC 0.5

static int g_pass, g_fail; /* check counters, summarized in the RESULT line */
static FILE *g_report;     /* optional duplicate of stdout (--report) */

static char g_col[512];  /* --collection: real .brstm files (read-only) */
static char g_inv[512];  /* --invalid: mutant .brstm files that must fail */
static char g_vgm[512];  /* --vgmstream: test.exe reference decoder */
static char g_work[512]; /* --work: scratch dir for temporary reference WAVs */

static int16_t g_chunk[4096 * 16]; /* render scratch: big enough for the
                                    * 4096-frame streaming reads and for
                                    * 16-channel interleaving (4096*16) */

typedef struct {
    const char *name;
    uint32_t rate;
    uint32_t channels;
    int loop;
    uint32_t loop_start;
    uint32_t total;
} ExpectFile;

/* Expected metadata, cross-checked against vgmstream in the T1 harness. */
static const ExpectFile k_files[] = {
    { "uTTC_N.brstm",         44100, 2, 1, 5634048, 12786908 },
    { "tKD_N.brstm",          44100, 2, 1, 1347584,  4876838 },
    { "104_N.brstm",          48000, 2, 1,  501760,  8771278 },
    { "n_rainbow32_f.brstm",  51269, 2, 1,        0,  8571425 },
    { "o_Wi-Fi_waiting32.brstm", 48000, 2, 0,      0,  7870464 },
    { "o_FanfareGP1_32.brstm", 48000, 4, 1,  860160,  1303244 },
    { "STRM_N_KOOPA_N.brstm", 48000, 8, 1,        0,  6270135 }
};

static const char *k_short = "o_Crs_In_Fan_Wifi.brstm";

/* The test battery's output channel: stdout always, report file when opened.
 * Every check/note goes through here so a redirected run and an interactive
 * one produce identical evidence. */
static void emit(const char *line)
{
    puts(line);
    fflush(stdout);
    if (g_report) {
        fputs(line, g_report);
        fputc('\n', g_report);
        fflush(g_report);
    }
}

/* Record a named check: PASS/FAIL is decided by the caller (ok), the
 * counters feed the final RESULT line and the exit code. Format-style so
 * every check can print the actual vs expected values as evidence. */
static void check(int ok, const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "[%s] %s", ok ? "PASS" : "FAIL", msg);
    emit(line);
    if (ok)
        g_pass++;
    else
        g_fail++;
}

/* Informational line (indented, not counted): measurements, timings and
 * state dumps that support a check but are not checks themselves. */
static void note(const char *fmt, ...)
{
    char msg[1024], line[1100];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "  %s", msg);
    emit(line);
}

/* Build a path inside the collection directory (the one read-only source
 * of real files for every test group). */
static void col_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s\\%s", g_col, name);
}

/* Monotonic wall clock in milliseconds (QueryPerformanceCounter); used for
 * latency and duration measurements. The frequency is queried once. */
static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)freq.QuadPart;
}

/* Poll player_get_state until the state matches or the timeout expires;
 * the engine runs its own thread, so tests observe state transitions the
 * same way the UI does instead of calling internal functions. */
static int wait_state(Player *p, PlayerState want, DWORD timeout_ms)
{
    double start = now_ms();
    do {
        if (player_get_state(p).state == want)
            return 1;
        Sleep(10);
    } while (now_ms() - start < (double)timeout_ms);
    return player_get_state(p).state == want;
}

/* Handle and memory snapshots for the leak checks. Handle counts are only
 * meaningful relative to a baseline taken after winmm_warmup(), because the
 * first open pays a one-time driver initialization (see that function). */
static DWORD handle_count(void)
{
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

static SIZE_T working_set(void)  /* process working set; 0 if the API fails */
{
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return pmc.WorkingSetSize;
    return 0;
}

/* The first waveOutOpen in a process pays a one-time winmm/driver
 * initialization (measured: ~100 handles on this machine) that survives
 * close; run one throwaway cycle before measuring handle balances. */
static void winmm_warmup(void)
{
    HWAVEOUT raw = NULL;
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 44100;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 44100 * 4;
    if (waveOutOpen(&raw, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL)
        == MMSYSERR_NOERROR)
        waveOutClose(raw);
}

/* ----------------------------------------------------------------- WAV --- */

/* Reference WAV loaded fully into memory: the loop comparison reads the
 * PCM sequentially against our render, so a whole-file buffer keeps the
 * compare loop simple; reference files are a few MB at most. */
typedef struct {
    uint8_t *bytes;        /* whole file */
    size_t size;
    uint16_t channels;
    uint16_t bits;
    const uint8_t *pcm;    /* points into bytes: the data chunk payload */
    uint32_t pcm_bytes;
    uint32_t frames;
} WavRef;

/* Little-endian readers: the reference WAV is always little-endian RIFF
 * regardless of host order. */
static uint32_t rd_u32le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *b)
{
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void wav_free(WavRef *w)  /* release and zero, safe to call twice */
{
    free(w->bytes);
    memset(w, 0, sizeof *w);
}

/* Parse the reference WAV: RIFF/WAVE magic, a PCM (format 1) fmt chunk and
 * a data chunk; anything else (compressed formats, missing chunks) is an
 * error, because the comparison assumes interleaved PCM16. Chunk lengths are
 * clamped to the file so a lying header cannot read past the buffer. */
static int wav_load(const char *path, WavRef *w, char *err, size_t errlen)
{
    FILE *f;
    long size;
    size_t got;
    size_t pos;
    int have_fmt = 0;

    memset(w, 0, sizeof *w);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 44) {
        fclose(f);
        snprintf(err, errlen, "wav too small (%ld bytes)", size);
        return -1;
    }
    w->bytes = (uint8_t *)malloc((size_t)size);
    if (!w->bytes) {
        fclose(f);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    got = fread(w->bytes, 1, (size_t)size, f);
    fclose(f);
    w->size = got;
    if (got < 44 || memcmp(w->bytes, "RIFF", 4)
        || memcmp(w->bytes + 8, "WAVE", 4)) {
        wav_free(w);
        snprintf(err, errlen, "not a RIFF/WAVE file");
        return -1;
    }
    pos = 12;
    while (pos + 8 <= w->size) {
        uint8_t *ch = w->bytes + pos;
        uint32_t len = rd_u32le(ch + 4);
        if (pos + 8 + len > w->size)
            len = (uint32_t)(w->size - pos - 8);
        if (!memcmp(ch, "fmt ", 4) && len >= 16) {
            if (rd_u16le(ch + 8) != 1) {
                wav_free(w);
                snprintf(err, errlen, "wav format is not PCM");
                return -1;
            }
            w->channels = rd_u16le(ch + 10);
            w->bits = rd_u16le(ch + 22);
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) {
                wav_free(w);
                snprintf(err, errlen, "data chunk before fmt chunk");
                return -1;
            }
            w->pcm = ch + 8;
            w->pcm_bytes = len;
            break;
        }
        pos += 8 + len + (len & 1);
    }
    if (!w->pcm) {
        wav_free(w);
        snprintf(err, errlen, "no data chunk");
        return -1;
    }
    if (w->bits != 16 || w->channels == 0) {
        wav_free(w);
        snprintf(err, errlen, "expected PCM16 with channels");
        return -1;
    }
    w->frames = w->pcm_bytes / ((uint32_t)w->channels * 2u);
    return 0;
}

/* --------------------------------------------------------------- groups -- */

/* Metadata group (headless): open every representative collection file and
 * compare rate/channels/total/loop against values already cross-checked
 * against vgmstream in the T1 harness (k_files). Covers 44100/48000/51269
 * rates, stereo/4ch/8ch and both loop and no-loop files. */
static void test_metadata(void)
{
    Player *p;
    size_t i;

    note("== metadata (headless) ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    for (i = 0; i < sizeof k_files / sizeof k_files[0]; i++) {
        const ExpectFile *e = &k_files[i];
        char path[600], err[512];
        int rc;
        PlayerSnapshot s;

        col_path(e->name, path, sizeof path);
        err[0] = '\0';
        rc = player_open(p, path, err, sizeof err);
        if (rc != 0) {
            check(0, "open %s (rc=%d): %s", e->name, rc, err);
            continue;
        }
        s = player_get_state(p);
        check(s.sample_rate == e->rate && s.channels == e->channels
              && s.total_samples == e->total
              && (s.file_loop != 0) == (e->loop != 0)
              && s.device_rate == e->rate && s.position == 0,
              "metadata %s: %u Hz, %u ch, %llu samples, loop=%d",
              e->name, s.sample_rate, s.channels,
              (unsigned long long)s.total_samples, s.file_loop);
        if (e->loop)
            check(s.loop_start == e->loop_start,
                  "loop_start %s: %u (expected %u)", e->name, s.loop_start,
                  e->loop_start);
        player_close(p);
    }
    player_destroy(p);
}

/* Speed group (headless): the reported device_rate is the FLSM equivalence
 * (player_device_rate), checked against the two conversions measured from
 * real FLSM output, plus the clamp bounds on set_speed through a real file.
 * The device itself always opens at the file rate; the speed is applied by
 * the resampler, which test_speed_chain verifies sample by sample. */
static void test_speed_mapping(void)
{
    Player *p;
    PlayerSnapshot s;
    double k = (double)PLAYER_FLSM_SPEED_PPM / (double)PLAYER_SPEED_SCALE;

    note("== speed -> device_rate mapping ==");
    check(player_device_rate(44100, PLAYER_FLSM_SPEED_PPM) == 47104,
          "device_rate(44100, FLSM) = 47104");
    check(player_device_rate(48000, PLAYER_FLSM_SPEED_PPM) == 51269,
          "device_rate(48000, FLSM) = 51269");
    check(player_device_rate(51269, PLAYER_SPEED_SCALE) == 51269,
          "1.0x keeps the base rate (51269)");
    check(player_device_rate(44100, 2000000u) == 88200,
          "2.0x doubles the device rate");
    check((uint32_t)(44100.0 * k) == 47104 && (uint32_t)(48000.0 * k) == 51269,
          "k=1.06812 reproduces both measured FLSM conversions");
    note("k = %.6f lies in [1.0681179, 1.068125) for 44100 and in "
         "[1.06810417, 1.068125) for 48000", k);

    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    {
        char path[600], err[512];
        col_path("104_N.brstm", path, sizeof path);
        err[0] = '\0';
        if (player_open(p, path, err, sizeof err) != 0) {
            check(0, "open 104_N.brstm for speed test: %s", err);
            player_destroy(p);
            return;
        }
    }
    s = player_get_state(p);
    check(s.device_rate == 48000, "default speed is 1.0x (device 48000)");
    player_set_speed(p, PLAYER_FLSM_SPEED_PPM);
    s = player_get_state(p);
    check(s.device_rate == 51269 && s.speed_ppm == PLAYER_FLSM_SPEED_PPM,
          "set_speed(FLSM) -> device 51269");
    player_set_speed(p, 2000000u);
    check(player_get_state(p).device_rate == 96000,
          "set_speed(2.0x) -> device 96000");
    player_set_speed(p, 0); /* below the clamp: 0.25x */
    check(player_get_state(p).device_rate == 12000,
          "set_speed(0) clamps to 0.25x -> device 12000");
    player_set_speed(p, 4000000u); /* above the clamp: 4.0x */
    check(player_get_state(p).device_rate == 192000,
          "set_speed(4.0x) -> device 192000");
    player_set_speed(p, PLAYER_SPEED_SCALE);
    check(player_get_state(p).device_rate == 48000,
          "back to 1.0x -> device 48000");
    player_close(p);
    player_destroy(p);
}

typedef struct {
    const int16_t *frames;
    size_t n, pos;
} SpeedSource;

static int speed_source_pull(void *ctx, int16_t *frame)
{
    SpeedSource *s = (SpeedSource *)ctx;
    if (s->pos >= s->n)
        return 0;
    frame[0] = s->frames[s->pos * 2];
    frame[1] = s->frames[s->pos * 2 + 1];
    s->pos++;
    return 1;
}

/* Speed chain (headless): at FLSM speed the engine must produce exactly what
 * the resampler produces from its own 1.0x decode of the same file. Both use
 * the same decoder, loop and step, so any sample mismatch means the cursor,
 * history or step in render_locked is wrong. The device opens at the file
 * rate: the speed lives in the resampler step, not in the device. */
static void test_speed_chain(void)
{
    Player *p;
    PlayerSnapshot s;
    char path[600], err[512];
    int16_t *ref = NULL, *eng = NULL;
    size_t ref_cap, total = 0, eng_n = 0, i;
    uint32_t step;
    int16_t buf[1024 * 2];
    int mism = 0;

    note("== speed chain: engine at FLSM vs resampler over the 1.0x decode ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created for speed chain");
        return;
    }
    col_path("n_block_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open n_block_N.brstm for speed chain: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    ref_cap = (size_t)s.total_samples + 1;
    ref = (int16_t *)malloc(ref_cap * 2 * sizeof(int16_t));
    if (!ref) { check(0, "allocate 1.0x reference"); player_destroy(p); return; }

    player_set_loop(p, 0);
    player_play(p);
    for (;;) {
        uint32_t n = player_render(p, buf, 1024);
        if (n == 0) break;
        if (total + n > ref_cap) break;
        memcpy(ref + total * 2, buf, (size_t)n * 2 * sizeof(int16_t));
        total += n;
    }
    player_stop(p);
    player_close(p);

    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "reopen for FLSM render: %s", err);
        free(ref); player_destroy(p);
        return;
    }
    player_set_loop(p, 0);
    player_set_speed(p, PLAYER_FLSM_SPEED_PPM);
    s = player_get_state(p);
    step = resampler_step(s.sample_rate, player_device_rate(s.sample_rate, PLAYER_FLSM_SPEED_PPM));

    /* An empty 1.0x render would make the allocations below zero-sized. */
    if (total == 0) {
        check(0, "1.0x reference rendered no samples");
        free(ref); player_destroy(p);
        return;
    }
    eng = (int16_t *)malloc(total * 2 * sizeof(int16_t));
    if (!eng) { check(0, "allocate FLSM render"); free(ref); player_destroy(p); return; }
    player_play(p);
    for (;;) {
        uint32_t n = player_render(p, buf, 1024);
        if (n == 0) break;
        if (eng_n + n > total) break;
        memcpy(eng + eng_n * 2, buf, (size_t)n * 2 * sizeof(int16_t));
        eng_n += n;
    }
    player_stop(p);
    player_close(p);
    player_destroy(p);

    {
        SpeedSource src;
        Resampler r;
        int16_t *flsm;
        size_t got;
        if (eng_n == 0) {
            check(0, "FLSM render produced no samples");
            free(ref);
            free(eng);
            return;
        }
        flsm = (int16_t *)malloc(eng_n * 2 * sizeof(int16_t));
        if (!flsm) {
            check(0, "allocate reference resample");
            free(ref);
            free(eng);
            return;
        }
        src.frames = ref;
        src.n = total;
        src.pos = 0;
        resampler_reset(&r, 2);
        got = resampler_render(&r, step, speed_source_pull, &src, flsm, eng_n);

        check(eng_n > 0 && got == eng_n,
              "FLSM render length %zu matches the resampler reference %zu",
              eng_n, got);
        for (i = 0; i < got * 2; i++) {
            if (eng[i] != flsm[i]) {
                mism++;
            }
        }
        check(mism == 0,
              "engine at FLSM is sample-identical to the resampler over its 1.0x decode "
              "(%d mismatched samples)", mism);
        free(flsm);
    }

    free(ref);
    free(eng);
}

/* Invalid group (headless): every mutant generated by make_invalid_cases.py
 * must be rejected and the untampered control must still open afterwards
 * (recovery after failures). The directory listing is dynamic, so mutants
 * are added/removed without touching this test. */
static void test_invalid(void)
{
    WIN32_FIND_DATAA fd;
    HANDLE hfind;
    char pattern[600];
    Player *p;
    int total = 0, rejected = 0, accepted = 0;

    note("== invalid files rejected (mutants of tKD_N) ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    snprintf(pattern, sizeof pattern, "%s\\*.brstm", g_inv);
    hfind = FindFirstFileA(pattern, &fd);
    if (hfind == INVALID_HANDLE_VALUE) {
        check(0, "invalid-cases directory readable: %s", g_inv);
        player_destroy(p);
        return;
    }
    do {
        char path[700], err[512];
        int rc, is_control;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(path, sizeof path, "%s\\%s", g_inv, fd.cFileName);
        is_control = _stricmp(fd.cFileName, "control_valid.brstm") == 0;
        err[0] = '\0';
        rc = player_open(p, path, err, sizeof err);
        total++;
        if (is_control) {
            if (rc == 0)
                accepted++;
            check(rc == 0, "valid control accepted (%s, rc=%d)",
                  fd.cFileName, rc);
        } else {
            if (rc != 0)
                rejected++;
            check(rc != 0, "invalid rejected: %s (rc=%d)", fd.cFileName, rc);
        }
    } while (FindNextFileA(hfind, &fd));
    FindClose(hfind);

    check(total > 0 && rejected == total - 1 && accepted == 1,
          "all %d mutants rejected and the control accepted", total - 1);

    /* Recovery: a valid file still opens after all those failures. */
    {
        char path[600], err[512];
        col_path("tKD_N.brstm", path, sizeof path);
        err[0] = '\0';
        check(player_open(p, path, err, sizeof err) == 0,
              "valid file opens after the failures");
        player_close(p);
    }
    player_destroy(p);
}

/* Loop group: the strongest correctness check in the battery. vgmstream
 * renders the same file with its own loop handling (-l 2 -f 0: loop twice,
 * no fade), and our streaming render (headless) must be byte-identical over
 * the initial pass plus one full loop section. Also verifies the wrap count
 * and that each wrap lands exactly on loop_start. */
static void test_loop_reference(void)
{
    Player *p;
    char path[600], err[512], cmd[2048], refpath[700];
    PlayerSnapshot s;
    WavRef ref;
    uint64_t target, done, expect_frames, expect_wraps;
    int16_t *out;
    int rc;

    note("== loop rendering vs vgmstream (-l 2 -f 0) ==");

    /* Reference first: its frame count defines what we must reproduce.
     * The command starts with "call " so cmd.exe's /c quote-stripping rule
     * (first char is a quote) cannot mangle the quoted paths. */
    col_path("tKD_N.brstm", path, sizeof path);
    snprintf(refpath, sizeof refpath, "%s\\tKD_loop_ref.wav", g_work);
    snprintf(cmd, sizeof cmd, "call \"%s\" -l 2 -f 0 -o \"%s\" \"%s\"",
             g_vgm, refpath, path);
    rc = system(cmd);
    if (wav_load(refpath, &ref, err, sizeof err) != 0) {
        check(0, "vgmstream reference produced a wav (system rc=%d): %s",
              rc, err);
        return;
    }

    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        wav_free(&ref);
        return;
    }
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open tKD_N.brstm: %s", err);
        player_destroy(p);
        wav_free(&ref);
        return;
    }
    if (player_play(p) != 0) {
        check(0, "start playback for the loop comparison");
        player_destroy(p);
        wav_free(&ref);
        return;
    }
    s = player_get_state(p);
    expect_frames = (uint64_t)s.total_samples
                  + ((uint64_t)s.total_samples - s.loop_start);
    check(ref.frames == expect_frames,
          "reference frames = total + (total - loop_start): %llu (expected %llu)",
          (unsigned long long)ref.frames, (unsigned long long)expect_frames);
    check(ref.channels == s.channels,
          "reference channels match (%u vs %u)", ref.channels, s.channels);
    target = ref.frames;

    out = (int16_t *)malloc((size_t)target * s.channels * sizeof(int16_t));
    if (!out) {
        check(0, "allocate %llu-frame render buffer",
              (unsigned long long)target);
        player_destroy(p);
        wav_free(&ref);
        return;
    }

    done = 0;
    while (done < target) {
        uint64_t left = target - done;
        uint32_t want = left > 4096 ? 4096 : (uint32_t)left;
        uint32_t got = player_render(p, out + (size_t)(done * s.channels),
                                     want);
        if (got == 0)
            break;
        done += got;
    }
    check(done == target, "rendered %llu of %llu frames",
          (unsigned long long)done, (unsigned long long)target);
    s = player_get_state(p);

    expect_wraps = target > s.total_samples
                 ? (target - s.total_samples)
                   / ((uint64_t)s.total_samples - s.loop_start)
                 : 0;
    check(s.wraps == expect_wraps,
          "wrap count %u matches the reference pass count %llu",
          s.wraps, (unsigned long long)expect_wraps);
    check(s.last_wrap_position == s.loop_start,
          "wraps jump to loop_start (%u)", s.last_wrap_position);

    if (done == target && ref.pcm_bytes == (uint64_t)target * s.channels * 2) {
        size_t nbytes = (size_t)target * s.channels * sizeof(int16_t);
        int first = memcmp(out, ref.pcm, nbytes);
        if (first == 0) {
            check(1, "PCM byte-identical to vgmstream over %llu frames "
                     "(initial pass + loop section)",
                  (unsigned long long)target);
        } else {
            size_t i;
            const uint8_t *a = (const uint8_t *)out;
            for (i = 0; i < nbytes && a[i] == ref.pcm[i]; i++) {
            }
            check(0, "PCM differs from vgmstream at byte %llu of %llu "
                     "(frame %llu, ch %llu)",
                  (unsigned long long)i, (unsigned long long)nbytes,
                  (unsigned long long)(i / (s.channels * 2)),
                  (unsigned long long)((i / 2) % s.channels));
        }
    } else {
        check(0, "length mismatch prevents compare (mine=%llu frames, ref=%u)",
              (unsigned long long)done, ref.frames);
    }

    free(out);
    player_destroy(p);
    wav_free(&ref);
    DeleteFileA(refpath);
}

/* No-loop group: a file without loop metadata must play to exactly
 * total_samples and transition to FINISHED, even if loop-on was requested
 * (the flag cannot invent a loop that the file does not define). */
static void test_no_loop_end(void)
{
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    uint64_t done = 0;
    int early = 0;

    note("== no-loop file plays to the end and stops ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    col_path("o_Wi-Fi_waiting32.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open o_Wi-Fi_waiting32.brstm: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(!s.file_loop, "file has no loop metadata");
    player_set_loop(p, 1); /* loop requested on a file without loop info */
    if (player_play(p) != 0) {
        check(0, "start playback of the no-loop file");
        player_destroy(p);
        return;
    }

    while (done < s.total_samples) {
        uint32_t got = player_render(p, g_chunk, 4096);
        if (got == 0) {
            early = 1;
            break;
        }
        done += got;
    }
    check(!early && done == s.total_samples,
          "rendered exactly total_samples (%llu)",
          (unsigned long long)done);

    {
        uint32_t got = player_render(p, g_chunk, 4096);
        s = player_get_state(p);
        check(got == 0 && s.state == PLAYER_FINISHED,
              "next render returns 0 and state is FINISHED");
        check(s.wraps == 0 && s.position == s.total_samples,
              "no wrap on a loopless file, position = total (%llu)",
              (unsigned long long)s.position);
    }

    player_stop(p);
    check(player_get_state(p).state == PLAYER_STOPPED
          && player_get_state(p).position == 0,
          "stop resets to position 0");
    player_close(p);
    player_destroy(p);
}

/* Loop-off group: a file that HAS loop metadata but with the loop disabled
 * must behave like a no-loop file: play to total_samples and finish. */
static void test_loop_off_looped(void)
{
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    uint64_t done = 0;

    note("== looped file with the loop disabled stops at total ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    col_path("o_FanfareGP1_32.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open o_FanfareGP1_32.brstm: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(s.file_loop && s.loop_enabled, "loop metadata found and enabled");
    player_set_loop(p, 0);
    if (player_play(p) != 0) {
        check(0, "start playback of the 4-channel file");
        player_destroy(p);
        return;
    }

    while (done < s.total_samples) {
        uint32_t got = player_render(p, g_chunk, 4096);
        if (got == 0)
            break;
        done += got;
    }
    /* The last block ends exactly at total_samples, so the loop above exits
     * with the state still PLAYING; one more call is what observes the end
     * (returns 0 and transitions to FINISHED), the same call the audio
     * engine thread performs. */
    check(player_render(p, g_chunk, 4096) == 0,
          "render at the end of the file produces no more audio");
    s = player_get_state(p);
    check(done == s.total_samples && s.state == PLAYER_FINISHED
          && s.wraps == 0,
          "stops exactly at total_samples with the loop off (4 ch, %llu)",
          (unsigned long long)done);
    player_close(p);
    player_destroy(p);
}

/* Seek: after a jump the PCM must be identical to the same frames of a
 * linear pass (loop off). BRSTM blocks carry no embedded history, so
 * player_seek() re-decodes from the closest exact anchor (the captured
 * loop-block state, or the file start): every position is exact. */
static void test_seek(void)
{
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    uint32_t total, ch, ref_len, got, i, t, want;
    uint32_t targets[4];
    int16_t *ref;

    note("== seek (exact jump to a source position) ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    check(player_seek(p, 12345) == -1, "seek without a file returns -1");

    col_path("tKD_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open tKD_N.brstm: %s", err);
        player_destroy(p);
        return;
    }

    s = player_get_state(p);
    total = (uint32_t)s.total_samples;
    ch = s.channels;

    /* Linear reference pass with the loop off. */
    player_set_loop(p, 0);
    ref = (int16_t *)calloc((size_t)total * ch, sizeof(int16_t));
    if (!ref) {
        check(0, "reference buffer allocated for %u frames", total);
        player_close(p);
        player_destroy(p);
        return;
    }
    check(player_play(p) == 0, "linear pass starts");
    ref_len = 0;
    while (ref_len < total) {
        got = player_render(p, g_chunk, 4096);
        if (got == 0)
            break;
        memcpy(ref + (size_t)ref_len * ch, g_chunk,
               (size_t)got * ch * sizeof(int16_t));
        ref_len += got;
    }
    check(ref_len == total, "linear pass covers the file (%u frames)", ref_len);
    /* Final call drives the end-of-stream transition (same as loopoff). */
    got = player_render(p, g_chunk, 16);
    check(got == 0 && player_get_state(p).state == PLAYER_FINISHED,
          "loop-off pass ends FINISHED");

    /* Jump targets: file start, mid-file before the loop, the loop point
     * itself and near the end. */
    targets[0] = 1;
    targets[1] = 100000u;
    targets[2] = s.loop_start;
    targets[3] = total - 777u;
    for (i = 0; i < 4; i++) {
        t = targets[i];
        want = total - t;
        if (want > 1024)
            want = 1024;
        check(player_seek(p, t) == 0, "seek(%u) accepted", t);
        check(player_get_state(p).position == t,
              "position reports %u right after seek", t);
        check(player_play(p) == 0, "play resumes from the seeked point");
        got = player_render(p, g_chunk, want);
        check(got == want, "render returns %u frames after seek", want);
        check(memcmp(g_chunk, ref + (size_t)t * ch,
                     (size_t)want * ch * sizeof(int16_t)) == 0,
              "PCM after seek(%u) matches the linear pass", t);
        (void)player_stop(p);
    }

    /* Seek to the end behaves like a finished pass: play restarts at 0. */
    check(player_seek(p, total) == 0 && player_get_state(p).position == total,
          "seek clamps to total_samples");
    check(player_play(p) == 0, "play after seek-to-end starts");
    got = player_render(p, g_chunk, 4096);
    check(got == 4096 && memcmp(g_chunk, ref, 4096 * ch * sizeof(int16_t)) == 0,
          "play after seek-to-end restarts from the beginning");
    (void)player_stop(p);

    /* Seek while playing: rendering continues exactly at the target, with
     * the captured loop state as anchor (target after loop_start). */
    player_set_loop(p, 1);
    (void)player_play(p);
    (void)player_render(p, g_chunk, 4096);
    t = 2000000u;
    check(player_seek(p, t) == 0 && player_get_state(p).position == t,
          "seek while playing updates the position");
    got = player_render(p, g_chunk, 1024);
    check(got == 1024 && memcmp(g_chunk, ref + (size_t)t * ch,
                                1024 * ch * sizeof(int16_t)) == 0,
          "PCM after a seek while playing matches the linear pass");

    /* Seek near the end with the loop on: the wrap lands on loop_start and
     * the audio keeps matching the reference (exact loop state). */
    t = total - 500u;
    check(player_seek(p, t) == 0, "seek(%u) near the end with loop on", t);
    got = player_render(p, g_chunk, 2000);
    check(got == 2000
          && memcmp(g_chunk, ref + (size_t)t * ch,
                    500 * ch * sizeof(int16_t)) == 0
          && memcmp(g_chunk + 500 * ch,
                    ref + (size_t)s.loop_start * ch,
                    1500 * ch * sizeof(int16_t)) == 0,
          "wrap after a seek matches the reference at loop_start");

    free(ref);
    player_close(p);
    player_destroy(p);
}

static void test_channel_pair(void)
{
    /* A selected channel pair must render exactly that stereo pair; the
     * default keeps emitting every source channel (fold happens later, at
     * write time). Runs headless, so the check is bit-exact. */
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    uint32_t total, ch, ref_len, got, i, t;
    int16_t *ref;

    note("== channel pairs (version selection on multichannel files) ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    check(player_set_channel_pair(p, 0) == -1,
          "pair selection without a file returns -1");

    col_path(k_short, path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open %s: %s", k_short, err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    ch = s.channels;
    total = (uint32_t)s.total_samples;
    check(ch == 4 && s.pair_count == 2 && s.pair_sel == -1,
          "4ch file: pair_count=2, default mix-all (ch=%u count=%u sel=%d)",
          ch, s.pair_count, s.pair_sel);
    check(player_set_channel_pair(p, 0) == 0, "pair 1 accepted");
    check(player_get_state(p).pair_sel == 0, "snapshot reports pair 1");
    check(player_set_channel_pair(p, 2) == -1, "pair 3 rejected on 4 channels");
    check(player_set_channel_pair(p, -2) == -1, "-2 rejected");
    check(player_set_channel_pair(p, -1) == 0,
          "mix-all restored for the reference pass");

    /* Linear reference pass (loop off) over every source channel. */
    player_set_loop(p, 0);
    ref = (int16_t *)calloc((size_t)total * ch, sizeof(int16_t));
    if (!ref) {
        check(0, "reference buffer allocated for %u frames", total);
        player_close(p);
        player_destroy(p);
        return;
    }
    check(player_play(p) == 0, "linear pass starts");
    ref_len = 0;
    while (ref_len < total) {
        got = player_render(p, g_chunk, 4096);
        if (got == 0)
            break;
        memcpy(ref + (size_t)ref_len * ch, g_chunk,
               (size_t)got * ch * sizeof(int16_t));
        ref_len += got;
    }
    check(ref_len == total, "linear pass covers the file (%u frames)",
          ref_len);
    if (ref_len != total) {
        free(ref);
        player_close(p);
        player_destroy(p);
        return;
    }
    (void)player_render(p, g_chunk, 16); /* drives the end-of-stream state */

    /* Default output equals every source channel. */
    check(player_play(p) == 0, "play after the reference pass");
    got = player_render(p, g_chunk, 4096);
    check(got == 4096 && memcmp(g_chunk, ref, (size_t)4096 * ch * 2) == 0,
          "default render matches all source channels");
    (void)player_stop(p);

    /* Each pair renders its own channels as plain stereo. */
    for (t = 0; t < 2; t++) {
        int ok = 1;
        check(player_set_channel_pair(p, (int)t) == 0, "select pair %u", t);
        check(player_get_state(p).pair_sel == (int32_t)t,
              "snapshot reports pair %u", t);
        check(player_play(p) == 0, "play with pair %u", t);
        got = player_render(p, g_chunk, 4096);
        for (i = 0; i < got; i++) {
            if (g_chunk[i * 2] != ref[i * ch + t * 2]
                || g_chunk[i * 2 + 1] != ref[i * ch + t * 2 + 1]) {
                ok = 0;
                break;
            }
        }
        check(got == 4096 && ok, "pair %u render equals channels %u+%u",
              t, t * 2, t * 2 + 1);
        (void)player_stop(p);
    }

    /* Back to mix-all: the full source channel set comes back. */
    check(player_set_channel_pair(p, -1) == 0, "mix-all restored");
    check(player_play(p) == 0, "play after restore");
    got = player_render(p, g_chunk, 4096);
    check(got == 4096 && memcmp(g_chunk, ref, (size_t)4096 * ch * 2) == 0,
          "render matches all source channels after restore");
    (void)player_stop(p);

    /* A seek with a pair active stays aligned to the reference. */
    check(player_set_channel_pair(p, 1) == 0, "pair 2 selected for the seek");
    t = 500000u;
    check(player_seek(p, t) == 0, "seek(%u) with a pair selected", t);
    check(player_play(p) == 0, "play after seek");
    got = player_render(p, g_chunk, 1024);
    {
        int ok = 1;
        for (i = 0; i < got; i++) {
            if (g_chunk[i * 2] != ref[(t + i) * ch + 2]
                || g_chunk[i * 2 + 1] != ref[(t + i) * ch + 3]) {
                ok = 0;
                break;
            }
        }
        check(got == 1024 && ok, "seek + pair 2 renders at the target");
    }
    free(ref);
    player_close(p);
    player_destroy(p);

    /* Pair availability scales with the channel count. */
    p = player_create(0);
    if (!p) {
        check(0, "headless player created (8ch)");
        return;
    }
    col_path("STRM_N_KOOPA_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open STRM_N_KOOPA_N.brstm: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(s.channels == 8 && s.pair_count == 4 && s.pair_sel == -1,
          "8ch file: pair_count=4 (ch=%u count=%u sel=%d)",
          s.channels, s.pair_count, s.pair_sel);
    check(player_set_channel_pair(p, 3) == 0, "pair 4 accepted on 8 channels");
    check(player_set_channel_pair(p, 4) == -1, "pair 5 rejected on 8 channels");
    check(player_set_channel_pair(p, -1) == 0, "mix-all accepted");
    player_close(p);
    player_destroy(p);

    /* Stereo files expose no pairs. */
    p = player_create(0);
    if (!p) {
        check(0, "headless player created (stereo)");
        return;
    }
    col_path("tKD_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open tKD_N.brstm: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(s.pair_count == 0, "stereo file exposes no pairs");
    check(player_set_channel_pair(p, 0) == -1,
          "pair selection rejected on a stereo file");
    check(player_set_channel_pair(p, -1) == 0, "mix-all still accepted");
    player_close(p);
    player_destroy(p);
}

/* Memory group: render ~16M frames (many loop iterations) headless and
 * require the working set to stay flat. The baseline is taken after the
 * first 1M frames so one-time allocations (circular buffer, ADPC state)
 * are already paid and only growth would be flagged. */
static void test_memory(void)
{
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    uint64_t done = 0;
    const uint64_t target = 16000000;
    SIZE_T w0 = 0, w1;
    double delta;

    note("== memory stays flat while looping ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    col_path("104_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open 104_N.brstm: %s", err);
        player_destroy(p);
        return;
    }
    if (player_play(p) != 0) {
        check(0, "start playback for the memory test");
        player_destroy(p);
        return;
    }
    while (done < target) {
        uint32_t got = player_render(p, g_chunk, 4096);
        if (got == 0)
            break;
        done += got;
        if (done >= 1000000 && w0 == 0)
            w0 = working_set();
    }
    w1 = working_set();
    s = player_get_state(p);
    /* Render works in whole chunks, so it stops at the first chunk that
     * crosses `target` (overshoot < 4096 frames). */
    check(done >= target && done < target + 4096,
          "rendered %llu frames through the loop (target %llu)",
          (unsigned long long)done, (unsigned long long)target);
    check(s.wraps >= 1, "sustained render kept looping (wraps=%u)", s.wraps);
    delta = (double)w1 - (double)w0;
    note("working set: %.1f MB warm -> %.1f MB final (%+.1f KB)",
         (double)w0 / 1048576.0, (double)w1 / 1048576.0, delta / 1024.0);
    check(delta <= 4.0 * 1024.0 * 1024.0,
          "working set growth under 4 MB across %llu looped frames",
          (unsigned long long)target);
    player_close(p);
    player_destroy(p);
}

/* Headless transport group: the state machine (play/pause/resume/stop) with
 * no audio device, including a <50 ms latency budget per call - the engine
 * must never block the caller on the device thread. */
static void test_transport_headless(void)
{
    Player *p;
    char path[600], err[512];
    double lat, max_lat = 0.0;
    uint32_t got;
    uint64_t pos_a, pos_b;

    note("== transport without audio (headless state machine) ==");
    p = player_create(0);
    if (!p) {
        check(0, "headless player created");
        return;
    }
    col_path("tKD_N.brstm", path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open tKD_N.brstm: %s", err);
        player_destroy(p);
        return;
    }

    {
        int rc;
        double t0 = now_ms();
        rc = player_play(p);
        lat = now_ms() - t0;
    }
    if (lat > max_lat)
        max_lat = lat;
    check(player_get_state(p).state == PLAYER_PLAYING && lat < 50.0,
          "play -> PLAYING in %.2f ms", lat);

    got = player_render(p, g_chunk, 4096);
    pos_a = player_get_state(p).position;
    check(got == 4096 && pos_a == 4096,
          "render advances the position (%llu)", (unsigned long long)pos_a);

    lat = now_ms();
    (void)player_pause(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(player_get_state(p).state == PLAYER_PAUSED && lat < 50.0,
          "pause -> PAUSED in %.2f ms", lat);
    got = player_render(p, g_chunk, 4096);
    check(got == 0 && player_get_state(p).position == pos_a,
          "render is blocked while paused");

    lat = now_ms();
    (void)player_play(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(player_get_state(p).state == PLAYER_PLAYING && lat < 50.0,
          "resume -> PLAYING in %.2f ms", lat);
    (void)player_render(p, g_chunk, 4096);
    pos_b = player_get_state(p).position;
    check(pos_b > pos_a, "render resumes advancing (%llu)",
          (unsigned long long)pos_b);

    lat = now_ms();
    (void)player_stop(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(player_get_state(p).state == PLAYER_STOPPED
          && player_get_state(p).position == 0 && lat < 50.0,
          "stop -> STOPPED at position 0 in %.2f ms", lat);
    check(max_lat < 50.0, "slowest transport call was %.2f ms (< 50 ms)",
          max_lat);

    player_close(p);
    player_destroy(p);
}

/* Isolate handle growth: raw waveOut cycles (winmm only) vs full player
 * cycles (engine + device + file), with counts at every phase.
 *
 * Measured behaviour on the test machine: the FIRST waveOutOpen ever in the
 * process pays a one-time winmm/driver initialization (~100 handles) that
 * survives close; every later open/close cycle is balanced. Cycle 0 is
 * therefore a warm-up and the balance check compares the cycles after it. */
static void test_resource_probe(void)
{
    note("== resource probe ==");
    {
        HWAVEOUT raw = NULL;
        WAVEFORMATEX fmt;
        DWORD hbase = 0;
        int i;
        memset(&fmt, 0, sizeof fmt);
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 44100;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 4;
        fmt.nAvgBytesPerSec = 44100 * 4;
        for (i = 0; i < 5; i++) {
            MMRESULT r = waveOutOpen(&raw, WAVE_MAPPER, &fmt, 0, 0,
                                     CALLBACK_NULL);
            DWORD hopen = r == MMSYSERR_NOERROR ? handle_count() : 0;
            if (r == MMSYSERR_NOERROR)
                waveOutClose(raw);
            if (i == 0) {
                hbase = handle_count();
                note("raw waveOut warmup: open_r=%u handles %lu -> %lu -> %lu",
                     r, hbase, hopen, handle_count());
            } else {
                note("raw waveOut cycle %d: open_r=%u handles %lu -> %lu",
                     i, r, hopen, handle_count());
            }
        }
        note("raw waveOut net after warmup: %+ld handles over 4 cycles",
             (long)handle_count() - (long)hbase);
        check(handle_count() <= hbase + 2,
              "raw waveOut open/close cycles stay balanced (%lu vs %lu)",
              handle_count(), hbase);
    }
    {
        char path[600], err[512];
        DWORD hbase = 0;
        Player *p;
        int i;
        col_path("tKD_N.brstm", path, sizeof path);
        p = player_create(1);
        if (!p) {
            check(0, "audio player created");
            return;
        }
        for (i = 0; i < 3; i++) {
            err[0] = '\0';
            if (player_open(p, path, err, sizeof err) != 0) {
                check(0, "probe open %d failed: %s", i, err);
                break;
            }
            (void)player_play(p);
            (void)wait_state(p, PLAYER_PLAYING, 3000);
            (void)player_stop(p);
            (void)wait_state(p, PLAYER_STOPPED, 2000);
            player_close(p);
            note("player cycle %d (open/play/stop/close): %lu handles",
                 i, handle_count());
            if (i == 0)
                hbase = handle_count();
        }
        player_destroy(p);
        note("after destroy: %lu (warm cycle %lu)", handle_count(), hbase);
        check(hbase > 0 && handle_count() <= hbase + 4,
              "player open/play/stop/close cycles stay balanced and destroy "
              "releases the player (%lu vs %lu)",
              handle_count(), hbase);
    }
}

/* Real-device transport group: the same lifecycle on waveOut, plus what
 * only a real device can show - the multichannel open path (native or
 * stereo fold), the winmm failed-open handle tax documented in
 * audio_out.c, and the handle/memory balance across open/play/stop cycles
 * and after destroy. */
static void test_transport_audio(void)
{
    Player *p;
    char path[600], path2[600], err[512];
    PlayerSnapshot s;
    double lat, max_lat = 0.0;
    DWORD h0, h1, hm1 = 0, hm3 = 0;
    SIZE_T m0, m1;
    int i;

    note("== transport on the real audio device ==");
    winmm_warmup();
    h0 = handle_count();
    m0 = working_set();

    p = player_create(1);
    if (!p) {
        check(0, "audio player created");
        return;
    }
    h1 = handle_count();
    col_path("tKD_N.brstm", path, sizeof path);
    col_path("104_N.brstm", path2, sizeof path2);

    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open tKD_N.brstm: %s", err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(s.audio_active && s.audio_error == 0,
          "waveOut device open with the file (rate %u -> %u)",
          s.sample_rate, s.device_rate);
    note("handles after initial open: %lu", (unsigned long)handle_count());

    lat = now_ms();
    (void)player_play(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(wait_state(p, PLAYER_PLAYING, 4000), "play reaches PLAYING");
    check(player_get_state(p).audio_error == 0, "no audio error while playing");
    note("handles after first play: %lu", (unsigned long)handle_count());

    /* API latency while the engine thread is decoding and queuing. */
    {
        double worst = 0.0, t0, t1;
        for (i = 0; i < 50; i++) {
            t0 = now_ms();
            (void)player_get_state(p);
            t1 = now_ms();
            if (t1 - t0 > worst)
                worst = t1 - t0;
            Sleep(0);
        }
        note("player_get_state worst latency while playing: %.3f ms", worst);
        check(worst < 100.0, "engine API stays responsive (< 100 ms)");
    }

    lat = now_ms();
    (void)player_pause(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(wait_state(p, PLAYER_PAUSED, 2000), "pause reaches PAUSED");
    {
        uint64_t pa = player_get_state(p).position;
        Sleep(300);
        uint64_t pb = player_get_state(p).position;
        check(pa == pb, "position frozen while paused (%llu)",
              (unsigned long long)pa);
    }

    lat = now_ms();
    (void)player_play(p);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    check(wait_state(p, PLAYER_PLAYING, 2000), "resume reaches PLAYING");
    {
        uint64_t pa = player_get_state(p).position;
        Sleep(700);
        uint64_t pb = player_get_state(p).position;
        check(pb > pa, "position advances after resume (%llu -> %llu)",
              (unsigned long long)pa, (unsigned long long)pb);
    }

    (void)player_stop(p);
    check(wait_state(p, PLAYER_STOPPED, 2000)
          && player_get_state(p).position == 0,
          "stop reaches STOPPED at position 0");
    note("handles after pause/resume/stop: %lu", (unsigned long)handle_count());

    /* Track change while playing. */
    (void)player_play(p);
    (void)wait_state(p, PLAYER_PLAYING, 4000);
    lat = now_ms();
    err[0] = '\0';
    i = player_open(p, path2, err, sizeof err);
    lat = now_ms() - lat;
    if (lat > max_lat)
        max_lat = lat;
    s = player_get_state(p);
    check(i == 0 && s.state == PLAYER_STOPPED && s.sample_rate == 48000
          && s.total_samples == 8771278,
          "track change while playing loads the new file and stops "
          "(%.0f ms)", lat);
    note("handles after track change: %lu", (unsigned long)handle_count());
    check(max_lat < 500.0, "slowest engine call during transport < 500 ms "
          "(%.2f ms)", max_lat);

    (void)player_play(p);
    check(wait_state(p, PLAYER_PLAYING, 4000), "play after track change");
    (void)player_stop(p);

    /* Multichannel playback: native when the device accepts >2 channels,
     * the stereo fold otherwise; in both cases audio must keep running. */
    {
        char path4[600];
        uint64_t pa, pb;
        col_path("o_FanfareGP1_32.brstm", path4, sizeof path4);
        err[0] = '\0';
        if (player_open(p, path4, err, sizeof err) == 0) {
            (void)player_play(p);
            check(wait_state(p, PLAYER_PLAYING, 4000),
                  "4-channel file starts playing");
            s = player_get_state(p);
            check(s.audio_error == 0,
                  "4-channel device open succeeded (folded=%d)",
                  s.audio_folded);
            pa = s.position;
            Sleep(600);
            pb = player_get_state(p).position;
            check(pb > pa, "4-channel playback advances (%llu -> %llu)",
                  (unsigned long long)pa, (unsigned long long)pb);
            note("4ch output path: %s",
                 s.audio_folded ? "stereo fold (device rejected 4ch)"
                                : "native multichannel");
            (void)player_stop(p);
            note("handles after 4ch play/stop: %lu",
                 (unsigned long)handle_count());
        } else {
            check(0, "open o_FanfareGP1_32.brstm: %s", err);
        }
    }

    /* Invalid file in audio mode, then recovery. */
    {
        char bad[700];
        snprintf(bad, sizeof bad, "%s\\bad_magic.brstm", g_inv);
        if (GetFileAttributesA(bad) != INVALID_FILE_ATTRIBUTES) {
            err[0] = '\0';
            check(player_open(p, bad, err, sizeof err) != 0,
                  "audio mode rejects an invalid file");
            check(player_get_state(p).audio_error == 0
                  && player_get_state(p).state == PLAYER_STOPPED,
                  "state stays clean after the rejection");
            err[0] = '\0';
            check(player_open(p, path, err, sizeof err) == 0,
                  "valid file opens after the rejection");
            note("handles after invalid+valid open: %lu",
                 (unsigned long)handle_count());
        } else {
            note("bad_magic.brstm not found, rejection check skipped");
        }
    }

    /* Open/play/stop cycles must not grow handles or memory. */
    for (i = 0; i < 3; i++) {
        err[0] = '\0';
        if (player_open(p, path, err, sizeof err) != 0) {
            check(0, "cycle %d open failed: %s", i, err);
            break;
        }
        (void)player_play(p);
        (void)wait_state(p, PLAYER_PLAYING, 4000);
        Sleep(150);
        (void)player_stop(p);
        (void)wait_state(p, PLAYER_STOPPED, 2000);
        if (i == 0)
            hm1 = handle_count();
    }
    hm3 = handle_count();
    note("handles: baseline %lu, after create %lu, cycles %lu",
         (unsigned long)h0, (unsigned long)h1, (unsigned long)hm3);

    /* The engine itself must be stable across cycles: open/play/stop/close
     * is net-zero for our code. This is the leak check for the player. */
    check(hm3 >= hm1 && hm3 - hm1 <= 4,
          "handle count stable across open/play/stop cycles (%lu -> %lu)",
          (unsigned long)hm1, (unsigned long)hm3);

    player_close(p);
    player_destroy(p);

    /* The absolute resting level after destroy is not a reliable leak signal
     * on this machine: a FAILED waveOutOpen leaks ~3 handles inside winmm
     * itself (measured in isolation with a raw-API probe: 10 failed
     * 4-channel opens cost +30 handles, with no player involved). The stereo
     * fallback retries after such a failure, so each multichannel open pays
     * that winmm-internal tax. Our own leak signal is the cycle check above;
     * what this settles is that the process does not keep growing without
     * bound. */
    {
        DWORD settled = handle_count();
        int waited = 0;
        while (waited < 500 && settled > h1 + 40) {
            Sleep(50);
            settled = handle_count();
            waited += 50;
        }
        m1 = working_set();
        note("handles after destroy: %lu (settled %lu after %d ms; h0 %lu, "
             "h1 %lu); working set %.1f -> %.1f MB",
             handle_count(), settled, waited, (unsigned long)h0,
             (unsigned long)h1, (double)m0 / 1048576.0,
             (double)m1 / 1048576.0);
        check(settled <= h1 + 40,
              "no unbounded handle growth after destroy (%lu vs create %lu; "
              "includes the winmm failed-open tax)",
              (unsigned long)settled, (unsigned long)h1);
    }
    check((double)m1 <= (double)m0 + 8.0 * 1024.0 * 1024.0,
          "memory growth under 8 MB after close (%+.1f MB)",
          ((double)m1 - (double)m0) / 1048576.0);
}

/* Run the current file from the start to PLAYER_FINISHED on the real device
 * and return the wall-clock seconds, or -1 on failure with the state dump
 * in *ok = 0. Used by test_realtime, which needs measured durations. */
static double play_to_finished(Player *p, double timeout_s, int *ok)
{
    double t0;
    *ok = 0;
    (void)player_stop(p);
    t0 = now_ms();
    if (player_play(p) != 0) {
        PlayerSnapshot q = player_get_state(p);
        note("play failed: state=%d audio_active=%d audio_error=%u "
             "decode_failed=%d",
             (int)q.state, q.audio_active, q.audio_error, q.decode_failed);
        return -1.0;
    }
    if (!wait_state(p, PLAYER_FINISHED, (DWORD)(timeout_s * 1000.0))) {
        PlayerSnapshot q = player_get_state(p);
        note("no FINISHED within %.0f s: state=%d position=%llu/%llu "
             "audio_error=%u decode_failed=%d",
             timeout_s, (int)q.state, (unsigned long long)q.position,
             (unsigned long long)q.total_samples, q.audio_error,
             q.decode_failed);
        return -1.0;
    }
    *ok = 1;
    return (now_ms() - t0) / 1000.0;
}

/* Realtime group: proves the speed change works through the actual audio
 * device, comparing wall-clock durations of a full non-looped pass at 1.0x
 * and at the FLSM speed (each within 4%/0.5 s of the expected duration),
 * then letting a real-time loop wrap happen and checking the wrap lands on
 * loop_start with the position staying inside [loop_start, total). */
static void test_realtime(void)
{
    Player *p;
    char path[600], err[512];
    PlayerSnapshot s;
    double expect_normal, expect_flsm, wall_normal, wall_flsm;
    double tol_normal, tol_flsm;
    int ok_a = 0, ok_b = 0;
    DWORD h0;
    SIZE_T m0;

    note("== real-time playback: 1.0x vs FLSM speed vs loop wrap ==");
    m0 = working_set();

    p = player_create(1);
    if (!p) {
        check(0, "audio player created");
        return;
    }
    col_path(k_short, path, sizeof path);
    err[0] = '\0';
    if (player_open(p, path, err, sizeof err) != 0) {
        check(0, "open %s: %s", k_short, err);
        player_destroy(p);
        return;
    }
    s = player_get_state(p);
    check(s.audio_active && s.audio_error == 0,
          "device opened for the short file (folded=%d)", s.audio_folded);
    /* Baseline after the device exists: later checks compare leak-free. */
    h0 = handle_count();
    note("%s: %u Hz, %u ch, %llu samples (%.1f s), loop=%d @ %u",
         k_short, s.sample_rate, s.channels,
         (unsigned long long)s.total_samples,
         (double)s.total_samples / s.sample_rate, s.file_loop, s.loop_start);
    check(s.file_loop, "short reference file carries loop metadata");

    /* Duration at 1.0x: loop off, plays straight to the end. */
    player_set_loop(p, 0);
    player_set_speed(p, PLAYER_SPEED_SCALE);
    s = player_get_state(p);
    expect_normal = (double)s.total_samples / (double)s.sample_rate;
    wall_normal = play_to_finished(p, 45.0, &ok_a);
    tol_normal = expect_normal * 0.04 > TOL_SEC ? expect_normal * 0.04 : TOL_SEC;
    check(ok_a && fabs(wall_normal - expect_normal) <= tol_normal,
          "duration at 1.0x: measured %.2f s, expected %.2f s (+-%.2f)",
          wall_normal, expect_normal, tol_normal);

    /* Duration at the FLSM speed: device rate must be the verified 51269. */
    player_set_speed(p, PLAYER_FLSM_SPEED_PPM);
    s = player_get_state(p);
    check(s.device_rate == 51269,
          "device rate at FLSM speed for a 48000 Hz file: %u (expected 51269)",
          s.device_rate);
    expect_flsm = (double)s.total_samples / (double)s.device_rate;
    wall_flsm = play_to_finished(p, 45.0, &ok_b);
    tol_flsm = expect_flsm * 0.04 > TOL_SEC ? expect_flsm * 0.04 : TOL_SEC;
    check(ok_b && fabs(wall_flsm - expect_flsm) <= tol_flsm,
          "duration at FLSM speed: measured %.2f s, expected %.2f s (+-%.2f)",
          wall_flsm, expect_flsm, tol_flsm);
    if (ok_a && ok_b) {
        note("measured speedup %.4f vs theoretical %.4f (device %u / %u)",
             wall_normal / wall_flsm,
             (double)s.device_rate / (double)s.sample_rate,
             s.device_rate, s.sample_rate);
        check(wall_flsm < wall_normal - 0.5,
              "FLSM playback is measurably shorter (%.2f s < %.2f s)",
              wall_flsm, wall_normal);
    }

    /* Real-time loop wrap under the FLSM speed, bounds-checked. */
    if (s.file_loop) {
        double t0, t_wrap = -1.0;
        int wrapped = 0, bad_position = 0, left_playing = 0;
        player_set_loop(p, 1);
        player_set_speed(p, PLAYER_FLSM_SPEED_PPM);
        s = player_get_state(p);
        expect_flsm = (double)s.total_samples / (double)s.device_rate;
        (void)player_stop(p);
        t0 = now_ms();
        if (player_play(p) == 0) {
            while (now_ms() - t0 < 40000.0) {
                PlayerSnapshot q = player_get_state(p);
                if (q.position > q.total_samples) {
                    bad_position = 1;
                    break;
                }
                if (q.wraps > 0) {
                    wrapped = 1;
                    t_wrap = (now_ms() - t0) / 1000.0;
                    break;
                }
                if (q.state != PLAYER_PLAYING) {
                    left_playing = 1;
                    break;
                }
                Sleep(50);
            }
        }
        if (!wrapped) {
            PlayerSnapshot q = player_get_state(p);
            note("wrap run ended: state=%d position=%llu/%llu wraps=%u "
                 "audio_error=%u decode_failed=%d",
                 (int)q.state, (unsigned long long)q.position,
                 (unsigned long long)q.total_samples, q.wraps, q.audio_error,
                 q.decode_failed);
        }
        check(bad_position == 0 && left_playing == 0 && wrapped,
              "loop wrapped in real time at %.2f s (expected ~%.2f s)",
              t_wrap, expect_flsm);
        s = player_get_state(p);
        check(s.last_wrap_position == s.loop_start,
              "wrap jumped exactly to loop_start (%u)", s.last_wrap_position);
        if (wrapped) {
            double tol = expect_flsm * 0.04 > TOL_SEC ? expect_flsm * 0.04
                                                      : TOL_SEC;
            check(fabs(t_wrap - expect_flsm) <= tol,
                  "wrap time matches the FLSM duration (%.2f vs %.2f s)",
                  t_wrap, expect_flsm);
            /* The position must stay inside the loop section afterwards. */
            {
                int in_bounds = 1;
                double end = now_ms() + 1000.0;
                while (now_ms() < end) {
                    PlayerSnapshot q = player_get_state(p);
                    if (q.position < q.loop_start
                        || q.position > q.total_samples)
                        in_bounds = 0;
                    Sleep(50);
                }
                check(in_bounds,
                      "position stays inside [loop_start, total) after the "
                      "wrap (observed over 1 s)");
            }
        }
        (void)player_stop(p);
    } else {
        check(0, "short reference file has loop metadata (needed for wrap)");
    }

    player_close(p);
    player_destroy(p);
    note("handles %lu -> %lu, working set %.1f -> %.1f MB",
         (unsigned long)h0, (unsigned long)handle_count(),
         (double)m0 / 1048576.0, (double)working_set() / 1048576.0);
    check(handle_count() <= h0 + 8, "no handle growth after the real-time run");
}

/* ---------------------------------------------------------------- main -- */

/* Parse "--flag value" pairs: returns 1 and consumes both tokens when argv[*i]
 * matches flag, otherwise 0. Keeps the main() argument loop readable. */
static int need_arg(int argc, char **argv, int *i, const char *flag,
                    char *out, size_t n)
{
    if (strcmp(argv[*i], flag) == 0 && *i + 1 < argc) {
        snprintf(out, n, "%s", argv[*i + 1]);
        *i += 1;
        return 1;
    }
    return 0;
}

/* Entry point: parse the required directory arguments (--collection,
 * --invalid, --vgmstream, --work plus the optional --report/--only), then
 * run each group. Exit 0 = all checks passed, 1 = any failure, 2 = usage. */
int main(int argc, char **argv)
{
    int i, have_col = 0, have_inv = 0, have_vgm = 0, have_work = 0;
    char report[600] = "";
    char only[64] = "";

    for (i = 1; i < argc; i++) {
        if (need_arg(argc, argv, &i, "--collection", g_col, sizeof g_col))
            have_col = 1;
        else if (need_arg(argc, argv, &i, "--invalid", g_inv, sizeof g_inv))
            have_inv = 1;
        else if (need_arg(argc, argv, &i, "--vgmstream", g_vgm, sizeof g_vgm))
            have_vgm = 1;
        else if (need_arg(argc, argv, &i, "--work", g_work, sizeof g_work))
            have_work = 1;
        else if (need_arg(argc, argv, &i, "--report", report, sizeof report))
            ;
        else if (need_arg(argc, argv, &i, "--only", only, sizeof only))
            ;
        else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!have_col || !have_inv || !have_vgm || !have_work) {
        fprintf(stderr,
                "usage: player_tests --collection <dir> --invalid <dir> "
                "--vgmstream <test.exe> --work <dir> "
                "[--report <file>] [--only <group>]\n"
                "groups: metadata speed invalid loop noloop loopoff seek pair "
                "memory th probe ta realtime\n");
        return 2;
    }
    if (report[0]) {
        /* Narrow fputs() on a ccs=UTF-8 stream aborts on this CRT (measured:
         * 0xC0000409 via a minimal probe), and the report content is ASCII,
         * so the file is written as plain bytes. */
        g_report = fopen(report, "w");
        if (!g_report)
            fprintf(stderr, "warning: cannot open report %s\n", report);
    }

    CreateDirectoryA(g_work, NULL);
    emit("=== T2 player tests ===");
    note("collection: %s", g_col);

#define RUN(group, fn) \
    do { if (!only[0] || strcmp(only, group) == 0) fn(); } while (0)

    RUN("metadata", test_metadata);
    RUN("speed", test_speed_mapping);
    RUN("speed", test_speed_chain);
    RUN("invalid", test_invalid);
    RUN("loop", test_loop_reference);
    RUN("noloop", test_no_loop_end);
    RUN("loopoff", test_loop_off_looped);
    RUN("seek", test_seek);
    RUN("pair", test_channel_pair);
    RUN("memory", test_memory);
    RUN("th", test_transport_headless);
    RUN("probe", test_resource_probe);
    RUN("ta", test_transport_audio);
    RUN("realtime", test_realtime);
#undef RUN

    {
        char line[200];
        snprintf(line, sizeof line, "RESULT: %s (%d checks, %d failed)",
                 g_fail ? "FAIL" : "PASS", g_pass + g_fail, g_fail);
        emit(line);
    }
    if (g_report) {
        fclose(g_report);
        g_report = NULL;
    }
    return g_fail ? 1 : 0;
}
