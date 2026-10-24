# OLPC XO-1

Hardware, software and performance notes for the OLPC XO-1 running OLPC OS
13.2.10, plus a Nix-based cross-compilation setup and graphics demos. The data
was collected 2026-10-24 over SSH from a running unit (a C2 mass-production
board): `/proc`, `/sys`, the OpenFirmware device tree, `dmesg`, `rpm`, and
benchmarks. In the examples, `<xo-host>` stands for the XO's hostname or address.

## Summary

| | |
|---|---|
| Model | OLPC XO-1, model `CL1`. Mass-production boards are revision **C2**, built by Quanta |
| Identity | Serial number, UUID, MAC, locale and keyboard layout are in the OpenFirmware `mfg-data` (`/proc/device-tree/mfg-data/`: `SN`, `U#`, `WM`, `LA`, `KL`, …) |
| Firmware (OFW) | **Q2F20** on the surveyed unit (DMI: "OLPC Ver 1.00.01 06/11/2014") |
| EC firmware | Ver 1.2.1 (EC code 0x58) |
| OS | **OLPC OS 13.2.10 for XO-1 (build 22)**, based on Fedora 18 |
| Kernel | `3.3.8_xo1-20150317.1153.olpc.7ceb87d` PREEMPT, i586 |
| Desktop | Sugar 0.112 |
| Hostname | `xo-XX-XX-XX`, from the last three bytes of the Wi-Fi MAC |

## Hardware

### CPU
- **AMD Geode LX 700** ("Geode(TM) Integrated Processor by AMD PCS"),
  family 5 model 10 stepping 2, **431 MHz**, single core, 862 BogoMIPS
- 128 KB L2 cache (64 KB I + 64 KB D L1)
- Flags: `fpu de pse tsc msr cx8 sep pge cmov clflush mmx mmxext 3dnowext 3dnow`.
  **No SSE/SSE2 and no PAE.** That rules out nearly every current i686 distro
  and binary, since most now require SSE2.
- 32-bit physical and virtual addressing
- No cpufreq scaling is exposed. Power saving is done by suspend-to-RAM through the DCON.
- On-chip extras used by the kernel: **Geode AES engine** (`geode-aes`) and **Geode RNG**

### Memory
- 256 MB DDR. Linux sees **222 MB** (227328 kB) because OpenFirmware reserves
  the top 8 MB and the framebuffer takes 16 MB.
- 64 MB swap configured (practically unused)
- At idle with Sugar and a Terminal running, about 114 MB is in use and about 114 MB is
  free or in cache.

### Storage
- **1 GB raw NAND** (2 chips) on the Marvell CaFé controller (`mtd0 "cafe_nand"`,
  128 KB erase blocks), no FTL
- Root filesystem is **JFFS2** directly on the MTD: 1.0 GB. With OLPC OS 13.2.10 and the
  default activities, about **800 MB is used and about 225 MB free**.
  `/home`, `/versions` and `/security` are bind-mounts of the same fs.
- `/tmp`, `/var/tmp`, `/var/log` and others are tmpfs (stateless setup)
- **SD card slot** (`sdhci-pci` on the CaFé chip)

### Display
- 7.5" dual-mode TFT, **1200×900**, 201 dpi (152×114 mm)
- **DCON** (Display CONtroller, version 2) can hold the image while the CPU
  sleeps. Backlight `dcon-bl`, levels 0–15.
- Framebuffer `lxfb` ("Geode LX"), 16 MB video memory, X runs at **16 bpp**
- Per the XO-1 spec, the panel has a backlit colour mode and a sunlight-readable monochrome
  reflective mode

### Graphics acceleration

The Geode LX graphics unit is **2D only, with no 3D or GPU shaders**. It has:

