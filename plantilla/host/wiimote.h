// wiimote.h — Wiimote virtual con Nunchuk: informes HID, memoria (EEPROM y registros), extensión e IR.
// No depende de IOS ni de SDL: dev_bt.cpp lo mete en la pila Bluetooth emulada y wii_host.cpp le da los mandos.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace wpad {

using Bytes = std::vector<uint8_t>;

// Botones como van en los dos bytes "core" de los informes.
enum Button : uint16_t {
    Left = 0x0001, Right = 0x0002, Down = 0x0004, Up = 0x0008, Plus = 0x0010,
    Two = 0x0100, One = 0x0200, B = 0x0400, A = 0x0800, Minus = 0x1000, Home = 0x8000,
};

struct Input {
    uint16_t buttons = 0;
    float stick_x = 0, stick_y = 0;        // Nunchuk, -1..1 (arriba positivo)
    bool c = false, z = false;
    bool shake = false;                    // agitar el Wiimote
    bool nunchuk_shake = false;            // agitar el Nunchuk
    bool pointer = false;                  // el puntero IR está apuntando a la pantalla
    float pointer_x = 0, pointer_y = 0;    // -1..1; y positivo hacia abajo
};

// Direcciones del Wiimote emulado (orden de escritura habitual, MSB primero). La Wii las guarda así en SYSCONF.
inline constexpr uint8_t kAddress[6] = {0x00, 0x19, 0x1D, 0xCA, 0xC7, 0x01};
inline constexpr char kName[] = "Nintendo RVL-CNT-01";

class Wiimote {
 public:
    Wiimote() { reset(); }
    void reset();                                           // conexión nueva: modos y memoria por defecto

    void output_report(const uint8_t* data, size_t size);   // informe de salida del host: data[0] = id
    bool pop_reply(Bytes& report);                          // respuestas encoladas (estado, lectura, ack); incluye el id
    // Informe de datos periódico según el modo elegido; false si no toca enviar uno.
    bool data_report(const Input& input, Bytes& report);

    bool rumble() const { return rumble_; }
    uint8_t leds() const { return leds_; }
    uint8_t report_mode() const { return mode_; }
    bool ir_enabled() const { return ir_on_; }

 private:
    void reply_status();
    void reply_ack(uint8_t id, uint8_t error);
    void read_memory(const uint8_t* p, size_t n);
    void write_memory(const uint8_t* p, size_t n);
    uint8_t peek(bool registers, uint32_t address, int& error);
    bool poke(bool registers, uint32_t address, uint8_t value, int& error);
    void buttons_bytes(uint8_t& b0, uint8_t& b1) const;

    std::deque<Bytes> replies_;
    Bytes last_;                 // último informe de datos enviado (modos sin envío continuo)
    Input input_;                // lo último que se vio, para leer el Nunchuk por registros
    uint8_t leds_ = 0, mode_ = 0x30;
    bool continuous_ = false, rumble_ = false, ir_on_ = false, speaker_on_ = false;
    unsigned frame_ = 0;
    std::array<uint8_t, 0x1700> eeprom_{};
    std::array<uint8_t, 0x100> ext_{}, speaker_{}, ir_{};
};

}  // namespace wpad
