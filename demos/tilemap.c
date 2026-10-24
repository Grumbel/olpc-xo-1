/* tilemap: smooth-scrolling tilemap with alpha-blended sprites, two ways.
 *
 *   blit  native 1200x900. 32x32 tiles in VRAM pixmaps, composed by the GP
 *         blitter into a wrap-around ring buffer. Per frame only newly exposed
 *         tiles are blitted, the visible part is copied out (<= 4 blits),
 *         then alpha sprites go on top with XRender Over (EXA Composite).
 *   xv    640x480 rendered by the CPU (16x16 tiles, same ring-buffer scheme,
 *         alpha blending in RGB565), scaled to fullscreen by the Xv overlay.
 *
 *   tilemap [-mode blit|xv|both] [-t seconds] [-n sprites] [-direct] [-nofilter]
 *           [-nofinish] [-profile] [-dump file.ppm]
 *
 *   -direct    blit mode: draw straight into the window, no back buffer
 *   -nofilter  xv mode: turn off the overlay's scaling filter
 *   -nofinish  only XSync per frame. XSync returns once the server
 *              has queued the GP commands, not when the GP is done, so the
 *              command buffer fills with seconds of work (and latency).
 *   -profile   blit mode: XSync after each phase and print where the time goes
 *   -dump      xv mode: save the last 640x480 frame (the overlay is not
 *              visible to XGetImage/screenshots)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xvlib.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>

#define MW 128 /* map size in tiles (wraps around) */
#define MH 128
#define NT 8   /* tile kinds */
#define NK 6   /* sprite kinds */
#define MAXSPR 512
#define LW 640 /* low-res size for the Xv mode */
#define LH 480
#define FOURCC_RV16 0x36315652

enum { DEEP, SHALLOW, SAND, GRASS, FLOWERS, FOREST, ROCK, PATH };

static Display *dpy;
static Window win;
static Visual *vis;
static GC gc;
static int W, H;
static double duration = 10.0;
static int nspr = 48, direct, filter = 1;
static const char *dump_path;
static uint8_t map[MH][MW];

typedef struct {
    double x, y, dx, dy, phase; /* world position and velocity in tile units */
    int kind;
} Sprite;
static Sprite spr[MAXSPR];

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int profile, nofinish;

/* Wait until the GP has executed everything: XSync alone only waits until the
 * server has queued the commands. Reading a pixel back makes EXA wait for idle. */
static void finish(void)
{
    if (nofinish) {
        XSync(dpy, False);
        return;
    }
    XImage *img = XGetImage(dpy, win, 0, 0, 1, 1, AllPlanes, ZPixmap);
    if (img)
        XDestroyImage(img);
}
static double prof[4]; /* tiles, ring->back, sprites, back->window */
static int prof_frames;

static void prof_mark(int phase, double *t)
{
    if (!profile)
        return;
    finish();
    double n = now();
    prof[phase] += n - *t;
    *t = n;
}

static inline int imod(long a, int m)
{
    long r = a % m;
    return (int)(r < 0 ? r + m : r);
}

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    x ^= x >> 16;
    return x;
}

static double hash01(int x, int y, int seed)
{
    return (hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)seed * 83492791u) &
            0xffffff) / (double)0x1000000;
}

/* ---- world ---------------------------------------------------------------- */

