#!/bin/sh
# Prueba PC de los módulos REL: ./tests/rel.sh /ruta/dolrecomp /ruta/DolRecomp
# Un DOL y dos REL (el 2 importa del 1; uno en Yaz0 y otro en LZ11 "en el disco"). DolRecomp los recompila
# como lo hace cactus-porter (modo carpeta desde 0x81800000), cactus-porter genera la tabla, y el runtime real
# (juego_glue + wii_rel) ejecuta: el DOL llama al prolog de cada módulo en la copia que "el juego" dejó en su
# heap, y el host lo desvía al código recompilado con la copia limpia reubicada. También recarga un módulo.
set -eu
exe=$(realpath "$1")
dr=$(realpath "$2")
raiz=$(cd "$(dirname "$0")/.." && pwd)
host="$raiz/plantilla/host"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
python3 - <<'PY'
import struct
from pathlib import Path

def words(ws): return b"".join(struct.pack(">I", w) for w in ws)

def build_rel(mid, text, data, bss, relocs, prolog):
    """relocs: {módulo: [(sección, desplazamiento, tipo, sección_destino, addend)]}"""
    h = 0x4C
    table = h
    text_off = 0x80
    data_off = text_off + len(text)
    imp_off = (data_off + len(data) + 7) & ~7
    imports = sorted(relocs)
    rel_off = imp_off + 8 * len(imports)
    blobs, cursor = [], rel_off
    imp = b""
    for mod in imports:
        out, sec, pos = b"", None, 0
        for (s, off, typ, tsec, add) in sorted(relocs[mod]):
            if s != sec:
                out += struct.pack(">HBBI", 0, 202, s, 0); sec, pos = s, 0
            out += struct.pack(">HBBI", off - pos, typ, tsec, add); pos = off
        out += struct.pack(">HBBI", 0, 203, 0, 0)
        imp += struct.pack(">II", mod, cursor)
        blobs.append(out); cursor += len(out)
    total = cursor
    f = bytearray(total)
    struct.pack_into(">IIIIIIII", f, 0, mid, 0, 0, 4, table, 0, 0, 3)
    struct.pack_into(">IIII", f, 0x20, bss, rel_off, imp_off, len(imp))
    f[0x30] = 1; f[0x31] = 1; f[0x32] = 1; f[0x33] = 0
    struct.pack_into(">III", f, 0x34, prolog, prolog, prolog)
    struct.pack_into(">III", f, 0x40, 32, 32, rel_off)  # align, bssAlign, fixSize
    struct.pack_into(">IIIIIIII", f, table, 0, 0, text_off | 1, len(text), data_off, len(data), 0, bss)
    f[text_off:text_off + len(text)] = text
    f[data_off:data_off + len(data)] = data
    f[imp_off:imp_off + len(imp)] = imp
    p = rel_off
    for b in blobs: f[p:p + len(b)] = b; p += len(b)
    return bytes(f)

ADDR32, ADDR16_LO, ADDR16_HA, REL24 = 1, 4, 6, 10
nop = 0x60000000
t1 = words([0x3C600000, 0x38630000, 0x80830000, 0x3CA00000, 0x38A50000, 0x90850000,
            0x7FE802A6, 0x48000001, 0x7FE803A6, 0x4E800020] + [nop] * 6 + [0x3AB50001, 0x4E800020])
d1 = words([0x12345678, 0])
rel1 = build_rel(1, t1, d1, 8, {
    1: [(1, 0x02, ADDR16_HA, 2, 0), (1, 0x06, ADDR16_LO, 2, 0), (1, 0x0E, ADDR16_HA, 3, 0),
        (1, 0x12, ADDR16_LO, 3, 0), (2, 0x04, ADDR32, 1, 0x40)],
    0: [(1, 0x1C, REL24, 0, 0x80004100)]}, 0)
t2 = words([0x7FC802A6, 0x48000001, 0x7FC803A6, 0x4E800020])
rel2 = build_rel(2, t2, words([0]), 0, {1: [(1, 0x04, REL24, 1, 0x40), (2, 0x00, ADDR32, 2, 0)]}, 0)
Path("rels/mod").mkdir(parents=True)
Path("rels/mod/a.rel").write_bytes(rel1)
Path("rels/mod/b.rel").write_bytes(rel2)
Path("raw1.bin").write_bytes(rel1)
Path("raw2.bin").write_bytes(rel2)

