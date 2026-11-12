# cactus-porter

Plantilla en desarrollo para armar ports de Wii para Anbernic RG35XX H. El flujo previsto recompila el juego en el PC y genera el game.pak en la consola desde el disco del usuario.

## Preparar el PC

Desde la raíz del proyecto, en Ubuntu 24.04 (WSL2 sirve):

```bash
sudo apt install git cmake ninja-build g++ python3
./cactus-porter.py fetch                 # DolRecomp y tu fork cactus-galaxy (rama rg35xxh-arm64)
./instalar-entorno-nebula-v2.sh          # toolchain aarch64 (glibc 2.27), SDL2 aarch64, glslang y SPIRV-Cross
source ~/nebula-entorno/entorno.sh
./cactus-porter.py doctor
```

`fetch` necesita acceso a tu repositorio privado cactus-galaxy (`gh auth login` o una llave SSH). La sonda de `probe` se compila con el zlib/xz/zstd/bzip2 que trae el fork, así que no hacen falta paquetes `-dev` del sistema. Repite el `source` al abrir una terminal nueva.

## Preparar cada juego

```bash
./cactus-porter.py probe --iso "/ruta/TuJuego.iso"
./cactus-porter.py recomp --juego XXXX01 --map main.map
./cactus-porter.py build --juego XXXX01
./cactus-porter.py package --juego XXXX01
```

Usa tu propio disco. Sustituye `XXXX01` por el ID que indique `probe`. El lanzador `.sh` toma el nombre del juego sin `: / \ * ? " < > |`, que la SD en exFAT/FAT32 y Windows no admiten. El perfil y el DOL se guardan en `trabajo/<ID>/`. Si careces del mapa del enlazador, omite `--map main.map`. La ruta prevista del paquete es `salida/<slug>.zip`.

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

Hecho y probado en PC:

- **CPU** (`juego_glue.cpp`): el backend C de DolRecomp deja al host los `mfspr`/`mtspr` fuera de su tabla (HID0, HID4, DEC, SPRG, BAT, WPAR, DMA, L2CR…) y `icbi`; el host los ejecuta. El stub `__OSExceptionVector` que OSInit copia a 0x500, 0x800, 0x900… no está recompilado: el host hace su trabajo (guarda el contexto y salta a `__OSExceptionTable`). Las excepciones del juego (FPU no disponible, DSI, programa) van al manejador del OS en vez de cerrar el port. Timebase a 60,75 MHz, decrementador con interrupción y locked cache (0xE0000000 con su DMA).
- **Presupuesto**: 12,15 M ciclos de Broadway por retrace, en porciones de 2048 para que las interrupciones entren a tiempo, con un tope de 14 ms reales (`CACTUS_CPU_MS`). `CACTUS_TRACE=1` registra PC/LR cada segundo.
- **Arranque** (`wii_boot.cpp`): memoria baja del OS como la deja el apploader (valores de Dolphin), bi2.bin, la FST bajo el final de MEM1 (DVDOpen) y setting.txt cifrado según la región.
- **IOS**: respuestas diferidas en IPC, `/dev/di` con las órdenes que usa la biblioteca DVD, `/dev/es` (ID del título y carpeta de datos para NANDInit), `/dev/stm/eventhook` e `immediate` (apagar o volver al menú cierra el port). La NAND crea `title/00010000/<ID>/data`. Los dispositivos que falten se registran en log.txt como `[ios] sin dispositivo`.
- **MMIO**: el FIFO del CP ya no reenvía lo que llegó por el write-gather pipe (dibujaba dos veces) ni deja la interrupción del CP levantada para siempre. PI informa el botón RESET suelto; PE guarda sus habilitaciones; DSPCR y la interrupción AID del DMA de audio.
- **SDL**: el host usa la capa del fork (`galaxy::sdl::start` y `pump_events`), así que los mandos se abren con la base de PortMaster y se sale con MENU/SELECT + Start. Antes Start solo cerraba el juego.

Pendiente, por orden de bloqueo:

1. **DSP/AX**: los juegos con AX esperan correo del DSP en AXInit y se quedan ahí. Hace falta un HLE del microcódigo AX.
2. **WPAD**: no hay `/dev/usb/oh1/57e/305` (Bluetooth): el juego no ve Wiimotes. Hay que traducir el mando SDL a un Wiimote emulado.
3. **SYSCONF**: sin `/shared2/sys/SYSCONF` en la NAND, algunos juegos muestran un error de configuración.
4. Módulos REL, validación del GX en la consola y pruebas en hardware.

### Pruebas en PC

Con DolRecomp compilado (`trabajo/dolrecomp-build/dolrecomp`, lo deja `recomp`):

```bash
dr="$PWD/fuentes/DolRecomp"; exe="$PWD/trabajo/dolrecomp-build/dolrecomp"
./tests/dispatch.sh "$exe" "$dr"   # despacho, máscaras, vector externo y rfi
./tests/runtime.sh "$exe" "$dr"    # SPR por fallback, icbi, decrementador y vector del OS en el host
./tests/ios.sh "$dr"               # memoria de arranque, FST, IPC, ES, DI y eventhook diferido
```
