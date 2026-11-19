#ifndef PERFSTAT_H
#define PERFSTAT_H

#include <stdbool.h>
#include <stdint.h>

// Only the diagnostic implementation needs LVGL definitions. Playback code
// can mark an optional phase without importing the GUI library.
typedef struct _lv_display_t lv_display_t;
typedef struct _lv_timer_t lv_timer_t;

// Wake-up and frame accounting, for measuring what the player costs at rest
// and while drawing. Off unless asked for, and then only on the UI thread.
//
// Turned on by [debug] perf_stats = N in device_config.ini (or SONIX_PERF=N in
// the environment, which wins): every N seconds one "perf:" line goes to the
// log with
//   - the main loop's passes per second and what woke each one (a timer that
//     came due, a post from another thread, the touch panel);
//   - which LVGL timers ran and how often, by callback address -- turned into
//     names with addr2line on the unstripped binary (see PERF-1.3.0.md);
//   - frames drawn, the UI thread's CPU per frame (layout + render + flush,
//     average and worst) and the pixels each one repainted;
//   - elapsed handler/frame/flush and pointer-dispatch-to-next-render time,
//     with all-sample 1 ms histogram upper bounds (p50/p95/p99) and maxima;
//     dispatch time excludes kernel input backlog and physical panel scanout;
//   - the context switches of every thread of the process (a sleeping thread
//     that wakes costs one), the process's CPU share, its resident and peak
//     memory and the malloc arena.
//
// With the key absent or 0 every hook is a test of one static flag and
// nothing else: no timer, no display callback, no /proc reads.
//
// Thread contract: every function here is for the UI thread only.

// Reads the setting and, when it asks for statistics, starts the reporting
// timer and hooks the display's refresh events. Call once, after the display
// and the first screens exist.
void perfstat_init(lv_display_t *disp);

// A timer that exists only on the emulator (SDL's event pump): left out of the
// counts so the figures read like the device's.
void perfstat_ignore_timer(lv_timer_t *timer);

// Around each pass of the main loop: before and after lv_timer_handler(), then
// what poll() returned (0 = the sleep ran out, so a timer came due).
void perfstat_pass_begin(void);
void perfstat_pass_end(void);
void perfstat_woke(bool by_post, bool by_touch);

// True while statistics are being gathered.
bool perfstat_enabled(void);

// Optional named GUI boundaries (constant labels only, no paths/session data).
// Requires SONIX_PERF_PHASES=1 and enabled collection; otherwise no clock/log work.
void perfstat_phase(const char *name);

#endif /* PERFSTAT_H */
