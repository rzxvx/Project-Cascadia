/* sdlshot: LD_PRELOAD; saves frame SDLSHOT_FRAME (default 300) of the real
 * SDL renderer to /tmp/ref.ppm -- the reference for sgxsdl's output. */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

void SDL_RenderPresent(SDL_Renderer *r)
{
	static void (*real)(SDL_Renderer *);
	static int frame;
	int want = getenv("SDLSHOT_FRAME") ? atoi(getenv("SDLSHOT_FRAME")) : 300;

	if (!real)
		real = dlsym(RTLD_NEXT, "SDL_RenderPresent");
	if (++frame == want) {
		int w, h, i;
		unsigned char *p;
		FILE *f;

		SDL_GetRendererOutputSize(r, &w, &h);
		p = malloc(w * h * 3);
		SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_RGB24, p, w * 3);
		f = fopen("/tmp/ref.ppm", "wb");
		fprintf(f, "P6 %d %d 255\n", w, h);
		fwrite(p, 1, w * h * 3, f);
		fclose(f);
		fprintf(stderr, "sdlshot: frame %d saved (%dx%d)\n", frame, w, h);
		(void)i;
	}
	real(r);
}
