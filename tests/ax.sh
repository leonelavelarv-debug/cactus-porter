#!/bin/sh
# Prueba PC de la mezcla de AX de Wii: ./tests/ax.sh
# Arranca un "AX" con el hash de una versión catalogada (0x4CC52064, PB de la disposición nueva) y le manda una
# lista: SETUP, PROCESS con dos voces (PCM16 en bucle a MAIN L y AUXA L; ADPCM sin bucle a MAIN R), MIX_AUXA con
# el callback de AUX, OUTPUT y END. Comprueba los correos, la salida R/L, lo que sube a AUXA y la PB devuelta.
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
cat > harness.cpp <<'CPP'
#include "dsp_hle.h"
#include "ax_structs.h"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
static std::vector<uint8_t> mem(0x01800000);
static bool line;
static uint8_t* span(uint32_t a, size_t n) {
    a &= 0x0FFFFFFFu;
    return (a < mem.size() && n <= mem.size() - a) ? mem.data() + a : nullptr;
}
static void irq(bool on) { line = on; }
static void send(uint32_t m) { dsp::write(0x5000, uint16_t(m >> 16)); dsp::write(0x5002, uint16_t(m)); }
static bool pending() { return dsp::read(0x5004) & 0x8000; }
static uint32_t recv() { uint32_t hi = dsp::read(0x5004), lo = dsp::read(0x5006); return (hi << 16) | lo; }
static void expect(uint32_t m) {
    assert(pending());
    const uint32_t got = recv();
    if (got != m) { std::fprintf(stderr, "esperaba %08X, llegó %08X\n", m, got); assert(false); }
}
static void be16(uint32_t a, uint16_t v) { mem[a] = uint8_t(v >> 8); mem[a + 1] = uint8_t(v); }
static uint16_t rd16(uint32_t a) { return uint16_t((mem[a] << 8) | mem[a + 1]); }
static int32_t rd32(uint32_t a) { return int32_t((uint32_t(rd16(a)) << 16) | rd16(a + 2)); }
static void be32(uint32_t a, uint32_t v) { be16(a, uint16_t(v >> 16)); be16(a + 2, uint16_t(v)); }
static void image_with_hash(uint32_t addr, uint32_t target) {
    for (int i = 1; i <= 32; ++i) mem[addr + i - 1] = uint8_t((target >> ((3 * (33 - i)) % 32)) & 1);
}
// PB en memoria con la disposición nueva: la estructura completa sin el campo "updates".
constexpr size_t kUb = offsetof(ax::AXPBWii, updates), kUe = kUb + sizeof(ax::PBUpdatesWii);
static void put_pb(uint32_t a, const ax::AXPBWii& pb) {
    const uint16_t* w = reinterpret_cast<const uint16_t*>(&pb);
    uint32_t o = a;
    for (size_t i = 0; i < sizeof(pb) / 2; ++i) {
        if (i * 2 >= kUb && i * 2 < kUe) continue;
        be16(o, w[i]); o += 2;
    }
}
static ax::AXPBWii get_pb(uint32_t a) {
    ax::AXPBWii pb{};
    uint16_t* w = reinterpret_cast<uint16_t*>(&pb);
    uint32_t o = a;
    for (size_t i = 0; i < sizeof(pb) / 2; ++i) {
        if (i * 2 >= kUb && i * 2 < kUe) continue;
        w[i] = rd16(o); o += 2;
    }
    return pb;
}

constexpr uint32_t PB1 = 0x10000, PB2 = 0x10400, INIT = 0x11000, LIST = 0x12000, PCM = 0x00200000,
                   ADPCM = 0x00100000, OUT_LR = 0x13000, OUT_S = 0x14000, AUX_W = 0x15000, AUX_R = 0x16000;

