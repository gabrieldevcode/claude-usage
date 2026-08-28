#!/usr/bin/env bash
#
# flash.sh - grava a versao atual do firmware na CYD ESP32-2432S028.
#
# Uso: conecte a placa no USB e rode:
#     ./flash.sh
#
# O script encontra a porta sozinho (espera ate 30s se ainda nao conectou),
# compila e grava. Pre-requisitos: arduino-cli + core esp32 3.3.11 + libs
# (ver docs/HARDWARE-CYD.md).
#
# No Windows use flash.ps1 - a CYD usa um CH340, que aparece como COMx e nao
# como /dev/tty*.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if ! command -v arduino-cli >/dev/null; then
  echo "erro: arduino-cli nao encontrado" >&2
  exit 1
fi

# A CYD usa um CH340: ttyUSB* no Linux, cu.wchusbserial*/cu.usbserial* no macOS.
find_port() { ls /dev/ttyUSB* /dev/cu.wchusbserial* /dev/cu.usbserial* 2>/dev/null | head -1; }

PORT="$(find_port || true)"
if [ -z "$PORT" ]; then
  echo "==> nenhuma placa na USB - conecte o device (aguardando ate 30s)..."
  for _ in $(seq 1 30); do
    sleep 1
    PORT="$(find_port || true)"
    [ -n "$PORT" ] && break
  done
fi
if [ -z "$PORT" ]; then
  echo "erro: nenhuma porta serial apareceu. A placa esta conectada?" >&2
  exit 1
fi

echo "==> placa encontrada em $PORT"
echo "==> compilando e gravando a versao atual..."
firmware/claude_stick/build.sh upload "$PORT"
echo
echo "==> pronto! O device reinicia sozinho (vai pedir o PIN na tela)."
