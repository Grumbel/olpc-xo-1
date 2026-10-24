/* blit-bench: what the Geode LX blitter (via EXA) does, and what it costs.
 *
 *   blit-bench [-t seconds-per-test] [-win] [size|ops|rops|vram ...]
 *
 *   -win  draw into a fullscreen window instead of an offscreen pixmap
 *
 *   size  cost per operation vs. square size (fill, XCopyArea, Render Src/Over)
 *   ops   every XRender operator, with and without masks, 64x64 and 256x256
 *   rops  core X raster ops (GC functions), tiled and stippled fills
 *   vram  how many pixmaps stay fast before EXA runs out of offscreen memory
 *
 * Every test uses fresh pixmaps, so a software fallback in one test (which
 * migrates pixmaps out of video memory) does not slow down the next one, and
 * ends with finish(), which waits until the GP is idle (XSync does not).
 *
 * finish() reads a pixel back from a small mapped window. Reading from an
 * offscreen pixmap is not enough: EXA keeps a system-memory copy of pixmaps
 * and only waits for the GP if the pixels read were touched by queued
 * operations. The screen has no such copy, so a read from it always waits.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrender.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static Display *dpy;
static Window win, root;
static Visual *vis;
static GC gc;
static double tmin = 0.4;
static int to_window;

#define DW 1024 /* destination pixmap (or the part of the window used) */
#define DH 880
#define SRCW 512 /* source pixmaps */
#define SRCH 512

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void finish(Drawable d)
{
    (void)d;
    XImage *img = XGetImage(dpy, win, 0, 0, 1, 1, AllPlanes, ZPixmap);
    if (img)
        XDestroyImage(img);
}

static uint32_t rng = 12345;
static int rnd(int n)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (unsigned)n);
}

static void put_image(Drawable d, int depth, int bpp, void *data, int w, int h)
{
    GC g = XCreateGC(dpy, d, 0, NULL);
    XImage *img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, data, (unsigned)w,
                               (unsigned)h, bpp, w * bpp / 8);
    XPutImage(dpy, d, g, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    img->data = NULL;
    XDestroyImage(img);
    XFreeGC(dpy, g);
}

/* fresh pixmaps for one test */
typedef struct {
    Pixmap dst, src16, src32, src8;
    Picture pdst, p16, p32, p8, solid;
} Set;

