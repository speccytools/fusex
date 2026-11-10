#!/usr/bin/env bash
# Collect the installed MSYS2 package notices for the DLLs actually shipped.
set -euo pipefail

root=${1:?Usage: collect_windows_notices.sh DLL-directory notice-directory}
notices=${2:?Usage: collect_windows_notices.sh DLL-directory notice-directory}
prefix=${MINGW_PREFIX:?Run this script in an MSYS2 MinGW environment}
mkdir -p "$notices"
declare -A packages=()

while IFS= read -r dll; do
  name=$(basename "$dll")
  case "$name" in
    libspectrum-*.dll) continue ;; # COPYING comes from the selected source tree.
  esac
  installed="$prefix/bin/$name"
  test -f "$installed" && cmp -s "$dll" "$installed" || {
    echo "Runtime DLL does not match the installed MSYS2 package: $dll" >&2
    exit 1
  }
  package=$(pacman -Qqo "$installed")
  packages["$package"]=1
done < <(find "$root" -maxdepth 1 -type f -iname '*.dll' -print)

for package in "${!packages[@]}"; do
  found=0
  while IFS= read -r file; do
    test -f "$file" || continue
    case "$file" in
      */share/licenses/*|*/share/doc/*)
        case "$(basename "$file")" in
          COPYING*|LICENSE*|COPYRIGHT*|AUTHORS*|README*)
            cp "$file" "$notices/$package-$(basename "$file").txt"
            found=1 ;;
        esac ;;
    esac
  done < <(pacman -Qlq "$package")
  if [ "$found" = 0 ]; then
    echo "No third-party notice found for shipped package $package" >&2
    exit 1
  fi
done
