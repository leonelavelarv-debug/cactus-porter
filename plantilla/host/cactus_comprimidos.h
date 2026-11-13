// cactus_comprimidos.h — descompresores de Nintendo para los REL del disco: Yaz0 ("Yaz0", SZS) y LZ77 de
// tipo 0x10 y 0x11 (LZ11, el de los .LZ de New Super Mario Bros. Wii). Solo cabecera: lo usan el host (lee los
// REL del game.pak en la consola) y nebula-pak (los saca del disco en el PC para DolRecomp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cactus_lz {

inline bool yaz0(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    if (n < 16 || std::memcmp(in, "Yaz0", 4) != 0) return false;
    const size_t size = (size_t(in[4]) << 24) | (size_t(in[5]) << 16) | (size_t(in[6]) << 8) | in[7];
    if (size > (256u << 20)) return false;
    out.assign(size, 0);
    size_t src = 16, dst = 0;
    while (dst < size) {
        if (src >= n) return false;
        const uint8_t code = in[src++];
        for (int bit = 7; bit >= 0 && dst < size; --bit) {
            if (code & (1u << bit)) {
                if (src >= n) return false;
                out[dst++] = in[src++];
                continue;
            }
            if (src + 2 > n) return false;
            const uint8_t b1 = in[src++], b2 = in[src++];
            const size_t dist = ((size_t(b1 & 0xF) << 8) | b2) + 1;
            size_t len = b1 >> 4;
            if (len == 0) {
                if (src >= n) return false;
                len = size_t(in[src++]) + 0x12;
            } else {
                len += 2;
            }
            if (dist > dst) return false;
            for (size_t i = 0; i < len && dst < size; ++i, ++dst) out[dst] = out[dst - dist];
        }
    }
    return true;
}

inline bool lz77(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
    if (n < 4 || (in[0] != 0x10 && in[0] != 0x11)) return false;
    const bool lz11 = in[0] == 0x11;
    size_t size = size_t(in[1]) | (size_t(in[2]) << 8) | (size_t(in[3]) << 16), src = 4;
    if (size == 0) {  // tamaño extendido de 32 bits
        if (n < 8) return false;
        size = size_t(in[4]) | (size_t(in[5]) << 8) | (size_t(in[6]) << 16) | (size_t(in[7]) << 24);
        src = 8;
    }
    if (size > (256u << 20)) return false;
    out.assign(size, 0);
    size_t dst = 0;
    while (dst < size) {
        if (src >= n) return false;
        const uint8_t flags = in[src++];
        for (int bit = 7; bit >= 0 && dst < size; --bit) {
            if (!(flags & (1u << bit))) {
                if (src >= n) return false;
                out[dst++] = in[src++];
                continue;
            }
            size_t len, dist;
            if (!lz11) {
                if (src + 2 > n) return false;
                const uint8_t b0 = in[src++], b1 = in[src++];
                len = size_t(b0 >> 4) + 3;
                dist = ((size_t(b0 & 0xF) << 8) | b1) + 1;
            } else {
                if (src >= n) return false;
                const uint8_t b0 = in[src];
                if ((b0 >> 4) == 0) {
                    if (src + 3 > n) return false;
                    const uint8_t b1 = in[src + 1], b2 = in[src + 2];
                    len = ((size_t(b0 & 0xF) << 4) | (b1 >> 4)) + 0x11;
                    dist = ((size_t(b1 & 0xF) << 8) | b2) + 1;
                    src += 3;
                } else if ((b0 >> 4) == 1) {
                    if (src + 4 > n) return false;
                    const uint8_t b1 = in[src + 1], b2 = in[src + 2], b3 = in[src + 3];
                    len = ((size_t(b0 & 0xF) << 12) | (size_t(b1) << 4) | (b2 >> 4)) + 0x111;
                    dist = ((size_t(b2 & 0xF) << 8) | b3) + 1;
                    src += 4;
                } else {
                    if (src + 2 > n) return false;
                    const uint8_t b1 = in[src + 1];
                    len = size_t(b0 >> 4) + 1;
                    dist = ((size_t(b0 & 0xF) << 8) | b1) + 1;
                    src += 2;
                }
            }
            if (dist > dst) return false;
            for (size_t i = 0; i < len && dst < size; ++i, ++dst) out[dst] = out[dst - dist];
        }
    }
    return true;
}

// ¿Parece un REL? (versión 1-3 y tabla de secciones dentro del archivo)
inline bool looks_like_rel(const std::vector<uint8_t>& f) {
    if (f.size() < 0x40) return false;
    auto be32 = [&](size_t o) { return (uint32_t(f[o]) << 24) | (uint32_t(f[o + 1]) << 16) | (uint32_t(f[o + 2]) << 8) | f[o + 3]; };
    const uint32_t version = be32(0x1C), sections = be32(0x0C), table = be32(0x10);
    return version >= 1 && version <= 3 && sections > 0 && sections < 64 && table >= 0x40 &&
           size_t(table) + size_t(sections) * 8 <= f.size();
}

// El archivo tal cual si ya es un REL; si no, descomprimido si es Yaz0/LZ77 y el resultado es un REL.
inline bool rel_bytes(const std::vector<uint8_t>& file, std::vector<uint8_t>& out) {
    if (looks_like_rel(file)) { out = file; return true; }
    std::vector<uint8_t> d;
    if ((yaz0(file.data(), file.size(), d) || lz77(file.data(), file.size(), d)) && looks_like_rel(d)) {
        out = std::move(d);
        return true;
    }
    return false;
}

}  // namespace cactus_lz
