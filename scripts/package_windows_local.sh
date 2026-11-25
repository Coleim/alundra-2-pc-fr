#!/usr/bin/env bash
# Stage and zip the cross-built Windows game for personal use with your own
# disc: unzip, copy alundra.cue + alundra.bin into disc/, run alundra_2.exe.
#
# Usage: scripts/package_windows_local.sh [build-dir] [out-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-${ROOT}/build-win64}"
OUT="${2:-${ROOT}/dist}"
VERSION="$(tr -d '[:space:]' < "${ROOT}/VERSION")"
NAME="alundra-2-fr-recomp-${VERSION}-windows-x64"
STAGE="${OUT}/${NAME}"
OBJDUMP="x86_64-w64-mingw32-objdump"
SYSROOT="$(x86_64-w64-mingw32-gcc -print-sysroot)"

[[ -f "${BUILD}/alundra_2.exe" ]] || { echo "error: ${BUILD}/alundra_2.exe missing" >&2; exit 1; }

rm -rf "${STAGE}"
mkdir -p "${STAGE}/disc" "${STAGE}/mods" "${STAGE}/licenses"

cp "${BUILD}/alundra_2.exe" "${STAGE}/"
cp -R "${BUILD}/assets" "${BUILD}/bios" "${STAGE}/"
cp -R "${BUILD}/mods/bundled" "${STAGE}/mods/"
cp "${BUILD}/mods/README.md" "${STAGE}/mods/"
for f in game_options.toml psx_game_version.txt alundra_2.execution.json; do
    [[ -f "${BUILD}/${f}" ]] && cp "${BUILD}/${f}" "${STAGE}/"
done

# The disc keeps the file names of the original dump.
sed -e 's#^disc = .*#disc = "disc/alundra.cue"#' \
    -e 's#^discs = .*#discs = ["disc/alundra.cue"]#' \
    -e 's#^bin_name = .*#bin_name = "alundra.bin"#' \
    -e 's#^cue_name = .*#cue_name = "alundra.cue"#' \
    "${ROOT}/game.toml" > "${STAGE}/game.toml"

# MinGW runtime DLLs the executable imports, if it is not fully static.
for dll in $("${OBJDUMP}" -p "${BUILD}/alundra_2.exe" | awk '/DLL Name:/ {print $3}' | grep -i '^lib.*\.dll$' || true); do
    src="$(find "${SYSROOT}" -iname "${dll}" -print -quit)"
    [[ -n "${src}" ]] && cp "${src}" "${STAGE}/"
done

cp "${ROOT}/LICENSE" "${ROOT}/THIRD_PARTY_NOTICES.md" "${STAGE}/licenses/"
cp "${ROOT}/psxrecomp/LICENSE" "${STAGE}/licenses/psxrecomp-LICENSE"
cp -R "${ROOT}/psxrecomp/runtime/licenses/." "${STAGE}/licenses/"

cat > "${STAGE}/LISEZMOI.txt" <<'EOF'
Alundra 2 - Une Legende Est Nee (France) - recompilation PC
===========================================================

1. Copiez vos fichiers du jeu dans le dossier "disc" :
     disc\alundra.cue
     disc\alundra.bin
   (alundra.cue doit contenir la ligne : FILE "alundra.bin" BINARY)

2. Lancez alundra_2.exe.

Aucun BIOS n'est necessaire : OpenBIOS (libre) est inclus.
Les sauvegardes sont creees dans le dossier "saves".
EOF

(cd "${OUT}" && rm -f "${NAME}.zip" && zip -qr "${NAME}.zip" "${NAME}")
echo "${OUT}/${NAME}.zip"
