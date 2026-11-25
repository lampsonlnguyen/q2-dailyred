/* dailyred-launcher.c — Daily Red Q2 Pod card app: launcher + gpSP glue.
 *
 * Plain C, not C++: the Q2 loader's crt-q2.c __start runs no .init_array,
 * so file-scope C++ globals would stay unconstructed (see the
 * q2_card_app_startup skill and the q2-pico8 precedent). Keep this file
 * and everything it links C-only; if C++ ever creeps in, run
 * __init_array_start..__init_array_end manually as the first statement
 * of main().
 *
 * What this file is:
 *   A standalone .dailyred card app (DESIGN.md §2, choice (a)): the
 *   q2-boy Pocket Boy architecture with the multi-system library, the
 *   ROM browser, itch.io, covers and save states deleted, and a minimal
 *   Daily Red launcher in their place. The libretro glue, video scaling,
 *   audio2/gameview handling, battery-save and atomic-write helpers are
 *   adapted from q2-boy src/main.c (Pocket Boy v0.4.2, GPL-2.0-or-later);
 *   see the "q2-boy:" comments. The Daily Red logic itself (seed, stats,
 *   ROM config patch, status scan) lives in dailyred.c and is a port of
 *   upstream Daily Red's web/daily.js, web/rom-config.js, web/game-link.js.
 *
 * What this file is NOT:
 *   - It never touches a retail ROM. The ROM is built from
 *     pret/pokefirered source + patches/pokefirered-dailyred.patch
 *     (scripts/build-rom-linux.sh) and copied by the user into the app
 *     folder as dailyred.gba; it is patched only in a RAM copy.
 *   - It has no save-state support (deliberate: a mid-battle state would
 *     let a lost daily battle be rewound; DESIGN.md §6).
 *   - It has no network, no itch.io, no cover art, no ROM picker.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "platform.h"
#include "dailyred.h"
#include "font.h"
#include "libretro.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Identity / paths                                                    */
/* ------------------------------------------------------------------ */

#define DAILYRED_VERSION "0.1.0"
#define APP_ID "dailyred"
/* App folder on the SD card (the firmware launches /.dailyred/run, which
 * copies the binary to /tmp — so the app folder is a fixed path, not
 * derived from /proc/self/exe). */
#define APP_DIR "/mnt/mmc/.dailyred"
#define DATA_DIR_DEFAULT "/mnt/mmc/dailyred"
/* Data dir: /mnt/mmc/dailyred on device; --smoke overrides it with argv[3]
 * so the emulator test can run against a scratch directory. */
static const char *data_dir = DATA_DIR_DEFAULT;
#define ROM_NAME "dailyred.gba"
#define STATS_NAME "stats.json"
#define DYNAREC_FLAG_NAME "enable-dynarec.txt"
#define AUDIO_SOURCE_NAME "audio-source.cfg"
#define LOG_NAME "q2-dailyred.log"

#define GBA_WIDTH 240
#define GBA_HEIGHT 160
#define CART_WIDTH 512
#define CART_HEIGHT 512

#define KEY(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define RGB(value) (0xff000000u | (value))
#define ACCENT RGB(0x87e4c7)
#define WHITE RGB(0xf7faf9)
#define GRAY RGB(0xa6b9bb)
#define DARK RGB(0x0c161a)
#define RED_INK RGB(0xe05d5d)

#ifndef Q2_RUNTIME_DIR
#define Q2_RUNTIME_DIR "/tmp"
#endif
#define AUDIO_MODE Q2_RUNTIME_DIR "/q2-dailyred.audio"
#define NATIVE_VIEW Q2_RUNTIME_DIR "/q2-dailyred.view"
#define QUIT_OPTION Q2_RUNTIME_DIR "/q2-dailyred.quit"
#define ADDON_CAPS "/tmp/q2-addon.caps"

#define PATH_CAP 700

/* ------------------------------------------------------------------ */
/* Screens / picture                                                   */
/* ------------------------------------------------------------------ */

enum Screen { SCREEN_LAUNCHER, SCREEN_GAME, SCREEN_MENU };
enum Scale { SCALE_FIT, SCALE_CROP, SCALE_STRETCH, SCALE_INTEGER, SCALE_COUNT };
enum VideoFilter { FILTER_NEAREST, FILTER_SHARP_BILINEAR };

static const char *const scale_names[SCALE_COUNT] = {"Fit", "Crop", "Stretch", "Integer"};

static enum Screen screen = SCREEN_LAUNCHER;
static enum Scale scaling = SCALE_FIT;
static enum VideoFilter video_filter = FILTER_SHARP_BILINEAR;
static int running, dirty = 1, menu_focus, launcher_focus;
static bool music_only, use_dynarec, music_mode, audio2, firmware_keys;
static bool native_supported, native_yielded, prefer_music, remember_source;
static bool audio_timeout, smoke, smoke_mute;
static char smoke_rom[PATH_CAP]; /* --smoke ROM path (command line, not SD) */
static uint64_t notice_end, next_save, last_activity;
static char notice[95];
static double fps = 59.7275;
static char game_device[16], game_gain[8];
static uint64_t audio_request_time, native_request_time;
static bool performance_log;
static volatile sig_atomic_t launch_stage;

/* ------------------------------------------------------------------ */
/* libretro core table — gpSP only, statically linked (q2-boy §3)     */
/* ------------------------------------------------------------------ */

#define CORE_API(X) \
    X(set_environment) X(set_video_refresh) X(set_audio_sample) X(set_audio_sample_batch) \
    X(set_input_poll) X(set_input_state) X(init) X(deinit) X(set_controller_port_device) \
    X(load_game) X(unload_game) X(get_system_info) X(get_system_av_info) X(run) X(get_memory_size) \
    X(get_memory_data) X(serialize_size) X(serialize) X(unserialize)
struct Core {
#define API_MEMBER(name) __typeof__(&retro_##name) name;
    CORE_API(API_MEMBER)
#undef API_MEMBER
};
static const struct Core gpsp = {
#define API_GPSP(name) .name = retro_##name,
    CORE_API(API_GPSP)
#undef API_GPSP
};
static const struct Core *core;

static uint32_t cart[CART_WIDTH * CART_HEIGHT], color[65536];
static uint32_t *canvas;
static unsigned cart_width = GBA_WIDTH, cart_height = GBA_HEIGHT;
static bool have_frame, allow_video = true, core_options_changed;

static int clamp(int x, int low, int high) { return x < low ? low : x > high ? high : x; }

/* ------------------------------------------------------------------ */
/* Daily Red run state                                                 */
/* ------------------------------------------------------------------ */

static dailyred_stats stats;
static char stats_path[PATH_CAP];
static char today[DAILYRED_DATE_LEN];
static uint32_t seed;
static bool played_today;        /* lastPlayed == today (result known) */
static bool practice_mode;       /* active run: daily=0, nothing recorded */
static uint32_t practice_seed(void)
{
    static uint32_t previous;
    uint32_t value;
    FILE *random = fopen("/dev/urandom", "rb");
    bool ok = random && fread(&value, sizeof value, 1, random) == 1;
    if (random) fclose(random);
    if (!ok) {
        struct timespec ts = {0};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        value = (uint32_t)time(NULL) ^ (uint32_t)ts.tv_nsec ^ (uint32_t)getpid();
    }
    if (!value || value == previous) value = previous == UINT32_MAX ? 1 : previous + 1;
    previous = value;
    return value;
}
static bool battle_seen;         /* phase reached BATTLE at least once */
static bool result_recorded;     /* result written to stats for this run */
static long status_offset;       /* cached DAILYRED-STATUS1 offset in SYSTEM_RAM, -1 = unscanned */
static bool status_unavailable;  /* SYSTEM_RAM not exposed by the core (logged once) */
static bool status_fallback;     /* pending was set at boot because status is unavailable */
static uint64_t finished_at;     /* when phase FINISHED was first seen (close 1 s later) */
static uint64_t played_redraw_at; /* next 1 s countdown refresh on the launcher */
static bool rom_missing;         /* no dailyred.gba found at startup */

