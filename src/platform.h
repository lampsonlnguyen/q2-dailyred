#ifndef Q2GBA_PLATFORM_H
#define Q2GBA_PLATFORM_H
#include <stdint.h>
#define WIDTH 375
#define HEIGHT 320
typedef struct { char kind, action; int x, y; } Event;
#include <signal.h>
extern volatile sig_atomic_t quitting;
extern int raw_buttons;
int platform_open(const char *device, const char *volume, int music, int firmware_keys);
int platform_audio_open(const char *device, const char *volume);
void platform_audio_select(int route, int gain);
void platform_audio_release(void);
void platform_audio_set_rate(unsigned rate);
void platform_audio_gain(int gain);
void platform_audio_attenuation(int percent);
void platform_close(void);
void platform_yield_to_player(void);
void platform_resume_from_player(void);
int platform_event(Event *event);
void platform_wait(uint64_t deadline);
uint32_t *platform_canvas(void);
void platform_present(int asynchronous);
void platform_audio_service(void);
int platform_audio_pending(void);
typedef struct {
    unsigned flips, dropped_frames, longest_flip_us, underruns, dropped_samples;
} PlatformStats;
void platform_set_diagnostics(int enabled);
PlatformStats platform_take_stats(void);
void platform_audio_write(const int16_t *samples, unsigned frames, int muted);
void platform_audio_reset(void);
uint64_t clock_ns(void);
#endif
