#!/bin/sh
# Builds the gba2cia demo ROM (assets/demo/gba2cia-demo.gba).
set -eu
cd "$(dirname "$0")"
IMAGE=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
mkdir -p build
docker run --rm -v "$PWD:/work" -w /work "$IMAGE" bash -c '
set -e
export PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:$PATH
arm-none-eabi-gcc -mcpu=arm7tdmi -mthumb -mthumb-interwork -O2 -ffreestanding -nostdlib -Wall \
  -T demo.ld start.s demo.c -lgcc -o build/demo.elf
arm-none-eabi-objcopy -O binary build/demo.elf build/code.gba
'
python3 -I make_demo.py build/code.gba build/demo.gba
docker run --rm -v "$PWD:/work" -w /work "$IMAGE" bash -c '
/opt/devkitpro/tools/bin/gbafix build/demo.gba -tGBA2CIADEMO -cG2CE -m01 >/dev/null
'
mkdir -p ../../assets/demo
cp build/demo.gba ../../assets/demo/gba2cia-demo.gba
echo "Built assets/demo/gba2cia-demo.gba"
