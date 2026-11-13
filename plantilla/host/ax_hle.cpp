// SPDX-License-Identifier: GPL-2.0-or-later
// ax_hle.cpp — ver ax_hle.h. La lógica, el orden de las operaciones y la aritmética son los de Dolphin
// (AXWii.cpp, AX.cpp, AXVoice.h con AX_WII, DSPAccelerator.cpp). Cambios para este host:
//  * memoria por Env::span, en big endian a mano; el acelerador lee la "ARAM" de Wii (bit 28: MEM2, si no MEM1);
//  * las versiones de AX que Dolphin no reconoce leen la PB con la disposición más reciente (sin "updates") y lo
//    avisan una vez, en vez de dejarla sin leer;
//  * un comando desconocido termina la lista (Dolphin seguiría leyendo fuera del búfer);
//  * sin coeficientes polifásicos (no hay DROM): remuestreo lineal, como Dolphin sin dsp_coef.bin.
#include "ax_hle.h"
#include "ax_structs.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ax {
namespace {

using u8 = uint8_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;

#define HILO_TO_32(name) ((u32(name##_hi) << 16) | name##_lo)

enum AXMixControl : u32 {
    MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000, MIX_AUXC_L = 0x040000, MIX_AUXC_L_RAMP = 0x080000,
    MIX_AUXC_R = 0x100000, MIX_AUXC_R_RAMP = 0x200000, MIX_AUXC_S = 0x400000, MIX_AUXC_S_RAMP = 0x800000,
};

enum CmdType {
    CMD_SETUP = 0x00, CMD_ADD_TO_LR = 0x01, CMD_SUB_TO_LR = 0x02, CMD_ADD_SUB_TO_LR = 0x03, CMD_PROCESS = 0x04,
    CMD_MIX_AUXA = 0x05, CMD_MIX_AUXB = 0x06, CMD_MIX_AUXC = 0x07, CMD_UPL_AUXA_MIX_LRSC = 0x08,
    CMD_UPL_AUXB_MIX_LRSC = 0x09, CMD_COMPRESSOR = 0x0A, CMD_OUTPUT = 0x0B, CMD_OUTPUT_DPL2 = 0x0C,
    CMD_WM_OUTPUT = 0x0D, CMD_END = 0x0E,
};
enum CmdTypeOld {
    CMD_SETUP_OLD = 0x00, CMD_ADD_TO_LR_OLD = 0x01, CMD_SUB_TO_LR_OLD = 0x02, CMD_ADD_SUB_TO_LR_OLD = 0x03,
    CMD_PB_ADDR_OLD = 0x04, CMD_PROCESS_OLD = 0x05, CMD_MIX_AUXA_OLD = 0x06, CMD_MIX_AUXB_OLD = 0x07,
    CMD_MIX_AUXC_OLD = 0x08, CMD_UPL_AUXA_MIX_LRSC_OLD = 0x09, CMD_UPL_AUXB_MIX_LRSC_OLD = 0x0a,
    CMD_COMPRESSOR_OLD = 0x0B, CMD_OUTPUT_OLD = 0x0C, CMD_OUTPUT_DPL2_OLD = 0x0D, CMD_WM_OUTPUT_OLD = 0x0E,
    CMD_END_OLD = 0x0F,
};

constexpr u32 DSP_SYNC = 0xDCD10004;

s16 ClampS16(s64 sample) { return s16(std::clamp<s64>(sample, -0x8000, 0x7FFF)); }

enum class Layout { Old, Mid, New };

// ---------------------------------------------------------------- memoria
struct Mem {
    Env env;
    u8* ptr(u32 addr, size_t n) const { return env.span ? env.span(addr, n) : nullptr; }
    void read16s(void* dst, u32 addr, size_t bytes) const {
        u8* d = static_cast<u8*>(dst);
        const u8* p = ptr(addr, bytes);
        if (!p) { std::memset(d, 0, bytes); return; }
        for (size_t i = 0; i + 1 < bytes; i += 2) { const u16 v = u16((p[i] << 8) | p[i + 1]); std::memcpy(d + i, &v, 2); }
    }
    void write16s(u32 addr, const void* src, size_t bytes) const {
        const u8* s = static_cast<const u8*>(src);
        u8* p = ptr(addr, bytes);
        if (!p) return;
        for (size_t i = 0; i + 1 < bytes; i += 2) { u16 v; std::memcpy(&v, s + i, 2); p[i] = u8(v >> 8); p[i + 1] = u8(v); }
        written(addr, bytes);
    }
    void write32s(u32 addr, const int* src, size_t count) const {
        u8* p = ptr(addr, count * 4);
        if (!p) return;
        for (size_t i = 0; i < count; ++i) {
            const u32 v = u32(src[i]);
            p[4 * i] = u8(v >> 24); p[4 * i + 1] = u8(v >> 16); p[4 * i + 2] = u8(v >> 8); p[4 * i + 3] = u8(v);
        }
        written(addr, count * 4);
    }
    void read32s(int* dst, u32 addr, size_t count) const {
        const u8* p = ptr(addr, count * 4);
        for (size_t i = 0; i < count; ++i)
            dst[i] = p ? int(u32((p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3])) : 0;
    }
    void written(u32 addr, size_t n) const {
        if (env.mem_written) env.mem_written(addr | 0x80000000u, n);
    }
    // "ARAM" de Wii para el acelerador (DSPManager::ReadARAM en modo Wii).
    u8 aram(u32 address) const {
        const u32 a = (address & 0x10000000u) ? (0x10000000u | (address & 0x03FFFFFFu)) : (address & 0x01FFFFFFu);
        const u8* p = ptr(a, 1);
        return p ? *p : 0;
    }
};

// ---------------------------------------------------------------- acelerador (DSPAccelerator.cpp)
class Accelerator {
 public:
    explicit Accelerator(const Mem& mem) : mem_(mem) {}
    AXPBWii* acc_pb = nullptr;

