#!/bin/sh
# Prueba PC del arranque y de IOS: ./tests/ios.sh /ruta/DolRecomp
# game.pak sintético: memoria baja, FST, IPC (open, ioctlv de ES, DI, eventhook diferido) y NAND.
set -eu
dr=$(realpath "$1")
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
python3 - <<'PY'
import struct
from pathlib import Path
pak = bytearray(0x20000)
pak[0:6] = b'RTSE01'
pak[0x20:0x24] = b'Test'
dol, fst = 0x8000, 0x10000
struct.pack_into('>IIII', pak, 0x420, dol >> 2, fst >> 2, 0x30 >> 2, 0x40 >> 2)
struct.pack_into('>I', pak, dol + 0x00, 0x100)       # texto 0
struct.pack_into('>I', pak, dol + 0x48, 0x80004000)
struct.pack_into('>I', pak, dol + 0x90, 0x20)
struct.pack_into('>I', pak, dol + 0xE0, 0x80004000)
# FST: raíz (2 entradas) + a.bin en 0x18000, 4 bytes; tabla de nombres detrás.
struct.pack_into('>III', pak, fst, 0x01000000, 0, 2)
struct.pack_into('>III', pak, fst + 12, 0x00000000, 0x18000 >> 2, 4)
pak[fst + 24:fst + 30] = b'a.bin\0'
pak[0x18000:0x18004] = b'HOLA'
Path('game.pak').write_bytes(pak)
PY
cat > harness.cpp <<'CPP'
#include "dev_di.h"
#include "dev_fs.h"
#include "dev_misc.h"
#include "wii_boot.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
static int irqs;
static void irq(CPUState*) { ++irqs; }
static CPUState cpu;
static uint32_t rd(uint32_t a) { return mem_read32(&cpu, a); }
static void wr(uint32_t a, uint32_t v) { mem_write32(&cpu, a, v); }
// Petición IPC en la dirección física 0x10000 (como la manda el SDK).
static void send(uint32_t cmd, int32_t fd, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
                 uint32_t a4 = 0) {
    const uint32_t r = 0x80010000;
    wr(r, cmd); wr(r + 4, 0); wr(r + 8, uint32_t(fd));
    wr(r + 12, a0); wr(r + 16, a1); wr(r + 20, a2); wr(r + 24, a3); wr(r + 28, a4);
    auto& k = ios::kernel();
    k.reg_write(0xCD000000, 0x00010000);
    k.reg_write(0xCD000004, 0x31);            // X1 + IY1 + IY2
    assert(k.reg_read(0xCD000004) & 2);       // Y2: aceptada
    k.reg_write(0xCD000004, 0x32);            // el PPC confirma Y2
}
static bool replied() { return ios::kernel().reg_read(0xCD000004) & 4; }
static int32_t take_reply() {
    auto& k = ios::kernel();
    assert(replied() && k.reg_read(0xCD000008) == 0x00010000 && rd(0x80010000) == 8);
    const int32_t result = int32_t(rd(0x80010004));
    k.reg_write(0xCD000004, 0x34);            // confirma Y1
    return result;
}
static int32_t open_dev(const char* path) {
    std::strcpy(reinterpret_cast<char*>(wii::resolve(&cpu, 0x80010100, 64)), path);
    send(1, 0, 0x00010100, 0);
    return take_reply();
}
int main() {
    assert(cpu_init(&cpu) && cpu_alloc_mem2(&cpu, WII_MEM2_SIZE));
    wii::PakDisc pak;
    assert(pak.open("game.pak"));
    std::vector<uint8_t> file;
    assert(pak.read_file("a.bin", file) && std::memcmp(file.data(), "HOLA", 4) == 0);
    wii::BootInfo boot;
    assert(wii::setup_boot_memory(&cpu, pak, boot));
    assert(boot.game_id == "RTSE01" && boot.title_id == 0x0001000052545345ull && !boot.pal);
    assert(rd(0x80000000) == 0x52545345 && rd(0x80000020) == 0x0D15EA5E && rd(0x80003180) == 0x52545345);
    const uint32_t fst = rd(0x80000038);
    assert(fst == ((0x817FFFFFu - 0x40) & ~0x1Fu) && rd(0x8000003C) == 0x40 && rd(0x80000034) == fst);
    assert(rd(fst) == 0x01000000 && rd(fst + 8) == 2 && rd(fst + 16) == (0x18000 >> 2));
    assert((mem_read8(&cpu, 0x80003800) ^ 0xFA) == 'A');  // setting.txt cifrado

    mkdir("nand", 0700);
    auto& k = ios::kernel();
    k.init(&cpu, irq);
    k.register_device("/dev/fs", ios::make_dev_fs("nand"));
    k.register_device("/dev/di", std::make_shared<ios::DevDI>(pak));
    ios::register_misc_devices(k, boot.title_id);

    // ES: ID del título por ioctlv (vector de salida de 8 bytes en 0x10200).
    const int32_t es = open_dev("/dev/es");
    assert(es >= 0);
    wr(0x80010300, 0x00010200); wr(0x80010304, 8);
    send(7, es, 0x20, 0, 1, 0x00010300);
    assert(take_reply() == 0 && rd(0x80010200) == 0x00010000 && rd(0x80010204) == 0x52545345);

    // DI: lectura 0x71 de a.bin a 0x10400.
    const int32_t di = open_dev("/dev/di");
    wr(0x80010500, 0x71000000); wr(0x80010504, 4); wr(0x80010508, 0x18000 >> 2);
    send(6, di, 0x71, 0x00010500, 0x20, 0x00010400, 4);
    assert(take_reply() == 1 && rd(0x80010400) == 0x484F4C41);

    // STM: el eventhook no contesta hasta RELEASE_EH.
    const int32_t hook = open_dev("/dev/stm/eventhook");
    const int32_t imm = open_dev("/dev/stm/immediate");
    send(6, hook, 0x1000);
    assert(!replied());
    wr(0x80011000, 6); wr(0x80011004, 0); wr(0x80011008, uint32_t(imm));
    wr(0x8001100C, 0x3002);
    k.reg_write(0xCD000000, 0x00011000);
    k.reg_write(0xCD000004, 0x31);
    k.reg_write(0xCD000004, 0x32);
    assert(replied() && k.reg_read(0xCD000008) == 0x00010000);  // primero la del eventhook
    k.reg_write(0xCD000004, 0x34);
    assert(replied() && k.reg_read(0xCD000008) == 0x00011000);  // luego la de RELEASE_EH
    k.reg_write(0xCD000004, 0x34);
    assert(!replied() && irqs > 0);

    // Un dispositivo inexistente devuelve ENOENT.
    assert(open_dev("/dev/usb/oh1/57e/305") == -6);
    cpu_free(&cpu);
    std::puts("ok");
}
CPP
cc -O1 -I"$dr/src" -c "$dr/src/cpu/cpu.c"
c++ -std=c++20 -O1 -I"$host" -I"$dr/src" harness.cpp cpu.o \
    "$host/wii_boot.cpp" "$host/wii_dvd.cpp" "$host/ios.cpp" "$host/dev_di.cpp" "$host/dev_fs.cpp" \
    "$host/dev_misc.cpp" -lm -o check
./check
echo "OK: memoria de arranque, FST, IPC, ES, DI y eventhook diferido"
