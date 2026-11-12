# cactus-porter

Plantilla en desarrollo para armar ports de Wii para Anbernic RG35XX H. El flujo previsto recompila el juego en el PC y genera el game.pak en la consola desde el disco del usuario.

## Preparar el PC

Desde la raíz del proyecto, en Ubuntu o Debian:

```bash
sudo apt install git cmake ninja-build g++ clang lld zlib1g-dev libbz2-dev liblzma-dev libzstd-dev
export AARCH64_XTOOLS=/ruta/x-tools/aarch64-rpi3-linux-gnu
export CACTUS_SDL2_PREFIX=/ruta/sdl2-aarch64
./cactus-porter.py doctor
./cactus-porter.py fetch
```

Sustituye las rutas por las de tu toolchain aarch64 con glibc ≤ 2.27 y tu SDL2 compilado para aarch64. La guía de referencia del fork es `docs/RG35XXH.md`. Repite los dos `export` al abrir una terminal nueva, salvo que ya estén en la configuración de tu shell.

## Preparar cada juego

```bash
./cactus-porter.py probe --iso "/ruta/TuJuego.iso"
./cactus-porter.py recomp --juego XXXX01 --map main.map
./cactus-porter.py build --juego XXXX01
./cactus-porter.py package --juego XXXX01
```

Usa tu propio disco. Sustituye `XXXX01` por el ID que indique `probe`. El perfil y el DOL se guardan en `trabajo/<ID>/`. Si careces del mapa del enlazador, omite `--map main.map`. La ruta prevista del paquete es `salida/<slug>.zip`.

## Primera ejecución prevista en la RG35XX H

Instala el ZIP del juego con PortMaster en KNULLI o muOS. Coloca tu disco en `ports/<slug>/gamedata/` o en `roms/wii/`. Al abrir el port, el lanzador llama al wrapper para crear `gamedata/game.pak` y luego arranca el host.

## Comprobar el cableado GX del fork

Ejecuta estas búsquedas desde la raíz de cactus-porter con el checkout en la rama rg35xxh-arm64:

```bash
git -C fuentes/cactus-galaxy branch --show-current
grep -nE 'add_library|add_executable' fuentes/cactus-galaxy/platform/knulli/game/CMakeLists.txt
grep -nE 'host_window_knulli|platform/sdl|sdl_host' fuentes/cactus-galaxy/platform/knulli/game/CMakeLists.txt
```

El target estático es `galaxy_gx_gles`. La prueba `nebula_gles_renderer_tests` añade al ejecutable `host_window_knulli.cpp` y `platform/sdl/display_clock.cpp`. El host completo de Nebula añade también `thread_sampler.cpp`. La configuración del fork requiere CMake 3.22 o posterior, glslang con SPIRV-Tools y SPIRV-Cross.

## Configurar y compilar el host por separado

Desde la raíz de cactus-porter, después de `recomp`, sustituye `TEST01` por el ID del perfil y `test01` por su slug. Usa las rutas reales del toolchain y de SDL2. La configuración requiere Ninja, Clang, LLD, un sysroot compatible con la consola y las dependencias GLES del fork. CMake puede compilar pequeñas sondas en esta fase.

```bash
export AARCH64_XTOOLS=/ruta/x-tools/aarch64-rpi3-linux-gnu
export CACTUS_SDL2_PREFIX=/ruta/sdl2-aarch64
id_juego=TEST01
slug=test01
build_dir="$PWD/trabajo/$id_juego/build-host"

cmake -S "$PWD/plantilla/host" -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/fuentes/cactus-galaxy/platform/knulli/aarch64-toolchain.cmake" \
  -DCACTUS_GALAXY_ROOT="$PWD/fuentes/cactus-galaxy" \
  -DDOLRECOMP_DIR="$PWD/fuentes/DolRecomp" \
  -DGENERATED_DIR="$PWD/trabajo/$id_juego/recomp/${id_juego}_generated" \
  -DCACTUS_SLUG="$slug" \
  -DCACTUS_SDL2_PREFIX="$CACTUS_SDL2_PREFIX"
```

Si la configuración termina correctamente, en la misma terminal compila el host y sus dependencias y comprueba la arquitectura:

```bash
cmake --build "$build_dir" --target "${slug}-host" && file "$build_dir/${slug}-host"
```

El resultado esperado de `file` es un ELF de 64 bits para AArch64. El arranque se comprueba en la RG35XX H.

## Estado de esta entrega

El host cuenta con su CMakeLists.txt y el puente GX. El comando build configura el host con las fuentes C generadas para el juego y el checkout de cactus-galaxy. Falta conectar el despacho de CPU en juego_glue.cpp y validar la compilación ARM64 con el fork y la ejecución en la consola.
