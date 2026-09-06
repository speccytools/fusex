#!/usr/bin/env bash

# Build both the Spectranext-ready and stock FuseX Windows installers on macOS.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SETTINGS_PATH="$SCRIPT_DIR/settings.dat"
readonly APPCAST_REPOSITORY="$SCRIPT_DIR/../speccytools.github.io"
readonly APPCAST_DESTINATION="$APPCAST_REPOSITORY/updates/windows/appcast.xml"
readonly MAKEFILE_BACKUP="$(mktemp "${TMPDIR:-/tmp}/fusex-deploy-win-makefile.XXXXXX")"
readonly TEST_DRIVER_PATH="$SCRIPT_DIR/test-driver"
TEST_DRIVER_EXISTED=0
if [[ -e "$TEST_DRIVER_PATH" ]]; then
  TEST_DRIVER_EXISTED=1
fi

cp -p "$SCRIPT_DIR/Makefile" "$MAKEFILE_BACKUP"

restore_settings() {
  git -C "$SCRIPT_DIR" restore --source=HEAD -- settings.dat
}

handle_interrupt() {
  exit 130
}

cleanup() {
  restore_settings || true
  cp -p "$MAKEFILE_BACKUP" "$SCRIPT_DIR/Makefile"
  rm -f "$MAKEFILE_BACKUP"
  if [[ "$TEST_DRIVER_EXISTED" == "0" ]]; then
    rm -f "$TEST_DRIVER_PATH"
  fi
}

trap cleanup EXIT
trap handle_interrupt INT TERM HUP

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Error: deploy_win.sh must be run on macOS." >&2
  exit 1
fi

if [[ ! -d "$APPCAST_REPOSITORY/.git" ]]; then
  echo "Error: appcast repository not found: $APPCAST_REPOSITORY" >&2
  exit 1
fi

for command_name in git perl make pkg-config cmake makensis autoreconf automake glibtoolize \
  x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ \
  x86_64-w64-mingw32-ar x86_64-w64-mingw32-ranlib \
  x86_64-w64-mingw32-strip x86_64-w64-mingw32-windres; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Error: required command not found: $command_name" >&2
    echo "Install the macOS build tools with: brew install mingw-w64 nsis autoconf automake libtool" >&2
    exit 1
  fi
done

# Begin from the committed defaults and restore them even if either build fails.
restore_settings

make_spectranext_settings() {
  restore_settings

  perl -pi -e \
    's/^spectranet,[ \t]*boolean,[ \t]*[01][ \t]*$/spectranet, boolean, 1/; s/^start_machine,[ \t]*string,[ \t]*"[^"]*",,[ \t]*'"'"'m'"'"',[ \t]*machine[ \t]*$/start_machine, string, "128",, '"'"'m'"'"', machine/' \
    "$SETTINGS_PATH"

  if ! grep -Fqx 'spectranet, boolean, 1' "$SETTINGS_PATH" || \
     ! grep -Fqx 'start_machine, string, "128",, '\''m'\'', machine' "$SETTINGS_PATH"; then
    echo "Error: failed to apply the Spectranext defaults to settings.dat." >&2
    exit 1
  fi
}

build_installer() {
  local variant_name="$1"
  local skip_appcast="$2"

  echo "==> Building the $variant_name Windows installer"
  WIN32_CLEAN=1 \
  WIN32_DIST_TARGET=dist-win32-exe \
  REQUIRE_NSIS=1 \
  SKIP_APPCAST="$skip_appcast" \
    bash "$SCRIPT_DIR/build_win32.sh"

  PACKAGE_VERSION="$(sed -n 's/^#define PACKAGE_VERSION "\(.*\)"/\1/p' "$SCRIPT_DIR/config.h" | head -1)"
  if [[ -z "$PACKAGE_VERSION" ]]; then
    echo "Error: could not determine PACKAGE_VERSION from config.h." >&2
    exit 1
  fi

  BUILT_INSTALLER="$SCRIPT_DIR/fusex-${PACKAGE_VERSION}-win32-setup.exe"
  if [[ ! -f "$BUILT_INSTALLER" ]]; then
    echo "Error: NSIS did not create $BUILT_INSTALLER" >&2
    exit 1
  fi
}

make_spectranext_settings
build_installer Spectranext 1
spectranext_installer="$SCRIPT_DIR/fusex-spectranext-${PACKAGE_VERSION}-win32-setup.exe"
mv -f "$BUILT_INSTALLER" "$spectranext_installer"

echo "==> Restoring stock FuseX settings"
restore_settings
build_installer stock 0
stock_installer="$BUILT_INSTALLER"

if [[ ! -f "$APPCAST_DESTINATION" ]]; then
  echo "Error: Windows appcast was not created at $APPCAST_DESTINATION" >&2
  exit 1
fi

echo "==> Deployment complete"
echo "    $spectranext_installer"
echo "    $stock_installer"
echo "    $APPCAST_DESTINATION"
