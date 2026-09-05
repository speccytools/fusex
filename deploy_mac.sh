#!/usr/bin/env bash

# Build both the Spectranext-ready and stock FuseX macOS distributions.
# Xcode uses the signing team and identities configured in FuseX.xcodeproj.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$SCRIPT_DIR/fusepb"
readonly PROJECT_PATH="$PROJECT_DIR/FuseX.xcodeproj"
readonly SETTINGS_PATH="$SCRIPT_DIR/settings.dat"
readonly BUILD_DIR="$SCRIPT_DIR/build"
readonly DIST_APP_PATH="$SCRIPT_DIR/dist/FuseX.app"
readonly APPCAST_REPOSITORY="$SCRIPT_DIR/../speccytools.github.io"
readonly APPCAST_DESTINATION="$APPCAST_REPOSITORY/appcast.xml"
readonly SPARKLE_APPCAST_TOOL="${SPARKLE_GENERATE_APPCAST:-$(find "$HOME/Library/Developer/Xcode/DerivedData" -path '*/SourcePackages/artifacts/sparkle/Sparkle/bin/generate_appcast' -type f -print -quit 2>/dev/null)}"
readonly NOTARIZATION_TIMEOUT="${NOTARIZATION_TIMEOUT:-21600}"
readonly NOTARIZATION_POLL_INTERVAL="${NOTARIZATION_POLL_INTERVAL:-60}"

restore_settings() {
  git -C "$SCRIPT_DIR" restore --source=HEAD -- settings.dat
}

handle_interrupt() {
  exit 130
}

trap restore_settings EXIT
trap handle_interrupt INT TERM HUP

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Error: deploy_mac.sh must be run on macOS." >&2
  exit 1
fi

if ! command -v git >/dev/null 2>&1; then
  echo "Error: required command not found: git" >&2
  exit 1
fi

# Always begin from the committed settings, including when a later preflight
# check fails. The EXIT trap also guarantees that the file is restored.
restore_settings

if [[ ! -d "$APPCAST_REPOSITORY/.git" ]]; then
  echo "Error: appcast repository not found: $APPCAST_REPOSITORY" >&2
  exit 1
fi

if [[ -z "$SPARKLE_APPCAST_TOOL" || ! -x "$SPARKLE_APPCAST_TOOL" ]]; then
  echo "Error: Sparkle generate_appcast tool not found." >&2
  echo "Build or resolve the FuseX Sparkle package first, or set SPARKLE_GENERATE_APPCAST." >&2
  exit 1
fi

for command_name in make perl plutil xcodebuild create-dmg; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Error: required command not found: $command_name" >&2
    exit 1
  fi
done

if ! [[ "$NOTARIZATION_TIMEOUT" =~ ^[1-9][0-9]*$ ]]; then
  echo "Error: NOTARIZATION_TIMEOUT must be a positive number of seconds." >&2
  exit 1
fi

if ! [[ "$NOTARIZATION_POLL_INTERVAL" =~ ^[1-9][0-9]*$ ]]; then
  echo "Error: NOTARIZATION_POLL_INTERVAL must be a positive number of seconds." >&2
  exit 1
fi

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

clean_xcode_project() {
  echo "==> Cleaning FuseX.xcodeproj"
  (
    cd "$PROJECT_DIR"
    xcodebuild clean \
      -project "$PROJECT_PATH" \
      -scheme FuseX \
      -configuration Deployment
  )
}

create_archive() {
  local archive_name="$1"
  local archive_path="$BUILD_DIR/$archive_name.xcarchive"

  rm -rf "$archive_path"
  make -C "$SCRIPT_DIR" archive "ARCHIVE_PATH=build/$archive_name.xcarchive"

  if [[ ! -d "$archive_path" ]]; then
    echo "Error: Xcode did not create $archive_path" >&2
    exit 1
  fi

  CREATED_ARCHIVE_PATH="$archive_path"
}

upload_for_direct_distribution() {
  local archive_path="$1"
  local variant_name="$2"
  local export_options="$BUILD_DIR/ExportOptions-${variant_name}.plist"
  local upload_path="$BUILD_DIR/${variant_name}-upload"

  rm -rf "$upload_path"
  rm -f "$export_options"
  plutil -create xml1 "$export_options"
  plutil -insert method -string developer-id "$export_options"
  plutil -insert destination -string upload "$export_options"

  echo "==> Uploading $variant_name archive for Direct Distribution"
  xcodebuild -exportArchive \
    -archivePath "$archive_path" \
    -exportPath "$upload_path" \
    -exportOptionsPlist "$export_options" \
    -allowProvisioningUpdates
}