/* periodic value noise on a `period`-cell lattice spanning the map */
static double vnoise(double x, double y, int period, int seed)
{
    int x0 = (int)floor(x), y0 = (int)floor(y);
    double fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    double a = hash01(imod(x0, period), imod(y0, period), seed);
    double b = hash01(imod(x0 + 1, period), imod(y0, period), seed);
    double c = hash01(imod(x0, period), imod(y0 + 1, period), seed);
    double d = hash01(imod(x0 + 1, period), imod(y0 + 1, period), seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static void make_map(void)
{
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            double h = 0, amp = 0.5, total = 0;
            for (int o = 0, cells = 8; o < 4; o++, cells *= 2, amp /= 2) {
                h += amp * vnoise(x * cells / (double)MW, y * cells / (double)MH, cells, o + 1);
                total += amp;
            }
            h /= total;
            int k = h < 0.36 ? DEEP : h < 0.43 ? SHALLOW : h < 0.47 ? SAND
                  : h < 0.62 ? (hash01(x, y, 9) < 0.12 ? FLOWERS : GRASS)
                  : h < 0.72 ? FOREST : ROCK;
            map[y][x] = (uint8_t)k;
        }
    /* a few paths across the land */
    for (int i = 0; i < 4; i++) {
        int row = (int)(hash01(i, 0, 21) * MH), col = (int)(hash01(i, 1, 22) * MW);
        for (int j = 0; j < MW; j++) {
            if (map[row][j] >= SAND && map[row][j] != ROCK)
                map[row][j] = PATH;
            if (map[j][col] >= SAND && map[j][col] != ROCK)
                map[j][col] = PATH;
        }
    }
}

/* tile texel at (u, v) in [0,1); `px`,`py` index a 32x32 texel grid */
static void tile_rgb(int k, int px, int py, int *r, int *g, int *b)
{
    double n = hash01(px, py, k + 100), u = (px + 0.5) / 32, v = (py + 0.5) / 32;
    switch (k) {
    case DEEP:
    case SHALLOW: {
        double wave = 0.5 + 0.5 * sin((u * 2 + v) * 2 * M_PI * 2 + (k == DEEP ? 0 : 1));
        int base = k == DEEP ? 0 : 50;
        *r = base / 2 + (int)(wave * 20);
        *g = 60 + base + (int)(wave * 30);
        *b = 140 + base + (int)(wave * 40);
        break;
    }
    case SAND:
        *r = 220 + (int)(n * 25);
        *g = 196 + (int)(n * 25);
        *b = 130 + (int)(n * 20);
        break;
    case GRASS:
    case FLOWERS:
        *r = 60 + (int)(n * 30);
        *g = 140 + (int)(n * 50);
        *b = 50 + (int)(n * 20);
        if (k == FLOWERS) {
            /* four flowers per tile */
            for (int f = 0; f < 4; f++) {
                double fx = 0.2 + 0.6 * hash01(f, 0, 31), fy = 0.2 + 0.6 * hash01(f, 1, 32);
                if ((u - fx) * (u - fx) + (v - fy) * (v - fy) < 0.006) {
                    *r = f & 1 ? 250 : 240;
                    *g = f & 1 ? 230 : 80;
                    *b = f & 1 ? 60 : 120;
                }
            }
        }
        break;
    case FOREST: {
        *r = 50 + (int)(n * 20);
        *g = 120 + (int)(n * 30);
        *b = 40;
        double d = (u - 0.5) * (u - 0.5) + (v - 0.45) * (v - 0.45);
        if (d < 0.13) { /* tree crown, lit from the upper left */
            double l = 1.1 - 2.2 * ((u - 0.38) * (u - 0.38) + (v - 0.3) * (v - 0.3)) - n * 0.15;
            *r = (int)(20 + 40 * l);
            *g = (int)(70 + 90 * l);
            *b = (int)(20 + 30 * l);
        } else if (u > 0.44 && u < 0.56 && v > 0.75) { /* trunk */
            *r = 100;
            *g = 70;
            *b = 40;
        }
        break;
    }
    case ROCK: {
        double c = vnoise(u * 4, v * 4, 4, 41);
        *r = *g = *b = 110 + (int)(c * 70 + n * 20);
        break;
    }
    default: { /* cobblestone path */
        int cx = px / 8, cy = (py + (px / 8 % 2) * 4) / 8;
        int edge = (px % 8 == 0) || ((py + (px / 8 % 2) * 4) % 8 == 0);
        double s = hash01(cx, cy, 51);
        *r = edge ? 90 : 150 + (int)(s * 40);
        *g = edge ? 80 : 135 + (int)(s * 35);
        *b = edge ? 70 : 115 + (int)(s * 30);
    }
    }
    if (*r > 255) *r = 255;
    if (*g > 255) *g = 255;
    if (*b > 255) *b = 255;
    if (*r < 0) *r = 0;
    if (*g < 0) *g = 0;
    if (*b < 0) *b = 0;
}

/* RGB565 tiles of size t (32 or 16; 16 is a 2x2 box filter of 32) */
static void make_tiles(int t, uint16_t *out /* NT * t * t */)
{
    int f = 32 / t;
    for (int k = 0; k < NT; k++)
        for (int y = 0; y < t; y++)
            for (int x = 0; x < t; x++) {
                int sr = 0, sg = 0, sb = 0, r, g, b;
                for (int j = 0; j < f; j++)
                    for (int i = 0; i < f; i++) {
                        tile_rgb(k, x * f + i, y * f + j, &r, &g, &b);
                        sr += r;
                        sg += g;
                        sb += b;
                    }
                out[(k * t + y) * t + x] = rgb565(sr / (f * f), sg / (f * f), sb / (f * f));
            }
}

/* glowing orbs, premultiplied ARGB32, size s */
static void make_sprites(int s, uint32_t *out /* NK * s * s */)
{
    static const double hue[NK][3] = {{1, .35, .3}, {.35, 1, .45}, {.4, .6, 1},
                                      {1, .9, .3},  {1, .4, 1},    {.4, 1, 1}};
    for (int k = 0; k < NK; k++)
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                double u = (x + 0.5) / s - 0.5, v = (y + 0.5) / s - 0.5;
                double d = sqrt(u * u + v * v);
                /* soft glow halo */
                double ga = d < 0.5 ? 0.8 * pow(1 - d / 0.5, 1.5) : 0;
                double r = hue[k][0] * ga, g = hue[k][1] * ga, b = hue[k][2] * ga, a = ga;
                /* shaded core */
                double ca = fmin(fmax((0.2 - d) * s, 0), 1);
                if (ca > 0) {
                    double hx = u + 0.07, hy = v + 0.07;
                    double l = fmin(1.0, 0.35 + 0.65 * (1 - sqrt(hx * hx + hy * hy) / 0.3));
                    double spec = pow(fmax(0, 1 - sqrt(hx * hx + hy * hy) / 0.08), 2);
                    double cr = fmin(1, hue[k][0] * l + spec), cg = fmin(1, hue[k][1] * l + spec),
                           cb = fmin(1, hue[k][2] * l + spec);
                    r = cr * ca + r * (1 - ca);
                    g = cg * ca + g * (1 - ca);
                    b = cb * ca + b * (1 - ca);
                    a = ca + a * (1 - ca);
                }
                out[(k * s + y) * s + x] = (uint32_t)(a * 255 + .5) << 24 |
                                           (uint32_t)(r * 255 + .5) << 16 |
                                           (uint32_t)(g * 255 + .5) << 8 | (uint32_t)(b * 255 + .5);
            }
}

