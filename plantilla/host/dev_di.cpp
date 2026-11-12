// Órdenes de /dev/di que usa la biblioteca DVD del SDK (números de Dolphin, IOS/DI/DI.h).
// DI contesta 1 si la orden termina bien y 2 si falla el medio.
#include "dev_di.h"
#include <cstdio>
#include <cstring>

namespace ios {
namespace {
constexpr s32 kOk = 1, kDriveError = 2;
void put32(u8* out, u32 out_len, u32 value) {
    if (out && out_len >= 4) wbe32(out, value);
}
}

s32 DevDI::ioctl(s32, u32 request, u8* in, u32 in_len, u8* out, u32 out_len) {
    if (out && out_len) std::memset(out, 0, out_len);
    switch (request) {
    case IOCTL_DVD_READ: {  // 0x71: lectura de la partición abierta
        if (!in || in_len < 12) return IPC_EINVAL;
        const u32 length = be32(in + 4);
        const std::uint64_t offset = std::uint64_t(be32(in + 8)) << 2;
        if (length > out_len || (length && !out)) return IPC_EINVAL;
        return !length || pak_.read(offset, length, out) ? kOk : kDriveError;
    }
    case 0x70:  // ReadDiskID: la cabecera del disco
        return out && out_len >= 0x20 && pak_.read(0, 0x20, out) ? kOk : kDriveError;
    case 0x12:  // Inquiry: revisión y fecha de la unidad
        put32(out, out_len, 0x00000002u);
        if (out && out_len >= 8) wbe32(out + 4, 0x20060526u);
        return kOk;
    case 0x88:  // GetCoverStatus: 2 = disco dentro
        put32(out, out_len, 2);
        return kOk;
    case 0x7A:  // GetCoverRegister: tapa cerrada
    case 0x95:  // GetStatusRegister
    case 0x96:  // GetControlRegister
    case 0xE0:  // RequestError: sin error
        return kOk;
    case 0x8D:  // UnencryptedRead: el game.pak no guarda lo de fuera de la partición
    case 0xD0:  // ReadDvd
    case 0xDA:  // ReadDiskBca
        return kDriveError;
    case 0x79: case 0x7E: case 0x86: case 0x8A: case 0x8C: case 0x8E: case 0xA4:
    case 0xAB: case 0xD2: case 0xD9: case 0xDB: case 0xDC: case 0xDD: case 0xDF:
    case 0xE3: case 0xE4:
        return kOk;  // reset, motor, búsquedas, tapa, partición: nada que hacer con el game.pak
    default: {
        static u32 logged = 0;
        if (logged++ < 16) std::fprintf(stderr, "[di] ioctl %02x sin implementar\n", request);
        return kOk;
    }
    }
}

s32 DevDI::ioctlv(s32, u32 request, u32 nin, u32 nout, IOVec* v) {
    for (u32 i = nin; i < nin + nout; ++i)
        if (v[i].data && v[i].len) std::memset(v[i].data, 0, v[i].len);
    if (request == 0x8B) return kOk;  // OpenPartition: la del juego ya es el game.pak (TMD y error ES a cero)
    std::fprintf(stderr, "[di] ioctlv %02x sin implementar\n", request);
    return kDriveError;
}
}
