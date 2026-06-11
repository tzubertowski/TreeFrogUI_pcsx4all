/*
 * pcsx4all SF3000 port
 * Display: /dev/dis ioctl + /dev/fb0 direct mmap (same as picoarch)
 * Input:   cubevol shared memory at /tmp/joy_key
 * Exit:    SELECT+START → exit(0) → icube relaunches FrogUI
 * Menu:    SELECT+L1    → GameMenu()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <linux/fb.h>
#include <time.h>
#include <math.h>
#include <dirent.h>

#include "SDL.h"
#include "port.h"
#include "r3000a.h"
#include "plugins.h"
#include "sio.h"
#include "plugin_lib.h"
#include "perfmon.h"
#include "cdrom_hacks.h"
#include "cheat.h"
#include "hwdisp.h"

#ifdef SPU_PCSXREARMED
  #include "spu/spu_pcsxrearmed/spu_config.h"
#endif
#ifdef USE_GPULIB
  #include "gpu/gpulib/gpu.h"
#endif
#ifdef GPU_UNAI
  #include "gpu/gpu_unai/gpu.h"
#endif
#ifdef PSXREC
  extern u32 cycle_multiplier;
#endif

static void plog(const char *msg);

uint8_t use_speedup = 0;
void update_window_size(int w, int h) {
    if (w == SCREEN_WIDTH && h == SCREEN_HEIGHT) return;
    free(SCREEN);
    SCREEN_WIDTH  = w;
    SCREEN_HEIGHT = h;
    SCREEN = calloc((size_t)w * h, 2);
}

/* --------------------------------------------------------------------------
 * Display state
 * -------------------------------------------------------------------------- */
static int          dis_fd  = -1;
static int          fb_fd   = -1;
static uint32_t    *fb_mem  = NULL;
static size_t       fb_size = 0;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static int          current_page = 0;

#define PHYS_W      854
#define PHYS_H      480
#define PAGE_Y_SIZE 1280
#define FB_X_VIS    180
#define FB_Y_TOT    1014  /* fb_y range 0..1013 maps to PHYS_W=854 physical px */

static uint16_t *rotated_buf    = NULL;
static int       rotated_cap    = 0;
static int       last_fb_y_off  = -1;
static int       last_fb_y_len  = -1;

struct blend_data { int16_t gy1, gy2; uint8_t w1, w2; };
static struct blend_data blend_lut[FB_X_VIS];
static int last_h_blend = -1;

unsigned short *SCREEN        = NULL;
int             SCREEN_WIDTH  = 320;
int             SCREEN_HEIGHT = 240;
int             scale_mode    = 0;   /* 0=aspect-correct, 1=fullscreen */
int             scale_filter  = 1;   /* 0=nearest (SW fb0 — broken on this HW), 1=bilinear (HW HCGE) */
int             use_hwdisp    = 0;   /* 0=software path, 1=HW via driver.so */

/* Gamma / black-lift: 100 = off (linear). >100 lifts crushed blacks (PS1
 * true-black games unreadable on this panel). Applied to SCREEN (RGB565) just
 * before blit. Safe in-place: vout re-copies the whole visible area from VRAM
 * every frame, so the LUT never accumulates. */
int             gamma_percent = 100;
static uint8_t  glut5[32], glut6[64];
static int      glut_built_for = -1;
static void gamma_build_lut(void) {
    if (glut_built_for == gamma_percent) return;
    double g = gamma_percent / 100.0;            /* >1 brightens shadows */
    for (int i = 0; i < 32; i++) {
        int v = (int)(pow(i / 31.0, 1.0 / g) * 31.0 + 0.5);
        glut5[i] = v > 31 ? 31 : v;
    }
    for (int i = 0; i < 64; i++) {
        int v = (int)(pow(i / 63.0, 1.0 / g) * 63.0 + 0.5);
        glut6[i] = v > 63 ? 63 : v;
    }
    glut_built_for = gamma_percent;
}
static void gamma_apply(uint16_t *px, int count) {
    if (gamma_percent <= 100 || !px) return;
    gamma_build_lut();
    for (int i = 0; i < count; i++) {
        uint16_t p = px[i];
        px[i] = (glut5[(p >> 11) & 31] << 11)
              | (glut6[(p >>  5) & 63] <<  5)
              |  glut5[ p        & 31];
    }
}

/* RGB565 → ARGB8888 with full 8-bit channel expansion */
static inline uint32_t cvt565(uint16_t c) {
    return 0xFF000000u
         | (uint32_t)((c & 0xF800) << 8) | (uint32_t)((c & 0xE000) << 3)
         | (uint32_t)((c & 0x07E0) << 5) | (uint32_t)((c & 0x0600) >> 1)
         | (uint32_t)((c & 0x001F) << 3) | (uint32_t)((c & 0x001C) >> 2);
}

static void dis_assert(void) {
    if (dis_fd < 0) dis_fd = open("/dev/dis", O_RDWR);
    if (dis_fd >= 0) {
        struct { int a, b, c; } buf = {1, 0, 0};
        ioctl(dis_fd, 0xc00c0e0c, &buf);
    }
}

/* OSD overlay (battery, volume) is drawn by cubevol on /dev/fb1 (ARGB plane).
 * Hide: memset fb1 to zero (alpha=0 → invisible). cubevol has no signal
 *       handler so it won't redraw on its own.
 * Show: restart cubevol via killall + spawn; new instance draws OSD on init. */