static void init_sprites(void)
{
    for (int i = 0; i < nspr; i++) {
        spr[i].x = hash01(i, 0, 61) * MW;
        spr[i].y = hash01(i, 1, 62) * MH;
        double ang = hash01(i, 2, 63) * 2 * M_PI, sp = 1 + 3 * hash01(i, 3, 64);
        spr[i].dx = cos(ang) * sp;
        spr[i].dy = sin(ang) * sp;
        spr[i].phase = hash01(i, 4, 65) * 2 * M_PI;
        spr[i].kind = i % NK;
    }
}

/* sprite world position at time t, in tile units */
static void sprite_pos(int i, double t, double *x, double *y)
{
    *x = spr[i].x + spr[i].dx * t + 0.8 * sin(t * 2 + spr[i].phase);
    *y = spr[i].y + spr[i].dy * t + 0.8 * cos(t * 1.7 + spr[i].phase);
}

/* camera top-left at time t, in tile units */
static void camera(double t, double *x, double *y)
{
    *x = t * 3.0 + 6 * sin(t * 0.31);
    *y = t * 1.6 + 5 * sin(t * 0.23 + 1);
}

/* screen position of a world point relative to the camera, wrapped into
 * [-margin, size - margin): sprites live in a field that travels with the view */
static double wrap_rel(double p, double cam, double world, double margin)
{
    double d = p - cam;
    return d - world * floor((d + margin) / world);
}

