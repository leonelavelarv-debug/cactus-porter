// wii_rel.cpp — ver wii_rel.h. La disposición de secciones y la reubicación replican las de DolRecomp
// (src/frontend/container/rel.c: rel_load_image y rel_apply_relocations), porque el C generado lleva esas
// direcciones grabadas: cualquier diferencia rompería el código recompilado.
#include "wii_rel.h"
#include "cactus_comprimidos.h"
#include "guest_memory.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace rel {
namespace {

constexpr uint32_t kMarker = 0xCAC70000u;  // en nameSize (cabecera + 0x18) de la copia del juego
constexpr uint8_t R_PPC_NONE = 0, R_PPC_ADDR32 = 1, R_PPC_ADDR24 = 2, R_PPC_ADDR16 = 3, R_PPC_ADDR16_LO = 4,
                  R_PPC_ADDR16_HI = 5, R_PPC_ADDR16_HA = 6, R_PPC_ADDR14 = 7, R_PPC_ADDR14_BRTAKEN = 8,
                  R_PPC_ADDR14_BRNTAKEN = 9, R_PPC_REL24 = 10, R_PPC_REL14 = 11, R_DOLPHIN_NOP = 201,
                  R_DOLPHIN_SECTION = 202, R_DOLPHIN_END = 203;

struct Section { uint32_t offset = 0, size = 0, addr = 0; bool exec = false, bss = false; };

struct Module {
    uint32_t id = 0, base = 0, end = 0, fix_size = 0, bss_lo = 0, bss_hi = 0;
    uint32_t rel_offset = 0, imp_offset = 0, imp_size = 0;
    std::string path;
    std::vector<uint8_t> image;
    std::vector<Section> sec;
    CactusRelFunction (*find)(uint32_t) = nullptr;
    uint32_t x = 0;  // dónde lo enlazó el juego la última vez
};

struct State {
    CPUState* cpu = nullptr;
    std::vector<Module> mods;  // por base
    std::vector<Range> dol;
    std::vector<uint32_t> warned;
    uint32_t head = 0, tail = 0;
    size_t migrations = 0;
} s;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
uint32_t align_up(uint32_t v, uint32_t a) { return uint32_t((uint64_t(v) + a - 1) / a * a); }

uint32_t rd32(uint32_t a) {
    const uint8_t* p = wii::resolve(s.cpu, a, 4);
    return p ? be32(p) : 0;
}
void wr32(uint32_t a, uint32_t v) {
    if (uint8_t* p = wii::resolve(s.cpu, a, 4)) put32(p, v);
}

// rel_load_image de DolRecomp: secciones con datos en base + desplazamiento; BSS tras fixSize, alineada.
bool parse(Module& m) {
    const std::vector<uint8_t>& f = m.image;
    const uint32_t size = uint32_t(f.size());
    if (size < 0x40) return false;
    const uint8_t* h = f.data();
    if (be32(h) != m.id) {
        std::fprintf(stderr, "[rel] %s es el módulo %u, no el %u de la tabla\n", m.path.c_str(), be32(h), m.id);
        return false;
    }
    const uint32_t count = be32(h + 0x0C), table = be32(h + 0x10), version = be32(h + 0x1C);
    m.rel_offset = be32(h + 0x24);
    m.imp_offset = be32(h + 0x28);
    m.imp_size = be32(h + 0x2C);
    const uint32_t bss_align = size >= 0x48 ? be32(h + 0x44) : 4;
    m.fix_size = version >= 3 && size >= 0x4C ? be32(h + 0x48) : size;
    if (!m.fix_size || m.fix_size > size || !count || count > 64 || uint64_t(table) + count * 8ull > size) return false;
    uint32_t cursor = align_up(m.base + m.fix_size, bss_align ? bss_align : 4);
    m.bss_lo = cursor;
    m.sec.assign(count, {});
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = h + table + i * 8;
        Section& sc = m.sec[i];
        const uint32_t raw = be32(e);
        sc.size = be32(e + 4);
        sc.offset = raw & ~1u;
        sc.exec = raw & 1u;
        sc.bss = sc.size && !sc.offset && !sc.exec;
        if (!sc.size) continue;
        if (sc.bss) {
            sc.addr = cursor;
            cursor = align_up(cursor + sc.size, bss_align ? bss_align : 4);
            continue;
        }
        if (uint64_t(sc.offset) + sc.size > size) return false;
        sc.addr = m.base + sc.offset;
    }
    m.bss_hi = cursor;
    m.end = std::max(m.base + size, cursor);
    return true;
}

