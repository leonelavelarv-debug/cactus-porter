// wii_gx.h — puente GX hacia el renderer GLES de cactus-galaxy (galaxy_gx_gles) y runtime del CPU.
#pragma once
#include <cstddef>
#include <cstdint>
extern "C" {
#include "cpu/cpu.h"
}
namespace cactus {
bool     gx_init(CPUState* cpu, int width, int height);  // regiones + servicios + initialize
void     gx_fifo_write(const uint8_t* data, uint32_t size); // bytes GX big-endian (write-gather pipe)
void     gx_frame(bool present, uint32_t xfb);           // un render_frame por retrace
void     gx_shutdown();
// Estado que el MMIO del host consulta (los callbacks del renderer llegan de otro hilo):
uint32_t gx_pi_cause_exchange(uint32_t clear_mask);      // lee/ack causas PI
void     gx_pi_raise(uint32_t bits);
uint16_t gx_pe_token_value();
bool     gx_quit_requested();

// Runtime del CPU sobre el C generado (juego_glue.cpp).
bool     game_init(CPUState* cpu, uint32_t entry);       // MSR, SPR y ganchos de DolRecomp
bool     game_frame(CPUState* cpu, const uint32_t& pi_mask);  // un retrace de trabajo del juego
// Locked cache (0xE0000000, 16 KiB): nullptr si [ea, ea+size) no cae dentro.
uint8_t* locked_cache(uint32_t ea, std::size_t size);
}
