#pragma once
#include "ios.h"
#include "wii_dvd.h"

namespace ios {
class DevDI final : public Device {
 public:
    explicit DevDI(wii::PakDisc& pak) : pak_(pak) {}
    s32 open(const char*, u32) override { return 0; }
    s32 ioctl(s32 handle, u32 request, u8* in, u32 in_len, u8* out, u32 out_len) override;
 private:
    wii::PakDisc& pak_;
};
}
