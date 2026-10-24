# AGENTS.md

Notes for AI coding agents working in this repository.

## What this is

Documentation, a cross-compilation setup and graphics demos for the OLPC XO-1
(AMD Geode LX, 431 MHz, 256 MB RAM, 1200×900×16 display) running OLPC OS 13.2.10
(Fedora 18). `README.md` is the main document: hardware, software, how to
cross-compile, and benchmark results.

| Path | Contents |
|---|---|
| `README.md` | Hardware/software survey, cross-compiling guide, benchmark results |
| `flake.nix` | Sysroot, clang toolchain (`xo-cc`, `xo-c++`, `xo-pkg-config`, …), CMake/Meson cross files, demos, checks |
| `sysroot/xo-installed.tsv` | `rpm -qa` of an XO running OLPC OS 13.2.10 |
| `sysroot/lock.json` | Generated list of RPMs (URL + sha256) that make up the sysroot |
| `tools/gen-sysroot-lock.py` | Resolves `lock.json` from Fedora 18 / OLPC repo metadata |
| `tools/update-sysroot-lock.sh` | Regenerates `xo-installed.tsv` (from an XO) and `lock.json` |
| `tests/` | Toolchain smoke tests (C, C++, SDL 1.2, GTK 3) |
| `demos/` | `x11-accel`, `sdl-accel`, `tilemap`: acceleration benchmarks and demos |
| `XO-ORANGE.md` | **Private, git-ignored**: the specific unit's hostname, serial, network, state |

## Rules

- **Keep committed files generic for the XO-1 line.** Never commit hostnames, IP
  or MAC addresses, serial numbers, UUIDs, SSIDs, battery serials or other
  unit-specific or personal data. In docs and examples use `<xo-host>`. Put
  unit-specific findings in `XO-ORANGE.md`, which must never be committed
  (it is in `.gitignore`; do not force-add it).
- **Record new findings.** When you learn something new about the XO-1
  (measurements, behaviour, corrections), update `README.md` in the same change
  and commit it. If a finding contradicts something already written, fix the old
  text rather than adding a contradicting note.
- **Only report what you measured.** Mark facts taken from specs or memory as such
  (for example "per XO-1 spec"). If something could not be checked (for example the
  overlay's on-screen colours), say so.

## Building

```sh
nix build .#sysroot          # ~314 MiB of RPMs on first build
nix build .#demos            # result/bin/{x11-accel,sdl-accel,tilemap}
nix flake check              # builds tests/ and demos/
nix develop                  # shell with $CC=xo-cc, $CXX=xo-c++, $PKG_CONFIG, …
```

New flake-referenced files must be `git add`ed before Nix can see them.

Toolchain facts that matter when adding code:
- Target `i686-redhat-linux`, `-march=geode`. The CPU has **no SSE/SSE2**,
  MMX/3DNow! only.
- C++ uses gcc 4.7.2's libstdc++: at most C++11 *library* features. The wrapper
  passes `-fno-sized-deallocation -fno-aligned-allocation`.
- Only link against libraries in the sysroot. They are exactly the versions
  installed on the XO.
- If `xo-installed.tsv` changes, regenerate the lock with
  `tools/update-sysroot-lock.sh [olpc@<xo-host>]` and rebuild.

## Running on the XO

The device's address and access details are in `XO-ORANGE.md`; ask the user if
that file is missing.

- The XO has no compiler. Cross-compile here, `scp` binaries to `/tmp` on the XO
  (tmpfs), and run them there. Copy out of `result/` with `cp -L` and `chmod u+w`,
  because store files are read-only.
- GUI programs: `DISPLAY=:0`. Sugar runs on the display; demos use a fullscreen
  override-redirect window.
- `sudo` needs a TTY: `ssh -tt … 'sudo …'`.
- Newer OpenSSH clients print a post-quantum key exchange warning on every connection.
  Filter it out when parsing output.
- Missing on the XO: `gdb`, `strings`, `lspci`, `lsusb`, `/usr/bin/time`, `bc`, `glxinfo`.
  Available: `python` 2.7, `perl`, `lua`, `x11perf`, `xvinfo`, `xdpyinfo`, `stdbuf`.
- **Be gentle with the internal flash:** 1 GB raw NAND with JFFS2, about 225 MB free,
  about 1.2 MB/s writes, and ECC-corrected bit flips already occur. Keep benchmark
  files small, put temporary data in `/tmp`, and delete what you create.
- Wi-Fi throughput can be very low (tens of kB/s with a weak signal). Avoid large transfers.

## Benchmarking pitfalls (learned the hard way)

- **`XSync` does not wait for the GP (blitter).** The Geode X driver queues commands
  in a 256 KB buffer that can hold seconds of work, so frame rates look 3–10× too
  high until the buffer fills. End every measured frame with a 1×1 `XGetImage`
  (`finish()` in the demos).
- Run each measurement long enough (≥ 6 s) and check per-second numbers for drift.
  Results carried over from a previous test's backlog skew short runs.
- The Xv overlay is not visible to `XGetImage` or screenshots. Check CPU-rendered
  frames by dumping them (`tilemap -dump`). The overlay's on-screen output can only be
  checked by the user looking at the XO's screen.
- To check rendering visually without the device, build the demo natively on the
  host and run it under `Xvfb -screen 0 1200x900x16`, then screenshot it with
  `xwd | magick`. That covers the RENDER/blit paths; Xvfb has no Xv.
- SDL 1.2 has no hardware surfaces or blits on this machine. Only `SDL_CreateYUVOverlay`
  (Xv) is accelerated, and its software YUV fallback segfaults.

## Commits

- Commit on `master`. There is no remote.
- End commit messages with the `Co-Authored-By:` trailer the session specifies.
- Before committing, check that nothing unit-specific slipped in, e.g.
  `git diff --cached | grep -iE 'fritz|xo-[0-9a-f]{2}-|[0-9a-f]{2}:[0-9a-f]{2}:[0-9a-f]{2}:'`.
