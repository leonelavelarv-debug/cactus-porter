// /dev/usb/oh1/57e/305: el adaptador Bluetooth de la Wii, con un Wiimote (y Nunchuk) virtual al otro lado.
// La pila Bluetooth vive dentro del juego (WUD/BTE): habla HCI por este dispositivo con tres ioctlv de USB
// (0 control = órdenes HCI, 1 bulk = datos ACL, 2 interrupción = eventos HCI) y mantiene peticiones de lectura
// abiertas que IOS solo contesta cuando hay algo que entregar. Aquí se contestan las órdenes HCI, se hace
// "reconectar" al Wiimote (petición de conexión, L2CAP en los PSM 0x11 y 0x13) y se le pasan los informes HID.
#pragma once
#include "ios.h"
#include "wiimote.h"
#include <deque>
#include <functional>
#include <memory>

namespace ios {

// El kernel de IOS, o uno falso en las pruebas.
struct BtHooks {
    std::function<u32()> current_request;
    std::function<void(u32 request, s32 result)> reply;
};

class DevBt final : public Device {
 public:
    explicit DevBt(BtHooks hooks);
    s32 open(const char* path, u32 mode) override;
    s32 close(s32) override;
    s32 ioctl(s32, u32 request, u8* in, u32 in_len, u8* out, u32 out_len) override;
    s32 ioctlv(s32, u32 request, u32 nin, u32 nout, IOVec* v) override;

    // Una vez por cuadro: avanza los temporizadores y manda el informe de datos del Wiimote.
    void tick(const wpad::Input& input);
    bool linked() const { return linked_; }          // canales de control e interrupción abiertos
    bool rumble() const { return wm_.rumble(); }
    const wpad::Wiimote& wiimote() const { return wm_; }

 private:
    using Bytes = wpad::Bytes;
    struct Pending { u32 request; u8* buffer; u32 capacity; };
    struct AclPacket { Bytes bytes; bool data; };
    struct Channel { uint16_t local; uint16_t remote = 0; bool opened = false, host_configured = false, we_configured = false; };

    void reset_link();
    void event(uint8_t code, const Bytes& params);
    void command_complete(uint16_t opcode, const Bytes& ret = {});
    void command_status(uint16_t opcode);
    void command(const uint8_t* p, size_t n);
    void connection_request();
    void connection_complete();
    void send_acl(uint16_t cid, const Bytes& payload, bool data = false);
    void signal(uint8_t code, uint8_t ident, const Bytes& data);
    void acl_in(const uint8_t* p, size_t n);
    void l2cap_signal(const uint8_t* p, size_t n);
    void hid_data(Channel& ch, const uint8_t* p, size_t n);
    void send_report(const Bytes& report);
    void deliver();

    BtHooks h_;
    wpad::Wiimote wm_;
    std::deque<Bytes> events_;
    std::deque<AclPacket> acl_;
    std::deque<Pending> pending_events_, pending_acl_;
    Channel chan_[2] = {{0x0050}, {0x0051}};
    unsigned frame_ = 0, opened_at_ = 0, scan_since_ = 0;
    uint8_t ident_ = 1;
    bool opened_ = false, page_scan_ = false, request_sent_ = false, connected_ = false, linked_ = false;
};

// Con el kernel de IOS real.
std::shared_ptr<DevBt> make_bt_device();

}  // namespace ios
