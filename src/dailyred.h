/* dailyred.h — Daily Red launcher logic: seed, dates, stats, ROM config
 * patching, RAM status-block scanning.
 *
 * This file is plain C with no Q2, platform, or emulator dependency, so it
 * builds and runs on the host (see tests/). It is a line-for-line port of
 * the website's logic in upstream Daily Red's web/daily.js,
 * web/rom-config.js and web/game-link.js — same field names, same
 * transitions, same test vectors — with the browser's localStorage replaced
 * by a JSON file on the SD card (see dailyred_stats_load/save).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef Q2_DAILYRED_H
#define Q2_DAILYRED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Dates                                                               */
/* ------------------------------------------------------------------ */

/* "YYYY-MM-DD" (11 bytes incl. NUL). */
#define DAILYRED_DATE_LEN 11

/* Format a time_t as the LOCAL calendar date, "YYYY-MM-DD".
 * The Q2 firmware owns the timezone (its own clock / toolsSetTimeZone);
 * localtime_r() is the only date source — no tzdata, no DST tables
 * (q2-boy analysis §10: the worldclock city tables are NOT needed here). */
void dailyred_format_date(time_t when, char out[DAILYRED_DATE_LEN]);

/* Today's local date, "YYYY-MM-DD". */
void dailyred_today(char out[DAILYRED_DATE_LEN]);

/* Howard Hinnant civil-date arithmetic (pure integer, no tzdata, no DST):
 * days since 1970-01-01, and back. Used for streak math so a DST change
 * can never skip or repeat a day — the equivalent of web/daily.js addDays(),
 * which anchors at midday for the same reason. */
int32_t dailyred_days_from_civil(int year, unsigned month, unsigned day);
void dailyred_civil_from_days(int32_t days, int *year, unsigned *month, unsigned *day);

/* Parse "YYYY-MM-DD" into y/m/d. Returns false unless the string is
 * exactly that shape and the date is real (month 1-12, day in range). */
bool dailyred_parse_date(const char *date, int *year, unsigned *month, unsigned *day);

/* out = date + delta days ("YYYY-MM-DD"). Returns false on bad input. */
bool dailyred_add_days(const char *date, int delta, char out[DAILYRED_DATE_LEN]);

/* Milliseconds from `now` until the next local midnight — the web
 * msUntilTomorrow(). Computed via mktime() on tomorrow 00:00 local, so a
 * DST transition day counts its real length, not 86400 s. */
int64_t dailyred_ms_until_tomorrow(time_t now);

/* Format a millisecond countdown as "HH:MM:SS" (9 bytes incl. NUL).
 * Hours may exceed 24 on a long DST day; the value is clamped at 99. */
void dailyred_format_countdown(int64_t ms, char out[9]);

/* ------------------------------------------------------------------ */
/* Seed — FNV-1a over "dailyred:YYYY-MM-DD", exactly web/daily.js     */
/* ------------------------------------------------------------------ */

/* FNV-1a 32-bit of the string "dailyred:<date>"; returns hash || 1, so
 * the result is never 0 (0 is the ROM's "no injected seed" sentinel).
 * Test vectors (verified against the real web/daily.js under Node):
 *   2026-10-11 -> 4075659768    2026-11-24 -> 3525968457
 *   2026-11-25 -> 3509190838    2026-01-01 -> 886997947
 *   2025-12-31 -> 771109277     2000-01-01 -> 1273113247 */
uint32_t dailyred_seed(const char *date);

/* Parse a practice seed typed by a person (web/rom-config.js parseSeed):
 * decimal string, 1-10 digits, value 1..0xFFFFFFFF. Returns 0 if invalid. */
uint32_t dailyred_parse_seed(const char *text);

/* ------------------------------------------------------------------ */
/* ROM config struct — DAILYRED-CONF-V2, 28 bytes (see DESIGN.md §3)  */
/* ------------------------------------------------------------------ */

#define DAILYRED_CONF_MAGIC "DAILYRED-CONF-V2"
#define DAILYRED_CONF_MAGIC_LEN 16
/* Offsets relative to the start of the magic: seed u32 LE @+16,
 * wins u16 LE @+20, battles u16 LE @+22, daily u8 @+24. */
#define DAILYRED_CONF_SIZE 28

/* Scan `rom` (length `len`) for the DAILYRED-CONF-V2 magic. Returns the
 * byte offset just past the magic (where the seed is written), or -1 if
 * the magic is absent. The offset is NEVER hard-coded: it moves with
 * every rebuild of the ROM. */
long dailyred_find_config(const uint8_t *rom, size_t len);

/* Patch a working in-RAM copy of the ROM: seed (u32 LE, never 0),
 * wins/battles (u16 LE, saturated at 0xFFFF), daily (1 = daily game,
 * 0 = practice). The stored ROM file is never written. Returns the
 * config offset, or -1 if the magic was not found (fatal build error:
 * rebuild the ROM with scripts/build-rom-linux.sh). */