/* ---- ring buffer bookkeeping (shared by both modes) ---------------------- */

typedef struct {
    int t, rtw, rth;  /* tile size, ring size in tiles */
    int *tag;         /* world tile stored in each slot, packed */
} Ring;

static void ring_init(Ring *r, int t, int w, int h)
{
    r->t = t;
    r->rtw = (w + t - 1) / t + 1;
    r->rth = (h + t - 1) / t + 1;
    r->tag = malloc(sizeof(int) * (size_t)(r->rtw * r->rth));
    for (int i = 0; i < r->rtw * r->rth; i++)
        r->tag[i] = INT_MIN;
}

/* calls draw(slot_x, slot_y, kind) for every visible tile not yet in the ring */
static int ring_update(Ring *r, long camx, long camy, int w, int h,
                       void (*draw)(int sx, int sy, int kind, void *ctx), void *ctx)
{
    int n = 0;
    long tx0 = (long)floor(camx / (double)r->t), ty0 = (long)floor(camy / (double)r->t);
    long tx1 = (long)floor((camx + w - 1) / (double)r->t), ty1 = (long)floor((camy + h - 1) / (double)r->t);
    for (long ty = ty0; ty <= ty1; ty++)
        for (long tx = tx0; tx <= tx1; tx++) {
            int sx = imod(tx, r->rtw), sy = imod(ty, r->rth);
            int tag = (int)(imod(ty, 1 << 15) << 15 | imod(tx, 1 << 15));
            if (r->tag[sy * r->rtw + sx] == tag)
                continue;
            r->tag[sy * r->rtw + sx] = tag;
            draw(sx, sy, map[imod(ty, MH)][imod(tx, MW)], ctx);
            n++;
        }
    return n;
}

/* the up to four rectangles that copy the wrapped ring region to the screen */
typedef struct {
    int sx, sy, w, h, dx, dy;
} Part;

static int ring_parts(const Ring *r, long camx, long camy, int w, int h, Part *p)
{
    int rw = r->rtw * r->t, rh = r->rth * r->t;
    int sx = imod(camx, rw), sy = imod(camy, rh);
    int w1 = rw - sx < w ? rw - sx : w, h1 = rh - sy < h ? rh - sy : h, n = 0;
    p[n++] = (Part){sx, sy, w1, h1, 0, 0};
    if (w1 < w)
        p[n++] = (Part){0, sy, w - w1, h1, w1, 0};
    if (h1 < h)
        p[n++] = (Part){sx, 0, w1, h - h1, 0, h1};
    if (w1 < w && h1 < h)
        p[n++] = (Part){0, 0, w - w1, h - h1, w1, h1};
    return n;
}

static void put_image(Drawable d, GC g, int depth, void *data, int w, int h)
{
    int bpp = depth == 16 ? 16 : 32;
    XImage *img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, data, (unsigned)w,
                               (unsigned)h, bpp, w * bpp / 8);
    XPutImage(dpy, d, g, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    img->data = NULL;
    XDestroyImage(img);
}

/* per-second progress line plus a summary */
typedef struct {
    double t0, tsec;
    int frames, fsec;
    long tiles, tsec_tiles, sprites;
} Stats;

