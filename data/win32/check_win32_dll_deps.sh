#!/usr/bin/env bash
set -euo pipefail

root_dir=${1:-}
if [ -z "$root_dir" ] || [ ! -d "$root_dir" ]; then
  echo "Usage: $0 <directory>" >&2
  exit 1
fi

is_system_dll() {
  case "$1" in
    kernel32.dll|user32.dll|gdi32.dll|advapi32.dll|shell32.dll|ole32.dll| \
    oleaut32.dll|uuid.dll|ws2_32.dll|comdlg32.dll|winmm.dll|version.dll| \
    msvcrt.dll|bcrypt.dll|ntdll.dll|shlwapi.dll|crypt32.dll|dbghelp.dll| \
    psapi.dll|iphlpapi.dll|secur32.dll|setupapi.dll|imm32.dll|mpr.dll| \
    winspool.drv|api-ms-win-*|ext-ms-*|comctl32.dll|dwmapi.dll|dnsapi.dll| \
    dwrite.dll|gdiplus.dll|hid.dll|msimg32.dll|opengl32.dll|rpcrt4.dll|usp10.dll| \
    dsound.dll|dinput8.dll|dxgi.dll|uxtheme.dll|wintrust.dll|normaliz.dll| \
    powrprof.dll|cfgmgr32.dll|avrt.dll|msacm32.dll|ucrtbase.dll) return 0 ;;
  esac
  return 1
}

# Prefer the staged library's exact runtime set, then the selected MSYS2
# environment. Never resolve native dependencies from MSYS /usr/bin.
search_dirs=("$root_dir" /opt/fuse/bin)
if [ -n "${MINGW_PREFIX:-${MSYSTEM_PREFIX:-}}" ]; then
  search_dirs+=("${MINGW_PREFIX:-$MSYSTEM_PREFIX}/bin")
fi

if [ -n "${WIN32_DLL_SEARCH_DIRS:-}" ]; then
  # shellcheck disable=SC2206
  extra_dirs=(${WIN32_DLL_SEARCH_DIRS})
  search_dirs+=("${extra_dirs[@]}")
fi

find_binaries() {
  if [ "${WIN32_DLL_DEPS_SCAN_EXES:-1}" = 0 ]; then
    find "$root_dir" -maxdepth 1 -type f -iname '*.dll' -print
  else
    find "$root_dir" -maxdepth 1 -type f \( -iname '*.exe' -o -iname '*.dll' \) -print
  fi
}

changed=1
while [ "$changed" -ne 0 ]; do
  changed=0
  while IFS= read -r file; do
    # Inspect outside process substitution so malformed PE files fail the build.
    imports=$(objdump -p "$file")
    while IFS= read -r dll; do
      dll_lc=$(printf '%s' "$dll" | tr '[:upper:]' '[:lower:]')
      case "$dll_lc" in
        msys-*.dll|cygwin*.dll)
          echo "Non-native runtime dependency for ${file}: ${dll}" >&2
          exit 1 ;;
      esac
      if is_system_dll "$dll_lc"; then
        continue
      fi

      if [ -f "$root_dir/$dll" ]; then
        continue
      fi

      found=0
      for searchdir in "${search_dirs[@]}"; do
        if [ -f "$searchdir/$dll" ]; then
          cp -f "$searchdir/$dll" "$root_dir/"
          changed=1
          found=1
          break
        fi
      done

      if [ "$found" -eq 0 ]; then
        echo "Missing dependency for ${file}: ${dll}" >&2
        exit 1
      fi
    done < <(
      printf '%s\n' "$imports" | awk '/DLL Name:/ { sub(/.*DLL Name: /, ""); sub(/\r$/, ""); print }'
    )
  done < <(find_binaries)
done
