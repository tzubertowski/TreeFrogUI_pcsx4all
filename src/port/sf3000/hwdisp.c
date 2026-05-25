/* sf3000-hwdisp implementation. dlopen's driver.so, calls video_driver_*.
 *
 * Driver scales src dims → panel (854x480) via HCGE DMA with bilinear filter.
 * Two filter modes:
 *   - HW (default): pass src as-is, driver scales (bilinear).
 *   - Nearest: SW nearest-upscale src to driver native (1280x720) framing,
 *     so driver doesn't scale further → pixels stay sharp.
 *
 * Aspect-pad: when target aspect set, source is centered horizontally with
 * black pillar-bars so driver's stretch becomes uniform. */

#include "hwdisp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define HW_W   1280
#define HW_H    720
#define HW_PITCH (HW_W * 2)
#define HW_BUFSZ (HW_W * HW_H * 2)

static void   *g_handle = NULL;
static int     g_active = 0;

typedef int  (*fn_init_t)(void);
typedef void (*fn_deinit_t)(void);
typedef int  (*fn_disp_t)(void *src, int w, int h, int pitch);

static fn_init_t   p_init   = NULL;
static fn_deinit_t p_deinit = NULL;
static fn_disp_t   p_disp   = NULL;

/* Aspect-pad staging buffer (lazy alloc, resized on demand) */
static uint16_t *g_pad_buf  = NULL;
static int       g_pad_cap  = 0;
static int       g_pad_w    = 0;
static int       g_pad_h    = 0;

/* Nearest-upscale buffer (always 1280x720) */
static uint16_t *g_near_buf = NULL;

static int g_aspect_num = 0;
static int g_aspect_den = 0;
static int g_filter_nearest = 0;

int hwdisp_init(void) {
    if (g_active) return 0;

    g_handle = dlopen("/mnt/sdcard/cubegm/driver.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_handle) {
        fprintf(stderr, "hwdisp: dlopen failed: %s\n", dlerror());
        return -1;
    }

    p_init   = (fn_init_t)  dlsym(g_handle, "video_drivers_init");
    p_deinit = (fn_deinit_t)dlsym(g_handle, "video_driver_deinit");
    p_disp   = (fn_disp_t)  dlsym(g_handle, "video_driver_disp_frame");

    if (!p_init || !p_deinit || !p_disp) {
        fprintf(stderr, "hwdisp: dlsym failed (init=%p deinit=%p disp=%p)\n",
                p_init, p_deinit, p_disp);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    int rv = p_init();
    if (rv <= 0) {
        fprintf(stderr, "hwdisp: video_drivers_init returned %d\n", rv);
        dlclose(g_handle); g_handle = NULL;
        return -1;
    }

    g_active = 1;
    fprintf(stderr, "hwdisp: HW path active (init rv=%d)\n", rv);
    return 0;
}

int hwdisp_active(void) { return g_active; }

void hwdisp_set_target_aspect(int num, int den) {
    g_aspect_num = num;
    g_aspect_den = den;
}

void hwdisp_set_filter(int nearest) {
    g_filter_nearest = nearest ? 1 : 0;
    /* If switching to nearest, ensure native buffer exists. */
    if (g_filter_nearest && !g_near_buf) {
        g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
        if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
    }
}

/* Pad horizontally: src(w×h) → g_pad_buf(pad_w×h), src centered, sides black. */
static void pad_horizontal(const void *src, int w, int h, int pitch_bytes, int pad_w) {
    int need = pad_w * h;
    if (need > g_pad_cap) {
        free(g_pad_buf);
        g_pad_cap = need + 4096;
        g_pad_buf = (uint16_t*)malloc(g_pad_cap * sizeof(uint16_t));
        g_pad_h   = 0;
        g_pad_w   = 0;
    }
    if (!g_pad_buf) return;

    int off_x = (pad_w - w) / 2;
    if (off_x < 0) off_x = 0;

    if (pad_w != g_pad_w || h != g_pad_h) {
        memset(g_pad_buf, 0, (size_t)pad_w * h * sizeof(uint16_t));
        g_pad_w = pad_w;
        g_pad_h = h;
    }

    for (int y = 0; y < h; y++) {
        const uint16_t *srow = (const uint16_t *)((const char *)src + y * pitch_bytes);
        uint16_t *drow = g_pad_buf + (size_t)y * pad_w + off_x;
        memcpy(drow, srow, (size_t)w * sizeof(uint16_t));
    }
}

/* Nearest-upscale src into g_near_buf (1280×720). Pads with black to fit
 * target aspect if set. Otherwise full-stretch upscales to 1280×720.
 *
 * Fast paths:
 *   - Integer scale (dst_w = w*n, dst_h = h*m): unrolled replication +
 *     vertical row memcpy. Avoids per-pixel lookup tables.
 *   - Generic: xmap lookup. */
static void upscale_nearest(const void *src, int w, int h, int pitch_bytes) {
    if (!g_near_buf) return;

    int dst_w, dst_h;
    if (g_aspect_num > 0 && g_aspect_den > 0) {
        /* Integer scale preferring largest factor that still fits */
        int my = HW_H / h;
        if (my < 1) my = 1;
        int dw = w * my;
        if (dw > HW_W) {
            /* Width-limited: pick scale by width instead */
            my = HW_W / w; if (my < 1) my = 1;
            dw = w * my;
        }
        dst_h = h * my;
        dst_w = dw;
    } else {
        /* Full stretch: integer-snap to 1280×720 if possible */
        int mx = HW_W / w; if (mx < 1) mx = 1;
        int my = HW_H / h; if (my < 1) my = 1;
        dst_w = w * mx; dst_h = h * my;
    }
    int off_x = (HW_W - dst_w) / 2;
    int off_y = (HW_H - dst_h) / 2;
    if (off_x < 0) off_x = 0;
    if (off_y < 0) off_y = 0;

    /* Clear borders only when geometry changes */
    static int last_dst_w = -1, last_dst_h = -1;
    if (dst_w != last_dst_w || dst_h != last_dst_h) {
        memset(g_near_buf, 0, HW_BUFSZ);
        last_dst_w = dst_w; last_dst_h = dst_h;
    }

    const int sp = pitch_bytes / 2;
    const uint16_t *s = (const uint16_t *)src;
    const int nx = dst_w / w;    /* H replication factor (integer) */
    const int ny = dst_h / h;    /* V replication factor (integer) */

    /* Integer-scale fast path: expand one row, copy ny times.
     * Use uint32_t writes (2 px/word) where alignment permits. */
    if (nx >= 1 && ny >= 1 && nx * w == dst_w && ny * h == dst_h) {
        const int row_bytes = dst_w * 2;
        for (int sy = 0; sy < h; sy++) {
            const uint16_t *srow = s + sy * sp;
            uint16_t *drow = g_near_buf + (size_t)(sy * ny + off_y) * HW_W + off_x;

            switch (nx) {
            case 1:
                memcpy(drow, srow, (size_t)w * 2);
                break;
            case 2: {
                /* 1 src px → 1 uint32_t write (p|p<<16) */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    d32[sx] = p | (p << 16);
                }
                break;
            }
            case 3:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 3;
                    dp[0] = p; dp[1] = p; dp[2] = p;
                }
                break;
            case 4: {
                /* 1 src px → 2 uint32_t writes */
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*2  ] = pp;
                    d32[sx*2+1] = pp;
                }
                break;
            }
            case 5:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * 5;
                    dp[0] = p; dp[1] = p; dp[2] = p; dp[3] = p; dp[4] = p;
                }
                break;
            case 6: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*3  ] = pp;
                    d32[sx*3+1] = pp;
                    d32[sx*3+2] = pp;
                }
                break;
            }
            case 8: {
                uint32_t *d32 = (uint32_t *)drow;
                for (int sx = 0; sx < w; sx++) {
                    uint32_t p = srow[sx];
                    uint32_t pp = p | (p << 16);
                    d32[sx*4  ] = pp;
                    d32[sx*4+1] = pp;
                    d32[sx*4+2] = pp;
                    d32[sx*4+3] = pp;
                }
                break;
            }
            default:
                for (int sx = 0; sx < w; sx++) {
                    uint16_t p = srow[sx];
                    uint16_t *dp = drow + sx * nx;
                    for (int k = 0; k < nx; k++) dp[k] = p;
                }
                break;
            }

            /* Vertical replication: copy this row (ny-1) more times */
            for (int v = 1; v < ny; v++)
                memcpy(drow + (size_t)v * HW_W, drow, row_bytes);
        }
        return;
    }

    /* Generic fallback: xmap lookup */
    static int xmap[HW_W];
    static int last_w_map = -1, last_dst_w_map = -1;
    if (w != last_w_map || dst_w != last_dst_w_map) {
        for (int dx = 0; dx < dst_w; dx++) xmap[dx] = dx * w / dst_w;
        last_w_map = w; last_dst_w_map = dst_w;
    }
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * h / dst_h;
        const uint16_t *srow = s + sy * sp;
        uint16_t *drow = g_near_buf + (size_t)(dy + off_y) * HW_W + off_x;
        for (int dx = 0; dx < dst_w; dx++)
            drow[dx] = srow[xmap[dx]];
    }
}