static void stats_frame(Stats *s, const char *mode, int tiles, int sprites_drawn)
{
    double t = now();
    s->frames++;
    s->fsec++;
    s->tiles += tiles;
    s->tsec_tiles += tiles;
    s->sprites += sprites_drawn;
    if (t - s->tsec >= 1.0) {
        printf("  %s  t=%4.1fs  %5.1f fps  %5.1f new tiles/frame", mode, t - s->t0,
               s->fsec / (t - s->tsec), (double)s->tsec_tiles / s->fsec);
        if (profile && prof_frames)
            printf("  [ms/frame: tiles %.2f, ring->back %.2f, sprites %.2f, back->win %.2f]",
                   prof[0] * 1e3 / prof_frames, prof[1] * 1e3 / prof_frames,
                   prof[2] * 1e3 / prof_frames, prof[3] * 1e3 / prof_frames);
        printf("\n");
        memset(prof, 0, sizeof prof);
        prof_frames = 0;
        fflush(stdout);
        s->tsec = t;
        s->fsec = 0;
        s->tsec_tiles = 0;
    }
}

static void stats_done(Stats *s, const char *what)
{
    double t = now() - s->t0;
    printf("%s: %.1f fps average, %d frames in %.1f s, %.1f new tiles and %.1f sprites per frame\n\n",
           what, s->frames / t, s->frames, t, (double)s->tiles / s->frames,
           (double)s->sprites / s->frames);
    fflush(stdout);
}

/* ---- blit mode -------------------------------------------------------------- */

typedef struct {
    Pixmap atlas, ring;
    int t;
} BlitCtx;

static void blit_tile(int sx, int sy, int kind, void *vctx)
{
    BlitCtx *c = vctx;
    XCopyArea(dpy, c->atlas, c->ring, gc, kind * c->t, 0, (unsigned)c->t, (unsigned)c->t,
              sx * c->t, sy * c->t);
}

