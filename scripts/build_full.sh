#!/usr/bin/env bash
# Build the playable game from YOUR disc image, from scratch:
#   disc image -> game files (tools/extract_disc.py) -> assembly (splat) -> C (tools/a2lift.py)
#   -> alundra2 for macOS (universal) and Windows x64 (MinGW) -> packages in dist/
#
# Usage: scripts/build_full.sh --disc /path/to/alundra.cue [--no-mac] [--no-win] [--out dist]
#
# Needs: python3, cmake, ninja; on macOS Xcode command line tools; for Windows the MinGW-w64
# cross compiler (brew install mingw-w64, or apt-get install mingw-w64). SDL3 is downloaded
# (or taken from $SDL3_SOURCE_DIR). The game's files and its translated code stay on this
# machine (extracted/, decomp/, src/game/lifted/ are ignored by Git).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"
DISC=""
MAC=1
WIN=1
OUT="${ROOT}/dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --disc)   DISC="${2:?}"; shift 2 ;;
    --no-mac) MAC=0; shift ;;
    --no-win) WIN=0; shift ;;
    --out)    OUT="${2:?}"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -n "${DISC}" && -f "${DISC}" ]] || { echo "error: --disc <alundra.cue> required" >&2; exit 2; }
[[ "$(uname)" == "Darwin" ]] || MAC=0

step() { printf '\n== %s\n' "$*"; }

step "Python tools (.venv-decomp)"
if [[ ! -x .venv-decomp/bin/python ]]; then python3 -m venv .venv-decomp; fi
.venv-decomp/bin/pip install -q --disable-pip-version-check -r tools/requirements-decomp.txt
PY="${ROOT}/.venv-decomp/bin/python"

step "Game files from the disc (extracted/)"
"${PY}" tools/extract_disc.py "${DISC}" --out extracted

step "Assembly of the game's code (decomp/, splat)"
for c in config/*.yaml; do "${PY}" -m splat split "$c" > /dev/null; done

step "Game code in C (src/game/lifted/)"
"${PY}" tools/a2lift.py

SDL_ARGS=(-DA2_FETCH_SDL3=ON)
if [[ -n "${SDL3_SOURCE_DIR:-}" ]]; then SDL_ARGS+=(-DFETCHCONTENT_SOURCE_DIR_SDL3="${SDL3_SOURCE_DIR}"); fi

if [[ "${MAC}" == 1 ]]; then
  step "macOS build (universal)"
  cmake -S . -B build-release-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DA2_FRAMEWORK=OFF "${SDL_ARGS[@]}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" > /dev/null
  cmake --build build-release-native --target alundra2
fi

if [[ "${WIN}" == 1 ]]; then
  step "Windows x64 build (MinGW)"
  if [[ "$(uname)" == "Darwin" ]]; then TOOLCHAIN=cmake/toolchain-mingw-w64-macos.cmake; else TOOLCHAIN=cmake/toolchain-mingw-w64.cmake; fi
  cmake -S . -B build-win64-native -G Ninja -DCMAKE_BUILD_TYPE=Release -DA2_FRAMEWORK=OFF "${SDL_ARGS[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" > /dev/null
  cmake --build build-win64-native --target alundra2
fi

step "Packages"
ARGS=(--out "${OUT}")
[[ "${MAC}" == 1 ]] || ARGS+=(--mac /nonexistent)
[[ "${WIN}" == 1 ]] || ARGS+=(--win /nonexistent)
scripts/package_native.sh "${ARGS[@]}"
