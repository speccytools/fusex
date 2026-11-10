#!/usr/bin/env bash
# Use the normal MSYS2 packages, pinned explicitly while absent from the DB.
# The resulting cache travels with libspectrum to the downstream Fuse job.
set -euo pipefail

test "${MSYSTEM:-}" = MINGW32 || {
  echo "Run in MSYS2 MINGW32, not MSYS/UCRT64 or a Cygwin cross environment" >&2
  exit 1
}
script_dir=$(cd "$(dirname "$0")" && pwd)
lock="$script_dir/windows-mingw32-crypto.sha256"
cache=${1:-/opt/fuse/share/fuse/packages}
mkdir -p "$cache"
cache=$(cd "$cache" && pwd)
packages=()
while read -r hash name; do
  # Windows checkouts may supply CRLF even when invoked from an MSYS2 shell.
  name=${name%$'\r'}
  case "$hash" in ''|'#'*) continue ;; esac
  for file in "$name" "$name.sig"; do
    if [ ! -f "$cache/$file" ]; then
      curl --fail --location --retry 3 \
        "https://repo.msys2.org/mingw/mingw32/$file" -o "$cache/$file.part"
      mv "$cache/$file.part" "$cache/$file"
    fi
  done
  # Fail closed: do not disable pacman's signature policy to use retained files.
  # pacman 6.1's wrapper rejects non-root verification unless lock-never is
  # configured, although its --init no longer adds that setting. Use GnuPG
  # directly with the same keyring and trusted-signer requirement instead.
  status=$(gpg --batch --homedir /etc/pacman.d/gnupg --status-fd 1 \
    --verify "$cache/$name.sig" "$cache/$name")
  if ! grep -qE '^\[GNUPG:\] TRUST_(FULLY|ULTIMATE)( |$)' <<< "$status"; then
    echo "Package signature is not from a trusted MSYS2 signer: $name" >&2
    exit 1
  fi
  packages+=("$cache/$name")
done < "$lock"
(cd "$cache" && tr -d '\r' < "$lock" | sha256sum --check -)
# Install both in one transaction; dependencies such as cc-libs and gettext
# continue to resolve from the normal MINGW32 package database.
pacman --noconfirm -U --needed "${packages[@]}"
