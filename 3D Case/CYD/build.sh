#!/usr/bin/env bash
# Gera os STL e as pré-visualizações a partir do clawd_cyd.scad.
#
# Uso:   ./build.sh
# Windows (Git Bash / WSL) e Linux/macOS. No PowerShell, use build.ps1.
set -euo pipefail
cd "$(dirname "$0")"

SCAD="${OPENSCAD:-openscad}"
if ! command -v "$SCAD" >/dev/null 2>&1; then
  for c in "/c/Program Files/OpenSCAD/openscad.exe" \
           "/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD"; do
    [ -x "$c" ] && SCAD="$c" && break
  done
fi
command -v "$SCAD" >/dev/null 2>&1 || [ -x "$SCAD" ] || {
  echo "openscad nao encontrado. Instale (winget install OpenSCAD.OpenSCAD)"; exit 1; }

for p in corpo tampa teste; do
  echo "==> $p.stl"
  "$SCAD" --export-format binstl -o "$p.stl" -D "part=\"$p\"" clawd_cyd.scad
done

echo "==> pre-visualizacoes"
# frente: orto, de frente e em pe (a peca tem a cara em Z=0, entao gira 180)
"$SCAD" -o preview-frente.png --imgsize=880,780 --projection=o \
        --camera=0,0,0,180,0,180,235 --colorscheme=Tomorrow \
        -D 'part="corpo"' clawd_cyd.scad
"$SCAD" -o preview-tras.png   --imgsize=880,780 --projection=o \
        --camera=0,0,0,0,0,0,235   --colorscheme=Tomorrow \
        -D 'part="corpo"' clawd_cyd.scad
"$SCAD" -o preview-lado.png   --imgsize=820,560 --projection=p \
        --camera=0,0,0,68,0,215,175 --colorscheme=Tomorrow \
        -D 'part="corpo"' clawd_cyd.scad
"$SCAD" -o preview-tampa.png  --imgsize=760,520 --projection=p \
        --camera=0,0,0,52,0,28,150  --colorscheme=Tomorrow \
        -D 'part="tampa"' clawd_cyd.scad

echo
ls -la ./*.stl
