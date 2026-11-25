#!/usr/bin/env bash
# Build a self-contained "Alundra 2.app" for personal use: the executable, the
# libraries it needs, OpenBIOS AND your own disc image all live inside the app.
# Double-click and play. The result contains the game, so never publish it.
#
# Usage:
#   scripts/package_macos_app.sh --disc /path/to/alundra.cue [--build build-release] [--out dist]
#
# Saves go to ~/Library/Application Support/Alundra 2/saves.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${ROOT}/build-release"
OUT="${ROOT}/dist"
DISC=""
APP_NAME="Alundra 2"
BIN_NAME="alundra_2"
BUNDLE_ID="com.coleim.alundra2fr"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --disc)  DISC="${2:?}"; shift 2 ;;
    --build) BUILD="${2:?}"; shift 2 ;;
    --out)   OUT="${2:?}"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${DISC}" && -f "${DISC}" ]] || { echo "error: --disc <file.cue> required" >&2; exit 2; }
[[ -x "${BUILD}/${BIN_NAME}" ]] || { echo "error: ${BUILD}/${BIN_NAME} missing (build it first)" >&2; exit 1; }
command -v dylibbundler >/dev/null || { echo "error: brew install dylibbundler" >&2; exit 1; }

DISC_DIR="$(cd "$(dirname "${DISC}")" && pwd)"
BIN_SRC="$(sed -n 's/^FILE "\(.*\)" BINARY.*/\1/p' "${DISC}" | head -1)"
[[ -f "${DISC_DIR}/${BIN_SRC}" ]] || { echo "error: ${BIN_SRC} (from the cue) not found next to it" >&2; exit 1; }

APP="${OUT}/${APP_NAME}.app"
RES="${APP}/Contents/Resources"
MACOS="${APP}/Contents/MacOS"
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"

rm -rf "${APP}"
mkdir -p "${MACOS}" "${RES}/disc" "${APP}/Contents/Frameworks"

cp "${BUILD}/${BIN_NAME}" "${MACOS}/${BIN_NAME}"

# Data the runtime resolves beside the executable. It lives in Resources and is
# reached through symlinks, as codesign refuses loose data under Contents/MacOS.
link_data() { ln -s "../Resources/$1" "${MACOS}/$1"; }
cp -R "${BUILD}/assets" "${BUILD}/bios" "${RES}/"
mkdir -p "${RES}/mods"
cp -R "${BUILD}/mods/bundled" "${BUILD}/mods/README.md" "${RES}/mods/"
cp "${BUILD}/game_options.toml" "${BUILD}/psx_game_version.txt" "${BUILD}/${BIN_NAME}.execution.json" "${RES}/"
for n in assets bios mods disc game.toml game_options.toml; do link_data "$n"; done
cp "${BUILD}/psx_game_version.txt" "${MACOS}/"

# The disc keeps neutral names; APFS clones make this instant and free.
cp -c "${DISC_DIR}/${BIN_SRC}" "${RES}/disc/alundra.bin" 2>/dev/null || cp "${DISC_DIR}/${BIN_SRC}" "${RES}/disc/alundra.bin"
printf 'FILE "alundra.bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n' > "${RES}/disc/alundra.cue"

sed -e 's#^disc = .*#disc = "disc/alundra.cue"#' \
    -e 's#^discs = .*#discs = ["disc/alundra.cue"]#' \
    -e 's#^bin_name = .*#bin_name = "alundra.bin"#' \
    -e 's#^cue_name = .*#cue_name = "alundra.cue"#' \
    "${ROOT}/game.toml" > "${RES}/game.toml"

mkdir -p "${RES}/licenses"
cp "${ROOT}/LICENSE" "${ROOT}/THIRD_PARTY_NOTICES.md" "${RES}/licenses/"
cp "${ROOT}/psxrecomp/LICENSE" "${RES}/licenses/psxrecomp-LICENSE"
cp -R "${ROOT}/psxrecomp/runtime/licenses/." "${RES}/licenses/"

# The launcher script is the bundle's executable; the real one sits beside it.
mv "${MACOS}/${BIN_NAME}" "${MACOS}/${BIN_NAME}.real"
cat > "${MACOS}/${APP_NAME}" <<EOF
#!/bin/bash
DIR="\$(cd "\$(dirname "\$0")" && pwd)"
SAVES="\$HOME/Library/Application Support/Alundra 2/saves"
mkdir -p "\$SAVES"
export SDL_JOYSTICK_HIDAPI_STEAM=1
exec "\$DIR/${BIN_NAME}.real" --game "\$DIR/game.toml" --disc "\$DIR/../Resources/disc/alundra.cue" --memcard-dir "\$SAVES" --no-launcher "\$@"
EOF
chmod +x "${MACOS}/${APP_NAME}"

cat > "${APP}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>${APP_NAME}</string>
  <key>CFBundleDisplayName</key><string>${APP_NAME}</string>
  <key>CFBundleIdentifier</key><string>${BUNDLE_ID}</string>
  <key>CFBundleExecutable</key><string>${APP_NAME}</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
</dict></plist>
EOF

# Icon from the framework PNG.
ICONSET="$(mktemp -d)/AppIcon.iconset"
mkdir -p "${ICONSET}"
for s in 16 32 64 128 256 512; do
  sips -z "$s" "$s" "${ROOT}/assets/psxrecomp.png" --out "${ICONSET}/icon_${s}x${s}.png" >/dev/null
done
iconutil -c icns "${ICONSET}" -o "${RES}/AppIcon.icns" 2>/dev/null || true

dylibbundler -od -b -x "${MACOS}/${BIN_NAME}.real" \
  -d "${APP}/Contents/Frameworks" -p @executable_path/../Frameworks >/dev/null

codesign --force --deep --sign - "${APP}" >/dev/null
codesign --verify --deep "${APP}"

BAD="$(otool -L "${MACOS}/${BIN_NAME}.real" | tail -n +2 | awk '{print $1}' \
  | grep -vE '^(/usr/lib/|/System/Library/|@executable_path/../Frameworks/)' || true)"
[[ -z "${BAD}" ]] || { echo "error: not self-contained: ${BAD}" >&2; exit 1; }

echo "${APP}"
du -sh "${APP}"
