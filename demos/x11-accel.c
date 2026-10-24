/* x11-accel: exercises every acceleration path the Geode LX X driver offers
 * (EXA Solid/Copy/Composite, Xv overlay scaling) next to the CPU paths, and
 * reports frame rates.
 *
 *   x11-accel [-t seconds] [test ...]
 *
 * Tests: fill copy100 copy1000 over100 over1000 rscale-nearest rscale-bilinear
 *        rscale-only xv-rv16 xv-yv12 xv-yv12-only shm-full shm-640
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xvlib.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "scene.h"

#define SW 640
#define SH 480
#define FOURCC_RV16 0x36315652
#define FOURCC_YV12 0x32315659

static Display *dpy;
static Window win;
static Visual *vis;
static GC gc;
static int W, H;
static double duration = 3.0;

static Pixmap bg_full, bg_small, atlas, atlas32, back, scene;
static Picture atlas_pict, back_pict, scene_pict, win_pict;
static uint16_t *bg565_small, *bg565_full;
static uint8_t *bgy, *bgu, *bgv;
static XvPortID xv_port;
static int have_render, have_xv, have_shm;

static void put_image(Drawable d, GC g, int depth, void *data, int w, int h)
{
    int bpp = depth == 16 ? 16 : 32;
    XImage *img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, data, (unsigned)w,
                               (unsigned)h, bpp, w * bpp / 8);
    XPutImage(dpy, d, g, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    img->data = NULL;
    XDestroyImage(img);
}

static void report(const char *name, int frames, double t, const char *extra)
{
    printf("%-18s %7.1f fps  %s\n", name, frames / t, extra ? extra : "");
    fflush(stdout);
}

static void clear_window(unsigned long pixel)
{
    XSetForeground(dpy, gc, pixel);
    XFillRectangle(dpy, win, gc, 0, 0, (unsigned)W, (unsigned)H);
    XSync(dpy, False);
}

/* ---- EXA Solid: XFillRectangles ---------------------------------------- */
static void test_fill(void)
{
    enum { NR = 64 };
    XRectangle r[NKINDS][NR];
    double area = 0;
    srand(2);
    for (int c = 0; c < NKINDS; c++)
        for (int i = 0; i < NR; i++) {
            r[c][i].width = (unsigned short)(8 + rand() % 200);
            r[c][i].height = (unsigned short)(8 + rand() % 200);
            r[c][i].x = (short)(rand() % (W - r[c][i].width));
            r[c][i].y = (short)(rand() % (H - r[c][i].height));
            area += r[c][i].width * r[c][i].height;
        }
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        for (int c = 0; c < NKINDS; c++) {
            XSetForeground(dpy, gc, (unsigned long)rgb565(rand() & 255, rand() & 255, rand() & 255));
            XFillRectangles(dpy, win, gc, r[c], NR);
        }
        XSync(dpy, False);
        frames++;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%d rects/frame, %.0f Mpixel/s", NKINDS * NR,
             frames * area / t / 1e6);
    report("fill", frames, t, buf);
}

/* scrolling background + sprites into the back buffer, by blitter */
static void blit_scene(Pixmap dst, Picture dstp, Pixmap bg, int w, int h, int off, int n, int alpha)
{
    XCopyArea(dpy, bg, dst, gc, off, 0, (unsigned)(w - off), (unsigned)h, 0, 0);
    if (off)
        XCopyArea(dpy, bg, dst, gc, 0, 0, (unsigned)off, (unsigned)h, w - off, 0);
    for (int i = 0; i < n; i++) {
        int x = (int)sprites[i].x, y = (int)sprites[i].y, k = sprites[i].kind;
        if (alpha)
            XRenderComposite(dpy, PictOpOver, atlas_pict, None, dstp, k * SPR, 0, 0, 0, x, y,
                             SPR, SPR);
        else
            XCopyArea(dpy, atlas, dst, gc, k * SPR, 0, SPR, SPR, x, y);
    }
}

/* ---- EXA Copy / Composite: sprites into a VRAM back buffer -------------- */
static void test_sprites(const char *name, int n, int alpha)
{
    init_sprites(n, W, H);
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(n, W, H);
        blit_scene(back, back_pict, bg_full, W, H, (frames * 2) % W, n, alpha);
        XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
        XSync(dpy, False);
        frames++;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%d %s sprites, %.0f sprites/s", n, alpha ? "alpha" : "opaque",
             frames * n / t);
    report(name, frames, t, buf);
}

