/* Draws a scrolling RGB565 gradient for two seconds and reports the frame rate. */
#include <SDL.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Surface *s = SDL_SetVideoMode(1200, 900, 16, SDL_SWSURFACE | SDL_FULLSCREEN);
    if (!s) {
        fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError());
        return 1;
    }
    char driver[32];
    printf("SDL %d.%d.%d, video driver %s, %dx%d@%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_VideoDriverName(driver, sizeof driver), s->w, s->h,
           s->format->BitsPerPixel);

    Uint32 start = SDL_GetTicks();
    int frames = 0;
    while (SDL_GetTicks() - start < 2000) {
        SDL_LockSurface(s);
        for (int y = 0; y < s->h; y++) {
            Uint16 *row = (Uint16 *)((Uint8 *)s->pixels + y * s->pitch);
            Uint16 g = (Uint16)(((y >> 2) & 0x3f) << 5);
            for (int x = 0; x < s->w; x++)
                row[x] = (Uint16)((((x + frames) >> 3) & 0x1f) << 11 | g | (frames & 0x1f));
        }
        SDL_UnlockSurface(s);
        SDL_Flip(s);
        frames++;
    }
    printf("%d frames in 2 s: %.1f fps\n", frames, frames / 2.0);
    SDL_Quit();
    return 0;
}
