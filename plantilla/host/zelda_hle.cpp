// SPDX-License-Identifier: GPL-2.0-or-later
// zelda_hle.cpp — ver zelda_hle.h. Port del HLE de Dolphin (Zelda.cpp, "new Zelda HLE" de Pierre Bourdon y el
// Dolphin Emulator Project) a este host: la lógica, los formatos y la aritmética son los de Dolphin. Cambios:
//  * la memoria se lee por Env::span (big endian a mano) y las escrituras avisan por Env::mem_written;
//  * comprobaciones donde Dolphin leería fuera de rango o se quedaría en un bucle (búfer de comandos vacío,
//    reverb con tamaño 0, punteros nulos, bucles PCM sin longitud);
//  * PCM16 desde MRAM rellena el final con la última muestra donde Dolphin pisaba el principio del búfer;
//  * el cifrado de GBA (comando 0C de Pikmin/Animal Crossing en GameCube) no está: se registra y se contesta.
#include "zelda_hle.h"
#include "perfil.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace zelda {
namespace {

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;

enum Flag : u32 {
    NO_ARAM = 0x001,              // Wii: las "lecturas de ARAM" van a memoria principal + base (comando 0E)
    MAKE_DOLBY_LOUDER = 0x002,
    LIGHT_PROTOCOL = 0x004,
    FOUR_MIXING_DESTS = 0x008,
    TINY_VPB = 0x010,
    VOLUME_EXPLICIT_STEP = 0x020,
    SYNC_PER_FRAME = 0x040,
    NO_CMD_0D = 0x080,
    SUPPORTS_GBA_CRYPTO = 0x100,
    WEIRD_CMD_0C = 0x200,
    COMBINED_CMD_0D = 0x400,
};

struct Known { u32 hash, flags; const char* name; };
constexpr Known kUcodes[] = {
    {0x24B22038, LIGHT_PROTOCOL | FOUR_MIXING_DESTS | TINY_VPB | VOLUME_EXPLICIT_STEP | NO_CMD_0D | WEIRD_CMD_0C,
     "IPL de GameCube NTSC"},
    {0x6BA3B3EA, LIGHT_PROTOCOL | FOUR_MIXING_DESTS | NO_CMD_0D, "IPL de GameCube PAL"},
    {0xDF059F68, LIGHT_PROTOCOL | NO_CMD_0D | SUPPORTS_GBA_CRYPTO, "Pikmin GC (demo)"},
    {0x4BE6A5CB, LIGHT_PROTOCOL | NO_CMD_0D | SUPPORTS_GBA_CRYPTO, "Pikmin GC / Animal Crossing"},
    {0x42F64AC4, LIGHT_PROTOCOL | NO_CMD_0D | WEIRD_CMD_0C, "Luigi's Mansion"},
    {0x267FD05A, SYNC_PER_FRAME | NO_CMD_0D, "Pikmin GC PAL"},
    {0x56D36052, SYNC_PER_FRAME | NO_CMD_0D, "Super Mario Sunshine"},
    {0x86840740, 0, "Zelda: The Wind Waker"},
    {0x2FCDF1EC, MAKE_DOLBY_LOUDER, "Zelda FSA / Mario Kart DD / Pikmin 2 GC"},
    {0x6CA33A6D, MAKE_DOLBY_LOUDER | COMBINED_CMD_0D, "Zelda TP GC / DK Jungle Beat GC"},
    {0x6C3F6F94, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D, "Zelda TP Wii / Link's Crossbow Training"},
    {0xD643001F, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D, "Super Mario Galaxy 1/2 / DK Jungle Beat Wii"},
    {0xB7EB9A9C, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D, "Pikmin 1 NPC"},
    {0xEAEB38CC, NO_ARAM | MAKE_DOLBY_LOUDER | COMBINED_CMD_0D, "Pikmin 2 NPC"},
};

const Known* find(u32 hash) {
    for (const Known& k : kUcodes) if (k.hash == hash) return &k;
    return nullptr;
}

constexpr u32 DSP_INIT = 0xDCD10000, DSP_SYNC = 0xDCD10004, DSP_FRAME_END = 0xDCD10005;
constexpr u32 TASK_MAIL_MASK = 0xFFFF0000, TASK_MAIL_TO_DSP = 0xCDD10000;
constexpr u32 MAIL_RESUME = 0xCDD10000, MAIL_NEW_UCODE = 0xCDD10001, MAIL_RESET = 0xCDD10002,
              MAIL_CONTINUE = 0xCDD10003;

bool trace_enabled() {
    static const bool on = std::getenv("CACTUS_TRACE_DSP") != nullptr;
    return on;
}
#define ZTRACE(...) do { if (trace_enabled()) { std::fprintf(stderr, "[zelda] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

template <typename T> T clamp16(T v) { return std::clamp<T>(v, -0x8000, 0x7FFF); }

}  // namespace

bool known(u32 hash) { return find(hash) != nullptr; }
u32 flags_for(u32 hash) { const Known* k = find(hash); return k ? k->flags : 0; }

// ================================================================ mezclador
#define ACCESSOR32(field, Name) \
    u32 Get##Name() const { return (u32(field##_h) << 16) | field##_l; } \
    void Set##Name(u32 v) { field##_h = u16(v >> 16); field##_l = u16(v); }

// Bloque de parámetros de voz: 0xC0 palabras de 16 bits en memoria (0x80 en las versiones TINY_VPB). Las 0x80
// primeras las escribe el DSP; el resto es de solo lectura.
struct VPB {
    u16 enabled, done, resampling_ratio, unk_03, reset_vpb, end_reached, use_constant_sample, samples_to_keep_count;
    struct Channel { u16 id; s16 target_volume, current_volume; u16 unk; } channels[6];
    u16 unk_20_28[8];
    u16 dolby_voice_position;
    u8 GetDolbyVoiceX() const { return (dolby_voice_position >> 8) & 0x7F; }
    u8 GetDolbyVoiceY() const { return dolby_voice_position & 0x7F; }
    s16 dolby_reverb_factor, dolby_volume_current, dolby_volume_target;
    u16 use_dolby_volume, unk_2D, unk_2E, unk_2F;
    u16 current_pos_frac, unk_31, afc_remaining_decoded_samples;
    s16 constant_sample;
    u16 current_position_h, current_position_l;
    ACCESSOR32(current_position, CurrentPosition)
    u16 samples_before_loop, unk_37;
    u16 current_aram_addr_h, current_aram_addr_l;
    ACCESSOR32(current_aram_addr, CurrentARAMAddr)
    u16 remaining_length_h, remaining_length_l;
    ACCESSOR32(remaining_length, RemainingLength)
    s16 resample_buffer[4];
    s16 variable_fir_history[20];
    s16 biquad_xn1, biquad_xn2, biquad_yn1, biquad_yn2;
    s16 afc_remaining_samples[0x10];
    s16* AFCYN2() { return &afc_remaining_samples[0xE]; }
    s16* AFCYN1() { return &afc_remaining_samples[0xF]; }
    s16 low_pass_yn1, low_pass_xn1;
    u16 unk_6A_80[0x80 - 0x6A];