/* ------------------------------------------------------------------ */
/* Drawing (q2-boy: rect/draw_mask/text_at, same 16x17 Liberation font) */
/* ------------------------------------------------------------------ */

static void status(const char *message)
{
    snprintf(notice, sizeof notice, "%s", message);
    notice_end = clock_ns() + 3000000000ull;
    dirty = 1;
}

static void rect(int x, int y, int w, int h, uint32_t fill)
{
    int left = clamp(x, 0, WIDTH), right = clamp(x + w, 0, WIDTH);
    int top = clamp(y, 0, HEIGHT), bottom = clamp(y + h, 0, HEIGHT);
    for (int row = top; row < bottom; ++row)
        for (int col = left; col < right; ++col) canvas[row * WIDTH + col] = fill;
}

static inline uint32_t blend_video(uint32_t a, uint32_t b, unsigned weight)
{
    const uint32_t red_blue = 0x00ff00ffu, green = 0x0000ff00u;
    unsigned rest = 256 - weight;
    uint32_t rb = ((a & red_blue) * rest + (b & red_blue) * weight) >> 8;
    uint32_t g = ((a & green) * rest + (b & green) * weight) >> 8;
    return RGB((rb & red_blue) | (g & green));
}

static void draw_mask(int x, int y, int width, int height, const void *mask_bytes,
                      uint32_t ink, int left, int right)
{
    const unsigned char *mask = mask_bytes;
    const int stride = (width + 1) / 2;
    int first_col = clamp(left - x, 0, width), end_col = clamp(right - x, 0, width);
    int first_row = clamp(-y, 0, height), end_row = clamp(HEIGHT - y, 0, height);
    for (int row = first_row; row < end_row; ++row) {
        for (int col = first_col; col < end_col; ++col) {
            unsigned alpha = (mask[row * stride + col / 2] >> (4 * (col % 2))) & 15u;
            if (!alpha) continue;
            uint32_t *pixel = &canvas[(y + row) * WIDTH + x + col];
            *pixel = alpha == 15 ? ink : blend_video(*pixel, ink, alpha * 17);
        }
    }
}

static void text_at(int x, int y, const char *value, uint32_t ink, int limit)
{
    for (; *value; ++value) {
        unsigned ch = (unsigned char)*value;
        unsigned a = ch < 32 || ch > 126 ? '?' - 32 : ch - 32;
        if (x + advance[a] > limit) break;
        draw_mask(x, y, 16, 17, &glyphs[a], ink, 0, WIDTH);
        x += advance[a];
    }
}

static int text_width(const char *value)
{
    int width = 0;
    for (; *value; ++value) {
        unsigned ch = (unsigned char)*value;
        unsigned a = ch < 32 || ch > 126 ? '?' - 32 : ch - 32;
        width += advance[a];
    }
    return width;
}

static void header(const char *title, const char *sub)
{
    rect(0, 0, WIDTH, HEIGHT, DARK);
    text_at(17, 10, title, ACCENT, WIDTH - 16);
    if (sub) text_at(17, 31, sub, GRAY, WIDTH - 16);
}

/* ------------------------------------------------------------------ */
/* Video scaling (q2-boy: game_picture/draw_nearest/draw_sharp, GBA only) */
/* ------------------------------------------------------------------ */

typedef struct { int x, y, width, height; } Picture;

static Picture game_picture(void)
{
    int width = WIDTH, height = HEIGHT;
    if (scaling == SCALE_INTEGER) {
        unsigned scale_x = WIDTH / cart_width, scale_y = HEIGHT / cart_height;
        unsigned scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale) {
            width = (int)(scale * cart_width);
            height = (int)(scale * cart_height);
            return (Picture){(WIDTH - width) / 2, (HEIGHT - height) / 2, width, height};
        }
    }
    /* GBA keeps its own aspect (no 4:3 handling: this app is GBA-only). */
    unsigned aspect_w = cart_width, aspect_h = cart_height;
    bool panel_is_narrower = WIDTH * aspect_h <= HEIGHT * aspect_w;
    if (scaling != SCALE_STRETCH) {
        if (panel_is_narrower == (scaling != SCALE_CROP)) height = (int)(WIDTH * aspect_h / aspect_w);
        else width = (int)(HEIGHT * aspect_w / aspect_h);
    }
    return (Picture){(WIDTH - width) / 2, (HEIGHT - height) / 2, width, height};
}

static void draw_nearest(Picture p)
{
    int left = clamp(p.x, 0, WIDTH), right = clamp(p.x + p.width, 0, WIDTH);
    static int columns[WIDTH], rows[HEIGHT];
    static Picture previous;
    static unsigned width, height;
    const int top = clamp(p.y, 0, HEIGHT), bottom = clamp(p.y + p.height, 0, HEIGHT);
    if (width != cart_width || height != cart_height || memcmp(&p, &previous, sizeof p)) {
        for (int x = left; x < right; ++x) columns[x] = (x - p.x) * (int)cart_width / p.width;
        for (int y = top; y < bottom; ++y) rows[y] = (y - p.y) * (int)cart_height / p.height;
        previous = p;
        width = cart_width;
        height = cart_height;
    }
    uint32_t *pixels = canvas;
    for (int y = top; y < bottom; ++y) {
        const uint32_t *source = cart + rows[y] * cart_width;
        uint32_t *dest = pixels + y * WIDTH;
        for (int x = left; x < right; ++x) dest[x] = source[columns[x]];
    }
}

typedef struct { uint16_t pixel, weight; } VideoSample;

static void sharp_axis(VideoSample *samples, int start, int end, int origin, int output, int input)
{
    for (int i = start; i < end; ++i) {
        int offset = i - origin;
        if (output >= input) {
            /* Blend only where a destination pixel straddles a source pixel edge. */
            int edge = offset * input;
            int overlap = edge % output + input - output;
            samples[i] = (VideoSample){(uint16_t)(edge / output), (uint16_t)(overlap > 0 ? overlap * 256 / input : 0)};
        } else {
            /* Use a centered bilinear sample when a high-resolution frame needs shrinking. */
            int center = (2 * offset + 1) * input - output;
            int step = 2 * output;
            samples[i] = (VideoSample){(uint16_t)(center / step), (uint16_t)((center % step) * 256 / step)};
        }
    }
}

static inline uint32_t sample_column(const uint32_t *row, VideoSample sample)
{
    uint32_t pixel = row[sample.pixel];
    return sample.weight ? blend_video(pixel, row[sample.pixel + 1], sample.weight) : pixel;
}

static const uint32_t *sharp_row(uint32_t cache[2][WIDTH], int cached[2],
                                 const VideoSample columns[WIDTH], int row, int left, int right)
{
    unsigned slot = (unsigned)row % 2;
    if (cached[slot] != row) {
        const uint32_t *source = cart + row * (int)cart_width;
        for (int x = left; x < right; ++x) cache[slot][x] = sample_column(source, columns[x]);
        cached[slot] = row;
    }
    return cache[slot];
}

static void draw_sharp(Picture p)
{
    static VideoSample columns[WIDTH], rows[HEIGHT];
    static Picture previous;
    static unsigned width, height;
    uint32_t row_cache[2][WIDTH];
    int cached[2] = {-1, -1};
    int left = clamp(p.x, 0, WIDTH), right = clamp(p.x + p.width, 0, WIDTH);
    int top = clamp(p.y, 0, HEIGHT), bottom = clamp(p.y + p.height, 0, HEIGHT);
    if (width != cart_width || height != cart_height || memcmp(&p, &previous, sizeof p)) {
        sharp_axis(columns, left, right, p.x, p.width, (int)cart_width);
        sharp_axis(rows, top, bottom, p.y, p.height, (int)cart_height);
        previous = p;
        width = cart_width;
        height = cart_height;
    }
    uint32_t *pixels = canvas;
    for (int y = top; y < bottom; ++y) {
        int source_y = rows[y].pixel;
        const uint32_t *row = sharp_row(row_cache, cached, columns, source_y, left, right);
        uint32_t *dest = pixels + y * WIDTH;
        if (rows[y].weight) {
            const uint32_t *next = sharp_row(row_cache, cached, columns, source_y + 1, left, right);
            for (int x = left; x < right; ++x)
                dest[x] = blend_video(row[x], next[x], rows[y].weight);
        } else {
            memcpy(dest + left, row + left, (size_t)(right - left) * sizeof(*dest));
        }
    }
}