static Set make_set(void)
{
    Set s;
    s.dst = to_window ? win : XCreatePixmap(dpy, root, DW, DH, 16);
    s.src16 = XCreatePixmap(dpy, root, SRCW, SRCH, 16);
    s.src32 = XCreatePixmap(dpy, root, SRCW, SRCH, 32);
    s.src8 = XCreatePixmap(dpy, root, SRCW, SRCH, 8);
    uint16_t *d16 = malloc(SRCW * SRCH * 2);
    uint32_t *d32 = malloc(SRCW * SRCH * 4);
    uint8_t *d8 = malloc(SRCW * SRCH);
    for (int y = 0; y < SRCH; y++)
        for (int x = 0; x < SRCW; x++) {
            int i = y * SRCW + x;
            unsigned a = (unsigned)((x * 7 + y * 3) & 255); /* varying alpha */
            unsigned r = (unsigned)(x & 255), g = (unsigned)(y & 255), b = 128;
            d16[i] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            d32[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
            d8[i] = (uint8_t)a;
        }
    put_image(s.src16, 16, 16, d16, SRCW, SRCH);
    put_image(s.src32, 32, 32, d32, SRCW, SRCH);
    put_image(s.src8, 8, 8, d8, SRCW, SRCH);
    free(d16);
    free(d32);
    free(d8);
    XSetForeground(dpy, gc, 0x4208);
    XFillRectangle(dpy, s.dst, gc, 0, 0, DW, DH);

    s.pdst = XRenderCreatePicture(dpy, s.dst, XRenderFindVisualFormat(dpy, vis), 0, NULL);
    s.p16 = XRenderCreatePicture(dpy, s.src16, XRenderFindVisualFormat(dpy, vis), 0, NULL);
    s.p32 = XRenderCreatePicture(dpy, s.src32, XRenderFindStandardFormat(dpy, PictStandardARGB32), 0, NULL);
    s.p8 = XRenderCreatePicture(dpy, s.src8, XRenderFindStandardFormat(dpy, PictStandardA8), 0, NULL);
    XRenderColor c = {0xc000, 0x4000, 0x2000, 0x8000};
    s.solid = XRenderCreateSolidFill(dpy, &c);
    /* Warm-up. A pixmap filled with XPutImage starts out in system memory, and
     * EXA does small operations from it in software (about 0.5-1 ms each, with
     * a GP sync) until enough large accelerated operations have moved it into
     * video memory. */
    for (int i = 0; i < 200; i++) {
        XCopyArea(dpy, s.src16, s.dst, gc, 0, 0, SRCW, SRCH, 0, 0);
        XRenderComposite(dpy, PictOpOver, s.p32, None, s.pdst, 0, 0, 0, 0, 0, 0, SRCW, SRCH);
        XRenderComposite(dpy, PictOpOver, s.solid, s.p8, s.pdst, 0, 0, 0, 0, 0, 0, SRCW, SRCH);
    }
    finish(s.dst);
    return s;
}

static void free_set(Set *s)
{
    XRenderFreePicture(dpy, s->pdst);
    XRenderFreePicture(dpy, s->p16);
    XRenderFreePicture(dpy, s->p32);
    XRenderFreePicture(dpy, s->p8);
    XRenderFreePicture(dpy, s->solid);
    if (!to_window)
        XFreePixmap(dpy, s->dst);
    XFreePixmap(dpy, s->src16);
    XFreePixmap(dpy, s->src32);
    XFreePixmap(dpy, s->src8);
    XSync(dpy, False);
}

typedef enum { K_FILL, K_COPY, K_SRC16, K_OVER32, K_RENDER, K_ROP, K_TILE, K_STIPPLE } Kind;

typedef struct {
    Kind kind;
    int op;          /* Render op or GC function */
    int src, mask;   /* Render: 16/32/8 bpp source picture, 0 = solid; mask 0/8/32 */
} Test;

/* runs one test at size sz; returns microseconds per operation */
static double run(const Test *t, int sz)
{
    Set s = make_set();
    Pixmap tile = 0, stip = 0;
    if (t->kind == K_TILE) {
        tile = XCreatePixmap(dpy, root, 16, 16, 16);
        XSetForeground(dpy, gc, 0xf800);
        XFillRectangle(dpy, tile, gc, 0, 0, 16, 16);
        XSetForeground(dpy, gc, 0x07e0);
        XFillRectangle(dpy, tile, gc, 0, 0, 8, 8);
        XSetTile(dpy, gc, tile);
        XSetFillStyle(dpy, gc, FillTiled);
    } else if (t->kind == K_STIPPLE) {
        static char bits[] = {0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa};
        stip = XCreateBitmapFromData(dpy, root, bits, 8, 8);
        XSetStipple(dpy, gc, stip);
        XSetFillStyle(dpy, gc, FillOpaqueStippled);
        XSetBackground(dpy, gc, 0x001f);
    }
    if (t->kind == K_ROP)
        XSetFunction(dpy, gc, t->op);
    XSetForeground(dpy, gc, 0x7bef);

    Picture src = t->src == 16 ? s.p16 : t->src == 32 ? s.p32 : t->src == 8 ? s.p8 : s.solid;
    Picture mask = t->mask == 8 ? s.p8 : t->mask == 32 ? s.p32 : None;
    long ops = 0;
    int batch = 1;
    double t0 = now(), el;
    do {
        for (int i = 0; i < batch; i++) {
            int x = rnd(DW - sz + 1), y = rnd(DH - sz + 1);
            int sx = rnd(SRCW - (sz < SRCW ? sz : SRCW) + 1), sy = rnd(SRCH - (sz < SRCH ? sz : SRCH) + 1);
            switch (t->kind) {
            case K_FILL:
            case K_TILE:
            case K_STIPPLE:
                /* A new colour per fill: Xlib merges consecutive same-GC fills into
                 * one request, and the server then fills overlapping parts once. */
                XSetForeground(dpy, gc, (unsigned long)(i * 2654435761u & 0xffff));
                XFillRectangle(dpy, s.dst, gc, x, y, (unsigned)sz, (unsigned)sz);
                break;
            case K_COPY:
            case K_ROP:
                XCopyArea(dpy, s.src16, s.dst, gc, sx, sy, (unsigned)sz, (unsigned)sz, x, y);
                break;
            case K_SRC16:
                XRenderComposite(dpy, PictOpSrc, s.p16, None, s.pdst, sx, sy, 0, 0, x, y,
                                 (unsigned)sz, (unsigned)sz);
                break;
            case K_OVER32:
                XRenderComposite(dpy, PictOpOver, s.p32, None, s.pdst, sx, sy, 0, 0, x, y,
                                 (unsigned)sz, (unsigned)sz);
                break;
            case K_RENDER:
                XRenderComposite(dpy, t->op, src, mask, s.pdst, sx, sy, sx, sy, x, y,
                                 (unsigned)sz, (unsigned)sz);
                break;
            }
        }
        finish(s.dst);
        ops += batch;
        el = now() - t0;
        if (batch < 4096)
            batch *= 2;
    } while (el < tmin);

    XSetFunction(dpy, gc, GXcopy);
    XSetFillStyle(dpy, gc, FillSolid);
    if (tile)
        XFreePixmap(dpy, tile);
    if (stip)
        XFreePixmap(dpy, stip);
    free_set(&s);
    return el * 1e6 / (double)ops;
}

static void test_size(void)
{
    static const int sizes[] = {4, 8, 16, 32, 64, 128, 256, 512};
    static const struct {
        const char *name;
        Kind kind;
    } k[] = {{"fill", K_FILL}, {"copy", K_COPY}, {"render-src", K_SRC16}, {"render-over", K_OVER32}};
    enum { NS = sizeof sizes / sizeof sizes[0] };
    printf("== cost per operation vs. size (square sprites, 16 bpp destination)\n");
    printf("%-12s", "size");
    for (int i = 0; i < NS; i++)
        printf(" %8d", sizes[i]);
    printf("   fixed     per-pixel\n");
    for (unsigned j = 0; j < sizeof k / sizeof k[0]; j++) {
        Test t = {k[j].kind, 0, 0, 0};
        double us[NS];
        printf("%-12s", k[j].name);
        for (int i = 0; i < NS; i++) {
            us[i] = run(&t, sizes[i]);
            printf(" %8.1f", us[i]);
            fflush(stdout);
        }
        /* least-squares fit us = a + b * pixels */
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 0; i < NS; i++) {
            double px = (double)sizes[i] * sizes[i];
            sx += px;
            sy += us[i];
            sxx += px * px;
            sxy += px * us[i];
        }
        double b = (NS * sxy - sx * sy) / (NS * sxx - sx * sx), a = (sy - b * sx) / NS;
        printf("   %5.1f us  %5.2f ns (%.0f Mpixel/s)\n", a, b * 1e3, 1.0 / b);
    }
    printf("(us per operation)\n\n");
}

