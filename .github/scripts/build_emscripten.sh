#!/usr/bin/env bash
# Build adjacent, clean Fuse/libspectrum checkouts using the documented web flags.
set -euo pipefail
ROOT="$(cd "${1:?Usage: build_emscripten.sh SOURCE_ROOT}" && pwd)"
mkdir -p "$ROOT/build/wasm/"{libspectrum,fuse,prefix}
PREFIX="$ROOT/build/wasm/prefix"
JOBS="${JOBS:-4}"

(cd "$ROOT/libspectrum" && ./autogen.sh)
(cd "$ROOT/fuse" && ./autogen.sh)

cd "$ROOT/build/wasm/libspectrum"
emconfigure "$ROOT/libspectrum/configure" \
  --host=wasm32-unknown-emscripten --prefix="$PREFIX" \
  --disable-shared --enable-static \
  --with-zlib --without-bzip2 --without-libgcrypt \
  --with-wav-backend=pcm --with-fake-glib \
  CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'
emmake make -j"$JOBS"
emmake make install

cd "$ROOT/build/wasm/fuse"
export EM_PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
LIBSPECTRUM_CFLAGS="-I$PREFIX/include" \
LIBSPECTRUM_LIBS="$PREFIX/lib/libspectrum.a" \
SDL2_CFLAGS='-sUSE_SDL=2' SDL2_LIBS='-sUSE_SDL=2' \
emconfigure "$ROOT/fuse/configure" \
  --host=wasm32-unknown-emscripten --with-sdl --without-x \
  --without-pthread --disable-sockets --without-joystick \
  --with-zlib --without-png --without-libxml2 \
  --disable-desktop-integration CFLAGS='-O2' LDFLAGS='-sUSE_ZLIB'
emmake make -j"$JOBS"

for file in fuse.html fuse.js fuse.wasm fuse.data; do
  test -s "$file"
done
