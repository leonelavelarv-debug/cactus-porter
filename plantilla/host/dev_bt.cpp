// dev_bt.cpp — ver dev_bt.h. Orden de eventos y parámetros según la especificación HCI/L2CAP; el flujo de
// conexión (petición de conexión del Wiimote, autenticación, L2CAP iniciado por la Wii) sigue lo que hace
// la pila de WUD. Todo se contesta al instante: no hay radio.
#include "dev_bt.h"
#include "perfil.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ios {
namespace {

constexpr u32 USB_CTRLMSG = 0, USB_BLKMSG = 1, USB_INTRMSG = 2;
constexpr uint8_t EP_ACL_OUT = 0x02, EP_EVENT = 0x81, EP_ACL_IN = 0x82;
constexpr uint16_t kHandle = 0x0100;
constexpr unsigned kConnectDelay = 120;     // cuadros entre habilitar el escaneo de páginas y la "pulsación"
constexpr unsigned kFallbackDelay = 900;    // sin escaneo en 15 s: se llama a la Wii igualmente
constexpr size_t kMaxQueued = 64;
constexpr uint8_t kWiiAddress[6] = {0x11, 0x02, 0x19, 0x79, 0x00, 0x00};  // dirección de la Wii, MSB primero
constexpr uint8_t kClassOfDevice[3] = {0x04, 0x25, 0x00};                 // 0x002504: mando de juegos

constexpr uint16_t OP_INQUIRY = 0x0401, OP_CREATE_CON = 0x0405, OP_DISCONNECT = 0x0406, OP_ACCEPT_CON = 0x0409,
    OP_REJECT_CON = 0x040A, OP_LINK_KEY_REP = 0x040B, OP_LINK_KEY_NEG_REP = 0x040C, OP_PIN_REP = 0x040D,
    OP_PIN_NEG_REP = 0x040E, OP_CHANGE_PACKET_TYPE = 0x040F, OP_AUTH_REQ = 0x0411, OP_SET_ENCRYPT = 0x0413,
    OP_REMOTE_NAME = 0x0419, OP_READ_REMOTE_FEATURES = 0x041B, OP_READ_REMOTE_VERSION = 0x041D,
    OP_READ_CLOCK_OFFSET = 0x041F, OP_HOLD_MODE = 0x0801, OP_SNIFF_MODE = 0x0803, OP_EXIT_SNIFF = 0x0804,
    OP_SWITCH_ROLE = 0x080B, OP_READ_LINK_POLICY = 0x080C, OP_WRITE_LINK_POLICY = 0x080D,
    OP_RESET = 0x0C03, OP_READ_PIN_TYPE = 0x0C09, OP_READ_STORED_KEY = 0x0C0D, OP_WRITE_STORED_KEY = 0x0C11,
    OP_DELETE_STORED_KEY = 0x0C12, OP_READ_LOCAL_NAME = 0x0C14, OP_WRITE_SCAN_ENABLE = 0x0C1A,
    OP_READ_COD = 0x0C23, OP_READ_LOCAL_VERSION = 0x1001, OP_READ_LOCAL_FEATURES = 0x1003,
    OP_READ_BUFFER_SIZE = 0x1005, OP_READ_BD_ADDR = 0x1009;

bool returns_status(uint16_t op) {
    switch (op) {
    case OP_INQUIRY: case OP_CREATE_CON: case OP_DISCONNECT: case OP_ACCEPT_CON: case OP_REJECT_CON:
    case OP_CHANGE_PACKET_TYPE: case OP_AUTH_REQ: case OP_SET_ENCRYPT: case OP_REMOTE_NAME:
    case OP_READ_REMOTE_FEATURES: case OP_READ_REMOTE_VERSION: case OP_READ_CLOCK_OFFSET: case OP_HOLD_MODE:
    case OP_SNIFF_MODE: case OP_EXIT_SNIFF: case OP_SWITCH_ROLE:
        return true;
    default:
        return false;
    }
}

void put16(wpad::Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put(wpad::Bytes& b, const wpad::Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

// Las direcciones viajan al revés (primero el byte bajo) en HCI.
wpad::Bytes hci_address(const uint8_t* msb_first) { return {msb_first[5], msb_first[4], msb_first[3], msb_first[2], msb_first[1], msb_first[0]}; }
bool is_wiimote(const uint8_t* hci) {
    for (int i = 0; i < 6; ++i) if (hci[i] != wpad::kAddress[5 - i]) return false;
    return true;
}
wpad::Bytes padded_name(const char* name, size_t size) {
    wpad::Bytes b(size, 0);
    std::memcpy(b.data(), name, std::min(std::strlen(name), size - 1));
    return b;
}
uint16_t be16(const u8* p) { return uint16_t((p[0] << 8) | p[1]); }
uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

}  // namespace

DevBt::DevBt(BtHooks hooks) : h_(std::move(hooks)) {}

s32 DevBt::open(const char*, u32) {
    reset_link();
    events_.clear(); acl_.clear(); pending_events_.clear(); pending_acl_.clear();
    opened_ = true;
    opened_at_ = frame_;
    return 0;
}

s32 DevBt::close(s32) {
    opened_ = false;
    pending_events_.clear(); pending_acl_.clear();
    return IPC_OK;
}

void DevBt::reset_link() {
    page_scan_ = request_sent_ = connected_ = linked_ = false;
    chan_[0] = {0x0050};
    chan_[1] = {0x0051};
    wm_.reset();
}

s32 DevBt::ioctl(s32, u32, u8*, u32, u8* out, u32 out_len) {
    if (out && out_len) std::memset(out, 0, out_len);
    return IPC_OK;  // suspender, reanudar, lista de dispositivos...: sin efecto
}

// ---------------------------------------------------------------- eventos y órdenes HCI
void DevBt::event(uint8_t code, const Bytes& params) {
    Bytes e{code, uint8_t(params.size())};
    put(e, params);
    events_.push_back(std::move(e));
}

void DevBt::command_complete(uint16_t opcode, const Bytes& ret) {
    Bytes p{1};
    put16(p, opcode);
    p.push_back(0);  // estado
    put(p, ret);
    event(0x0E, p);
}

void DevBt::command_status(uint16_t opcode) {
    Bytes p{0, 1};
    put16(p, opcode);
    event(0x0F, p);
}

void DevBt::connection_request() {
    request_sent_ = true;
    std::fprintf(stderr, "[bt] el Wiimote pide conectarse\n");
    CACTUS_MENSAJE("WPAD: el Wiimote pide conectarse");
    Bytes p = hci_address(wpad::kAddress);
    p.insert(p.end(), kClassOfDevice, kClassOfDevice + 3);
    p.push_back(1);  // enlace ACL
    event(0x04, p);
}

void DevBt::connection_complete() {
    connected_ = true;
    wm_.reset();
    Bytes p{0};
    put16(p, kHandle);
    put(p, hci_address(wpad::kAddress));
    p.push_back(1);  // ACL
    p.push_back(0);  // sin cifrado
    event(0x03, p);
}

void DevBt::command(const uint8_t* c, size_t n) {
    if (n < 3) return;
    const uint16_t op = le16(c);
    const size_t plen = std::min<size_t>(c[2], n - 3);
    const uint8_t* a = c + 3;
    const Bytes addr = plen >= 6 ? Bytes(a, a + 6) : Bytes(6, 0);
    const bool for_wiimote = plen >= 6 && is_wiimote(a);
    const uint16_t handle = plen >= 2 ? le16(a) : 0;
    if (returns_status(op)) command_status(op);

    switch (op) {
    case OP_RESET:
        reset_link();
        command_complete(op);
        break;
    case OP_WRITE_SCAN_ENABLE:
        command_complete(op);
        if (plen >= 1 && (a[0] & 2) && !page_scan_) { page_scan_ = true; scan_since_ = frame_; }
        else if (plen >= 1 && !(a[0] & 2)) page_scan_ = false;
        break;
    case OP_READ_BD_ADDR: command_complete(op, hci_address(kWiiAddress)); break;
    case OP_READ_BUFFER_SIZE: {
        Bytes r;
        put16(r, 339); r.push_back(64); put16(r, 10); put16(r, 8);  // ACL máx., SCO máx., paquetes ACL, SCO
        command_complete(op, r);
        break;
    }
    case OP_READ_LOCAL_VERSION: {
        Bytes r{3};  // HCI 1.2
        put16(r, 0); r.push_back(3); put16(r, 0x000F); put16(r, 0);
        command_complete(op, r);
        break;
    }
    case OP_READ_LOCAL_FEATURES: command_complete(op, {0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80}); break;
    case OP_READ_LOCAL_NAME: command_complete(op, padded_name("Nintendo RVL-CNT-01", 248)); break;
    case OP_READ_COD: command_complete(op, {0x04, 0x02, 0x00}); break;
    case OP_READ_PIN_TYPE: command_complete(op, {0}); break;
    case OP_READ_STORED_KEY: { Bytes r; put16(r, 1); put16(r, 0); command_complete(op, r); break; }
    case OP_DELETE_STORED_KEY: { Bytes r; put16(r, 0); command_complete(op, r); break; }
    case OP_WRITE_STORED_KEY: command_complete(op, {0}); break;
    case OP_READ_LINK_POLICY: { Bytes r; put16(r, handle); put16(r, 0); command_complete(op, r); break; }
    case OP_WRITE_LINK_POLICY: { Bytes r; put16(r, handle); command_complete(op, r); break; }

    case OP_INQUIRY:
        if (!connected_) {
            Bytes r{1};
            put(r, hci_address(wpad::kAddress));
            r.push_back(1); r.push_back(0); r.push_back(0);                   // modo de escaneo, reservado
            r.insert(r.end(), kClassOfDevice, kClassOfDevice + 3);
            put16(r, 0);                                                      // desfase de reloj
            event(0x02, r);
        }
        event(0x01, {0});
        break;
    case OP_CREATE_CON:
        if (for_wiimote) connection_complete();
        else { Bytes p{4}; put16(p, 0); put(p, addr); p.push_back(1); p.push_back(0); event(0x03, p); }  // no existe
        break;
    case OP_ACCEPT_CON:
        if (for_wiimote) {
            if (plen >= 7 && a[6] == 0) { Bytes p{0}; put(p, addr); p.push_back(0); event(0x12, p); }  // cambio de rol: maestro
            connection_complete();
        }
        break;
    case OP_REJECT_CON: break;
    case OP_DISCONNECT: {
        Bytes p{0};
        put16(p, handle);
        p.push_back(0x16);  // terminada por el host local
        event(0x05, p);
        connected_ = linked_ = false;
        chan_[0] = {0x0050}; chan_[1] = {0x0051};
        request_sent_ = false;
        scan_since_ = frame_;  // si la pila vuelve a escuchar, el Wiimote vuelve a conectarse
        break;
    }
    case OP_AUTH_REQ: event(0x17, hci_address(wpad::kAddress)); break;   // pide la clave de enlace
    case OP_LINK_KEY_REP: {
        Bytes r = addr;
        command_complete(op, r);
        Bytes p{0}; put16(p, kHandle); event(0x06, p);                    // autenticado
        break;
    }
    case OP_LINK_KEY_NEG_REP:
        command_complete(op, addr);
        event(0x16, hci_address(wpad::kAddress));                         // pide el PIN
        break;
    case OP_PIN_REP: {
        command_complete(op, addr);
        Bytes k = hci_address(wpad::kAddress);
        k.resize(6 + 16, 0x5A);
        k.push_back(0);
        event(0x18, k);                                                   // clave de enlace nueva
        Bytes p{0}; put16(p, kHandle); event(0x06, p);
        break;
    }
    case OP_PIN_NEG_REP: command_complete(op, addr); break;
    case OP_SET_ENCRYPT: { Bytes p{0}; put16(p, handle); p.push_back(plen >= 3 ? a[2] : 0); event(0x08, p); break; }
    case OP_REMOTE_NAME: {
        Bytes p{0};
        put(p, addr);
        put(p, padded_name(wpad::kName, 248));
        event(0x07, p);
        break;
    }
    case OP_READ_REMOTE_FEATURES: {
        Bytes p{0}; put16(p, handle);
        for (uint8_t b : {0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00}) p.push_back(b);
        event(0x0B, p);
        break;
    }
    case OP_READ_REMOTE_VERSION: {
        Bytes p{0}; put16(p, handle); p.push_back(2); put16(p, 0x0001); put16(p, 0);
        event(0x0C, p);
        break;
    }
    case OP_SWITCH_ROLE: { Bytes p{0}; put(p, addr); p.push_back(plen >= 7 ? a[6] : 0); event(0x12, p); break; }
    case OP_SNIFF_MODE: case OP_EXIT_SNIFF: case OP_HOLD_MODE: {
        Bytes p{0}; put16(p, handle); p.push_back(op == OP_SNIFF_MODE ? 2 : 0); put16(p, 0);
        event(0x14, p);
        break;
    }
    default:
        if (!returns_status(op)) command_complete(op);  // órdenes sin efecto aquí (máscaras, tiempos, parches...)
        break;
    }
}

// ---------------------------------------------------------------- ACL y L2CAP
void DevBt::send_acl(uint16_t cid, const Bytes& payload, bool data) {
    Bytes p;
    put16(p, uint16_t(kHandle | 0x2000));  // inicio de paquete
    put16(p, uint16_t(4 + payload.size()));
    put16(p, uint16_t(payload.size()));
    put16(p, cid);
    put(p, payload);
    if (data) acl_.erase(std::remove_if(acl_.begin(), acl_.end(), [](const AclPacket& a) { return a.data; }), acl_.end());
    if (acl_.size() < kMaxQueued) acl_.push_back({std::move(p), data});
}

void DevBt::signal(uint8_t code, uint8_t ident, const Bytes& data) {
    Bytes p{code, ident};
    put16(p, uint16_t(data.size()));
    put(p, data);
    send_acl(1, p);
}

void DevBt::l2cap_signal(const uint8_t* p, size_t n) {
    while (n >= 4) {
        const uint8_t code = p[0], ident = p[1];
        const size_t len = std::min<size_t>(le16(p + 2), n - 4);
        const uint8_t* d = p + 4;
        switch (code) {
        case 0x02:  // petición de conexión: psm, cid de origen
            if (len >= 4) {
                const uint16_t psm = le16(d), scid = le16(d + 2);
                Channel* ch = psm == 0x11 ? &chan_[0] : psm == 0x13 ? &chan_[1] : nullptr;
                Bytes r;
                if (!ch) {
                    put16(r, 0); put16(r, scid); put16(r, 2); put16(r, 0);  // PSM no admitido
                    signal(0x03, ident, r);
                    break;
                }
                ch->remote = scid;
                ch->host_configured = ch->we_configured = ch->opened = false;
                put16(r, ch->local); put16(r, scid); put16(r, 0); put16(r, 0);
                signal(0x03, ident, r);
                Bytes cfg;
                put16(cfg, scid); put16(cfg, 0);  // nuestra petición de configuración, sin opciones
                signal(0x04, ident_++, cfg);
            }
            break;
        case 0x04:  // petición de configuración del host para uno de nuestros canales
            if (len >= 4) {
                const uint16_t dcid = le16(d);
                for (Channel& ch : chan_) {
                    if (ch.local != dcid) continue;
                    Bytes r;
                    put16(r, ch.remote); put16(r, 0); put16(r, 0);  // aceptada tal cual
                    signal(0x05, ident, r);
                    ch.host_configured = true;
                    ch.opened = ch.host_configured && ch.we_configured;
                }
            }
            break;
        case 0x05:  // el host aceptó nuestra configuración
            if (len >= 6) {
                const uint16_t scid = le16(d), result = le16(d + 4);
                for (Channel& ch : chan_) {
                    if (ch.local != scid || result != 0) continue;
                    ch.we_configured = true;
                    ch.opened = ch.host_configured && ch.we_configured;
                }
            }
            break;
        case 0x06:  // desconexión
            if (len >= 4) {
                const uint16_t dcid = le16(d), scid = le16(d + 2);
                Bytes r;
                put16(r, dcid); put16(r, scid);
                signal(0x07, ident, r);
                for (Channel& ch : chan_) if (ch.local == dcid) { ch = Channel{ch.local}; }
            }
            break;
        case 0x08: signal(0x09, ident, Bytes(d, d + len)); break;  // eco
        case 0x0A: {  // información: no se admite
            Bytes r;
            put16(r, len >= 2 ? le16(d) : 0); put16(r, 1);
            signal(0x0B, ident, r);
            break;
        }
        case 0x07: case 0x09: case 0x0B: break;  // respuestas que no esperamos
        default: {
            Bytes r;
            put16(r, 0);  // orden no entendida
            signal(0x01, ident, r);
            break;
        }
        }
        p += 4 + len;
        n -= 4 + len;
    }
    linked_ = chan_[0].opened && chan_[1].opened;
}

void DevBt::hid_data(Channel& ch, const uint8_t* p, size_t n) {
    if (!n) return;
    if (p[0] == 0xA2 || p[0] == 0x52) {  // DATA/SET_REPORT de salida
        wm_.output_report(p + 1, n - 1);
        if (p[0] == 0x52) send_acl(ch.remote, {0x00});  // HANDSHAKE: correcto
    }
}

void DevBt::acl_in(const uint8_t* p, size_t n) {
    if (n < 8) return;
    const size_t dlen = std::min<size_t>(le16(p + 2), n - 4);
    if (dlen < 4) return;
    const uint8_t* l2 = p + 4;
    const size_t len = std::min<size_t>(le16(l2), dlen - 4);
    const uint16_t cid = le16(l2 + 2);
    if (cid == 1) { l2cap_signal(l2 + 4, len); return; }
    for (Channel& ch : chan_)
        if (ch.local == cid) { hid_data(ch, l2 + 4, len); return; }
}

void DevBt::send_report(const Bytes& report) {
    Bytes p{0xA1};  // DATA, entrada
    put(p, report);
    const bool data = report[0] >= 0x30;
    send_acl(chan_[1].remote, p, data);
}

// ---------------------------------------------------------------- entrega y ioctlv
void DevBt::deliver() {
    auto fill = [this](std::deque<Pending>& waiting, const Bytes& bytes) {
        const Pending w = waiting.front();
        waiting.pop_front();
        const u32 n = u32(std::min<size_t>(bytes.size(), w.capacity));
        if (w.buffer && n) std::memcpy(w.buffer, bytes.data(), n);
        h_.reply(w.request, s32(n));
    };
    while (!events_.empty() && !pending_events_.empty()) {
        Bytes e = std::move(events_.front());
        events_.pop_front();
        fill(pending_events_, e);
    }
    while (!acl_.empty() && !pending_acl_.empty()) {
        Bytes b = std::move(acl_.front().bytes);
        acl_.pop_front();
        fill(pending_acl_, b);
    }
}

s32 DevBt::ioctlv(s32, u32 request, u32 nin, u32 nout, IOVec* v) {
    const u32 count = nin + nout;
    const u32 me = h_.current_request ? h_.current_request() : 0;
    auto have = [&](u32 i, u32 len) { return i < count && v[i].data && v[i].len >= len; };
    switch (request) {
    case USB_CTRLMSG:  // órdenes HCI: tipo, petición, valor, índice y longitud, y los datos en el sexto vector
        if (!have(0, 1) || !have(1, 1) || !have(4, 2)) return IPC_EINVAL;
        {
            const uint8_t type = v[0].data[0];
            const u32 length = be16(v[4].data);
            if ((type & 0x60) == 0x20 && count > 5 && v[5].data)
                command(v[5].data, std::min<u32>(length, v[5].len));
            h_.reply(me, s32(length));
            deliver();
            return IPC_NO_REPLY;
        }
    case USB_BLKMSG: case USB_INTRMSG: {
        if (count < 3 || !have(0, 1) || !have(1, 2)) return IPC_EINVAL;
        const uint8_t endpoint = v[0].data[0];
        const u32 length = be16(v[1].data);
        IOVec& data = v[count - 1];
        if (request == USB_INTRMSG && endpoint == EP_EVENT) {
            pending_events_.push_back({me, data.data, data.len});
        } else if (request == USB_BLKMSG && endpoint == EP_ACL_IN) {
            pending_acl_.push_back({me, data.data, data.len});
        } else if (request == USB_BLKMSG && endpoint == EP_ACL_OUT) {
            if (data.data) acl_in(data.data, std::min<u32>(length, data.len));
            h_.reply(me, s32(length));
            Bytes credit{1};  // paquetes completados: libera un hueco de ACL en la pila
            put16(credit, kHandle); put16(credit, 1);
            event(0x13, credit);
        } else {
            return IPC_OK;
        }
        deliver();
        return IPC_NO_REPLY;
    }
    default:
        for (u32 i = nin; i < count; ++i) if (v[i].data && v[i].len) std::memset(v[i].data, 0, v[i].len);
        return IPC_OK;
    }
}

// ---------------------------------------------------------------- por cuadro
void DevBt::tick(const wpad::Input& input) {
    ++frame_;
    if (opened_ && !connected_ && !request_sent_) {
        if (page_scan_ && frame_ - scan_since_ >= kConnectDelay) connection_request();
        else if (!page_scan_ && frame_ - opened_at_ >= kFallbackDelay) {
            std::fprintf(stderr, "[bt] la pila no habilitó el escaneo de páginas; se avisa al Wiimote igualmente\n");
            connection_request();
        }
    }
    if (linked_) {
        wpad::Bytes r;
        while (wm_.pop_reply(r)) send_report(r);
        if (wm_.data_report(input, r)) send_report(r);
    }
    deliver();
}

std::shared_ptr<DevBt> make_bt_device() {
    return std::make_shared<DevBt>(BtHooks{
        [] { return kernel().current_request(); },
        [](u32 request, s32 result) { kernel().queue_reply(request, result); }});
}

}  // namespace ios
