// wiimote.cpp — ver wiimote.h. Formatos de informe, memoria y extensión según WiiBrew (Wiimote y
// Wiimote/Extension_Controllers). Sin Motion Plus ni altavoz: el altavoz traga sus datos y 0xA6xxxx da error 7.
#include "wiimote.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace wpad {
namespace {

// Acelerómetro de 10 bits. Cero en 512 y 1 g en 616 (en la EEPROM: 0x80 y 0x9A en los 8 bits altos).
constexpr uint16_t kRestX = 512, kRestY = 512, kRestZ = 616;
constexpr uint16_t kNunRestZ = 716;  // Nunchuk: 1 g en 0xB3 en los 8 bits altos
constexpr int kShake = 300;          // oscilación al agitar, por cuadro

uint8_t sum_plus(const uint8_t* p, size_t n, uint8_t extra) {
    unsigned s = extra;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return uint8_t(s);
}

// Calibración del acelerómetro del Wiimote (EEPROM 0x16 y su copia en 0x20): cero, 1 g y suma de control.
void wiimote_calibration(uint8_t* p) {
    p[0] = p[1] = p[2] = 0x80; p[3] = 0;
    p[4] = p[5] = p[6] = 0x9A; p[7] = 0;
    p[8] = sum_plus(p, 8, 0x55);
}

// Calibración del Nunchuk (registro 0x20 y su copia en 0x30): acelerómetro y rango del stick, con dos sumas.
void nunchuk_calibration(uint8_t* c) {
    c[0] = c[1] = c[2] = 0x80; c[3] = 0;
    c[4] = c[5] = c[6] = 0xB3; c[7] = 0;
    c[8] = 0xE0; c[9] = 0x20; c[10] = 0x80;
    c[11] = 0xE0; c[12] = 0x20; c[13] = 0x80;
    c[14] = sum_plus(c, 14, 0x55);
    c[15] = sum_plus(c, 14, 0xAA);
}

uint8_t stick8(float v) {
    const float s = 0x80 + std::max(-1.0f, std::min(1.0f, v)) * 0x60;
    return uint8_t(s);
}

struct Accel { uint16_t x, y, z; };

Accel shaken(Accel rest, bool shake, unsigned frame) {
    if (!shake) return rest;
    const int d = (frame & 1) ? kShake : -kShake;
    return {uint16_t(rest.x + d), uint16_t(rest.y), uint16_t(rest.z - d)};
}

void nunchuk_bytes(const Input& in, unsigned frame, uint8_t* out) {
    const Accel a = shaken({kRestX, kRestY, kNunRestZ}, in.nunchuk_shake, frame);
    out[0] = stick8(in.stick_x);
    out[1] = stick8(in.stick_y);
    out[2] = uint8_t(a.x >> 2); out[3] = uint8_t(a.y >> 2); out[4] = uint8_t(a.z >> 2);
    out[5] = uint8_t(((a.z & 3) << 6) | ((a.y & 3) << 4) | ((a.x & 3) << 2) | (in.c ? 0 : 2) | (in.z ? 0 : 1));
}

// Dos puntos del sensor bar vistos por la cámara de 1024x768 (invisible: 1023). Apuntar a la derecha mueve los
// puntos a la izquierda, y apuntar hacia abajo los sube.
void ir_dots(const Input& in, bool enabled, uint16_t* d) {
    for (int i = 0; i < 8; ++i) d[i] = 1023;
    if (!enabled || !in.pointer) return;
    const int mx = int(512 - in.pointer_x * 350), my = int(384 - in.pointer_y * 200), half = 50;
    if (mx - half < 0 || mx + half > 1022 || my < 0 || my > 766) return;
    d[0] = uint16_t(mx - half); d[1] = uint16_t(my);
    d[2] = uint16_t(mx + half); d[3] = uint16_t(my);
}

void ir_basic(const uint16_t* d, uint8_t* out) {
    for (int p = 0; p < 2; ++p) {
        const uint16_t x1 = d[4 * p], y1 = d[4 * p + 1], x2 = d[4 * p + 2], y2 = d[4 * p + 3];
        out[5 * p + 0] = uint8_t(x1);
        out[5 * p + 1] = uint8_t(y1);
        out[5 * p + 2] = uint8_t(((y1 >> 8) & 3) << 6 | ((x1 >> 8) & 3) << 4 | ((y2 >> 8) & 3) << 2 | ((x2 >> 8) & 3));
        out[5 * p + 3] = uint8_t(x2);
        out[5 * p + 4] = uint8_t(y2);
    }
}

void ir_extended(const uint16_t* d, uint8_t* out) {
    for (int i = 0; i < 4; ++i) {
        const uint16_t x = d[2 * i], y = d[2 * i + 1];
        const bool seen = !(x == 1023 && y == 1023);
        out[3 * i + 0] = uint8_t(x);
        out[3 * i + 1] = uint8_t(y);
        out[3 * i + 2] = uint8_t(((y >> 8) & 3) << 6 | ((x >> 8) & 3) << 4 | (seen ? 4 : 15));
    }
}

}  // namespace