static void run_blit(void)
{
    const int T = 32, S = 48;
    Window root = DefaultRootWindow(dpy);
    Ring ring;
    ring_init(&ring, T, W, H);

    uint16_t *tiles = malloc(NT * T * T * 2);
    make_tiles(T, tiles);
    /* atlas laid out horizontally: kind k at x = k*T */
    uint16_t *row = malloc(NT * T * T * 2);
    for (int k = 0; k < NT; k++)
        for (int y = 0; y < T; y++)
            memcpy(row + y * NT * T + k * T, tiles + (k * T + y) * T, T * 2);
    BlitCtx ctx = {XCreatePixmap(dpy, root, NT * T, T, 16),
                   XCreatePixmap(dpy, root, (unsigned)(ring.rtw * T), (unsigned)(ring.rth * T), 16), T};
    put_image(ctx.atlas, gc, 16, row, NT * T, T);
    free(row);
    free(tiles);

    uint32_t *sp = malloc(NK * S * S * 4), *sprow = malloc(NK * S * S * 4);
    make_sprites(S, sp);
    for (int k = 0; k < NK; k++)
        for (int y = 0; y < S; y++)
            memcpy(sprow + y * NK * S + k * S, sp + (k * S + y) * S, S * 4);
    Pixmap satlas = XCreatePixmap(dpy, root, NK * S, S, 32);
    GC gc32 = XCreateGC(dpy, satlas, 0, NULL);
    put_image(satlas, gc32, 32, sprow, NK * S, S);
    XFreeGC(dpy, gc32);
    free(sp);
    free(sprow);

    XRenderPictFormat *f16 = XRenderFindVisualFormat(dpy, vis);
    Picture spict = XRenderCreatePicture(dpy, satlas,
                                         XRenderFindStandardFormat(dpy, PictStandardARGB32), 0, NULL);
    Pixmap back = direct ? win : XCreatePixmap(dpy, root, (unsigned)W, (unsigned)H, 16);
    Picture bpict = XRenderCreatePicture(dpy, back, f16, 0, NULL);

    double fieldw = W + 2 * S, fieldh = H + 2 * S;
    Stats st = {0};
    double cx, cy;
    camera(0, &cx, &cy);
    ring_update(&ring, (long)floor(cx * T), (long)floor(cy * T), W, H, blit_tile, &ctx);
    finish();
    printf("blit mode: %dx%d native, %dx%d tiles, ring %dx%d px, %d alpha sprites %dx%d%s\n",
           W, H, T, T, ring.rtw * T, ring.rth * T, nspr, S, S,
           direct ? ", drawing directly into the window" : ", back buffer pixmap");
    st.t0 = st.tsec = now();
    double t;
    while ((t = now() - st.t0) < duration) {
        camera(t, &cx, &cy);
        long camx = (long)floor(cx * T), camy = (long)floor(cy * T);
        double tp = now();
        int nt = ring_update(&ring, camx, camy, W, H, blit_tile, &ctx);
        prof_mark(0, &tp);
        Part p[4];
        int np = ring_parts(&ring, camx, camy, W, H, p);
        for (int i = 0; i < np; i++)
            XCopyArea(dpy, ctx.ring, back, gc, p[i].sx, p[i].sy, (unsigned)p[i].w,
                      (unsigned)p[i].h, p[i].dx, p[i].dy);
        prof_mark(1, &tp);
        int drawn = 0;
        for (int i = 0; i < nspr; i++) {
            double wx, wy;
            sprite_pos(i, t, &wx, &wy);
            int x = (int)floor(wrap_rel(wx * T, camx, fieldw, S));
            int y = (int)floor(wrap_rel(wy * T, camy, fieldh, S));
            if (x - S / 2 >= W || y - S / 2 >= H || x + S / 2 <= 0 || y + S / 2 <= 0)
                continue;
            XRenderComposite(dpy, PictOpOver, spict, None, bpict, spr[i].kind * S, 0, 0, 0,
                             x - S / 2, y - S / 2, S, S);
            drawn++;
        }
        prof_mark(2, &tp);
        if (!direct)
            XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
        finish();
        prof_mark(3, &tp);
        prof_frames++;
        stats_frame(&st, "blit", nt, drawn);
    }
    stats_done(&st, "blit");

    XRenderFreePicture(dpy, bpict);
    XRenderFreePicture(dpy, spict);
    if (!direct)
        XFreePixmap(dpy, back);
    XFreePixmap(dpy, satlas);
    XFreePixmap(dpy, ctx.atlas);
    XFreePixmap(dpy, ctx.ring);
    free(ring.tag);
}

/* ---- xv mode ---------------------------------------------------------------- */

typedef struct {
    uint16_t *ring, *tiles;
    int pitch, t;
} CpuCtx;

static void cpu_tile(int sx, int sy, int kind, void *vctx)
{
    CpuCtx *c = vctx;
    for (int y = 0; y < c->t; y++)
        memcpy(c->ring + (sy * c->t + y) * c->pitch + sx * c->t,
               c->tiles + (kind * c->t + y) * c->t, (size_t)c->t * 2);
}

/* premultiplied ARGB32 over RGB565 */
static void blend_sprite(uint16_t *fb, int pitch, int w, int h, const uint32_t *src, int s, int x0, int y0)
{
    for (int y = 0; y < s; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= h)
            continue;
        uint16_t *d = fb + yy * pitch;
        const uint32_t *srow = src + y * s;
        for (int x = 0; x < s; x++) {
            int xx = x0 + x;
            uint32_t p = srow[x];
            unsigned a = p >> 24;
            if (!a || xx < 0 || xx >= w)
                continue;
            unsigned sr = (p >> 16) & 255, sg = (p >> 8) & 255, sb = p & 255;
            if (a < 255) {
                unsigned q = d[xx], inv = 255 - a;
                unsigned dr = (q >> 11) << 3, dg = ((q >> 5) & 63) << 2, db = (q & 31) << 3;
                sr += (dr * inv + 128) >> 8;
                sg += (dg * inv + 128) >> 8;
                sb += (db * inv + 128) >> 8;
            }
            d[xx] = (uint16_t)((sr >> 3) << 11 | (sg >> 2) << 5 | (sb >> 3));
        }
    }
}