/* ---- XRender transform: 640x480 scene scaled up to the screen ----------- */
static void test_render_scale(const char *name, const char *filter, int n)
{
    XTransform xf = {{{XDoubleToFixed((double)SW / W), 0, 0},
                      {0, XDoubleToFixed((double)SH / H), 0},
                      {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, scene_pict, &xf);
    XRenderSetPictureFilter(dpy, scene_pict, filter, NULL, 0);
    init_sprites(n, SW, SH);
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(n, SW, SH);
        blit_scene(scene, scene_pict, bg_small, SW, SH, (frames * 2) % SW, n, 1);
        XRenderComposite(dpy, PictOpSrc, scene_pict, None, win_pict, 0, 0, 0, 0, 0, 0,
                         (unsigned)W, (unsigned)H);
        XSync(dpy, False);
        frames++;
    }
    XTransform id = {{{XDoubleToFixed(1), 0, 0}, {0, XDoubleToFixed(1), 0}, {0, 0, XDoubleToFixed(1)}}};
    XRenderSetPictureTransform(dpy, scene_pict, &id);
    char buf[96];
    snprintf(buf, sizeof buf, "%dx%d -> %dx%d, %s, %d alpha sprites", SW, SH, W, H, filter, n);
    report(name, frames, t, buf);
}

/* ---- Xv overlay: CPU-rendered 640x480 frame, scaled by the video unit --- */
static void test_xv(const char *name, int fourcc, int n)
{
    XShmSegmentInfo shm;
    XvImage *img = XvShmCreateImage(dpy, xv_port, fourcc, NULL, SW, SH, &shm);
    if (!img) {
        printf("%-18s format not supported\n", name);
        return;
    }
    shm.shmid = shmget(IPC_PRIVATE, (size_t)img->data_size, IPC_CREAT | 0600);
    shm.shmaddr = img->data = shmat(shm.shmid, NULL, 0);
    shm.readOnly = False;
    XShmAttach(dpy, &shm);
    XSync(dpy, False);
    shmctl(shm.shmid, IPC_RMID, NULL);

    Atom ck = XInternAtom(dpy, "XV_COLORKEY", True);
    int key = 0;
    if (ck != None)
        XvGetPortAttribute(dpy, xv_port, ck, &key);
    clear_window((unsigned long)key);

    init_sprites(n, SW, SH);
    uint8_t *d = (uint8_t *)img->data;
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(n, SW, SH);
        int off = (frames * 2) % SW;
        if (fourcc == FOURCC_RV16) {
            uint16_t *fb = (uint16_t *)(d + img->offsets[0]);
            scroll_copy((uint8_t *)fb, img->pitches[0], (uint8_t *)bg565_small, SW * 2, SW * 2,
                        SH, off * 2);
            draw_sprites_565(fb, img->pitches[0] / 2, n);
        } else { /* YV12: Y, V, U */
            uint8_t *y = d + img->offsets[0], *v = d + img->offsets[1], *u = d + img->offsets[2];
            scroll_copy(y, img->pitches[0], bgy, SW, SW, SH, off);
            scroll_copy(u, img->pitches[2], bgu, SW / 2, SW / 2, SH / 2, off / 2);
            scroll_copy(v, img->pitches[1], bgv, SW / 2, SW / 2, SH / 2, off / 2);
            draw_sprites_yuv(y, img->pitches[0], u, v, img->pitches[2], n);
        }
        XvShmPutImage(dpy, xv_port, win, gc, img, 0, 0, SW, SH, 0, 0, (unsigned)W,
                      (unsigned)H, False);
        XSync(dpy, False);
        frames++;
    }
    XvStopVideo(dpy, xv_port, win);
    XShmDetach(dpy, &shm);
    shmdt(shm.shmaddr);
    XFree(img);
    clear_window(0);
    char buf[96];
    snprintf(buf, sizeof buf, "%dx%d -> %dx%d by overlay, %d CPU-drawn sprites", SW, SH, W, H, n);
    report(name, frames, t, buf);
}

