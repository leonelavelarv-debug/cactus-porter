#include "dev_di.h"

namespace ios {
s32 DevDI::ioctl(s32, u32 request, u8* in, u32 in_len, u8* out, u32 out_len) {
    if (request != IOCTL_DVD_READ || !in || in_len != 0x20) return IPC_EINVAL;
    const u32 length = be32(in + 4);
    const std::uint64_t offset = std::uint64_t(be32(in + 8)) << 2;
    if (length > out_len || (length && !out)) return IPC_EINVAL;

    // DI devuelve 1 al completar el comando y 2 si falla la lectura del medio.
    if (length && !pak_.read(offset, length, out)) return 2;
    return 1;
}
}