#include <sys/mman.h>
#include <linux/fb.h>
#include <stdlib.h>
static void fb1_clear(void) {
    int fd = open("/dev/fb1", O_RDWR);
    if (fd < 0) return;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 && finfo.smem_len > 0) {
        void *mem = mmap(NULL, finfo.smem_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (mem != MAP_FAILED) { memset(mem, 0, finfo.smem_len); munmap(mem, finfo.smem_len); }
    }
    close(fd);
}
/* Only FrogUI restores OSD on its own init. pcsx4all keeps fb1 cleared. */
static void fb1_blank(int blank) { if (blank) fb1_clear(); }

static int display_init(void) {
    dis_assert();

    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) { perror("open /dev/fb0"); return -1; }

    ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo);

    fb_size = finfo.smem_len;
    fb_mem  = mmap(NULL, fb_size, PROT_READ|PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap fb0");
        close(fb_fd); fb_fd = -1;
        return -1;
    }

    fprintf(stderr, "SF3000 fb: %ux%u bpp=%u line=%u mem=%p\n",
            vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
            finfo.line_length, (void*)fb_mem);

    /* Clear to black */
    for (size_t i = 0; i < fb_size / 4; i++) fb_mem[i] = 0xFF000000u;

    /* Off-screen 320x240 RGB565 buffer for GPU to render into */
    SCREEN       = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, 2);
    SCREEN_WIDTH  = 320;
    SCREEN_HEIGHT = 240;

    /* HW display via driver.so. Fail loudly if unavailable. */
    if (hwdisp_init() != 0) {
        plog("display_init: hwdisp_init FAILED");
        fprintf(stderr, "display_init: hwdisp_init FAILED — aborting\n");
        return -1;
    }
    use_hwdisp = 1;
    fb1_blank(1);   /* hide battery/volume OSD during gameplay */
    /* scale_mode 0 = Aspect-Correct (nearest SW-upscale to 1280×720 with
     *   16:9 pad — driver does uniform downscale, letterbox bars survive)
     * scale_mode 1 = Fullscreen (HW bilinear direct, driver stretches per axis)
     * Driver-side aspect padding alone is not respected — bars get scaled
     * away. Putting bars into the 1280×720 staging buffer works. */
    if (scale_mode == 1) {
        hwdisp_set_target_aspect(0, 0);
        hwdisp_set_filter(0);
    } else {
        hwdisp_set_target_aspect(16, 9);
        hwdisp_set_filter(1);
    }
    plog("display_init: HW path active");
    fprintf(stderr, "display_init: HW path active\n");
    return 0;
}

/* PSX wide-mode aspect normalize: when src width exceeds 4:3-of-height (i.e.
 * non-square PSX pixels, e.g. 512×240 / 640×240 / 384×240), downsample src
 * horizontally to 4:3 ratio (320×240 for 640×240). Pass result to driver via
 * the standard 16:9 pad. Driver then scales 4:3-padded buf uniformly to panel
 * → correct 4:3 image with pillarbox.
 *
 * Cost: one horizontal downsample pass (~150KB/frame for typical src). Much
 * cheaper than full panel compose; mirrors how libretro cores (pcsx_rearmed)
 * normalize their output. */
static void compose_aspect_4to3(const void *src, int w, int h, int pitch) {
    int target_w = h * 4 / 3;
    if (target_w >= w) {
        /* Already 4:3 or narrower (320×240, 256×240): pass directly. */
        hwdisp_set_target_aspect(16, 9);
        hwdisp_set_filter(0);
        hwdisp_present(src, w, h, pitch);
        return;
    }

    /* Downsample W → target_w via nearest decimation. */
    static uint16_t buf[640 * 512];        /* fits up to 640×480 PSX hi-res */
    static int x_map[640];
    static int last_w = -1, last_tgt = -1;
    if (target_w > 640) target_w = 640;

    if (w != last_w || target_w != last_tgt) {
        for (int dx = 0; dx < target_w; dx++) x_map[dx] = dx * w / target_w;
        last_w = w; last_tgt = target_w;
    }

    const uint16_t *s = (const uint16_t *)src;
    const int sp = pitch / 2;
    for (int y = 0; y < h && y < 512; y++) {
        const uint16_t *srow = s + y * sp;
        uint16_t *drow = buf + y * target_w;
        for (int dx = 0; dx < target_w; dx++) drow[dx] = srow[x_map[dx]];
    }

    hwdisp_set_target_aspect(16, 9);
    hwdisp_set_filter(0);
    hwdisp_present(buf, target_w, h, target_w * 2);
}