    enum SamplesSourceType {
        SRC_SQUARE_WAVE = 0, SRC_SAW_WAVE = 1, SRC_SQUARE_WAVE_25PCT = 3,
        SRC_CONST_PATTERN_0 = 7, SRC_CONST_PATTERN_0_VARIABLE_STEP = 10, SRC_CONST_PATTERN_1 = 4,
        SRC_CONST_PATTERN_2 = 11, SRC_CONST_PATTERN_3 = 12,
        SRC_AFC_LQ_FROM_ARAM = 5, SRC_PCM8_FROM_ARAM = 8, SRC_AFC_HQ_FROM_ARAM = 9, SRC_PCM16_FROM_ARAM = 16,
        SRC_PCM16_FROM_MRAM = 33,
    };
    u16 samples_source_type, is_looping;
    s16 loop_yn1, loop_yn2;
    u16 filter_flags;  // bits 0-4: tamaño del FIR variable; bit 5: biquad
    u16 variable_fir_filter_size() const { return filter_flags & 0x1F; }
    bool enable_biquad_filter() const { return (filter_flags >> 5) & 1; }
    u16 end_requested, unk_86, unk_87;
    u16 loop_address_h, loop_address_l;
    ACCESSOR32(loop_address, LoopAddress)
    u16 loop_start_position_h, loop_start_position_l;
    ACCESSOR32(loop_start_position, LoopStartPosition)
    u16 base_address_h, base_address_l;
    ACCESSOR32(base_address, BaseAddress)
    u16 unk_8E, unk_8F;
    u16 variable_fir_coeffs[20];
    s16 biquad_bn1, biquad_bn2, biquad_an1, biquad_an2;
    u16 low_pass_coeff;
    u16 padding[0xC0];

    u16* words() { return reinterpret_cast<u16*>(this); }

    void Uncompress() {  // VPB de 0x80 palabras (IPL NTSC) -> completo
        u16* w = words();
        for (int i = 0; i < 0x40; ++i) { w[0x80 + i] = w[0x40 + i]; w[0x40 + i] = 0; }
        for (int i = 0; i < 0x10; ++i) { w[0x58 + i] = w[0x30 + i]; w[0x30 + i] = 0; }
        for (int i = 0; i < 0x18; ++i) { w[0x30 + i] = w[0x18 + i]; w[0x18 + i] = 0; }
    }
    void Compress() {
        u16* w = words();
        for (int i = 0; i < 0x18; ++i) { w[0x18 + i] = w[0x30 + i]; w[0x30 + i] = 0; }
        for (int i = 0; i < 0x10; ++i) { w[0x30 + i] = w[0x58 + i]; w[0x58 + i] = 0; }
        for (int i = 0; i < 0x40; ++i) { w[0x40 + i] = w[0x80 + i]; w[0x80 + i] = 0; }
    }
};
#define WORD_AT(field, word) static_assert(offsetof(VPB, field) == (word) * 2, #field)
WORD_AT(channels, 0x08);
WORD_AT(dolby_voice_position, 0x28);
WORD_AT(use_dolby_volume, 0x2C);
WORD_AT(current_pos_frac, 0x30);
WORD_AT(constant_sample, 0x33);
WORD_AT(remaining_length_h, 0x3A);
WORD_AT(resample_buffer, 0x3C);
WORD_AT(afc_remaining_samples, 0x58);
WORD_AT(low_pass_yn1, 0x68);
WORD_AT(samples_source_type, 0x80);
WORD_AT(filter_flags, 0x84);
WORD_AT(loop_address_h, 0x88);
WORD_AT(base_address_h, 0x8C);
WORD_AT(low_pass_coeff, 0xA8);

struct ReverbPB {
    u16 enabled, circular_buffer_size, circular_buffer_base_h, circular_buffer_base_l;
    ACCESSOR32(circular_buffer_base, CircularBufferBase)
    struct Destination { u16 buffer_id, volume; } dest[2];
    s16 filter_coeffs[8];
};
static_assert(sizeof(ReverbPB) == 0x20, "ReverbPB");

class Renderer {
 public:
    using MixingBuffer = std::array<s16, 0x50>;

    explicit Renderer(const Env& env) : env_(env) {}

    void PrepareFrame();
    void AddVoice(u16 voice_id);
    void FinalizeFrame();

    u32 flags = 0;
    std::array<s16, 0x80> sine_table{};
    std::array<s16, 0x100> const_patterns{};
    std::array<s16, 0x100> resampling_coeffs{};
    std::array<s16, 0x20> afc_coeffs{};
    u32 vpb_base = 0, reverb_pb_base = 0, aram_base = 0;
    u16 output_volume = 0;
    u32 output_l = 0, output_r = 0;

    // Memoria en big endian, de 16 bits en 16 bits (CopyFromEmuSwapped / CopyToEmuSwapped de Dolphin).
    void read16s(void* dst, u32 addr, size_t bytes) const {
        u8* d = static_cast<u8*>(dst);
        const u8* p = env_.span ? env_.span(addr, bytes) : nullptr;
        if (!p) { std::memset(d, 0, bytes); return; }
        for (size_t i = 0; i + 1 < bytes; i += 2) { const u16 v = u16((p[i] << 8) | p[i + 1]); std::memcpy(d + i, &v, 2); }
    }
    void write16s(u32 addr, const void* src, size_t bytes) const {
        const u8* s = static_cast<const u8*>(src);
        u8* p = env_.span ? env_.span(addr, bytes) : nullptr;
        if (!p) return;
        for (size_t i = 0; i + 1 < bytes; i += 2) { u16 v; std::memcpy(&v, s + i, 2); p[i] = u8(v >> 8); p[i + 1] = u8(v); }
        if (env_.mem_written) env_.mem_written(addr, bytes);
    }

