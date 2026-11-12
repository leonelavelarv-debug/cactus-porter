#include "sysconf.h"
#include "wiimote.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace wiisys {
namespace {

constexpr size_t kSize = 0x4000;
enum Type : uint8_t { BigArray = 1, SmallArray = 2, Byte = 3, Short = 4, Long = 5, Bool = 7 };

struct Item { std::string name; Type type; std::vector<uint8_t> data; };

Item byte(const char* n, uint8_t v) { return {n, Byte, {v}}; }
Item boolean(const char* n, bool v) { return {n, Bool, {uint8_t(v)}}; }
Item word32(const char* n, uint32_t v) { return {n, Long, {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}}; }

// BT.DINF (0x461 bytes): nº de mandos registrados y 10 entradas {MAC[6], nombre[64]}, luego 6 entradas "activas".
Item dinf() {
    constexpr size_t kEntry = 70;
    std::vector<uint8_t> d(1 + 10 * kEntry + 6 * kEntry, 0);
    d[0] = 1;
    std::memcpy(&d[1], wpad::kAddress, 6);
    std::memcpy(&d[7], wpad::kName, std::strlen(wpad::kName));
    return {"BT.DINF", BigArray, std::move(d)};
}

Item cdif() { return {"BT.CDIF", BigArray, std::vector<uint8_t>(0x205, 0)}; }

std::vector<Item> items() {
    return {byte("IPL.LNG", 1), byte("IPL.AR", 0), boolean("IPL.E60", true), boolean("IPL.PGS", true),
            byte("IPL.SND", 1), boolean("IPL.EULA", true), word32("IPL.CB", 0), word32("IPL.IDL", 0),
            byte("BT.BAR", 1), byte("BT.MOT", 1), byte("BT.SPKV", 0x58), word32("BT.SENS", 3),
            cdif(), dinf()};
}

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v); }

}  // namespace

std::vector<uint8_t> build() {
    const std::vector<Item> list = items();
    std::vector<uint8_t> f(kSize, 0);
    std::memcpy(&f[0], "SCv0", 4);
    put16(f, 4, uint16_t(list.size()));
    size_t at = 6 + 2 * list.size();
    for (size_t i = 0; i < list.size(); ++i) {
        const Item& it = list[i];
        put16(f, 6 + 2 * i, uint16_t(at));
        f[at++] = uint8_t(it.type << 5 | (it.name.size() - 1));
        std::memcpy(&f[at], it.name.data(), it.name.size());
        at += it.name.size();
        if (it.type == BigArray) { put16(f, at, uint16_t(it.data.size() - 1)); at += 2; }
        else if (it.type == SmallArray) f[at++] = uint8_t(it.data.size() - 1);
        std::memcpy(&f[at], it.data.data(), it.data.size());
        at += it.data.size();
    }
    std::memcpy(&f[kSize - 4], "SCed", 4);
    return f;
}

bool ensure(const std::string& nand_dir) {
    const std::string path = nand_dir + "/shared2/sys/SYSCONF";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size == long(kSize)) return true;
    const std::vector<uint8_t> f = build();
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    const bool ok = std::fwrite(f.data(), 1, f.size(), out) == f.size();
    return std::fclose(out) == 0 && ok;
}

}  // namespace wiisys
