// STM: el SDK deja una petición en /dev/stm/eventhook que IOS solo contesta al pulsar POWER/RESET o cuando el
// juego la libera (RELEASE_EH); contestarla al momento haría que el juego se apagara. Números de Dolphin (IOS/STM).
// ES: NANDInit pide el ID del título y su carpeta de datos (/title/00010000/<ID>/data) para las partidas.
#include "dev_misc.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace ios {
namespace {
constexpr u32 STM_EVENTHOOK = 0x1000, STM_HOTRESET = 0x2001, STM_HOTRESET_FOR_PD = 0x2002, STM_SHUTDOWN = 0x2003;
constexpr u32 STM_RELEASE_EH = 0x3002;

struct StmState { u32 pending = 0; };

class DevStmEventHook final : public Device {
 public:
    explicit DevStmEventHook(std::shared_ptr<StmState> s) : s_(std::move(s)) {}
    s32 open(const char*, u32) override { return 0; }
    s32 ioctl(s32, u32 request, u8*, u32, u8*, u32) override {
        if (request != STM_EVENTHOOK) return IPC_EINVAL;
        if (s_->pending) return IPC_EEXIST;
        s_->pending = kernel().current_request();
        return IPC_NO_REPLY;
    }
 private:
    std::shared_ptr<StmState> s_;
};

class DevStmImmediate final : public Device {
 public:
    explicit DevStmImmediate(std::shared_ptr<StmState> s) : s_(std::move(s)) {}
    s32 open(const char*, u32) override { return 0; }
    s32 ioctl(s32, u32 request, u8*, u32, u8* out, u32 out_len) override {
        if (out && out_len) std::memset(out, 0, out_len);
        switch (request) {
        case STM_RELEASE_EH:
            if (!s_->pending) return IPC_ENOENT;
            kernel().queue_reply(s_->pending, IPC_OK);
            s_->pending = 0;
            return IPC_OK;
        case STM_HOTRESET: case STM_HOTRESET_FOR_PD: case STM_SHUTDOWN:
            std::fprintf(stderr, "[stm] el juego pide %s\n", request == STM_SHUTDOWN ? "apagar" : "reiniciar");
            kernel().request_shutdown();
            return IPC_OK;
        default:
            return IPC_OK;  // LED, atenuado de vídeo, versión: sin efecto aquí
        }
    }
 private:
    std::shared_ptr<StmState> s_;
};

class DevES final : public Device {
 public:
    explicit DevES(std::uint64_t title) : title_(title) {}
    s32 open(const char*, u32) override { return 0; }
    s32 ioctlv(s32, u32 request, u32 nin, u32 nout, IOVec* v) override {
        IOVec* out = v + nin;
        for (u32 i = 0; i < nout; ++i) if (out[i].data && out[i].len) std::memset(out[i].data, 0, out[i].len);
        switch (request) {
        case 0x20:  // GetTitleID
            if (nout < 1 || out[0].len < 8) return IPC_EINVAL;
            put64(out[0].data, title_);
            return IPC_OK;
        case 0x1D: {  // GetTitleDir
            if (nin < 1 || nout < 1 || v[0].len < 8 || out[0].len < 30) return IPC_EINVAL;
            const std::uint64_t t = (std::uint64_t(be32(v[0].data)) << 32) | be32(v[0].data + 4);
            char path[32];
            std::snprintf(path, sizeof path, "/title/%08x/%08x/data", u32(t >> 32), u32(t));
            std::memcpy(out[0].data, path, std::min<std::size_t>(out[0].len, std::strlen(path) + 1));
            return IPC_OK;
        }
        case 0x07:  // GetDeviceID
            if (nout >= 1 && out[0].len >= 4) wbe32(out[0].data, 0x0403AC68u);
            return IPC_OK;
        case 0x21:  // SetUID
            return IPC_OK;
        default: {
            static std::set<u32> logged;
            if (logged.insert(request).second)
                std::fprintf(stderr, "[es] ioctlv %02x sin implementar (salidas a cero)\n", request);
            return IPC_OK;
        }
        }
    }
 private:
    static void put64(u8* p, std::uint64_t v) { wbe32(p, u32(v >> 32)); wbe32(p + 4, u32(v)); }
    std::uint64_t title_;
};
}  // namespace

void register_misc_devices(Kernel& k, std::uint64_t title_id) {
    auto stm = std::make_shared<StmState>();
    k.register_device("/dev/stm/eventhook", std::make_shared<DevStmEventHook>(stm));
    k.register_device("/dev/stm/immediate", std::make_shared<DevStmImmediate>(stm));
    k.register_device("/dev/es", std::make_shared<DevES>(title_id));
}
}  // namespace ios