    void SetStartAddress(u32 a) { start_ = a & 0x3fffffff; }
    void SetEndAddress(u32 a) { end_ = a & 0x3fffffff; }
    void SetCurrentAddress(u32 a) { cur_ = a & 0xbfffffff; }
    void SetSampleFormat(u16 f) { format_ = f; }
    void SetGain(s16 g) { gain_ = g; }
    void SetYn1(s16 v) { yn1_ = v; }
    void SetYn2(s16 v) { yn2_ = v; stopped_ = false; }
    void SetPredScale(u16 p) { pred_scale_ = p & 0x7f; }
    u32 GetCurrentAddress() const { return cur_; }
    s16 GetYn1() const { return yn1_; }
    s16 GetYn2() const { return yn2_; }
    u16 GetPredScale() const { return pred_scale_; }

    u16 ReadSample(const s16* coefs) {
        if (stopped_) return 0;
        const u16 size = format_ & 3, decode = (format_ >> 2) & 3, gain_scale = (format_ >> 4) & 3;
        u16 val = 0;
        u8 step = 0;
        s16 raw = (decode == 1 || decode == 3) ? s16(input_) : s16(current_sample(size));
        const int ci = (pred_scale_ >> 4) & 7;
        const s32 c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
        if (decode == 0) {  // ADPCM
            raw &= 0xF;
            const int scale = 1 << (pred_scale_ & 0xF);
            if (raw >= 8) raw = s16(raw - 16);
            const s32 v = (scale * raw) + ((0x400 + c1 * yn1_ + c2 * yn2_) >> 11);
            val = u16(std::clamp(v, -0x8000, 0x7FFF));
            step = 2;
            yn2_ = yn1_;
            yn1_ = s16(val);
            cur_ += 1;
            if ((end_ & 0xf) == 0x0 && cur_ == end_) {
                cur_ = start_ + 1;
            } else if ((end_ & 0xf) == 0x1 && cur_ == end_ - 1) {
                cur_ = start_;
            } else if ((cur_ & 15) == 0) {
                pred_scale_ = mem_.aram((cur_ & ~15u) >> 1);
                cur_ += 2;
                step += 2;
            }
        } else {  // PCM (o MMIO)
            const u8 shift = gain_scale == 0 ? 11 : gain_scale == 1 ? 0 : gain_scale == 2 ? 16 : 0;
            const s32 v = ((s32(gain_) * raw) >> shift) + (((c1 * yn1_) >> shift) + ((c2 * yn2_) >> shift));
            val = u16(s16(v));
            yn2_ = yn1_;
            yn1_ = s16(val);
            step = 2;
            if (decode != 1) cur_ += 1;
        }
        if (cur_ == end_ + step - 1) {
            cur_ = start_;
            stopped_ = true;
            on_end();
        }
        SetCurrentAddress(cur_);
        return val;
    }

 private:
    u16 current_sample(u16 size) const {
        u16 v = 0;
        switch (size) {
        case 0: v = mem_.aram(cur_ >> 1); v = (cur_ & 1) ? (v & 0xf) : (v >> 4); break;
        case 1: v = mem_.aram(cur_); break;
        case 2: v = u16((mem_.aram(cur_ * 2) << 8) | mem_.aram(cur_ * 2 + 1)); break;
        default: break;
        }
        return v;
    }
    // HLEAccelerator::OnSampleReadEndException (AXVoice.h, AX_WII)
    void on_end() {
        if (acc_pb->audio_addr.looping) {
            SetPredScale(acc_pb->adpcm_loop_info.pred_scale);
            if (acc_pb->is_stream != 1) {
                SetYn1(s16(acc_pb->adpcm_loop_info.yn1));
                SetYn2(s16(acc_pb->adpcm_loop_info.yn2));
            } else {
                SetYn1(GetYn1());
                SetYn2(GetYn2());
            }
        } else {
            acc_pb->running = 0;
        }
    }

