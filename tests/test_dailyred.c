/* Host unit tests for src/dailyred.c (seed, dates, stats, ROM patch,
 * status scan). No Q2, platform, or emulator dependency.
 *
 * Build + run: sh tests/run_tests.sh
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "dailyred.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond) do { \
    ++checks; \
    if (!(cond)) { ++failures; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ_U(a, b) do { \
    ++checks; \
    unsigned _a = (a), _b = (b); \
    if (_a != _b) { ++failures; printf("FAIL %s:%d: %s == %u, want %u\n", __FILE__, __LINE__, #a, _a, _b); } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    ++checks; \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b)) { ++failures; printf("FAIL %s:%d: \"%s\" == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, _a, _b); } \
} while (0)

static void test_seed(void)
{
    /* Vectors verified against the real web/daily.js under Node. */
    CHECK_EQ_U(dailyred_seed("2026-10-11"), 4075659768u);
    CHECK_EQ_U(dailyred_seed("2026-11-24"), 3525968457u);
    CHECK_EQ_U(dailyred_seed("2026-11-25"), 3509190838u);
    CHECK_EQ_U(dailyred_seed("2026-01-01"), 886997947u);
    CHECK_EQ_U(dailyred_seed("2025-12-31"), 771109277u);
    CHECK_EQ_U(dailyred_seed("2000-01-01"), 1273113247u);
    CHECK(dailyred_seed("2026-11-24") != 0);
    CHECK(dailyred_seed("2026-11-24") != dailyred_seed("2026-11-25"));

    CHECK_EQ_U(dailyred_parse_seed("1234"), 1234u);
    CHECK_EQ_U(dailyred_parse_seed("4294967295"), 4294967295u);
    CHECK_EQ_U(dailyred_parse_seed("0"), 0u);
    CHECK_EQ_U(dailyred_parse_seed("4294967296"), 0u);
    CHECK_EQ_U(dailyred_parse_seed(""), 0u);
    CHECK_EQ_U(dailyred_parse_seed("12a4"), 0u);
    CHECK_EQ_U(dailyred_parse_seed("12345678901"), 0u); /* 11 digits */
    CHECK_EQ_U(dailyred_parse_seed(" 42 "), 42u);
}

static void test_dates(void)
{
    int y;
    unsigned m, d;
    CHECK(dailyred_parse_date("2026-11-24", &y, &m, &d) && y == 2026 && m == 11 && d == 24);
    CHECK(dailyred_parse_date("2024-02-29", &y, &m, &d)); /* leap day */
    CHECK(!dailyred_parse_date("2026-02-29", &y, &m, &d));
    CHECK(!dailyred_parse_date("2026-13-01", &y, &m, &d));
    CHECK(!dailyred_parse_date("2026-11-31", &y, &m, &d));
    CHECK(!dailyred_parse_date("2026-11-24x", &y, &m, &d));
    CHECK(!dailyred_parse_date("2026/11/24", &y, &m, &d));
    CHECK(!dailyred_parse_date("", &y, &m, &d));

    /* Hinnant round-trip, incl. leap days and the epoch. */
    const char *days[] = {"1970-01-01", "2000-02-29", "2024-02-29", "2026-11-24",
                          "1900-03-01", "2100-02-28", "1999-12-31"};
    for (size_t i = 0; i < sizeof days / sizeof days[0]; ++i) {
        CHECK(dailyred_parse_date(days[i], &y, &m, &d));
        int32_t n = dailyred_days_from_civil(y, m, d);
        int y2;
        unsigned m2, d2;
        dailyred_civil_from_days(n, &y2, &m2, &d2);
        char back[DAILYRED_DATE_LEN];
        snprintf(back, sizeof back, "%04d-%02d-%02d", y2, m2, d2);
        CHECK_EQ_STR(back, days[i]);
    }
    CHECK_EQ_U((unsigned)dailyred_days_from_civil(1970, 1, 1), 0u);

    char out[DAILYRED_DATE_LEN];
    CHECK(dailyred_add_days("2026-11-24", 1, out) && !strcmp(out, "2026-11-25"));
    CHECK(dailyred_add_days("2026-11-24", -1, out) && !strcmp(out, "2026-11-23"));
    CHECK(dailyred_add_days("2025-12-31", 1, out) && !strcmp(out, "2026-01-01"));
    CHECK(dailyred_add_days("2026-01-01", -1, out) && !strcmp(out, "2025-12-31"));
    CHECK(dailyred_add_days("2024-02-28", 1, out) && !strcmp(out, "2024-02-29"));
    CHECK(dailyred_add_days("2026-02-28", 1, out) && !strcmp(out, "2026-03-01"));
    /* US DST spring-forward day still steps one calendar day. */
    CHECK(dailyred_add_days("2026-03-08", 1, out) && !strcmp(out, "2026-03-09"));
    CHECK(dailyred_add_days("2026-11-01", 1, out) && !strcmp(out, "2026-11-02"));
    CHECK(!dailyred_add_days("not-a-date", 1, out));

    /* Countdown formatting. */
    char cd[9];
    dailyred_format_countdown(3661000, cd);
    CHECK_EQ_STR(cd, "01:01:01");
    dailyred_format_countdown(0, cd);
    CHECK_EQ_STR(cd, "00:00:00");
    dailyred_format_countdown(-5, cd);
    CHECK_EQ_STR(cd, "00:00:00");
    dailyred_format_countdown(86399000, cd);
    CHECK_EQ_STR(cd, "23:59:59");

    /* ms until tomorrow, in UTC (always available, no tzdata needed). */
    setenv("TZ", "UTC", 1);
    tzset();
    struct tm noon = {.tm_year = 2026 - 1900, .tm_mon = 11 - 1, .tm_mday = 24,
                      .tm_hour = 12, .tm_min = 0, .tm_sec = 0, .tm_isdst = -1};
    time_t t = mktime(&noon);
    CHECK(t != (time_t)-1);
    CHECK_EQ_U((unsigned)(dailyred_ms_until_tomorrow(t) / 1000), 43200u); /* 12 h */
    char today[DAILYRED_DATE_LEN];
    dailyred_format_date(t, today);
    CHECK_EQ_STR(today, "2026-11-24");
}