static XvPortID find_xv_port(void)
{
    unsigned v, r, rb, ev, er, nad = 0;
    XvAdaptorInfo *ai;
    if (XvQueryExtension(dpy, &v, &r, &rb, &ev, &er) != Success ||
        XvQueryAdaptors(dpy, DefaultRootWindow(dpy), &nad, &ai) != Success)
        return 0;
    XvPortID port = 0;
    for (unsigned a = 0; a < nad && !port; a++)
        if (ai[a].type & XvImageMask)
            for (XvPortID p = ai[a].base_id; p < ai[a].base_id + ai[a].num_ports && !port; p++)
                if (XvGrabPort(dpy, p, CurrentTime) == Success)
                    port = p;
    XvFreeAdaptorInfo(ai);
    return port;
}

static void run_xv(void)
{
    const int T = 16, S = 24;
    XvPortID port = find_xv_port();
    if (!port) {
        printf("xv mode: no usable XVideo port\n");
        return;
    }
    XShmSegmentInfo shm;
    XvImage *img = XvShmCreateImage(dpy, port, FOURCC_RV16, NULL, LW, LH, &shm);
    if (!img) {
        printf("xv mode: RV16 not supported\n");
        XvUngrabPort(dpy, port, CurrentTime);
        return;
    }
    shm.shmid = shmget(IPC_PRIVATE, (size_t)img->data_size, IPC_CREAT | 0600);
    shm.shmaddr = img->data = shmat(shm.shmid, NULL, 0);
    shm.readOnly = False;
    XShmAttach(dpy, &shm);
    XSync(dpy, False);
    shmctl(shm.shmid, IPC_RMID, NULL);

    Atom a_filter = XInternAtom(dpy, "XV_FILTER", True), a_key = XInternAtom(dpy, "XV_COLORKEY", True);
    if (a_filter != None)
        XvSetPortAttribute(dpy, port, a_filter, filter);
    int key = 0;
    if (a_key != None)
        XvGetPortAttribute(dpy, port, a_key, &key);
    XSetForeground(dpy, gc, (unsigned long)key);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);

    Ring ring;
    ring_init(&ring, T, LW, LH);
    CpuCtx ctx = {malloc((size_t)(ring.rtw * T * ring.rth * T) * 2), malloc(NT * T * T * 2),
                  ring.rtw * T, T};
    make_tiles(T, ctx.tiles);
    uint32_t *sp = malloc(NK * S * S * 4);
    make_sprites(S, sp);

    uint16_t *fb = (uint16_t *)(img->data + img->offsets[0]);
    int fpitch = img->pitches[0] / 2;
    double fieldw = LW + 2 * S, fieldh = LH + 2 * S;
    Stats st = {0};
    double cx, cy;
    camera(0, &cx, &cy);
    ring_update(&ring, (long)floor(cx * T), (long)floor(cy * T), LW, LH, cpu_tile, &ctx);
    printf("xv mode: %dx%d RV16 drawn by the CPU, %dx%d tiles, %d alpha sprites %dx%d, "
           "overlay scales to %dx%d, filter %s\n",
           LW, LH, T, T, nspr, S, S, W, H, filter ? "on" : "off");
    st.t0 = st.tsec = now();
    double t;
    while ((t = now() - st.t0) < duration) {
        camera(t, &cx, &cy);
        long camx = (long)floor(cx * T), camy = (long)floor(cy * T);
        int nt = ring_update(&ring, camx, camy, LW, LH, cpu_tile, &ctx);
        Part p[4];
        int np = ring_parts(&ring, camx, camy, LW, LH, p);
        for (int i = 0; i < np; i++)
            for (int y = 0; y < p[i].h; y++)
                memcpy(fb + (p[i].dy + y) * fpitch + p[i].dx,
                       ctx.ring + (p[i].sy + y) * ctx.pitch + p[i].sx, (size_t)p[i].w * 2);
        int drawn = 0;
        for (int i = 0; i < nspr; i++) {
            double wx, wy;
            sprite_pos(i, t, &wx, &wy);
            int x = (int)floor(wrap_rel(wx * T, camx, fieldw, S));
            int y = (int)floor(wrap_rel(wy * T, camy, fieldh, S));
            if (x - S / 2 >= LW || y - S / 2 >= LH || x + S / 2 <= 0 || y + S / 2 <= 0)
                continue;
            blend_sprite(fb, fpitch, LW, LH, sp + spr[i].kind * S * S, S, x - S / 2, y - S / 2);
            drawn++;
        }
        XvShmPutImage(dpy, port, win, gc, img, 0, 0, LW, LH, 0, 0, (unsigned)W, (unsigned)H, False);
        finish(); /* the shared image is rewritten next frame */
        stats_frame(&st, "xv  ", nt, drawn);
    }
    stats_done(&st, "xv");

    FILE *f = dump_path ? fopen(dump_path, "wb") : NULL;
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", LW, LH);
        for (int y = 0; y < LH; y++)
            for (int x = 0; x < LW; x++) {
                unsigned q = fb[y * fpitch + x];
                fputc((int)((q >> 11) << 3), f);
                fputc((int)(((q >> 5) & 63) << 2), f);
                fputc((int)((q & 31) << 3), f);
            }
        fclose(f);
    }

    XvStopVideo(dpy, port, win);
    XvUngrabPort(dpy, port, CurrentTime);
    XShmDetach(dpy, &shm);
    shmdt(shm.shmaddr);
    XFree(img);
    free(ctx.ring);
    free(ctx.tiles);
    free(sp);
    free(ring.tag);
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);
}