void Wiimote::reset() {
    replies_.clear();
    last_.clear();
    input_ = {};
    leds_ = 0; mode_ = 0x30;
    continuous_ = rumble_ = ir_on_ = speaker_on_ = false;
    frame_ = 0;
    eeprom_.fill(0); ext_.fill(0); speaker_.fill(0); ir_.fill(0);
    wiimote_calibration(&eeprom_[0x16]);
    wiimote_calibration(&eeprom_[0x20]);
    nunchuk_calibration(&ext_[0x20]);
    nunchuk_calibration(&ext_[0x30]);
    static const uint8_t id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};  // Nunchuk
    std::memcpy(&ext_[0xFA], id, sizeof id);
}

void Wiimote::buttons_bytes(uint8_t& b0, uint8_t& b1) const {
    b0 = uint8_t(input_.buttons & 0x1F);
    b1 = uint8_t((input_.buttons >> 8) & 0x9F);
}

void Wiimote::reply_status() {
    uint8_t b0, b1;
    buttons_bytes(b0, b1);
    const uint8_t flags = uint8_t((1 << 1) /* Nunchuk conectado */ | (speaker_on_ ? 4 : 0) | (ir_on_ ? 8 : 0) | (leds_ << 4));
    replies_.push_back({0x20, b0, b1, flags, 0, 0, 0xC0});
}

void Wiimote::reply_ack(uint8_t id, uint8_t error) {
    uint8_t b0, b1;
    buttons_bytes(b0, b1);
    replies_.push_back({0x22, b0, b1, id, error});
}

// Espacios: EEPROM (0x1700 bytes) y registros por esclavo I2C en el byte alto: 0xA2 altavoz, 0xA4 extensión,
// 0xB0 cámara IR. 0xA6 sería el Motion Plus: no existe, error 7. Fuera de la EEPROM, error 8.
uint8_t Wiimote::peek(bool registers, uint32_t address, int& error) {
    if (!registers) {
        if (address < eeprom_.size()) return eeprom_[address];
        error = 8;
        return 0;
    }
    const uint32_t slave = (address >> 16) & 0xFF, off = address & 0xFFFF;
    if (off >= 0x100) { error = 8; return 0; }
    switch (slave) {
    case 0xA2: return speaker_[off];
    case 0xA4:
        if (off >= 0x08 && off < 0x0E) {  // lectura directa de los datos del Nunchuk
            uint8_t n[6];
            nunchuk_bytes(input_, frame_, n);
            return n[off - 0x08];
        }
        return ext_[off];
    case 0xB0: return ir_[off];
    default: error = 7; return 0;
    }
}

bool Wiimote::poke(bool registers, uint32_t address, uint8_t value, int& error) {
    if (!registers) {
        if (address >= eeprom_.size()) { error = 8; return false; }
        eeprom_[address] = value;
        return true;
    }
    const uint32_t slave = (address >> 16) & 0xFF, off = address & 0xFFFF;
    if (off >= 0x100) { error = 8; return false; }
    switch (slave) {
    case 0xA2: speaker_[off] = value; return true;
    case 0xA4: ext_[off] = value; return true;  // 0xF0/0xFB: el SDK quita el cifrado y aquí nunca lo hay
    case 0xB0: ir_[off] = value; return true;
    default: error = 7; return false;
    }
}

