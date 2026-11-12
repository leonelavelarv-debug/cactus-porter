// Arranque emulado (como el BS2/apploader HLE de Dolphin, Core/Boot/Boot_BS2Emu.cpp): el OS de Revolution lee
// estos valores en OSInit, DVDInit (FST), VIInit (modo de vídeo) y SCInit (setting.txt).
#include "wii_boot.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace wii {
namespace {
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void w32(CPUState* cpu, uint32_t a, uint32_t v) {
    if (uint8_t* p = resolve(cpu, a, 4)) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
}
void w16(CPUState* cpu, uint32_t a, uint16_t v) {
    if (uint8_t* p = resolve(cpu, a, 2)) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
}
void w8(CPUState* cpu, uint32_t a, uint8_t v) {
    if (uint8_t* p = resolve(cpu, a, 1)) *p = v;
}
bool copy_from_pak(CPUState* cpu, const PakDisc& pak, uint64_t offset, uint32_t size, uint32_t addr) {
    uint8_t* dst = resolve(cpu, addr, size);
    return dst && pak.read(offset, size, dst);
}

bool is_pal(char region) {
    return std::strchr("PDFSIXYHUVLM", region) != nullptr && region != '\0';
}

// setting.txt (0x80003800): texto plano cifrado con el XOR rotativo del IPL (semilla 0x73B5DBFA).
void write_setting_txt(CPUState* cpu, char region, bool pal) {
    const char* area = "USA"; const char* model = "RVL-001(USA)"; const char* code = "LU"; const char* game = "US";
    if (region == 'J') { area = "JPN"; model = "RVL-001(JPN)"; code = "LJ"; game = "JP"; }
    else if (region == 'K' || region == 'Q' || region == 'T') { area = "KOR"; model = "RVL-001(KOR)"; code = "LKM"; game = "KR"; }
    else if (pal) { area = "EUR"; model = "RVL-001(EUR)"; code = "LEH"; game = "EU"; }
    char text[256] = {};
    std::snprintf(text, sizeof text,
                  "AREA=%s\r\nMODEL=%s\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=%s\r\nSERNO=000000000\r\nVIDEO=%s\r\nGAME=%s\r\n",
                  area, model, code, pal ? "PAL" : "NTSC", game);
    uint32_t key = 0x73B5DBFAu;
    for (uint32_t i = 0; i < sizeof text; ++i) {
        w8(cpu, 0x80003800u + i, uint8_t(uint8_t(text[i]) ^ uint8_t(key)));
        key = (key << 1) | (key >> 31);
    }
}
}  // namespace

