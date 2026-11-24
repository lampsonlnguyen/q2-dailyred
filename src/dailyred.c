/* dailyred.c — Daily Red launcher logic (plain C, host-buildable).
 *
 * Ports of upstream Daily Red's web/daily.js, web/rom-config.js and
 * web/game-link.js. Where the website uses localStorage, this uses a JSON
 * file with the same field names and semantics.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "dailyred.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

void dailyred_format_date(time_t when, char out[DAILYRED_DATE_LEN])
{
    struct tm local;
    localtime_r(&when, &local);
    /* Format into a roomy buffer first: the year from localtime is
     * unbounded as far as the compiler can prove. */
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%04d-%02d-%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    memcpy(out, tmp, 10);
    out[10] = 0;
}

void dailyred_today(char out[DAILYRED_DATE_LEN])
{
    dailyred_format_date(time(NULL), out);
}

/* Howard Hinnant's civil-date algorithms (public domain):
 * https://howardhinnant.github.io/date_algorithms.html */
int32_t dailyred_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int32_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void dailyred_civil_from_days(int32_t days, int *year, unsigned *month, unsigned *day)
{
    int32_t z = days + 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int32_t y = (int32_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp + (mp < 10 ? 3 : -9);
    *year = (int)(y + (m <= 2));
    *month = m;
    *day = d;
}

static bool is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(int year, unsigned month)
{
    static const unsigned len[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap(year)) return 29;
    return len[month - 1];
}

bool dailyred_parse_date(const char *date, int *year, unsigned *month, unsigned *day)
{
    if (!date || strlen(date) != 10 || date[4] != '-' || date[7] != '-')
        return false;
    for (int i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        if (date[i] < '0' || date[i] > '9') return false;
    }
    int y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
            (date[2] - '0') * 10 + (date[3] - '0');
    unsigned m = (unsigned)((date[5] - '0') * 10 + (date[6] - '0'));
    unsigned d = (unsigned)((date[8] - '0') * 10 + (date[9] - '0'));
    if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return false;
    *year = y;
    *month = m;
    *day = d;
    return true;
}

bool dailyred_add_days(const char *date, int delta, char out[DAILYRED_DATE_LEN])
{
    int year;
    unsigned month, day;
    if (!dailyred_parse_date(date, &year, &month, &day)) return false;
    int32_t days = dailyred_days_from_civil(year, month, day) + delta;
    dailyred_civil_from_days(days, &year, &month, &day);
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%04d-%02d-%02d", year, month, day);
    memcpy(out, tmp, 10);
    out[10] = 0;
    return true;
}

int64_t dailyred_ms_until_tomorrow(time_t now)
{
    struct tm local;
    localtime_r(&now, &local);
    local.tm_mday += 1;
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1; /* let mktime() resolve DST for tomorrow */
    time_t midnight = mktime(&local);
    if (midnight == (time_t)-1) return 0;
    return (int64_t)(midnight - now) * 1000;
}

void dailyred_format_countdown(int64_t ms, char out[9])
{
    if (ms < 0) ms = 0;
    int64_t total = ms / 1000;
    int hours = (int)(total / 3600);
    int minutes = (int)((total % 3600) / 60);
    int seconds = (int)(total % 60);
    if (hours > 99) hours = 99;
    /* Hand-built, not snprintf: the values are already range-checked
     * and this keeps -Wformat-truncation quiet on every compiler. */
    out[0] = (char)('0' + hours / 10);
    out[1] = (char)('0' + hours % 10);
    out[2] = ':';
    out[3] = (char)('0' + minutes / 10);
    out[4] = (char)('0' + minutes % 10);
    out[5] = ':';
    out[6] = (char)('0' + seconds / 10);
    out[7] = (char)('0' + seconds % 10);
    out[8] = 0;
}

/* ------------------------------------------------------------------ */
/* Seed                                                                */
/* ------------------------------------------------------------------ */

uint32_t dailyred_seed(const char *date)
{
    /* FNV-1a 32-bit over "dailyred:YYYY-MM-DD", exactly web/daily.js.
     * All arithmetic is uint32_t, so it wraps exactly like Math.imul. */
    uint32_t hash = 0x811c9dc5u;
    char buf[32];
    snprintf(buf, sizeof buf, "dailyred:%s", date);
    for (const unsigned char *p = (const unsigned char *)buf; *p; ++p) {
        hash ^= *p;
        hash *= 0x01000193u;
    }
    return hash ? hash : 1u;
}

uint32_t dailyred_parse_seed(const char *text)
{
    if (!text) return 0;
    while (*text == ' ' || *text == '\t') ++text;
    size_t len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                   text[len - 1] == '\n' || text[len - 1] == '\r'))
        --len;
    if (len < 1 || len > 10) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + (uint64_t)(text[i] - '0');
    }
    if (value < 1 || value > 0xFFFFFFFFull) return 0;
    return (uint32_t)value;
}