static void display_blit(const void *src, int w, int h, int pitch) {
    /* Always use HW path: SW fb0 writes are invisible once hwdisp_init has
     * activated HCGE in this process, because per-frame dis_assert() can't
     * override HCGE display routing on this hardware. */
    if (use_hwdisp) {
        if (scale_mode == 0) {
            /* Aspect-correct: PSX 4:3 regardless of internal resolution */
            compose_aspect_4to3(src, w, h, pitch);
        } else {
            /* Fullscreen stretch */
            hwdisp_set_target_aspect(0, 0);
            hwdisp_set_filter(0);
            hwdisp_present(src, w, h, pitch);
        }
        return;
    }
    dis_assert();
    if (!fb_mem || !src) return;

    const int dst_stride = (int)(finfo.line_length / 4);

    /* Transpose src(w×h) → rotated_buf(h×w) column-major.
       gy-outer/gx-inner: reads are sequential (row[gx]), writes are strided. */
    if (w * h > rotated_cap) {
        free(rotated_buf);
        rotated_cap = w * h + 4096;
        rotated_buf = malloc(rotated_cap * sizeof(uint16_t));
    }
    const uint16_t *s = src;
    for (int gy = 0; gy < h; gy++) {
        const uint16_t *row = (const uint16_t *)((const char *)s + gy * pitch);
        for (int gx = 0; gx < w; gx++)
            rotated_buf[gx * h + gy] = row[gx];
    }

    /* Nearest-neighbor LUT: gy[fx] = source row for output column fx */
    static int16_t gy_lut[FB_X_VIS];
    static int last_h_lut = -1;
    if (h != last_h_lut) {
        for (int fx = 0; fx < FB_X_VIS; fx++)
            gy_lut[fx] = (int16_t)((FB_X_VIS - 1 - fx) * h / FB_X_VIS);
        last_h_lut = h;
    }

    /* Bilinear blend LUT (rebuilt when h changes) */
    extern int scale_filter;
    if (scale_filter && h != last_h_blend) {
        for (int fx = 0; fx < FB_X_VIS; fx++) {
            int gf = ((FB_X_VIS - 1 - fx) * h * 256) / FB_X_VIS;
            int gy = gf >> 8;
            blend_lut[fx].gy1 = (int16_t)gy;
            blend_lut[fx].gy2 = (int16_t)((gy + 1 < h) ? gy + 1 : gy);
            blend_lut[fx].w2  = (uint8_t)((gf & 0xFF) >> 1);
            blend_lut[fx].w1  = (uint8_t)(128 - blend_lut[fx].w2);
        }
        last_h_blend = h;
    }

    /* Scale mode: 0=aspect-correct, 1=fullscreen-stretch */
    extern int scale_mode;
    int fb_y_len, fb_y_off;
    /* Physical display width for this frame (capped at PHYS_W=854) */
    /* PS1 outputs 4:3 on TV regardless of internal hres */
    int disp_w = (scale_mode == 1) ? PHYS_W : (PHYS_H * 4 / 3);
    if (disp_w > PHYS_W) disp_w = PHYS_W;
    /* fb_y range 0..FB_Y_TOT-1 maps to PHYS_W physical pixels (same formula as picoarch) */
    fb_y_len = disp_w * FB_Y_TOT / PHYS_W;
    fb_y_off = (FB_Y_TOT - fb_y_len) / 2;

    /* Double-buffer page flip */
    current_page = (current_page + 1) % 2;
    int page_y   = current_page * PAGE_Y_SIZE;

    /* Clear borders once when geometry changes */
    if (fb_y_off != last_fb_y_off || fb_y_len != last_fb_y_len) {
        for (int p = 0; p < 2; p++)
            for (int fy = 0; fy < PAGE_Y_SIZE; fy++)
                for (int fx = 0; fx < FB_X_VIS; fx++)
                    fb_mem[(p * PAGE_Y_SIZE + fy) * dst_stride + fx] = 0xFF000000u;
        last_fb_y_off = fb_y_off;
        last_fb_y_len = fb_y_len;
    }

    /* Blit with bilinear-like vertical blend */
    static uint32_t row_cache[FB_X_VIS];
    int fb_y = fb_y_off;
    while (fb_y < fb_y_off + fb_y_len) {
        int gx = (fb_y - fb_y_off) * w / fb_y_len;
        int count = 1;
        while (fb_y + count < fb_y_off + fb_y_len &&
               ((fb_y + count - fb_y_off) * w / fb_y_len) == gx)
            count++;

        const uint16_t *col = rotated_buf + gx * h;
        if (!scale_filter) {
            for (int fx = 0; fx < FB_X_VIS; fx++)
                row_cache[fx] = cvt565(col[gy_lut[fx]]);
        } else {
            for (int fx = 0; fx < FB_X_VIS; fx++) {
                uint16_t p1 = col[blend_lut[fx].gy1];
                if (blend_lut[fx].w2 == 0) {
                    row_cache[fx] = cvt565(p1);
                } else {
                    uint16_t p2 = col[blend_lut[fx].gy2];
                    if (p1 == p2) {
                        row_cache[fx] = cvt565(p1);
                    } else {
                        uint32_t c1 = cvt565(p1), c2 = cvt565(p2);
                        uint32_t w1 = blend_lut[fx].w1, w2 = blend_lut[fx].w2;
                        uint32_t rb = (((c1&0xFF00FF)*w1+(c2&0xFF00FF)*w2)>>7)&0xFF00FF;
                        uint32_t g  = (((c1&0x00FF00)*w1+(c2&0x00FF00)*w2)>>7)&0x00FF00;
                        row_cache[fx] = 0xFF000000u | rb | g;
                    }
                }
            }
        }
        uint32_t *dst = fb_mem + (page_y + fb_y) * dst_stride;
        for (int i = 0; i < count; i++) {
            for (int k = 0; k < 18; k++) {
                dst[k*10+0]=row_cache[k*10+0]; dst[k*10+1]=row_cache[k*10+1];
                dst[k*10+2]=row_cache[k*10+2]; dst[k*10+3]=row_cache[k*10+3];
                dst[k*10+4]=row_cache[k*10+4]; dst[k*10+5]=row_cache[k*10+5];
                dst[k*10+6]=row_cache[k*10+6]; dst[k*10+7]=row_cache[k*10+7];
                dst[k*10+8]=row_cache[k*10+8]; dst[k*10+9]=row_cache[k*10+9];
            }
            dst += dst_stride;
        }
        fb_y += count;
    }

    /* Page flip — timing handled by pl_frame_limit() in EmuUpdate() */
    struct fb_var_screeninfo vi = vinfo;
    vi.xoffset = 0; vi.yoffset = page_y;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);
}