 private:
    template <size_t N, size_t B>
    static void ApplyVolumeInPlace(std::array<s16, N>* buf, u16 vol) {
        for (size_t i = 0; i < N; ++i) {
            s32 tmp = s32(u32(s32((*buf)[i])) * u32(vol));
            tmp >>= 16 - B;
            (*buf)[i] = s16(std::clamp(tmp, -0x8000, 0x7FFF));
        }
    }
    template <size_t N>
    static s32 AddBuffersWithVolumeRamp(std::array<s16, N>* dst, const std::array<s16, N>& src, s32 vol, s32 step) {
        if (!vol && !step) return vol;
        for (size_t i = 0; i < N; ++i) {
            (*dst)[i] = s16((*dst)[i] + (((vol >> 16) * src[i]) >> 16));
            vol += step;
        }
        return vol;
    }
    static void AddBuffersWithVolume(s16* dst, const s16* src, size_t count, u16 vol) {
        while (count--) {
            const s32 v = (s32(*src++) * s32(vol)) >> 15;
            *dst = s16(*dst + std::clamp(v, -0x8000, 0x7FFF));
            ++dst;
        }
    }

    MixingBuffer* BufferForID(u16 id);
    void FetchVPB(u16 voice_id, VPB* vpb);
    void StoreVPB(u16 voice_id, VPB* vpb);
    void LoadInputSamples(MixingBuffer* buffer, VPB* vpb);
    static u16 NeededRawSamplesCount(const VPB& vpb) { return u16((vpb.current_pos_frac + 0x50 * vpb.resampling_ratio) >> 12); }
    void Resample(VPB* vpb, const s16* src, MixingBuffer* dst);
    const u8* aram_ptr(u32 offset, size_t size) const {
        if (flags & NO_ARAM) return env_.span ? env_.span(aram_base + offset, size) : nullptr;
        return env_.aram ? env_.aram(offset, size) : nullptr;
    }
    template <typename T> void DownloadPCMSamplesFromARAM(s16* dst, VPB* vpb, u16 count);
    void DownloadAFCSamplesFromARAM(s16* dst, VPB* vpb, u16 count);
    void DecodeAFC(VPB* vpb, s16* dst, size_t block_count);
    void DownloadRawSamplesFromMRAM(s16* dst, VPB* vpb, u16 count);
    static void ApplyLowPassFilter(MixingBuffer* buf, VPB* vpb);
    static void ApplyBiquadFilter(MixingBuffer* buf, VPB* vpb);
    void ApplyReverb(bool post_rendering);

    Env env_;
    bool prepared_ = false;
    MixingBuffer front_left_{}, front_right_{}, back_left_{}, back_right_{};
    MixingBuffer front_left_reverb_{}, front_right_reverb_{}, back_left_reverb_{}, back_right_reverb_{};
    MixingBuffer unk0_reverb_{}, unk1_reverb_{}, unk0_{}, unk1_{}, unk2_{};
    std::array<u16, 4> reverb_frames_{};
    std::array<s16, 8> unk0_last8_{}, unk1_last8_{}, front_left_last8_{}, front_right_last8_{};
};

void Renderer::PrepareFrame() {
    if (prepared_) return;
    front_left_.fill(0);
    front_right_.fill(0);
    ApplyVolumeInPlace<0x50, 1>(&back_left_, 0x6784);
    ApplyVolumeInPlace<0x50, 1>(&back_right_, 0x6784);

    ApplyReverb(false);
    AddBuffersWithVolume(front_left_reverb_.data(), back_left_reverb_.data(), 0x50, 0x7FFF);
    AddBuffersWithVolume(front_right_reverb_.data(), back_left_reverb_.data(), 0x50, 0xB820);
    AddBuffersWithVolume(front_left_reverb_.data(), back_right_reverb_.data() + 0x28, 0x28, 0xB820);
    AddBuffersWithVolume(front_right_reverb_.data(), back_right_reverb_.data() + 0x28, 0x28, 0x7FFF);
    back_left_reverb_.fill(0);
    back_right_reverb_.fill(0);

    // Los patrones 2 y 3 cambian en cada cuadro (los 0 y 1 son fijos).
    s16* p2 = const_patterns.data() + 2 * 0x40;
    s32 yn2 = p2[0x40 - 2], yn1 = p2[0x40 - 1], v;
    for (int i = 0; i < 0x40; i += 2) {
        v = s32(s64(yn2) * yn1 - s64(p2[i]) * 65536);
        yn2 = yn1; yn1 = p2[i];
        p2[i] = s16(v >> 16);
        v = s32(2 * (s64(yn2) * yn1 + s64(p2[i + 1]) * 65536));
        yn2 = yn1; yn1 = p2[i + 1];
        p2[i + 1] = s16(v >> 16);
    }
    s16* p3 = const_patterns.data() + 3 * 0x40;
    yn2 = p3[0x40 - 2];
    yn1 = p3[0x40 - 1];
    const s16 acc = s16(yn1);
    s16 step = s16(p3[0] + s32((s64(yn1) * yn2 + (s64(yn2) * 65536 + yn1)) >> 16));
    step = s16((step & 0x1FF) | 0x2000);
    for (s32 i = 0; i < 0x40; ++i) p3[i] = s16(acc + (i + 1) * step);

    prepared_ = true;
}

void Renderer::ApplyReverb(bool post_rendering) {
    if (!reverb_pb_base) return;
    MixingBuffer* buffers[4] = {&unk0_reverb_, &unk1_reverb_, &front_left_reverb_, &front_right_reverb_};
    std::array<s16, 8>* last8[4] = {&unk0_last8_, &unk1_last8_, &front_left_last8_, &front_right_last8_};

    for (u16 idx = 0; idx < 4; ++idx) {
        ReverbPB rpb{};
        read16s(&rpb, reverb_pb_base + idx * sizeof(ReverbPB), sizeof(ReverbPB));
        if (!rpb.enabled) continue;
        u16 frame = reverb_frames_[idx];
        const u32 mram = rpb.GetCircularBufferBase() + frame * 0x50 * sizeof(s16);

        if (!post_rendering) {
            std::array<s16, 0x58> buffer;
            for (u16 i = 0; i < 8; ++i) buffer[i] = (*last8[idx])[i];
            read16s(&buffer[8], mram, 0x50 * sizeof(s16));
            for (u16 i = 0; i < 8; ++i) (*last8[idx])[i] = buffer[0x50 + i];

            auto filter = [&] {
                for (u16 i = 0; i < 0x50; ++i) {
                    s32 sample = 0;
                    for (u16 j = 0; j < 8; ++j) sample += s32(buffer[i + j]) * rpb.filter_coeffs[j];
                    sample >>= 15;
                    buffer[i] = s16(std::clamp(sample, -0x8000, 0x7FFF));
                }
            };
            if (rpb.enabled & 1) filter();
            for (const auto& dest : rpb.dest) {
                if (!dest.buffer_id) continue;
                if (MixingBuffer* d = BufferForID(dest.buffer_id)) AddBuffersWithVolume(d->data(), buffer.data(), 0x50, dest.volume);
            }
            if (rpb.enabled & 2) filter();
            for (u16 i = 0; i < 0x50; ++i) (*buffers[idx])[i] = buffer[i];
        } else {
            write16s(mram, buffers[idx]->data(), sizeof(MixingBuffer));
            frame = rpb.circular_buffer_size ? u16((frame + 1) % rpb.circular_buffer_size) : 0;
            reverb_frames_[idx] = frame;
        }
    }
}

