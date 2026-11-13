#include "ai_dma.h"

namespace ai {

void Dma::write_control(uint16_t v) {
    const bool was_on = control & 0x8000;
    control = v;
    if (!was_on && (v & 0x8000)) {
        cursor = source;
        left = v & 0x7fff;
    }
}

unsigned Dma::tick(int64_t cycles, uint32_t rate, uint8_t* (*span)(uint32_t, size_t), std::vector<uint8_t>& out) {
    unsigned irqs = 0;
    acc += double(cycles) * (double(rate) * 4.0) / 729000000.0;
    while (acc >= 32.0) {
        acc -= 32.0;
        const size_t at = out.size();
        out.resize(at + 32, 0);
        if (!(control & 0x8000)) continue;  // parado: silencio
        if (const uint8_t* src = span ? span(cursor, 32) : nullptr) {
            uint8_t* dst = out.data() + at;
            for (int i = 0; i < 8; ++i) {  // R, L -> L, R
                dst[4 * i] = src[4 * i + 2];
                dst[4 * i + 1] = src[4 * i + 3];
                dst[4 * i + 2] = src[4 * i];
                dst[4 * i + 3] = src[4 * i + 1];
            }
        }
        if (left) {
            --left;
            cursor += 32;
        }
        if (!left) {
            cursor = source;
            left = control & 0x7fff;
            ++irqs;
        }
    }
    return irqs;
}

}  // namespace ai
