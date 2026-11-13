#!/bin/sh
# Prueba PC del HLE del microcódigo Zelda (protocolo de Mario Galaxy 1/2): ./tests/zelda.sh
# Arranca el microcódigo desde la ROM con una imagen cuyo hash Ector es el de Galaxy (0xD643001F), lo prepara
# (órdenes 0E y 01), mezcla dos voces (PCM16 desde memoria y AFC de 4 bits) en uno y en dos cuadros, y comprueba
# los correos, los búferes L/R, la VPB que el DSP devuelve y el cambio de microcódigo de ida y vuelta.
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
cat > harness.cpp <<'CPP'
#include "dsp_hle.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static std::vector<uint8_t> mem(0x01800000);
static bool line;
static uint32_t writes;
static uint8_t* span(uint32_t a, size_t n) {
    a &= 0x0FFFFFFFu;
    return (a < mem.size() && n <= mem.size() - a) ? mem.data() + a : nullptr;
}
static void irq(bool on) { line = on; }
static void written(uint32_t a, size_t) { assert(a & 0x80000000u); ++writes; }

static void send(uint32_t m) { dsp::write(0x5000, uint16_t(m >> 16)); dsp::write(0x5002, uint16_t(m)); }
static bool pending() { return dsp::read(0x5004) & 0x8000; }
static uint32_t recv() { uint32_t hi = dsp::read(0x5004), lo = dsp::read(0x5006); return (hi << 16) | lo; }
static void clear_dspint() { dsp::write(0x500A, 0x180); }
static void be16(uint32_t a, uint16_t v) { mem[a] = uint8_t(v >> 8); mem[a + 1] = uint8_t(v); }
static uint16_t rd16(uint32_t a) { return uint16_t((mem[a] << 8) | mem[a + 1]); }
// Orden del microcódigo: cabecera con el número de correos y luego los correos.
static void command(std::vector<uint32_t> mails) {
    send(uint32_t(mails.size()));
    for (uint32_t m : mails) send(m);
}
static void expect(uint32_t m) {
    assert(pending());
    const uint32_t got = recv();
    if (got != m) { std::fprintf(stderr, "esperaba %08X, llegó %08X\n", m, got); assert(false); }
}

// Imagen de 32 bytes con el hash Ector pedido: cada byte i (0 o 1) pone un bit distinto (3 es primo con 32).
static void image_with_hash(uint32_t addr, uint32_t target) {
    for (int i = 1; i <= 32; ++i) mem[addr + i - 1] = uint8_t((target >> ((3 * (33 - i)) % 32)) & 1);
}

// VPB (palabras de 16 bits): campos que usa la prueba.
constexpr uint32_t VPB = 0x10000, TABLES = 0x20000, AFC_COEFS = 0x21000, PCM = 0x30000, ARAM = 0x40000;
constexpr uint32_t LBUF = 0x50000, RBUF = 0x51000;
static void vpb16(int voice, int word, uint16_t v) { be16(VPB + voice * 0x180 + word * 2, v); }
static uint16_t vpb_rd(int voice, int word) { return rd16(VPB + voice * 0x180 + word * 2); }