Renderer::MixingBuffer* Renderer::BufferForID(u16 id) {
    switch (id) {
    case 0x0D00: return &front_left_;
    case 0x0D60: return &front_right_;
    case 0x0F40: return &back_left_;
    case 0x0CA0: return &back_right_;
    case 0x0E80: return &front_left_reverb_;
    case 0x0EE0: return &front_right_reverb_;
    case 0x0C00: return &back_left_reverb_;
    case 0x0C50: return &back_right_reverb_;
    case 0x0DC0: return &unk0_reverb_;
    case 0x0E20: return &unk1_reverb_;
    case 0x09A0: return &unk0_;
    case 0x0FA0: return &unk1_;
    case 0x0B00: return &unk2_;
    default: return nullptr;
    }
}

void Renderer::ApplyLowPassFilter(MixingBuffer* buf, VPB* vpb) {
    s32 yn1 = vpb->reset_vpb ? 0 : vpb->low_pass_yn1;
    s32 xn1 = vpb->reset_vpb ? 0 : vpb->low_pass_xn1;
    const s32 coeff = vpb->low_pass_coeff;
    for (int i = 0; i < 0x50; ++i) {
        const s32 xn0 = (*buf)[i];
        s64 tmp = xn0 - xn1;
        tmp *= coeff;
        tmp >>= 7;
        tmp += yn1;
        const s16 yn0 = s16(std::clamp<s64>(tmp, -0x8000, 0x7FFF));
        (*buf)[i] = yn0;
        yn1 = yn0;
        xn1 = xn0;
    }
    vpb->low_pass_yn1 = s16(yn1);
    vpb->low_pass_xn1 = s16(xn1);
}

void Renderer::ApplyBiquadFilter(MixingBuffer* buf, VPB* vpb) {
    s32 xn1 = vpb->biquad_xn1, xn2 = vpb->biquad_xn2, yn1 = vpb->biquad_yn1, yn2 = vpb->biquad_yn2;
    for (int i = 0; i < 0x50; ++i) {
        const s32 xn0 = (*buf)[i];
        s64 tmp = 0;
        tmp += s64(vpb->biquad_bn1) * xn1;
        tmp += s64(vpb->biquad_bn2) * xn2;
        tmp += s64(vpb->biquad_an1) * yn1;
        tmp += s64(vpb->biquad_an2) * yn2;
        const s16 yn0 = s16(std::clamp<s64>(tmp >> 15, -0x8000, 0x7FFF));
        (*buf)[i] = yn0;
        xn2 = xn1; xn1 = xn0; yn2 = yn1; yn1 = yn0;
    }
    vpb->biquad_xn1 = s16(xn1); vpb->biquad_xn2 = s16(xn2);
    vpb->biquad_yn1 = s16(yn1); vpb->biquad_yn2 = s16(yn2);
}

void Renderer::AddVoice(u16 voice_id) {
    VPB vpb;
    FetchVPB(voice_id, &vpb);
    if (!vpb.enabled || vpb.done) return;

    MixingBuffer input;
    LoadInputSamples(&input, &vpb);

    if (vpb.low_pass_coeff != 0) ApplyLowPassFilter(&input, &vpb);
    if (vpb.enable_biquad_filter() &&
        (vpb.biquad_an2 != 0 || vpb.biquad_an1 != 0 || vpb.biquad_bn2 != 0 || vpb.biquad_bn1 != 0x7FFF))
        ApplyBiquadFilter(&input, &vpb);

    if (vpb.use_dolby_volume) {
        if (vpb.end_requested) {
            vpb.dolby_volume_target = s16(vpb.dolby_volume_current / 2);
            if (vpb.dolby_volume_target == 0) vpb.done = true;
        }
        const s16 right = sine_table[vpb.GetDolbyVoiceX()];
        const s16 back = sine_table[vpb.GetDolbyVoiceY()];
        const s16 left = sine_table[vpb.GetDolbyVoiceX() ^ 0x7F];
        const s16 front = sine_table[vpb.GetDolbyVoiceY() ^ 0x7F];
        const u16 shift = (flags & MAKE_DOLBY_LOUDER) ? 15 : 16;
        s16 quad[4] = {s16((left * front) >> shift), s16((left * back) >> shift), s16((right * front) >> shift),
                       s16((right * back) >> shift)};
        const s16 delta = s16(vpb.dolby_volume_target - vpb.dolby_volume_current);
        s16 deltas[4];
        for (size_t i = 0; i < 4; ++i) deltas[i] = s16((u16(quad[i]) * delta) >> shift);
        for (s16& q : quad) q = s16((q * vpb.dolby_volume_current) >> shift);
        s16 rev[4], rev_deltas[4];
        for (size_t i = 0; i < 4; ++i) {
            rev[i] = s16((quad[i] * vpb.dolby_reverb_factor) >> shift);
            rev_deltas[i] = s16((deltas[i] * vpb.dolby_reverb_factor) >> shift);
        }
        struct { MixingBuffer* buffer; s16 volume, delta; } outs[8] = {
            {&front_left_, quad[0], deltas[0]}, {&back_left_, quad[1], deltas[1]},
            {&front_right_, quad[2], deltas[2]}, {&back_right_, quad[3], deltas[3]},
            {&front_left_reverb_, rev[0], rev_deltas[0]}, {&back_left_reverb_, rev[1], rev_deltas[1]},
            {&front_right_reverb_, rev[2], rev_deltas[2]}, {&back_right_reverb_, rev[3], rev_deltas[3]},
        };
        for (const auto& o : outs)
            AddBuffersWithVolumeRamp(o.buffer, input, s32(o.volume) * 65536, (s32(o.delta) * 65536) / s32(o.buffer->size()));
        vpb.dolby_volume_current = vpb.dolby_volume_target;
    } else {
        const int n = (flags & FOUR_MIXING_DESTS) ? 4 : 6;
        if (vpb.end_requested) {
            bool all_mute = true;
            for (int i = 0; i < n; ++i) {
                vpb.channels[i].target_volume = s16(vpb.channels[i].current_volume / 2);
                all_mute &= (vpb.channels[i].target_volume == 0);
            }
            if (all_mute) vpb.done = true;
        }
        for (int i = 0; i < n; ++i) {
            if (!vpb.channels[i].id) continue;
            const s16 delta = (flags & VOLUME_EXPLICIT_STEP)
                                  ? vpb.channels[i].target_volume
                                  : s16(vpb.channels[i].target_volume - vpb.channels[i].current_volume);
            const s32 step = (s32(delta) * 65536) / s32(input.size());
            if (!vpb.channels[i].current_volume && !step) continue;
            MixingBuffer* dst = BufferForID(vpb.channels[i].id);
            if (!dst) continue;
            const s32 nv = AddBuffersWithVolumeRamp(dst, input, s32(vpb.channels[i].current_volume) * 65536, step);
            vpb.channels[i].current_volume = s16(nv >> 16);
        }
    }
    if (!vpb.use_constant_sample) vpb.reset_vpb = false;
    StoreVPB(voice_id, &vpb);
}

