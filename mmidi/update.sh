#!/bin/sh
# update.sh - update the bundled mmidi to a released version
#
#   mmidi/update.sh VERSION [MMIDI]
#
# VERSION is a tag of the mmidi repository, e.g. v2.1.1 (a commit works as
# well). The files are taken from MMIDI, a local clone of mmidi, when it is
# given, and downloaded from GitHub (with curl or wget) when not. The
# Version line in mmidi/README.md is set to the new version. Rebuild and
# test afterwards; git diff shows what changed.
set -e

version=$1
src=$2
if [ -z "$version" ]; then
    echo "usage: $0 VERSION [path/to/mmidi]   e.g. $0 v2.1.1" >&2
    exit 2
fi
dir=$(cd "$(dirname "$0")" && pwd)
url=https://raw.githubusercontent.com/realgraybox/mmidi
files="v2/mmidi.c v2/mmidi.h v2/mmmodel.c v2/mmmodel.h LICENSE"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in $files; do
    out=$tmp/$(basename "$f")
    if [ -n "$src" ]; then
        git -C "$src" show "$version:$f" > "$out" 2> /dev/null ||
            { echo "$0: $src has no $f at $version; nothing changed" >&2; exit 1; }
    elif command -v curl > /dev/null; then
        curl -fsSL "$url/$version/$f" -o "$out" ||
            { echo "$0: cannot download $url/$version/$f; nothing changed" >&2; exit 1; }
    elif command -v wget > /dev/null; then
        wget -q -O "$out" "$url/$version/$f" ||
            { echo "$0: cannot download $url/$version/$f; nothing changed" >&2; exit 1; }
    else
        echo "$0: neither curl nor wget found; give a local clone of mmidi" >&2
        exit 1
    fi
done

# the API version, from MMIDI_VERSION 0xMMmmpp
api() {
    v=$(sed -n 's/^#define MMIDI_VERSION 0x\([0-9a-fA-F]\{6\}\).*/\1/p' "$1")
    [ -n "$v" ] || { echo "?"; return; }
    echo "$((0x$(echo "$v" | cut -c1-2))).$((0x$(echo "$v" | cut -c3-4))).$((0x$(echo "$v" | cut -c5-6)))"
}
new=$(api "$tmp/mmidi.h")
if [ "$new" = "?" ]; then
    echo "$0: $version has no MMIDI_VERSION in v2/mmidi.h; nothing changed" >&2
    exit 1
fi
old=$(api "$dir/mmidi.h" 2> /dev/null || echo "?")

cp "$tmp"/* "$dir"/
sed "s|^Version: .*|Version: $version (API $new)|" "$dir/README.md" > "$tmp/README.md"
cp "$tmp/README.md" "$dir/README.md"
echo "mmidi: API $old -> $new ($version). Rebuild and test."
if [ "${old%%.*}" != "${new%%.*}" ]; then
    echo "mmidi: the major version changed: an existing call changed meaning," \
         "see v2/README.txt section 3.3" >&2
fi