void Wiimote::read_memory(const uint8_t* p, size_t n) {
    if (n < 6) return;
    const bool registers = (p[0] & 0x04) != 0;
    uint32_t address = (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    uint32_t left = (uint32_t(p[4]) << 8) | p[5];
    uint8_t b0, b1;
    buttons_bytes(b0, b1);
    while (left) {
        const uint32_t chunk = std::min<uint32_t>(left, 16);
        Bytes r(6 + 16, 0);  // 21 BB BB SE AA AA + 16 bytes de datos
        int error = 0;
        for (uint32_t i = 0; i < chunk && !error; ++i) r[6 + i] = peek(registers, address + i, error);
        if (error) std::fill(r.begin() + 6, r.end(), 0);
        r[0] = 0x21; r[1] = b0; r[2] = b1;
        r[3] = uint8_t(((chunk - 1) << 4) | error);
        r[4] = uint8_t(address >> 8); r[5] = uint8_t(address);  // 16 bits bajos de la dirección
        replies_.push_back(std::move(r));
        address += chunk;
        left -= chunk;
    }
}

void Wiimote::write_memory(const uint8_t* p, size_t n) {
    if (n < 6) return;
    const bool registers = (p[0] & 0x04) != 0;
    const uint32_t address = (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    const uint32_t size = std::min<uint32_t>(p[4], 16);
    int error = 0;
    for (uint32_t i = 0; i < size && 5 + i < n && !error; ++i) poke(registers, address + i, p[5 + i], error);
    reply_ack(0x16, uint8_t(error));
}

void Wiimote::output_report(const uint8_t* d, size_t n) {
    if (!n) return;
    const uint8_t id = d[0];
    const uint8_t* p = d + 1;
    const size_t len = n - 1;
    if (id >= 0x10 && id <= 0x1A && id != 0x18 && len) rumble_ = (p[0] & 1) != 0;
    switch (id) {
    case 0x10: break;
    case 0x11:
        if (len) { leds_ = p[0] >> 4; if (p[0] & 2) reply_ack(id, 0); }
        break;
    case 0x12:
        if (len >= 2) {
            continuous_ = (p[0] & 4) != 0;
            mode_ = p[1];
            last_.clear();
            if (p[0] & 2) reply_ack(id, 0);
        }
        break;
    case 0x13: case 0x1A:
        if (len) { ir_on_ = (p[0] & 4) != 0; reply_ack(id, 0); }
        break;
    case 0x14:
        if (len) { speaker_on_ = (p[0] & 4) != 0; reply_ack(id, 0); }
        break;
    case 0x15: reply_status(); break;
    case 0x16: write_memory(p, len); break;
    case 0x17: read_memory(p, len); break;
    case 0x18: break;  // datos del altavoz: se descartan
    case 0x19: reply_ack(id, 0); break;
    default: {
        static bool logged[256] = {};
        if (!logged[id]) { logged[id] = true; std::fprintf(stderr, "[wpad] informe de salida %02x sin implementar\n", id); }
        break;
    }
    }
}

bool Wiimote::pop_reply(Bytes& report) {
    if (replies_.empty()) return false;
    report = std::move(replies_.front());
    replies_.pop_front();
    return true;
}

bool Wiimote::data_report(const Input& in, Bytes& report) {
    input_ = in;
    ++frame_;
    uint8_t id = mode_;
    if (id != 0x3D && (id < 0x30 || id > 0x37)) {
        static bool logged = false;
        if (!logged) { logged = true; std::fprintf(stderr, "[wpad] modo de datos %02x no soportado: se usa 0x37\n", id); }
        id = 0x37;
    }
    uint8_t b0, b1;
    buttons_bytes(b0, b1);
    const bool has_accel = id == 0x31 || id == 0x33 || id == 0x35 || id == 0x37;
    const Accel a = shaken({kRestX, kRestY, kRestZ}, in.shake, frame_);
    if (has_accel) {
        b0 |= uint8_t((a.x & 3) << 5);
        b1 |= uint8_t((((a.y >> 1) & 1) << 5) | (((a.z >> 1) & 1) << 6));
    }
    uint8_t nun[21] = {};
    nunchuk_bytes(in, frame_, nun);
    uint16_t dots[8];
    ir_dots(in, ir_on_, dots);
    uint8_t ir[12];

    Bytes r{id};
    auto add = [&r](const uint8_t* p, size_t n) { r.insert(r.end(), p, p + n); };
    const uint8_t core[2] = {b0, b1}, accel[3] = {uint8_t(a.x >> 2), uint8_t(a.y >> 2), uint8_t(a.z >> 2)};
    switch (id) {
    case 0x30: add(core, 2); break;
    case 0x31: add(core, 2); add(accel, 3); break;
    case 0x32: add(core, 2); add(nun, 8); break;
    case 0x33: ir_extended(dots, ir); add(core, 2); add(accel, 3); add(ir, 12); break;
    case 0x34: add(core, 2); add(nun, 19); break;
    case 0x35: add(core, 2); add(accel, 3); add(nun, 16); break;
    case 0x36: ir_basic(dots, ir); add(core, 2); add(ir, 10); add(nun, 9); break;
    case 0x37: ir_basic(dots, ir); add(core, 2); add(accel, 3); add(ir, 10); add(nun, 6); break;
    case 0x3D: add(nun, 21); break;
    }
    if (!continuous_ && r == last_) return false;
    last_ = r;
    report = std::move(r);
    return true;
}

}  // namespace wpad