/* ------------------------------------------------------------------ */
/* ROM config struct                                                   */
/* ------------------------------------------------------------------ */

static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

long dailyred_find_config(const uint8_t *rom, size_t len)
{
    static const uint8_t magic[DAILYRED_CONF_MAGIC_LEN] = DAILYRED_CONF_MAGIC;
    if (!rom || len < DAILYRED_CONF_SIZE) return -1;
    size_t limit = len - DAILYRED_CONF_MAGIC_LEN;
    for (size_t i = 0; i <= limit; ++i) {
        if (!memcmp(rom + i, magic, DAILYRED_CONF_MAGIC_LEN))
            return (long)i + DAILYRED_CONF_MAGIC_LEN;
    }
    return -1;
}

long dailyred_patch_config(uint8_t *rom, size_t len, uint32_t seed,
                           unsigned wins, unsigned battles, int daily)
{
    long offset = dailyred_find_config(rom, len);
    if (offset < 0) return -1;
    if (seed == 0) seed = 1; /* 0 is the ROM's "no injected seed" sentinel */
    if (wins > 0xFFFF) wins = 0xFFFF;
    if (battles > 0xFFFF) battles = 0xFFFF;
    put_u32le(rom + offset, seed);
    put_u16le(rom + offset + 4, (uint16_t)wins);
    put_u16le(rom + offset + 6, (uint16_t)battles);
    rom[offset + 8] = daily ? 1 : 0;
    return offset;
}

/* ------------------------------------------------------------------ */
/* RAM status block                                                    */
/* ------------------------------------------------------------------ */

long dailyred_find_status(const uint8_t *ram, size_t len, uint32_t seed)
{
    static const uint8_t magic[DAILYRED_STATUS_MAGIC_LEN] = DAILYRED_STATUS_MAGIC;
    if (!ram || len < DAILYRED_STATUS_SIZE) return -1;
    size_t limit = len - DAILYRED_STATUS_SIZE;
    for (size_t i = 0; i <= limit; ++i) {
        if (!memcmp(ram + i, magic, DAILYRED_STATUS_MAGIC_LEN) &&
            get_u32le(ram + i + 16) == seed)
            return (long)i;
    }
    return -1;
}

bool dailyred_read_status(const uint8_t *ram, size_t len, long offset,
                          int *phase, int *result)
{
    if (!ram || offset < 0 || (size_t)offset + DAILYRED_STATUS_SIZE > len)
        return false;
    if (phase) *phase = ram[offset + 20];
    if (result) *result = ram[offset + 21];
    return true;
}

/* ------------------------------------------------------------------ */
/* Stats                                                               */
/* ------------------------------------------------------------------ */

void dailyred_stats_empty(dailyred_stats *stats)
{
    memset(stats, 0, sizeof *stats);
}

bool dailyred_atomic_write(const char *path, const void *data, size_t length)
{
    char temp[1024];
    if (snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp)
        return false;
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, length, f) == length;
    if (ok) ok = fflush(f) == 0;
    if (ok) ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(temp, path) == 0) return true;
    unlink(temp);
    return false;
}

/* Minimal JSON reader for our own file: find "key": <value> and pull an
 * unsigned or a quoted string out of it. Unknown fields are ignored;
 * a missing or null field leaves the caller's default in place. */
static const char *find_key(const char *json, const char *key)
{
    char needle[32];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *at = strstr(json, needle);
    if (!at) return NULL;
    at += strlen(needle);
    while (*at == ' ' || *at == '\t') ++at;
    if (*at != ':') return NULL;
    ++at;
    while (*at == ' ' || *at == '\t') ++at;
    return at;
}

static bool json_uint(const char *json, const char *key, unsigned *out)
{
    const char *at = find_key(json, key);
    if (!at || *at < '0' || *at > '9') return false;
    char *end = NULL;
    unsigned long value = strtoul(at, &end, 10);
    if (end == at || value > 0xFFFFFFFFul) return false;
    *out = (unsigned)value;
    return true;
}

static bool json_string(const char *json, const char *key, char *out, size_t capacity)
{
    const char *at = find_key(json, key);
    if (!at || *at != '"') return false;
    ++at;
    size_t n = 0;
    while (*at && *at != '"' && n + 1 < capacity) out[n++] = *at++;
    if (*at != '"') return false;
    out[n] = 0;
    return true;
}

