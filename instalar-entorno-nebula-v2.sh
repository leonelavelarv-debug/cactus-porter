#!/usr/bin/env bash
# Desde la raíz de cactus-porter, en Ubuntu 24.04 x86-64.
set -euo pipefail
BASE=$(realpath -m "${1:-$HOME/nebula-entorno}")
PORT=$(pwd -P)
TC="$PORT/fuentes/cactus-galaxy/platform/knulli/aarch64-toolchain.cmake"
[[ -f "$TC" ]] || { echo 'Entra en ~/cactus-porter y ejecuta fetch primero.' >&2; exit 1; }
[[ $(uname -m) == x86_64 ]] || { echo 'Este instalador usa herramientas para PC x86-64.' >&2; exit 1; }
sudo apt-get update
sudo apt-get install -y build-essential clang-18 lld-18 cmake ninja-build git wget xz-utils ca-certificates file binutils python3 pkg-config zlib1g-dev libbz2-dev liblzma-dev libzstd-dev
mkdir -p "$BASE/tools" "$BASE/bin" "$BASE/deps"
for cmd in clang clang++ ld.lld; do
    ln -sfn "$(command -v "$cmd-18")" "$BASE/bin/$cmd"
done
export PATH="$BASE/bin:$PATH"
triplet=aarch64-rpi3-linux-gnu
archive="$BASE/tools/x-tools-aarch64-rpi3-linux-gnu-gcc14.tar.xz"
sha=39c42ae5f0e261203f16f69292fc1a7449381507c5f2088939fb8e258c892cec
verified_archive() { echo "$sha  $archive" | sha256sum --check --status; }
if [[ ! -f "$archive" ]] || ! verified_archive; then
    wget -O "$archive.part" "https://github.com/tttapa/toolchains/releases/download/1.4.0/${archive##*/}"
    echo "$sha  $archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
tcdir="$BASE/tools/gcc14-1.4.0"
if [[ ! -f "$tcdir/.complete" ]]; then
    mkdir -p "$tcdir"
    tar -xJf "$archive" -C "$tcdir"
    touch "$tcdir/.complete"
fi
gcc=$(find "$tcdir" -path "*/bin/$triplet-gcc" -print -quit)
[[ -x "$gcc" ]] || { echo 'El archivo extraído carece del compilador esperado.' >&2; exit 1; }
export AARCH64_XTOOLS=$(dirname "$(dirname "$gcc")")
test -d "$AARCH64_XTOOLS/$triplet/sysroot"
"$gcc" -dumpfullversion

checkout() {
    local dest=$1 repo=$2 rev=$3
    if [[ ! -e "$dest" ]]; then git init -q "$dest"; fi
    [[ -d "$dest/.git" ]] || { echo "Comprueba el directorio existente: $dest" >&2; return 1; }
    if git -C "$dest" rev-parse --verify HEAD >/dev/null 2>&1; then
        [[ "$(git -C "$dest" rev-parse HEAD)" == "$rev" ]] || {
            echo "Ya existe otra revisión en $dest. Se conserva tu copia." >&2; return 0;
        }
    else
        git -C "$dest" fetch --depth 1 "https://github.com/$repo.git" "$rev"
        git -C "$dest" checkout -q --detach FETCH_HEAD
    fi
}
checkout "$BASE/deps/SDL2" libsdl-org/SDL c98c4fbff6d8f3016a3ce6685bf8f43433c3efcc
export CACTUS_SDL2_PREFIX="$BASE/sdl2-a64"
# Para SDL, usa GCC del SDK y limita pkg-config a las bibliotecas de destino.
(
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBRARY_PATH CMAKE_PREFIX_PATH PKG_CONFIG_PATH
sysroot=$("$gcc" -print-sysroot)
test -f "$sysroot/usr/include/pthread.h"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig:$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig"
sdl_tc="$BASE/sdl-gcc14.cmake"
cat > "$sdl_tc" <<'CMAKE'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(sdk "$ENV{AARCH64_XTOOLS}")
set(CMAKE_SYSROOT "$ENV{PKG_CONFIG_SYSROOT_DIR}")
set(CMAKE_C_COMPILER "${sdk}/bin/aarch64-rpi3-linux-gnu-gcc")
set(CMAKE_CXX_COMPILER "${sdk}/bin/aarch64-rpi3-linux-gnu-g++")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CMAKE
cmake -S "$BASE/deps/SDL2" -B "$BASE/deps/build-sdl2-gcc14" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$sdl_tc" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$CACTUS_SDL2_PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_RPATH=OFF
cmake --build "$BASE/deps/build-sdl2-gcc14" --parallel 4
cmake --install "$BASE/deps/build-sdl2-gcc14"
)
lib="$CACTUS_SDL2_PREFIX/lib"
test -f "$CACTUS_SDL2_PREFIX/include/SDL2/SDL.h"
test -f "$lib/libSDL2-2.0.so.0"
if [[ ! -e "$lib/libSDL2-2.0.so" ]]; then ln -s libSDL2-2.0.so.0 "$lib/libSDL2-2.0.so"; fi
file -L "$lib/libSDL2-2.0.so"
LC_ALL=C readelf -h "$lib/libSDL2-2.0.so" | grep 'Machine:.*AArch64'

# platform/knulli/game busca los repositorios hermanos bajo fuentes/deps.
deps="$PORT/fuentes/deps"
mkdir -p "$deps"
checkout "$deps/glslang" KhronosGroup/glslang e1b562a8bed273a02f30b59b66a5d499793cede5
checkout "$deps/glslang/External/spirv-tools" KhronosGroup/SPIRV-Tools ef96ed763b43b59b33b31b362f09a02b729fa1c9
checkout "$deps/glslang/External/spirv-tools/external/spirv-headers" KhronosGroup/SPIRV-Headers 496543121ce6419f23d6fa5d7194ba66c36212d2
checkout "$deps/SPIRV-Cross" KhronosGroup/SPIRV-Cross 1a6169566c73d3da552748fc372fe2bbb856e46e
{
    printf 'export AARCH64_XTOOLS=%q\n' "$AARCH64_XTOOLS"
    printf 'export CACTUS_SDL2_PREFIX=%q\n' "$CACTUS_SDL2_PREFIX"
    printf 'export NEBULA_SDL2=%q\n' "$CACTUS_SDL2_PREFIX"
    printf 'export PATH=%q:"$PATH"\n' "$BASE/bin"
} > "$BASE/entorno.sh"
python3 "$PORT/cactus-porter.py" doctor
printf 'Listo. En la misma terminal ejecuta: source %q\n' "$BASE/entorno.sh"
