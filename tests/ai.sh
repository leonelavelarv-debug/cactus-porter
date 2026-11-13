#!/bin/sh
# Prueba PC del DMA de audio (ai_dma.cpp): ritmo por ciclos emulados, AIDINT al acabar cada bloque, registros
# nuevos al terminar el bloque en curso, orden R,L -> L,R y silencio con el DMA parado: ./tests/ai.sh
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
cat > harness.cpp <<'CPP'
#include "ai_dma.h"
#include <cassert>
#include <cstdio>
static uint8_t mem[0x10000];
static uint8_t* span(uint32_t a, size_t n) { a &= 0xFFFF; return a + n <= sizeof mem ? mem + a : nullptr; }
constexpr int64_t kBlock = 182250;  // ciclos de Broadway para 32 bytes a 32 kHz estéreo de 16 bits
int main() {
    for (int i = 0; i < 64; ++i) mem[0x1000 + i] = uint8_t(i);         // dos bloques en 0x1000
    for (int i = 0; i < 64; ++i) mem[0x2000 + i] = uint8_t(0x80 + i);  // y otros dos en 0x2000
    ai::Dma d;
    std::vector<uint8_t> out;
    assert(d.tick(kBlock, 32000, span, out) == 0 && out.size() == 32 && out[0] == 0 && out[31] == 0);  // parado
    out.clear();
    d.write_source_high(0);
    d.write_source_low(0x1000);
    d.write_control(0x8002);
    assert(d.cursor == 0x1000 && d.left == 2 && d.blocks_left() == 1);
    assert(d.tick(kBlock / 2, 32000, span, out) == 0 && out.empty());   // medio bloque: aún nada
    assert(d.tick(kBlock / 2, 32000, span, out) == 0 && out.size() == 32);
    assert(out[0] == 2 && out[1] == 3 && out[2] == 0 && out[3] == 1);     // R(0,1) L(2,3) -> L, R
    assert(d.left == 1 && d.cursor == 0x1020);
    // El juego cambia el origen mientras suena: entra al acabar el bloque en curso.
    d.write_source_low(0x2000);
    d.write_control(0x8002);
    assert(d.cursor == 0x1020 && d.left == 1);
    assert(d.tick(kBlock, 32000, span, out) == 1);                       // fin del bloque: AIDINT y recarga
    assert(out[32] == 0x22 && d.cursor == 0x2000 && d.left == 2);
    assert(d.tick(2 * kBlock, 32000, span, out) == 1 && out.size() == 128 && out[64] == 0x82);
    // A 48 kHz el mismo tiempo da 1,5 veces más bytes.
    std::vector<uint8_t> o48;
    ai::Dma e;
    e.write_control(0x8000 | 100);
    e.tick(kBlock * 2, 48000, span, o48);
    assert(o48.size() == 96);
    // Parado: silencio y sin interrupciones.
    d.write_control(0);
    out.clear();
    assert(d.tick(4 * kBlock, 32000, span, out) == 0 && out.size() == 128 && out[5] == 0);
    std::puts("ai: ok");
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/ai_dma.cpp" -o harness
./harness
