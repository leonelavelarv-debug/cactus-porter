#!/bin/sh
# Prueba PC del runtime del CPU: ./tests/runtime.sh /ruta/dolrecomp /ruta/DolRecomp
# SPR fuera de la tabla de DolRecomp (HID0, HID4, DEC), icbi, el decrementador y el vector del OS
# ejecutado por el host (stub __OSExceptionVector en RAM, como lo deja OSInit).
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
code = {
    # 0x80004000: bl sub; bucle: addi r3,r3,1; b bucle
    0x000: 0x48000101, 0x004: 0x38630001, 0x008: 0x4BFFFFFC,
    # 0x80004100 (sub): mfspr r7,HID0; icbi 0,r4; li r8,0x200; mtdec r8; mfspr r9,HID4; blr
    0x100: 0x7CF0FAA6, 0x104: 0x7C0027AC, 0x108: 0x39000200, 0x10C: 0x7D1603A6,
    0x110: 0x7D33FAA6, 0x114: 0x4E800020,
    # 0x80004200 (manejador del decrementador, al estilo de OSLoadContext):
    # addi r10,r10,1; lwz r5,0x198(r4); mtsrr0 r5; lwz r5,0x19C(r4); mtsrr1 r5;
    # lwz r3,0xC(r4); lwz r5,0x14(r4); lwz r4,0x10(r4); rfi
    0x200: 0x394A0001, 0x204: 0x80A40198, 0x208: 0x7CBA03A6, 0x20C: 0x80A4019C,
    0x210: 0x7CBB03A6, 0x214: 0x8064000C, 0x218: 0x80A40014, 0x21C: 0x80840010,
    0x220: 0x4C000064,
}
size = 0x224
dol = bytearray(0x100 + size)
struct.pack_into('>I', dol, 0x00, 0x100)          # texto 0: offset
struct.pack_into('>I', dol, 0x48, 0x80004000)     # dirección
struct.pack_into('>I', dol, 0x90, size)           # tamaño
struct.pack_into('>I', dol, 0xE0, 0x80004000)     # entrada
for off in range(0, size, 4):
    struct.pack_into('>I', dol, 0x100 + off, code.get(off, 0x60000000))
Path('proof.dol').write_bytes(dol)
PY
"$exe" --cpu broadway proof.dol TEST01 TEST01.c >/dev/null
cc -O2 -I"$dr/src" -c "$dr/src/cpu/cpu.c" TEST01_chunks/*.c
cat > harness.cpp <<'CPP'
#include "wii_gx.h"
#include <cassert>
#include <cstdio>
namespace cactus {
uint32_t gx_pi_cause_exchange(uint32_t) { return 0; }
}
int main() {
    CPUState cpu{};
    assert(cpu_init(&cpu));
    // Lo que OSInit deja: contexto actual, tabla de excepciones y el stub copiado al vector 0x900.
    mem_write32(&cpu, 0x800000C0, 0x00005000);
    mem_write32(&cpu, 0x800000D4, 0x80005000);
    mem_write32(&cpu, 0x80003000 + 8 * 4, 0x80004200);
    const uint32_t stub[] = {0x7C9043A6, 0x808000C0, 0x38600008, 0x808000D4,
                             0x3CA08000, 0x38A54300, 0x7CBA03A6, 0x4C000064};
    for (unsigned i = 0; i < 8; ++i) mem_write32(&cpu, 0x80000900 + 4 * i, stub[i]);
    assert(cactus::game_init(&cpu, 0x80004000));
    cpu.msr |= 0x8000;  // MSR[EE]
    for (int i = 0; i < 4 && !cpu.gpr[10]; ++i) assert(cactus::game_frame(&cpu, 0));
    std::printf("r3=%u r7=%08x r9=%08x r10=%u srr0=%08x msr=%08x tb=%llu\n", cpu.gpr[3], cpu.gpr[7],
                cpu.gpr[9], cpu.gpr[10], mem_read32(&cpu, 0x80005198), cpu.msr,
                (unsigned long long)cpu.timebase);
    assert(cpu.gpr[7] == 0x0011C464 && cpu.gpr[9] == 0x83900000);  // HID0 / HID4 por fallback
    assert(cpu.gpr[10] == 1);                                       // un decrementador entregado
    assert(mem_read32(&cpu, 0x8000500C) != 0);                      // r3 guardado en el contexto
    const uint32_t saved_pc = mem_read32(&cpu, 0x80005198);
    assert(saved_pc == 0x80004004 || saved_pc == 0x80004008);
    assert(cpu.msr & 0x8000);                                       // rfi devolvió MSR[EE]
    const uint32_t before = cpu.gpr[3];
    assert(cactus::game_frame(&cpu, 0));
    assert(cpu.gpr[3] > before && cpu.gpr[10] == 1);                // sigue el bucle, sin otro DEC
    assert(cactus::locked_cache(0xE0003FFC, 4) && !cactus::locked_cache(0xE0003FFE, 4));
    cpu_free(&cpu);
}
CPP
c++ -std=c++20 -O2 -DCACTUS_GENERATED_HEADER='"TEST01.h"' -I. -I"$host" \
    -I"$dr/src" "$host/juego_glue.cpp" harness.cpp ./*.o -lm -o check
./check
echo "OK: SPR por fallback, icbi, decrementador y vector del OS en el host"