void Renderer::FinalizeFrame() {
    ApplyVolumeInPlace<0x50, 4>(&front_left_, output_volume);
    ApplyVolumeInPlace<0x50, 4>(&front_right_, output_volume);
    write16s(output_l, front_left_.data(), sizeof(front_left_));
    write16s(output_r, front_right_.data(), sizeof(front_right_));
    output_l += sizeof(front_left_);
    output_r += sizeof(front_right_);
    ApplyReverb(true);
    prepared_ = false;
}

void Renderer::FetchVPB(u16 voice_id, VPB* vpb) {
    const u32 size = ((flags & TINY_VPB) ? 0x80 : 0xC0) * sizeof(u16);
    std::memset(vpb, 0, sizeof(*vpb));
    read16s(vpb, vpb_base + voice_id * size, size);
    if (flags & TINY_VPB) vpb->Uncompress();
}

void Renderer::StoreVPB(u16 voice_id, VPB* vpb) {
    const u32 size = ((flags & TINY_VPB) ? 0x80 : 0xC0) * sizeof(u16);
    if (flags & TINY_VPB) vpb->Compress();
    write16s(vpb_base + voice_id * size, vpb, size - 0x80);  // solo la parte que escribe el DSP
}

void Renderer::LoadInputSamples(MixingBuffer* buffer, VPB* vpb) {
    std::array<s16, 4 + 0x500 + 0x10> raw{};
    for (size_t i = 0; i < 4; ++i) raw[i] = vpb->resample_buffer[i];

    if (vpb->use_constant_sample) { buffer->fill(vpb->constant_sample); return; }

    switch (vpb->samples_source_type) {
    case VPB::SRC_SQUARE_WAVE:
    case VPB::SRC_SQUARE_WAVE_25PCT: {
        const u32 shift = vpb->samples_source_type == VPB::SRC_SQUARE_WAVE ? 1 : 2;
        const u32 mask = (1u << shift) - 1;
        const u32 ratio = u32(vpb->resampling_ratio) << (shift - 1);
        u32 pos = u32(vpb->current_pos_frac) << shift;
        for (s16& sample : *buffer) { sample = ((pos >> 16) & mask) ? s16(0xC000) : s16(0x4000); pos += ratio; }
        vpb->current_pos_frac = u16(pos >> shift);
        break;
    }
    case VPB::SRC_SAW_WAVE: {
        u32 pos = vpb->current_pos_frac;
        for (s16& sample : *buffer) { sample = s16(pos & 0xFFFF); pos += vpb->resampling_ratio >> 1; }
        vpb->current_pos_frac = u16(pos);
        break;
    }
    case VPB::SRC_CONST_PATTERN_0: case VPB::SRC_CONST_PATTERN_0_VARIABLE_STEP: case VPB::SRC_CONST_PATTERN_1:
    case VPB::SRC_CONST_PATTERN_2: case VPB::SRC_CONST_PATTERN_3: {
        constexpr u16 SIZE = 0x40;
        u16 idx = 0;
        const bool variable = vpb->samples_source_type == VPB::SRC_CONST_PATTERN_0_VARIABLE_STEP;
        switch (vpb->samples_source_type) {
        case VPB::SRC_CONST_PATTERN_1: idx = 1; break;
        case VPB::SRC_CONST_PATTERN_2: idx = 2; break;
        case VPB::SRC_CONST_PATTERN_3: idx = 3; break;
        default: break;
        }
        const s16* pattern = const_patterns.data() + idx * SIZE;
        u32 pos = u32(vpb->current_pos_frac) << 6;
        const u32 step = u32(vpb->resampling_ratio) << 5;
        for (size_t i = 0; i < buffer->size(); ++i) {
            (*buffer)[i] = pattern[(pos >> 16) % SIZE];
            pos = (pos + step) % (u32(SIZE) << 16);
            if (variable) pos = (u32(pos << 10) + u32(s32(back_right_[i]) * s32(vpb->resampling_ratio))) >> 10;
        }
        vpb->current_pos_frac = u16(pos >> 6);
        break;
    }
    case VPB::SRC_PCM8_FROM_ARAM:
        DownloadPCMSamplesFromARAM<s8>(raw.data() + 4, vpb, NeededRawSamplesCount(*vpb));
        Resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_AFC_HQ_FROM_ARAM:
    case VPB::SRC_AFC_LQ_FROM_ARAM:
        DownloadAFCSamplesFromARAM(raw.data() + 4, vpb, NeededRawSamplesCount(*vpb));
        Resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_PCM16_FROM_ARAM:
        DownloadPCMSamplesFromARAM<s16>(raw.data() + 4, vpb, NeededRawSamplesCount(*vpb));
        Resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_PCM16_FROM_MRAM:
        DownloadRawSamplesFromMRAM(raw.data() + 4, vpb, NeededRawSamplesCount(*vpb));
        Resample(vpb, raw.data(), buffer);
        break;
    default: {
        static bool warned[0x40];
        const u16 t = vpb->samples_source_type;
        if (t < 0x40 && !warned[t]) {
            warned[t] = true;
            std::fprintf(stderr, "[zelda] fuente de muestras %u sin implementar: voz en silencio\n", t);
        }
        buffer->fill(0);
        return;
    }
    }
}

void Renderer::Resample(VPB* vpb, const s16* src, MixingBuffer* dst) {
    const u32 ratio = vpb->resampling_ratio;
    u32 pos = vpb->current_pos_frac;
    if ((ratio >> 12) >= 4) {
        for (s16& d : *dst) { pos += ratio; d = src[pos >> 12]; }
    } else {
        for (s16& d : *dst) {
            const u32 ci = ((pos & 0xFFF) >> 6) * 4;  // 0x40 grupos de 4 coeficientes por la parte fraccionaria
            const s16* coeffs = &resampling_coeffs[ci];
            const s16* in = &src[pos >> 12];
            s64 acc = 0;
            for (size_t i = 0; i < 4; ++i) acc += s64(2) * coeffs[i] * in[i];
            acc >>= 16;
            d = s16(std::clamp<s64>(acc, -0x8000, 0x7FFF));
            pos += ratio;
        }
    }
    for (u32 i = 0; i < 4; ++i) vpb->resample_buffer[i] = src[(pos >> 12) + i];
    vpb->constant_sample = (*dst)[dst->size() - 1];
    vpb->current_pos_frac = u16(pos & 0xFFF);
}

