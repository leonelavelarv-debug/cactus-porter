// SPDX-License-Identifier: GPL-2.0-or-later
// ax_hle.h — mezcla de audio del microcódigo AX de Wii (el que usa casi todo el catálogo: Wii Sports, Mario
// Kart Wii, NSMBW...). Port de AXWii.cpp, AX.cpp, AXVoice.h y DSPAccelerator.cpp de Dolphin (Copyright 2008
// Dolphin Emulator Project, GPL-2.0-or-later): listas de comandos, voces ADPCM/PCM8/PCM16 por el acelerador,
// remuestreo lineal, envolvente, filtros paso bajo y biquad, mezcla a MAIN/AUXA/AUXB/AUXC y a los altavoces de
// los Wiimote, callbacks de AUX, compresor y salida estéreo al búfer del DMA de audio.
// Sin los coeficientes de la DROM del DSP (dsp_coef.bin) el remuestreo polifásico usa el lineal, como Dolphin.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace ax {

struct Env {
    uint8_t* (*span)(uint32_t addr, size_t len);      // memoria principal (física o virtual)
    void (*mem_written)(uint32_t addr, size_t len);   // opcional
    void (*push_mail)(uint32_t mail, bool irq);       // correo DSP -> CPU
};

class Wii {
 public:
    Wii(const Env& env, uint32_t ector_hash);
    ~Wii();
    // Ejecuta una lista de comandos (0xBABE0000|tamaño y su dirección). No manda DSP_YIELD: eso es de dsp_hle.
    void command_list(uint32_t addr, uint16_t size_words);
    uint32_t lists() const { return lists_; }
    uint32_t voices_processed() const { return voices_; }

    struct State;
 private:
    std::unique_ptr<State> st_;
    uint32_t lists_ = 0, voices_ = 0;
};

}  // namespace ax