int main() {
    dsp::init({span, irq, written});
    dsp::write(0x500A, 0x0001);
    assert(recv() == 0x8071FEED);
    dsp::write(0x500A, 0x0100);

    // Arranque desde la ROM con la "imagen" de Galaxy.
    image_with_hash(0x1000, 0xD643001F);
    send(0x80F3A001); send(0x1000);
    send(0x80F3A002); send(32);
    send(0x80F3D001); send(0);
    assert(dsp::current_ucode() == dsp::Ucode::Zelda && dsp::ucode_hash() == 0xD643001F);
    assert(line);
    expect(0xDCD10000);
    expect(0xF3551111);
    clear_dspint();

    // 0E: base de la "ARAM" (Wii: memoria principal).
    command({0x8E000000, ARAM});
    assert(line);
    expect(0xDCD10004); expect(0xF3558E00);
    clear_dspint();

    // Tablas de la orden 01: coeficientes de resampleo (los 4 de cada grupo: 0, 0, 0, 0x7FFF -> toma la
    // cuarta muestra), patrones y senos a cero; coeficientes AFC a cero (solo cuenta el delta).
    for (int g = 0; g < 0x40; ++g) be16(TABLES + (g * 4 + 3) * 2, 0x7FFF);

    // Voz 0: PCM16 desde memoria, constante 0x4000, 0x200 muestras, a delante-izquierda con volumen fijo.
    for (int i = 0; i < 0x200; ++i) be16(PCM + i * 2, 0x4000);
    vpb16(0, 0x00, 1);                  // enabled
    vpb16(0, 0x02, 0x1000);             // ratio 1.0
    vpb16(0, 0x08, 0x0D00); vpb16(0, 0x09, 0x7FFF); vpb16(0, 0x0A, 0x7FFF);  // canal 0: id, destino, actual
    vpb16(0, 0x3A, 0); vpb16(0, 0x3B, 0x200);  // longitud restante
    vpb16(0, 0x80, 33);                 // PCM16_FROM_MRAM
    vpb16(0, 0x8A, 0x200);              // inicio del bucle (posición) = fin
    vpb16(0, 0x8C, uint16_t(PCM >> 16)); vpb16(0, 0x8D, uint16_t(PCM));

    // Voz 1: AFC de 4 bits (fuente 9, 9 bytes por bloque) en la "ARAM", a delante-derecha.
    // Cabecera 0xB0: delta 1<<11 e índice 0. Nibbles 0..15 -> muestras n*2048 (con signo).
    for (int b = 0; b < 8; ++b) {
        mem[ARAM + b * 9] = 0xB0;
        for (int i = 0; i < 8; ++i) mem[ARAM + b * 9 + 1 + i] = uint8_t(((2 * i) << 4) | (2 * i + 1));
    }
    vpb16(1, 0x00, 1);
    vpb16(1, 0x02, 0x1000);
    vpb16(1, 0x04, 1);                  // reset_vpb: arranca desde el principio
    vpb16(1, 0x08, 0x0D60); vpb16(1, 0x09, 0x7FFF); vpb16(1, 0x0A, 0x7FFF);
    vpb16(1, 0x80, 9);
    vpb16(1, 0x8B, 0x80);               // 128 muestras (AFC usa los 32 bits; PCM desde memoria, solo la parte alta)
    vpb16(1, 0x8C, 0); vpb16(1, 0x8D, 0);

    command({0x81000002, VPB, TABLES, AFC_COEFS, 0});
    expect(0xDCD10004); expect(0xF3558100);
    clear_dspint();

    // 02: un cuadro a volumen 1.0 (4.12), con los dos argumentos extra de COMBINED_CMD_0D.
    command({0x82011000, LBUF, RBUF, 0, 0});
    assert(!pending());                 // espera la sincronía de voces
    send(0x00000000);                   // "viene un correo de sincronía"
    send(0x0000C000);                   // bloque 0: voces 0 y 1
    expect(0xDCD10004); expect(0xF355FF00);
    expect(0xDCD10005);
    assert(!pending());
    clear_dspint();
    send(0xCDD10003);                   // CONTINUE
    assert(!pending());
    assert(dsp::frames_processed() == 1);

    // Esperado: resampleo = floor(x * 0xFFFE / 0x10000) de la muestra anterior; mezcla con 0x7FFF -> >> 16.
    auto chain = [](int x) { int r = (2 * 0x7FFF * x) >> 16; return int16_t((0x7FFF * r) >> 16); };
    assert(int16_t(rd16(LBUF)) == 0);  // la primera sale de la historia (ceros)
    for (int k = 1; k < 0x50; ++k) assert(int16_t(rd16(LBUF + 2 * k)) == chain(0x4000));
    for (int k = 1; k < 0x50; ++k) {
        const int n = (k - 1) % 16;
        const int sample = int16_t(n << 12) >> 1;
        if (int16_t(rd16(RBUF + 2 * k)) != chain(sample)) {
            std::fprintf(stderr, "R[%d] = %d, esperaba %d\n", k, int16_t(rd16(RBUF + 2 * k)), chain(sample));
            assert(false);
        }
    }
    // La VPB vuelve con la posición avanzada (voz 0) y el reinicio consumido (voz 1).
    assert(vpb_rd(0, 0x34) == 0x50 && vpb_rd(0, 0x3B) == 0x200 - 0x50 && !vpb_rd(0, 0x01));
    assert(vpb_rd(1, 0x04) == 0 && vpb_rd(1, 0x3B) == 0x80 - 0x50);
    assert(writes > 0);

    // Dos cuadros seguidos: una sincronía por cuadro; el segundo va 0xA0 bytes más allá.
    std::memset(&mem[LBUF], 0, 0x200);
    command({0x82021000, LBUF, RBUF, 0, 0});
    send(0); send(0x00008000);          // solo la voz 0
    expect(0xDCD10004); expect(0xF355FF00);
    assert(!pending());
    send(0); send(0x00008000);
    expect(0xDCD10004); expect(0xF355FF01);
    expect(0xDCD10005);
    clear_dspint();
    send(0xCDD10003);
    assert(int16_t(rd16(LBUF + 0xA0 + 2)) == chain(0x4000));
    assert(vpb_rd(1, 0x3B) == 0x80 - 0x50);  // la voz 1 no estaba en la sincronía: no avanzó
    assert(dsp::frames_processed() == 3);

    // Fin de una voz sin bucle: a la AFC le quedan 0x30 muestras, el cuadro pide 0x50 -> se rellena con
    // ceros y la VPB vuelve con "done".
    std::memset(&mem[RBUF], 0x77, 0xA0);
    command({0x82011000, LBUF, RBUF, 0, 0});
    send(0); send(0x00004000);          // voz 1
    expect(0xDCD10004); expect(0xF355FF00); expect(0xDCD10005);
    clear_dspint();
    send(0xCDD10003);
    assert(vpb_rd(1, 0x01) == 1);       // (como en Dolphin, la longitud restante no se pone a 0)
    assert(rd16(RBUF + 2 * 0x4F) == 0);  // el final del cuadro, ya en silencio
    // Una voz terminada ya no suena.
    command({0x82011000, LBUF, RBUF, 0, 0});
    send(0); send(0x00004000);
    expect(0xDCD10004); expect(0xF355FF00); expect(0xDCD10005);
    clear_dspint();
    send(0xCDD10003);
    for (int k = 0; k < 0x50; ++k) assert(rd16(RBUF + 2 * k) == 0);

    // Cambio de microcódigo (p. ej. a otro de AX) y vuelta: Zelda se recupera con DSP_RESUME.
    for (int i = 0; i < 32; ++i) mem[0x2000 + i] = uint8_t(0x55 + i);
    send(0xCDD10001);
    const uint32_t to_ax[10] = {0, 0, 0, 0x2000, 32, 0, 0x10, 0, 0, 0};
    for (uint32_t m : to_ax) send(m);
    assert(dsp::current_ucode() == dsp::Ucode::Ax);
    expect(0xDCD10000);
    clear_dspint();
    send(0xCDD10001);
    const uint32_t back[10] = {0, 0, 0, 0x1000, 32, 0, 0x10, 0, 0, 0};
    for (uint32_t m : back) send(m);
    assert(dsp::current_ucode() == dsp::Ucode::Zelda && dsp::ucode_hash() == 0xD643001F);
    expect(0xDCD10001);
    assert(!pending());
    clear_dspint();
    assert(dsp::frames_processed() == 5);  // el estado de Zelda sigue ahí
    command({0x8F000000});                 // NOP
    expect(0xDCD10004); expect(0xF3558F00);

    std::puts("zelda: ok");
    return 0;
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/dsp_hle.cpp" "$host/ax_hle.cpp" "$host/zelda_hle.cpp" -o harness
./harness
