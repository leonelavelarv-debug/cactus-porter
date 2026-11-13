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

Usa tu propio disco. Si el juego trae módulos REL, `probe` los saca y `recomp` los recompila junto al DOL. Si hiciste `probe` antes de esta versión, repítelo. Sustituye `XXXX01` por el ID que indique `probe`. El lanzador `.sh` toma el nombre del juego sin `: / \ * ? " < > |`, que la SD en exFAT/FAT32 y Windows no admiten. El perfil y el DOL se guardan en `trabajo/<ID>/`. Si careces del mapa del enlazador, omite `--map main.map`. La ruta prevista del paquete es `salida/<slug>.zip`.

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
- **WPAD** (`dev_bt.cpp`, `wiimote.cpp`, `sysconf.cpp`): `/dev/usb/oh1/57e/305` es un adaptador Bluetooth emulado (HCI por los ioctlv de USB, L2CAP en los PSM 0x11/0x13) con un Wiimote + Nunchuk virtual (EEPROM y calibración, extensión, IR, informes 0x30-0x37 y 0x3D, vibración, LEDs). Pasados unos 2 s tras habilitar el escaneo, el Wiimote «pide conectarse». El host crea `shared2/sys/SYSCONF` con el Wiimote registrado. Mapeo: stick izq. = Nunchuk, cruz = cruz, sur = A, este/RT = B, oeste = C, LB/LT = Z, RB = agitar, norte = 1, captura = 2, Start/Back/Home = +/−/Home, stick der. = puntero IR.
- **DSP** (`dsp_hle.cpp`): DSPCR con sus interrupciones, buzones CPU↔DSP en cola, arranque del microcódigo (ROM `0x8071FEED`, código de `__OSInitAudioSystem` `0x80544348`, subida por parejas `0x80F3…`), protocolo AX (`DSP_INIT`, `RESUME`, `YIELD` por lista de comandos), cambio de microcódigo con los diez correos de Dolphin (y vuelta al anterior con `DSP_RESUME`) y DMA de ARAM real (16 MiB, con ARINT). El microcódigo se identifica con el hash «Ector» de Dolphin: la familia Zelda va a `zelda_hle.cpp` y el resto a AX de Wii. `CACTUS_TRACE_DSP=1` registra todos los correos y el hash; si la CPU espera un correo que nunca llega, avisa por stderr.
- **AX de Wii** (`ax_hle.cpp`, portado de `AXWii.cpp`, `AX.cpp`, `AXVoice.h` y `DSPAccelerator.cpp` de Dolphin, GPL-2.0-or-later): el microcódigo de casi todo el catálogo. Listas de comandos nuevas y antiguas (SETUP, ADD/SUB_TO_LR, PROCESS, MIX_AUXA/B/C con su callback, UPL_AUX*_MIX_LRSC, COMPRESSOR, OUTPUT/OUTPUT_DPL2 con `DSP_SYNC`, WM_OUTPUT, END), las tres disposiciones de la PB, voces ADPCM/PCM8/PCM16 por el acelerador (con bucles, streams y fin de voz), remuestreo lineal, envolvente, paso bajo y biquad, y mezcla a MAIN/AUXA/AUXB/AUXC y a los altavoces de los Wiimote. La salida estéreo va al búfer que da el juego, que la manda al DMA de audio. Sin la DROM del DSP no hay remuestreo polifásico: se usa el lineal, como Dolphin sin `dsp_coef.bin`. Versiones de AX que Dolphin no cataloga: se avisa por stderr y se usa la PB más reciente. En PC, 64 voces ADPCM cuestan ~140 µs por lista de 3 ms (5 % de un núcleo).
- **Módulos REL** (`wii_rel.cpp`): `probe` saca los `.rel` del disco (sueltos, Yaz0 o LZ77/LZ11) y `recomp` los recompila todos juntos con DolRecomp desde 0x81800000. Ese hueco de 8 MiB está sobre los 24 MiB de MEM1 y a menos de 32 MiB del DOL, así que los `bl` llegan. El host da a MEM1 32 MiB y el juego sigue viendo 24. Cuando el juego salta a un REL que enlazó en su heap (su prolog, un puntero a función), el host monta una copia limpia leída del game.pak en la dirección compilada y la reubica como DolRecomp (DOL, el propio módulo y los demás REL). Desvía ese salto y los siguientes al código recompilado, sin buscar `OSLink` dentro del juego. Si el juego recarga el módulo, se monta otra vez. Límites: 8 MiB de REL en total (Brawl no cabe) y los REL dentro de archivos `.arc` (Zelda TP) no se extraen.
- **Microcódigo Zelda** (`zelda_hle.cpp`, portado de `Zelda.cpp` de Dolphin, GPL-2.0-or-later): Super Mario Galaxy 1 y 2 (`0xD643001F`), Zelda TP Wii, Pikmin NPC, DK Jungle Beat Wii y los de GameCube (Wind Waker, Sunshine, Luigi's Mansion…). Protocolo completo (órdenes 01/02/0C/0D/0E, sincronía por cada 16 voces o por cuadro, protocolo «light») y mezclador real: VPB, PCM8/PCM16/AFC de 2 y 4 bits, ondas y patrones, resampleo con la tabla del juego, filtros paso bajo y biquad, reverb, posición Dolby y volumen de salida. Escribe L/R (80 muestras por cuadro) donde lo pide el juego, que los pasa al DMA de audio. En PC, 64 voces AFC cuestan ~60 µs por cuadro (2,5 % de un núcleo).
- **SDL**: el host usa la capa del fork (`galaxy::sdl::start` y `pump_events`), así que los mandos se abren con la base de PortMaster y se sale con MENU/SELECT + Start. Antes Start solo cerraba el juego.

Pendiente, por orden de bloqueo:

1. **Pruebas en la consola** (ver «Plan de prueba en hardware»): nada de lo de arriba se ha corrido todavía en la RG35XX H ni con un disco real.
2. **WPAD, pruebas reales**: el adaptador Bluetooth emulado existe (ver arriba) pero solo se ha probado contra una pila HCI/L2CAP escrita aquí (`tests/wpad.sh`), no contra el SDK ni un juego. Sin verificar: orden de bytes de la MAC y el tamaño exacto de `BT.DINF` en SYSCONF, y la secuencia de conexión que espera WUD.
3. **Validación del GX** del fork con juegos que no sean Galaxy (funciones del TEV o copias de EFB que Galaxy no usa).
4. **REL**: archivos `.arc`/U8 con REL dentro, juegos con más de 8 MiB de REL (habría que agrupar los que no conviven) y REL cargados con `OSLinkFixed` que liberan su tabla de reubicación (aquí no importa: el host usa su propia copia).
5. **Audio**: remuestreo polifásico (necesita la DROM del DSP), retardo inicial (ITD) y Dolby Pro Logic II, que tampoco tiene Dolphin. La respuesta de AX llega al instante: algún juego que espere un retraso (en Dolphin, The Clone Wars) podría colgarse.

### Plan de prueba en hardware

En la consola, por SSH, con el port instalado y el disco en `ports/<slug>/gamedata/` (la primera vez el lanzador hace el `game.pak`):

```bash
CACTUS_TRACE=1 CACTUS_TRACE_DSP=1 "/userdata/roms/ports/<Título del juego>.sh"
```

El lanzador guarda todo en `ports/<slug>/log.txt`. Qué mirar, en este orden:

1. `Inicio del DOL` y luego `[cpu] retrace …` cada segundo con el PC cambiando: la CPU corre. Si sale `Sin bloque recompilado para PC=…`, copia esa línea y la anterior.
2. `[dsp] microcódigo XXXXXXXX: Zelda` o `… AX de Wii`: el audio arrancó. Con AX, que no salga `sin catalogar`; si sale, apunta el número.
3. `[bt] el Wiimote pide conectarse` a los pocos segundos y, después, que el juego responda a los botones.
4. Con REL: `[rel] N módulos recompilados listos` al empezar y `[rel] módulo … se ejecuta recompilado` al cargar cada uno. `no está recompilado` o `no se pudo reubicar` son fallos.
5. Imagen: que se vea algo distinto de negro y que el log no se llene de `[gx]`. Una captura con el fallo vale más que la descripción.

Envía el log completo (o sus últimas 200 líneas) junto con el ID del juego.

### Pruebas en PC

Con DolRecomp compilado (`trabajo/dolrecomp-build/dolrecomp`, lo deja `recomp`):

```bash
dr="$PWD/fuentes/DolRecomp"; exe="$PWD/trabajo/dolrecomp-build/dolrecomp"
./tests/dispatch.sh "$exe" "$dr"   # despacho, máscaras, vector externo y rfi
./tests/runtime.sh "$exe" "$dr"    # SPR por fallback, icbi, decrementador y vector del OS en el host
./tests/ios.sh "$dr"               # memoria de arranque, FST, IPC, ES, DI y eventhook diferido
./tests/wpad.sh                    # pila Bluetooth de prueba: HCI, L2CAP, HID, Nunchuk y SYSCONF (sin DolRecomp)
./tests/dsp.sh                     # DSPCR, buzones, arranque de microcódigo, protocolo AX, cambio de microcódigo y DMA de ARAM
./tests/ax.sh                      # AX de Wii: lista con voces PCM16 en bucle y ADPCM, AUXA con callback, salida R/L y PB
./tests/zelda.sh                   # microcódigo Zelda (Galaxy): órdenes, sincronía, voces PCM/AFC, búferes L/R y VPB
./tests/rel.sh "$exe" "$dr"        # REL recompilados: DOL + dos módulos (Yaz0 y LZ11), importaciones entre ellos y recarga
```