static void test_ops(void)
{
    static const struct {
        const char *name;
        int op;
    } ops[] = {
        {"Clear", PictOpClear}, {"Src", PictOpSrc}, {"Dst", PictOpDst}, {"Over", PictOpOver},
        {"OverReverse", PictOpOverReverse}, {"In", PictOpIn}, {"InReverse", PictOpInReverse},
        {"Out", PictOpOut}, {"OutReverse", PictOpOutReverse}, {"Atop", PictOpAtop},
        {"AtopReverse", PictOpAtopReverse}, {"Xor", PictOpXor}, {"Add", PictOpAdd},
        {"Saturate", PictOpSaturate}, {"Multiply", PictOpMultiply}, {"Screen", PictOpScreen},
        {"Overlay", PictOpOverlay}, {"Darken", PictOpDarken}, {"Lighten", PictOpLighten},
        {"ColorDodge", PictOpColorDodge}, {"ColorBurn", PictOpColorBurn},
        {"HardLight", PictOpHardLight}, {"SoftLight", PictOpSoftLight},
        {"Difference", PictOpDifference}, {"Exclusion", PictOpExclusion},
        {"HSLHue", PictOpHSLHue}, {"DisjointOver", PictOpDisjointOver},
        {"ConjointOver", PictOpConjointOver},
    };
    printf("== XRender operators, ARGB32 source -> RGB565 destination (us per operation)\n");
    printf("%-14s %9s %9s %9s   %s\n", "op", "64x64", "256x256", "Mpix/s", "vs Over @256");
    Test over = {K_RENDER, PictOpOver, 32, 0};
    double ref = run(&over, 256);
    for (unsigned i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        Test t = {K_RENDER, ops[i].op, 32, 0};
        double a = run(&t, 64), b = run(&t, 256);
        printf("%-14s %9.1f %9.1f %9.0f   %5.1fx%s\n", ops[i].name, a, b, 65536 / b, b / ref,
               b / ref > 3 ? "  <- software fallback" : "");
        fflush(stdout);
    }
    printf("\n== sources and masks, op Over (us per operation)\n");
    static const struct {
        const char *name;
        int src, mask;
    } v[] = {
        {"RGB565 src", 16, 0},       {"ARGB32 src", 32, 0},          {"A8 src", 8, 0},
        {"solid src", 0, 0},         {"solid + A8 mask", 0, 8},      {"RGB565 + A8 mask", 16, 8},
        {"ARGB32 + A8 mask", 32, 8}, {"ARGB32 + ARGB32 mask", 32, 32},
    };
    printf("%-22s %9s %9s %9s   %s\n", "source / mask", "64x64", "256x256", "Mpix/s", "vs Over @256");
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        Test t = {K_RENDER, PictOpOver, v[i].src, v[i].mask};
        double a = run(&t, 64), b = run(&t, 256);
        printf("%-22s %9.1f %9.1f %9.0f   %5.1fx%s\n", v[i].name, a, b, 65536 / b, b / ref,
               b / ref > 3 ? "  <- software fallback" : "");
        fflush(stdout);
    }
    printf("\n");
}