template <typename T>
void Renderer::DownloadPCMSamplesFromARAM(s16* dst, VPB* vpb, u16 count) {
    if (vpb->done) { std::fill(dst, dst + count, s16(0)); return; }
    if (vpb->reset_vpb) {
        vpb->SetRemainingLength(vpb->GetLoopStartPosition() - vpb->GetCurrentPosition());
        vpb->SetCurrentARAMAddr(vpb->GetBaseAddress() + vpb->GetCurrentPosition() * sizeof(T));
    }
    vpb->end_reached = false;
    int empty_loops = 0;
    while (count) {
        if (vpb->end_reached) {
            vpb->end_reached = false;
            if (!vpb->is_looping || ++empty_loops > 4) {  // un bucle de longitud 0 no avanza: se termina la voz
                std::fill(dst, dst + count, s16(0));
                vpb->done = true;
                break;
            }
            vpb->SetCurrentPosition(vpb->GetLoopAddress());
            vpb->SetRemainingLength(vpb->GetLoopStartPosition() - vpb->GetCurrentPosition());
            vpb->SetCurrentARAMAddr(vpb->GetBaseAddress() + vpb->GetCurrentPosition() * sizeof(T));
        }
        const u16 n = u16(std::min(vpb->GetRemainingLength(), u32(count)));
        if (n) empty_loops = 0;
        const u8* p = aram_ptr(vpb->GetCurrentARAMAddr(), n * sizeof(T));
        for (u16 i = 0; i < n; ++i) {
            if (!p) { *dst++ = 0; continue; }
            if constexpr (sizeof(T) == 1) *dst++ = s16(s16(s8(p[i])) * 256);
            else *dst++ = s16((p[2 * i] << 8) | p[2 * i + 1]);
        }
        vpb->SetRemainingLength(vpb->GetRemainingLength() - n);
        vpb->SetCurrentARAMAddr(vpb->GetCurrentARAMAddr() + n * sizeof(T));
        count = u16(count - n);
        if (!vpb->GetRemainingLength()) vpb->end_reached = true;
    }
}

void Renderer::DownloadAFCSamplesFromARAM(s16* dst, VPB* vpb, u16 count) {
    if (vpb->reset_vpb) {
        *vpb->AFCYN1() = 0;
        *vpb->AFCYN2() = 0;
        vpb->afc_remaining_decoded_samples = 0;
        vpb->SetRemainingLength(vpb->GetLoopStartPosition());
        vpb->SetCurrentARAMAddr(vpb->GetBaseAddress());
    }
    if (vpb->done) { std::fill(dst, dst + count, s16(0)); return; }
    if (vpb->afc_remaining_decoded_samples > 0x10) vpb->afc_remaining_decoded_samples = 0x10;

    for (int guard = 0; guard < 64; ++guard) {
        const u16 out = std::min(vpb->afc_remaining_decoded_samples, count);
        const s16* base = &vpb->afc_remaining_samples[0x10 - vpb->afc_remaining_decoded_samples];
        for (size_t i = 0; i < out; ++i) *dst++ = base[i];
        vpb->afc_remaining_decoded_samples = u16(vpb->afc_remaining_decoded_samples - out);
        count = u16(count - out);

        if (count == 0) return;
        if (count <= vpb->GetRemainingLength()) {
            const u16 blocks = u16((count + 0xF) >> 4);
            const u16 decoded = u16(blocks << 4);
            if (decoded < vpb->GetRemainingLength()) {
                vpb->afc_remaining_decoded_samples = u16(decoded - count);
                vpb->SetRemainingLength(vpb->GetRemainingLength() - decoded);
            } else {
                vpb->afc_remaining_decoded_samples = u16(vpb->GetRemainingLength() - count);
                vpb->SetRemainingLength(0);
            }
            DecodeAFC(vpb, dst, blocks);
            if (vpb->afc_remaining_decoded_samples) {
                for (size_t i = 0; i < 0x10; ++i) vpb->afc_remaining_samples[i] = dst[decoded - 0x10 + i];
                if (!vpb->GetRemainingLength() && vpb->GetLoopStartPosition()) {
                    // Reordena las muestras guardadas para la próxima vuelta del bucle. Como en Dolphin, el
                    // origen puede caer antes del arreglo (campos anteriores de la VPB).
                    u16* w = vpb->words();
                    int src = 0x58 + int((vpb->GetLoopStartPosition() + 0xF) & 0xF);
                    for (size_t i = 0; i < vpb->afc_remaining_decoded_samples; ++i) w[0x58 + 0x10 - i - 1] = w[src--];
                }
            }
            return;
        }
        if (vpb->GetRemainingLength()) {
            count = u16(count - vpb->GetRemainingLength());
            DecodeAFC(vpb, dst, (vpb->GetRemainingLength() + 0xF) >> 4);
            dst += vpb->GetRemainingLength();
        }
        if (!vpb->is_looping) {
            vpb->done = true;
            std::fill(dst, dst + count, s16(0));
            return;
        }
        // Vuelta al principio del bucle: la fuente (5 o 9) es también los bytes por bloque de 16 muestras.
        const u32 loop_off = (vpb->GetLoopAddress() >> 4) * vpb->samples_source_type;
        vpb->SetCurrentARAMAddr(vpb->GetBaseAddress() + loop_off);
        *vpb->AFCYN2() = vpb->loop_yn2;
        *vpb->AFCYN1() = vpb->loop_yn1;
        DecodeAFC(vpb, vpb->afc_remaining_samples, 1);
        vpb->afc_remaining_decoded_samples = u16(0x10 - (vpb->GetLoopAddress() & 0xF));
        u32 remaining = vpb->GetLoopStartPosition();
        remaining -= vpb->afc_remaining_decoded_samples;
        remaining -= vpb->GetLoopAddress();
        vpb->SetRemainingLength(remaining);
    }
    std::fill(dst, dst + count, s16(0));  // bucle degenerado
    vpb->done = true;
}