static void test_rom_config(void)
{
    uint8_t rom[4096];
    memset(rom, 0xFF, sizeof rom);
    /* Magic at a deliberately unaligned, non-round offset: the launcher
     * must find it by scanning, never by a hard-coded address. */
    size_t magic_at = 1000;
    memcpy(rom + magic_at, DAILYRED_CONF_MAGIC, DAILYRED_CONF_MAGIC_LEN);
    CHECK_EQ_U((unsigned)dailyred_find_config(rom, sizeof rom), 1016u);
    long off = dailyred_patch_config(rom, sizeof rom, 3525968457u, 4, 9, 1);
    CHECK_EQ_U((unsigned)off, 1016u);
    CHECK(rom[1016] == 0x49 && rom[1017] == 0x02 && rom[1018] == 0x2A && rom[1019] == 0xD2); /* seed LE */
    CHECK(rom[1020] == 4 && rom[1021] == 0);   /* wins LE */
    CHECK(rom[1022] == 9 && rom[1023] == 0);   /* battles LE */
    CHECK(rom[1024] == 1);                     /* daily */
    CHECK(rom[1025] == 0xFF && rom[1026] == 0xFF && rom[1027] == 0xFF); /* padding untouched */

    /* Practice patch: daily=0, zero record. */
    off = dailyred_patch_config(rom, sizeof rom, 1234, 0, 0, 0);
    CHECK(off == 1016 && rom[1024] == 0);

    /* Saturation at 0xFFFF, and seed 0 is forced to 1. */
    off = dailyred_patch_config(rom, sizeof rom, 0, 70000, 99999, 1);
    CHECK(off == 1016);
    CHECK(rom[1016] == 1 && rom[1017] == 0 && rom[1018] == 0 && rom[1019] == 0);
    CHECK(rom[1020] == 0xFF && rom[1021] == 0xFF);
    CHECK(rom[1022] == 0xFF && rom[1023] == 0xFF);

    /* No magic anywhere: fatal, not a guessed address. */
    uint8_t plain[512];
    memset(plain, 0, sizeof plain);
    CHECK(dailyred_find_config(plain, sizeof plain) == -1);
    CHECK(dailyred_patch_config(plain, sizeof plain, 1, 0, 0, 1) == -1);
    /* A ROM shorter than the struct cannot contain it. */
    CHECK(dailyred_find_config(rom, 10) == -1);
}

