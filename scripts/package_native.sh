#!/usr/bin/env bash
# Package the standalone game (alundra2, phase 4) for macOS and Windows.
#
# The packages hold the program only: no file of the game. At first launch the program
# asks for the player's own disc image (.cue/.bin of Alundra 2, France, SLES-02601) and
# copies the game's files from it. The program itself is translated from the game's code:
# keep the packages for personal use, never publish them.
#
# Usage: scripts/package_native.sh [--mac build-release-native] [--win build-win64-native] [--out dist]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MAC_BUILD="${ROOT}/build-release-native"
WIN_BUILD="${ROOT}/build-win64-native"
OUT="${ROOT}/dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mac) MAC_BUILD="${2:?}"; shift 2 ;;
    --win) WIN_BUILD="${2:?}"; shift 2 ;;
    --out) OUT="${2:?}"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
mkdir -p "${OUT}"

# ---- macOS: Alundra 2.app
if [[ -x "${MAC_BUILD}/alundra2" ]]; then
  NAME="alundra2-fr-${VERSION}-macos"
  STAGE="${OUT}/${NAME}"
  APP="${STAGE}/Alundra 2.app"
  rm -rf "${STAGE}" "${OUT}/${NAME}.zip"
  mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Resources"
  cp "${MAC_BUILD}/alundra2" "${APP}/Contents/MacOS/alundra2"
  strip -x "${APP}/Contents/MacOS/alundra2" 2>/dev/null || true
  ICONSET="$(mktemp -d)/alundra2.iconset"
  mkdir -p "${ICONSET}"
  for size in 16 32 128 256 512; do
    sips -z ${size} ${size} "${ROOT}/assets/psxrecomp.png" --out "${ICONSET}/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) "${ROOT}/assets/psxrecomp.png" --out "${ICONSET}/icon_${size}x${size}@2x.png" >/dev/null
  done
  iconutil -c icns "${ICONSET}" -o "${APP}/Contents/Resources/alundra2.icns"
  cat > "${APP}/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Alundra 2</string>
  <key>CFBundleDisplayName</key><string>Alundra 2</string>
  <key>CFBundleIdentifier</key><string>com.coleim.alundra2fr</string>
  <key>CFBundleExecutable</key><string>alundra2</string>
  <key>CFBundleIconFile</key><string>alundra2</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${VERSION}</string>
  <key>CFBundleVersion</key><string>${VERSION}</string>
  <key>CFBundleDevelopmentRegion</key><string>fr</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST
  cp "${ROOT}/docs/INSTALL_FR.md" "${STAGE}/LISEZMOI.md"
  codesign --force --deep -s - "${APP}" >/dev/null 2>&1 || echo "warning: ad-hoc signing failed" >&2
  (cd "${OUT}" && ditto -c -k --keepParent "${NAME}" "${NAME}.zip")
  echo "macOS:   ${OUT}/${NAME}.zip"
else
  echo "skipping macOS: ${MAC_BUILD}/alundra2 missing" >&2
fi

# ---- Windows: alundra2.exe
if [[ -f "${WIN_BUILD}/alundra2.exe" ]]; then
  NAME="alundra2-fr-${VERSION}-windows-x64"
  STAGE="${OUT}/${NAME}"
  rm -rf "${STAGE}" "${OUT}/${NAME}.zip"
  mkdir -p "${STAGE}"
  cp "${WIN_BUILD}/alundra2.exe" "${STAGE}/"
  x86_64-w64-mingw32-strip "${STAGE}/alundra2.exe" 2>/dev/null || true
  # Windows Notepad: CRLF line ends
  sed 's/$/\r/' "${ROOT}/docs/INSTALL_FR.md" > "${STAGE}/LISEZMOI.txt"
  (cd "${OUT}" && rm -f "${NAME}.zip" && zip -qr "${NAME}.zip" "${NAME}")
  echo "Windows: ${OUT}/${NAME}.zip"
else
  echo "skipping Windows: ${WIN_BUILD}/alundra2.exe missing" >&2
fi