void Renderer::DecodeAFC(VPB* vpb, s16* dst, size_t block_count) {
    const u32 addr = vpb->GetCurrentARAMAddr();
    const u8* src = aram_ptr(addr, block_count * vpb->samples_source_type);
    vpb->SetCurrentARAMAddr(addr + u32(block_count) * vpb->samples_source_type);
    if (!src) { std::fill(dst, dst + block_count * 16, s16(0)); return; }

    for (size_t b = 0; b < block_count; ++b) {
        s16 nibbles[16];
        const s16 delta = s16(1 << ((*src >> 4) & 0xF));
        const s16 idx = s16(*src & 0xF);
        src++;
        if (vpb->samples_source_type == VPB::SRC_AFC_HQ_FROM_ARAM) {
            for (size_t i = 0; i < 16; i += 2) { nibbles[i] = s16(*src >> 4); nibbles[i + 1] = s16(*src & 0xF); src++; }
            for (s16& n : nibbles) n = s16(s16(n << 12) >> 1);
        } else {
            for (size_t i = 0; i < 16; i += 4) {
                nibbles[i + 0] = s16((*src >> 6) & 3);
                nibbles[i + 1] = s16((*src >> 4) & 3);
                nibbles[i + 2] = s16((*src >> 2) & 3);
                nibbles[i + 3] = s16((*src >> 0) & 3);
                src++;
            }
            for (s16& n : nibbles) n = s16(s16(n << 14) >> 1);
        }
        s32 yn1 = *vpb->AFCYN1(), yn2 = *vpb->AFCYN2();
        for (s16 n : nibbles) {
            s32 sample = s32(s64(delta) * n + s64(yn1) * afc_coeffs[idx * 2] + s64(yn2) * afc_coeffs[idx * 2 + 1]);
            sample >>= 11;
            sample = std::clamp(sample, -0x8000, 0x7FFF);
            *dst++ = s16(sample);
            yn2 = yn1;
            yn1 = sample;
        }
        *vpb->AFCYN2() = s16(yn2);
        *vpb->AFCYN1() = s16(yn1);
    }
}

void Renderer::DownloadRawSamplesFromMRAM(s16* dst, VPB* vpb, u16 count) {
    const u32 addr = vpb->GetBaseAddress() + vpb->current_position_h * sizeof(u16);
    const u32 remaining = vpb->GetRemainingLength();
    if (count > remaining) {
        s16 last = 0;
        if (remaining) { read16s(dst, addr, remaining * sizeof(s16)); last = dst[remaining - 1]; }
        for (u32 i = remaining; i < count; ++i) dst[i] = last;
        vpb->current_position_h = u16(vpb->current_position_h + remaining);
        vpb->SetRemainingLength(0);
        vpb->done = true;
    } else {
        vpb->SetRemainingLength(remaining - count);
        vpb->samples_before_loop = u16(vpb->loop_start_position_h - vpb->current_position_h);
        if (count <= vpb->samples_before_loop) {
            read16s(dst, addr, count * sizeof(s16));
            vpb->current_position_h = u16(vpb->current_position_h + count);
        } else {
            read16s(dst, addr, vpb->samples_before_loop * sizeof(s16));
            vpb->SetBaseAddress(vpb->GetLoopAddress());
            vpb->current_position_h = u16(count - vpb->samples_before_loop);
            read16s(dst + vpb->samples_before_loop, vpb->GetLoopAddress(), vpb->current_position_h * sizeof(s16));
        }
    }
}

// ================================================================ protocolo
Ucode::Ucode(const Env& env, u32 hash) : env_(env), hash_(hash), flags_(flags_for(hash)), r_(new Renderer(env)) {
    r_->flags = flags_;
    const Known* k = find(hash);
    std::fprintf(stderr, "[zelda] microcódigo %08X (%s), banderas %03X\n", hash, k ? k->name : "desconocido", flags_);
}

Ucode::~Ucode() = default;

void Ucode::initialize() {
    if (flags_ & LIGHT_PROTOCOL) {
        env_.push_mail(0x88881111, false);
    } else {
        env_.push_mail(DSP_INIT, true);
        env_.push_mail(0xF3551111, false);  // saludo
    }
}

Action Ucode::handle_mail(u32 mail) {
    return (flags_ & LIGHT_PROTOCOL) ? mail_light(mail) : mail_default(mail);
}

Action Ucode::mail_default(u32 mail) {
    switch (state_) {
    case State::Waiting:
        if (mail & 0x80000000u) {
            if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP) mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
            switch (mail) {
            case MAIL_NEW_UCODE:
                can_execute_ = true;
                run_pending_commands();
                ZTRACE("cambio de microcódigo");
                state_ = State::Waiting;
                return Action::UploadNewUcode;
            case MAIL_RESET:
                state_ = State::Halted;
                return Action::ResetToRom;
            case MAIL_CONTINUE:
                can_execute_ = true;
                run_pending_commands();
                break;
            case MAIL_RESUME:
            default:
                ZTRACE("orden de fin de tarea %08X: se detiene", mail);
                state_ = State::Halted;
                break;
            }
        } else if (!(mail & 0xFFFF)) {
            if (rendering()) {
                state_ = State::Rendering;
            } else {
                ZTRACE("correo de sincronía %08X sin render en curso: se detiene", mail);
                state_ = State::Halted;
            }
        } else {
            state_ = State::WritingCmd;
            expected_cmd_mails_ = mail & 0xFFFF;
        }
        break;

    case State::Rendering:
        if (flags_ & SYNC_PER_FRAME) {
            const int base = sync_second_half_ ? 2 : 0;
            sync_skip_[base] = u16(mail >> 16);
            sync_skip_[base + 1] = u16(mail);
            if (sync_second_half_) sync_max_voice_ = 0xFFFF;
            render_audio();
            if (sync_second_half_) state_ = State::Waiting;
            sync_second_half_ = !sync_second_half_;
        } else {
            sync_max_voice_ = (((mail >> 16) & 0xF) + 1) << 4;
            sync_skip_[(mail >> 16) & 0xFF] = u16(mail);
            render_audio();
            state_ = State::Waiting;
        }
        break;

    case State::WritingCmd:
        write32(mail);
        if (--expected_cmd_mails_ == 0) {
            pending_ += 1;
            state_ = State::Waiting;
            run_pending_commands();
        }
        break;

    case State::Halted:
        ZTRACE("correo %08X con el microcódigo detenido", mail);
        break;
    }
    return Action::None;
}