const Module* by_id(uint32_t id) {
    for (const Module& m : s.mods) if (m.id == id) return &m;
    return nullptr;
}

Module* by_address(uint32_t a) {
    auto it = std::upper_bound(s.mods.begin(), s.mods.end(), a, [](uint32_t v, const Module& m) { return v < m.base; });
    if (it == s.mods.begin()) return nullptr;
    --it;
    return a < it->end ? &*it : nullptr;
}

bool section_address(const Module& m, uint32_t index, uint32_t offset, uint32_t& out) {
    if (index >= m.sec.size() || !m.sec[index].size || offset > m.sec[index].size) return false;
    out = m.sec[index].addr + offset;
    return true;
}

// rel_apply_relocations de DolRecomp, pero sobre la copia en memoria del invitado.
bool relocate(const Module& m) {
    if (!m.imp_size) return true;
    const uint8_t* f = m.image.data();
    const uint32_t size = uint32_t(m.image.size());
    if (m.imp_size % 8 || uint64_t(m.imp_offset) + m.imp_size > size) return false;
    for (uint32_t i = 0; i < m.imp_size / 8; ++i) {
        const uint8_t* imp = f + m.imp_offset + i * 8;
        const uint32_t module = be32(imp);
        uint32_t cursor = be32(imp + 4), cur_sec = 0, cur_off = 0;
        const Module* target_mod = module == 0 ? nullptr : module == m.id ? &m : by_id(module);
        if (module && !target_mod) {
            std::fprintf(stderr, "[rel] el módulo %u importa del %u, que no está recompilado\n", m.id, module);
            return false;
        }
        bool ended = false;
        while (uint64_t(cursor) + 8 <= size) {
            const uint8_t* e = f + cursor;
            cursor += 8;
            const uint16_t delta = be16(e);
            const uint8_t type = e[2], sect = e[3];
            const uint32_t addend = be32(e + 4);
            if (type == R_DOLPHIN_SECTION) { cur_sec = sect; cur_off = 0; continue; }
            if (type == R_DOLPHIN_END) { ended = true; break; }
            cur_off += delta;
            if (type == R_DOLPHIN_NOP || type == R_PPC_NONE) continue;
            uint32_t target = addend, where = 0;
            if (target_mod && !section_address(*target_mod, sect, addend, target)) return false;
            if (!section_address(m, cur_sec, cur_off, where) || m.sec[cur_sec].bss) return false;
            uint8_t* p = wii::resolve(s.cpu, where, 4);
            if (!p) return false;
            switch (type) {
            case R_PPC_ADDR32: put32(p, target); break;
            case R_PPC_ADDR24: put32(p, (be32(p) & 0xFC000003u) | (target & 0x03FFFFFCu)); break;
            case R_PPC_ADDR16: case R_PPC_ADDR16_LO: put16(p, uint16_t(target)); break;
            case R_PPC_ADDR16_HI: put16(p, uint16_t(target >> 16)); break;
            case R_PPC_ADDR16_HA: put16(p, uint16_t((target + 0x8000u) >> 16)); break;
            case R_PPC_ADDR14: case R_PPC_ADDR14_BRTAKEN: case R_PPC_ADDR14_BRNTAKEN:
                put32(p, (be32(p) & 0xFFFF0003u) | (target & 0x0000FFFCu));
                break;
            case R_PPC_REL24: put32(p, (be32(p) & 0xFC000003u) | ((target - where) & 0x03FFFFFCu)); break;
            case R_PPC_REL14: put32(p, (be32(p) & 0xFFFF0003u) | ((target - where) & 0x0000FFFCu)); break;
            default:
                std::fprintf(stderr, "[rel] reubicación %u desconocida en el módulo %u\n", type, m.id);
                return false;
            }
        }
        if (!ended) return false;
    }
    return true;
}

