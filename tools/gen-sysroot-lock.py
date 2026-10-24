#!/usr/bin/env python3
"""Resolve the RPM set for an XO-1 cross-compilation sysroot.

Inputs:
  sysroot/xo-installed.tsv  `rpm -qa` dump from the XO (name, epoch:ver-rel, arch, srpm)
  <repo-dir>/<name>/*primary.xml.gz  yum metadata of the Fedora 18 / OLPC repos

The sysroot consists of:
  * every installed package that ships shared libraries (exact XO versions),
  * the -devel/-headers subpackages built from the same source RPMs,
    at exactly the same version as installed on the XO,
  * the dependency closure of those devel packages (devel -> devel, lib -> lib),
  * a few toolchain roots (gcc for crt*.o/libgcc, kernel-headers, ...).

Output: sysroot/lock.json, consumed by flake.nix.
"""

import argparse
import gzip
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

NS = {"c": "http://linux.duke.edu/metadata/common",
      "rpm": "http://linux.duke.edu/metadata/rpm"}

REPOS = [
    # (name, base URL) -- earlier entries win when the same NEVRA appears twice
    ("olpcxo1", "http://rpmdropbox.laptop.org/f18-xo1"),
    ("olpc", "http://rpmdropbox.laptop.org/f18"),
    ("updates", "https://archives.fedoraproject.org/pub/archive/fedora/linux/updates/18/i386"),
    ("fedora", "https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/18/Everything/i386/os"),
]

ARCHES = {"i686", "i586", "i386", "noarch"}

# Always included; their requirements are not followed (we only want files from them).
TOOLCHAIN_ROOTS = ["gcc", "glibc-devel", "glibc-headers", "kernel-headers",
                   "libstdc++-devel", "libgcc", "glibc", "libstdc++"]
NOFOLLOW = {"gcc"}

# Devel packages that are huge or useless for cross-compiling.
EXCLUDE = {"kernel-devel", "kernel-headers-olpc", "glibc-static", "libstdc++-static"}

SONAME_RE = re.compile(r"^lib.*\.so(\.\d+)*(\(.*\))?$|^ld-linux\.so")


def is_devel(name):
    return (name.endswith("-devel") or name.endswith("-headers")
            or name in ("xorg-x11-proto-devel", "kernel-headers"))


def srpm_name(srpm):
    # foo-bar-1.2-3.fc18.src.rpm -> foo-bar
    return srpm.rsplit("-", 2)[0] if srpm else None


class Pkg:
    __slots__ = ("name", "arch", "evr", "url", "sha256", "srpm", "provides",
                 "requires", "files", "repo", "size")

    def nevra(self):
        return f"{self.name}-{self.evr}.{self.arch}"


