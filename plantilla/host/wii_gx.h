// wii_gx.h — puente GX hacia el renderer GLES de cactus-galaxy (galaxy_gx_gles).
#pragma once
#include <cstdint>
extern "C" {
#include "cpu/cpu.h"
}
namespace cactus {
bool     gx_init(CPUState* cpu, int width, int height);  // regiones + servicios + initialize
void     gx_fifo_write(const uint8_t* data, uint32_t size); // bytes GX big-endian (pipe + FIFO)
void     gx_frame(bool present, uint32_t xfb);           // un render_frame por retrace
void     gx_shutdown();
// Estado que el MMIO del host consulta (los callbacks del renderer llegan de otro hilo):
uint32_t gx_pi_cause_exchange(uint32_t clear_mask);      // lee/ack causas PI
void     gx_pi_raise(uint32_t bits);
uint16_t gx_pe_token_value();
bool     gx_quit_requested();
// Ejecución del C generado (sigue siendo por juego, ver juego_glue.cpp):
void     game_init(CPUState* cpu, uint32_t entry);
void     game_frame(CPUState* cpu);
}
