// ABI de IOS usada desde el host. Las solicitudes en RAM se codifican en big-endian.
#pragma once
#include <cstdint>

namespace ios {
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

// Comandos del bloque IPC (Dolphin, Core/IOS/IPCCommon.h).
inline constexpr u32 IOS_OPEN = 1, IOS_CLOSE = 2, IOS_READ = 3, IOS_WRITE = 4;
inline constexpr u32 IOS_SEEK = 5, IOS_IOCTL = 6, IOS_IOCTLV = 7, IOS_REPLY = 8;

inline constexpr s32 IPC_OK = 0, IPC_EACCES = -1, IPC_EEXIST = -2, IPC_EINVAL = -4;
inline constexpr s32 IPC_EMAXOPEN = -5, IPC_ENOENT = -6, IPC_EQUEUEFULL = -8, IPC_ENOMEM = -22;

// Operaciones de /dev/di; véase libogc gc/di/di.h y WiiBrew /dev/di.
inline constexpr u32 IOCTL_DVD_READ = 0x71;
inline constexpr u32 IOCTL_DVD_WAIT_FOR_COVER_CLOSE = 0x79;
inline constexpr u32 IOCTL_DVD_GET_COVER_STATUS = 0x88;
inline constexpr u32 IOCTL_DVD_SEEK = 0xab;
inline constexpr u32 IOCTL_DVD_REQUEST_ERROR = 0xe0;
inline constexpr u32 IOCTL_DVD_STOP_MOTOR = 0xe3;

// /dev/fs: libogc libogc/isfs.c.
inline constexpr u32 IOCTL_FS_CREATE_DIR = 3, IOCTL_FS_READ_DIR = 4;
inline constexpr u32 IOCTL_FS_SET_ATTR = 5, IOCTL_FS_GET_ATTR = 6;
inline constexpr u32 IOCTL_FS_DELETE = 7, IOCTL_FS_RENAME = 8, IOCTL_FS_CREATE = 9;
inline constexpr u32 IOCTL_FS_GET_FILE_STATS = 11, IOCTL_FS_GET_USAGE = 12, IOCTL_FS_SHUTDOWN = 13;
inline constexpr u32 FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_RW = 3;

inline constexpr u32 be32(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
}
inline constexpr void wbe32(u8* p, u32 value) {
    p[0] = u8(value >> 24);
    p[1] = u8(value >> 16);
    p[2] = u8(value >> 8);
    p[3] = u8(value);
}

// Vista del host después de validar la dirección y la longitud del vector invitado.
// En la RAM de Wii, cada descriptor consta de dos enteros BE de 32 bits.
struct IOVec { u8* data; u32 len; };
} // namespace ios
