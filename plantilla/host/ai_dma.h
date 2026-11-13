// ai_dma.h — DMA de audio del DSP hacia el AI (0xCC005030-0xCC00503A), al ritmo del tiempo emulado.
// Como UpdateAudioDMA de Dolphin: cada 32 bytes (8 muestras estéreo) avanza un bloque; al acabar la longitud
// programada se recarga desde los registros y salta AIDINT para que el juego prepare el bloque siguiente.
// En memoria las muestras van en big endian y en orden R, L; la salida queda en L, R (lo que espera SDL).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ai {

struct Dma {
    uint32_t source = 0, cursor = 0, control = 0, left = 0;
    double acc = 0;  // bytes pendientes de emitir

    void write_source_high(uint16_t v) { source = (uint32_t(v & 0x1fff) << 16) | (source & 0xffff); }
    void write_source_low(uint16_t v) { source = (source & 0xffff0000u) | (v & 0xffe0); }
    // 0x5036: al encenderse carga origen y longitud; en marcha, lo nuevo entra al acabar el bloque actual.
    void write_control(uint16_t v);
    uint16_t blocks_left() const { return uint16_t(left ? left - 1 : 0); }  // 0x503A

    // Avanza `cycles` ciclos de Broadway (729 MHz) a `rate` Hz. Añade a `out` lo que sonaría (silencio si el
    // DMA está parado) y devuelve cuántas veces hay que levantar AIDINT.
    unsigned tick(int64_t cycles, uint32_t rate, uint8_t* (*span)(uint32_t, size_t), std::vector<uint8_t>& out);
};

}  // namespace ai