/* ---- CPU baseline: render in RAM, upload with MIT-SHM ------------------- */
static void test_shm(const char *name, int w, int h, int n)
{
    XShmSegmentInfo shm;
    XImage *img = XShmCreateImage(dpy, vis, 16, ZPixmap, NULL, &shm, (unsigned)w, (unsigned)h);
    shm.shmid = shmget(IPC_PRIVATE, (size_t)(img->bytes_per_line * h), IPC_CREAT | 0600);
    shm.shmaddr = img->data = shmat(shm.shmid, NULL, 0);
    shm.readOnly = False;
    XShmAttach(dpy, &shm);
    XSync(dpy, False);
    shmctl(shm.shmid, IPC_RMID, NULL);
    clear_window(0);

    const uint16_t *bg = w == W ? bg565_full : bg565_small;
    init_sprites(n, w, h);
    int frames = 0;
    double t0 = now(), t;
    while ((t = now() - t0) < duration) {
        move_sprites(n, w, h);
        scroll_copy((uint8_t *)img->data, img->bytes_per_line, (const uint8_t *)bg, w * 2, w * 2,
                    h, ((frames * 2) % w) * 2);
        draw_sprites_565((uint16_t *)img->data, img->bytes_per_line / 2, n);
        XShmPutImage(dpy, win, gc, img, 0, 0, (W - w) / 2, (H - h) / 2, (unsigned)w,
                     (unsigned)h, False);
        XSync(dpy, False);
        frames++;
    }
    XShmDetach(dpy, &shm);
    shmdt(shm.shmaddr);
    img->data = NULL;
    XDestroyImage(img);
    char buf[96];
    snprintf(buf, sizeof buf, "%dx%d unscaled, CPU-drawn, %d sprites", w, h, n);
    report(name, frames, t, buf);
}

static void print_caps(void)
{
    int op, ev, er, maj = 0, min = 0;
    printf("display %s, %dx%d depth %d\n", DisplayString(dpy), W, H, DefaultDepth(dpy, 0));
    have_shm = XShmQueryExtension(dpy);
    printf("MIT-SHM: %s\n", have_shm ? "yes" : "no");
    if ((have_render = XRenderQueryExtension(dpy, &ev, &er)))
        XRenderQueryVersion(dpy, &maj, &min);
    printf("RENDER: %s %d.%d\n", have_render ? "yes" : "no", maj, min);

    unsigned v, r, rb, xev, xer, nad = 0;
    XvAdaptorInfo *ai = NULL;
    have_xv = XQueryExtension(dpy, "XVideo", &op, &ev, &er) &&
              XvQueryExtension(dpy, &v, &r, &rb, &xev, &xer) == Success &&
              XvQueryAdaptors(dpy, DefaultRootWindow(dpy), &nad, &ai) == Success && nad > 0;
    if (!have_xv) {
        printf("XVideo: no\n");
        return;
    }
    for (unsigned a = 0; a < nad; a++) {
        if (!(ai[a].type & XvImageMask))
            continue;
        xv_port = ai[a].base_id;
        int nf;
        XvImageFormatValues *f = XvListImageFormats(dpy, xv_port, &nf);
        printf("XVideo: adaptor \"%s\", port %lu, formats:", ai[a].name, xv_port);
        for (int i = 0; i < nf; i++)
            printf(" %.4s", (char *)&f[i].id);
        printf("\n");
        XFree(f);
        unsigned ne;
        XvEncodingInfo *enc;
        if (XvQueryEncodings(dpy, xv_port, &ne, &enc) == Success) {
            for (unsigned i = 0; i < ne; i++)
                if (!strcmp(enc[i].name, "XV_IMAGE"))
                    printf("XVideo: max image %lux%lu\n", enc[i].width, enc[i].height);
            XvFreeEncodingInfo(enc);
        }
        break;
    }
    XvFreeAdaptorInfo(ai);
    if (XvGrabPort(dpy, xv_port, CurrentTime) != Success) {
        printf("XVideo: cannot grab port\n");
        have_xv = 0;
    }
}

