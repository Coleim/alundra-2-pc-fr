#!/usr/bin/env bash
# Launch the local macOS build with this project's game.toml (disc under disc/).
# Extra arguments are passed through, e.g. --no-launcher to boot straight in.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec "${ROOT}/build-release/alundra_2" --game "${ROOT}/game.toml" "$@"