Action Ucode::mail_light(u32 mail) {
    bool add_command = true;
    switch (state_) {
    case State::Waiting:
        write32(mail);
        switch ((mail >> 24) & 0x7F) {
        case 0x00: expected_cmd_mails_ = 0; break;
        case 0x01: expected_cmd_mails_ = 4; break;
        case 0x02: expected_cmd_mails_ = 2; break;
        case 0x03: add_command = false; break;
        case 0x0C:
            expected_cmd_mails_ = (flags_ & SUPPORTS_GBA_CRYPTO) ? 1 : (flags_ & WEIRD_CMD_0C) ? 2 : 0;
            break;
        default: ZTRACE("orden desconocida (protocolo light) %08X", mail); break;
        }
        if (expected_cmd_mails_) {
            state_ = State::WritingCmd;
        } else if (add_command) {
            pending_ += 1;
            run_pending_commands();
        }
        break;
    case State::WritingCmd:
        write32(mail);
        if (--expected_cmd_mails_ == 0) {
            pending_ += 1;
            state_ = State::Waiting;
            run_pending_commands();
        }
        break;
    case State::Rendering:
        sync_max_voice_ = 0xFFFFFFFF;
        sync_skip_.fill(0xFFFF);
        render_audio();
        if (env_.raise_dspint) env_.raise_dspint();
        break;
    case State::Halted:
        break;
    }
    return Action::None;
}

u32 Ucode::read32() {
    if (rd_ == wr_) { ZTRACE("faltan parámetros de orden"); return 0; }
    const u32 v = cmd_[rd_];
    rd_ = (rd_ + 1) % cmd_.size();
    return v;
}

void Ucode::write32(u32 v) {
    cmd_[wr_] = v;
    wr_ = (wr_ + 1) % cmd_.size();
}

void Ucode::run_pending_commands() {
    if (rendering() || !can_execute_) return;
    while (pending_) {
        if (rd_ == wr_) { pending_ = 0; break; }  // Dolphin se quedaría en bucle
        const u32 cmd_mail = read32();
        if (!(cmd_mail & 0x80000000u)) continue;
        const u32 command = (cmd_mail >> 24) & 0x7F;
        const u16 sync = u16(cmd_mail >> 16);
        const u32 extra = cmd_mail & 0xFFFF;
        pending_--;
        ZTRACE("orden %02X (%08X)", command, cmd_mail);

        switch (command) {
        case 0x00: case 0x0A: case 0x0B: case 0x0F:
            send_ack(Ack::Standard, sync);
            break;
        case 0x03:
            if (flags_ & LIGHT_PROTOCOL) break;
            send_ack(Ack::Standard, sync);
            break;
        case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09:
            std::fprintf(stderr, "[zelda] orden %02X que cuelga al microcódigo real: se detiene\n", command);
            state_ = State::Halted;
            return;

        case 0x01: {  // preparación: VPB, coeficientes de resampleo, patrones, tabla de senos, AFC, reverb
            voices_per_frame_ = u16(extra);
            r_->vpb_base = read32();
            const u32 tables = read32();
            r_->read16s(r_->resampling_coeffs.data(), tables, 0x200);
            r_->read16s(r_->const_patterns.data(), tables + 0x200, 0x200);
            if (!(flags_ & LIGHT_PROTOCOL)) r_->read16s(r_->sine_table.data(), tables + 0x400, 0x100);
            r_->read16s(r_->afc_coeffs.data(), read32(), 0x40);
            r_->reverb_pb_base = read32();
            ZTRACE("preparación: %u voces, VPB en %08X, reverb en %08X", voices_per_frame_, r_->vpb_base, r_->reverb_pb_base);
            send_ack(Ack::Standard, sync);
            break;
        }

        case 0x02:  // render de N cuadros: se queda con el control de los correos hasta terminar
            req_frames_ = (cmd_mail >> 16) & 0xFF;
            r_->output_volume = u16(cmd_mail);
            r_->output_l = read32();
            r_->output_r = read32();
            if (flags_ & COMBINED_CMD_0D) { read32(); read32(); }
            cur_frame_ = 0;
            cur_voice_ = 0;
            if (flags_ & LIGHT_PROTOCOL) {
                send_ack(Ack::Standard, u16(req_frames_));
                state_ = State::Rendering;
            } else {
                render_audio();
            }
            return;

        case 0x0C:
            if (flags_ & SUPPORTS_GBA_CRYPTO) {
                std::fprintf(stderr, "[zelda] cifrado de GBA (orden 0C) sin implementar\n");
                read32();
            } else if (flags_ & WEIRD_CMD_0C) {
                read32(); read32();
            }
            send_ack(Ack::Standard, sync);
            break;

        case 0x0D:
            if (!(flags_ & NO_CMD_0D)) read32();
            send_ack(Ack::Standard, sync);
            break;

        case 0x0E:  // Wii: base de la "ARAM" en memoria principal
            r_->aram_base = read32();
            ZTRACE("base de ARAM %08X", r_->aram_base);
            send_ack(Ack::Standard, sync);
            break;

        default:
            std::fprintf(stderr, "[zelda] orden inexistente %02X: se detiene\n", command);
            state_ = State::Halted;
            return;
        }
    }
}

void Ucode::send_ack(Ack type, u16 sync) {
    if (flags_ & LIGHT_PROTOCOL) {
        // El protocolo light contesta con la dirección del manejador de la orden en el código del DSP.
        sync = u16(2 * ((sync >> 8) & 0x7F) + 0x62);
        env_.push_mail(0x80000000u | sync, false);
        return;
    }
    env_.push_mail(type == Ack::Standard ? DSP_SYNC : DSP_FRAME_END, true);
    if (type == Ack::Standard) env_.push_mail(0xF3550000u | sync, false);
}

void Ucode::render_audio() {
    if (!rendering()) return;
    CACTUS_ZONA("DSP: Zelda, voces");
    while (cur_frame_ < req_frames_) {
        if (cur_voice_ == 0) r_->PrepareFrame();
        const u16 voices = std::min<u16>(voices_per_frame_, u16(sync_skip_.size() << 4));
        while (cur_voice_ < voices) {
            if (cur_voice_ >= sync_max_voice_) return;  // espera el siguiente correo de sincronía
            const u16 f = sync_skip_[cur_voice_ >> 4];
            if (f & (1u << (0xF - (cur_voice_ & 0xF)))) r_->AddVoice(u16(cur_voice_));
            cur_voice_++;
        }
        if (!(flags_ & LIGHT_PROTOCOL)) send_ack(Ack::Standard, u16(0xFF00 | cur_frame_));
        r_->FinalizeFrame();
        ++frames_rendered_;
        cur_voice_ = 0;
        sync_max_voice_ = 0;
        cur_frame_++;
    }
    if (!(flags_ & LIGHT_PROTOCOL)) {
        send_ack(Ack::DoneRendering, 0);
        can_execute_ = false;  // hasta que la CPU conteste (0xCDD10003)
    } else {
        state_ = State::Waiting;
    }
}

}  // namespace zelda
