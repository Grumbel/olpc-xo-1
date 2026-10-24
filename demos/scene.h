/* Shared scene for the acceleration demos: bouncing 32x32 balls over a
 * scrolling background, available as ARGB32, RGB565+mask and YV12 data. */
#ifndef XO_SCENE_H
#define XO_SCENE_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define SPR 32
#define NKINDS 8
#define MAXSPR 4000

typedef struct {
    float x, y, dx, dy;
    int kind;
} Sprite;

static Sprite sprites[MAXSPR];

/* per kind: premultiplied ARGB32, RGB565 + 1-bit mask, YUV planes + masks */
static uint32_t spr_argb[NKINDS][SPR * SPR];
static uint16_t spr_565[NKINDS][SPR * SPR];
static uint8_t spr_mask[NKINDS][SPR * SPR];
static uint8_t spr_y[NKINDS][SPR * SPR];
static uint8_t spr_u[NKINDS][SPR / 2 * SPR / 2], spr_v[NKINDS][SPR / 2 * SPR / 2];
static uint8_t spr_cmask[NKINDS][SPR / 2 * SPR / 2];

static inline double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static inline void rgb_to_yuv(int r, int g, int b, uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
    *u = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
    *v = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
}

static inline void make_sprites(void)
{
    static const int pal[NKINDS][3] = {
        {255, 64, 64}, {64, 255, 64}, {64, 128, 255}, {255, 255, 64},
        {255, 64, 255}, {64, 255, 255}, {255, 160, 32}, {240, 240, 240},
    };
    for (int k = 0; k < NKINDS; k++) {
        for (int py = 0; py < SPR; py++)
            for (int px = 0; px < SPR; px++) {
                float dx = px + 0.5f - SPR / 2.0f, dy = py + 0.5f - SPR / 2.0f;
                float d = sqrtf(dx * dx + dy * dy);
                float a = fminf(fmaxf(SPR / 2.0f - 0.5f - d, 0.0f), 1.0f);
                /* highlight towards the upper left */
                float hx = dx + 6, hy = dy + 6;
                float light = 1.0f - sqrtf(hx * hx + hy * hy) / (SPR * 0.9f);
                light = fminf(fmaxf(light, 0.25f), 1.0f);
                int r = (int)(pal[k][0] * light), g = (int)(pal[k][1] * light),
                    b = (int)(pal[k][2] * light);
                int ai = (int)(a * 255);
                int i = py * SPR + px;
                spr_argb[k][i] = (uint32_t)ai << 24 | (uint32_t)(r * ai / 255) << 16 |
                                 (uint32_t)(g * ai / 255) << 8 | (uint32_t)(b * ai / 255);
                spr_565[k][i] = rgb565(r, g, b);
                spr_mask[k][i] = ai >= 128;
                uint8_t u, v;
                rgb_to_yuv(r, g, b, &spr_y[k][i], &u, &v);
                if (!(px & 1) && !(py & 1)) {
                    int c = (py / 2) * (SPR / 2) + px / 2;
                    spr_u[k][c] = u;
                    spr_v[k][c] = v;
                    spr_cmask[k][c] = ai >= 128;
                }
            }
    }
}

static inline void init_sprites(int n, int w, int h)
{
    srand(1);
    for (int i = 0; i < n && i < MAXSPR; i++) {
        sprites[i].x = (float)(rand() % (w - SPR));
        sprites[i].y = (float)(rand() % (h - SPR));
        sprites[i].dx = (rand() % 200 - 100) / 25.0f;
        sprites[i].dy = (rand() % 200 - 100) / 25.0f;
        sprites[i].kind = i % NKINDS;
    }
}

static inline void move_sprites(int n, int w, int h)
{
    for (int i = 0; i < n; i++) {
        Sprite *s = &sprites[i];
        s->x += s->dx;
        s->y += s->dy;
        if (s->x < 0 || s->x > w - SPR) {
            s->dx = -s->dx;
            s->x = s->x < 0 ? 0 : (float)(w - SPR);
        }
        if (s->y < 0 || s->y > h - SPR) {
            s->dy = -s->dy;
            s->y = s->y < 0 ? 0 : (float)(h - SPR);
        }
    }
}

/* Background colour at (x, y): diagonal gradient with a checkerboard. */
static inline void bg_rgb(int x, int y, int *r, int *g, int *b)
{
    int check = ((x >> 5) ^ (y >> 5)) & 1;
    *r = (x * 255 / 1200) / 2 + check * 40;
    *g = (y * 255 / 900) / 2 + check * 40;
    *b = 96 + check * 40;
}

/* CPU sprite drawing into an RGB565 buffer */
static inline void draw_sprites_565(uint16_t *fb, int pitch, int n)
{
    for (int i = 0; i < n; i++) {
        int sx = (int)sprites[i].x, sy = (int)sprites[i].y, k = sprites[i].kind;
        for (int py = 0; py < SPR; py++) {
            uint16_t *row = fb + (sy + py) * pitch + sx;
            const uint16_t *src = spr_565[k] + py * SPR;
            const uint8_t *m = spr_mask[k] + py * SPR;
            for (int px = 0; px < SPR; px++)
                if (m[px])
                    row[px] = src[px];
        }
    }
}

/* CPU sprite drawing into YV12/I420 planes (positions rounded to even) */
static inline void draw_sprites_yuv(uint8_t *y, int ypitch, uint8_t *u, uint8_t *v, int cpitch, int n)
{
    for (int i = 0; i < n; i++) {
        int sx = (int)sprites[i].x & ~1, sy = (int)sprites[i].y & ~1, k = sprites[i].kind;
        for (int py = 0; py < SPR; py++) {
            uint8_t *row = y + (sy + py) * ypitch + sx;
            const uint8_t *src = spr_y[k] + py * SPR, *m = spr_mask[k] + py * SPR;
            for (int px = 0; px < SPR; px++)
                if (m[px])
                    row[px] = src[px];
        }
        for (int py = 0; py < SPR / 2; py++) {
            uint8_t *ur = u + (sy / 2 + py) * cpitch + sx / 2;
            uint8_t *vr = v + (sy / 2 + py) * cpitch + sx / 2;
            const uint8_t *m = spr_cmask[k] + py * SPR / 2;
            for (int px = 0; px < SPR / 2; px++)
                if (m[px]) {
                    ur[px] = spr_u[k][py * SPR / 2 + px];
                    vr[px] = spr_v[k][py * SPR / 2 + px];
                }
        }
    }
}

/* Copy a horizontally scrolled copy of `src` (w*h, same pitch) into `dst`. */
static inline void scroll_copy(uint8_t *dst, int dpitch, const uint8_t *src, int spitch, int wbytes,
                        int h, int offbytes)
{
    for (int yy = 0; yy < h; yy++) {
        const uint8_t *s = src + yy * spitch;
        uint8_t *d = dst + yy * dpitch;
        memcpy(d, s + offbytes, (size_t)(wbytes - offbytes));
        memcpy(d + wbytes - offbytes, s, (size_t)offbytes);
    }
}

#endif