bool setup_boot_memory(CPUState* cpu, const PakDisc& pak, BootInfo& info) {
    uint8_t boot[0x440];
    if (!pak.read(0, sizeof boot, boot)) return false;
    info.game_id.assign(reinterpret_cast<const char*>(boot), 6);
    info.title_id = (uint64_t(0x00010000u) << 32) | be32(boot);
    info.pal = is_pal(char(boot[3]));

    // Cabecera del disco (ID, versión, nombre corto) y la copia del ID que IOS deja en 0x3180.
    if (!copy_from_pak(cpu, pak, 0, 0x20, 0x80000000u)) return false;
    w32(cpu, 0x80003180u, be32(boot));

    w32(cpu, 0x80000020u, 0x0D15EA5Eu);   // arranque normal
    w32(cpu, 0x80000024u, 0x00000001u);
    w32(cpu, 0x80000028u, 0x01800000u);   // MEM1: 24 MiB
    w32(cpu, 0x8000002Cu, 0x00000023u);   // placa de producción
    w32(cpu, 0x80000030u, 0x00000000u);   // ArenaLo: la del DOL
    w32(cpu, 0x800000CCu, info.pal ? 1u : 0u);  // modo de vídeo del IPL
    w32(cpu, 0x800000E4u, 0x8008F7B8u);
    w32(cpu, 0x800000F0u, 0x01800000u);   // memoria simulada
    w32(cpu, 0x800000F4u, 0x8179B500u);   // bi2.bin
    w32(cpu, 0x800000F8u, 0x0E7BE2C0u);   // bus: 243 MHz
    w32(cpu, 0x800000FCu, 0x2B73A840u);   // CPU: 729 MHz
    w32(cpu, 0x800030C0u, 0);
    w32(cpu, 0x800030C4u, 0);
    w32(cpu, 0x800030D8u, 0xFFFFFFFFu);
    w32(cpu, 0x800030DCu, 0);
    w16(cpu, 0x800030E0u, 0);             // PADInit
    w16(cpu, 0x800030E6u, 0x8201);
    w32(cpu, 0x800030F0u, 0);
    w32(cpu, 0x80003100u, 0x01800000u);   // MEM1 física
    w32(cpu, 0x80003104u, 0x01800000u);   // MEM1 simulada
    w32(cpu, 0x8000310Cu, 0);
    w32(cpu, 0x80003110u, 0x8179D500u);   // fin de la arena de MEM1 (la FST lo baja)
    w32(cpu, 0x80003118u, 0x04000000u);   // MEM2: 64 MiB
    w32(cpu, 0x8000311Cu, 0x04000000u);
    w32(cpu, 0x80003120u, 0x93400000u);   // fin de MEM2 para el juego (IOS reserva lo de arriba)
    w32(cpu, 0x80003124u, 0x90000800u);   // ArenaLo de MEM2
    w32(cpu, 0x80003128u, 0x933E0000u);   // ArenaHi de MEM2
    w32(cpu, 0x80003130u, 0x933E0000u);   // MEM2 de IOS
    w32(cpu, 0x80003134u, 0x93400000u);
    w32(cpu, 0x80003138u, 0x00000011u);
    w32(cpu, 0x80003140u, 0x00240D17u);   // IOS36 v3351
    w32(cpu, 0x80003144u, 0x00062507u);
    w32(cpu, 0x80003158u, 0x0000FF16u);
    w8(cpu, 0x8000315Cu, 0x80);
    w16(cpu, 0x8000315Eu, 0x0113);
    w32(cpu, 0x80003160u, 0);
    w32(cpu, 0x80003184u, 0x80000000u);   // dirección del ID del juego
    w32(cpu, 0x80003188u, 0x00240D17u);

    // bi2.bin, donde lo deja el apploader.
    if (!copy_from_pak(cpu, pak, 0x440, 0x2000, 0x8179B500u)) return false;

    // La FST bajo el final de MEM1: DVDOpen la busca en 0x80000038.
    const uint32_t fst_offset = be32(boot + 0x424), fst_size = be32(boot + 0x428) << 2;
    const uint32_t fst_max = be32(boot + 0x42C) << 2;
    if (!fst_offset || !fst_size || fst_max < fst_size || fst_max > 0x400000u) {
        std::fprintf(stderr, "[boot] la FST del game.pak no es válida\n");
        return false;
    }
    const uint32_t arena_high = (0x817FFFFFu - fst_max) & ~0x1Fu;
    if (!copy_from_pak(cpu, pak, uint64_t(fst_offset) << 2, fst_size, arena_high)) return false;
    w32(cpu, 0x80000034u, arena_high);    // ArenaHi
    w32(cpu, 0x80000038u, arena_high);    // FST
    w32(cpu, 0x8000003Cu, fst_max);
    w32(cpu, 0x80003110u, arena_high);
    if (arena_high < 0x8179D500u)
        std::fprintf(stderr, "[boot] aviso: la FST (%u KiB) pisa bi2.bin\n", fst_max >> 10);

    write_setting_txt(cpu, char(boot[3]), info.pal);
    std::fprintf(stderr, "[boot] %s, %s, FST en %08x (%u KiB)\n", info.game_id.c_str(), info.pal ? "PAL" : "NTSC",
                 arena_high, fst_size >> 10);
    return true;
}
}  // namespace wii