static void setup(void)
{
    make_sprites();

    /* background images */
    bg565_full = malloc((size_t)W * H * 2);
    bg565_small = malloc(SW * SH * 2);
    bgy = malloc(SW * SH);
    bgu = malloc(SW * SH / 4);
    bgv = malloc(SW * SH / 4);
    int r, g, b;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            bg_rgb(x, y, &r, &g, &b);
            bg565_full[y * W + x] = rgb565(r, g, b);
        }
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            uint8_t yy, u, v;
            bg_rgb(x * W / SW, y * H / SH, &r, &g, &b);
            bg565_small[y * SW + x] = rgb565(r, g, b);
            rgb_to_yuv(r, g, b, &yy, &u, &v);
            bgy[y * SW + x] = yy;
            if (!(x & 1) && !(y & 1)) {
                bgu[(y / 2) * (SW / 2) + x / 2] = u;
                bgv[(y / 2) * (SW / 2) + x / 2] = v;
            }
        }

    /* VRAM pixmaps */
    Window root = DefaultRootWindow(dpy);
    bg_full = XCreatePixmap(dpy, root, (unsigned)W, (unsigned)H, 16);
    bg_small = XCreatePixmap(dpy, root, SW, SH, 16);
    back = XCreatePixmap(dpy, root, (unsigned)W, (unsigned)H, 16);
    scene = XCreatePixmap(dpy, root, SW, SH, 16);
    atlas = XCreatePixmap(dpy, root, NKINDS * SPR, SPR, 16);
    atlas32 = XCreatePixmap(dpy, root, NKINDS * SPR, SPR, 32);
    put_image(bg_full, gc, 16, bg565_full, W, H);
    put_image(bg_small, gc, 16, bg565_small, SW, SH);

    uint16_t *a16 = malloc(NKINDS * SPR * SPR * 2);
    uint32_t *a32 = malloc(NKINDS * SPR * SPR * 4);
    for (int k = 0; k < NKINDS; k++)
        for (int y = 0; y < SPR; y++)
            for (int x = 0; x < SPR; x++) {
                int i = y * SPR + x, j = y * NKINDS * SPR + k * SPR + x;
                a16[j] = spr_mask[k][i] ? spr_565[k][i] : 0;
                a32[j] = spr_argb[k][i];
            }
    put_image(atlas, gc, 16, a16, NKINDS * SPR, SPR);
    GC gc32 = XCreateGC(dpy, atlas32, 0, NULL);
    put_image(atlas32, gc32, 32, a32, NKINDS * SPR, SPR);
    XFreeGC(dpy, gc32);
    free(a16);
    free(a32);

    if (have_render) {
        XRenderPictFormat *f16 = XRenderFindVisualFormat(dpy, vis);
        XRenderPictFormat *f32 = XRenderFindStandardFormat(dpy, PictStandardARGB32);
        atlas_pict = XRenderCreatePicture(dpy, atlas32, f32, 0, NULL);
        back_pict = XRenderCreatePicture(dpy, back, f16, 0, NULL);
        scene_pict = XRenderCreatePicture(dpy, scene, f16, 0, NULL);
        win_pict = XRenderCreatePicture(dpy, win, f16, 0, NULL);
    }
    XSync(dpy, False);
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
    if (argc > 2 && !strcmp(argv[1], "-t")) {
        duration = atof(argv[2]);
        first = 3;
    }
    if (!(dpy = XOpenDisplay(NULL))) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }
    W = DisplayWidth(dpy, 0);
    H = DisplayHeight(dpy, 0);
    vis = DefaultVisual(dpy, 0);
    if (DefaultDepth(dpy, 0) != 16) {
        fprintf(stderr, "needs a 16 bpp display\n");
        return 1;
    }

    XSetWindowAttributes wa = {.override_redirect = True, .background_pixel = 0,
                               .event_mask = StructureNotifyMask};
    win = XCreateWindow(dpy, DefaultRootWindow(dpy), 0, 0, (unsigned)W, (unsigned)H, 0,
                        CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &wa);
    gc = XCreateGC(dpy, win, 0, NULL);
    print_caps();
    XMapRaised(dpy, win);
    for (XEvent e;;) {
        XNextEvent(dpy, &e);
        if (e.type == MapNotify)
            break;
    }
    setup();
    printf("\n");

#define RUN(name, call)                    \
    if (wanted(argc, argv, first, name)) { \
        call;                              \
    }
    RUN("fill", test_fill());
    RUN("copy100", test_sprites("copy100", 100, 0));
    RUN("copy1000", test_sprites("copy1000", 1000, 0));
    if (have_render) {
        RUN("over100", test_sprites("over100", 100, 1));
        RUN("over1000", test_sprites("over1000", 1000, 1));
        RUN("rscale-nearest", test_render_scale("rscale-nearest", "nearest", 200));
        RUN("rscale-bilinear", test_render_scale("rscale-bilinear", "bilinear", 200));
        RUN("rscale-only", test_render_scale("rscale-only", "nearest", 0));
    }
    if (have_xv && have_shm) {
        RUN("xv-rv16", test_xv("xv-rv16", FOURCC_RV16, 200));
        RUN("xv-yv12", test_xv("xv-yv12", FOURCC_YV12, 200));
        RUN("xv-yv12-only", test_xv("xv-yv12-only", FOURCC_YV12, 0));
    }
    if (have_shm) {
        RUN("shm-full", test_shm("shm-full", W, H, 200));
        RUN("shm-640", test_shm("shm-640", SW, SH, 200));
    }
    if (have_xv)
        XvUngrabPort(dpy, xv_port, CurrentTime);
    XCloseDisplay(dpy);
    return 0;
}