int main(int argc, char **argv)
{
    const char *mode = "both";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-mode") && i + 1 < argc)
            mode = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            nspr = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-direct"))
            direct = 1;
        else if (!strcmp(argv[i], "-nofilter"))
            filter = 0;
        else if (!strcmp(argv[i], "-nofinish"))
            nofinish = 1;
        else if (!strcmp(argv[i], "-profile"))
            profile = 1;
        else if (!strcmp(argv[i], "-dump") && i + 1 < argc)
            dump_path = argv[++i];
        else {
            fprintf(stderr, "usage: %s [-mode blit|xv|both] [-t seconds] [-n sprites] [-direct] [-nofilter] [-nofinish] [-profile] [-dump file.ppm]\n",
                    argv[0]);
            return 2;
        }
    }
    if (nspr < 0)
        nspr = 0;
    if (nspr > MAXSPR)
        nspr = MAXSPR;

    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }
    W = DisplayWidth(dpy, 0);
    H = DisplayHeight(dpy, 0);
    vis = DefaultVisual(dpy, 0);
    int ev, er;
    if (DefaultDepth(dpy, 0) != 16 || !XShmQueryExtension(dpy) || !XRenderQueryExtension(dpy, &ev, &er)) {
        fprintf(stderr, "needs a 16 bpp display with MIT-SHM and RENDER\n");
        return 1;
    }
    XSetWindowAttributes wa = {.override_redirect = True, .background_pixel = 0,
                               .event_mask = StructureNotifyMask};
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)W, (unsigned)H, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    gc = XCreateGC(dpy, win, 0, NULL);
    XMapRaised(dpy, win);
    for (XEvent e;;) {
        XNextEvent(dpy, &e);
        if (e.type == MapNotify)
            break;
    }
    make_map();
    init_sprites();

    if (!strcmp(mode, "blit") || !strcmp(mode, "both"))
        run_blit();
    if (!strcmp(mode, "xv") || !strcmp(mode, "both"))
        run_xv();
    XCloseDisplay(dpy);
    return 0;
}