static void shutdown_display(void) {
    fb1_blank(0);   /* restore OSD on exit so FrogUI shows battery */
    if (use_hwdisp) {
        hwdisp_deinit();
        use_hwdisp = 0;
    }
    if (fb_fd >= 0) {
        struct fb_var_screeninfo vi = vinfo;
        vi.xoffset = 0; vi.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);
        if (fb_mem && fb_mem != MAP_FAILED) { munmap(fb_mem, fb_size); fb_mem = NULL; }
        close(fb_fd); fb_fd = -1;
    }
    if (dis_fd >= 0) { close(dis_fd); dis_fd = -1; }
}

/* --------------------------------------------------------------------------
 * Input: cubevol shared memory
 * -------------------------------------------------------------------------- */
#define CV_UP     4
#define CV_DOWN   6
#define CV_LEFT   7
#define CV_RIGHT  5
#define CV_A     13   /* Cross    */
#define CV_B     14   /* Circle   */
#define CV_X     12   /* Square   */
#define CV_Y     15   /* Triangle */
#define CV_L     10
#define CV_R     11
#define CV_L2     8
#define CV_R2     9
#define CV_SEL    0
#define CV_START  3

/* PSX pad bits (0=pressed) */
#define PSX_SELECT    (1<<0)
#define PSX_START     (1<<3)
#define PSX_UP        (1<<4)
#define PSX_RIGHT     (1<<5)
#define PSX_DOWN      (1<<6)
#define PSX_LEFT      (1<<7)
#define PSX_L2        (1<<8)
#define PSX_R2        (1<<9)
#define PSX_L1        (1<<10)
#define PSX_R1        (1<<11)
#define PSX_TRIANGLE  (1<<12)
#define PSX_CIRCLE    (1<<13)
#define PSX_CROSS     (1<<14)
#define PSX_SQUARE    (1<<15)

static volatile uint32_t *cv_keys = NULL;
static uint16_t pad1 = 0xFFFF; /* all released */

static void input_init(void) {
    key_t k = ftok("/tmp/joy_key", 'a');
    if (k == (key_t)-1) { fprintf(stderr, "SF3000: ftok failed\n"); return; }
    int id = shmget(k, 4, 0666);
    if (id < 0) { fprintf(stderr, "SF3000: shmget failed\n"); return; }
    void *p = shmat(id, NULL, 0);
    if (p == (void*)-1) { fprintf(stderr, "SF3000: shmat failed\n"); return; }
    cv_keys = (volatile uint32_t*)p;
}

static inline int btn(uint32_t state, int bit) { return (state >> bit) & 1; }

void config_save(void);