wait_and_export_notarized_app() {
  local archive_path="$1"
  local variant_name="$2"
  local started_at="$SECONDS"
  local attempt_log="$BUILD_DIR/export-notarized-${variant_name}.log"

  echo "==> Waiting for Apple to notarize $variant_name"
  while (( SECONDS - started_at < NOTARIZATION_TIMEOUT )); do
    rm -rf "$DIST_APP_PATH"
    mkdir -p "$SCRIPT_DIR/dist"

    if xcodebuild -exportNotarizedApp \
      -archivePath "$archive_path" \
      -exportPath "$SCRIPT_DIR/dist" >"$attempt_log" 2>&1; then
      if [[ ! -d "$DIST_APP_PATH" ]]; then
        echo "Error: notarized export completed without creating $DIST_APP_PATH" >&2
        exit 1
      fi
      echo "==> Exported notarized app to $DIST_APP_PATH"
      return
    fi

    echo "    Notarization is not ready; checking again in ${NOTARIZATION_POLL_INTERVAL}s"
    sleep "$NOTARIZATION_POLL_INTERVAL"
  done

  echo "Error: notarization of $variant_name did not finish within ${NOTARIZATION_TIMEOUT}s." >&2
  echo "Last Xcode response:" >&2
  tail -n 30 "$attempt_log" >&2
  exit 1
}

make_dmg() {
  local output_name="$1"
  local standard_name="FuseX-${FUSEX_VERSION}.dmg"

  make -C "$SCRIPT_DIR" dmg
  if [[ ! -f "$SCRIPT_DIR/$standard_name" ]]; then
    echo "Error: make dmg did not create $standard_name" >&2
    exit 1
  fi

  if [[ "$output_name" != "$standard_name" ]]; then
    mv -f "$SCRIPT_DIR/$standard_name" "$SCRIPT_DIR/$output_name"
  fi
  echo "==> Created $SCRIPT_DIR/$output_name"
}

generate_appcast() {
  local dmg_name="FuseX-${FUSEX_VERSION}.dmg"

  echo "==> Generating macOS Sparkle appcast"
  make -C "$SCRIPT_DIR" appcast \
    "APPCAST_ARCHIVE=./$dmg_name" \
    "SPARKLE_GENERATE_APPCAST=$SPARKLE_APPCAST_TOOL"

  if [[ ! -f "$BUILD_DIR/appcast/appcast.xml" ]]; then
    echo "Error: appcast generation did not create $BUILD_DIR/appcast/appcast.xml" >&2
    exit 1
  fi

  cp -f "$BUILD_DIR/appcast/appcast.xml" "$APPCAST_DESTINATION"
  echo "==> Updated $APPCAST_DESTINATION"
}

FUSEX_VERSION="$(awk -F'= ' '/MARKETING_VERSION = / { gsub(/[";]/, "", $2); print $2; exit }' "$PROJECT_PATH/project.pbxproj")"
if [[ -z "$FUSEX_VERSION" ]]; then
  echo "Error: could not determine MARKETING_VERSION from FuseX.xcodeproj." >&2
  exit 1
fi
readonly FUSEX_VERSION

mkdir -p "$BUILD_DIR"

echo "==> Building FuseX $FUSEX_VERSION distributions"
make_spectranext_settings
clean_xcode_project

create_archive FuseX-spectranext
spectranext_archive="$CREATED_ARCHIVE_PATH"
upload_for_direct_distribution "$spectranext_archive" spectranext
wait_and_export_notarized_app "$spectranext_archive" spectranext
make_dmg "FuseX-spectranext-${FUSEX_VERSION}.dmg"

echo "==> Restoring stock FuseX settings"
restore_settings

create_archive FuseX
stock_archive="$CREATED_ARCHIVE_PATH"
upload_for_direct_distribution "$stock_archive" stock
wait_and_export_notarized_app "$stock_archive" stock
make_dmg "FuseX-${FUSEX_VERSION}.dmg"
generate_appcast

echo "==> Deployment complete"
echo "    $SCRIPT_DIR/FuseX-spectranext-${FUSEX_VERSION}.dmg"
echo "    $SCRIPT_DIR/FuseX-${FUSEX_VERSION}.dmg"
echo "    $APPCAST_DESTINATION"
