#!/bin/sh
# Builds the gba2cia player and refreshes assets/player/player.cxi.
# Requires Docker; everything runs in the official devkitPro image.
set -eu
cd "$(dirname "$0")"
IMAGE=devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5
docker run --rm -v "$PWD:/work" -w /work "$IMAGE" bash -c '
set -e
mkdir -p build/tools
if [ ! -f build/mgba/libmgba.a ]; then
  cmake -S third_party/mgba -B build/mgba -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE=/work/third_party/mgba/src/platform/3ds/CMakeToolchain.txt \
    -DCMAKE_BUILD_TYPE=Release -DLIBMGBA_ONLY=ON -DBUILD_STATIC=ON -DBUILD_SHARED=OFF \
    -DM_CORE_GB=OFF -DM_CORE_GBA=ON -DUSE_DEBUGGERS=OFF -DUSE_GDB_STUB=OFF -DUSE_EDITLINE=OFF \
    -DUSE_ZLIB=OFF -DUSE_PNG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF -DUSE_ELF=OFF \
    -DUSE_LZMA=OFF -DUSE_FFMPEG=OFF -DUSE_DISCORD_RPC=OFF -DUSE_LUA=OFF -DENABLE_SCRIPTING=OFF \
    -DBUILD_LTO=OFF
  make -C build/mgba mgba -j"$(nproc)"
fi
if [ ! -x build/tools/Project_CTR/makerom/bin/makerom ]; then
  git clone -q --depth 1 https://github.com/3DSGuy/Project_CTR.git build/tools/Project_CTR
  make -C build/tools/Project_CTR/makerom deps -j"$(nproc)"
  make -C build/tools/Project_CTR/makerom -j"$(nproc)"
fi
make -j"$(nproc)"
build/tools/Project_CTR/makerom/bin/makerom -f cxi -o build/player.cxi -rsf player.rsf \
  -elf build/player.elf -target t -exefslogo
'
cp build/player.cxi ../assets/player/player.cxi
echo "Updated assets/player/player.cxi"
