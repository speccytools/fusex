#!/usr/bin/env bash

# Build the complete FuseX macOS and Windows release from a Mac.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Error: deploy.sh must be run on macOS." >&2
  exit 1
fi

echo "==> Starting macOS deployment"
bash "$SCRIPT_DIR/deploy_mac.sh"

echo "==> Starting Windows deployment"
bash "$SCRIPT_DIR/deploy_win.sh"

echo "==> Complete FuseX deployment finished"
