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
#include <dirent.h>

#include "SDL.h"
#include "port.h"
#include "r3000a.h"
#include "plugins.h"
#include "plugin_lib.h"
#include "perfmon.h"
#include "cdrom_hacks.h"
#include "cheat.h"

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
static int       last_gh        = -1;
static int       last_disp_h    = -1;
static int       last_fb_y_off  = -1;
static int       last_fb_y_len  = -1;

struct blend_data { int16_t gy1, gy2; uint8_t w1, w2; };
static struct blend_data blend_lut[FB_X_VIS];

unsigned short *SCREEN        = NULL;
int             SCREEN_WIDTH  = 320;
int             SCREEN_HEIGHT = 240;
int             scale_mode    = 0;   /* 0=aspect-correct, 1=fullscreen */

/* RGB565 → ARGB8888 with full 8-bit channel expansion */
static inline uint32_t cvt565(uint16_t c) {
    return 0xFF000000u
         | (uint32_t)((c & 0xF800) << 8) | (uint32_t)((c & 0xE000) << 3)
         | (uint32_t)((c & 0x07E0) << 5) | (uint32_t)((c & 0x0600) >> 1)
         | (uint32_t)((c & 0x001F) << 3) | (uint32_t)((c & 0x001C) >> 2);
}

static void dis_assert(void) {
    int dis = open("/dev/dis", O_RDWR);
    if (dis >= 0) {
        struct { int a, b, c; } buf = {1, 0, 0};
        ioctl(dis, 0xc00c0e0c, &buf);
        close(dis);
    }
}

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
    return 0;
}

