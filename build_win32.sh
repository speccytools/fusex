#!/bin/bash
# FuseX — Windows distribution build (MSYS2 MinGW 64-bit).
# Prerequisite: MSYS2 with mingw-w64-x86_64 toolchain, perl, zip, 7zip (optional), NSIS (optional).

set -e

echo "=== Building FuseX Windows distribution ==="

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

if [[ -z "${MSYSTEM:-}" ]]; then
    echo -e "${RED}Error: run this from MSYS2 (e.g. \"MSYS2 MinGW 64-bit\"), not plain PowerShell.${NC}"
    exit 1
fi

if [[ "$MSYSTEM" != "MINGW64" && "$MSYSTEM" != "UCRT64" ]]; then
    echo -e "${YELLOW}Warning: MINGW64 or UCRT64 is recommended (current: $MSYSTEM).${NC}"
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

# Windows NSIS installs makensis.exe here but does not add it to MSYS PATH — match typical fuse dev setups.
for _nsis in "/c/Program Files/NSIS" "/c/Program Files (x86)/NSIS"; do
  if [[ -d "$_nsis" ]] && [[ -x "$_nsis/makensis.exe" || -x "$_nsis/makensis" ]]; then
    PATH="$_nsis:${PATH:-}"
    export PATH
    break
  fi
done

# Code generators assume Unix line endings on option/menu data (avoid CRLF breakage under Windows).
for f in ui/options.dat menu_data.dat settings.dat keysyms.dat \
         z80/opcodes_base.dat z80/opcodes_cb.dat z80/opcodes_ddfd.dat \
         z80/opcodes_ddfdcb.dat z80/opcodes_ed.dat; do
  if [[ -f "$f" ]]; then
    sed -i 's/\r$//' "$f" 2>/dev/null || true
  fi
done

echo -e "${GREEN}Directory: $SCRIPT_DIR${NC}"

echo -e "\n${GREEN}Checking tools...${NC}"
for tool in gcc make pkg-config perl; do
    if ! command -v "$tool" &> /dev/null; then
        echo -e "${RED}Missing: $tool${NC}"
        case "$tool" in
          perl) echo "  pacman -S mingw-w64-x86_64-perl" ;;
          *) echo "  pacman -S mingw-w64-x86_64-$tool" ;;
        esac
        exit 1
    fi
    echo "  OK $tool"
done

if [[ ! -d "$SCRIPT_DIR/3rdparty/dist/bin" ]] || \
   [[ ! -f "$SCRIPT_DIR/3rdparty/dist/lib/libmbedtls.a" ]] || \
   [[ ! -f "$SCRIPT_DIR/3rdparty/dist/lib/libssh2.a" ]]; then
    echo -e "\n${GREEN}Building 3rdparty (libspectrum, mbedTLS/libssh2 for Spectranet, optional deps)...${NC}"
    ( cd "$SCRIPT_DIR/3rdparty" && make -j"$(nproc 2>/dev/null || echo 4)" \
        MBEDTLS_MAKE_ARGS="WINDOWS=1 SHARED=1" )
else
    echo -e "\n${GREEN}Using existing 3rdparty/dist${NC}"
fi

DIST_PREFIX="$SCRIPT_DIR/3rdparty/dist"
export PKG_CONFIG_PATH="$DIST_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# Link the static archive explicitly (avoids picking /usr/local libspectrum.dll.a via libtool).
PC="$DIST_PREFIX/lib/pkgconfig/libspectrum.pc"
if [[ -f "$PC" ]]; then
  sed -i 's|^Libs:.*|Libs: ${libdir}/libspectrum.a|' "$PC" 2>/dev/null || true
fi

# Static libspectrum from 3rdparty: strip dllimport so MinGW links the .a correctly.
if [[ -f "$DIST_PREFIX/include/libspectrum.h" ]]; then
  sed -i 's/#define LIBSPECTRUM_API __declspec( dllimport )/#define LIBSPECTRUM_API/g' \
    "$DIST_PREFIX/include/libspectrum.h" 2>/dev/null || true