void hwdisp_present(const void *src, int w, int h, int pitch_bytes) {
    if (!g_active || !p_disp || !src) return;

    /* Nearest filter: SW upscale to 1280×720, driver does no further scale. */
    if (g_filter_nearest) {
        if (!g_near_buf) {
            g_near_buf = (uint16_t*)malloc(HW_BUFSZ);
            if (g_near_buf) memset(g_near_buf, 0, HW_BUFSZ);
        }
        if (g_near_buf) {
            upscale_nearest(src, w, h, pitch_bytes);
            p_disp(g_near_buf, HW_W, HW_H, HW_PITCH);
            return;
        }
        /* Fallthrough to HW path if alloc failed */
    }

    /* HW (bilinear) path: pass through, optional aspect pad. */
    if (g_aspect_num <= 0 || g_aspect_den <= 0) {
        p_disp((void *)src, w, h, pitch_bytes);
        return;
    }

    int pad_w = h * g_aspect_num / g_aspect_den;
    if (pad_w <= w) {
        p_disp((void *)src, w, h, pitch_bytes);
        return;
    }

    pad_horizontal(src, w, h, pitch_bytes, pad_w);
    if (!g_pad_buf) {
        p_disp((void *)src, w, h, pitch_bytes);
        return;
    }
    p_disp(g_pad_buf, pad_w, h, pad_w * 2);
}

void hwdisp_deinit(void) {
    if (!g_active) return;
    if (p_deinit) p_deinit();
    if (g_pad_buf) { free(g_pad_buf); g_pad_buf = NULL; g_pad_cap = 0; g_pad_w = 0; g_pad_h = 0; }
    if (g_near_buf) { free(g_near_buf); g_near_buf = NULL; }
    if (g_handle) { dlclose(g_handle); g_handle = NULL; }
    p_init = NULL; p_deinit = NULL; p_disp = NULL;
    g_active = 0;
}