/* ------------------------------------------------------------------ */
/* File helpers (q2-boy: file_path/file_write/read_text/has_cap)       */
/* ------------------------------------------------------------------ */

static int file_path(char *path, size_t capacity, const char *base, const char *ext)
{
    int n = snprintf(path, capacity, "%s%s", base, ext);
    return n >= 0 && (size_t)n < capacity;
}

static int join_path(char *path, size_t capacity, const char *dir, const char *name)
{
    int n = snprintf(path, capacity, "%s/%s", dir, name);
    return n >= 0 && (size_t)n < capacity;
}

/* Atomic write: <path>.tmp + fflush + fsync + rename (same as q2-boy's
 * file_write and dailyred_atomic_write — the game never saves, but the
 * stats file and the audio-mode request both need this). */
static int file_write(const char *path, const void *data, size_t length)
{
    char temp[PATH_CAP];
    if (!file_path(temp, sizeof temp, path, ".tmp")) return false;
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, length, f) == length && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok = false;
    if (ok && !rename(temp, path)) return true;
    unlink(temp);
    return false;
}

static bool read_text(const char *path, char *buf, size_t capacity)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t n = fread(buf, 1, capacity - 1, file);
    bool ok = !ferror(file) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!ok || memchr(buf, 0, n)) { errno = EINVAL; return false; }
    buf[n] = 0;
    return true;
}