long dailyred_patch_config(uint8_t *rom, size_t len, uint32_t seed,
                           unsigned wins, unsigned battles, int daily);

/* ------------------------------------------------------------------ */
/* RAM status block — DAILYRED-STATUS1, 24 bytes (see DESIGN.md §4)   */
/* ------------------------------------------------------------------ */

#define DAILYRED_STATUS_MAGIC "DAILYRED-STATUS1"
#define DAILYRED_STATUS_MAGIC_LEN 16
#define DAILYRED_STATUS_SIZE 24

/* Phase / result bytes, at magic+20 / magic+21. */
enum dailyred_phase {
    DAILYRED_PHASE_EXPLORING = 0,
    DAILYRED_PHASE_BATTLE = 1,
    DAILYRED_PHASE_RESULT = 2,
    DAILYRED_PHASE_FINISHED = 3,
};
enum dailyred_result {
    DAILYRED_RESULT_NONE = 0,
    DAILYRED_RESULT_WON = 1,
    DAILYRED_RESULT_LOST = 2,
};

/* Scan a RAM image (`ram`, `len` bytes — the core's SYSTEM_RAM region)
 * for DAILYRED-STATUS1 followed by the injected seed as the next u32 LE
 * word (the ROM's own copy of a magic is a different string, so only the
 * live EWRAM copy matches). Returns the byte offset of the magic, or -1
 * if not found yet (the block is only initialised just after boot). */
long dailyred_find_status(const uint8_t *ram, size_t len, uint32_t seed);

/* Read phase/result from a status block at a known offset (from
 * dailyred_find_status). Returns false if the offset is out of range. */
bool dailyred_read_status(const uint8_t *ram, size_t len, long offset,
                          int *phase, int *result);

/* ------------------------------------------------------------------ */
/* Stats — the localStorage `dailyred-stats` object, as a JSON file    */
/* ------------------------------------------------------------------ */

#define DAILYRED_RESULT_LEN 5 /* "won" / "lost" + NUL */

typedef struct {
    unsigned wins;      /* all-time wins */
    unsigned losses;    /* all-time losses */
    unsigned win_streak; /* daily wins in a row */
    unsigned day_streak; /* days played in a row */
    char last_played[DAILYRED_DATE_LEN]; /* "YYYY-MM-DD" of last FINISHED battle */
    char last_result[DAILYRED_RESULT_LEN]; /* "won" | "lost" */
    char pending[DAILYRED_DATE_LEN];     /* "YYYY-MM-DD" of a started, unfinished battle */
    bool has_last_played;
    bool has_last_result;
    bool has_pending;
} dailyred_stats;

/* Zero stats (the web EMPTY defaults). */
void dailyred_stats_empty(dailyred_stats *stats);

/* Load stats from a JSON file at `path`. Missing file, corrupt JSON, or
 * unknown/missing fields fall back to EMPTY defaults for those fields —
 * exactly web/daily.js loadStats(). Returns false only when the file
 * could not be read at all (missing file is normal on first launch). */
bool dailyred_stats_load(dailyred_stats *stats, const char *path);

/* Save stats to `path` atomically: write <path>.tmp, fflush, fsync,
 * fclose, rename. Returns false on any I/O error (the old file is left
 * in place). */
bool dailyred_stats_save(const dailyred_stats *stats, const char *path);

/* Today's result ("won" / "lost"), or NULL if today's battle isn't done.
 * Mirrors web resultFor(). */
const char *dailyred_result_for(const dailyred_stats *stats, const char *date);

/* Streaks as they stand on `date`: alive only if lastPlayed is today or
 * yesterday, else both 0. Totals are never reset. Mirrors web
 * currentStreaks(). */
void dailyred_current_streaks(const dailyred_stats *stats, const char *date,
                              unsigned *win_streak, unsigned *day_streak);

/* From the moment RED's battle starts the day counts as played.
 * No-op if pending is already this date or the day already has a result.
 * Mirrors web markBattleStarted(). */
void dailyred_mark_battle_started(dailyred_stats *stats, const char *date);

/* Record a finished battle. Idempotent: no-op if lastPlayed is already
 * this date (the launcher polls, so this runs on every poll after the
 * result is known). Mirrors web recordResult(). */
void dailyred_record_result(dailyred_stats *stats, const char *date, bool won);

/* Run once at launch, before anything else: a pending battle from a day
 * that never got a result counts as a LOSS on the day it started; a
 * pending that was already recorded is just cleared. Mirrors web
 * settleAbandonedGame(). */
void dailyred_settle_abandoned(dailyred_stats *stats);

/* ------------------------------------------------------------------ */
/* Atomic file write (shared helper)                                   */
/* ------------------------------------------------------------------ */

/* Write `data`/`length` to `path` via <path>.tmp + fsync + rename.
 * Returns false on any I/O error (old file left in place). */
bool dailyred_atomic_write(const char *path, const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* Q2_DAILYRED_H */
