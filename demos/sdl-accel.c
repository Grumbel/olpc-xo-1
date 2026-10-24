/* sdl-accel: what SDL 1.2 can use of the Geode LX acceleration.
 *
 *   sdl-accel [-t seconds] [-noscale]
 *
 * Prints SDL_GetVideoInfo(), then runs:
 *   sw-blit   1200x900 software surface, 200 colour-keyed sprites (RLE), SDL_Flip
 *   yuv-640   640x480 YV12 overlay drawn by the CPU, scaled to fullscreen by
 *             SDL_DisplayYUVOverlay (XVideo on X11 -> hardware scaler);
 *             with -noscale it is shown 1:1, centred
 */
#include <SDL.h>
#include <stdio.h>

#include "scene.h"

#define SW 640
#define SH 480
#define NSPR 200

static double duration = 3.0;
static int noscale;

static void print_info(void)
{
    const SDL_VideoInfo *vi = SDL_GetVideoInfo();
    char drv[32];
    printf("SDL %d.%d.%d, video driver %s\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
           SDL_PATCHLEVEL, SDL_VideoDriverName(drv, sizeof drv));
    printf("hw_available=%u wm_available=%u blit_hw=%u blit_hw_CC=%u blit_hw_A=%u\n"
           "blit_sw=%u blit_sw_CC=%u blit_sw_A=%u blit_fill=%u video_mem=%u KB\n",
           vi->hw_available, vi->wm_available, vi->blit_hw, vi->blit_hw_CC, vi->blit_hw_A,
           vi->blit_sw, vi->blit_sw_CC, vi->blit_sw_A, vi->blit_fill, vi->video_mem);
}

static void test_sw_blit(SDL_Surface *screen)
{
    SDL_Surface *bg = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, screen->h, 16, 0xf800,
                                           0x07e0, 0x001f, 0);
    SDL_Surface *spr[NKINDS];
    for (int y = 0; y < screen->h; y++)
        for (int x = 0; x < screen->w; x++) {
            int r, g, b;
            bg_rgb(x, y, &r, &g, &b);
            ((Uint16 *)((Uint8 *)bg->pixels + y * bg->pitch))[x] = rgb565(r, g, b);
        }
    for (int k = 0; k < NKINDS; k++) {
        spr[k] = SDL_CreateRGBSurface(SDL_SWSURFACE, SPR, SPR, 16, 0xf800, 0x07e0, 0x001f, 0);
        for (int i = 0; i < SPR * SPR; i++)
            ((Uint16 *)spr[k]->pixels)[i] = spr_mask[k][i] ? (spr_565[k][i] | 0x0001) : 0;
        SDL_SetColorKey(spr[k], SDL_SRCCOLORKEY | SDL_RLEACCEL, 0);
    }

    init_sprites(NSPR, screen->w, screen->h);
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(NSPR, screen->w, screen->h);
        SDL_BlitSurface(bg, NULL, screen, NULL);
        for (int i = 0; i < NSPR; i++) {
            SDL_Rect d = {(Sint16)sprites[i].x, (Sint16)sprites[i].y, 0, 0};
            SDL_BlitSurface(spr[sprites[i].kind], NULL, screen, &d);
        }
        SDL_Flip(screen);
        frames++;
    }
    printf("%-10s %7.1f fps  %dx%d software surface (flags 0x%x), %d RLE sprites\n", "sw-blit",
           frames / t, screen->w, screen->h, screen->flags, NSPR);
    for (int k = 0; k < NKINDS; k++)
        SDL_FreeSurface(spr[k]);
    SDL_FreeSurface(bg);
}

static void test_yuv(SDL_Surface *screen)
{
    SDL_Overlay *ov = SDL_CreateYUVOverlay(SW, SH, SDL_YV12_OVERLAY, screen);
    if (!ov) {
        printf("yuv-640: SDL_CreateYUVOverlay failed: %s\n", SDL_GetError());
        return;
    }
    uint8_t *bgy = malloc(SW * SH), *bgu = malloc(SW * SH / 4), *bgv = malloc(SW * SH / 4);
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int r, g, b;
            uint8_t yy, u, v;
            bg_rgb(x * screen->w / SW, y * screen->h / SH, &r, &g, &b);
            rgb_to_yuv(r, g, b, &yy, &u, &v);
            bgy[y * SW + x] = yy;
            if (!(x & 1) && !(y & 1)) {
                bgu[(y / 2) * (SW / 2) + x / 2] = u;
                bgv[(y / 2) * (SW / 2) + x / 2] = v;
            }
        }

    SDL_Rect dst = {0, 0, (Uint16)screen->w, (Uint16)screen->h};
    if (noscale)
        dst = (SDL_Rect){(Sint16)((screen->w - SW) / 2), (Sint16)((screen->h - SH) / 2), SW, SH};
    init_sprites(NSPR, SW, SH);
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(NSPR, SW, SH);
        int off = (frames * 2) % SW;
        SDL_LockYUVOverlay(ov);
        /* YV12 planes: Y, V, U */
        scroll_copy(ov->pixels[0], ov->pitches[0], bgy, SW, SW, SH, off);
        scroll_copy(ov->pixels[2], ov->pitches[2], bgu, SW / 2, SW / 2, SH / 2, off / 2);
        scroll_copy(ov->pixels[1], ov->pitches[1], bgv, SW / 2, SW / 2, SH / 2, off / 2);
        draw_sprites_yuv(ov->pixels[0], ov->pitches[0], ov->pixels[2], ov->pixels[1],
                         ov->pitches[2], NSPR);
        SDL_UnlockYUVOverlay(ov);
        SDL_DisplayYUVOverlay(ov, &dst);
        frames++;
    }
    printf("%-10s %7.1f fps  %dx%d YV12 -> %dx%d, hw_overlay=%u, %d CPU-drawn sprites\n",
           "yuv-640", frames / t, SW, SH, dst.w, dst.h, ov->hw_overlay, NSPR);
    SDL_FreeYUVOverlay(ov);
    free(bgy);
    free(bgu);
    free(bgv);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc)
            duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "-noscale"))
            noscale = 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    print_info();
    make_sprites();

    SDL_Surface *screen = SDL_SetVideoMode(0, 0, 16, SDL_HWSURFACE | SDL_DOUBLEBUF | SDL_FULLSCREEN);
    if (!screen) {
        fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError());
        return 1;
    }
    printf("asked for SDL_HWSURFACE|SDL_DOUBLEBUF, got %s%s\n\n",
           screen->flags & SDL_HWSURFACE ? "SDL_HWSURFACE" : "SDL_SWSURFACE",
           screen->flags & SDL_DOUBLEBUF ? "|SDL_DOUBLEBUF" : "");
    SDL_ShowCursor(SDL_DISABLE);

    test_sw_blit(screen);
    test_yuv(screen);
    SDL_Quit();
    return 0;
}
