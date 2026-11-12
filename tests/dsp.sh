#!/bin/sh
# Prueba PC del HLE del DSP (buzones, arranque de microcódigo, protocolo AX, DMA de ARAM): ./tests/dsp.sh
# No necesita DolRecomp ni SDL: compila solo plantilla/host/dsp_hle.cpp con un host falso.
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
# Hash "Ector" (el de Dolphin) de las imágenes de prueba, calculado fuera de C++ para no comprobarse con uno mismo.
ector() { python3 -c '
import sys
h = 0
for b in bytes(range(int(sys.argv[1]), int(sys.argv[2]))):
    h ^= b
    h = ((h << 3) | (h >> 29)) & 0xFFFFFFFF
print(h)' "$1" "$2"; }
crc_ax=$(ector 0 32)
crc_new=$(ector 64 128)
cat > harness.cpp <<CPP
#include "dsp_hle.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static uint8_t mem[0x100000];
static bool line;
static unsigned irq_changes;
static uint32_t written_addr, written_len;
static uint8_t* span(uint32_t a, size_t n) { return (a < sizeof mem && n <= sizeof mem - a) ? mem + a : nullptr; }
static void irq(bool on) { if (on != line) ++irq_changes; line = on; }
static void written(uint32_t a, size_t n) { written_addr = a; written_len = uint32_t(n); }

static void send(uint32_t mail) { dsp::write(0x5000, uint16_t(mail >> 16)); dsp::write(0x5002, uint16_t(mail)); }
static bool pending() { return dsp::read(0x5004) & 0x8000; }
static uint32_t recv() {  // como DSPReadMailFromDSP: alto y luego bajo
    uint32_t hi = dsp::read(0x5004), lo = dsp::read(0x5006);
    return (hi << 16) | lo;
}
static uint16_t cr() { return dsp::read(0x500A); }
static void clear_dspint() { dsp::write(0x500A, 0x180); }  // w1c de DSPINT conservando su máscara

int main() {
    dsp::init({span, irq, written});
    assert(!pending() && dsp::current_ucode() == dsp::Ucode::None);
    send(0x12345678);                                  // sin microcódigo: se ignora, sin colgarse
    assert(!pending());

    // __OSInitAudioSystem: RES + HALT + INIT, y luego se baja INIT -> código de ARAM -> 0x80544348.
    dsp::write(0x500A, 0x08AD);
    assert(dsp::current_ucode() == dsp::Ucode::Rom);
    assert(pending() && recv() == 0x8071FEED && !pending());
    assert(!(cr() & 1) && (cr() & 0x800));             // RES no se queda puesto (el SDK espera a que baje)
    dsp::write(0x500A, 0x00A8);
    assert(dsp::current_ucode() == dsp::Ucode::InitAudio);
    assert(pending() && recv() == 0x80544348 && !pending());
    assert(!(cr() & 0x800));

    // DSPInit/__DSP_boot_task: RES -> 0x8071FEED y subida del microcódigo con parejas etiqueta/dato.
    dsp::write(0x500A, 0x0001);
    assert(recv() == 0x8071FEED);
    for (int i = 0; i < 32; ++i) mem[0x1000 + i] = uint8_t(i);
    send(0x80F3A001); assert(!pending()); send(0x00001000);
    send(0x80F3C002); send(0);
    send(0x80F3A002); send(32);
    send(0x80F3B002); send(0);
    assert(!pending());
    dsp::write(0x500A, 0x0100);                        // el SDK habilita la interrupción del DSP
    clear_dspint();
    send(0x80F3D001); send(0);
    assert(dsp::current_ucode() == dsp::Ucode::Ax);
    assert(dsp::ucode_hash() == ${crc_ax}u);
    assert((cr() & 0x80) && line);                     // DSP_INIT trae interrupción
    assert(pending() && recv() == 0xDCD10000);
    clear_dspint();
    assert(!line && !pending());

    // Reanudar y lista de comandos: cada una responde con su correo y una interrupción.
    send(0xCDD10000);
    assert(line && recv() == 0xDCD10001);
    clear_dspint();
    mem[0x2000] = 0x00; mem[0x2001] = 0x0F;            // CMD_END
    send(0xBABE0004); assert(!pending());              // primero el tamaño...
    send(0x00002000);                                  // ...y luego la dirección
    assert(pending() && recv() == 0xDCD10002 && dsp::frames_processed() == 1);
    clear_dspint();
    send(0xCDD10003); assert(!pending());              // CONTINUE no se contesta

    // Cola de correos: el segundo no levanta la interrupción hasta que la CPU lee el primero.
    send(0xCDD10000); send(0xCDD10000);
    assert(line);
    clear_dspint();
    assert(!line && pending());
    assert(recv() == 0xDCD10001);
    assert(line && (cr() & 0x80));                     // el segundo pasa al frente y avisa
    assert(recv() == 0xDCD10001 && !pending());
    clear_dspint();

    // Cambio de microcódigo: 0xCDD10001 y diez correos (MRAM<-DRAM: destino/tamaño/origen; IRAM: dirección/
    // tamaño/destino/vector; DRAM: dirección/tamaño/destino).
    for (int i = 0; i < 64; ++i) mem[0x3000 + i] = uint8_t(64 + i);
    send(0xCDD10001);
    const uint32_t p[10] = {0x4000, 0, 0, 0x3000, 64, 0, 0x10, 0, 0, 0};
    for (int i = 0; i < 10; ++i) { assert(!pending()); send(p[i]); }
    assert(dsp::ucode_hash() == ${crc_new}u);
    assert(pending() && recv() == 0xDCD10000);
    clear_dspint();
    // Volver al microcódigo anterior (el mismo hash): se recupera y contesta DSP_RESUME, no DSP_INIT.
    send(0xCDD10001);
    const uint32_t back[10] = {0x4000, 0, 0, 0x1000, 32, 0, 0x10, 0, 0, 0};
    for (int i = 0; i < 10; ++i) send(back[i]);
    assert(dsp::ucode_hash() == ${crc_ax}u && dsp::current_ucode() == dsp::Ucode::Ax);
    assert(line && recv() == 0xDCD10001 && !pending());
    clear_dspint();

    // 0xCDD10002 devuelve el DSP a su ROM.
    send(0xCDD10002);
    assert(dsp::current_ucode() == dsp::Ucode::Rom && recv() == 0x8071FEED);
    // RES descarta lo que hubiera sin leer.
    send(0x80F3A001); send(0x1000); send(0x80F3D001); send(0);
    dsp::write(0x500A, 1);
    assert(recv() == 0x8071FEED && !pending());

    // Interrupciones: AIDINT (DMA de audio) y su máscara.
    dsp::write(0x500A, 0x0010);
    assert(!line);
    dsp::raise_status(0x08);
    assert(line && (cr() & 0x08));
    dsp::write(0x500A, 0x0008 | 0x0010);               // limpiar AIDINT escribiendo 1
    assert(!line && !(cr() & 0x08));

    // DMA de ARAM: memoria -> ARAM -> otra zona de memoria, con ARINT al terminar.
    dsp::write(0x500A, 0x0040);
    for (int i = 0; i < 64; ++i) mem[0x5000 + i] = uint8_t(0xA0 + i);
    dsp::write(0x5020, 0); dsp::write(0x5022, 0x5000);
    dsp::write(0x5024, 0); dsp::write(0x5026, 0x0100);
    dsp::write(0x5028, 0); dsp::write(0x502A, 64);
    assert((cr() & 0x20) && line);
    dsp::write(0x500A, 0x0020 | 0x0040);
    assert(!line);
    dsp::write(0x5022, 0x6000);
    dsp::write(0x5024, 0); dsp::write(0x5026, 0x0100);
    dsp::write(0x5028, 0x8000); dsp::write(0x502A, 64);  // bit 15: ARAM -> memoria
    assert(std::memcmp(mem + 0x6000, mem + 0x5000, 64) == 0);
    assert(written_addr == (0x80000000u | 0x6000) && written_len == 64);
    // Fuera de ARAM se ignora sin tocar memoria.
    std::memset(mem + 0x7000, 0x11, 32);
    dsp::write(0x5022, 0x7000);
    dsp::write(0x5024, 0x0200); dsp::write(0x5026, 0);  // 32 MiB
    dsp::write(0x5028, 0x8000); dsp::write(0x502A, 32);
    assert(mem[0x7000] == 0x11 && mem[0x701F] == 0x11);

    std::puts("dsp: ok");
    return 0;
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/dsp_hle.cpp" "$host/zelda_hle.cpp" -o harness
./harness