static void sf3000_menu(void) {
    int sel = 0;
    uint32_t prev = cv_keys ? *cv_keys : 0;
    fb1_blank(0);   /* show battery while in pause menu */

    while (1) {
        char scale_label[48];
        snprintf(scale_label,  sizeof(scale_label),  "Scale:  %s",
                 scale_mode == 1 ? "Fullscreen" : "Aspect-Correct");
        char gamma_label[48];
        if (gamma_percent <= 100)
            snprintf(gamma_label, sizeof(gamma_label), "Gamma:  Off");
        else
            snprintf(gamma_label, sizeof(gamma_label), "Gamma:  %d.%02d",
                     gamma_percent / 100, gamma_percent % 100);

        char pixskip_label[48], ilace_label[48];
        snprintf(pixskip_label, sizeof(pixskip_label), "Pixel Skip (speed): %s",
                 gpu_unai_config_ext.pixel_skip ? "On" : "Off");
        snprintf(ilace_label, sizeof(ilace_label), "Interlace (speed):  %s",
                 gpu_unai_config_ext.ilace_force ? "On" : "Off");

        const char *items[] = {
            "Resume",
            "Save State (slot 1)",
            "Load State (slot 1)",
            scale_label,
            gamma_label,
            pixskip_label,
            ilace_label,
            "PCSX Settings",
            "Exit to FrogUI",
            NULL
        };
        int n = 0; while (items[n]) n++;

        video_clear();
        port_printf(2, 0, "=== PCSX4ALL MENU ===");
        for (int i = 0; i < n; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s%s", (i == sel) ? "> " : "  ", items[i]);
            port_printf(2, (i + 2) * 10, buf);
        }
        port_printf(2, (n + 3) * 10, "UP/DOWN=navigate  A=select  B=back");
        video_flip();

        usleep(16000);
        if (!cv_keys) continue;
        uint32_t k = *cv_keys;

        int up  = btn(k, CV_UP)    && !btn(prev, CV_UP);
        int dn  = btn(k, CV_DOWN)  && !btn(prev, CV_DOWN);
        int lt  = btn(k, CV_LEFT)  && !btn(prev, CV_LEFT);
        int rt  = btn(k, CV_RIGHT) && !btn(prev, CV_RIGHT);
        int a   = btn(k, CV_A)     && !btn(prev, CV_A);
        int b   = btn(k, CV_B)     && !btn(prev, CV_B);
        prev = k;

        if (up && sel > 0)   sel--;
        if (dn && sel < n-1) sel++;
        if (b) { fb1_blank(1); return; }
        /* LEFT/RIGHT adjusts scale (sel 3) and gamma (sel 4) */
        if (lt || rt) {
            if (sel == 3) {
                scale_mode = (scale_mode + 1) % 2;
                last_fb_y_off = -1; last_fb_y_len = -1; last_h_blend = -1;
                if (scale_mode == 1) {
                    hwdisp_set_target_aspect(0, 0); hwdisp_set_filter(0);
                } else {
                    hwdisp_set_target_aspect(16, 9); hwdisp_set_filter(1);
                }
            } else if (sel == 4) {
                gamma_percent += rt ? 10 : -10;
                if (gamma_percent < 100) gamma_percent = 100;
                if (gamma_percent > 250) gamma_percent = 250;
            } else if (sel == 5) {
                gpu_unai_config_ext.pixel_skip = !gpu_unai_config_ext.pixel_skip;
            } else if (sel == 6) {
                gpu_unai_config_ext.ilace_force = !gpu_unai_config_ext.ilace_force;
            }
        }
        if (a) {
            switch (sel) {
                case 0: fb1_blank(1); return;
                case 1: state_save(1); usleep(300000); fb1_blank(1); return;
                case 2: state_load(1); usleep(300000); fb1_blank(1); return;
                case 3:
                    scale_mode = (scale_mode + 1) % 2;
                    last_fb_y_off = -1; last_fb_y_len = -1; last_h_blend = -1;
                    hwdisp_set_target_aspect(scale_mode == 1 ? 0 : 16, scale_mode == 1 ? 0 : 9);
                    break;
                case 4: /* gamma: A toggles off/default */
                    gamma_percent = (gamma_percent > 100) ? 100 : 130;
                    break;
                case 5: /* pixel skip: A toggles */
                    gpu_unai_config_ext.pixel_skip = !gpu_unai_config_ext.pixel_skip;
                    break;
                case 6: /* interlace: A toggles */
                    gpu_unai_config_ext.ilace_force = !gpu_unai_config_ext.ilace_force;
                    break;
                case 7: {
                    extern int GameMenu(void);
                    extern void sdl_poll_reset(void);
                    while (cv_keys && btn(*cv_keys, CV_A)) usleep(10000);
                    sdl_poll_reset();
                    GameMenu();
                    prev = cv_keys ? *cv_keys : 0;
                    last_fb_y_off = -1; last_fb_y_len = -1;
                    break;
                }
                case 8: config_save(); exit(0);
            }
        }
    }
}

static uint32_t sdl_prev = 0;
void sdl_poll_reset(void) { sdl_prev = cv_keys ? *cv_keys : 0; }

/* SDL_PollEvent — translates cubevol buttons into SDL key events for frontend.c menu */
int SDL_PollEvent(SDL_Event *e) {
    uint32_t prev = sdl_prev;
    static SDLKey queue[32];
    static uint8_t  qtype[32]; /* SDL_KEYDOWN or SDL_KEYUP */
    static int qhead = 0, qtail = 0;

    static const struct { int cv_bit; SDLKey sym; } map[] = {
        { CV_UP,    SDLK_UP    }, { CV_DOWN,  SDLK_DOWN  },
        { CV_LEFT,  SDLK_LEFT  }, { CV_RIGHT, SDLK_RIGHT },
        { CV_A,     SDLK_LCTRL }, { CV_B,     SDLK_LALT  },
        { CV_X,     SDLK_SPACE }, { CV_Y,     SDLK_LSHIFT},
        { CV_L,     SDLK_TAB   }, { CV_R,     SDLK_BACKSPACE },
        { CV_START, SDLK_RETURN}, { CV_SEL,   SDLK_ESCAPE},
    };
    static const int NMAP = sizeof(map)/sizeof(map[0]);

    if (!e) return 0;

    /* Refill queue from current cv_keys state */
    if (qhead == qtail && cv_keys) {
        uint32_t k = *cv_keys;
        uint32_t changed = k ^ prev;
        sdl_prev = k;
        for (int i = 0; i < NMAP && qtail < 32; i++) {
            if (!((changed >> map[i].cv_bit) & 1)) continue;
            queue[qtail] = map[i].sym;
            qtype[qtail] = ((k >> map[i].cv_bit) & 1) ? SDL_KEYDOWN : SDL_KEYUP;
            qtail++;
        }
    }

    if (qhead == qtail) return 0;

    e->type = qtype[qhead];
    e->key.keysym.sym = queue[qhead];
    qhead++;
    if (qhead == qtail) { qhead = qtail = 0; }
    return 1;
}