static void test_status_block(void)
{
    static uint8_t ram[256 * 1024]; /* one GBA EWRAM */
    memset(ram, 0, sizeof ram);
    uint32_t seed = 3525968457u;
    size_t at = 12345;
    memcpy(ram + at, DAILYRED_STATUS_MAGIC, DAILYRED_STATUS_MAGIC_LEN);
    ram[at + 16] = 0x49; ram[at + 17] = 0x02; ram[at + 18] = 0x2A; ram[at + 19] = 0xD2;
    ram[at + 20] = DAILYRED_PHASE_BATTLE;
    ram[at + 21] = DAILYRED_RESULT_NONE;
    CHECK_EQ_U((unsigned)dailyred_find_status(ram, sizeof ram, seed), 12345u);
    int phase = -1, result = -1;
    CHECK(dailyred_read_status(ram, sizeof ram, 12345, &phase, &result));
    CHECK(phase == DAILYRED_PHASE_BATTLE && result == DAILYRED_RESULT_NONE);

    /* Wrong seed: the block belongs to another boot, not this one. */
    CHECK(dailyred_find_status(ram, sizeof ram, 1u) == -1);
    /* No block at all (game hasn't booted far enough yet). */
    static uint8_t empty[256 * 1024];
    memset(empty, 0, sizeof empty);
    CHECK(dailyred_find_status(empty, sizeof empty, seed) == -1);
    /* Out-of-range offset. */
    CHECK(!dailyred_read_status(ram, sizeof ram, (long)sizeof ram - 10, &phase, &result));
    /* The ROM config magic must NOT match a status scan. */
    memcpy(ram + 60000, DAILYRED_CONF_MAGIC, DAILYRED_CONF_MAGIC_LEN);
    CHECK(dailyred_find_status(ram, sizeof ram, seed) == 12345);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static void test_stats(void)
{
    const char *path = "/tmp/dailyred-test-stats.json";
    unlink(path);
    unlink("/tmp/dailyred-test-stats.json.tmp");

    dailyred_stats s;
    CHECK(!dailyred_stats_load(&s, path)); /* missing file is normal */
    CHECK_EQ_U(s.wins, 0u);
    CHECK_EQ_U(s.losses, 0u);
    CHECK(!s.has_last_played && !s.has_pending);

    /* Round-trip through the file, incl. a pending battle. */
    s.wins = 4; s.losses = 9; s.win_streak = 2; s.day_streak = 3;
    strcpy(s.last_played, "2026-11-23"); s.has_last_played = true;
    strcpy(s.last_result, "won"); s.has_last_result = true;
    strcpy(s.pending, "2026-11-24"); s.has_pending = true;
    CHECK(dailyred_stats_save(&s, path));
    CHECK(access("/tmp/dailyred-test-stats.json.tmp", F_OK) != 0); /* no .tmp left behind */
    dailyred_stats back;
    CHECK(dailyred_stats_load(&back, path));
    CHECK_EQ_U(back.wins, 4u);
    CHECK_EQ_U(back.losses, 9u);
    CHECK_EQ_U(back.win_streak, 2u);
    CHECK_EQ_U(back.day_streak, 3u);
    CHECK_EQ_STR(back.last_played, "2026-11-23");
    CHECK_EQ_STR(back.last_result, "won");
    CHECK_EQ_STR(back.pending, "2026-11-24");

    /* Corrupt JSON falls back to EMPTY; partial JSON merges defaults. */
    write_file(path, "this is not json{{{");
    CHECK(dailyred_stats_load(&back, path));
    CHECK_EQ_U(back.wins, 0u);
    CHECK(!back.has_last_played);
    write_file(path, "{\"wins\":7}");
    CHECK(dailyred_stats_load(&back, path));
    CHECK_EQ_U(back.wins, 7u);
    CHECK_EQ_U(back.losses, 0u);
    CHECK(!back.has_last_played);
    write_file(path, "{\"wins\":1,\"lastPlayed\":\"2026-02-30\",\"lastResult\":\"maybe\",\"pending\":\"2026-11-24\"}");
    CHECK(dailyred_stats_load(&back, path));
    CHECK(!back.has_last_played); /* impossible date rejected */
    CHECK(!back.has_last_result); /* unknown result rejected */
    CHECK(back.has_pending);      /* valid pending kept */

    /* resultFor / currentStreaks. */
    dailyred_stats_empty(&s);
    s.wins = 4; s.losses = 9; s.win_streak = 2; s.day_streak = 3;
    strcpy(s.last_played, "2026-11-23"); s.has_last_played = true;
    strcpy(s.last_result, "won"); s.has_last_result = true;
    CHECK_EQ_STR(dailyred_result_for(&s, "2026-11-23"), "won");
    CHECK(dailyred_result_for(&s, "2026-11-24") == NULL);
    unsigned ws, ds;
    dailyred_current_streaks(&s, "2026-11-24", &ws, &ds); /* yesterday: alive */
    CHECK_EQ_U(ws, 2u);
    CHECK_EQ_U(ds, 3u);
    dailyred_current_streaks(&s, "2026-11-23", &ws, &ds); /* today: alive */
    CHECK_EQ_U(ws, 2u);
    dailyred_current_streaks(&s, "2026-11-25", &ws, &ds); /* missed a day: dead */
    CHECK_EQ_U(ws, 0u);
    CHECK_EQ_U(ds, 0u);

    /* recordResult: consecutive win extends both streaks; idempotent. */
    dailyred_record_result(&s, "2026-11-24", true);
    CHECK_EQ_U(s.wins, 5u);
    CHECK_EQ_U(s.win_streak, 3u);
    CHECK_EQ_U(s.day_streak, 4u);
    CHECK_EQ_STR(s.last_played, "2026-11-24");
    CHECK_EQ_STR(s.last_result, "won");
    CHECK(!s.has_pending);
    dailyred_record_result(&s, "2026-11-24", false); /* second poll: no-op */
    CHECK_EQ_U(s.wins, 5u);
    CHECK_EQ_U(s.losses, 9u);

    /* Loss after a gap resets both streaks to their fresh values. */
    dailyred_stats_empty(&s);
    s.win_streak = 5; s.day_streak = 9;
    strcpy(s.last_played, "2026-11-20"); s.has_last_played = true;
    strcpy(s.last_result, "won"); s.has_last_result = true;
    dailyred_record_result(&s, "2026-11-24", false);
    CHECK_EQ_U(s.losses, 1u);
    CHECK_EQ_U(s.win_streak, 0u);
    CHECK_EQ_U(s.day_streak, 1u);

    /* markBattleStarted: sets pending once, never overwrites a result. */
    dailyred_stats_empty(&s);
    dailyred_mark_battle_started(&s, "2026-11-24");
    CHECK(s.has_pending && !strcmp(s.pending, "2026-11-24"));
    dailyred_mark_battle_started(&s, "2026-11-24"); /* repeat poll: no-op */
    CHECK(s.has_pending && !strcmp(s.pending, "2026-11-24"));
    dailyred_record_result(&s, "2026-11-24", true);
    dailyred_mark_battle_started(&s, "2026-11-24"); /* already played: no-op */
    CHECK(!s.has_pending);

    /* settleAbandonedGame: pending from another day is a loss ON that day. */
    dailyred_stats_empty(&s);
    s.wins = 4; s.losses = 9;
    strcpy(s.last_played, "2026-11-22"); s.has_last_played = true;
    strcpy(s.last_result, "won"); s.has_last_result = true;
    strcpy(s.pending, "2026-11-23"); s.has_pending = true;
    dailyred_settle_abandoned(&s);
    CHECK_EQ_U(s.losses, 10u);
    CHECK_EQ_STR(s.last_played, "2026-11-23");
    CHECK_EQ_STR(s.last_result, "lost");
    CHECK_EQ_U(s.win_streak, 0u);
    CHECK(!s.has_pending);
    /* Pending equal to lastPlayed (already recorded): just cleared. */
    dailyred_stats_empty(&s);
    s.wins = 1;
    strcpy(s.last_played, "2026-11-24"); s.has_last_played = true;
    strcpy(s.last_result, "won"); s.has_last_result = true;
    strcpy(s.pending, "2026-11-24"); s.has_pending = true;
    dailyred_settle_abandoned(&s);
    CHECK_EQ_U(s.wins, 1u);
    CHECK_EQ_U(s.losses, 0u);
    CHECK(!s.has_pending);
    /* No pending: nothing happens. */
    dailyred_stats_empty(&s);
    dailyred_settle_abandoned(&s);
    CHECK_EQ_U(s.wins, 0u);

    unlink(path);
}

int main(void)
{
    test_seed();
    test_dates();
    test_rom_config();
    test_status_block();
    test_stats();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
