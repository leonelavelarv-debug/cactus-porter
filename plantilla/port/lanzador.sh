#!/bin/bash
# @TITULO@ (@ID@) — port generado por cactus-porter (DolRecomp + cactus-galaxy).
# El .pak lo hace el wrapper en la consola desde tu propio disco; este lanzador solo arranca.
directory="userdata"
if [ -d "/opt/system/Tools/PortMaster" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster" ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "/roms/ports/PortMaster" ]; then controlfolder="/roms/ports/PortMaster"
else controlfolder="/userdata/roms/ports/PortMaster"; fi
[ -f "$controlfolder/control.txt" ] && . "$controlfolder/control.txt"
GAMEDIR="/$directory/ports/@SLUG@"
exec > >(tee "$GAMEDIR/log.txt") 2>&1
cd "$GAMEDIR" || exit 1
for tool in nebula-pak cactus-galaxy-wrapper @SLUG@-host; do [ -f "$tool" ] && chmod +x "$tool"; done
export SDL_GAMECONTROLLERCONFIG="${sdl_controllerconfig:-}"
export CACTUS_LANG="${CACTUS_LANG:-es}"
mkdir -p "$GAMEDIR/gamedata" "$GAMEDIR/nand"
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  ./cactus-galaxy-wrapper --smg "$GAMEDIR" --setup
fi
if [ ! -f "$GAMEDIR/gamedata/game.pak" ]; then
  echo "Falta gamedata/game.pak. Pon tu disco de @TITULO@ (@ID@: ISO, RVZ, WIA, WBFS, GCZ o CISO)"
  echo "en ports/@SLUG@/gamedata/ o en roms/wii/ y abre el juego otra vez: el wrapper lo convierte aqui mismo."
  sleep 8
  exit 1
fi
exec ./@SLUG@-host --pak "$GAMEDIR/gamedata/game.pak" --nand "$GAMEDIR/nand" "$@"