static void display_blit(const void *src, int w, int h, int pitch) {
    dis_assert();
    if (!fb_mem || !src) return;

    const int dst_stride = (int)(finfo.line_length / 4);

    /* Rotate: transpose src (w×h) → rotated_buf (h×w) */
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

    /* Rebuild vertical blend LUT — always fill full height */
    if (h != last_gh || h != last_disp_h) {
        int fb_x_len = FB_X_VIS;
        int fb_x_off = 0;
        for (int fx = 0; fx < FB_X_VIS; fx++) {
            if (fx < fb_x_off || fx >= fb_x_off + fb_x_len) {
                blend_lut[fx].gy1 = -1;
                blend_lut[fx].w2  = 0;
            } else {
                int ax = fx - fb_x_off;
                int gf = ((fb_x_len - 1 - ax) * h * 256) / fb_x_len;
                int gy = gf >> 8;
                blend_lut[fx].gy1 = gy;
                blend_lut[fx].gy2 = (gy + 1 < h) ? (gy + 1) : gy;
                blend_lut[fx].w2  = (gf & 0xFF) >> 1;
                blend_lut[fx].w1  = 128 - blend_lut[fx].w2;
            }
        }
        last_gh     = h;
        last_disp_h = h;
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
        for (int fx = 0; fx < FB_X_VIS; fx++) {
            int gy1 = blend_lut[fx].gy1;
            if (gy1 < 0) { row_cache[fx] = 0xFF000000u; continue; }
            int gy2 = blend_lut[fx].gy2;
            uint16_t p1 = col[gy1];
            if (blend_lut[fx].w2 == 0 || gy1 == gy2) {
                row_cache[fx] = cvt565(p1);
            } else {
                uint16_t p2 = col[gy2];
                if (p1 == p2) { row_cache[fx] = cvt565(p1); }
                else {
                    uint32_t c1 = cvt565(p1), c2 = cvt565(p2);
                    uint32_t w1 = blend_lut[fx].w1, w2 = blend_lut[fx].w2;
                    uint32_t rb = (((c1&0xFF00FF)*w1 + (c2&0xFF00FF)*w2) >> 7) & 0xFF00FF;
                    uint32_t g  = (((c1&0x00FF00)*w1 + (c2&0x00FF00)*w2) >> 7) & 0x00FF00;
                    row_cache[fx] = 0xFF000000u | rb | g;
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

    /* Page flip */
    struct fb_var_screeninfo vi = vinfo;
    vi.xoffset = 0; vi.yoffset = page_y;
    ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);

    /* Frame timing: target 16666 µs (60 fps) */
    static struct timeval last_tv = {0,0};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (last_tv.tv_sec) {
        long long diff = (tv.tv_sec - last_tv.tv_sec)*1000000LL
                       + (tv.tv_usec - last_tv.tv_usec);
        if (diff > 0 && diff < 16666) {
            long long sl = 16666 - diff;
            if (sl > 2000) usleep(sl - 2000);
            do { gettimeofday(&tv, NULL);
                 diff = (tv.tv_sec-last_tv.tv_sec)*1000000LL
                       +(tv.tv_usec-last_tv.tv_usec);
            } while (diff < 16666);
        }
    }
    last_tv = tv;
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
#define CV_X     12   /* Triangle */
#define CV_Y     15   /* Square   */
#define CV_L     10
#define CV_R     11
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

    while (1) {
        char scale_label[48];
        snprintf(scale_label, sizeof(scale_label), "Scale: %s",
                 scale_mode == 1 ? "Fullscreen" : "Aspect-Correct");

        const char *items[] = {
            "Resume",
            "Save State (slot 1)",
            "Load State (slot 1)",
            scale_label,
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

        int up  = btn(k, CV_UP)   && !btn(prev, CV_UP);
        int dn  = btn(k, CV_DOWN) && !btn(prev, CV_DOWN);
        int a   = btn(k, CV_A)    && !btn(prev, CV_A);
        int b   = btn(k, CV_B)    && !btn(prev, CV_B);
        prev = k;

        if (up && sel > 0)   sel--;
        if (dn && sel < n-1) sel++;
        if (b) return;
        if (a) {
            switch (sel) {
                case 0: return;
                case 1: state_save(1); usleep(300000); return;
                case 2: state_load(1); usleep(300000); return;
                case 3:
                    scale_mode = (scale_mode + 1) % 2;
                    last_fb_y_off = -1; last_fb_y_len = -1; /* force redraw */
                    break;
                case 4: {
                    extern int GameMenu(void);
                    extern void sdl_poll_reset(void);
                    while (cv_keys && btn(*cv_keys, CV_A)) usleep(10000);
                    sdl_poll_reset();
                    GameMenu();
                    prev = cv_keys ? *cv_keys : 0;
                    last_fb_y_off = -1; last_fb_y_len = -1;
                    break;
                }
                case 5: config_save(); exit(0);
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
    if (btn(k, CV_X))     p &= ~PSX_TRIANGLE;
    if (btn(k, CV_Y))     p &= ~PSX_SQUARE;
    if (btn(k, CV_L))     p &= ~PSX_L1;
    if (btn(k, CV_R))     p &= ~PSX_R1;
    if (btn(k, CV_SEL))   p &= ~PSX_SELECT;
    if (btn(k, CV_START)) p &= ~PSX_START;
    pad1 = p;
}

uint16_t pad_read(int num) { return (num == 0 ? pad1 : 0xFFFF); }

/* --------------------------------------------------------------------------
 * Video API
 * -------------------------------------------------------------------------- */
void video_flip(void) {
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
#endif
#ifdef SPU_PCSXREARMED
        else if (!strcmp(line,"SpuUseInterpolation")) spu_config.iUseInterpolation = v;
        else if (!strcmp(line,"SpuUseReverb"))        spu_config.iUseReverb = v;
        else if (!strcmp(line,"SpuVolume"))           spu_config.iVolume = v;
#endif
        else if (!strcmp(line,"ScaleMode")) scale_mode = v;
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
    fprintf(f, "lighting %d\nfast_lighting %d\nblending %d\ndithering %d\nclip_368 %d\n",
            gpu_unai_config_ext.lighting, gpu_unai_config_ext.fast_lighting,
            gpu_unai_config_ext.blending, gpu_unai_config_ext.dithering,
            gpu_unai_config_ext.clip_368);
#endif
#ifdef SPU_PCSXREARMED
    fprintf(f, "SpuUseInterpolation %d\nSpuUseReverb %d\nSpuVolume %d\n",
            spu_config.iUseInterpolation, spu_config.iUseReverb, spu_config.iVolume);
#endif
    fprintf(f, "ScaleMode %d\n", scale_mode);
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
void plog_pub(const char *msg) {
    int fd = open("/mnt/sdcard/pcsx4all_boot.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd >= 0) { write(fd, msg, strlen(msg)); write(fd, "\n", 1); close(fd); }
}

static void plog(const char *msg) {
    int fd = open("/mnt/sdcard/pcsx4all_boot.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, msg, strlen(msg));
        write(fd, "\n", 1);
        close(fd);
    }
}

int main(int argc, char *argv[]) {
    plog("main: start");
    if (argc < 2) { plog("main: no ROM arg, exit"); return 1; }
    plog(argv[1]);
    /* Defaults */
    Config.Xa = 0; Config.Mdec = 0; Config.PsxAuto = 1;
    Config.Cdda = 0; Config.HLE = 1;
    Config.SpuUpdateFreq = 4; Config.ForcedXAUpdates = 1;
    Config.ShowFps = 0; Config.FrameLimit = 1; Config.FrameSkip = -1;
#ifdef PSXREC
    cycle_multiplier = 0x200;
#endif
    strncpy(Config.BiosDir, homedir, sizeof(Config.BiosDir)-1);
    strncpy(Config.Bios, "scph1001.bin", sizeof(Config.Bios)-1);

#ifdef GPU_UNAI
    gpu_unai_config_ext.lighting     = 1;
    gpu_unai_config_ext.fast_lighting= 1;
    gpu_unai_config_ext.blending     = 1;
    gpu_unai_config_ext.dithering    = 0;
    gpu_unai_config_ext.clip_368     = 0;
#endif

    config_load();
    plog("main: config loaded");

    /* ROM from command line arg 1 */
    if (argc >= 2) {
        SetIsoFile(argv[1]);
        strncpy(Config.LastDir, argv[1], sizeof(Config.LastDir)-1);
        char *sl = strrchr(Config.LastDir, '/');
        if (sl) *sl = '\0';
    }

    if (display_init() < 0) { plog("main: display_init FAILED"); return 1; }
    plog("main: display_init OK");
    input_init();
    plog("main: input_init OK");

    mkdir(sstatesdir, 0755);
    mkdir(cheatsdir, 0755);

    if (psxInit() == -1) { plog("main: psxInit FAILED"); return 1; }
    plog("main: psxInit OK");
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
    plog("main: CDR_init start");
    if (CDR_init() < 0) { plog("main: CDR_init FAILED"); return 1; }
    plog("main: GPU_init start");
    if (GPU_init() < 0) { plog("main: GPU_init FAILED"); return 1; }
    plog("main: SPU_init start");
    if (SPU_init() < 0) { plog("main: SPU_init FAILED"); return 1; }
    plog("main: CDR_open start");
    if (CDR_open() < 0) { plog("main: CDR_open FAILED"); return 1; }
    plog("main: plugins OK");
    plog("main: LoadPlugins OK");

    pl_init();
    psxReset();
    plog("main: psxReset OK");

    if (argc >= 2) {
        if (CheckCdrom() == -1) {
            plog("main: CheckCdrom FAILED");
            SetIsoFile(NULL);
        } else if (LoadCdrom() == -1) {
            plog("main: LoadCdrom FAILED");
            SetIsoFile(NULL);
        } else {
            cheat_load();
            plog("main: LoadCdrom OK");
        }
    }

    CheckforCDROMid_applyhacks();
    plog("main: starting CPU");

    psxCpu->Execute();

    config_save();
    return 0;
}
