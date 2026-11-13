// wii_rel.h — módulos REL recompilados por DolRecomp.
//
// DolRecomp compila cada REL para una dirección fija (aquí desde 0x81800000: el hueco de 8 MiB sobre los 24 MiB
// de MEM1, a menos de 32 MiB del DOL para que lleguen sus bl). El juego, en cambio, lee el REL a su heap (X) y
// lo enlaza ahí con OSLink. Este módulo no toca OSLink: cuando el juego salta a código de un REL enlazado en X
// (su prolog, o cualquier puntero a función), monta en la dirección fija (B) una copia limpia del REL leída del
// game.pak, la reubica exactamente como DolRecomp (DOL, el propio módulo y los demás REL compilados con él),
// borra su BSS y desvía el salto a B + (dirección - X). Desde ahí todo el código y los datos del módulo viven en B.
// La copia de X queda marcada (campo nameSize de su cabecera) para notar si el juego vuelve a cargar el módulo.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct CPUState;

extern "C" {
typedef void (*CactusRelFunction)(CPUState*);
typedef struct CactusRelModule {
    uint32_t id;                              // número de módulo (cabecera del REL)
    uint32_t base;                            // dirección para la que lo compiló DolRecomp
    const char* disc_path;                    // ruta en el disco (puede estar comprimido: Yaz0 / LZ77)
    CactusRelFunction (*find)(uint32_t);      // dolrecomp_find_original del C generado del módulo
} CactusRelModule;
// Tabla generada por cactus-porter (cactus_rels.c); sin REL: count 0.
extern const CactusRelModule cactus_rel_modules[];
extern const unsigned cactus_rel_module_count;
}

namespace rel {

inline constexpr uint32_t kRegionStart = 0x81800000u, kRegionEnd = 0x82000000u;
inline constexpr uint32_t kModuleList = 0x800030C8u;  // __OSModuleList (cabeza, cola) del OS

struct Range { uint32_t lo, hi; };
using ReadFile = std::function<bool(const std::string& path, std::vector<uint8_t>& out)>;

// Lee y valida los módulos (la RAM de MEM1 debe llegar a kRegionEnd). Devuelve cuántos quedaron listos.
// `dol_code`: secciones de código del DOL, que nunca son de un REL (para descartar rápido en dispatch).
size_t init(CPUState* cpu, const CactusRelModule* modules, size_t count, const ReadFile& read,
            std::vector<Range> dol_code);
// Desde el despacho de DolRecomp (host_call): true si la dirección era de un REL y se ejecutó.
bool dispatch(CPUState* cpu, uint32_t address);
size_t module_count();
size_t migrations();

}  // namespace rel
