// Núcleo IPC síncrono. Los mensajes y vectores de Wii están en big-endian.
#include "ios.h"
#include "guest_memory.h"
#include <array>
#include <utility>

namespace ios {
namespace {
constexpr u32 send = 1, ack = 2, response = 4;
constexpr u32 enable_response = 0x10, enable_ack = 0x20;
constexpr u32 ipc_interrupt = 1u << 30;
constexpr u32 request_size = 0x20;
}

Kernel& kernel() {
    static Kernel instance;
    return instance;
}

void Kernel::init(CPUState* cpu, IrqFn irq) {
    for (auto& s : slots_) {
        if (s.dev) s.dev->close(s.handle);
        s = {};
    }
    cpu_ = cpu;
    irq_ = irq;
    accepted_ = ppcmsg_ = ppcctrl_ = armmsg_ = irq_flags_ = 0;
    irq_mask_ = ipc_interrupt;
    update_irq();
}

void Kernel::register_device(const std::string& path, std::shared_ptr<Device> dev) {
    if (dev) devices_[path] = std::move(dev);
    else devices_.erase(path);
}

u8* Kernel::guest_ptr(u32 addr, u32 len) {
    return wii::resolve(cpu_, addr, len);
}

Kernel::Slot* Kernel::slot(s32 fd) {
    if (fd < 0 || fd >= 32 || !slots_[fd].dev) return nullptr;
    return &slots_[fd];
}

s32 Kernel::do_open(const char* path, u32 mode) {
    std::string name(path);
    auto it = devices_.find(name);
    if (it == devices_.end() && !name.empty() && name.front() == '/' &&
        name != "/dev" && name.compare(0, 5, "/dev/") != 0)
        it = devices_.find("/dev/fs");
    if (it == devices_.end()) return IPC_ENOENT;
    auto dev = it->second;
    for (s32 fd = 0; fd < 32; ++fd) {
        if (slots_[fd].dev) continue;
        const s32 handle = dev->open(path, mode);
        if (handle < 0) return handle;
        slots_[fd] = {dev, handle};
        return fd;
    }
    return IPC_EMAXOPEN;
}

s32 Kernel::do_ioctlv(s32 fd, u32 req, u32 nin, u32 nout, u32 address) {
    std::array<IOVec, 16> vectors{};
    Slot* s = slot(fd);
    if (!s || nin > vectors.size() || nout > vectors.size() - nin) return IPC_EINVAL;
    const u32 count = nin + nout;
    const u8* table = guest_ptr(address, count * 8);
    if (count && !table) return IPC_EINVAL;
    for (u32 i = 0; i < count; ++i) {
        auto& v = vectors[i];
        v.len = be32(table + 8*i + 4);
        if (v.len) {
            v.data = guest_ptr(be32(table + 8*i), v.len);
            if (!v.data) return IPC_EINVAL;
        }
    }
    return s->dev->ioctlv(s->handle, req, nin, nout, vectors.data());
}

void Kernel::process(u32 address) {
    u8* req = guest_ptr(address, request_size);
    if (!req) {
        reply(address, IPC_EINVAL);
        return;
    }
    const auto arg = [req](u32 i) { return be32(req + 12 + i * 4); };
    const s32 fd = s32(be32(req + 8));
    Slot* s = slot(fd);
    const u32 cmd = be32(req);
    s32 result = IPC_EINVAL;
    if (cmd == IOS_OPEN) {
        std::array<char, 64> path{};
        for (u32 i = 0; i < path.size(); ++i) {
            const u8* start = guest_ptr(arg(0), i + 1);
            if (!start) break;
            path[i] = char(start[i]);
            if (!path[i]) {
                result = do_open(path.data(), arg(1));
                break;
            }
        }
    } else if (s) {
        switch (cmd) {
        case IOS_CLOSE:
            result = s->dev->close(s->handle);
            *s = {};
            break;
        case IOS_READ:
        case IOS_WRITE: {
            u8* data = guest_ptr(arg(0), arg(1));
            if (data || !arg(1))
                result = cmd == IOS_READ ? s->dev->read(s->handle, data, arg(1))
                                        : s->dev->write(s->handle, data, arg(1));
            break;
        }
        case IOS_SEEK:
            result = s->dev->seek(s->handle, s32(arg(0)), arg(1));
            break;
        case IOS_IOCTL: {
            u8* in = arg(2) ? guest_ptr(arg(1), arg(2)) : nullptr;
            u8* out = arg(4) ? guest_ptr(arg(3), arg(4)) : nullptr;
            if ((!arg(2) || in) && (!arg(4) || out))
                result = s->dev->ioctl(s->handle, arg(0), in, arg(2), out, arg(4));
            break;
        }
        case IOS_IOCTLV:
            result = do_ioctlv(fd, arg(0), arg(1), arg(2), arg(3));
            break;
        }
    }
    reply(address, result);
}

void Kernel::reply(u32 address, s32 result) {
    if (u8* req = guest_ptr(address, request_size)) {
        wbe32(req + 8, be32(req));
        wbe32(req + 4, u32(result));
        wbe32(req, IOS_REPLY);
    }
    armmsg_ = address;
    ppcctrl_ |= response;
}

void Kernel::update_irq() {
    if (((ppcctrl_ & response) && (ppcctrl_ & enable_response)) ||
        ((ppcctrl_ & ack) && (ppcctrl_ & enable_ack)))
        irq_flags_ |= ipc_interrupt;
    if (irq_) irq_(cpu_);
}

u32 Kernel::reg_read(u32 addr) {
    switch (addr & 0xffffu) {
    case 0: return ppcmsg_;
    case 4: return ppcctrl_;
    case 8: return armmsg_;
    case 0x30: return irq_flags_;
    case 0x34: return irq_mask_;
    default: return 0;
    }
}

void Kernel::reg_write(u32 addr, u32 value) {
    switch (addr & 0xffffu) {
    case 0: ppcmsg_ = value; return;
    case 4: {
        const bool acknowledged = (ppcctrl_ & value & ack) != 0;
        ppcctrl_ &= ~(value & (ack | response));
        ppcctrl_ &= ~(enable_ack | enable_response);
        ppcctrl_ |= value & (enable_ack | enable_response | send);
        if (acknowledged) process(accepted_);
        // Mantener X1 pendiente mientras el PPC consume la respuesta anterior.
        if ((ppcctrl_ & (send | ack | response)) == send) {
            accepted_ = ppcmsg_;
            ppcctrl_ = (ppcctrl_ & ~send) | ack;
        }
        break;
    }
    case 0x30: irq_flags_ &= ~value; break;
    case 0x34: irq_mask_ = value & ipc_interrupt; break;
    default: return;
    }
    update_irq();
}
} // namespace ios