void pad_update(void) {
    if (!cv_keys) return;
    uint32_t k = *cv_keys;

    /* SELECT+START → exit to FrogUI */
    if (btn(k, CV_SEL) && btn(k, CV_START)) {
        config_save();
        exit(0);
    }

    /* SELECT+L → in-game menu (wait for release so SELECT doesn't trigger _KEY_BACK) */
    static int menu_was = 0;
    int menu_now = btn(k, CV_SEL) && btn(k, CV_L);
    if (menu_now && !menu_was) {
        while (cv_keys && (btn(*cv_keys, CV_SEL) || btn(*cv_keys, CV_L)))
            usleep(10000);
        sf3000_menu();
    }
    menu_was = menu_now;

    /* Build PSX pad word (0=pressed, 1=released) */
    uint16_t p = 0xFFFF;
    if (btn(k, CV_UP))    p &= ~PSX_UP;
    if (btn(k, CV_DOWN))  p &= ~PSX_DOWN;
    if (btn(k, CV_LEFT))  p &= ~PSX_LEFT;
    if (btn(k, CV_RIGHT)) p &= ~PSX_RIGHT;
    if (btn(k, CV_A))     p &= ~PSX_CROSS;
    if (btn(k, CV_B))     p &= ~PSX_CIRCLE;
    if (btn(k, CV_X))     p &= ~PSX_SQUARE;
    if (btn(k, CV_Y))     p &= ~PSX_TRIANGLE;
    if (btn(k, CV_L))     p &= ~PSX_L1;
    if (btn(k, CV_R))     p &= ~PSX_R1;
    if (btn(k, CV_L2))    p &= ~PSX_L2;
    if (btn(k, CV_R2))    p &= ~PSX_R2;
    if (btn(k, CV_SEL))   p &= ~PSX_SELECT;
    if (btn(k, CV_START)) p &= ~PSX_START;
    pad1 = p;
}

uint16_t pad_read(int num) { return (num == 0 ? pad1 : 0xFFFF); }

/* --------------------------------------------------------------------------
 * Video API
 * -------------------------------------------------------------------------- */
void video_flip(void) {
    if (Config.ShowFps) port_printf(2, 2, pl_data.stats_msg);
    gamma_apply(SCREEN, SCREEN_WIDTH * SCREEN_HEIGHT);
    display_blit(SCREEN, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH * 2);
}

void video_clear(void) {
    memset(SCREEN, 0, SCREEN_WIDTH * SCREEN_HEIGHT * 2);
}

void port_printf(int x, int y, const char *text) {
    if (!SCREEN || !text) return;
    extern void basic_text_out16_nf(void *fb, int w, int x, int y, const char *text);
    basic_text_out16_nf(SCREEN, SCREEN_WIDTH, x, y, text);
}

/* --------------------------------------------------------------------------
 * Timing
 * -------------------------------------------------------------------------- */
unsigned get_ticks(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned)(tv.tv_sec * 1000000ULL + tv.tv_usec);
}

void wait_ticks(unsigned us) {
    usleep(us);
}

/* --------------------------------------------------------------------------
 * Config paths
 * -------------------------------------------------------------------------- */
static char homedir[PATH_MAX]    = "/mnt/sdcard/cubegm/cores/.pcsx4all";
char        sstatesdir[PATH_MAX] = "/mnt/sdcard/cubegm/cores/.pcsx4all/savestates";
char        cheatsdir[PATH_MAX]  = "/mnt/sdcard/cubegm/cores/.pcsx4all/cheats";

struct ps1_controller player_controller[2] = {{0},{0}};

void Set_Controller_Mode(void) {
    switch (Config.Analog_Mode) {
        default:
            player_controller[0].id = 0x41;
            player_controller[0].pad_mode = 0;
            player_controller[0].pad_controllertype = 0;
            break;
        case 1:
            player_controller[0].id = 0x53;
            player_controller[0].pad_mode = 1;
            player_controller[0].pad_controllertype = 1;
            break;
        case 2:
            player_controller[0].id = 0x73;
            player_controller[0].pad_mode = 1;
            player_controller[0].pad_controllertype = 1;
            break;
    }
}

/* --------------------------------------------------------------------------
 * Config load/save (matches icube-written cfg format)
 * -------------------------------------------------------------------------- */