# "Disco": a.rel en Yaz0 y b.rel en LZ11, codificados solo con literales.
def yaz0(b):
    out = bytearray(b"Yaz0" + struct.pack(">I", len(b)) + bytes(8))
    for i in range(0, len(b), 8):
        chunk = b[i:i + 8]; out.append(0xFF); out += chunk
    return bytes(out)
def lz11(b):
    out = bytearray([0x11, len(b) & 0xFF, (len(b) >> 8) & 0xFF, len(b) >> 16])
    for i in range(0, len(b), 8):
        out.append(0); out += b[i:i + 8]
    return bytes(out)
Path("disco/mod").mkdir(parents=True)
Path("disco/mod/a.rel.szs").write_bytes(yaz0(rel1))
Path("disco/mod/b.rel.LZ").write_bytes(lz11(rel2))

code = {0x00: 0x3D808060, 0x04: 0x818C0034, 0x08: 0x7D8903A6, 0x0C: 0x4E800421,
        0x10: 0x3D808061, 0x14: 0x818C0034, 0x18: 0x7D8903A6, 0x1C: 0x4E800421,
        0x20: 0x38630001, 0x24: 0x4BFFFFFC,
        0x100: 0x3A940001, 0x104: 0x4E800020}
size = 0x108
dol = bytearray(0x100 + size)
struct.pack_into(">I", dol, 0x00, 0x100)
struct.pack_into(">I", dol, 0x48, 0x80004000)
struct.pack_into(">I", dol, 0x90, size)
struct.pack_into(">I", dol, 0xE0, 0x80004000)
for off in range(0, size, 4):
    struct.pack_into(">I", dol, 0x100 + off, code.get(off, nop))
