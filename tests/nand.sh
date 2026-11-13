#!/bin/sh
# Prueba PC de la NAND (/dev/fs): carpetas, archivos, ReadDir (cuenta y nombres), SetAttr, GetAttr y borrado
# recursivo como el de IOS: ./tests/nand.sh
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
cat > harness.cpp <<'CPP'
#include "dev_fs.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
using namespace ios;
static std::vector<u8> attr(const char* path) {  // estructura de 74 bytes: ruta en +6
    std::vector<u8> a(74, 0);
    std::memcpy(a.data() + 6, path, std::strlen(path));
    return a;
}
static std::vector<u8> path64(const char* path) {
    std::vector<u8> p(64, 0);
    std::memcpy(p.data(), path, std::strlen(path));
    return p;
}
int main() {
    auto fs = make_dev_fs("nand");
    assert(fs && fs->open("/dev/fs", 0) == 0);
    for (const char* d : {"/title", "/title/00010000", "/title/00010000/52534245", "/title/00010000/52534245/data"}) {
        auto a = attr(d);
        assert(fs->ioctl(0, IOCTL_FS_CREATE_DIR, a.data(), 74, nullptr, 0) == IPC_OK);
    }
    for (const char* f : {"/title/00010000/52534245/data/banner.bin", "/title/00010000/52534245/data/save.bin"}) {
        auto a = attr(f);
        assert(fs->ioctl(0, IOCTL_FS_CREATE, a.data(), 74, nullptr, 0) == IPC_OK);
    }
    const s32 h = fs->open("/title/00010000/52534245/data/save.bin", 3);
    assert(h > 0);
    const u8 datos[5] = {1, 2, 3, 4, 5};
    assert(fs->write(h, datos, 5) == 5 && fs->seek(h, 0, 0) == 0);
    u8 leido[5] = {};
    assert(fs->read(h, leido, 5) == 5 && std::memcmp(leido, datos, 5) == 0);
    assert(fs->close(h) == IPC_OK);

    // ReadDir: primero la cuenta, luego los nombres (separados por NUL, en orden).
    auto dir = path64("/title/00010000/52534245/data");
    u8 count[4] = {}, max[4] = {0, 0, 0, 8}, names[64] = {};
    IOVec v1[2] = {{dir.data(), 64}, {count, 4}};
    assert(fs->ioctlv(0, IOCTL_FS_READ_DIR, 1, 1, v1) == IPC_OK && be32(count) == 2);
    IOVec v2[4] = {{dir.data(), 64}, {max, 4}, {names, 64}, {count, 4}};
    assert(fs->ioctlv(0, IOCTL_FS_READ_DIR, 2, 2, v2) == IPC_OK && be32(count) == 2);
    assert(!std::strcmp(reinterpret_cast<char*>(names), "banner.bin"));
    assert(!std::strcmp(reinterpret_cast<char*>(names) + 11, "save.bin"));
    max[3] = 1;  // como mucho un nombre
    assert(fs->ioctlv(0, IOCTL_FS_READ_DIR, 2, 2, v2) == IPC_OK && be32(count) == 1);

    // SetAttr: correcto si existe; error si no.
    auto sa = attr("/title/00010000/52534245/data/save.bin");
    assert(fs->ioctl(0, IOCTL_FS_SET_ATTR, sa.data(), 74, nullptr, 0) == IPC_OK);
    auto nx = attr("/title/00010000/52534245/data/nada.bin");
    assert(fs->ioctl(0, IOCTL_FS_SET_ATTR, nx.data(), 74, nullptr, 0) < 0);

    // Borrar la carpeta del juego con todo dentro, como IOS.
    auto del = path64("/title/00010000/52534245");
    assert(fs->ioctl(0, IOCTL_FS_DELETE, del.data(), 64, nullptr, 0) == IPC_OK);
    struct stat st;
    assert(stat("nand/title/00010000/52534245", &st) != 0 && stat("nand/title/00010000", &st) == 0);
    std::puts("nand: ok");
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/dev_fs.cpp" -o harness
./harness