static void test_rops(void)
{
    static const struct {
        const char *name;
        int f;
    } f[] = {
        {"GXcopy", GXcopy}, {"GXxor", GXxor}, {"GXand", GXand}, {"GXor", GXor},
        {"GXinvert", GXinvert}, {"GXequiv", GXequiv}, {"GXnand", GXnand}, {"GXclear", GXclear},
    };
    printf("== core X: XCopyArea with GC function, and fill styles (us per operation)\n");
    printf("%-22s %9s %9s %9s\n", "operation", "64x64", "256x256", "Mpix/s");
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) {
        Test t = {K_ROP, f[i].f, 0, 0};
        double a = run(&t, 64), b = run(&t, 256);
        printf("XCopyArea %-12s %9.1f %9.1f %9.0f\n", f[i].name, a, b, 65536 / b);
        fflush(stdout);
    }
    static const struct {
        const char *name;
        Kind k;
    } fs[] = {{"fill solid", K_FILL}, {"fill tiled 16x16", K_TILE}, {"fill opaque stipple", K_STIPPLE}};
    for (unsigned i = 0; i < sizeof fs / sizeof fs[0]; i++) {
        Test t = {fs[i].k, 0, 0, 0};
        double a = run(&t, 64), b = run(&t, 256);
        printf("%-22s %9.1f %9.1f %9.0f\n", fs[i].name, a, b, 65536 / b);
        fflush(stdout);
    }
    printf("\n");
}