void migrate(Module& m, uint32_t x) {
    uint8_t* dst = wii::resolve(s.cpu, m.base, m.image.size());
    uint8_t* bss = m.bss_hi > m.bss_lo ? wii::resolve(s.cpu, m.bss_lo, m.bss_hi - m.bss_lo) : nullptr;
    if (!dst || (m.bss_hi > m.bss_lo && !bss)) {
        std::fprintf(stderr, "[rel] el módulo %u (%08X) no cabe en la RAM del host\n", m.id, m.base);
        return;
    }
    std::memcpy(dst, m.image.data(), m.image.size());
    if (bss) std::memset(bss, 0, m.bss_hi - m.bss_lo);
    if (!relocate(m)) std::fprintf(stderr, "[rel] no se pudo reubicar el módulo %u: puede fallar\n", m.id);
    wr32(x + 0x18, kMarker | (m.id & 0xFFFF));
    m.x = x;
    ++s.migrations;
    std::fprintf(stderr, "[rel] módulo %u (%s): el juego lo enlazó en %08X, se ejecuta recompilado en %08X\n",
                 m.id, m.path.c_str(), x, m.base);
}

// Recorre la lista de módulos del OS y monta en B los que el juego enlazó desde la última vez.
void sync() {
    s.head = rd32(kModuleList);
    s.tail = rd32(kModuleList + 4);
    uint32_t info = s.head;
    for (int guard = 0; info && guard < 256; ++guard) {
        if (!wii::resolve(s.cpu, info, 0x20)) break;
        const uint32_t id = rd32(info);
        Module* m = nullptr;
        for (Module& c : s.mods) if (c.id == id) m = &c;
        if (!m) {
            if (std::find(s.warned.begin(), s.warned.end(), id) == s.warned.end()) {
                s.warned.push_back(id);
                std::fprintf(stderr, "[rel] el juego enlazó el módulo %u, que no está recompilado: su código no correrá\n", id);
            }
        } else if (rd32(info + 0x18) != (kMarker | (id & 0xFFFF))) {
            migrate(*m, info);
        } else {
            m->x = info;
        }
        info = rd32(info + 4);
    }
}

bool run(CPUState* cpu, const Module& m, uint32_t a) {
    const CactusRelFunction fn = m.find ? m.find(a) : nullptr;
    if (!fn) return false;
    cpu->pc = a;
    fn(cpu);
    return true;
}

bool in_dol(uint32_t a) {
    for (const Range& r : s.dol) if (a >= r.lo && a < r.hi) return true;
    return false;
}

}  // namespace

size_t init(CPUState* cpu, const CactusRelModule* modules, size_t count, const ReadFile& read,
            std::vector<Range> dol_code) {
    s = State{};
    s.cpu = cpu;
    s.dol = std::move(dol_code);
    if (cpu->ram_size < kRegionEnd - 0x80000000u && count) {
        std::fprintf(stderr, "[rel] la RAM del host no cubre %08X-%08X: sin REL\n", kRegionStart, kRegionEnd);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        Module m;
        m.id = modules[i].id;
        m.base = modules[i].base;
        m.path = modules[i].disc_path ? modules[i].disc_path : "";
        m.find = modules[i].find;
        std::vector<uint8_t> file;
        if (!read(m.path, file) || !cactus_lz::rel_bytes(file, m.image)) {
            std::fprintf(stderr, "[rel] no se pudo leer %s del disco\n", m.path.c_str());
            continue;
        }
        if (!parse(m)) {
            std::fprintf(stderr, "[rel] %s no es un REL válido\n", m.path.c_str());
            continue;
        }
        if (m.base < kRegionStart || m.end > kRegionEnd) {
            std::fprintf(stderr, "[rel] el módulo %u (%08X-%08X) se sale de %08X-%08X\n", m.id, m.base, m.end,
                         kRegionStart, kRegionEnd);
            continue;
        }
        s.mods.push_back(std::move(m));
    }
    std::sort(s.mods.begin(), s.mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    if (!s.mods.empty()) std::fprintf(stderr, "[rel] %zu módulos recompilados listos\n", s.mods.size());
    return s.mods.size();
}

bool dispatch(CPUState* cpu, uint32_t a) {
    if (s.mods.empty()) return false;
    if (a >= kRegionStart && a < kRegionEnd) {
        if (rd32(kModuleList) != s.head || rd32(kModuleList + 4) != s.tail) sync();
        const Module* m = by_address(a);
        return m && run(cpu, *m, a);
    }
    const bool ram = (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x94000000u);
    if (!ram || in_dol(a)) return false;
    sync();
    for (const Module& m : s.mods) {
        if (!m.x || a < m.x || a - m.x >= m.fix_size) continue;
        const uint32_t b = a - m.x + m.base;
        if (run(cpu, m, b)) return true;
    }
    return false;
}

size_t module_count() { return s.mods.size(); }
size_t migrations() { return s.migrations; }

}  // namespace rel
