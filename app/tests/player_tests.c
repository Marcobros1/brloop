/* Automated tests for the T2 BRSTM player (console, no UI involved).
 *
 * Covers, against real collection files:
 *   1. Metadata of 44100/48000/51269 stereo, 4ch, 8ch and no-loop files.
 *   2. Rejection of every invalid mutant from make_invalid_cases.py and
 *      recovery afterwards.
 *   3. Speed -> device_rate mapping, including both measured FLSM
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

#define TOL_SEC 0.5 /* absolute floor of the wall-clock tolerance */

static int g_pass, g_fail;
static FILE *g_report;

static char g_col[512];
static char g_inv[512];
static char g_vgm[512];
static char g_work[512];

static int16_t g_chunk[4096 * 16]; /* render scratch for streaming tests */

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

static void col_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s\\%s", g_col, name);
}

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)freq.QuadPart;
}

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

static DWORD handle_count(void)
{
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return n;
}

static SIZE_T working_set(void)
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

typedef struct {
    uint8_t *bytes;
    size_t size;
    uint16_t channels;
    uint16_t bits;
    const uint8_t *pcm;
    uint32_t pcm_bytes;
    uint32_t frames;
} WavRef;

static uint32_t rd_u32le(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd_u16le(const uint8_t *b)
{
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void wav_free(WavRef *w)
{
    free(w->bytes);
    memset(w, 0, sizeof *w);
}

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
                "groups: metadata speed invalid loop noloop loopoff memory "
                "th probe ta realtime\n");
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
    RUN("invalid", test_invalid);
    RUN("loop", test_loop_reference);
    RUN("noloop", test_no_loop_end);
    RUN("loopoff", test_loop_off_looped);
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
