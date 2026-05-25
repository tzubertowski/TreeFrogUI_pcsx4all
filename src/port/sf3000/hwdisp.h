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

/* Set target output aspect for padding (default 0,0 = no pad, full stretch).
 * Driver scales source dims to panel; setting target aspect causes
 * hwdisp_present to copy source into a staging buffer padded with black
 * to match the target aspect, so driver's stretch becomes uniform.
 *
 * Typical: hwdisp_set_target_aspect(16, 9) for the SF3000 panel. Then a
 * 4:3 source ends up letter/pillar-boxed correctly. */
void hwdisp_set_target_aspect(int num, int den);

/* Filter mode (default 0 = HW bilinear via driver scale).
 * 1 = SW nearest upscale to driver native (1280x720) before send — driver
 *     does no further scaling, pixels stay sharp. Adds CPU cost. */
void hwdisp_set_filter(int nearest);

/* Present an RGB565 frame to the panel via HW.
 * src: RGB565 pixel buffer (16bpp)
 * w, h: source dimensions
 * pitch_bytes: source row stride in bytes
 * HW scales+rotates+presents on physical panel automatically. */
void hwdisp_present(const void *src, int w, int h, int pitch_bytes);

/* Cleanup. Safe to call even if init failed. */
void hwdisp_deinit(void);

#endif