void config_load(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/pcsx4all.cfg", homedir);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    int linenum = 0;
    while (fgets(line, sizeof(line), f)) {
        char *arg = strchr(line, ' ');
        if (!arg) continue;
        *arg++ = '\0';
        /* strip trailing newline */
        char *nl = strchr(arg, '\n'); if (nl) *nl = '\0';
        linenum++;
        int v; sscanf(arg, "%d", &v);

        if (!strcmp(line,"CONFIG_VERSION")) { if (v != 0) break; }
        else if (!strcmp(line,"Xa"))              Config.Xa = v;
        else if (!strcmp(line,"Mdec"))            Config.Mdec = v;
        else if (!strcmp(line,"PsxAuto"))         Config.PsxAuto = v;
        else if (!strcmp(line,"Cdda"))            Config.Cdda = v;
        else if (!strcmp(line,"HLE"))             Config.HLE = v;
        else if (!strcmp(line,"RCntFix"))         Config.RCntFix = v;
        else if (!strcmp(line,"VSyncWA"))         Config.VSyncWA = v;
        else if (!strcmp(line,"Cpu"))             Config.Cpu = v;
        else if (!strcmp(line,"PsxType"))         Config.PsxType = v;
        else if (!strcmp(line,"SpuIrq"))          Config.SpuIrq = v;
        else if (!strcmp(line,"SyncAudio"))       Config.SyncAudio = v;
        else if (!strcmp(line,"SpuUpdateFreq"))   Config.SpuUpdateFreq = v;
        else if (!strcmp(line,"ForcedXAUpdates")) Config.ForcedXAUpdates = v;
        else if (!strcmp(line,"ShowFps"))         Config.ShowFps = v;
        else if (!strcmp(line,"FrameLimit"))      Config.FrameLimit = v;
        else if (!strcmp(line,"FrameSkip"))       Config.FrameSkip = v;
        else if (!strcmp(line,"AnalogArrow"))     Config.AnalogArrow = v;
        else if (!strcmp(line,"Analog_Mode"))     Config.Analog_Mode = v;
        else if (!strcmp(line,"LastDir"))         strncpy(Config.LastDir, arg, sizeof(Config.LastDir)-1);
        else if (!strcmp(line,"BiosDir"))         strncpy(Config.BiosDir, arg, sizeof(Config.BiosDir)-1);
        else if (!strcmp(line,"Bios"))            strncpy(Config.Bios,    arg, sizeof(Config.Bios)-1);
#ifdef PSXREC
        else if (!strcmp(line,"CycleMultiplier")) sscanf(arg, "%x", &cycle_multiplier);
#endif
#ifdef GPU_UNAI
        else if (!strcmp(line,"lighting"))      gpu_unai_config_ext.lighting = v;
        else if (!strcmp(line,"fast_lighting")) gpu_unai_config_ext.fast_lighting = v;
        else if (!strcmp(line,"blending"))      gpu_unai_config_ext.blending = v;
        else if (!strcmp(line,"dithering"))     gpu_unai_config_ext.dithering = v;
        else if (!strcmp(line,"clip_368"))      gpu_unai_config_ext.clip_368 = v;
        else if (!strcmp(line,"pixel_skip"))    gpu_unai_config_ext.pixel_skip = v;
        else if (!strcmp(line,"ilace_force"))   gpu_unai_config_ext.ilace_force = v;
#endif
#ifdef SPU_PCSXREARMED
        else if (!strcmp(line,"SpuUseInterpolation")) spu_config.iUseInterpolation = v;
        else if (!strcmp(line,"SpuUseReverb"))        spu_config.iUseReverb = v;
        else if (!strcmp(line,"SpuVolume"))           spu_config.iVolume = v;
#endif
        else if (!strcmp(line,"ScaleMode"))   scale_mode   = v;
        else if (!strcmp(line,"ScaleFilter")) scale_filter = v;
        else if (!strcmp(line,"Gamma"))       gamma_percent = (v < 100 ? 100 : (v > 250 ? 250 : v));
    }
    fclose(f);
}

void config_save(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/pcsx4all.cfg", homedir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "CONFIG_VERSION 0\n"
               "Xa %d\nMdec %d\nPsxAuto %d\nCdda %d\nHLE %d\n"
               "RCntFix %d\nVSyncWA %d\nCpu %d\nPsxType %d\n"
               "SpuIrq %d\nSyncAudio %d\nSpuUpdateFreq %d\n"
               "ForcedXAUpdates %d\nShowFps %d\nFrameLimit %d\n"
               "FrameSkip %d\nAnalogArrow %d\nAnalog_Mode %d\n",
            Config.Xa, Config.Mdec, Config.PsxAuto, Config.Cdda, Config.HLE,
            Config.RCntFix, Config.VSyncWA, Config.Cpu, Config.PsxType,
            Config.SpuIrq, Config.SyncAudio, Config.SpuUpdateFreq,
            Config.ForcedXAUpdates, Config.ShowFps, Config.FrameLimit,
            Config.FrameSkip, Config.AnalogArrow, Config.Analog_Mode);
#ifdef PSXREC
    fprintf(f, "CycleMultiplier %03x\n", cycle_multiplier);
#endif
#ifdef GPU_UNAI
    fprintf(f, "lighting %d\nfast_lighting %d\nblending %d\ndithering %d\nclip_368 %d\n"
               "pixel_skip %d\nilace_force %d\n",
            gpu_unai_config_ext.lighting, gpu_unai_config_ext.fast_lighting,
            gpu_unai_config_ext.blending, gpu_unai_config_ext.dithering,
            gpu_unai_config_ext.clip_368,
            gpu_unai_config_ext.pixel_skip, gpu_unai_config_ext.ilace_force);
#endif
#ifdef SPU_PCSXREARMED
    fprintf(f, "SpuUseInterpolation %d\nSpuUseReverb %d\nSpuVolume %d\n",
            spu_config.iUseInterpolation, spu_config.iUseReverb, spu_config.iVolume);
#endif
    fprintf(f, "ScaleMode %d\nScaleFilter %d\nGamma %d\n", scale_mode, scale_filter, gamma_percent);
    if (Config.LastDir[0]) fprintf(f, "LastDir %s\n", Config.LastDir);
    if (Config.BiosDir[0]) fprintf(f, "BiosDir %s\n", Config.BiosDir);
    if (Config.Bios[0])    fprintf(f, "Bios %s\n",    Config.Bios);
    fclose(f);
}

/* --------------------------------------------------------------------------
 * State save/load
 * -------------------------------------------------------------------------- */
int state_load(int slot) {
    char path[PATH_MAX+64];
    snprintf(path, sizeof(path), "%s/%s.%d.sav", sstatesdir, CdromId, slot);
    if (access(path, R_OK) == 0) return LoadState(path);
    return -1;
}

int state_save(int slot) {
    mkdir(sstatesdir, 0755);
    char path[PATH_MAX+64];
    snprintf(path, sizeof(path), "%s/%s.%d.sav", sstatesdir, CdromId, slot);
    return SaveState(path);
}


/* --------------------------------------------------------------------------
 * main()
 * -------------------------------------------------------------------------- */
void plog_pub(const char *msg) { (void)msg; }
static void plog(const char *msg) { (void)msg; }