def load_repo(name, base, path):
    pkgs = []
    with gzip.open(path) as f:
        for _, el in ET.iterparse(f):
            if el.tag != "{%s}package" % NS["c"]:
                continue
            arch = el.findtext("c:arch", namespaces=NS)
            if arch not in ARCHES:
                el.clear()
                continue
            p = Pkg()
            p.repo = name
            p.name = el.findtext("c:name", namespaces=NS)
            p.arch = arch
            v = el.find("c:version", NS)
            p.evr = f"{v.get('epoch') or '0'}:{v.get('ver')}-{v.get('rel')}"
            p.url = base + "/" + el.find("c:location", NS).get("href")
            p.sha256 = el.findtext("c:checksum", namespaces=NS)
            p.size = int(el.find("c:size", NS).get("package"))
            fmt = el.find("c:format", NS)
            p.srpm = srpm_name(fmt.findtext("rpm:sourcerpm", namespaces=NS))
            p.provides = [e.get("name") for e in fmt.findall("rpm:provides/rpm:entry", NS)]
            p.requires = [e.get("name") for e in fmt.findall("rpm:requires/rpm:entry", NS)]
            p.files = [f.text for f in fmt.findall("c:file", NS)]
            pkgs.append(p)
            el.clear()
    return pkgs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repos", required=True, help="dir with <repo>/*primary.xml.gz")
    ap.add_argument("--installed", default="sysroot/xo-installed.tsv")
    ap.add_argument("--out", default="sysroot/lock.json")
    args = ap.parse_args()

    import glob
    allpkgs = []
    for name, base in REPOS:
        paths = glob.glob(f"{args.repos}/{name}/*primary.xml.gz")
        if not paths:
            sys.exit(f"missing metadata for repo {name}")
        allpkgs += load_repo(name, base, paths[0])

    by_nevra = {}
    by_name = defaultdict(list)
    providers = defaultdict(list)
    for p in allpkgs:
        if p.nevra() in by_nevra:          # first repo wins
            continue
        by_nevra[p.nevra()] = p
        by_name[p.name].append(p)
        for prov in p.provides + p.files:
            providers[prov].append(p)

    installed = {}          # name -> (evr, arch)
    installed_srpm_evr = {}  # srpm name -> evr of installed binary packages
    for line in open(args.installed):
        name, evr, arch, srpm = line.rstrip("\n").split("\t")
        installed[name] = (evr, arch)
        installed_srpm_evr[srpm_name(srpm)] = evr

    warnings = []

    def pick(name, want_evr=None):
        """Choose the package `name`, matching the XO version where possible."""
        cands = [p for p in by_name.get(name, ()) if p.arch != "src"]
        if not cands:
            return None
        if name in installed:
            evr, arch = installed[name]
            exact = [p for p in cands if p.evr == evr and p.arch == arch]
            if exact:
                return exact[0]
            warnings.append(f"{name}: installed {evr} not in any repo")
        srpm = cands[0].srpm
        want = want_evr or installed_srpm_evr.get(srpm)
        if want:
            exact = [p for p in cands if p.evr == want]
            if exact:
                return exact[0]
            # subpackages with their own version (perl-*, device-mapper from lvm2):
            # same source package, same release
            rel = want.rsplit("-", 1)[1]
            same = [p for p in cands if p.srpm == srpm and p.evr.rsplit("-", 1)[1] == rel]
            if same:
                return same[0]
            warnings.append(f"{name}: no build matching XO version {want}, using newest")
        # newest: prefer updates over release; i686 over i586
        order = {"olpcxo1": 0, "olpc": 1, "updates": 2, "fedora": 3}
        return sorted(cands, key=lambda p: (order[p.repo], p.arch != "i686"))[0]

    selected = {}
    skipped = {}

    def runtime_ok(p):
        """Return a requirement of devel package p whose library is missing on the XO."""
        for req in p.requires:
            if req.startswith("rpmlib(") or req in p.provides:
                continue
            provs = providers.get(req, [])
            if not provs or any(q.name in installed or q.name in selected for q in provs):
                continue
            # blocking: a soname, or a runtime subpackage of the same source package
            if SONAME_RE.match(req) or all(
                    q.srpm == p.srpm and not is_devel(q.name)
                    and any(SONAME_RE.match(x) for x in q.provides) for q in provs):
                return req
        return None

    def add(p, reason):
        if p is None or p.name in selected or p.name in EXCLUDE:
            return False
        selected[p.name] = (p, reason)
        return True

    # 1. installed library packages
    for name, (evr, arch) in installed.items():
        p = pick(name)
        if p and any(SONAME_RE.match(x) for x in p.provides):
            add(p, "lib")

    # 2. devel siblings of every selected library srpm (same EVR)
    lib_srpms = {p.srpm: p.evr for p, _ in selected.values()}
    for srpm, evr in lib_srpms.items():
        for name, pkgs in by_name.items():
            if is_devel(name) and any(q.srpm == srpm for q in pkgs):
                d = pick(name, evr)
                missing = d and runtime_ok(d)
                if d and not missing:
                    add(d, "devel")
                elif d:
                    skipped[d.name] = f"needs {missing}, not on the XO"

    # 3. toolchain roots
    for name in TOOLCHAIN_ROOTS:
        add(pick(name), "toolchain")

    # 4. dependency closure: devel requires -> devel/lib providers, lib requires -> sonames
    queue = list(selected)
    while queue:
        p, _ = selected[queue.pop()]
        if p.name in NOFOLLOW:
            continue
        for req in p.requires:
            if req.startswith("rpmlib(") or req in p.provides:
                continue
            lib_req = bool(SONAME_RE.match(req))
            if not is_devel(p.name) and not lib_req:
                continue
            provs = providers.get(req, [])
            if not provs:
                continue
            if any(q.name in selected for q in provs):
                continue
            # prefer an installed provider, then a devel one, then any library
            provs = sorted(provs, key=lambda q: (q.name not in installed, not is_devel(q.name)))
            q = pick(provs[0].name)
            if q is None:
                continue
            if is_devel(q.name):
                missing = runtime_ok(q)
                if missing:
                    skipped[q.name] = f"needs {missing}, not on the XO (wanted by {p.name})"
                    continue
            elif q.name not in installed:
                skipped[q.name] = f"not on the XO (wanted by {p.name})"
                continue
            if add(q, f"dep of {p.name}"):
                queue.append(q.name)

    out = []
    for name in sorted(selected):
        p, reason = selected[name]
        out.append({"name": p.name, "evr": p.evr, "arch": p.arch, "url": p.url,
                    "sha256": p.sha256, "size": p.size, "reason": reason,
                    "onDevice": p.name in installed or p.srpm in installed_srpm_evr})
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
        f.write("\n")

    total = sum(x["size"] for x in out)
    print(f"{len(out)} packages, {total / 2**20:.0f} MiB download", file=sys.stderr)
    for x in out:
        if not x["onDevice"]:
            print(f"  not on XO: {x['name']} {x['evr']} ({x['reason']})", file=sys.stderr)
    for name in sorted(skipped):
        if name not in selected:
            print(f"  skipped: {name}: {skipped[name]}", file=sys.stderr)
    for w in sorted(set(warnings)):
        print(f"  warning: {w}", file=sys.stderr)


if __name__ == "__main__":
    main()
