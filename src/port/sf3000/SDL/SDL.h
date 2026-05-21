/* Minimal SDL stub for SF3000 non-SDL port */
#ifndef _SDL_H
#define _SDL_H
#include <stdint.h>
#include <unistd.h>

typedef struct SDL_Surface {
    void    *pixels;
    int      pitch;
    int      w, h;
} SDL_Surface;

typedef enum {
    SDLK_UP=273, SDLK_DOWN, SDLK_RIGHT, SDLK_LEFT,
    SDLK_ESCAPE=27, SDLK_RETURN=13, SDLK_BACKSPACE=8, SDLK_TAB=9,
    SDLK_SPACE=32, SDLK_LSHIFT=304, SDLK_LCTRL=306, SDLK_LALT=308,
    SDLK_m='m', SDLK_v='v', SDLK_HOME=278,
    SDLK_LAST=512
} SDLKey;

typedef struct { SDLKey sym; } SDL_keysym;
typedef struct { SDL_keysym keysym; } SDL_KeyboardEvent;
typedef struct {
    uint8_t type;
    SDL_KeyboardEvent key;
} SDL_Event;

enum { SDL_KEYDOWN=2, SDL_KEYUP=3, SDL_QUIT=12 };
enum { SDL_INIT_VIDEO=1, SDL_INIT_JOYSTICK=2, SDL_INIT_NOPARACHUTE=4 };

static inline int  SDL_Init(uint32_t f) { (void)f; return 0; }
static inline void SDL_Quit(void) {}
int SDL_PollEvent(SDL_Event *e);
static inline uint32_t SDL_GetTicks(void) { return 0; }
static inline void SDL_Delay(uint32_t ms) { usleep((unsigned int)(ms)*1000); }

#endif /* _SDL_H */
