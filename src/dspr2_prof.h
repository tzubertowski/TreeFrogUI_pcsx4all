/*
 * dspr2_prof.h - Phase 1 go/no-go profiling for the DSPr2 plan.
 *
 * Measures how much of each displayed frame is spent in the gpu_unai software
 * rasterizer (do_cmd_list) versus total frame wall time. DSPr2 only speeds the
 * rasterizer ALU stage, so if the GPU share is small or the rasterizer is
 * memory-bound this tells us not to bother.
 *
 * Enable by adding -DDSPR2_PROFILE to CFLAGS. When undefined every macro is a
 * no-op, so the shipping build pays nothing.
 *
 * Output: every PROF_DUMP_FRAMES frames, one line to stderr (captured into
 * /mnt/sdcard/log.txt by the launch redirect).
 */
#ifndef DSPR2_PROF_H
#define DSPR2_PROF_H

#ifdef DSPR2_PROFILE

#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>

#define PROF_DUMP_FRAMES 120

static inline uint64_t prof_us(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Accumulators. Defined once (see DSPR2_PROF_DEFINE in gpulib_if.cpp). */
extern uint64_t g_prof_gpu_us;      /* summed do_cmd_list time this window  */
extern uint64_t g_prof_calls;       /* do_cmd_list invocations this window  */
extern uint64_t g_prof_window_us;   /* wall time accumulated this window    */
extern uint64_t g_prof_last_flip;   /* timestamp of previous video_flip     */
extern uint32_t g_prof_frames;      /* frames counted this window           */
extern uint64_t g_prof_gpu_tmp;     /* scratch: start time of current call  */

#define DSPR2_PROF_DEFINE \
  uint64_t g_prof_gpu_us = 0; \
  uint64_t g_prof_calls = 0; \
  uint64_t g_prof_window_us = 0; \
  uint64_t g_prof_last_flip = 0; \
  uint32_t g_prof_frames = 0; \
  uint64_t g_prof_gpu_tmp = 0;

/* Wrap the rasterizer (do_cmd_list body). */
#define PROF_GPU_BEGIN()  do { g_prof_gpu_tmp = prof_us(); } while (0)
#define PROF_GPU_END()    do { \
    g_prof_gpu_us += prof_us() - g_prof_gpu_tmp; \
    g_prof_calls++; \
  } while (0)

/* Call once per displayed frame, at video_flip. */
#define PROF_FRAME_TICK() do { \
    uint64_t _now = prof_us(); \
    if (g_prof_last_flip != 0) g_prof_window_us += _now - g_prof_last_flip; \
    g_prof_last_flip = _now; \
    if (++g_prof_frames >= PROF_DUMP_FRAMES) { \
      double _fr_ms  = (double)g_prof_window_us / g_prof_frames / 1000.0; \
      double _gpu_ms = (double)g_prof_gpu_us    / g_prof_frames / 1000.0; \
      double _pct    = g_prof_window_us ? \
                       100.0 * (double)g_prof_gpu_us / (double)g_prof_window_us : 0.0; \
      fprintf(stderr, "DSPR2_PROF: frame=%.2fms gpu=%.2fms gpu_share=%.1f%% " \
                      "fps=%.1f calls/frame=%.1f\n", \
              _fr_ms, _gpu_ms, _pct, \
              _fr_ms > 0 ? 1000.0 / _fr_ms : 0.0, \
              (double)g_prof_calls / g_prof_frames); \
      g_prof_gpu_us = g_prof_window_us = g_prof_calls = 0; \
      g_prof_frames = 0; \
    } \
  } while (0)

#else /* !DSPR2_PROFILE */

#define DSPR2_PROF_DEFINE
#define PROF_GPU_BEGIN()  do {} while (0)
#define PROF_GPU_END()    do {} while (0)
#define PROF_FRAME_TICK() do {} while (0)

#endif /* DSPR2_PROFILE */

#endif /* DSPR2_PROF_H */
