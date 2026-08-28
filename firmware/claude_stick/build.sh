#!/usr/bin/env bash
#
# Build / upload / monitor do Claude Usage Stick na CYD ESP32-2432S028.
#
# Uso:
#   ./build.sh                 # compila
#   ./build.sh upload          # compila + grava (procura a porta sozinho)
#   ./build.sh upload <porta>  # compila + grava na porta indicada
#   ./build.sh monitor <porta> # abre o serial monitor (115200)
#
# Pre-requisitos (ver docs/HARDWARE-CYD.md):
#   - arduino-cli 1.4+, core esp32:esp32 3.3.11
#   - libs: GFX Library for Arduino 1.6.5, lvgl 9.2.2
#
# No Windows use o build.ps1, que faz o mesmo e acha o arduino-cli embutido
# no Arduino IDE.
#
# O que mudou do original (que era um ESP32-S3 Guition JC4832W535):
#   - placa esp32:esp32:esp32 (ESP32 classico) em vez de esp32s3
#   - FlashSize=4M em vez de 16M
#   - sem PSRAM=opi: esta placa nao tem PSRAM nenhuma
#   - sem CDCOnBoot/USBMode: a serial passa por um CH340 externo, nao pelo
#     USB nativo do chip
#
# O -DLV_CONF_INCLUDE_SIMPLE + -I<sketch> faz o LVGL achar o nosso lv_conf.h.
# Vai tambem em compiler.S.extra_flags porque o core monta os .S do lvgl com
# essa variavel, e nao com a de C/C++ (achado do PR #3 do projeto original).
set -euo pipefail

SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:esp32:FlashSize=4M,PartitionScheme=custom,CPUFreq=240,FlashFreq=80,FlashMode=qio"

LVFLAGS="-DLV_CONF_INCLUDE_SIMPLE -I${SKETCH_DIR}"

find_port() {
  # Linux/macOS: CH340 aparece como ttyUSB* ou cu.wchusbserial*
  ls /dev/ttyUSB* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1
}

cmd="${1:-build}"
port="${2:-}"
if [ -z "$port" ] && [ "$cmd" != "build" ]; then
  port="$(find_port || true)"
  if [ -z "$port" ]; then
    echo "erro: nenhuma porta serial encontrada. Passe a porta como argumento." >&2
    exit 1
  fi
  echo "==> porta encontrada: $port"
fi

compile_args=(
  --fqbn "$FQBN"
  --build-property "compiler.cpp.extra_flags=$LVFLAGS"
  --build-property "compiler.c.extra_flags=$LVFLAGS"
  --build-property "compiler.S.extra_flags=$LVFLAGS"
)

case "$cmd" in
  monitor)
    exec arduino-cli monitor -p "$port" -c baudrate=115200
    ;;
  build)
    echo "==> compilando ($FQBN)"
    arduino-cli compile "${compile_args[@]}" "$SKETCH_DIR"
    ;;
  upload)
    # `compile --upload` compila e grava num passo so (upload puro nao aceita
    # --build-property)
    echo "==> compilando + gravando em $port ($FQBN)"
    arduino-cli compile "${compile_args[@]}" --upload -p "$port" "$SKETCH_DIR"
    ;;
  *)
    echo "comando desconhecido: $cmd (use: build | upload | monitor)" >&2
    exit 1
    ;;
esac