bool dailyred_stats_load(dailyred_stats *stats, const char *path)
{
    dailyred_stats_empty(stats);
    FILE *f = fopen(path, "rb");
    if (!f) return false; /* missing file: first launch, EMPTY is correct */
    char json[4096];
    size_t n = fread(json, 1, sizeof json - 1, f);
    bool read_ok = !ferror(f);
    fclose(f);
    if (!read_ok) return false;
    json[n] = 0;

    /* Corrupt JSON (or a JSON value of the wrong shape) leaves that field
     * at its EMPTY default, exactly like web loadStats()'s merge. */
    json_uint(json, "wins", &stats->wins);
    json_uint(json, "losses", &stats->losses);
    json_uint(json, "winStreak", &stats->win_streak);
    json_uint(json, "dayStreak", &stats->day_streak);
    char buf[DAILYRED_DATE_LEN];
    int py;
    unsigned pm, pd;
    if (json_string(json, "lastPlayed", buf, sizeof buf) &&
        dailyred_parse_date(buf, &py, &pm, &pd)) {
        strcpy(stats->last_played, buf);
        stats->has_last_played = true;
    }
    char result[DAILYRED_RESULT_LEN];
    if (json_string(json, "lastResult", result, sizeof result) &&
        (!strcmp(result, "won") || !strcmp(result, "lost"))) {
        strcpy(stats->last_result, result);
        stats->has_last_result = true;
    }
    if (json_string(json, "pending", buf, sizeof buf) &&
        dailyred_parse_date(buf, &py, &pm, &pd)) {
        strcpy(stats->pending, buf);
        stats->has_pending = true;
    }
    return true;
}

bool dailyred_stats_save(const dailyred_stats *stats, const char *path)
{
    char json[512];
    char lp[16], lr[16], pd[16];
    if (stats->has_last_played) snprintf(lp, sizeof lp, "\"%s\"", stats->last_played);
    else strcpy(lp, "null");
    if (stats->has_last_result) snprintf(lr, sizeof lr, "\"%s\"", stats->last_result);
    else strcpy(lr, "null");
    if (stats->has_pending) snprintf(pd, sizeof pd, "\"%s\"", stats->pending);
    else strcpy(pd, "null");
    int n = snprintf(json, sizeof json,
                 "{\"wins\":%u,\"losses\":%u,\"winStreak\":%u,\"dayStreak\":%u,"
                 "\"lastPlayed\":%s,\"lastResult\":%s,\"pending\":%s}",
                 stats->wins, stats->losses, stats->win_streak, stats->day_streak,
                 lp, lr, pd);
    if (n < 0 || (size_t)n >= sizeof json) return false;
    return dailyred_atomic_write(path, json, (size_t)n);
}

const char *dailyred_result_for(const dailyred_stats *stats, const char *date)
{
    if (stats->has_last_played && stats->has_last_result &&
        !strcmp(stats->last_played, date))
        return stats->last_result;
    return NULL;
}

void dailyred_current_streaks(const dailyred_stats *stats, const char *date,
                              unsigned *win_streak, unsigned *day_streak)
{
    char yesterday[DAILYRED_DATE_LEN];
    bool alive = false;
    if (stats->has_last_played) {
        if (!strcmp(stats->last_played, date)) alive = true;
        else if (dailyred_add_days(date, -1, yesterday) &&
                 !strcmp(stats->last_played, yesterday))
            alive = true;
    }
    if (win_streak) *win_streak = alive ? stats->win_streak : 0;
    if (day_streak) *day_streak = alive ? stats->day_streak : 0;
}

void dailyred_mark_battle_started(dailyred_stats *stats, const char *date)
{
    if ((stats->has_pending && !strcmp(stats->pending, date)) ||
        dailyred_result_for(stats, date))
        return;
    strcpy(stats->pending, date);
    stats->has_pending = true;
}

void dailyred_record_result(dailyred_stats *stats, const char *date, bool won)
{
    if (stats->has_last_played && !strcmp(stats->last_played, date)) return; /* idempotent */
    char yesterday[DAILYRED_DATE_LEN];
    bool consecutive = stats->has_last_played &&
                       dailyred_add_days(date, -1, yesterday) &&
                       !strcmp(stats->last_played, yesterday);
    stats->day_streak = consecutive ? stats->day_streak + 1 : 1;
    if (won) {
        stats->wins++;
        stats->win_streak = (consecutive && stats->has_last_result &&
                             !strcmp(stats->last_result, "won"))
                                ? stats->win_streak + 1 : 1;
    } else {
        stats->losses++;
        stats->win_streak = 0;
    }
    strcpy(stats->last_played, date);
    stats->has_last_played = true;
    strcpy(stats->last_result, won ? "won" : "lost");
    stats->has_last_result = true;
    stats->has_pending = false;
    stats->pending[0] = 0;
}

void dailyred_settle_abandoned(dailyred_stats *stats)
{
    if (!stats->has_pending) return;
    if (!stats->has_last_played || strcmp(stats->pending, stats->last_played) != 0) {
        /* The abandoned battle counts as a loss on the day it started. */
        char pending[DAILYRED_DATE_LEN];
        strcpy(pending, stats->pending);
        dailyred_record_result(stats, pending, false);
    } else {
        stats->has_pending = false;
        stats->pending[0] = 0;
    }
}
