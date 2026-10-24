# OLPC XO-1 `xo-XX-XX-XX`

Inventory of the OLPC XO-1 reachable at `ssh olpc@<xo-host>`.
Collected 2026-10-24 over SSH from the running system: `/proc`, `/sys`, the
OpenFirmware device tree, `dmesg`, `rpm`, plus a few small benchmarks.

## Summary

| | |
|---|---|
| Model | OLPC XO-1, board revision **C2** (mass-production), model `CL1` |
| Serial | `XXXXXXXXXXX` (Quanta, Shanghai) |
| UUID | `XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX` |
| Manufactured | XXXX-XX-XX |
| Locale/keyboard | USA, `en_US.UTF-8`, US keyboard (`KL=us`, `KM=olpc`) |
| Firmware (OFW) | **Q2F20** (DMI: "OLPC Ver 1.00.01 06/11/2014") |
| EC firmware | Ver 1.2.1 (EC code 0x58) |
| OS | **OLPC OS 13.2.10 for XO-1 (build 22)**, based on Fedora 18 |
| Kernel | `3.3.8_xo1-20150317.1153.olpc.7ceb87d` PREEMPT, i586 |
| Desktop | Sugar 0.112 |
| Wi-Fi MAC | `XX:XX:XX:XX:XX:XX` (the hostname comes from the last three bytes) |

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
- Root filesystem is **JFFS2** directly on the MTD: 1.0 GB, **799 MB used, 226 MB free (79%)**.
  `/home`, `/versions` and `/security` are bind-mounts of the same fs.
- `/tmp`, `/var/tmp`, `/var/log` and others are tmpfs (stateless setup)
- **SD card slot** (`sdhci-pci` on the CaFé chip)

### Display
- 7.5" dual-mode TFT, **1200×900**, 201 dpi (152×114 mm)
- **DCON** (Display CONtroller, version 2) can hold the image while the CPU
  sleeps. Backlight `dcon-bl`, levels 0–15, currently 11.
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
  (`Driver "amd"`, `AccelMethod EXA`) uses it through **EXA**, so XRender text
  and compositing are accelerated. Framebuffer compression is turned off
  (`NoCompression`). The driver gets 14 MB of video memory, of which about 10 MB
  is the EXA pixmap cache.
- **Video overlay with hardware colour conversion and scaling.** XVideo adaptor
  "AMD Geode LX" with 1 port and `PutImage` only (no XvShm, no XvMC).
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
- Currently connected to SSID "XXXX", 2.457 GHz (ch 10), signal **−82 dBm**
  (link quality 28/70), bitrate 5.5–48 Mb/s. The signal is weak.
- IPv4 X.X.X.X/24 (DHCP), IPv6 SLAAC
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
- **BYD LiFePO4** pack, design capacity 2800 mAh, 6.5 V max design
- Health "Good". At the time of the survey it was **nearly flat: 5%**
  (140 mAh), 5.37 V, trickle-charging on AC.
- Battery temperature 23.9 °C, ambient 35 °C

## Software

- **OLPC OS 13.2.10** (build 22, XO-1), based on **Fedora 18**, 746 RPMs
- systemd 201, NetworkManager 0.9.8, Xorg 1.13.3, GTK 3.6 / GTK 2.24,
  WebKitGTK 1.10, GStreamer 0.10 and 1.0, gnash 0.8.10
- Languages: **Python 2.7.3** (no Python 3), Perl 5.16, Lua, bash 4.2
- Tools present: `wget`, `curl`, `yum`, `vi` (vim-minimal), `nano`, `sudo`
- **Not installed**: `gcc`/`make`, `git`, `openssl` CLI, `screen`/`tmux`, `lspci`/`lsusb`, `bc`, `time`
- OpenSSH 6.1p1 / OpenSSL 1.0.1e. Modern clients warn that no post-quantum key
  exchange is offered.
- User `olpc` (uid 1000) is in `wheel`. `sudo` requires a TTY (`ssh -t`).
- **Clock is not set.** The system thinks it is 2013-01-01, so the RTC has no
  valid time and no NTP is running. TLS certificate checks will fail until it is fixed.

### Sugar activities installed (`~/Activities`)
Browse, Calculate, Chat, Clock, Distance, GetBooks, Help, ImageViewer, Implode,
Jukebox, Log, Maze, Measure, Memorize, Moon, MusicKeyboard, Paint, Pippy, Read,
Record, Scratch, Speak, StopWatch, TamTamEdit, TamTamJam, TamTamMini,
TamTamSynthLab, Terminal, TurtleBlocks, Write

### Largest processes at idle (RSS)
Sugar shell (python2) 53 MB, Terminal 29 MB, X 14 MB, another python2 12 MB,
dhclient 10 MB, journald 8 MB, metacity 8 MB

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

Test card: 2 GB SD (1.85 GiB, 3893248 sectors), 
made in 2005. FAT16, empty. The controller runs it in **SD high-speed mode, 48 MHz,
4-bit bus** (about 24 MB/s bus maximum). On first insertion the kernel logged a
read error at the last sector (3893120), and the card was then re-detected
(probably while it was still being seated). No errors occurred during the tests.

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

A full 1200×900×16 bpp frame is 2.16 MB. Uploading it with MIT-SHM takes about
13 ms, so about 75 fps is the upload-only ceiling before the CPU spends any
time producing the pixels.

### Network
- Ping RTT from the LAN: 4.8 / 5.6 / 6.1 ms (min/avg/max)
- SSH upload (8 MB random data): **about 69 kB/s**. Two things limit it: the
  weak Wi-Fi signal (−82 dBm) and the CPU cost of SSH encryption. A download
  test over SSH did not finish within 5 minutes and was stopped. For bulk
  transfers, use a USB stick or SD card, or move the XO closer to the AP.

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
