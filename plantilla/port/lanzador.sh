#!/bin/bash
# @TITULO@ (@ID@) — port generado por cactus-porter (DolRecomp + cactus-galaxy).
# El .pak lo hace el wrapper en la consola desde tu propio disco; este lanzador solo arranca.
# KNULLI puede invocar el lanzador con sh.
[ -n "${BASH_VERSION:-}" ] || exec /bin/bash "$0" "$@"
launcher_dir=$(cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
boot_log="$launcher_dir/@SLUG@-inicio.txt"
if ! ( : > "$boot_log" ) 2>/dev/null; then
  boot_log=$(mktemp "/tmp/@SLUG@-inicio.XXXXXX") || exit 1
fi
exec >>"$boot_log" 2>&1
printf 'Lanzador: %s\n' "$0"
directory="userdata"
controlfolder=
for pm in /opt/system/Tools/PortMaster /opt/tools/PortMaster "${XDG_DATA_HOME:-$HOME/.local/share}/PortMaster" /roms/ports/PortMaster /userdata/roms/ports/PortMaster; do
  if [ -r "$pm/control.txt" ]; then
    controlfolder=$pm
    break
  fi
done
if [ -n "$controlfolder" ]; then
  . "$controlfolder/control.txt"
  if [ -n "${CFW_NAME:-}" ] && [ -r "$controlfolder/mod_${CFW_NAME}.txt" ]; then
    . "$controlfolder/mod_${CFW_NAME}.txt"
  fi
  if declare -F get_controls >/dev/null; then
    get_controls
    echo "get_controls: $?"
  fi
  if [ -r "$controlfolder/gamecontrollerdb.txt" ]; then
    export NEBULA_GAMECONTROLLERDB="$controlfolder/gamecontrollerdb.txt"
  fi
fi
if [ -n "${sdl_controllerconfig:-}" ]; then
  export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
fi
echo "PortMaster: ${controlfolder:-sin detectar}; mando SDL: ${SDL_GAMECONTROLLERCONFIG:+configurado}; base: ${NEBULA_GAMECONTROLLERDB:-sin detectar}"
GAMEDIR=
for candidate in "/${directory#/}/ports/@SLUG@" "$launcher_dir/@SLUG@" \
    "$launcher_dir/../../ports/@SLUG@" "/userdata/ports/@SLUG@" "/userdata/roms/ports/@SLUG@"; do
  printf 'Buscando el host en %s\n' "$candidate"
  if [ -f "$candidate/@SLUG@-host" ]; then
    GAMEDIR=$(cd -- "$candidate" && pwd -P) || exit 1
    break
  fi
done
if [ -z "$GAMEDIR" ]; then
  echo "Falta @SLUG@-host en las carpetas anteriores."
  exit 1
fi
cd "$GAMEDIR" || exit 1
if ( : > "$GAMEDIR/log.txt" ); then
  printf 'Registro del juego: %s/log.txt\n' "$GAMEDIR"
  cat "$boot_log" > "$GAMEDIR/log.txt"
  exec >>"$GAMEDIR/log.txt" 2>&1
else
  echo "No se pudo abrir log.txt. El registro continúa aquí."
fi
for tool in nebula-pak cactus-galaxy-wrapper @SLUG@-host; do [ -f "$tool" ] && chmod +x "$tool"; done
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
