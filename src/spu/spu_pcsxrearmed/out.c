#include <stdio.h>
#include <stdlib.h>
#include "out.h"

#include "spu_config.h"	//senquack - to read new iDisabled setting

#define MAX_OUT_DRIVERS 5

static struct out_driver out_drivers[MAX_OUT_DRIVERS];
struct out_driver *out_current;
static int driver_count;

#define REGISTER_DRIVER(d) { \
	extern void out_register_##d(struct out_driver *drv); \
	out_register_##d(&out_drivers[driver_count++]); \
}

void SetupSound(void)
{
	int i;

	if (driver_count == 0) {

		//senquack - added iDisabled setting to force muting / use of nullspu driver
		if (spu_config.iDisabled) {
			REGISTER_DRIVER(none);
		} else {
#ifdef HAVE_SF3000
			REGISTER_DRIVER(sf3000);
#endif
#ifdef HAVE_OSS
			REGISTER_DRIVER(oss);
#endif
#ifdef HAVE_ALSA
			REGISTER_DRIVER(alsa);
#endif
#ifdef HAVE_SDL
			REGISTER_DRIVER(sdl);
#endif
#ifdef HAVE_PULSE
			REGISTER_DRIVER(pulse);
#endif
#ifdef HAVE_LIBRETRO
			REGISTER_DRIVER(libretro);
#endif
#if !defined(HAVE_SF3000) && !defined(HAVE_OSS) && !defined(HAVE_ALSA) && !defined(HAVE_SDL) && !defined(HAVE_PULSE) && !defined(HAVE_LIBRETRO)
			REGISTER_DRIVER(none);
#endif
		}
	}

	for (i = 0; i < driver_count; i++)
		if (out_drivers[i].init() == 0)
			break;

	if (i < 0 || i >= driver_count) {
		printf("the impossible happened\n");
		abort();
	}

	out_current = &out_drivers[i];
	printf("selected sound output driver: %s\n", out_current->name);
}