int main() {
    dsp::init({span, irq, nullptr});
    dsp::write(0x500A, 0x0001);
    assert(recv() == 0x8071FEED);
    dsp::write(0x500A, 0x0100);
    image_with_hash(0x1000, 0x4CC52064);
    send(0x80F3A001); send(0x1000);
    send(0x80F3A002); send(32);
    send(0x80F3D001); send(0);
    assert(dsp::current_ucode() == dsp::Ucode::Ax && dsp::ucode_hash() == 0x4CC52064);
    expect(0xDCD10000);

    // Voz 1: PCM16 constante 0x1000 (ganancia 0x800 = 1.0), 100 muestras en bucle, a MAIN L y AUXA L.
    for (int i = 0; i < 100; ++i) be16(PCM + 2 * i, 0x1000);
    ax::AXPBWii v1{};
    v1.next_pb_hi = uint16_t(PB2 >> 16); v1.next_pb_lo = uint16_t(PB2);
    v1.src_type = ax::SRCTYPE_LINEAR;
    v1.mixer_control_hi = 0x0001; v1.mixer_control_lo = 0x0001;    // AUXA L y MAIN L
    v1.running = 1;
    v1.mixer.main_left.volume = 0x8000;
    v1.mixer.auxA_left.volume = 0x8000;
    v1.vol_env.cur_volume = int16_t(0x8000);
    v1.audio_addr.looping = 1;
    v1.audio_addr.sample_format = ax::AUDIOFORMAT_PCM16;
    const uint32_t s1 = PCM / 2, e1 = s1 + 99;
    v1.audio_addr.loop_addr_hi = uint16_t(s1 >> 16); v1.audio_addr.loop_addr_lo = uint16_t(s1);
    v1.audio_addr.end_addr_hi = uint16_t(e1 >> 16); v1.audio_addr.end_addr_lo = uint16_t(e1);
    v1.audio_addr.cur_addr_hi = uint16_t(s1 >> 16); v1.audio_addr.cur_addr_lo = uint16_t(s1);
    v1.adpcm.gain = 0x0800;
    v1.src.ratio_hi = 1;
    put_pb(PB1, v1);

    // Voz 2: ADPCM, 4 marcos de 14 muestras (escala 2048, coeficientes 0), sin bucle, a MAIN R.
    const uint8_t frame[8] = {0x0B, 0x12, 0x34, 0x56, 0x70, 0x12, 0x34, 0x56};
    for (int f = 0; f < 4; ++f) std::memcpy(&mem[ADPCM + 8 * f], frame, 8);
    ax::AXPBWii v2{};
    v2.src_type = ax::SRCTYPE_LINEAR;
    v2.mixer_control_lo = 0x0002;                                   // MAIN R
    v2.running = 1;
    v2.mixer.main_right.volume = 0x8000;
    v2.vol_env.cur_volume = int16_t(0x8000);
    v2.audio_addr.sample_format = ax::AUDIOFORMAT_ADPCM;
    const uint32_t n0 = ADPCM * 2, e2 = n0 + 4 * 16 - 1, c2 = n0 + 2;
    v2.audio_addr.loop_addr_hi = uint16_t(n0 >> 16); v2.audio_addr.loop_addr_lo = uint16_t(n0);
    v2.audio_addr.end_addr_hi = uint16_t(e2 >> 16); v2.audio_addr.end_addr_lo = uint16_t(e2);
    v2.audio_addr.cur_addr_hi = uint16_t(c2 >> 16); v2.audio_addr.cur_addr_lo = uint16_t(c2);
    v2.adpcm.pred_scale = 0x0B;
    v2.src.ratio_hi = 1;
    put_pb(PB2, v2);

    // Lo que el callback de AUXA devuelve: +100 en L.
    for (int i = 0; i < 96; ++i) be32(AUX_R + 4 * i, 100);

    const uint16_t list[] = {
        0x0000, uint16_t(INIT >> 16), uint16_t(INIT),                                     // SETUP
        0x0004, uint16_t(PB1 >> 16), uint16_t(PB1),                                       // PROCESS
        0x0005, 0x8000, uint16_t(AUX_W >> 16), uint16_t(AUX_W), uint16_t(AUX_R >> 16), uint16_t(AUX_R),  // MIX_AUXA
        0x000B, 0x8000, uint16_t(OUT_S >> 16), uint16_t(OUT_S), uint16_t(OUT_LR >> 16), uint16_t(OUT_LR), // OUTPUT
        0x000E,                                                                           // END
    };
    for (size_t i = 0; i < sizeof list / 2; ++i) be16(LIST + 2 * i, list[i]);
    send(0xBABE0000u | uint32_t(sizeof list / 2));
    send(LIST);
    expect(0xDCD10004);   // OUTPUT: DSP_SYNC
    expect(0xDCD10002);   // fin de la lista: DSP_YIELD
    assert(!pending() && line);
    assert(dsp::frames_processed() == 1);

    // Muestras ADPCM esperadas: nibble con signo * 2048, 14 por marco; luego silencio.
    const int nib[14] = {1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6};
    for (int k = 0; k < 96; ++k) {
        const int16_t r = int16_t(rd16(OUT_LR + 4 * k)), l = int16_t(rd16(OUT_LR + 4 * k + 2));
        const int j = k - 3;  // el remuestreo lineal a 1.0 retrasa 3 muestras (historia de 4)
        const int want_r = (j >= 0 && j < 56) ? nib[j % 14] * 2048 : 0;
        const int want_l = (j >= 0 ? 0x1000 : 0) + 100;
        if (r != want_r || l != want_l) {
            std::fprintf(stderr, "muestra %d: R %d (quería %d), L %d (quería %d)\n", k, r, want_r, l, want_l);
            assert(false);
        }
        assert(rd32(AUX_W + 4 * k) == (j >= 0 ? 0x1000 : 0));        // AUXA L subido al callback
        assert(rd32(AUX_W + 4 * (96 + k)) == 0 && rd32(OUT_S + 4 * k) == 0);
    }
    // PB devueltas: la voz 1 avanzó 96 muestras; la 2 se acabó y se detuvo.
    const ax::AXPBWii p1 = get_pb(PB1), p2 = get_pb(PB2);
    assert(((uint32_t(p1.audio_addr.cur_addr_hi) << 16) | p1.audio_addr.cur_addr_lo) == s1 + 96);
    assert(p1.running == 1 && p1.src.last_samples[3] == 0x1000);
    assert(p2.running == 0);

    // Segunda lista: la voz 1 cruza el final y vuelve al principio del bucle.
    send(0xCDD10003);
    send(0xBABE0000u | uint32_t(sizeof list / 2));
    send(LIST);
    expect(0xDCD10004);
    expect(0xDCD10002);
    const ax::AXPBWii q1 = get_pb(PB1);
    const uint32_t cur = (uint32_t(q1.audio_addr.cur_addr_hi) << 16) | q1.audio_addr.cur_addr_lo;
    assert(cur == s1 + (192 - 100));
    assert(int16_t(rd16(OUT_LR + 4 * 50 + 2)) == 0x1000 + 100);
    std::puts("ax: ok");
    return 0;
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/dsp_hle.cpp" "$host/ax_hle.cpp" \
    "$host/zelda_hle.cpp" -o harness
./harness
