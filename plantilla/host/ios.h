// Despacho IPC y registro de dispositivos IOS.
#pragma once
#include "ios_constants.h"
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

struct CPUState;

namespace ios {

// Resultado de un dispositivo que contestará más tarde con Kernel::queue_reply (p. ej. /dev/stm/eventhook).
inline constexpr s32 IPC_NO_REPLY = INT32_MIN;

struct Device {
    virtual ~Device() = default;
    virtual s32 open(const char* path, u32 mode) = 0;
    virtual s32 close(s32) { return IPC_OK; }
    virtual s32 read(s32, u8*, u32) { return IPC_EINVAL; }
    virtual s32 write(s32, const u8*, u32) { return IPC_EINVAL; }
    virtual s32 seek(s32, s32, u32) { return IPC_EINVAL; }
    virtual s32 ioctl(s32, u32, u8*, u32, u8*, u32) { return IPC_EINVAL; }
    virtual s32 ioctlv(s32, u32, u32, u32, IOVec*) { return IPC_EINVAL; }
};

class Kernel {
 public:
    using IrqFn = void (*)(CPUState*);
    void init(CPUState* cpu, IrqFn irq);
    void register_device(const std::string& path, std::shared_ptr<Device> dev);

    // Registros IPC en 0xCD000000.
    u32 reg_read(u32 addr);
    void reg_write(u32 addr, u32 value);

    // Dirección física de la petición que se está procesando y respuesta diferida para ella.
    u32 current_request() const { return current_; }
    void queue_reply(u32 req_addr, s32 result);
    // Un juego pidió apagar o volver al menú (STM).
    bool shutdown_requested() const { return shutdown_; }
    void request_shutdown() { shutdown_ = true; }
    // Aviso de que IOS escribió en memoria del juego (lecturas de disco o NAND): el renderer descarta lo que
    // tuviera de esa zona (texturas). Recibe la dirección virtual (0x8.../0x9...).
    void set_written_hook(void (*hook)(u32 addr, u32 len)) { written_ = hook; }

 private:
    struct Slot { std::shared_ptr<Device> dev; s32 handle = -1; };
    CPUState* cpu_ = nullptr;
    IrqFn irq_ = nullptr;
    u32 ppcmsg_ = 0, ppcctrl_ = 0, armmsg_ = 0;
    Slot slots_[32];
    std::unordered_map<std::string, std::shared_ptr<Device>> devices_;

    u32 accepted_ = 0, irq_flags_ = 0, irq_mask_ = 0, current_ = 0;
    bool shutdown_ = false;
    void (*written_)(u32, u32) = nullptr;
    std::deque<std::pair<u32, s32>> replies_;
    void written(u32 addr, u32 len) { if (written_ && len) written_(addr | 0x80000000u, len); }
    void update_irq();
    void deliver();
    u8* guest_ptr(u32 addr, u32 len);
    void process(u32 req_addr);
    void reply(u32 req_addr, s32 result);
    s32 do_open(const char* path, u32 mode);
    Slot* slot(s32 fd);
    s32 do_ioctlv(s32 fd, u32 req, u32 nin, u32 nout, u32 vec_addr);
};

Kernel& kernel();
} // namespace ios
