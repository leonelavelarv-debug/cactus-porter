// dsp_hle.h — HLE del DSP de Wii: registro DSPCR, buzones CPU<->DSP, arranque de microcódigo,
// protocolo de AX (listas de comandos) y DMA de ARAM. No depende de SDL: se prueba en PC.
//
// Alcance: el DSP "contesta" al instante y con el protocolo correcto, así que el SDK (DSPInit/AXInit) termina
// su arranque y el juego sigue. El microcódigo Zelda (Mario Galaxy 1/2, Zelda TP...) sí mezcla audio
// (zelda_hle.cpp, portado de Dolphin). AX NO mezcla todavía: sus listas de comandos se aceptan y se descartan.
#pragma once
#include <cstddef>
#include <cstdint>

namespace dsp {

struct Host {
    uint8_t* (*span)(uint32_t addr, size_t len);     // memoria del invitado (dirección física o virtual)
    void (*irq_line)(bool asserted);                 // línea de interrupción del DSP hacia PI (bit 0x40)
    void (*mem_written)(uint32_t addr, size_t len);  // opcional: el DSP escribió en memoria principal
};

enum class Ucode { None, Rom, InitAudio, Ax, Zelda };

void init(const Host& host);   // engancha el host y deja el DSP como al encender
void refresh_irq();            // vuelve a evaluar la línea de interrupción (p. ej. tras escribir PI_CAUSE)

// Registros de 16 bits del bloque 0xCC005000: buzones (0x5000-0x5006), DSPCR (0x500A) y DMA de ARAM
// (0x5020-0x502A). Devuelven/ignoran lo que no es suyo.
bool handles(uint32_t reg);
uint16_t read(uint32_t reg);
void write(uint32_t reg, uint16_t value);

// Estado que otras partes del host levantan: AIDINT (0x08) al terminar un bloque del DMA de audio.
void raise_status(uint16_t bits);

Ucode current_ucode();
uint32_t ucode_hash();         // hash "Ector" (el de Dolphin) del microcódigo en marcha (0 si ninguno)
uint32_t frames_processed();   // listas de comandos AX aceptadas + cuadros que mezcló Zelda

}  // namespace dsp
