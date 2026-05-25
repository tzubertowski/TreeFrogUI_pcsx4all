/* sf3000-hwdisp — hardware display via driver.so + HCGE.
 * Wraps SF3000 HiChip Graphics Engine for rotation+scale+present in HW.
 * Falls back transparently if driver.so unavailable. */
#ifndef SF3000_HWDISP_H
#define SF3000_HWDISP_H

#include <stddef.h>
#include <stdint.h>

/* Initialize HW display backend. Returns 0 on success, -1 if unavailable
 * (caller falls back to software path). */
int  hwdisp_init(void);

/* True if HW path is active (init succeeded). */
int  hwdisp_active(void);

/* Present an RGB565 frame to the panel via HW.
 * src: RGB565 pixel buffer (16bpp)
 * w, h: source dimensions
 * pitch_bytes: source row stride in bytes
 * HW scales+rotates+presents on physical panel automatically. */
void hwdisp_present(const void *src, int w, int h, int pitch_bytes);

/* Cleanup. Safe to call even if init failed. */
void hwdisp_deinit(void);

#endif