fi
# Prefer the static archive: libspectrum.dll.a can make -lspectrum import from a DLL.
if [[ -f "$DIST_PREFIX/lib/libspectrum.a" && -f "$DIST_PREFIX/lib/libspectrum.dll.a" ]]; then
  rm -f "$DIST_PREFIX/lib/libspectrum.dll.a"
  echo "  Removed libspectrum.dll.a (use static libspectrum.a)"
fi

echo -e "\n${GREEN}Generating configure if needed...${NC}"
if [[ ! -f configure ]]; then
    ./autogen.sh
else
    echo "  configure present"
fi

echo -e "\n${GREEN}Configuring FuseX...${NC}"

# Sockets enabled (default): Spectranet, gdbserver remote, etc. require Winsock on Windows.
CONFIGURE_OPTS=( --with-win32 --without-zlib --without-png --prefix=/usr/local )
if pkg-config --exists libxml-2.0 2>/dev/null; then
    echo "  with libxml2 (pkg-config)"
else
    echo "  without libxml2"
    CONFIGURE_OPTS+=( --without-libxml2 )
fi

./configure "${CONFIGURE_OPTS[@]}"

echo -e "\n${GREEN}Building and creating Windows distribution (zip, 7z, installer if NSIS is available)...${NC}"
# Same as upstream fuse build_win32.sh: one target builds zip + 7z + setup.exe (see data/win32/distribution.mk).
if command -v makensis &>/dev/null || command -v makensis.exe &>/dev/null; then
    make dist-win32 -j"$(nproc 2>/dev/null || echo 4)"
else
    echo -e "${YELLOW}makensis not on PATH — building zip/7z only. Add NSIS to PATH or install to:${NC}"
    echo "  C:\\Program Files\\NSIS  or  C:\\Program Files (x86)\\NSIS"
    make dist-win32-zip dist-win32-7z -j"$(nproc 2>/dev/null || echo 4)"
fi

echo -e "\n${GREEN}=== Done ===${NC}"
ls -la "$SCRIPT_DIR"/fusex-*-win32*.zip "$SCRIPT_DIR"/fusex-*-win32*.7z 2>/dev/null || true
ls -la "$SCRIPT_DIR"/fusex-*-win32-setup.exe 2>/dev/null || true

SETUP_EXE=""
if [[ -f "$SCRIPT_DIR/config.h" ]]; then
    PACKAGE_VERSION="$( sed -n 's/^#define PACKAGE_VERSION "\(.*\)"/\1/p' "$SCRIPT_DIR/config.h" | head -1 )"
    if [[ -n "$PACKAGE_VERSION" ]]; then
        SETUP_EXE="$SCRIPT_DIR/fusex-${PACKAGE_VERSION}-win32-setup.exe"
        if [[ ! -f "$SETUP_EXE" ]]; then
            SETUP_EXE=""
        fi
    fi
fi
if [[ -z "$SETUP_EXE" ]]; then
    SETUP_EXE="$( ls "$SCRIPT_DIR"/fusex-*-win32-setup.exe 2>/dev/null | sort -V | tail -1 )"
fi
APPCAST_SRC="$SCRIPT_DIR/build/appcast-windows/appcast.xml"
if [[ -n "$SETUP_EXE" ]]; then
    echo -e "\n${GREEN}Generating WinSparkle appcast...${NC}"
    bash "$SCRIPT_DIR/data/win32/generate-appcast.sh" "$SETUP_EXE"
    APPCAST_PUSH_DIR="$SCRIPT_DIR/../speccytools.github.io/updates"
    if [[ -d "$APPCAST_PUSH_DIR" && -f "$APPCAST_SRC" ]]; then
        APPCAST_DEST="$APPCAST_PUSH_DIR/windows"
        mkdir -p "$APPCAST_DEST"
        cp -f "$APPCAST_SRC" "$APPCAST_DEST/appcast.xml"
        echo -e "${GREEN}Copied appcast to ${APPCAST_DEST}/appcast.xml${NC}"
    fi
else
    echo -e "\n${YELLOW}Skipping appcast generation (no setup.exe).${NC}"
fi
