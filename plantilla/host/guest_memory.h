// Traducción común para los búferes IPC y DMA del host.
#pragma once
#include <cstddef>
#include <cstdint>
extern "C" {
#include "cpu/cpu.h"
}

namespace wii {
inline std::uint8_t* resolve(CPUState* cpu, std::uint32_t addr, std::size_t len) {
    if (!cpu || (addr >> 30) == 1) return nullptr;
    const std::uint32_t physical = addr & 0x3fffffffu;
    const bool mem2 = physical >= 0x10000000u;
    const auto offset = mem2 ? physical - 0x10000000u : physical;
    const std::size_t size = mem2 ? cpu->mem2_size : cpu->ram_size;
    auto* base = mem2 ? cpu->mem2 : cpu->ram;
    if (!base || offset > size || len > size - offset) return nullptr;
    return base + offset;
}
}