- **Graphics Processor (GP): a 2D BLT engine.** It handles solid and pattern
  fills, screen-to-screen copies, the full set of 256 raster operations,
  transparent and mono expansion blits, and **alpha blending** (Porter-Duff
  compositing, which is what XRender's `Composite` uses). It also offers
  rotation in 90° steps. The X driver `xorg-x11-drv-geode` 2.11.15
  (`Driver "amd"`, `AccelMethod EXA`) uses it through **EXA**. The Xorg log shows
  it registering *Solid*, *Copy* and *Composite*, so XRender text and plain
  compositing are accelerated. **Scaled (transformed) composites are not**: the
  GP cannot scale, so the server falls back to software (see
  [Acceleration demos](#acceleration-demos)). Framebuffer compression is turned off
  (`NoCompression`). The driver gets 14 MB of video memory, of which about 10 MB
  is the EXA pixmap cache.
- **Video overlay with hardware colour conversion and scaling.** XVideo adaptor
  "AMD Geode LX" with 1 port, image input only (no `PutVideo`, no XvMC).
  `XvShmPutImage` works, so frames reach the server through shared memory.
  - Accepts packed YUV (UYVY, YUY2, YVYU, Y2YU), planar YUV (**I420, YV12**),
    Y800 greyscale, and RV16 (RGB565).
  - Does YUV→RGB conversion and **up/down scaling with an optional filter**
    (`XV_FILTER`) in hardware, keyed to a colour key (`XV_COLORKEY`).
  - The maximum source image is **1024×1024**.

  This means software-decoded video, for example through gstreamer's
  `xvimagesink`, can be scaled to fullscreen at no CPU cost.
- **No video decoding acceleration.** There is no MPEG/H.264 decoder. XvMC is
  compiled into the X server but not offered to clients. All decoding runs on the 431 MHz CPU, which
  in practice limits video to low-resolution MPEG-1/2, MPEG-4 ASP or Theora.
- **No OpenGL acceleration.** There is no DRM/DRI device (`/dev/dri` is absent)
  and no Mesa DRI driver. The GLX module loads, but GLX is not among the X
  server's extensions, and `glxinfo`/`glxgears` are not installed. Any GL would
  be pure software rendering (Mesa swrast).
- **Display controller (DC):** supports a hardware cursor, a video overlay
  plane and gamma. The **DCON** chip on the XO-1 sits after it and handles
  the panel-specific colour swizzle, anti-aliasing for mono mode and the
  self-refresh freeze.

In short: fills, blits and alpha compositing are hardware accelerated, and so
is scaled YUV video output. Anything else, such as decoding, 3D, and pixels the
CPU generates and pushes with `XPutImage`, depends on the CPU and memory bus.
See the [x11perf numbers](#graphics-x11-x11perf) under Performance.

#### Who can use the acceleration

**Only the X driver uses it.** The kernel exposes no acceleration interface:

- **No DRM/KMS.** `/sys/class/drm` and `/dev/dri` are absent, and the kernel
  has no `drivers/gpu` modules. As far as I know, mainline Linux has never
  had a DRM driver for the Geode.
- **The fbdev driver draws in software.** `lxfb` is built into the kernel and
  claims the GP register block (`lxfb-gp` in `/proc/iomem`). It only uses the
  chip for mode setting and panning, though. Console drawing uses the generic
  software routines (the `sysfillrect` and `syscopyarea` modules are loaded),
  and there is no ioctl for blit or fill commands. `/dev/fb0` therefore gives
  you a plain mapping of video memory, with CPU-speed pixel pushing only.
- **The X server drives the hardware directly from user space.** It runs as
  root and maps all PCI BARs of the graphics device through sysfs (with
  libpciaccess), plus a piece of `/dev/mem` (BIOS area at 0xf0000). Taken
  from `/proc/<Xorg pid>/maps`:

  | sysfs file (`/sys/bus/pci/devices/0000:00:01.1/`) | Physical | Block |
  |---|---|---|
  | `resource0` | `fd000000`, 16 MB | Video memory (framebuffer + offscreen) |
  | `resource1` | `fe000000`, 16 KB | **GP**: 2D blitter / ROP / alpha engine |
  | `resource2` | `fe004000`, 16 KB | **VG**: display controller (timing, cursor, compression) |
  | `resource3` | `fe008000`, 16 KB | **VP**: video processor (overlay, YUV→RGB, scaler) |
  | `resource4` | `fe00c000`, 16 KB | VIP (video input port) |

  `xorg-x11-drv-geode` then writes GP commands into those registers
  itself. The kernel does no arbitration.

**What this means:** any root program can drive the GP and the video overlay
the same way, by `mmap`ing `resource1`/`resource3` for registers and
`resource0` for video memory. The `lx_*.c` and `cim/` (Cimarron) sources in
xf86-video-geode are the reference, and so is the AMD Geode LX data book.
Do this only while X is not running or is switched away to another virtual
terminal, because two users of the GP would corrupt each other's state.
Within X, the supported routes are XRender (EXA) for 2D and XVideo for scaled
YUV output.

### Wireless
- **Marvell 88W8388** 802.11b/g on internal USB (`1286:2001`), driver
  `libertas` / `usb8xxx`, firmware `usb8388_olpc.bin`
- Interfaces: `eth0` (station) and **`msh0` (802.11s mesh)**
- No wired Ethernet

### Other peripherals

| Device | Details |
|---|---|
| Camera | OmniVision **OV7670** (VGA 640×480) on the Marvell CaFé `cafe_ccic` controller (V4L2 name `mcam`) |
| Audio | CS5536 companion chip AC'97 (`snd_cs5535audio`), AD1888 codec (per XO-1 spec; not probed), built-in speakers and mic |
| Touchpad | ALPS "HGPK" dual-mode (capacitive + resistive/pen), revision 0x50 |
| Keyboard | Sealed membrane, AT translated set 2, through `olpc-kbdshim` |
| Switches | Power button, lid switch, **e-book switch** (screen folded back) |
| USB | EHCI and OHCI on the CS5536, 3 external USB 2.0 ports (plus internal Wi-Fi) |
| Embedded controller | ENE KB3700 per XO-1 spec (battery, keyboard, power management) |

### PCI devices

| BDF | ID | Function | Driver |
|---|---|---|---|
| 00:01.0 | 1022:0028 | Geode LX host bridge | – |
| 00:01.1 | 1022:2081 | Geode LX video | `lxfb` |
| 00:01.2 | 1022:2082 | Geode LX AES/RNG | `geode-aes` |
| 00:0c.0 | 11ab:4100 | Marvell CaFé NAND | `cafe_nand` |
| 00:0c.1 | 11ab:4101 | Marvell CaFé SD | `sdhci-pci` |
| 00:0c.2 | 11ab:4102 | Marvell CaFé camera | `cafe1000-ccic` |
| 00:0f.0 | 1022:2090 | CS5536 ISA bridge (GPIO, SMBus, MFGPT) | `cs5535-mfd` |
| 00:0f.3 | 1022:2093 | CS5536 audio | `snd_cs5535audio` |
| 00:0f.4 | 1022:2094 | CS5536 OHCI | `ohci_hcd` |
| 00:0f.5 | 1022:2095 | CS5536 EHCI | `ehci_hcd` |

### Battery and power
- The surveyed unit has a **BYD LiFePO4** pack: design capacity 2800 mAh, 6.5 V max design
- `/sys/class/power_supply/olpc-battery` reports capacity, charge, voltage, current,
  temperature (battery and ambient), health, manufacturer and serial; `olpc-ac` reports the charger

## Software

- **OLPC OS 13.2.10** (build 22, XO-1), based on **Fedora 18**, 746 RPMs
- systemd 201, NetworkManager 0.9.8, Xorg 1.13.3, GTK 3.6 / GTK 2.24,
  WebKitGTK 1.10, GStreamer 0.10 and 1.0, gnash 0.8.10
- Languages: **Python 2.7.3** (no Python 3), Perl 5.16, Lua, bash 4.2
- Tools present: `wget`, `curl`, `yum`, `vi` (vim-minimal), `nano`, `sudo`
- **Not installed**: `gcc`/`make` (no headers or -devel packages at all; see [Cross-compiling](#cross-compiling)), `git`, `openssl` CLI, `screen`/`tmux`, `lspci`/`lsusb`, `bc`, `time`
- OpenSSH 6.1p1 / OpenSSL 1.0.1e. Modern clients warn that no post-quantum key
  exchange is offered.
- User `olpc` (uid 1000) is in `wheel`. `sudo` requires a TTY (`ssh -t`).
- **Check the clock.** No NTP client runs by default. If the RTC has lost its time,
  the system starts at 2013-01-01 (as on the surveyed unit), and TLS certificate
  checks fail until the clock is set.

### Sugar activities installed (`~/Activities`)
Browse, Calculate, Chat, Clock, Distance, GetBooks, Help, ImageViewer, Implode,
Jukebox, Log, Maze, Measure, Memorize, Moon, MusicKeyboard, Paint, Pippy, Read,
Record, Scratch, Speak, StopWatch, TamTamEdit, TamTamJam, TamTamMini,
TamTamSynthLab, Terminal, TurtleBlocks, Write

### Largest processes at idle (RSS)
Sugar shell (python2) 53 MB, Terminal 29 MB, X 14 MB, another python2 12 MB,
dhclient 10 MB, journald 8 MB, metacity 8 MB

## Cross-compiling

`flake.nix` provides a sysroot that matches the XO's installed software, plus a
clang-based cross toolchain. The XO itself has no compiler, headers or
`-devel` packages.

```sh
nix develop                       # xo-cc, xo-c++, xo-pkg-config, xo-ar, … and $CC/$CXX set
xo-cc -O2 -o hello hello.c
xo-cc -O2 -o game game.c $(xo-pkg-config --cflags --libs sdl)
scp hello olpc@<xo-host>:

cmake -DCMAKE_TOOLCHAIN_FILE=$XO_CMAKE_TOOLCHAIN …
meson setup --cross-file $XO_MESON_CROSS build
./configure --host=i686-redhat-linux   # i686-redhat-linux-gcc etc. are on $PATH
```

Flake outputs: `packages.x86_64-linux.{sysroot,toolchain,cmake-toolchain,meson-cross,demos}`
and `checks.x86_64-linux.{hello,demos}`. The `hello` check builds `tests/` (C, C++, SDL 1.2 and
GTK 3). All four were run on the XO and work.

### How the sysroot is built
- `sysroot/xo-installed.tsv` is the XO's `rpm -qa` (746 packages).
- `tools/gen-sysroot-lock.py` resolves `sysroot/lock.json` from the repo metadata of
  Fedora 18 (release and updates, archives.fedoraproject.org) and OLPC
  (`rpmdropbox.laptop.org/f18` and `/f18-xo1`). The result is **843 RPMs, 314 MiB to
  download, 883 MB unpacked**, made up of:
  - every installed package that ships shared libraries, at **exactly the version on the XO**
  - the `-devel`/`-headers` subpackages of the same source RPMs, at the same version,
    skipped when they need a library that isn't on the XO (Qt, OpenMPI, boost, …)
  - their devel→devel dependency closure
  - `gcc` 4.7.2 for `crt*.o`/`libgcc`, `libstdc++-devel` 4.7.2, and
    `kernel-headers` from OLPC's XO-1 kernel 3.3.8
- The flake fetches the RPMs by sha256 and unpacks them with bsdtar. It creates
  the merged-`/usr` symlinks and rewrites absolute symlinks to relative ones.
  Some repos are plain HTTP, but the fixed hashes protect integrity.
- To regenerate: `tools/update-sysroot-lock.sh olpc@<xo-host>`

### Toolchain details
- clang/lld 21 (nixpkgs 26.05), `--target=i686-redhat-linux --sysroot=… -march=geode`.
  `-march=geode` avoids SSE and long NOPs, which the Geode LX lacks.
  libgcc, libgcc_s and libstdc++ come from the sysroot, so binaries only need what is on the XO.
  The test binaries require at most GLIBC_2.1.3 and GLIBCXX_3.4.9.
- nixpkgs' unwrapped clang passes no `-dynamic-linker`. The wrapper adds
  `/lib/ld-linux.so.2` and the other link-only flags when linking.
- **C++:** the library is gcc 4.7's libstdc++, which has C++11 at most (no
  `std::make_unique`, `<optional>`, `<filesystem>`, …). Newer *language* features
  compile, because clang defaults to C++17. Sized and aligned deallocation are
  turned off (`-fno-sized-deallocation -fno-aligned-allocation`) because libstdc++ 4.7
  lacks those operators.
- Binaries are PIE by default, which glibc 2.16 handles.

## Performance

Measured on the idle machine (Sugar running, load about 0.4–1.6), on AC power.

### Boot
`systemd-analyze`: 2.7 s kernel + 10.4 s initrd + 27.5 s userspace = **40.7 s**
(the time from firmware to the Sugar UI is longer)

### CPU

| Test | Time |
|---|---|
| `python -c "for i in xrange(1000000): pass"` | 2.2 s |
| Python 1M-iteration `s += i` loop | 8.6 s |
| Python interpreter startup (`python -c pass`) | 0.38 s |
| Lua recursive `fib(25)` | 0.54 s |
| Perl `$s+=$_ for 1..1000000` | 3.1 s |
| bash `for ((i=0;i<100000;i++))` | 14.8 s |
| `md5sum` 32 MB | 2.4 s (about 13 MB/s) |
| `sha1sum` 32 MB | 4.2 s (about 7.6 MB/s) |
| `sha256sum` 32 MB | 6.1 s (about 5.3 MB/s) |
| `gzip -6` 32 MB of zeros | 5.1 s |
| `bzip2` 32 MB of zeros | 13.4 s |
| `/dev/urandom` read | 553 kB/s |

Overall that is roughly the speed of a late-1990s Pentium-class or K6-2 machine,
two orders of magnitude slower than a current desktop core.

### Memory
- `dd /dev/zero → /dev/null` (1 MB blocks): 1.1 GB/s (syscall/copy-bound)
- Write to tmpfs: **110 MB/s**

### Flash (JFFS2 on raw NAND)

| Test | Throughput |
|---|---|
| Sequential read, uncached (28 MB library) | **5.5 MB/s** |
| Sequential write, incompressible data + fsync | **1.2 MB/s** |
| Sequential write, zeros + fsync | 7.8 MB/s (JFFS2 compresses with zlib) |

### SD card

Test card: an old 2 GB SD card from 2005, FAT16, empty. The controller runs it in
**SD high-speed mode, 48 MHz, 4-bit bus** (about 24 MB/s bus maximum).

| Test | Throughput |
|---|---|
| Raw sequential read, 1 MB blocks, O_DIRECT (128 MB) | **7.6 MB/s** |
| Raw sequential read, 64 KB blocks, O_DIRECT (128 MB) | 7.9 MB/s |
| FAT16 file write, 1 MB blocks + fsync (64 MB) | **1.7 MB/s** |
| FAT16 file read, cold cache (64 MB) | 6.4 MB/s |
| FAT16 file write, 4 KB blocks O_DIRECT (4 MB) | 1.6 MB/s |
| 100 × 4 KB files + sync | 1.6 s |

The random 4 KB read latency could not be measured. 200 `dd` invocations took
about 1.9 s whether the offsets were random or fixed, so the roughly 9.5 ms
cost of starting each process hides the card's latency. Reads are faster than
the internal NAND (5.5 MB/s) and writes about equal to it. This old card,
rather than the XO's SD slot, is probably what limits the speed.

### Graphics (X11, x11perf)

Measured with 500×500 operations at 16 bpp. One 500×500 area is 0.5 MB.

| Operation | Rate | Throughput | Path |
|---|---|---|---|
| Solid rectangle fill | 1850/s | about 460 Mpixel/s | GPU (GP blitter) |
| Copy window→window / pixmap→window | 756–780/s | about 190 Mpixel/s | GPU blit |
| Scroll | 495/s | | GPU blit |
| Anti-aliased text (Charter 10) | about 59000 chars/s | | EXA Render |
| ShmPutImage (CPU → VRAM over shared memory) | 331/s | about 165 MB/s | CPU |
| PutImage (over the X socket) | 63/s | about 32 MB/s | CPU + protocol |
| GetImage (VRAM → CPU) | 32/s | about 16 MB/s | CPU, slow uncached VRAM read |

**SDL 1.2 (x11 driver), CPU-drawn full-screen 1200×900×16:** `tests/sdl-test.c`,
cross-compiled, reaches **13 fps**. Each frame computes every pixel and then
calls `SDL_Flip`. Calling `SDL_MapRGB` per pixel instead drops it to 2 fps.

A full 1200×900×16 bpp frame is 2.16 MB. Uploading it with MIT-SHM takes about
13 ms, so about 75 fps is the upload-only ceiling before the CPU spends any
time producing the pixels.

### Acceleration demos

`demos/x11-accel.c` and `demos/sdl-accel.c` (`nix build .#demos`) run every
path in turn for 3 s each, over a fullscreen override-redirect window. The scene
is a horizontally scrolling background with bouncing 32×32 balls.
- **Blitter tests:** background and sprites live in VRAM pixmaps. Opaque sprites
  use `XCopyArea`; alpha sprites use an ARGB32 atlas and `XRenderComposite` Over.
  The frame is composed in a back-buffer pixmap and then copied to the window.
- **CPU tests:** the scene is drawn into RAM and sent with `XShmPutImage` or `XvShmPutImage`.

Each frame ends with `finish()`, a 1×1 `XGetImage` that makes EXA wait until the
GP is idle. **`XSync` is not enough** (see [The XSync pitfall](#the-xsync-pitfall)).

`x11-accel` (Xlib + RENDER + Xv), measured on the XO:

| Test | fps | What |
|---|---|---|
| `fill` | 85.5 | 512 solid rects/frame, **530 Mpixel/s** (GP Solid) |
| `copy100` | 69.3 | 100 opaque 32×32 sprites + scrolling background + back→window copy (GP Copy) |
| `copy1000` | 15.2 | 1000 opaque sprites |
| `over100` | 72.9 | 100 alpha sprites, XRender Over (GP Composite) |
| `over1000` | 20.0 | 1000 alpha sprites |
| `rscale-nearest` | 17.0 | 640×480 GP scene (200 alpha sprites) → XRender transform to 1200×900 |
| `rscale-bilinear` | 0.9 | same, bilinear filter |
| `rscale-only` | 19.7 | XRender scale 640×480 → 1200×900 alone: **software fallback** |
| `xv-rv16` | **76.5** | 640×480 RGB565 drawn by CPU (200 sprites) → **Xv overlay scales** to 1200×900 |
| `xv-yv12` | 73.9 | same in YV12 |
| `xv-yv12-only` | 149.8 | Xv scaling with only the scrolling background |
| `shm-full` | 26.4 | 1200×900 drawn by CPU (200 sprites), `XShmPutImage` |
| `shm-640` | 73.7 | 640×480 drawn by CPU (200 sprites), unscaled |

What the numbers show:
- **Full-screen copies cost about 6 ms each** (about 180 Mpixel/s). A double-buffered,
  scrolled 1200×900 frame needs two of them (ring/background → back buffer → window),
  so about 12 ms. That alone caps the frame rate at about 80 fps.
- **Small blits cost 25–55 µs each:** about 18 000 opaque `XCopyArea` sprites/s and
  about 26 000 32×32 alpha composites/s (`copy1000` and `over1000`, after subtracting the
  background). Alpha composites are cheaper than `XCopyArea`. After the 12 ms of
  full-screen copies, a 60 fps frame has room for roughly 100–150 sprites.
  The GP pays off for large areas: fills, scrolling, full-frame copies,
  big alpha-blended layers.
- **Hardware scaling is only available through Xv.** XRender transforms fall back
  to software (about 20 fps for the scaling alone, under 1 fps bilinear). The Xv overlay
  scales 640×480 to 1200×900 for free. It also accepts RGB565 (`RV16`) besides YUV.
- **Render 640×480 on the CPU and let Xv scale it.** That reaches about 75 fps with
  200 sprites, against 26 fps for drawing 1200×900 directly. RV16 and YV12 cost about
  the same, so RGB565 drawing code can be kept.
  A GP-rendered pixmap cannot be fed to Xv: Xv takes images from client
  memory, and reading VRAM back runs at about 16 MB/s.
- **The overlay is a separate plane** shown where the window holds the colour key.
  The demo fills the window with the key from `XV_COLORKEY`.

### Tilemap demo

`demos/tilemap.c` is a smooth-scrolling 2D tilemap. The world is a procedural
128×128-tile torus with water, beach, grass, flowers, forest, rock and cobblestone
paths. Glowing orbs (premultiplied alpha) drift over it, and the camera moves
diagonally with a wobble. The same world is drawn two ways:

| Mode | How |
|---|---|
| `blit` | Native 1200×900, 32×32 tiles in VRAM. A **wrap-around ring buffer** pixmap (1248×960, one tile larger than the screen) holds the visible tiles, so each frame only blits the newly exposed tiles (about 1–3). The visible region is copied to a back buffer in up to 4 blits (where the ring wraps). 48×48 alpha orbs are drawn with XRender Over, then the back buffer is copied to the window. |
| `xv` | 640×480 in RAM, 16×16 tiles, the same ring-buffer scheme with `memcpy`. 24×24 orbs are alpha-blended by the CPU into RGB565, then `XvShmPutImage` RV16 is scaled to 1200×900 by the overlay. On screen the tiles are about 30 px and the orbs about 45 px, close to the blit mode. |

```sh
tilemap [-mode blit|xv|both] [-t seconds] [-n sprites] [-direct] [-nofilter] [-nofinish] [-profile] [-dump file.ppm]
```

Measured on the XO (6 s runs, about 44 orbs on screen with the default `-n 48`):

| Run | fps |
|---|---|
| `blit` | **74.5** |
| `blit -direct` (no back buffer: ring → window, orbs on the window; tears/flickers) | 114.0 |
| `blit -n 150` (about 137 orbs on screen) | 60.7 |
| `blit -n 0` | 86.7 |
| `xv` | **80.6** |
| `xv -n 150` | 51.1 |
| `xv -n 0` | 114.9 |
| `xv -nofilter` | 80.7 (the overlay filter is free) |

`blit -profile` (one sync per phase, so slightly slower in total) shows where the
frame goes: new tiles 0.5 ms, ring→back 6.1 ms, 43 orbs 2.5 ms, back→window 5.7 ms.
Each 48×48 alpha orb costs about 33 µs (from the `-n 150` difference).

- **Native blitting is a solid choice:** 60+ fps at full resolution with 130+ alpha
  sprites. The client's own CPU work is small, but the CPU is not free: the X
  server busy-waits on the GP. The two full-screen copies are the main cost. Drawing directly into the window
  saves one copy but tears.
- **Xv mode wins with few sprites and loses with many.** Here the CPU does all the
  drawing and blending, and alpha blending in RGB565 costs about 77 µs per 24×24
  orb (from the `-n 150` difference), more than twice the 33 µs of an alpha
  composite in blit mode. In exchange there is a free filtered upscale and a 640×480 working resolution,
  which suits pixel-art games.
- `-dump` writes the last Xv frame as a PPM, because the overlay is not visible to
  screenshots or `XGetImage`. The dumped frame was checked and looks right. The
  overlay's on-screen colours (RV16 channel order) were not checked, since I cannot see the XO's display.

### The XSync pitfall

The Geode X driver queues GP commands in a 256 KB command buffer, and `XSync` returns
once the **server** has processed the requests, not when the **GP** has executed
them. With few commands per frame (tilemap: about 50), the buffer holds seconds of
work. `tilemap -mode blit -nofinish` reports about 300 fps for the first seconds,
then falls to the real 80 fps once the buffer is full. At that point the X server
spins in user space waiting for buffer space: `vmstat` shows 93% user CPU, and
earlier ~1000 context switches/s drop to ~190.

Consequences:
- **Benchmarks** must wait for the GP. Reading back one pixel (`XGetImage` 1×1) does
  that, at a cost of a few percent.
- **Games** must throttle the same way, once per frame. Otherwise input latency
  can grow to seconds while the queue fills.
- The first version of `x11-accel` used `XSync` and overstated the per-sprite costs.
  The tables above were re-measured with `finish()`.

`sdl-accel` (SDL 1.2.15, x11 driver), measured on the XO:

```
hw_available=0 wm_available=1 blit_hw=0 blit_hw_CC=0 blit_hw_A=0 blit_fill=0 video_mem=0 KB
asked for SDL_HWSURFACE|SDL_DOUBLEBUF, got SDL_SWSURFACE
sw-blit       22.3 fps  1200x900 software surface, 200 RLE colour-keyed sprites
yuv-640       70.6 fps  640x480 YV12 -> 1200x900, hw_overlay=1, 200 CPU-drawn sprites
```

- **No hardware surfaces or blits in SDL.** SDL 1.2's x11 driver knows nothing
  about the GP: everything is a software surface flushed with MIT-SHM.
- **Hardware scaling works in SDL** through `SDL_CreateYUVOverlay`, which is backed
  by Xv (`hw_overlay=1`). `SDL_DisplayYUVOverlay` with a fullscreen rect gives
  the same 640×480→1200×900 scaling as raw Xv, at the same speed (70.6 vs 72.6 fps).
  SDL only offers YUV formats (no RV16), so the game has to draw in YUV or
  convert to it.
- With `SDL_VIDEO_YUV_HWACCEL=0`, SDL's software YUV path **segfaults** on the XO,
  even unscaled (`-noscale`). The crash is inside SDL. I could not debug it
  further because the XO has no gdb.
- SDL's `fbcon` driver has no Geode acceleration either. It was not tested,
  because it needs X to be stopped.

### Network
- With a weak Wi-Fi signal (−82 dBm), an SSH upload of 8 MB managed only about
  **69 kB/s**, and a download test over SSH did not finish within 5 minutes. SSH
  encryption on the 431 MHz CPU adds to this. For bulk transfers, use a USB stick or
  SD card, or a good Wi-Fi signal.

## Notes and caveats

- With 226 MB free on JFFS2 and about 1.2 MB/s writes, installing software is
  slow. JFFS2 also has to scan the whole flash at mount time.
- The Geode LX has no SSE, so modern 32-bit userlands such as Debian i386
  (≥ 13, which needs i686 + SSE2) will not run. Workable options are OLPC OS 13.2.x,
  older Fedora/Debian releases, or i586 builds of distros such as Alpine or Gentoo.
- A newer OLPC build is unlikely; 13.2.10 is the last XO-1 release.
- The camera probe in `dmesg` shows harmless SMBus timeouts while it probes for
  alternative sensors (siv120d/siv121c) before settling on the OV7670.
- The AC'97 codec reports "Failure reading codec reg 0x7e" at boot. This is common
  on the XO-1 and audio still registers.