static bool has_cap(const char *caps, const char *word)
{
    size_t length = strlen(word);
    for (const char *at = caps; *at;) {
        at += strspn(at, " \t\r\n");
        size_t n = strcspn(at, " \t\r\n");
        if (n == length && !strncmp(at, word, n)) return true;
        at += n;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Audio2 / gameview (q2-boy, renamed to the dailyred id)             */
/* ------------------------------------------------------------------ */

enum AudioAction { AUDIO_GAME = 1, AUDIO_MUSIC, AUDIO_NOW_PLAYING };
static enum AudioAction pending_action;

static bool enable_manifest_audio(bool supported, bool *was_audio2)
{
    /* q2-boy reads the manifest next to the binary; Daily Red's binary
     * runs from /tmp (see run), so the manifest lives at the fixed app
     * folder path. Rewrites audio= to 2 when the firmware supports audio2
     * (or back to 1 when it does not); the user must exit, back out of
     * Games and re-enter for Q2 Pod to re-read it. */
    char manifest[PATH_CAP], text[512];
    if (!join_path(manifest, sizeof manifest, APP_DIR, "q2app") || !read_text(manifest, text, sizeof text))
        return false;
    char *line = text;
    while (line) {
        char *end = strchr(line, '\n');
        if ((end ? end - line : (ptrdiff_t)strlen(line)) == 7 && !strncmp(line, "audio=0", 7)) {
            music_only = true;
            return true;
        }
        if ((end ? end - line : (ptrdiff_t)strlen(line)) >= 7 &&
            !strncmp(line, "audio=", 6) && (line[6] == '1' || line[6] == '2') &&
            (line[7] == '\r' || line[7] == '\n' || !line[7])) {
            *was_audio2 = line[6] == '2';
            if (*was_audio2 == supported) return true;
            line[6] = supported ? '2' : '1';
            return file_write(manifest, text, strlen(text));
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

static void store_audio_source(bool music)
{
    char path[PATH_CAP];
    const char *mode = music ? "music\n" : "game\n";
    prefer_music = music;
    if (file_path(path, sizeof path, data_dir, "/audio-source.cfg"))
        (void)file_write(path, mode, strlen(mode));
}

static bool write_quit_option(bool off)
{
    if (off) return file_write(QUIT_OPTION, "off", 3);
    return !unlink(QUIT_OPTION) || errno == ENOENT;
}

static bool switch_audio2(enum AudioAction action, bool remember)
{
    const char *mode = action == AUDIO_MUSIC ? "music" : "game";
    bool had_audio = !music_mode;
    if (action == AUDIO_MUSIC) platform_audio_release();
    if (!file_write(AUDIO_MODE, mode, strlen(mode))) {
        if (action == AUDIO_MUSIC && had_audio && game_device[0])
            (void)platform_audio_open(game_device, game_gain);
        status("Could not request Q2 audio");
        return false;
    }
    if (action == AUDIO_MUSIC) music_mode = true;
    pending_action = action;
    remember_source = remember;
    audio_request_time = clock_ns();
    audio_timeout = false;
    dirty = 1;
    return true;
}

static void yield_player(void)
{
    platform_yield_to_player();
    native_yielded = true;
}

static void resume_from_player(void)
{
    native_yielded = false;
    last_activity = clock_ns();
    platform_resume_from_player();
    dirty = 1;
}

static bool audio_arguments(const char *line, char device[16], char volume[8])
{
    char extra;
    int fields = sscanf(line, "%15s %7s %c", device, volume, &extra);
    if (fields == 1) { volume[0] = 0; return !strcmp(device, "plughw:0,0"); }
    if (fields != 2 || (strcmp(device, "plug:bluealsa") && strcmp(device, "usb"))) return false;
    const char *digits = volume + (!strcmp(device, "usb") && volume[0] == 'h');
    return *digits && strlen(digits) <= 3 && strspn(digits, "0123456789") == strlen(digits) &&
           atoi(digits) <= 100;
}

/* q2-boy audio2_poll, minus the legacy 1.8.8 bridge: polls the audio-mode
 * reply file (5 s timeout) and the Now Playing view file. */
static void audio2_poll(uint64_t now)
{
    if (pending_action) {
        char reply[48], device[16], volume[8];
        bool have_reply = read_text(AUDIO_MODE, reply, sizeof reply);
        if (pending_action == AUDIO_GAME && have_reply && audio_arguments(reply, device, volume)) {
            pending_action = 0;
            if (platform_audio_open(device, volume)) {
                strcpy(game_device, device);
                strcpy(game_gain, volume);
                music_mode = false;
                if (remember_source) store_audio_source(false);
            } else {
                (void)switch_audio2(AUDIO_MUSIC, false);
                status("Could not open game audio");
            }
            dirty = 1;
        } else if (pending_action == AUDIO_MUSIC && !have_reply && errno == ENOENT) {
            pending_action = 0;
            if (remember_source) store_audio_source(true);
            dirty = 1;
        } else if (pending_action == AUDIO_NOW_PLAYING && have_reply && !strcmp(reply, "open")) {
            pending_action = 0;
            resume_from_player();
            dirty = 1;
        } else if (!audio_timeout && now - audio_request_time > 5000000000ull) {
            audio_timeout = true;
            status("Waiting for Q2 audio. You can exit if it does not respond.");
        }
    }
    if (!native_yielded) return;
    char view[12];
    if (read_text(NATIVE_VIEW, view, sizeof view)) {
        if (!strcmp(view, "open")) { native_request_time = 0; return; }
        if (!native_request_time || now - native_request_time < 5000000000ull) return;
        if (unlink(NATIVE_VIEW) && errno != ENOENT) return;
        status("Could not open Q2 Now Playing");
    } else if (errno != ENOENT) return;
}

static uint64_t audio_deadline(void)
{
    if (pending_action) return audio_request_time + 5000000000ull;
    if (native_yielded && native_request_time) return native_request_time + 5000000000ull;
    return UINT64_MAX;
}

/* Ask the firmware to open Now Playing: pause emulation, hand audio back,
 * write "now"; the firmware replaces the file with "open" (see audio2_poll). */
static void request_now_playing(void)
{
    if (!firmware_keys) return;
    yield_player();
    (void)switch_audio2(AUDIO_MUSIC, false);
    if (!file_write(NATIVE_VIEW, "now", 3)) {
        resume_from_player();
        status("Could not open Q2 Now Playing");
        return;
    }
    pending_action = AUDIO_NOW_PLAYING;
    native_request_time = clock_ns();
}

/* ------------------------------------------------------------------ */
/* Logging (q2-boy log_launch, renamed log + app name)                 */
/* ------------------------------------------------------------------ */

static void write_fatal(int sig)
{
    const char *names[] = {"menu/initialization", "ROM load", "save RAM load",
                           "first emulated frame", "gameplay", "save/exit", "ALSA output"};
    char line[120] = "Fatal signal ";
    unsigned n = sizeof "Fatal signal " - 1;
    line[n++] = (char)('0' + (sig / 10) % 10);
    line[n++] = (char)('0' + sig % 10);
    line[n++] = ' ';
    line[n++] = 'i';
    line[n++] = 'n';
    line[n++] = ' ';
    unsigned stage = launch_stage;
    if (stage >= sizeof names / sizeof names[0]) stage = 0;
    const char *name = names[stage];
    while (*name && n + 1 < sizeof line) line[n++] = *name++;
    line[n++] = '\n';
    line[n] = 0;
    (void)!write(STDERR_FILENO, line, n);
    _exit(128 + sig);
}

static void log_launch(void)
{
    char log[PATH_CAP], previous[PATH_CAP], flag[PATH_CAP];
    if (file_path(log, sizeof log, data_dir, "/" LOG_NAME)) {
        struct stat st;
        if (!stat(log, &st) && st.st_size > 128 * 1024 &&
            file_path(previous, sizeof previous, log, ".previous"))
            (void)rename(log, previous);
        FILE *stream = freopen(log, "a", stderr);
        if (stream) setvbuf(stream, NULL, _IOLBF, 0);
    }
    if (file_path(flag, sizeof flag, data_dir, "/" DYNAREC_FLAG_NAME)) {
        FILE *f = fopen(flag, "r");
        if (f) { use_dynarec = fgetc(f) == '1'; fclose(f); }
    }
    if (file_path(flag, sizeof flag, data_dir, "/performance-log.txt")) {
        FILE *f = fopen(flag, "r");
        if (f) { performance_log = fgetc(f) == '1'; fclose(f); }
    }
    platform_set_diagnostics(performance_log);
    struct sigaction action = {.sa_handler = write_fatal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    fprintf(stderr, "\nq2-dailyred %s, engine=%s\n", DAILYRED_VERSION,
            use_dynarec ? "dynamic recompiler (opt-in)" : "interpreter");
    FILE *memory = fopen("/proc/meminfo", "r");
    if (memory) {
        char line[128];
        for (int i = 0; i < 4 && fgets(line, sizeof line, memory); ++i) fputs(line, stderr);
        fclose(memory);
    }
}

/* ------------------------------------------------------------------ */
/* libretro callbacks (q2-boy environment/video_frame/audio/input)     */
/* ------------------------------------------------------------------ */

/* q2-boy's log_core: the real libretro.h declares retro_log_printf_t as
 * returning void (my host stub originally said bool — fixed to match). */
static void log_core(enum retro_log_level level, const char *fmt, ...)
{
    if (level < RETRO_LOG_INFO) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static bool environment(unsigned command, void *data)
{
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = data_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "gpsp_drc")) v->value = use_dynarec ? "enabled" : "disabled";
        else if (!strcmp(v->key, "gpsp_rtc_time_source")) v->value = "system";
        else if (!strcmp(v->key, "gpsp_frameskip")) v->value = "disabled"; /* no fast-forward in v1 */
        else if (!strcmp(v->key, "gpsp_frameskip_interval")) v->value = "1";
        else { v->value = NULL; return false; }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = ((smoke ? !smoke_mute : !music_mode && screen == SCREEN_GAME) ? 2 : 0) |
                       (allow_video ? 1 : 0);
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = core_options_changed;
        core_options_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_core;
        return true;
    default: return false;
    }
}

static void video_frame(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (screen != SCREEN_GAME || !data || !w || !h || w > CART_WIDTH || h > CART_HEIGHT ||
        pitch < w * sizeof(uint16_t) || !allow_video) return;
    cart_width = w;
    cart_height = h;
    for (unsigned y = 0; y < h; ++y) {
        const uint16_t *line = (const uint16_t *)((const char *)data + y * pitch);
        uint32_t *dest = cart + y * w;
        for (unsigned x = 0; x < w; ++x) dest[x] = color[line[x]];
    }
    if (!have_frame) fprintf(stderr, "First cartridge video frame received\n");
    have_frame = true;
    dirty = 1;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    if (smoke) return frames;
    sig_atomic_t previous = launch_stage;
    launch_stage = 6;
    platform_audio_write(data, frames, music_mode || screen != SCREEN_GAME);
    launch_stage = previous;
    return frames;
}

static void audio_sample(int16_t l, int16_t r)
{
    int16_t pair[] = {l, r};
    audio_batch(pair, 1);
}

static void input_poll(void) {}

/* Physical + touch state, OR'd like q2-boy's input_state(). */
static unsigned physical_keys, pressed_this_frame;
static int battle_wheel_steps;
static bool battle_wheel_release;

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port || device != RETRO_DEVICE_JOYPAD || index) return 0;
    unsigned pressed = physical_keys | pressed_this_frame;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pressed;
    return id < 16 && (pressed & (1u << id)) != 0;
}

static void initialize_palette(void)
{
    for (unsigned i = 0; i < 65536; ++i) {
        unsigned r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        color[i] = RGB((r * 255 / 31 << 16) | (g * 255 / 63 << 8) | (b * 255 / 31));
    }
}

static void initialize_core(const struct Core *selected)
{
    core = selected;
    allow_video = true;
    core->set_environment(environment);
    core->set_video_refresh(video_frame);
    core->set_audio_sample(audio_sample);
    core->set_audio_sample_batch(audio_batch);
    core->set_input_poll(input_poll);
    core->set_input_state(input_state);
    core->init();
    core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}

static void release_core(void)
{
    if (core) { core->deinit(); core = NULL; }
}

/* ------------------------------------------------------------------ */
/* Battery save (q2-boy battery_io, SAVE_RAM only — gpsp gets no .rtc) */
/* ------------------------------------------------------------------ */

static char game_base[PATH_CAP];

typedef struct { void *bytes; size_t size; } SavedMemory;
static SavedMemory saved_ram;

static void battery_io(unsigned kind, const char *suffix, bool save, SavedMemory *saved)
{
    char path[PATH_CAP];
    size_t size = core->get_memory_size(kind);
    void *data = core->get_memory_data(kind);
    if (!size || !data || !file_path(path, PATH_CAP, game_base, suffix)) return;
    if (saved->size != size) {
        free(saved->bytes);
        saved->bytes = malloc(size);
        saved->size = 0;
    }
    if (save) {
        if (saved->size == size && !memcmp(saved->bytes, data, size)) return;
        if (!file_write(path, data, size)) { status("Could not write in-game save"); return; }
    } else {
        FILE *file = fopen(path, "rb");
        if (file) { (void)fread(data, 1, size, file); fclose(file); }
    }
    if (saved->bytes) {
        memcpy(saved->bytes, data, size);
        saved->size = size;
    }
}

/* The game itself never saves (no Save menu, no SaveGame call anywhere in
 * the patch), so the .srm is normally untouched — but q2-boy's autosave
 * rhythm is kept anyway: a stale .srm is the upstream-tested boot path. */
static void cartridge_saves(bool save)
{
    battery_io(RETRO_MEMORY_SAVE_RAM, ".srm", save, &saved_ram);
}

static void finish_rom(bool save)
{
    if (running) {
        launch_stage = 5;
        if (save) cartridge_saves(true);
        core->unload_game();
    }
    free(saved_ram.bytes);
    saved_ram = (SavedMemory){0};
    launch_stage = 0;
    release_core();
    running = 0;
    if (!smoke) platform_audio_reset();
}

/* ------------------------------------------------------------------ */
/* ROM loading — patch a RAM copy, never the stored file               */
/* ------------------------------------------------------------------ */

static bool find_rom(char path[PATH_CAP])
{
    /* Smoke mode: the ROM path comes from the command line. */
    if (smoke_rom[0]) {
        struct stat st;
        if (stat(smoke_rom, &st) || !S_ISREG(st.st_mode)) return false;
        if (snprintf(path, PATH_CAP, "%s", smoke_rom) >= (int)PATH_CAP) return false;
        return true;
    }
    /* App folder first (where the release install puts it), data dir as
     * a fallback for emulator testing. */
    if (join_path(path, PATH_CAP, APP_DIR, ROM_NAME)) {
        struct stat st;
        if (!stat(path, &st) && S_ISREG(st.st_mode)) return true;
    }
    if (join_path(path, PATH_CAP, data_dir, ROM_NAME)) {
        struct stat st;
        if (!stat(path, &st) && S_ISREG(st.st_mode)) return true;
    }
    return false;
}

static bool load_rom_daily(void)
{
    char path[PATH_CAP];
    if (!find_rom(path)) {
        rom_missing = true;
        return false;
    }
    rom_missing = false;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 192 ||
        (uint64_t)st.st_size > 32 * 1024 * 1024ull) {
        status("ROM is not a usable GBA image");
        return false;
    }

    /* Read the pristine ROM, patch the in-RAM copy, hand that to the core. */
    uint8_t *rom = malloc((size_t)st.st_size);
    FILE *f = rom ? fopen(path, "rb") : NULL;
    bool read = f && fread(rom, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    if (!read) { free(rom); status("Could not read the ROM"); return false; }

    unsigned wins = practice_mode ? 0 : stats.wins;
    unsigned battles = practice_mode ? 0 : stats.wins + stats.losses;
    long config = dailyred_patch_config(rom, (size_t)st.st_size, seed, wins, battles,
                                        practice_mode ? 0 : 1);
    if (config < 0) {
        free(rom);
        status("ROM has no Daily Red config. Rebuild it.");
        fprintf(stderr, "DAILYRED-CONF-V2 not found in %s\n", path);
        return false;
    }
    fprintf(stderr, "Daily Red: seed=%u wins=%u battles=%u daily=%d config@+%ld\n",
            seed, wins, battles, practice_mode ? 0 : 1, config);

    struct retro_system_info system_info = {0};
    gpsp.get_system_info(&system_info);
    struct retro_game_info info = {.path = path};
    char patched_path[PATH_CAP];
    bool use_temp_file = system_info.need_fullpath != 0;
    if (use_temp_file) {
        /* gpsp wants a path: write the patched copy to the data dir
         * (never back to the stored ROM) and delete it on exit. */
        if (!file_path(patched_path, sizeof patched_path, data_dir, "/dailyred-patched.gba") ||
            !file_write(patched_path, rom, (size_t)st.st_size)) {
            free(rom);
            status("Could not stage the patched ROM");
            return false;
        }
        info.path = patched_path;
        info.data = NULL;
        info.size = 0;
    } else {
        info.data = rom;
        info.size = (size_t)st.st_size;
    }

    if (running) finish_rom(true);
    initialize_core(&gpsp);
    fprintf(stderr, "Opening %s (%ld bytes), core=gpSP %s\n", path, (long)st.st_size,
            use_dynarec ? "dynarec" : "interpreter");
    launch_stage = 1;
    bool loaded = core->load_game(&info);
    free(rom);
    if (!loaded) {
        launch_stage = 0;
        release_core();
        if (use_temp_file) unlink(patched_path);
        status("Could not open this ROM");
        return false;
    }
    launch_stage = 2;
    struct retro_system_av_info av = {0};
    core->get_system_av_info(&av);
    fps = av.timing.fps >= 45 && av.timing.fps <= 65 ? av.timing.fps : 59.7275;
    cart_width = av.geometry.base_width;
    cart_height = av.geometry.base_height;
    if (!cart_width || !cart_height || cart_width > CART_WIDTH || cart_height > CART_HEIGHT) {
        core->unload_game();
        release_core();
        if (use_temp_file) unlink(patched_path);
        status("Core reported an unusable video size");
        return false;
    }
    if (!smoke && av.timing.sample_rate >= 8000 && av.timing.sample_rate <= 192000)
        platform_audio_set_rate((unsigned)(av.timing.sample_rate + 0.5));
    if (!join_path(game_base, sizeof game_base, data_dir, ROM_NAME)) {
        core->unload_game();
        release_core();
        if (use_temp_file) unlink(patched_path);
        status("Save path is too long");
        return false;
    }
    cartridge_saves(false);
    running = 1;
    have_frame = false;
    memset(cart, 0, sizeof cart);
    next_save = clock_ns() + 20000000000ull;
    battle_seen = false;
    result_recorded = false;
    status_offset = -1;
    status_unavailable = false;
    status_fallback = false;
    finished_at = 0;
    physical_keys = pressed_this_frame = 0;
    battle_wheel_steps = 0;
    battle_wheel_release = false;
    if (!smoke) {
        /* The day counts as played from the moment RED's battle starts —
         * but only if the status link works. If SYSTEM_RAM is not exposed
         * (see poll_status), the fallback marks the day pending at boot so
         * a mid-battle quit cannot silently bypass the rule; the launcher
         * says so on screen. */
        if (!practice_mode) {
            void *ram = core->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            size_t ram_size = core->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
            if (!ram || !ram_size) {
                status_fallback = true;
                dailyred_mark_battle_started(&stats, today);
                (void)dailyred_stats_save(&stats, stats_path);
                fprintf(stderr, "SYSTEM_RAM unavailable: pending set at boot (fallback)\n");
            }
        }
    }
    screen = SCREEN_GAME;
    dirty = 1;
    fprintf(stderr, "ROM loaded: %s (%.3f fps)\n", path, fps);
    return true;
}

/* ------------------------------------------------------------------ */
/* Status polling — the DAILYRED-STATUS1 block in EWRAM                */
/*                                                                     */
/* UNPROVEN (DESIGN.md §8, blocker 1): whether gpSP exposes EWRAM via */
/* retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM) was not verified in  */
/* the q2-boy checkout. This code scans once for the magic + seed and  */
/* then reads two bytes per poll, exactly like web/game-link.js — but  */
/* it must be proven in the Q2 Pod emulator before the daily rule can  */
/* be trusted. See also the status_fallback path in load_rom_daily().  */
/* ------------------------------------------------------------------ */

static bool poll_status(void)
{
    if (!running) return false;
    size_t ram_size = core->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const uint8_t *ram = core->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || !ram_size) {
        if (!status_unavailable) {
            status_unavailable = true;
            fprintf(stderr, "SYSTEM_RAM unavailable during play\n");
        }
        return false;
    }
    if (status_offset < 0) {
        status_offset = dailyred_find_status(ram, ram_size, seed);
        if (status_offset < 0) return false; /* game hasn't initialised the block yet */
        fprintf(stderr, "DAILYRED-STATUS1 found at SYSTEM_RAM+%ld\n", status_offset);
    }
    int phase, result;
    if (!dailyred_read_status(ram, ram_size, status_offset, &phase, &result)) {
        status_offset = -1; /* RAM moved/grew: rescan next poll */
        return false;
    }
    /* Practice and smoke runs still scan (the smoke test asserts on the
     * scan), but record nothing. */
    if (practice_mode) return false;
    if (phase >= DAILYRED_PHASE_BATTLE && !battle_seen) {
        battle_seen = true;
        dailyred_mark_battle_started(&stats, today);
        (void)dailyred_stats_save(&stats, stats_path);
        fprintf(stderr, "Battle started: pending=%s\n", today);
    }
    if (battle_seen && phase < DAILYRED_PHASE_BATTLE && result == DAILYRED_RESULT_NONE) {
        /* The game restarted mid-battle (or the player quit and came
         * back): the web rule records it as a loss immediately. */
        dailyred_record_result(&stats, today, false);
        (void)dailyred_stats_save(&stats, stats_path);
        result_recorded = true;
        fprintf(stderr, "Mid-battle restart detected: recorded as a loss\n");
        return true; /* caller ends the run */
    }
    if (result != DAILYRED_RESULT_NONE && !result_recorded) {
        dailyred_record_result(&stats, today, result == DAILYRED_RESULT_WON);
        (void)dailyred_stats_save(&stats, stats_path);
        result_recorded = true;
        played_today = true;
        fprintf(stderr, "Result recorded: %s\n", result == DAILYRED_RESULT_WON ? "won" : "lost");
    }
    if (phase == DAILYRED_PHASE_FINISHED && !finished_at)
        finished_at = clock_ns();
    return false;
}

/* Any exit from a run with a battle in progress and no result yet is an
 * abandonment: record the loss now (the web records it on the next load
 * via settleAbandonedGame; doing it here as well is the same outcome,
 * and it survives a TERM during USB takeover). Idempotent. */
static void settle_current_run(void)
{
    if (practice_mode || result_recorded) return;
    if (battle_seen || status_fallback) {
        dailyred_record_result(&stats, today, false);
        (void)dailyred_stats_save(&stats, stats_path);
        result_recorded = true;
        played_today = true;
        fprintf(stderr, "Run abandoned: recorded as a loss\n");
    }
}

/* ------------------------------------------------------------------ */
/* Daily Red controls */

/* Forward declarations: input handling calls the menu/launcher actions,
 * which are defined with the UI below. */
static void menu_move(int direction);
static void menu_confirm(void);
static void menu_back(void);
static void set_screen_menu(void);
static int launcher_rows(void);
static void launcher_activate(int row);
static void refresh_launcher_state(void);
static void tap_menu_or_launcher(int x, int y);
enum Physical { P_RETURN, P_PLAY, P_PREVIOUS, P_NEXT, P_CENTRE, PHYSICAL_COUNT };
static unsigned held_buttons;

static int decode_key(Event e)
{
    if (e.kind == 'g') {
        switch (e.x) { /* Linux md-gpio-keys codes */
        case 103: return P_RETURN;
        case 108: return P_PLAY;
        case 165: return P_PREVIOUS;
        case 163: return P_NEXT;
        case 116: return P_CENTRE;
        }
    }
    if (e.kind == 'k' && !raw_buttons) {
        switch (e.x) { /* Q2-Pod card game codes */
        case 170: return P_RETURN;
        case 171: return P_PLAY;
        case 222: return P_PREVIOUS;
        case 223: return P_NEXT;
        case 218: return P_CENTRE;
        }
    }
    return -1;
}

static unsigned physical_to_key(int id)
{
    static const unsigned keys[PHYSICAL_COUNT] =
        {KEY(B), 0, 0, 0, KEY(A)};
    return id >= 0 && id < PHYSICAL_COUNT ? keys[id] : 0;
}

static int wheel_delta;
static uint64_t wheel_time;

static void read_wheel(int delta)
{
    uint64_t now = clock_ns();
    if (!wheel_time || now - wheel_time > 300000000ull) wheel_delta = 0;
    wheel_time = now;
    wheel_delta += clamp(delta, -84, 84);
    /* q2-boy's sign: a negative delta steps the menu +1 (down), a
     * positive delta steps -1 (up). 21 units per tick. */
    int steps = wheel_delta / 21;
    wheel_delta %= 21;
    if (screen == SCREEN_GAME) battle_wheel_steps = clamp(battle_wheel_steps - steps, -8, 8);
    else while (steps) {
        int direction = steps > 0 ? -1 : 1;
        menu_move(direction);
        steps += direction;
    }
}

/* An inward swipe from either edge opens the pause menu during a battle. */
static int touch_down_x, touch_down_y;
static bool touch_active;

static void read_touch(char action, int x, int y)
{
    if (action == 'd') {
        touch_down_x = x;
        touch_down_y = y;
        touch_active = true;
        return;
    }
    if (!touch_active) return;
    if (screen == SCREEN_GAME) {
        int inward = touch_down_x < 46 ? x - touch_down_x :
                     touch_down_x >= WIDTH - 46 ? touch_down_x - x : 0;
        if (inward >= 36) {
            touch_active = false;
            set_screen_menu();
        }
    } else if (action == 'u' && abs(x - touch_down_x) + abs(y - touch_down_y) < 5) {
        tap_menu_or_launcher(x, y);
        dirty = 1;
    }
    if (action == 'u') touch_active = false;
}

static void set_screen_menu(void)
{
    if (screen != SCREEN_GAME) return;
    screen = SCREEN_MENU;
    battle_wheel_steps = 0;
    battle_wheel_release = false;
    menu_focus = 0;
    dirty = 1;
}

/* Menu navigation shared by the launcher and the in-game menu. */
static void menu_move(int direction)
{
    if (screen == SCREEN_MENU) {
        int rows = music_only ? 3 : firmware_keys ? 5 : 4;
        menu_focus += direction;
        if (menu_focus < 0) menu_focus = rows - 1;
        if (menu_focus >= rows) menu_focus = 0;
    } else if (screen == SCREEN_LAUNCHER) {
        int rows = launcher_rows();
        launcher_focus += direction;
        if (launcher_focus < 0) launcher_focus = rows - 1;
        if (launcher_focus >= rows) launcher_focus = 0;
    }
    dirty = 1;
}

static void read_event(Event e)
{
    if (e.kind == 'w') { read_wheel(e.x); return; }
    if (e.kind == 't') { read_touch(e.action, e.x, e.y); return; }
    if (e.kind == 'r') { /* input reset (SYN_DROPPED): drop all held keys */
        physical_keys = pressed_this_frame = held_buttons = 0;
        battle_wheel_steps = 0;
        battle_wheel_release = false;
        touch_active = false;
        return;
    }
    int id = decode_key(e), down = e.action == 'd';
    if (id < 0 || (e.action != 'u' && !down)) return;
    unsigned bit = 1u << id;
    if (down) {
        if (held_buttons & bit) return; /* held buttons do not auto-repeat */
        held_buttons |= bit;
        if (screen == SCREEN_GAME) {
            unsigned keys = physical_to_key(id);
            if (!(physical_keys & keys)) pressed_this_frame |= keys;
            physical_keys |= keys;
        } else if (id == P_CENTRE) {
            menu_confirm();
        } else if (id == P_RETURN) {
            menu_back();
        }
        /* P_PLAY / P_PREVIOUS / P_NEXT do nothing in menus: the wheel steps. */
    } else {
        held_buttons &= ~bit;
        if (screen == SCREEN_GAME) physical_keys &= ~physical_to_key(id);
    }
}

/* ------------------------------------------------------------------ */
/* Launcher + menu UI                                                  */
/* ------------------------------------------------------------------ */

static int launcher_rows(void)
{
    int rows = 0;
    if (!played_today && !rom_missing) ++rows;       /* Play today's battle */
    if (!rom_missing) ++rows;                         /* Practice */
    ++rows;                                           /* Exit */
    return rows;
}

static void launcher_activate(int row)
{
    int index = 0;
    if (!played_today && !rom_missing) {
        if (row == index) {
            practice_mode = false;
            dailyred_today(today);
            seed = dailyred_seed(today);
            if (load_rom_daily()) return;
            status("Could not start today's battle");
            return;
        }
        ++index;
    }
    if (!rom_missing) {
        if (row == index) {
            practice_mode = true;
            seed = practice_seed();
            if (load_rom_daily()) return;
            status("Could not start the practice battle");
            return;
        }
        ++index;
    }
    (void)index;
    /* Exit row (or the only row when already played / no ROM). */
    finish_rom(false);
    quitting = 1;
}

static void menu_confirm(void)
{
    if (screen == SCREEN_LAUNCHER) { launcher_activate(launcher_focus); return; }
    if (screen != SCREEN_MENU) return;
    /* Rows: 0 Resume, 1 Sound, 2 Now Playing (gameview only),
     * 3 Picture, 4 Exit to launcher. */
    int row = menu_focus;
    if (row == 0) { screen = SCREEN_GAME; dirty = 1; return; }
    if (!music_only && row == 1) {
        if (music_mode) (void)switch_audio2(AUDIO_GAME, true);
        else (void)switch_audio2(AUDIO_MUSIC, true);
        return;
    }
    if (firmware_keys && row == 2) { request_now_playing(); return; }
    if (row == (music_only ? 1 : firmware_keys ? 3 : 2)) {
        scaling = (scaling + 1) % SCALE_COUNT;
        video_filter = video_filter == FILTER_SHARP_BILINEAR ? FILTER_NEAREST : FILTER_SHARP_BILINEAR;
        dirty = 1;
        return;
    }
    /* Exit to launcher: an abandonment if the battle started. */
    settle_current_run();
    finish_rom(true);
    screen = SCREEN_LAUNCHER;
    launcher_focus = 0;
    refresh_launcher_state();
    dirty = 1;
}

static void menu_back(void)
{
    if (screen == SCREEN_MENU) { screen = SCREEN_GAME; dirty = 1; }
    else if (screen == SCREEN_LAUNCHER) { finish_rom(false); quitting = 1; }
}

/* Touch tap on a menu/launcher row: launcher rows start at y=232 (22 px
 * each), in-game menu rows at y=70 (26 px each). */
static void tap_menu_or_launcher(int x, int y)
{
    (void)x;
    if (screen == SCREEN_LAUNCHER) {
        int row = (y - 232) / 22;
        if (row >= 0 && row < launcher_rows()) {
            launcher_focus = row;
            launcher_activate(row);
        }
    } else if (screen == SCREEN_MENU) {
        int rows = music_only ? 3 : firmware_keys ? 5 : 4;
        int row = (y - 70) / 26;
        if (row >= 0 && row < rows) {
            menu_focus = row;
            menu_confirm();
        }
    }
}

/* Recompute today's date/seed/played state (called on launch and whenever
 * the launcher is shown again — the date may have rolled past midnight). */
static void refresh_launcher_state(void)
{
    dailyred_today(today);
    seed = dailyred_seed(today);
    played_today = dailyred_result_for(&stats, today) != NULL;
    if (launcher_focus >= launcher_rows()) launcher_focus = 0;
    played_redraw_at = 0;
}

static void draw_launcher(void)
{
    char line[128];
    header("Daily Red", "One battle with RED, every day.");
    int y = 62;
    text_at(17, y, "Today", GRAY, WIDTH - 16);
    snprintf(line, sizeof line, "%s", today);
    text_at(90, y, line, WHITE, WIDTH - 16);
    y += 22;

    unsigned ws, ds;
    dailyred_current_streaks(&stats, today, &ws, &ds);
    snprintf(line, sizeof line, "Record: %u wins / %u battles", stats.wins, stats.wins + stats.losses);
    text_at(17, y, line, WHITE, WIDTH - 16);
    y += 22;
    snprintf(line, sizeof line, "Streaks: %u win, %u day", ws, ds);
    text_at(17, y, line, WHITE, WIDTH - 16);
    y += 30;

    if (rom_missing) {
        text_at(17, y, "No ROM found.", RED_INK, WIDTH - 16);
        y += 22;
        text_at(17, y, "Build it (scripts/build-rom-linux.sh)", GRAY, WIDTH - 16);
        y += 22;
        text_at(17, y, "and copy dailyred.gba to /.dailyred/", GRAY, WIDTH - 16);
        y += 22;
    } else if (played_today) {
        const char *result = dailyred_result_for(&stats, today);
        snprintf(line, sizeof line, "%s", !strcmp(result, "won") ? "You beat RED today!" : "RED won today.");
        text_at(17, y, line, ACCENT, WIDTH - 16);
        y += 22;
        char countdown[9];
        dailyred_format_countdown(dailyred_ms_until_tomorrow(time(NULL)), countdown);
        snprintf(line, sizeof line, "Next battle in %s", countdown);
        text_at(17, y, line, WHITE, WIDTH - 16);
        y += 22;
        if (status_fallback)
            text_at(17, y, "Status link was unavailable; see log.", GRAY, WIDTH - 16);
    } else if (status_fallback) {
        text_at(17, y, "Status link unavailable:", RED_INK, WIDTH - 16);
        y += 22;
        text_at(17, y, "leaving a battle counts as a loss.", GRAY, WIDTH - 16);
        y += 22;
    }

    /* Rows. */
    int row = 0, ry = 232;
    if (!played_today && !rom_missing) {
        bool chosen = launcher_focus == row;
        text_at(17, ry, chosen ? "> Play today's battle" : "  Play today's battle",
                chosen ? ACCENT : WHITE, WIDTH - 16);
        ry += 22;
        ++row;
    }
    if (!rom_missing) {
        bool chosen = launcher_focus == row;
        text_at(17, ry, chosen ? "> Practice" : "  Practice",
                chosen ? ACCENT : WHITE, WIDTH - 16);
        ry += 22;
        ++row;
    }
    {
        bool chosen = launcher_focus == row;
        text_at(17, ry, chosen ? "> Exit" : "  Exit", chosen ? ACCENT : WHITE, WIDTH - 16);
    }
    if (notice_end > clock_ns()) text_at(17, 296, notice, GRAY, WIDTH - 16);
}

static void draw_game(void)
{
    Picture picture = game_picture();
    int right = picture.x + picture.width, bottom = picture.y + picture.height;
    if (picture.x > 0) rect(0, 0, picture.x, HEIGHT, 0);
    if (right < WIDTH) rect(right, 0, WIDTH - right, HEIGHT, 0);
    if (picture.y > 0) rect(0, 0, WIDTH, picture.y, 0);
    if (bottom < HEIGHT) rect(0, bottom, WIDTH, HEIGHT - bottom, 0);
    if (video_filter == FILTER_SHARP_BILINEAR &&
        (picture.width % (int)cart_width || picture.height % (int)cart_height)) draw_sharp(picture);
    else draw_nearest(picture);
    if (notice_end > clock_ns()) {
        rect(17, 296, WIDTH - 34, 20, DARK);
        text_at(22, 298, notice, GRAY, WIDTH - 22);
    }
}

static void draw_menu(void)
{
    header("Paused", "Daily Red");
    const char *rows[5];
    int count = 0;
    rows[count++] = "Resume";
    char sound[32];
    snprintf(sound, sizeof sound, "Sound: %s", music_mode ? "Q2 Music" : "Game");
    if (!music_only) rows[count++] = sound;
    if (firmware_keys) rows[count++] = "Q2 Now Playing";
    char picture[40];
    snprintf(picture, sizeof picture, "Picture: %s / %s", scale_names[scaling],
             video_filter == FILTER_SHARP_BILINEAR ? "Sharp" : "Nearest");
    rows[count++] = picture;
    rows[count++] = "Exit to launcher";
    for (int i = 0; i < count; ++i) {
        char line[64];
        snprintf(line, sizeof line, "%s %s", menu_focus == i ? ">" : " ", rows[i]);
        text_at(17, 70 + i * 26, line, menu_focus == i ? ACCENT : WHITE, WIDTH - 16);
    }
    text_at(17, 296, "Centre: choose   Return: back", GRAY, WIDTH - 16);
}

static void draw_ui(void)
{
    canvas = platform_canvas();
    if (screen == SCREEN_LAUNCHER) draw_launcher();
    else if (screen == SCREEN_MENU) draw_menu();
    else draw_game();
    platform_present(screen == SCREEN_GAME);
}

/* ------------------------------------------------------------------ */
/* Smoke test (--smoke): boot the patched ROM, prove the status link   */
/* ------------------------------------------------------------------ */

static int smoke_test(const char *date)
{
    /* Stats are not touched in smoke mode; the date is given so the run
     * is reproducible. The ROM comes from --smoke's argv[2] via find_rom. */
    snprintf(today, sizeof today, "%s", date);
    seed = dailyred_seed(today);
    practice_mode = true; /* no stats writes, no daily lock */
    if (!load_rom_daily()) return 1;
    for (int i = 0; i < 600; ++i) { /* ~10 s of emulated time */
        core->run();
        (void)poll_status();
        if (status_offset >= 0) break;
    }
    int phase = -1, result = -1;
    bool ok = false;
    if (status_offset >= 0) {
        size_t ram_size = core->get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
        const uint8_t *ram = core->get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
        ok = ram && dailyred_read_status(ram, ram_size, status_offset, &phase, &result) &&
             phase == DAILYRED_PHASE_EXPLORING && result == DAILYRED_RESULT_NONE;
        fprintf(stderr, "smoke: status@+%ld phase=%d result=%d have_frame=%d\n",
                status_offset, phase, result, have_frame);
    } else {
        fprintf(stderr, "smoke: DAILYRED-STATUS1 never appeared (SYSTEM_RAM link unproven)\n");
    }
    finish_rom(false);
    if (!have_frame) { fprintf(stderr, "smoke: no video frame\n"); return 2; }
    if (!ok) return 3;
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static bool prepare_dirs(void)
{
    struct stat st;
    if (stat(data_dir, &st) && errno == ENOENT) {
        if (mkdir(data_dir, 0755) && errno != EEXIST) return false;
    }
    return join_path(stats_path, sizeof stats_path, data_dir, STATS_NAME);
}

int main(int argc, char **argv)
{
    smoke = argc >= 4 && !strcmp(argv[1], "--smoke");
    smoke_mute = false;
    if (smoke) {
        /* q2-dailyred --smoke <rom> <datadir> [YYYY-MM-DD]
         * Runs without the framebuffer (like q2-boy's --smoke): boots the
         * patched ROM, waits for the status block, asserts it reaches
         * EXPLORING with no result. The datadir stands in for /mnt/mmc/dailyred. */
        const char *date = argc >= 5 ? argv[4] : "2026-11-24";
        data_dir = argv[3];
        if (snprintf(smoke_rom, sizeof smoke_rom, "%s", argv[2]) >= (int)sizeof smoke_rom)
            return 1;
        if (!join_path(stats_path, sizeof stats_path, argv[3], STATS_NAME)) return 1;
        if (mkdir(argv[3], 0755) && errno != EEXIST) return 1;
        initialize_palette();
        int result = smoke_test(date);
        release_core();
        return result;
    }

    if (!prepare_dirs()) return 1;
    initialize_palette();

    log_launch();



    /* Stats: settle an abandoned battle BEFORE offering Play. */
    (void)dailyred_stats_load(&stats, stats_path);
    dailyred_settle_abandoned(&stats);
    (void)dailyred_stats_save(&stats, stats_path);
    refresh_launcher_state();

    char caps[512] = "";
    if (!read_text(ADDON_CAPS, caps, sizeof caps)) caps[0] = 0;
    bool supports_audio2 = has_cap(caps, "audio2");
    bool was_audio2 = false;
    bool manifest_ready = enable_manifest_audio(supports_audio2, &was_audio2);
    audio2 = !music_only && supports_audio2 && argc == 1;
    firmware_keys = !music_only && has_cap(caps, "gameview");
    native_supported = audio2 && firmware_keys;
    music_mode = music_only || (argc == 1 && audio2);
    if (!platform_open(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, music_mode, music_only || firmware_keys)) {
        fprintf(stderr, "Framebuffer or input socket initialization failed: %s\n", strerror(errno));
        platform_close();
        return 1;
    }
    if (supports_audio2 && argc > 1)
        status(manifest_ready ? "Exit Daily Red, back out of Games, then reopen it"
                              : "Could not enable Q2 music on the card");
    else if (!supports_audio2 && was_audio2)
        status(manifest_ready ? "Exit Daily Red, back out of Games, then reopen it"
                              : "Could not restore game audio on the card");
    if (firmware_keys && !write_quit_option(false))
        status("Could not set force-close shortcut");
    if (audio2) {
        char path[PATH_CAP], mode[24];
        prefer_music = file_path(path, sizeof path, data_dir, "/" AUDIO_SOURCE_NAME) &&
                       read_text(path, mode, sizeof mode) && has_cap(mode, "music");
        if (!prefer_music) (void)switch_audio2(AUDIO_GAME, false);
    }
    if (!music_only && !raw_buttons && !native_supported && !firmware_keys && !notice_end)
        status("Limited input: holding Up may exit");

    /* The ROM may be absent on first install; the launcher says so and
     * offers Exit rather than failing outright. */
    {
        char probe[PATH_CAP];
        if (!find_rom(probe)) rom_missing = true;
    }
    last_activity = clock_ns();
    uint64_t next_frame = 0;
    while (!quitting) {
        Event e;
        for (int i = 0; i < 128 && platform_event(&e); ++i)
            if (!native_yielded) read_event(e);
        uint64_t now = clock_ns();
        audio2_poll(now);
        if (notice_end && now >= notice_end) { notice_end = 0; dirty = 1; }
        if (screen == SCREEN_LAUNCHER) {
            /* Midnight rollover: a new battle unlocks; refresh the date,
             * seed, result and countdown once a second. */
            if (!played_redraw_at || now >= played_redraw_at) {
                refresh_launcher_state();
                played_redraw_at = now + 1000000000ull;
                dirty = 1;
            }
        }
        bool advance = running && screen == SCREEN_GAME && !native_yielded;
        if (advance && now >= next_frame) {
            uint64_t period = (uint64_t)(1000000000.0 / fps);
            if (!next_frame || now - next_frame > period * 3) next_frame = now;
            next_frame += period;
            launch_stage = have_frame ? 4 : 3;
            if (battle_wheel_release) battle_wheel_release = false;
            else if (battle_wheel_steps) {
                bool right = battle_wheel_steps > 0;
                pressed_this_frame |= right ? KEY(RIGHT) : KEY(LEFT);
                battle_wheel_steps += right ? -1 : 1;
                battle_wheel_release = true;
            }
            core->run();
            pressed_this_frame = 0;
            if (poll_status()) {
                /* Mid-battle restart detected: end the run now. */
                settle_current_run();
                finish_rom(true);
                screen = SCREEN_LAUNCHER;
                refresh_launcher_state();
                status("Battle abandoned: recorded as a loss");
            } else if (finished_at && now - finished_at >= 1000000000ull) {
                /* Upstream waits 1000 ms after FINISHED, then closes. */
                finish_rom(true);
                screen = SCREEN_LAUNCHER;
                refresh_launcher_state();
                finished_at = 0;
                status(played_today && !strcmp(stats.last_result, "won")
                           ? "You beat RED today!"
                           : "RED won today. Come back tomorrow.");
            }
        } else if (!advance) {
            next_frame = 0;
        }
        if (running && now >= next_save) {
            cartridge_saves(true);
            next_save = now + 20000000000ull;
        }
        platform_audio_service();
        if (dirty && !native_yielded) draw_ui();
        uint64_t deadline = advance ? next_frame : UINT64_MAX;
        deadline = deadline < audio_deadline() ? deadline : audio_deadline();
        if (notice_end) deadline = deadline < notice_end ? deadline : notice_end;
        if (running) deadline = deadline < next_save ? deadline : next_save;
        if (screen == SCREEN_LAUNCHER && played_redraw_at)
            deadline = deadline < played_redraw_at ? deadline : played_redraw_at;
        if (platform_audio_pending()) deadline = deadline < now + 3000000ull ? deadline : now + 3000000ull;
        platform_wait(deadline);
    }
    settle_current_run();
    finish_rom(true);
    {
        /* A temp file only exists if gpsp needed a path; remove it. */
        char patched[PATH_CAP];
        if (file_path(patched, sizeof patched, data_dir, "/dailyred-patched.gba"))
            unlink(patched);
    }
    platform_close();
    fprintf(stderr, "Exited cleanly\n");
    return 0;
}
