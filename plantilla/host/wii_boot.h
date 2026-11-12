// Estado de arranque que el IPL, IOS y el apploader dejan en la RAM antes de saltar a main.dol.
#pragma once
#include <cstdint>
#include <string>
#include "guest_memory.h"
#include "wii_dvd.h"

namespace wii {
struct BootInfo {
    std::string game_id;  // 6 caracteres de boot.bin
    uint64_t title_id = 0;  // 00010000 + las 4 primeras letras del ID
    bool pal = false;
};
// Memoria baja (globals del OS), bi2.bin, la FST bajo el final de MEM1 y setting.txt.
bool setup_boot_memory(CPUState* cpu, const PakDisc& pak, BootInfo& info);
}
