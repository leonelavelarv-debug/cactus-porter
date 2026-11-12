#!/bin/sh
# Prueba PC de WPAD: hace de pila Bluetooth de la Wii contra /dev/usb/oh1/57e/305 (HCI, L2CAP, HID, Nunchuk) y
# comprueba el SYSCONF generado: ./tests/wpad.sh   (sin DolRecomp ni SDL)
set -eu
host=$(cd "$(dirname "$0")/../plantilla/host" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
cat > harness.cpp <<'CPP'
#include "dev_bt.h"
#include "sysconf.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
using namespace ios;
using Bytes = std::vector<uint8_t>;

// make_bt_device() usa el kernel real; aquí solo hace falta que enlace (el arnés usa sus propios ganchos).
namespace ios { Kernel& kernel() { static Kernel k; return k; } void Kernel::queue_reply(u32, s32) {} }

static u32 g_current = 0, g_next = 1;
static std::map<u32, s32> g_done;
struct Read { u32 req; Bytes buf; bool acl; };
static std::deque<Read*> g_reads;
static std::vector<Bytes> g_events, g_acl;
static DevBt* bt;

static u8 ep_byte; static u8 len_be[2];
static void post(bool acl) {
    Read* r = new Read{g_next++, Bytes(2048, 0), acl};
    ep_byte = acl ? 0x82 : 0x81; len_be[0] = 0x08; len_be[1] = 0x00;
    IOVec v[3] = {{&ep_byte, 1}, {len_be, 2}, {r->buf.data(), 2048}};
    g_current = r->req;
    s32 rc = bt->ioctlv(1, acl ? 1 : 2, 3, 0, v);
    assert(rc == IPC_NO_REPLY);
    g_reads.push_back(r);
}
static void collect() {  // recoge lo contestado y mantiene 4 lecturas de cada tipo
    for (auto it = g_reads.begin(); it != g_reads.end();) {
        auto d = g_done.find((*it)->req);
        if (d != g_done.end()) {
            Bytes b((*it)->buf.begin(), (*it)->buf.begin() + d->second);
            ((*it)->acl ? g_acl : g_events).push_back(b);
            delete *it; it = g_reads.erase(it);
        } else ++it;
    }
    int ev = 0, ac = 0;
    for (Read* r : g_reads) (r->acl ? ac : ev)++;
    for (; ev < 4; ++ev) post(false);
    for (; ac < 4; ++ac) post(true);
}
static void hci(uint16_t op, Bytes params = {}) {
    Bytes cmd{uint8_t(op), uint8_t(op >> 8), uint8_t(params.size())};
    cmd.insert(cmd.end(), params.begin(), params.end());
    u8 type = 0x20, req = 0, value[2] = {0, 0}, index[2] = {0, 0}, len[2] = {0, uint8_t(cmd.size())};
    IOVec v[6] = {{&type, 1}, {&req, 1}, {value, 2}, {index, 2}, {len, 2}, {cmd.data(), u32(cmd.size())}};
    g_current = g_next++;
    assert(bt->ioctlv(1, 0, 6, 0, v) == IPC_NO_REPLY);
    collect();
}
static void acl_out(Bytes payload, uint16_t cid) {  // paquete del host hacia el Wiimote
    Bytes p{0x00, 0x21, 0, 0, 0, 0, uint8_t(cid), uint8_t(cid >> 8)};
    p[2] = uint8_t(payload.size() + 4); p[4] = uint8_t(payload.size());
    p.insert(p.end(), payload.begin(), payload.end());
    u8 ep = 0x02, len[2] = {uint8_t(p.size() >> 8), uint8_t(p.size())};
    IOVec v[3] = {{&ep, 1}, {len, 2}, {p.data(), u32(p.size())}};
    g_current = g_next++;
    bt->ioctlv(1, 1, 3, 0, v);
    collect();
}
static void sig(uint8_t code, uint8_t ident, Bytes d) {
    Bytes p{code, ident, uint8_t(d.size()), 0};
    p.insert(p.end(), d.begin(), d.end());
    acl_out(p, 1);
}
static wpad::Input in;
static void ticks(int n) { for (int i = 0; i < n; ++i) { bt->tick(in); collect(); } }
static bool has_event(uint8_t code) { for (auto& e : g_events) if (e.size() >= 2 && e[0] == code) return true; return false; }
static Bytes take_event(uint8_t code) { for (size_t i = 0; i < g_events.size(); ++i) if (g_events[i][0] == code) { Bytes e = g_events[i]; g_events.erase(g_events.begin() + i); return e; } return {}; }
static bool complete(uint16_t op) { for (size_t i = 0; i < g_events.size(); ++i) { auto& e = g_events[i]; if (e[0] == 0x0E && e[3] == uint8_t(op) && e[4] == uint8_t(op >> 8)) return true; } return false; }
// Último informe HID (id dado) que llegó por el canal de interrupción: devuelve los bytes tras 0xA1.
static Bytes hid_report(uint8_t id) {
    for (size_t i = g_acl.size(); i-- > 0;) {
        Bytes& p = g_acl[i];
        if (p.size() > 10 && p[6] == 0x41 && p[7] == 0x00 && p[8] == 0xA1 && p[9] == id) return Bytes(p.begin() + 9, p.end());
    }
    return {};
}
static void hid_out(Bytes report) {  // DATA/salida por el canal de control 0x50
    report.insert(report.begin(), 0xA2);
    acl_out(report, 0x50);
}
static Bytes expect_acl_signal(uint8_t code) {
    for (auto& p : g_acl) if (p.size() >= 12 && p[6] == 1 && p[7] == 0 && p[8] == code) return Bytes(p.begin() + 8, p.end());
    return {};
}

int main() {
    DevBt dev(BtHooks{[] { return g_current; }, [](u32 r, s32 res) { g_done[r] = res; }});
    bt = &dev;
    assert(dev.open("/dev/usb/oh1/57e/305", 0) == 0);
    collect();

    hci(0x0C03);
    assert(complete(0x0C03));
    g_events.clear();
    hci(0x1009);  // BD_ADDR de la Wii, al revés
    {
        Bytes e = take_event(0x0E);
        assert(e.size() == 12 && e[5] == 0 && e[6] == 0x00 && e[7] == 0x00 && e[8] == 0x79 && e[9] == 0x19 && e[10] == 0x02 && e[11] == 0x11);
    }
    hci(0x1005); assert(complete(0x1005)); g_events.clear();

    hci(0x0C1A, {0x02});  // escaneo de páginas
    assert(complete(0x0C1A)); g_events.clear();
    ticks(100);
    assert(!has_event(0x04));
    ticks(40);
    assert(has_event(0x04));  // el Wiimote pide conectarse
    {
        Bytes e = take_event(0x04);
        assert(e[2] == 0x01 && e[3] == 0xC7 && e[4] == 0xCA && e[5] == 0x1D && e[6] == 0x19 && e[7] == 0x00);  // 00:19:1D:CA:C7:01 al revés
    }
    g_events.clear();
    const Bytes wm = {0x01, 0xC7, 0xCA, 0x1D, 0x19, 0x00};
    Bytes accept = wm; accept.push_back(0);
    hci(0x0409, accept);
    assert(has_event(0x0F) && has_event(0x03));
    { Bytes c = take_event(0x03); assert(c[2] == 0 && c[3] == 0x00 && c[4] == 0x01); }
    g_events.clear();

    hci(0x0411, {0x00, 0x01});  // autenticar
    assert(has_event(0x17));
    Bytes neg = wm; hci(0x040C, neg); assert(has_event(0x16));
    Bytes pin = wm; pin.push_back(6); pin.insert(pin.end(), {1, 2, 3, 4, 5, 6}); pin.resize(6 + 1 + 16, 0);
    hci(0x040D, pin);
    assert(has_event(0x18) && has_event(0x06));
    g_events.clear();

    // L2CAP: control (PSM 0x11) e interrupción (PSM 0x13), cada uno con configuración en los dos sentidos.
    for (auto [psm, scid, ident] : {std::tuple<uint16_t, uint16_t, uint8_t>{0x11, 0x0040, 1}, {0x13, 0x0041, 2}}) {
        g_acl.clear();
        sig(0x02, ident, {uint8_t(psm), 0, uint8_t(scid), uint8_t(scid >> 8)});
        Bytes rsp = expect_acl_signal(0x03);
        assert(rsp.size() >= 12 && rsp[4] == (psm == 0x11 ? 0x50 : 0x51) && rsp[6] == uint8_t(scid) && rsp[8] == 0 && rsp[10] == 0);
        assert(!expect_acl_signal(0x04).empty());                       // la Wii pide su configuración
        const uint8_t local = psm == 0x11 ? 0x50 : 0x51;
        sig(0x04, 10, {local, 0, 0, 0});                                 // el host configura el canal
        assert(!expect_acl_signal(0x05).empty());
        sig(0x05, 1, {local, 0, 0, 0, 0, 0});  // y acepta la configuración de la Wii
    }
    assert(dev.linked());

    // HID: informe de estado
    g_acl.clear();
    hid_out({0x15, 0x00});
    ticks(1);
    { Bytes s = hid_report(0x20); assert(s.size() >= 7); }

    // Nunchuk: desactivar el cifrado (0x55 a A400F0, 0x00 a A400FB) y leer su identificador
    g_acl.clear();
    {
        Bytes w1{0x16, 0x04, 0xA4, 0x00, 0xF0, 0x01, 0x55}; w1.resize(2 + 5 + 16, 0);
        hid_out(w1);
        Bytes w2{0x16, 0x04, 0xA4, 0x00, 0xFB, 0x01, 0x00}; w2.resize(2 + 5 + 16, 0);
        hid_out(w2);
        ticks(1);
        assert(!hid_report(0x22).empty());
        hid_out({0x17, 0x04, 0xA4, 0x00, 0xFA, 0x00, 0x06});
        ticks(1);
        Bytes r = hid_report(0x21);
        assert(r.size() >= 6 + 6 && (r[3] >> 4) == 5 && (r[3] & 0xF) == 0);  // tamaño-1 = 5, sin error
        const uint8_t id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
        assert(std::memcmp(&r[6], id, 6) == 0);
        // calibración del Nunchuk (16 bytes en 0x20): reposo del stick en 0x80
        g_acl.clear();
        hid_out({0x17, 0x04, 0xA4, 0x00, 0x20, 0x00, 0x10});
        ticks(1);
        r = hid_report(0x21);
        assert(r.size() >= 22 && r[6 + 8] == 0xE0 && r[6 + 9] == 0x20 && r[6 + 10] == 0x80);
        // ausencia de MotionPlus: lectura en el esclavo 0xA6 da error 7
        g_acl.clear();
        hid_out({0x17, 0x04, 0xA6, 0x00, 0xFA, 0x00, 0x06});
        ticks(1);
        r = hid_report(0x21);
        assert(!r.empty() && (r[3] & 0xF) == 7);
    }

    // Modo 0x37 continuo con A pulsado y el stick a la derecha
    g_acl.clear();
    hid_out({0x12, 0x04, 0x37});
    in.buttons = wpad::A | wpad::Up; in.stick_x = 1.0f; in.stick_y = 0.0f; in.z = true;
    ticks(2);
    {
        Bytes r = hid_report(0x37);
        assert(r.size() == 22);
        // el orden de bytes de botones es core0 = cruz/+, core1 = 2 1 B A - Home; comprobamos ambos
        assert((r[1] & 0x08) != 0);          // Arriba (0x0008) en el primer byte
        assert((r[2] & 0x08) != 0);          // A (0x0800) en el segundo byte
        const uint8_t* nk = &r[1 + 2 + 3 + 10];
        assert(nk[0] == 0xE0 && nk[1] == 0x80);   // stick X a tope, Y al centro
        assert((nk[5] & 1) == 0 && (nk[5] & 2) != 0);   // Z pulsado (bit activo a 0), C suelto
    }
    // Vibración del Wiimote: bit 0 de un informe de salida
    hid_out({0x10, 0x01});
    assert(dev.rumble());
    hid_out({0x10, 0x00});
    assert(!dev.rumble());

    // Cierre limpio
    assert(dev.close(1) == IPC_OK);

    // SYSCONF
    std::vector<uint8_t> f = wiisys::build();
    FILE* o = fopen("SYSCONF", "wb"); fwrite(f.data(), 1, f.size(), o); fclose(o);
    std::puts("wpad ok");
    return 0;
}
CPP
g++ -std=c++20 -Wall -Wextra -Werror -I"$host" harness.cpp "$host/dev_bt.cpp" "$host/wiimote.cpp" "$host/sysconf.cpp" -o harness
./harness
python3 - <<'PY'
import struct
f = open('SYSCONF', 'rb').read()
assert len(f) == 0x4000 and f[:4] == b'SCv0' and f[-4:] == b'SCed'
n, = struct.unpack('>H', f[4:6])
offs = struct.unpack('>%dH' % n, f[6:6 + 2 * n])
items = {}
for o in offs:
    t = f[o] >> 5
    name_len = (f[o] & 0x1F) + 1
    name = f[o + 1:o + 1 + name_len].decode()
    p = o + 1 + name_len
    if t == 1:
        size, = struct.unpack('>H', f[p:p + 2]); data = f[p + 2:p + 2 + size + 1]
    elif t == 3 or t == 7: data = f[p:p + 1]
    elif t == 5: data = f[p:p + 4]
    items[name] = (t, data)
t, d = items['BT.DINF']
assert t == 1 and len(d) == 0x461 and d[0] == 1
assert d[1:7] == bytes([0x00, 0x19, 0x1D, 0xCA, 0xC7, 0x01]) and d[7:26] == b'Nintendo RVL-CNT-01'
assert len(items['BT.CDIF'][1]) == 0x205
print('sysconf ok')
PY