    const Mem& mem_;
    u32 start_ = 0, end_ = 0, cur_ = 0;
    u16 format_ = 0, pred_scale_ = 0, input_ = 0;
    s16 gain_ = 0, yn1_ = 0, yn2_ = 0;
    bool stopped_ = false;
};

// ---------------------------------------------------------------- voces (AXVoice.h)
union AXBuffers {
    struct {
        int *main_left, *main_right, *main_surround;
        int *auxA_left, *auxA_right, *auxA_surround;
        int *auxB_left, *auxB_right, *auxB_surround;
        int *auxC_left, *auxC_right, *auxC_surround;
        int *wm_main0, *wm_aux0, *wm_main1, *wm_aux1, *wm_main2, *wm_aux2, *wm_main3, *wm_aux3;
    };
    struct {
        int* regular_ptrs[12];
        int* wiimote_ptrs[8];
    };
};

PBUpdateData LoadPBUpdates(const Mem& mem, const AXPBWii& pb) {
    PBUpdateData updates;
    mem.read16s(updates.data(), HILO_TO_32(pb.updates.data), sizeof(updates));
    return updates;
}

void ApplyUpdatesForMs(int curr_ms, AXPBWii& pb, u16* num_updates, const PBUpdateData& updates) {
    u32 start = 0;
    for (int i = 0; i < curr_ms; ++i) start += num_updates[i];
    if (start >= updates.size()) return;
    const u16 count = num_updates[curr_ms];
    if (count > updates.size() - start) return;
    for (u32 i = start; i < start + count; ++i) {
        const u16 off = updates[i].pb_offset, val = updates[i].new_value;
        if (off < sizeof(pb) / sizeof(u16)) reinterpret_cast<u16*>(&pb)[off] = val;
    }
}

template <typename Input>
u32 ResampleAudio(Input input, s16* output, u32 count, s16* last, u32 curr_pos, u32 ratio, int srctype,
                  const s16* coeffs) {
    int read = 0;
    if (coeffs && srctype == SRCTYPE_POLYPHASE) {
        s16 temp[4];
        u32 idx = 0;
        for (int k = 0; k < 4; ++k) temp[idx++ & 3] = last[k];
        for (u32 i = 0; i < count; ++i) {
            curr_pos += ratio;
            while (curr_pos >= 0x10000) { temp[idx++ & 3] = input(read++); curr_pos -= 0x10000; }
            const u16 frac = u16(((curr_pos & 0xFFFF) >> 9) << 2);
            const s16* c = &coeffs[frac];
            const s64 t0 = temp[idx++ & 3], t1 = temp[idx++ & 3], t2 = temp[idx++ & 3], t3 = temp[idx++ & 3];
            output[i] = ClampS16((t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15);
        }
        last[3] = temp[--idx & 3]; last[2] = temp[--idx & 3]; last[1] = temp[--idx & 3]; last[0] = temp[--idx & 3];
    } else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
        s16 temp[4];
        u32 idx = 0;
        for (int k = 0; k < 4; ++k) temp[idx++ & 3] = last[k];
        for (u32 i = 0; i < count; ++i) {
            curr_pos += ratio;
            while (curr_pos >= 0x10000) { temp[idx++ & 3] = input(read++); curr_pos -= 0x10000; }
            const u16 frac = u16(curr_pos & 0xFFFF), inv = u16(-frac);
            s16 sample;
            if (frac) {
                const s32 s0 = temp[idx++ & 3], s1 = temp[idx++ & 3];
                sample = s16(((s0 * inv) + (s1 * frac)) >> 16);
                idx += 2;
            } else {
                sample = temp[idx++ & 3];
                idx += 3;
            }
            output[i] = sample;
        }
        last[3] = temp[--idx & 3]; last[2] = temp[--idx & 3]; last[1] = temp[--idx & 3]; last[0] = temp[--idx & 3];
    } else {  // SRCTYPE_NEAREST
        for (u32 i = 0; i < count; ++i) output[i] = input(i);
        std::memcpy(last, output + count - 4, 4 * sizeof(u16));
    }
    return curr_pos;
}

void GetInputSamples(Accelerator& acc, AXPBWii& pb, s16* samples, u16 count, const s16* coeffs) {
    acc.acc_pb = &pb;
    acc.SetStartAddress(HILO_TO_32(pb.audio_addr.loop_addr));
    acc.SetEndAddress(HILO_TO_32(pb.audio_addr.end_addr));
    acc.SetCurrentAddress(HILO_TO_32(pb.audio_addr.cur_addr));
    acc.SetSampleFormat(pb.audio_addr.sample_format);
    acc.SetYn1(pb.adpcm.yn1);
    acc.SetYn2(pb.adpcm.yn2);
    acc.SetGain(s16(pb.adpcm.gain));
    acc.SetPredScale(pb.adpcm.pred_scale);
    if (coeffs) coeffs += pb.coef_select * 0x200;
    const u32 pos = ResampleAudio([&acc, &pb](u32) { return s16(acc.ReadSample(pb.adpcm.coefs)); }, samples, count,
                                  pb.src.last_samples, pb.src.cur_addr_frac, HILO_TO_32(pb.src.ratio), pb.src_type,
                                  coeffs);
    pb.src.cur_addr_frac = u16(pos & 0xFFFF);
    pb.audio_addr.cur_addr_hi = u16(acc.GetCurrentAddress() >> 16);
    pb.audio_addr.cur_addr_lo = u16(acc.GetCurrentAddress());
    pb.adpcm.yn1 = acc.GetYn1();
    pb.adpcm.yn2 = acc.GetYn2();
    pb.adpcm.pred_scale = acc.GetPredScale();
}

void MixAdd(int* out, const s16* input, u32 count, VolumeData* vd, s16* dpop, bool ramp) {
    u16& volume = vd->volume;
    const u16 delta = ramp ? vd->volume_delta : 0;
    for (u32 i = 0; i < count; ++i) {
        s64 sample = input[i];
        sample *= volume;
        sample >>= 15;
        const s16 s = ClampS16(s32(sample));
        out[i] += s;
        volume = u16(volume + delta);
        *dpop = s;
    }
}

void LowPassFilter(s16* samples, u32 count, PBLowPassFilter& f) {
    for (u32 i = 0; i < count; ++i)
        f.yn1 = samples[i] = ClampS16((f.a0 * s32(samples[i]) + f.b0 * s32(f.yn1)) >> 15);
}

void BiquadFilter(s16* samples, u32 count, PBBiquadFilter& f) {
    for (u32 i = 0; i < count; ++i) {
        const s16 xn0 = samples[i];
        s64 tmp = 0;
        tmp += f.b0 * s32(xn0);
        tmp += f.b1 * s32(f.xn1);
        tmp += f.b2 * s32(f.xn2);
        tmp += f.a1 * s32(f.yn1);
        tmp += f.a2 * s32(f.yn2);
        tmp *= 4;
        if (tmp & 0x10000) tmp += 0x8000;
        else tmp += 0x7FFF;
        tmp >>= 16;
        const s16 yn0 = ClampS16(tmp);
        f.xn2 = f.xn1; f.yn2 = f.yn1; f.xn1 = xn0; f.yn1 = yn0;
        samples[i] = yn0;
    }
}

void ProcessVoice(Accelerator& acc, AXPBWii& pb, const AXBuffers& b, u16 count, u32 mctrl, const s16* coeffs,
                  bool new_filter) {
    if (pb.running != 1) return;
    s16 samples[96];
    GetInputSamples(acc, pb, samples, count, coeffs);
    for (u32 i = 0; i < count; ++i) {
        const s32 volume = u16(pb.vol_env.cur_volume);  // sin signo en Wii
        samples[i] = ClampS16((s32(samples[i]) * volume) >> 15);
        pb.vol_env.cur_volume = s16(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
    }
    if (pb.lpf.on != 0) LowPassFilter(samples, count, pb.lpf);
    if (new_filter && pb.biquad.on != 0) BiquadFilter(samples, count, pb.biquad);

#define MIXC(C, buf, field) \
    if (mctrl & MIX_##C) MixAdd(b.buf, samples, count, &pb.mixer.field, &pb.dpop.field, (mctrl & MIX_##C##_RAMP) != 0)
    MIXC(MAIN_L, main_left, main_left);
    MIXC(MAIN_R, main_right, main_right);
    MIXC(MAIN_S, main_surround, main_surround);
    MIXC(AUXA_L, auxA_left, auxA_left);
    MIXC(AUXA_R, auxA_right, auxA_right);
    MIXC(AUXA_S, auxA_surround, auxA_surround);
    MIXC(AUXB_L, auxB_left, auxB_left);
    MIXC(AUXB_R, auxB_right, auxB_right);
    MIXC(AUXB_S, auxB_surround, auxB_surround);
    MIXC(AUXC_L, auxC_left, auxC_left);
    MIXC(AUXC_R, auxC_right, auxC_right);
    MIXC(AUXC_S, auxC_surround, auxC_surround);
#undef MIXC

    if (pb.remote) {  // altavoces de los Wiimote
        if (new_filter && pb.remote_iir.on != 0) {
            if (pb.remote_iir.on == 2) BiquadFilter(samples, count, pb.remote_iir.biquad);
            else LowPassFilter(samples, count, pb.remote_iir.lpf);
        }
        const u16 wm_count = count == 96 ? 18 : 6;
        s16 wm[18];
        const u32 pos = ResampleAudio([&samples](u32 i) { return samples[i]; }, wm, wm_count,
                                      pb.remote_src.last_samples, pb.remote_src.cur_addr_frac, 0x55555,
                                      SRCTYPE_POLYPHASE, coeffs);
        pb.remote_src.cur_addr_frac = u16(pos & 0xFFFF);
        int* outs[8] = {b.wm_main0, b.wm_aux0, b.wm_main1, b.wm_aux1, b.wm_main2, b.wm_aux2, b.wm_main3, b.wm_aux3};
        VolumeData* vols[8] = {&pb.remote_mixer.main0, &pb.remote_mixer.aux0, &pb.remote_mixer.main1,
                               &pb.remote_mixer.aux1, &pb.remote_mixer.main2, &pb.remote_mixer.aux2,
                               &pb.remote_mixer.main3, &pb.remote_mixer.aux3};
        s16* dpops[8] = {&pb.remote_dpop.main0, &pb.remote_dpop.aux0, &pb.remote_dpop.main1, &pb.remote_dpop.aux1,
                         &pb.remote_dpop.main2, &pb.remote_dpop.aux2, &pb.remote_dpop.main3, &pb.remote_dpop.aux3};
        for (int n = 0; n < 8; ++n) {
            const u16 bits = (pb.remote_mixer_control >> (2 * n)) & 3;
            if (bits) MixAdd(outs[n], wm, wm_count, vols[n], dpops[n], (bits & 2) != 0);
        }
    }
}

}  // namespace

// ================================================================ AXWii
struct Wii::State {
    Mem mem;
    Accelerator acc{mem};
    u32 crc = 0;
    Layout layout = Layout::New;
    bool old_axwii = false, new_filter = false;
    u16 last_main_volume = 0x8000;
    u16 last_aux_volumes[3] = {0x8000, 0x8000, 0x8000};
    u16 compressor_pos = 0;
    u16 cmdlist[512] = {};

    int main_left[32 * 5]{}, main_right[32 * 5]{}, main_surround[32 * 5]{};
    int auxA_left[32 * 5]{}, auxA_right[32 * 5]{}, auxA_surround[32 * 5]{};
    int auxB_left[32 * 5]{}, auxB_right[32 * 5]{}, auxB_surround[32 * 5]{};
    int auxC_left[32 * 3]{}, auxC_right[32 * 3]{}, auxC_surround[32 * 3]{};
    int wm0[18]{}, aux0[18]{}, wm1[18]{}, aux1[18]{}, wm2[18]{}, aux2[18]{}, wm3[18]{}, aux3[18]{};

    struct BufferDesc { int* ptr; int samples_per_milli; };

    void SetupProcessing(u32 init_addr) {
        const BufferDesc buffers[20] = {
            {main_left, 32}, {main_right, 32}, {main_surround, 32}, {auxA_left, 32}, {auxA_right, 32},
            {auxA_surround, 32}, {auxB_left, 32}, {auxB_right, 32}, {auxB_surround, 32}, {auxC_left, 32},
            {auxC_right, 32}, {auxC_surround, 32}, {wm0, 6}, {aux0, 6}, {wm1, 6}, {aux1, 6}, {wm2, 6}, {aux2, 6},
            {wm3, 6}, {aux3, 6}};
        u16 init[3 * 20];
        mem.read16s(init, init_addr, sizeof(init));
        for (size_t i = 0; i < 20; ++i) {
            const s32 value = s32((u32(init[3 * i]) << 16) | init[3 * i + 1]);
            const s16 delta = s16(init[3 * i + 2]);
            const int n = 3 * buffers[i].samples_per_milli;
            if (value == 0) std::memset(buffers[i].ptr, 0, n * sizeof(int));
            else for (int j = 0; j < n; ++j) buffers[i].ptr[j] = value + j * delta;
        }
    }

    void AddToLR(u32 addr, bool neg) {
        int v[96];
        mem.read32s(v, addr, 96);
        for (int i = 0; i < 96; ++i) {
            const int val = neg ? -v[i] : v[i];
            main_left[i] += val;
            main_right[i] += val;
        }
    }

    void AddSubToLR(u32 addr) {
        int v[192];
        mem.read32s(v, addr, 192);
        for (int i = 0; i < 96; ++i) main_left[i] += v[i];
        for (int i = 0; i < 96; ++i) main_right[i] -= v[96 + i];
    }

    static u32 ConvertMixerControl(u32 mc) {
        u32 r = 0;
        if (mc & 0x00000001) r |= MIX_MAIN_L;
        if (mc & 0x00000002) r |= MIX_MAIN_R;
        if (mc & 0x00000004) r |= MIX_MAIN_L | MIX_MAIN_R | MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP;
        if (mc & 0x00000008) r |= MIX_MAIN_S;
        if (mc & 0x00000010) r |= MIX_MAIN_S | MIX_MAIN_S_RAMP;
        if (mc & 0x00010000) r |= MIX_AUXA_L;
        if (mc & 0x00020000) r |= MIX_AUXA_R;
        if (mc & 0x00040000) r |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
        if (mc & 0x00080000) r |= MIX_AUXA_S;
        if (mc & 0x00100000) r |= MIX_AUXA_S | MIX_AUXA_S_RAMP;
        if (mc & 0x00200000) r |= MIX_AUXB_L;
        if (mc & 0x00400000) r |= MIX_AUXB_R;
        if (mc & 0x00800000) r |= MIX_AUXB_L | MIX_AUXB_R | MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
        if (mc & 0x01000000) r |= MIX_AUXB_S;
        if (mc & 0x02000000) r |= MIX_AUXB_S | MIX_AUXB_S_RAMP;
        if (mc & 0x04000000) r |= MIX_AUXC_L;
        if (mc & 0x08000000) r |= MIX_AUXC_R;
        if (mc & 0x10000000) r |= MIX_AUXC_L | MIX_AUXC_R | MIX_AUXC_L_RAMP | MIX_AUXC_R_RAMP;
        if (mc & 0x20000000) r |= MIX_AUXC_S;
        if (mc & 0x40000000) r |= MIX_AUXC_S | MIX_AUXC_S_RAMP;
        return r;
    }

    static void GenerateVolumeRamp(u16* out, u16 vol1, u16 vol2, size_t n) {
        float curr = vol1;
        for (size_t i = 0; i < n; ++i) {
            curr += (vol2 - vol1) / float(n);
            out[i] = u16(curr);
        }
    }

    // Las tres disposiciones de la PB de Wii (ReadPB/WritePB de AXWii.cpp).
    void ReadPB(u32 addr, AXPBWii& pb) const {
        char* dst = reinterpret_cast<char*>(&pb);
        constexpr size_t ub = offsetof(AXPBWii, updates), ue = ub + sizeof(PBUpdatesWii);
        constexpr size_t gb = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
        constexpr size_t ge = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
        switch (layout) {
        case Layout::Old:
            mem.read16s(dst, addr, gb);
            std::memset(dst + gb, 0, ge - gb);
            mem.read16s(dst + ge, addr + gb, sizeof(pb) - ge);
            break;
        case Layout::Mid:
            mem.read16s(dst, addr, ub);
            std::memset(dst + ub, 0, sizeof(PBUpdatesWii));
            mem.read16s(dst + ue, addr + ub, gb - ue);
            std::memset(dst + gb, 0, ge - gb);
            mem.read16s(dst + ge, addr + gb, sizeof(pb) - ge);
            break;
        case Layout::New:
            mem.read16s(dst, addr, ub);
            std::memset(dst + ub, 0, sizeof(PBUpdatesWii));
            mem.read16s(dst + ue, addr + ub, sizeof(pb) - ue);
            break;
        }
    }

    void WritePB(u32 addr, const AXPBWii& pb) const {
        const char* src = reinterpret_cast<const char*>(&pb);
        constexpr size_t ub = offsetof(AXPBWii, updates), ue = ub + sizeof(PBUpdatesWii);
        constexpr size_t gb = offsetof(AXPBWii, hpf) + sizeof(PBHighPassFilter);
        constexpr size_t ge = offsetof(AXPBWii, biquad) + sizeof(PBBiquadFilter);
        switch (layout) {
        case Layout::Old:
            mem.write16s(addr, src, gb);
            mem.write16s(addr + gb, src + ge, sizeof(pb) - ge);
            break;
        case Layout::Mid:
            mem.write16s(addr, src, ub);
            mem.write16s(addr + ub, src + ue, gb - ue);
            mem.write16s(addr + gb, src + ge, sizeof(pb) - ge);
            break;
        case Layout::New:
            mem.write16s(addr, src, ub);
            mem.write16s(addr + ub, src + ue, sizeof(pb) - ue);
            break;
        }
    }

    u32 ProcessPBList(u32 pb_addr) {
        constexpr u32 spms = 32;
        u32 voices = 0;
        AXPBWii pb;
        for (int guard = 0; pb_addr && guard < 4096; ++guard) {
            AXBuffers buffers = {{main_left, main_right, main_surround, auxA_left, auxA_right, auxA_surround,
                                  auxB_left, auxB_right, auxB_surround, auxC_left, auxC_right, auxC_surround,
                                  wm0, aux0, wm1, aux1, wm2, aux2, wm3, aux3}};
            ReadPB(pb_addr, pb);
            if (pb.running == 1) ++voices;
            if (old_axwii && (pb.updates.num_updates[0] | pb.updates.num_updates[1] | pb.updates.num_updates[2])) {
                const PBUpdateData updates = LoadPBUpdates(mem, pb);
                for (int ms = 0; ms < 3; ++ms) {
                    ApplyUpdatesForMs(ms, pb, pb.updates.num_updates, updates);
                    ProcessVoice(acc, pb, buffers, spms, ConvertMixerControl(HILO_TO_32(pb.mixer_control)), nullptr,
                                 new_filter);
                    for (auto& p : buffers.regular_ptrs) p += spms;
                    for (auto& p : buffers.wiimote_ptrs) p += 6;
                }
            } else {
                ProcessVoice(acc, pb, buffers, 96, ConvertMixerControl(HILO_TO_32(pb.mixer_control)), nullptr,
                             new_filter);
            }
            WritePB(pb_addr, pb);
            pb_addr = HILO_TO_32(pb.next_pb);
        }
        return voices;
    }

    void MixAUXSamples(int aux_id, u32 write_addr, u32 read_addr, u16 volume) {
        u16 ramp[96];
        GenerateVolumeRamp(ramp, last_aux_volumes[aux_id], volume, 96);
        last_aux_volumes[aux_id] = volume;
        int* mains[3] = {main_left, main_right, main_surround};
        const int* bufs[3] = {auxA_left, auxA_right, auxA_surround};
        if (aux_id == 1) { bufs[0] = auxB_left; bufs[1] = auxB_right; bufs[2] = auxB_surround; }
        if (aux_id == 2) { bufs[0] = auxC_left; bufs[1] = auxC_right; bufs[2] = auxC_surround; }
        if (write_addr) {
            for (const int* b : bufs) {
                mem.write32s(write_addr, b, 96);
                write_addr += 96 * sizeof(int);
            }
        }
        int in[3 * 96];
        mem.read32s(in, read_addr, 3 * 96);
        for (int c = 0; c < 3; ++c)
            for (u32 j = 0; j < 96; ++j) {
                s64 sample = s64(s32(in[c * 96 + j]));
                sample *= ramp[j];
                mains[c][j] += s32(sample >> 15);
            }
    }

    void UploadAUXMixLRSC(int aux_id, const u32* a, u16 volume) {
        int* l = aux_id ? auxB_left : auxA_left;
        int* r = aux_id ? auxB_right : auxA_right;
        int* s = aux_id ? auxB_surround : auxA_surround;
        int* c = aux_id ? auxC_surround : auxC_right;
        mem.write32s(a[0], l, 96);
        mem.write32s(a[0] + 96 * sizeof(int), r, 96);
        mem.write32s(a[0] + 2 * 96 * sizeof(int), s, 96);
        mem.write32s(a[1], c, 96);
        u16 ramp[96];
        GenerateVolumeRamp(ramp, last_aux_volumes[aux_id], volume, 96);
        last_aux_volumes[aux_id] = volume;
        int* dest[4] = {main_left, main_right, main_surround, auxC_left};
        for (u32 m = 0; m < 4; ++m) {
            mem.read32s(l, a[2 + m], 96);
            for (u32 i = 0; i < 96; ++i) {
                s64 sample = s64(s32(l[i]));
                sample *= ramp[i];
                dest[m][i] += s32(sample >> 15);
            }
        }
    }

    void RunCompressor(u16 threshold, u16 release_frames, u32 table_addr, u32 millis) {
        bool triggered = false;
        for (u32 i = 0; i < 32 * millis; ++i)
            if (std::abs(main_left[i]) > int(threshold) || std::abs(main_right[i]) > int(threshold)) {
                triggered = true;
                break;
            }
        const u32 frame_bytes = 32 * millis * sizeof(s16);
        u32 off = 0;
        if (triggered) {
            off = compressor_pos * frame_bytes;
            compressor_pos = release_frames;
        } else if (compressor_pos) {
            --compressor_pos;
            off = (11 + compressor_pos) * frame_bytes;
        } else {
            return;
        }
        u16 ramp[32 * 5];
        mem.read16s(ramp, table_addr + off, 32 * millis * sizeof(u16));
        for (u32 i = 0; i < 32 * millis; ++i) {
            main_left[i] = int((s64(main_left[i]) * ramp[i]) >> 15);
            main_right[i] = int((s64(main_right[i]) * ramp[i]) >> 15);
        }
    }

    void OutputSamples(u32 lr_addr, u32 surround_addr, u16 volume, bool upload_auxc) {
        u16 ramp[96];
        GenerateVolumeRamp(ramp, last_main_volume, volume, 96);
        last_main_volume = volume;
        mem.write32s(surround_addr, main_surround, 96);
        if (upload_auxc) mem.write32s(surround_addr + 96 * sizeof(int), auxC_left, 96);
        for (size_t i = 0; i < 96; ++i) {
            s64 left = main_left[i], right = main_right[i];
            left = (left * ramp[i]) >> 15;
            right = (right * ramp[i]) >> 15;
            main_left[i] = ClampS16(left);
            main_right[i] = ClampS16(right);
        }
        u8* out = mem.ptr(lr_addr, 96 * 4);
        if (out) {
            for (size_t i = 0; i < 96; ++i) {  // R, L entrelazados, big endian
                const u16 r = u16(s16(main_right[i])), l = u16(s16(main_left[i]));
                out[4 * i] = u8(r >> 8); out[4 * i + 1] = u8(r); out[4 * i + 2] = u8(l >> 8); out[4 * i + 3] = u8(l);
            }
            mem.written(lr_addr, 96 * 4);
        }
        mem.env.push_mail(DSP_SYNC, true);
    }

    void OutputWMSamples(const u32* a) {
        int* bufs[4] = {wm0, wm1, wm2, wm3};
        for (u32 i = 0; i < 4; ++i) {
            u8* out = mem.ptr(a[i], 18 * sizeof(s16));
            if (!out) continue;
            for (u32 j = 0; j < 18; ++j) {
                const u16 s = u16(ClampS16(bufs[i][j]));
                out[2 * j] = u8(s >> 8); out[2 * j + 1] = u8(s);
            }
            mem.written(a[i], 18 * sizeof(s16));
        }
    }

    // HandleCommandList de AXWii.cpp (versiones nueva y antigua).
    u32 HandleCommandList(u32 size) {
        u32 voices = 0, pb_addr = 0, i = 0;
        auto next = [&]() -> u16 { return i < 512 ? cmdlist[i++] : u16(CMD_END_OLD); };
        auto addr = [&]() -> u32 { const u32 hi = next(); return (hi << 16) | next(); };
        auto addresses = [&](u32* out, int n) { for (int k = 0; k < n; ++k) out[k] = addr(); };
        for (int guard = 0; guard < 4096; ++guard) {
            if (i >= size) return voices;  // sin CMD_END: la lista acaba con su tamaño
            const u16 cmd = next();
            if (old_axwii) {
                switch (cmd) {
                case CMD_SETUP_OLD: SetupProcessing(addr()); break;
                case CMD_ADD_TO_LR_OLD: case CMD_SUB_TO_LR_OLD: AddToLR(addr(), cmd == CMD_SUB_TO_LR_OLD); break;
                case CMD_ADD_SUB_TO_LR_OLD: AddSubToLR(addr()); break;
                case CMD_PB_ADDR_OLD: pb_addr = addr(); break;
                case CMD_PROCESS_OLD: voices += ProcessPBList(pb_addr); break;
                case CMD_MIX_AUXA_OLD: case CMD_MIX_AUXB_OLD: case CMD_MIX_AUXC_OLD: {
                    const u16 vol = next();
                    const u32 w = addr(), r = addr();
                    MixAUXSamples(cmd - CMD_MIX_AUXA_OLD, w, r, vol);
                    break;
                }
                case CMD_UPL_AUXA_MIX_LRSC_OLD: case CMD_UPL_AUXB_MIX_LRSC_OLD: {
                    const u16 vol = next();
                    u32 a[6];
                    addresses(a, 6);
                    UploadAUXMixLRSC(cmd == CMD_UPL_AUXB_MIX_LRSC_OLD, a, vol);
                    break;
                }
                case CMD_COMPRESSOR_OLD: {
                    const u16 th = next(), frames = next();
                    RunCompressor(th, frames, addr(), 3);
                    break;
                }
                case CMD_OUTPUT_OLD: case CMD_OUTPUT_DPL2_OLD: {
                    const u32 s = addr(), lr = addr();
                    OutputSamples(lr, s, 0x8000, cmd == CMD_OUTPUT_DPL2_OLD);
                    break;
                }
                case CMD_WM_OUTPUT_OLD: { u32 a[4]; addresses(a, 4); OutputWMSamples(a); break; }
                case CMD_END_OLD: return voices;
                default: unknown(cmd); return voices;
                }
            } else {
                switch (cmd) {
                case CMD_SETUP: SetupProcessing(addr()); break;
                case CMD_ADD_TO_LR: case CMD_SUB_TO_LR: AddToLR(addr(), cmd == CMD_SUB_TO_LR); break;
                case CMD_ADD_SUB_TO_LR: AddSubToLR(addr()); break;
                case CMD_PROCESS: voices += ProcessPBList(addr()); break;
                case CMD_MIX_AUXA: case CMD_MIX_AUXB: case CMD_MIX_AUXC: {
                    const u16 vol = next();
                    const u32 w = addr(), r = addr();
                    MixAUXSamples(cmd - CMD_MIX_AUXA, w, r, vol);
                    break;
                }
                case CMD_UPL_AUXA_MIX_LRSC: case CMD_UPL_AUXB_MIX_LRSC: {
                    const u16 vol = next();
                    u32 a[6];
                    addresses(a, 6);
                    UploadAUXMixLRSC(cmd == CMD_UPL_AUXB_MIX_LRSC, a, vol);
                    break;
                }
                case CMD_COMPRESSOR: {
                    const u16 th = next(), frames = next();
                    RunCompressor(th, frames, addr(), 3);
                    break;
                }
                case CMD_OUTPUT: case CMD_OUTPUT_DPL2: {
                    const u16 vol = crc == 0xd9c4bf34 ? 0x8000 : next();
                    const u32 s = addr(), lr = addr();
                    OutputSamples(lr, s, vol, cmd == CMD_OUTPUT_DPL2);
                    break;
                }
                case CMD_WM_OUTPUT: { u32 a[4]; addresses(a, 4); OutputWMSamples(a); break; }
                case CMD_END: return voices;
                default: unknown(cmd); return voices;
                }
            }
        }
        return voices;
    }

    void unknown(u16 cmd) {
        static int logged = 0;
        if (logged++ < 8) std::fprintf(stderr, "[ax] comando %04X desconocido: fin de la lista\n", cmd);
    }
};

Wii::Wii(const Env& env, uint32_t hash) : st_(new State{Mem{env}}) {
    State& s = *st_;
    s.crc = hash;
    s.old_axwii = hash == 0xfa450138 || hash == 0x7699af32;
    s.new_filter = hash == 0x347112ba || hash == 0x4cc52064;
    switch (hash) {
    case 0x7699af32: case 0xfa450138: s.layout = Layout::Old; break;
    case 0xd9c4bf34: case 0xadbc06bd: s.layout = Layout::Mid; break;
    case 0x347112ba: case 0x4cc52064: s.layout = Layout::New; break;
    default:
        s.layout = Layout::New;
        std::fprintf(stderr, "[ax] AX de Wii %08X sin catalogar en Dolphin: PB con la disposición más reciente. "
                             "Si el audio sale mal, avisa con este número.\n", hash);
        break;
    }
}

Wii::~Wii() = default;

void Wii::command_list(uint32_t addr, uint16_t size) {
    State& s = *st_;
    if (size >= 512) {
        std::fprintf(stderr, "[ax] lista de comandos demasiado grande (%u palabras)\n", size);
        return;
    }
    std::memset(s.cmdlist, 0, sizeof(s.cmdlist));
    s.mem.read16s(s.cmdlist, addr, size * sizeof(u16));
    voices_ += s.HandleCommandList(size);
    ++lists_;
}

}  // namespace ax