int main(int argc, char *argv[]) {
    plog("main: start");
    if (argc < 2) { plog("main: no ROM arg, exit"); return 1; }
    plog(argv[1]);
    /* Defaults */
    Config.Xa = 0; Config.Mdec = 0; Config.PsxAuto = 1;
    Config.Cdda = 0; Config.HLE = 0;  /* 0=BIOS (preferred); psxMemInit falls back to HLE if missing */
    Config.SpuUpdateFreq = 4; Config.ForcedXAUpdates = 1;
    Config.ShowFps = 0; Config.FrameLimit = 1; Config.FrameSkip = -1;
#ifdef PSXREC
    /* Stock 0x200 (2.0).  LOWERING it makes each instruction cost fewer cycles,
     * so more instructions are emulated per frame = MORE host work = slower.
     * RAISING it (e.g. 0x240/0x280) is the speed-hack (fewer emulated
     * instructions) but starves the game's CPU → in-game slowdown/glitches.
     * Keep accurate default; tune per-game via pcsx4all.cfg CycleMultiplier. */
    cycle_multiplier = 0x200;
#endif
    strncpy(Config.BiosDir, homedir, sizeof(Config.BiosDir)-1);
    strncpy(Config.Bios, "scph1001.bin", sizeof(Config.Bios)-1);

#ifdef GPU_UNAI
    gpu_unai_config_ext.lighting     = 0;
    gpu_unai_config_ext.fast_lighting= 1;
    gpu_unai_config_ext.blending     = 0;
    gpu_unai_config_ext.dithering    = 0;
    gpu_unai_config_ext.clip_368     = 0;
    /* SF3000: keep GPU output full-quality by default — NO line skipping
     * (ilace_force=0) and NO pixel skipping (pixel_skip=0; risky with the HW
     * downscaler anyway).  Speed comes from the lossless levers (cycle_multiplier
     * + CPU governor).  Both remain per-game opt-in via pcsx4all.cfg. */
    gpu_unai_config_ext.pixel_skip   = 0;
    gpu_unai_config_ext.ilace_force  = 0;
#endif
#ifdef SPU_PCSXREARMED
    spu_config.iHaveConfiguration = 1;
    spu_config.iUseReverb         = 0;
    spu_config.iUseInterpolation  = 0;
    spu_config.iXAPitch           = 0;
    spu_config.iVolume            = 1024;
    spu_config.iUseThread         = 0;
    spu_config.iUseFixedUpdates   = 0;
    spu_config.iDisabled          = 0;
#endif

    config_load();

    /* Memory cards: the sf3000 port never set these, so Config.Mcd1/2 were
     * empty → BIOS reported "no card" → games couldn't save. Point them at
     * persistent files under homedir/memcards (LoadMcd auto-creates the .mcr
     * if the parent dir exists). */
    {
        static char mcddir[PATH_MAX];
        snprintf(mcddir, sizeof(mcddir), "%s/memcards", homedir);
        mkdir(mcddir, 0755);
        snprintf(Config.Mcd1, sizeof(Config.Mcd1), "%s/mcd001.mcr", mcddir);
        snprintf(Config.Mcd2, sizeof(Config.Mcd2), "%s/mcd002.mcr", mcddir);
    }

    /* BIOS setup: defaults above set Config.HLE=0, Config.BiosDir=homedir,
     * Config.Bios="scph1001.bin".  config_load() overwrites these with saved
     * values from pcsx4all.cfg if it exists.  psxMemInit() auto-reverts to HLE
     * if the BIOS file is missing.  No post-load override needed — user's
     * saved settings are respected across sessions. */

    /* ROM from command line arg 1 */
    if (argc >= 2) {
        SetIsoFile(argv[1]);
        strncpy(Config.LastDir, argv[1], sizeof(Config.LastDir)-1);
        char *sl = strrchr(Config.LastDir, '/');
        if (sl) *sl = '\0';
    }

    if (display_init() < 0) { plog("display_init FAILED"); return 1; }
    atexit(shutdown_display);
    input_init();

    mkdir(sstatesdir, 0755);
    mkdir(cheatsdir, 0755);

    if (psxInit() == -1) { plog("psxInit FAILED"); return 1; }
    {
        extern R3000Acpu psxRec;
        plog(psxCpu == &psxRec ? "dynarec ACTIVE" : "INTERPRETER (slow!)");
    }

    /* This port hand-rolls plugin init and never calls LoadPlugins(), so do the
     * memcard load here (creates the .mcr files if absent). Without this the
     * cards are never inserted → games can't save. */
    LoadMcd(MCD1, Config.Mcd1);
    LoadMcd(MCD2, Config.Mcd2);

    if (CDR_init() < 0) { plog("CDR_init FAILED"); return 1; }
    if (GPU_init() < 0) { plog("GPU_init FAILED"); return 1; }
    if (SPU_init() < 0) { plog("SPU_init FAILED"); return 1; }
    if (CDR_open() < 0) { plog("CDR_open FAILED"); return 1; }

    pl_init();
    psxReset();

    if (argc >= 2) {
        if (CheckCdrom() == -1) {
            SetIsoFile(NULL);
        } else if (LoadCdrom() == -1) {
            SetIsoFile(NULL);
        } else {
            cheat_load();
        }
    }

    CheckforCDROMid_applyhacks();

    psxCpu->Execute();

    config_save();
    return 0;
}
