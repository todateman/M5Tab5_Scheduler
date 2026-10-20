#!/bin/sh
# src/main.cpp を PC 向けにビルドし、擬似スクリーンショットを docs/ に出力する。
#
#   ./tools/sim/build.sh            # ビルドして撮影（2026/10/24 13:50:00 の画面）
#   ./tools/sim/build.sh "2026/10/25 12:40:00"
#
# 必要なもの: clang++、SDL2 のヘッダーとライブラリ（macOS は `brew install sdl2`）、
#             一度 PlatformIO でビルドして .pio/libdeps に取得済みの M5GFX
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SIM="$ROOT/tools/sim"
OUT="$SIM/build"
M5GFX=$(ls -d "$ROOT"/.pio/libdeps/*/M5GFX/src 2>/dev/null | head -n 1)
SDL_PREFIX=$(brew --prefix sdl2 2>/dev/null || echo /usr/local)

if [ -z "$M5GFX" ]; then
  echo "M5GFX が見つかりません。先に PlatformIO でビルドしてください" >&2
  exit 1
fi

CFLAGS="-O2 -w -I$M5GFX -I$SDL_PREFIX/include"
CXXFLAGS="$CFLAGS -std=c++17"

mkdir -p "$OUT/lib"
for src in \
  lgfx/v1/LGFXBase.cpp lgfx/v1/LGFX_Sprite.cpp lgfx/v1/LGFX_Button.cpp lgfx/v1/lgfx_fonts.cpp \
  lgfx/v1/misc/common_function.cpp lgfx/v1/misc/DividedFrameBuffer.cpp \
  lgfx/v1/misc/pixelcopy.cpp lgfx/v1/misc/SpriteBuffer.cpp \
  lgfx/v1/panel/Panel_Device.cpp lgfx/v1/platforms/sdl/common.cpp \
  lgfx/utility/lgfx_miniz.c lgfx/utility/lgfx_pngle.c lgfx/utility/lgfx_qoi.c \
  lgfx/utility/lgfx_qrcode.c lgfx/utility/lgfx_tjpgd.c \
  lgfx/Fonts/IPA/lgfx_font_japan.c lgfx/Fonts/efont/lgfx_efont_ja.c lgfx/Fonts/efont/lgfx_efont_cn.c \
  lgfx/Fonts/efont/lgfx_efont_kr.c lgfx/Fonts/efont/lgfx_efont_tw.c
do
  obj="$OUT/lib/$(echo "$src" | tr '/' '_').o"
  if [ ! -f "$obj" ]; then
    echo "compile $src"
    case "$src" in
      *.c)   clang   $CFLAGS   -c "$M5GFX/$src" -o "$obj" ;;
      *.cpp) clang++ $CXXFLAGS -c "$M5GFX/$src" -o "$obj" ;;
    esac
  fi
done

echo "compile src/main.cpp (tools/sim/sim_main.cpp)"
clang++ $CXXFLAGS -I"$SIM/stubs" "$SIM/sim_main.cpp" "$OUT"/lib/*.o \
  -L"$SDL_PREFIX/lib" -lSDL2 -o "$OUT/tab5_sim"

mkdir -p "$ROOT/docs"
"$OUT/tab5_sim" "$ROOT/docs" "$ROOT/microsd" "${1:-2026/10/24 13:50:00}"