Path("proof.dol").write_bytes(dol)
PY
"$exe" --cpu broadway proof.dol TEST01 TEST01.c >/dev/null
mkdir -p gen out
"$exe" --cpu broadway --rel-base 0x81800000 rels TEST01 out > rels.log
# La tabla y el pegamento, con las mismas funciones que cactus-porter recomp.
python3 - "$raiz/cactus-porter.py" <<'PY'
import importlib.util, sys
from pathlib import Path
spec = importlib.util.spec_from_file_location("porter", sys.argv[1])
porter = importlib.util.module_from_spec(spec); spec.loader.exec_module(porter)
rels = {"mod/a.rel": "mod/a.rel.szs", "mod/b.rel": "mod/b.rel.LZ"}
mods = porter.leer_modulos(Path("rels.log").read_text(), Path("rels"), rels)
assert len(mods) == 2, mods
porter.generar_tabla_rels(Path("gen").resolve(), Path("out").resolve(), mods)
srcs = [l.strip().strip('"') for l in Path("gen/cactus_rels.cmake").read_text().splitlines() if l.strip().startswith('"')]
Path("rel_sources.txt").write_text("\n".join(srcs) + "\n")
PY
cc -O1 -I"$dr/src" -c "$dr/src/cpu/cpu.c" TEST01_chunks/*.c
i=0
while read -r src; do
    i=$((i + 1))
    cc -O1 -I"$dr/src" -c "$src" -o "rel_$i.o"
done < rel_sources.txt
cat > harness.cpp <<'CPP'
#include "wii_gx.h"
#include "wii_rel.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
namespace cactus {
uint32_t gx_pi_cause_exchange(uint32_t) { return 0; }
}
static std::vector<uint8_t> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static CPUState cpu{};
static void put(uint32_t a, const std::vector<uint8_t>& b) { for (size_t i = 0; i < b.size(); ++i) mem_write8(&cpu, a + uint32_t(i), b[i]); }
static uint32_t base_of(uint32_t id) {
    for (unsigned i = 0; i < cactus_rel_module_count; ++i) if (cactus_rel_modules[i].id == id) return cactus_rel_modules[i].base;
    return 0;
}
// Lo que deja OSLink que importa aquí: el prolog absoluto y la lista de módulos.
static void link(uint32_t x, const std::vector<uint8_t>& raw) {
    put(x, raw);
    const uint32_t text = mem_read32(&cpu, x + 0x4C + 8) & ~1u;
    mem_write32(&cpu, x + 0x34, x + text);
}
static void run_from_entry() {
    cpu.pc = 0x80004000;
    for (int i = 0; i < 3; ++i) assert(cactus::game_frame(&cpu, 0));
    assert(cpu.pc >= 0x80004020 && cpu.pc <= 0x80004024);
}
int main() {
    assert(cpu_init(&cpu));
    std::free(cpu.ram);
    cpu.ram_size = rel::kRegionEnd - 0x80000000u;
    cpu.ram = static_cast<uint8_t*>(std::calloc(1, cpu.ram_size));
    const auto raw1 = slurp("raw1.bin"), raw2 = slurp("raw2.bin");
    const size_t n = rel::init(&cpu, cactus_rel_modules, cactus_rel_module_count,
        [](const std::string& path, std::vector<uint8_t>& out) { out = slurp("disco/" + path); return !out.empty(); },
        {{0x80004000u, 0x80004108u}});
    assert(n == 2);
    const uint32_t b1 = base_of(1), b2 = base_of(2);
    assert(b1 == 0x81800000u && b2 > b1);
    const uint32_t x1 = 0x80600000u, x2 = 0x80610000u;
    link(x1, raw1);
    link(x2, raw2);
    mem_write32(&cpu, rel::kModuleList, x1);
    mem_write32(&cpu, rel::kModuleList + 4, x2);
    mem_write32(&cpu, x1 + 4, x2);
    mem_write32(&cpu, x2 + 8, x1);
    assert(cactus::game_init(&cpu, 0x80004000));
    run_from_entry();

    const uint32_t text1 = b1 + 0x80, data1 = b1 + 0x80 + 0x48;
    const uint32_t bss1 = (b1 + mem_read32(&cpu, x1 + 0x48) + 31) & ~31u;
    std::printf("r20=%u r21=%u bss=%08X migraciones=%zu\n", cpu.gpr[20], cpu.gpr[21], mem_read32(&cpu, bss1), rel::migrations());
    assert(cpu.gpr[20] == 1);                              // REL 1 -> DOL
    assert(cpu.gpr[21] == 1);                              // REL 2 -> REL 1 (importación)
    assert(mem_read32(&cpu, bss1) == 0x12345678u);         // datos y BSS del módulo en B
    assert(mem_read32(&cpu, data1 + 4) == text1 + 0x40);   // ADDR32 propio, reubicado en B
    assert(mem_read32(&cpu, b2 + 0x80 + 0x10) == data1);   // ADDR32 al módulo 1
    assert(mem_read32(&cpu, x1 + 0x18) == 0xCAC70001u && mem_read32(&cpu, x2 + 0x18) == 0xCAC70002u);
    assert(rel::migrations() == 2);

    // El juego vuelve a cargar el módulo 1 en el mismo sitio: datos limpios otra vez.
    mem_write32(&cpu, data1, 0xDEADu);
    mem_write32(&cpu, bss1, 0);
    link(x1, raw1);
    mem_write32(&cpu, x1 + 4, x2);                         // OSLink lo vuelve a encolar
    cpu.gpr[20] = cpu.gpr[21] = 0;
    run_from_entry();
    assert(rel::migrations() == 3);                        // solo el 1
    assert(mem_read32(&cpu, data1) == 0x12345678u && mem_read32(&cpu, bss1) == 0x12345678u);
    assert(cpu.gpr[20] == 1 && cpu.gpr[21] == 1);

    // Un módulo que no se recompiló: aviso, sin caerse.
    const uint32_t x9 = 0x80620000u;
    mem_write32(&cpu, x9, 9);
    mem_write32(&cpu, x9 + 4, 0);
    mem_write32(&cpu, x2 + 4, x9);
    mem_write32(&cpu, rel::kModuleList + 4, x9);
    run_from_entry();
    std::puts("OK: REL recompilados, importaciones entre módulos, Yaz0/LZ11 y recarga");
    return 0;
}
CPP
c++ -std=c++20 -O1 -DCACTUS_GENERATED_HEADER='"TEST01.h"' -I. -I"$host" -I"$dr/src" \
    "$host/juego_glue.cpp" "$host/wii_rel.cpp" harness.cpp ./*.o -lm -o check
./check
