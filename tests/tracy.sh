#!/bin/sh
# Prueba PC de la instrumentación con Tracy: ./tests/tracy.sh /ruta/tracy [tracy-capture tracy-csvexport]
# 1) Compila con TRACY_ENABLE las unidades instrumentadas del host (DSP, AX, Zelda, REL, Bluetooth, runtime).
# 2) Si se le pasan tracy-capture y tracy-csvexport (de la misma versión), arranca un programa con perfil.h,
#    lo captura por 127.0.0.1 como hará "cactus-porter perf" con la consola, y comprueba el resumen.
set -eu
tr=$(realpath "$1")
raiz=$(cd "$(dirname "$0")/.." && pwd)
host="$raiz/plantilla/host"
dr="${DOLRECOMP_DIR:-}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
F="-std=c++20 -Wall -Wextra -Werror -DTRACY_ENABLE -DTRACY_ON_DEMAND -DTRACY_NO_FRAME_IMAGE -I$host -I$tr/public"
for f in dsp_hle ax_hle zelda_hle dev_bt wiimote; do
    g++ $F -c "$host/$f.cpp" -o "$f.o"
done
if [ -n "$dr" ]; then g++ $F -I"$dr/src" -c "$host/wii_rel.cpp" -o wii_rel.o; fi
cat > demo.cpp <<'CPP'
#include "perfil.h"
#include <chrono>
#include <thread>
int main() {
    CACTUS_HILO("juego (CPU, GX, audio)");
    CACTUS_GRAFICA_PORCENTAJE("CPU: % del retrace emulado");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int f = 0; std::chrono::steady_clock::now() < end; ++f) {
        { CACTUS_ZONA("CPU: código recompilado"); std::this_thread::sleep_for(std::chrono::milliseconds(9)); }
        { CACTUS_ZONA("GX: dibujar el cuadro"); std::this_thread::sleep_for(std::chrono::milliseconds(4)); }
        CACTUS_GRAFICA("CPU: % del retrace emulado", 80);
        CACTUS_GRAFICA("audio en cola (ms)", f % 50 ? 20 : 0);
        if (f % 60 == 0) CACTUS_MENSAJE("DSP: microcódigo %08X (AX de Wii)", 0x4CC52064u);
        CACTUS_CUADRO();
    }
}
CPP
g++ -std=c++20 -O2 -DTRACY_ENABLE -DTRACY_ON_DEMAND -DTRACY_NO_FRAME_IMAGE -I"$host" -I"$tr/public" demo.cpp \
    "$tr/public/TracyClient.cpp" -lpthread -ldl -o demo 2>/dev/null
echo "instrumentación compilada con Tracy"
if [ $# -ge 3 ]; then
    cap=$(realpath "$2"); csv=$(realpath "$3")
    ./demo & pid=$!
    sleep 1
    "$cap" -a 127.0.0.1 -o demo.tracy -s 2 -f >/dev/null
    wait "$pid"
    python3 - "$raiz/cactus-porter.py" "$csv" <<'PY'
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location("porter", sys.argv[1])
porter = importlib.util.module_from_spec(spec); spec.loader.exec_module(porter)
texto = porter.resumen_traza(Path(sys.argv[2]), Path("demo.tracy"))
print(texto)
assert "CPU: código recompilado" in texto and "GX: dibujar el cuadro" in texto
assert "Velocidad de la CPU emulada: 80 %" in texto
assert "cuadros con la cola de SDL vacía" in texto
assert "microcódigo 4CC52064" in texto
PY
    echo "OK: captura por red y resumen de cactus-porter perf"
fi
