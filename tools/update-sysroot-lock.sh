#!/usr/bin/env bash
# Regenerate sysroot/xo-installed.tsv and sysroot/lock.json.
# Usage: tools/update-sysroot-lock.sh [user@xo-host]
# Without a host, the existing sysroot/xo-installed.tsv is reused.
set -euo pipefail

cd "$(dirname "$0")/.."
host=${1:-}
meta=$(mktemp -d)
trap 'rm -rf "$meta"' EXIT

if [ -n "$host" ]; then
    ssh "$host" "rpm -qa --qf '%{NAME}\t%{EPOCHNUM}:%{VERSION}-%{RELEASE}\t%{ARCH}\t%{SOURCERPM}\n'" \
        | sort > sysroot/xo-installed.tsv
fi

fetch() {
    local name=$1 base=$2 primary
    mkdir -p "$meta/$name"
    curl -sfL "$base/repodata/repomd.xml" -o "$meta/$name/repomd.xml"
    primary=$(grep -o 'href="[^"]*primary.xml[^"]*"' "$meta/$name/repomd.xml" | head -1 | cut -d'"' -f2)
    curl -sfL "$base/$primary" -o "$meta/$name/$(basename "$primary")"
}

fetch fedora https://archives.fedoraproject.org/pub/archive/fedora/linux/releases/18/Everything/i386/os
fetch updates https://archives.fedoraproject.org/pub/archive/fedora/linux/updates/18/i386
fetch olpc http://rpmdropbox.laptop.org/f18
fetch olpcxo1 http://rpmdropbox.laptop.org/f18-xo1

python3 -I tools/gen-sysroot-lock.py --repos "$meta" \
    --installed sysroot/xo-installed.tsv --out sysroot/lock.json
