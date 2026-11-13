#!/bin/sh
# Prueba PC: ./tests/dispatch.sh /ruta/dolrecomp /ruta/DolRecomp
set -eu
exe=$(realpath "$1")
dr=$(realpath "$2")
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
python3 - <<'PY'
import struct
from pathlib import Path
# Bucle que incrementa r3. El vector externo reconoce la IRQ y ejecuta rfi.
dol = bytearray(0x124)
words = {0:0x100, 4:0x110, 0x48:0x80004000, 0x4c:0x80000500,
         0x90:12, 0x94:20, 0xe0:0x80004000,
         0x100:0x38630001, 0x104:0x4bfffffc, 0x108:0x4e800020,
         0x110:0x3c80cc00, 0x114:0x38a00100, 0x118:0x90a43000,
         0x11c:0x38c60001, 0x120:0x4c000064}
for offset, value in words.items():
    struct.pack_into('>I', dol, offset, value)
Path('proof.dol').write_bytes(dol)
PY
"$exe" --cpu broadway proof.dol TEST01 TEST01.c
cc -O2 -I"$dr/src" -c "$dr/src/cpu/cpu.c" TEST01_chunks/*.c
cat > harness.cpp <<'CPP'
#include "wii_gx.h"
#include <cassert>
static uint32_t pending;
namespace cactus {
uint32_t gx_pi_cause_exchange(uint32_t ack) {
    auto previous = pending;
    pending &= ~ack;
    return previous;
}
}
int main() {
    CPUState cpu{};
    assert(cpu_init(&cpu));
    cpu.external_write = [](CPUState*, uint32_t address, uint64_t value, uint8_t size) {
        assert(address == 0xcc003000 && size == 4);
        cactus::gx_pi_cause_exchange(uint32_t(value));
    };
    assert(cactus::game_init(&cpu, 0x80004000));
    pending = 0x100;
    assert(cactus::game_frame(&cpu, 0x100)); // MSR[EE] desactivado
    assert(cpu.gpr[3] && cpu.gpr[6] == 0);
    cpu.msr |= 0x8000;
    assert(cactus::game_frame(&cpu, 0)); // PI enmascarado
    assert(cpu.gpr[6] == 0);
    auto before = cpu.gpr[3];
    assert(cactus::game_frame(&cpu, 0x100));
    assert(!pending && cpu.srr0 == 0x80004000 && cpu.gpr[6] == 1);
    assert((cpu.msr & 0x8000) && cpu.gpr[3] > before);
    cpu_free(&cpu);
}
CPP
c++ -std=c++20 -O2 -DCACTUS_GENERATED_HEADER='"TEST01.h"' -I. -I"$host" \
    -I"$dr/src" "$host/juego_glue.cpp" "$host/wii_rel.cpp" -x c "$host/rel_vacio.c" -x none harness.cpp ./*.o -lm -o check
./check
echo "OK: despacho, máscaras, vector externo, rfi y continuación"
