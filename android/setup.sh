#!/usr/bin/env bash
# android/setup.sh — 拉取第三方依赖并建好链接，让本目录可独立 gradle 构建。
# 用法：在仓库根执行  android/setup.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DEPS="$HERE/deps"
mkdir -p "$DEPS"

SDL_VER="SDL2-2.30.0"
ZLIB_VER="zlib-1.3.1"
PNG_VER="libpng-1.6.43"

dl() { # url dest
  if [ ! -e "$2" ]; then
    echo "下载 $1"
    curl -fL --retry 2 -o "${2}.tar.gz" "$1"
    tar xf "${2}.tar.gz" -C "$DEPS"
  fi
}

dl "https://libsdl.org/release/${SDL_VER}.tar.gz"        "$DEPS/$SDL_VER"
dl "https://zlib.net/${ZLIB_VER}.tar.gz"                 "$DEPS/$ZLIB_VER"
dl "https://download.sourceforge.net/libpng/${PNG_VER}.tar.gz" "$DEPS/$PNG_VER"

# SDL 源码软链到 jni/SDL（jni/CMakeLists.txt 里 add_subdirectory(SDL)）
ln -sfn "$DEPS/$SDL_VER" "$HERE/app/jni/SDL"

echo ""
echo "完成。构建前确认 local.properties 里 sdk.dir 指向你的 Android SDK，然后："
echo "  cd $HERE && gradle :app:assembleDebug"
