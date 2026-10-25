#!/bin/sh
# usage: build.sh <gcc> <output.so> <newest glibc symbol version allowed, e.g. 2.17>
set -eu

CC="$1"
OUT="$2"
GLIBC_MAX="$3"
VENDOR=build/vendor

fetch() {
    mkdir -p "$VENDOR"
    curl -fsSL -o "$VENDOR/$1" "$2"
    echo "$3  $VENDOR/$1" | sha256sum -c -
}

STB=https://raw.githubusercontent.com/nothings/stb/003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5
LZ4=https://raw.githubusercontent.com/lz4/lz4/ebb370ca83af193212df4dcbadcc5d87bc0de2f0/lib
fetch stb_image.h "$STB/stb_image.h" 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
fetch stb_truetype.h "$STB/stb_truetype.h" ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab
fetch stb_image_write.h "$STB/stb_image_write.h" cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
fetch dlmalloc.c https://gee.cs.oswego.edu/pub/misc/malloc.c be7be56b40b4b31417e74e77687f9b96127ea6cae534cf236aee40a4c739508d
fetch lz4.c "$LZ4/lz4.c" 9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539
fetch lz4.h "$LZ4/lz4.h" 26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f

mkdir -p "$(dirname "$OUT")"
$CC -shared -fPIC -fvisibility=hidden -O2 -Wall -Wextra -I"$VENDOR" -Isrc \
    -o "$OUT" src/shim.c src/igm.c src/state.c -ldl -lpthread

# Anything newer than the oldest device's glibc and the preload is dropped
# with one line in the log. libm's symbols are newer still, so it must not
# be linked at all. Only the SDL wrappers and malloc's family are exported.
newest=$(readelf -V "$OUT" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n | tail -1)
echo "newest glibc symbol: $newest (allowed $GLIBC_MAX)"
[ "$(printf '%s\n%s\n' "$newest" "$GLIBC_MAX" | sort -t. -k1,1n -k2,2n | tail -1)" = "$GLIBC_MAX" ]
if readelf -d "$OUT" | grep -q 'libm\.so'; then echo "links libm"; exit 1; fi
for sym in SDL_RenderPresent SDL_PollEvent SDL_GetTicks SDL_GetModState SDL_OpenAudio SDL_OpenAudioDevice \
    malloc calloc realloc free; do
    readelf -W --dyn-syms "$OUT" | grep -Eq " FUNC +GLOBAL +DEFAULT +[0-9]+ $sym$" || { echo "missing $sym"; exit 1; }
done
if readelf -W --dyn-syms "$OUT" | grep -Eq 'stbi|stbtt|mspace|LZ4| (igm|state)_'; then
    echo "exports more than the wrappers"; exit 1
fi
ls -l "$OUT"