/* Allocate n 512x512x16 pixmaps (512 KB each) and fill them round-robin. While
 * they fit in EXA's offscreen memory every fill is a GP op; past that EXA
 * keeps migrating pixmaps between system and video memory. */
static void test_vram(void)
{
    enum { MAXP = 40, PW = 512, PH = 512 };
    Pixmap p[MAXP];
    printf("== offscreen memory: n pixmaps of 512 KB, filled round-robin\n");
    printf("%6s %8s %12s\n", "n", "MB", "us per fill");
    for (int n = 2; n <= MAXP; n += 2) {
        for (int i = 0; i < n; i++)
            p[i] = XCreatePixmap(dpy, root, PW, PH, 16);
        long ops = 0;
        double t0 = now(), el;
        do {
            for (int i = 0; i < n; i++) {
                XSetForeground(dpy, gc, (unsigned long)rnd(65536));
                XFillRectangle(dpy, p[i], gc, 0, 0, PW, PH);
                finish(p[i]);
            }
            ops += n;
            el = now() - t0;
        } while (el < tmin);
        printf("%6d %8.1f %12.1f\n", n, n * 0.5, el * 1e6 / (double)ops);
        fflush(stdout);
        for (int i = 0; i < n; i++)
            XFreePixmap(dpy, p[i]);
        XSync(dpy, False);
    }
    printf("(finish() after every fill; a jump means EXA ran out of offscreen memory)\n\n");
}

static int wanted(int argc, char **argv, int first, const char *name)
{
    if (first >= argc)
        return 1;
    for (int i = first; i < argc; i++)
        if (!strcmp(argv[i], name))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (;;) {
        if (first + 1 < argc && !strcmp(argv[first], "-t")) {
            tmin = atof(argv[first + 1]);
            first += 2;
        } else if (first < argc && !strcmp(argv[first], "-win")) {
            to_window = 1;
            first++;
        } else
            break;
    }
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }
    int ev, er, maj = 0, min = 0;
    if (DefaultDepth(dpy, 0) != 16 || !XRenderQueryExtension(dpy, &ev, &er)) {
        fprintf(stderr, "needs a 16 bpp display with RENDER\n");
        return 1;
    }
    XRenderQueryVersion(dpy, &maj, &min);
    root = DefaultRootWindow(dpy);
    vis = DefaultVisual(dpy, 0);
    /* all drawing goes to pixmaps; the small window is only read back by finish() */
    XSetWindowAttributes wa = {.override_redirect = True, .event_mask = StructureNotifyMask};
    unsigned ww = to_window ? (unsigned)DisplayWidth(dpy, 0) : 8, wh = to_window ? (unsigned)DisplayHeight(dpy, 0) : 8;
    win = XCreateWindow(dpy, root, 0, 0, ww, wh, 0, CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWEventMask, &wa);
    XMapRaised(dpy, win);
    for (XEvent e;;) {
        XNextEvent(dpy, &e);
        if (e.type == MapNotify)
            break;
    }
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetGraphicsExposures(dpy, gc, False); /* no NoExpose event per XCopyArea */
    printf("RENDER %d.%d, %.1f s per measurement, destination: %s\n\n", maj, min, tmin,
           to_window ? "fullscreen window" : "offscreen pixmap");

    if (wanted(argc, argv, first, "size"))
        test_size();
    if (wanted(argc, argv, first, "ops"))
        test_ops();
    if (wanted(argc, argv, first, "rops"))
        test_rops();
    if (wanted(argc, argv, first, "vram"))
        test_vram();
    XCloseDisplay(dpy);
    return 0;
}
