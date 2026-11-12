// dsp_hle.cpp — HLE del DSP de Wii (ver dsp_hle.h). Modelo, sacado de cómo lo usa el SDK:
//
//  * DSPCR (0x500A): AIDINT/ARINT/DSPINT (0x08/0x20/0x80) se limpian escribiendo 1 y sus máscaras van un bit
//    más arriba. RES (0x01) y PIINT (0x02) son disparadores y no se guardan. INIT (0x800) sí se guarda.
//  * Escribir RES reinicia el DSP en su ROM: manda 0x8071FEED. Bajar INIT (1->0) arranca el código de
//    __OSInitAudioSystem desde ARAM: manda 0x80544348.
//  * ROM: el SDK sube el microcódigo con parejas "etiqueta, dato" (0x80F3A001 dirección, 0x80F3C002 destino,
//    0x80F3A002 longitud, 0x80F3B002 DRAM, 0x80F3D001 vector de inicio). Con la última arranca el microcódigo.
//  * AX: el microcódigo contesta DSP_INIT (0xDCD10000) al arrancar, DSP_RESUME (0xDCD10001) a 0xCDD10000 y
//    DSP_YIELD (0xDCD10002) al terminar una lista de comandos (0xBABE0000|tamaño, y luego su dirección).
//    0xCDD10001 anuncia un cambio de microcódigo (siete correos de parámetros).
//  * Los buzones son de un solo correo: el DSP no manda el siguiente hasta que la CPU lee el bajo (0x5006).
//    Aquí se encolan. Los correos con interrupción levantan DSPINT cuando llegan al frente.
//
// Todos los microcódigos se tratan como AX. Si uno usa otro protocolo (p. ej. el de Zelda/Mario Galaxy) los
// correos desconocidos se registran con CACTUS_TRACE_DSP=1 y el juego se quedará esperando.
#include "dsp_hle.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace dsp {
namespace {

constexpr uint16_t CR_RES = 0x01, CR_PIINT = 0x02, CR_HALT = 0x04, CR_AIINT = 0x08, CR_AIMSK = 0x10,
                   CR_ARINT = 0x20, CR_ARMSK = 0x40, CR_DSPINT = 0x80, CR_DSPMSK = 0x100, CR_INIT = 0x800;
constexpr uint16_t CR_W1C = CR_AIINT | CR_ARINT | CR_DSPINT;
constexpr uint16_t CR_RW = CR_HALT | CR_AIMSK | CR_ARMSK | CR_DSPMSK | CR_INIT;

constexpr uint32_t ROM_READY = 0x8071FEED, INITAUDIO_READY = 0x80544348;
constexpr uint32_t MAIL_RESUME = 0xCDD10000, MAIL_NEW_UCODE = 0xCDD10001, MAIL_RESET = 0xCDD10002,
                   MAIL_CONTINUE = 0xCDD10003, MAIL_CMDLIST = 0xBABE0000, MAIL_CMDLIST_MASK = 0xFFFF0000;
constexpr uint32_t DSP_INIT = 0xDCD10000, DSP_RESUME = 0xDCD10001, DSP_YIELD = 0xDCD10002;
constexpr uint32_t ARAM_SIZE = 16u * 1024 * 1024;
constexpr uint32_t POLL_WARN = 2000000;

struct Mail { uint32_t value; bool irq; };

struct State {
    Host host{};
    bool trace = false;
    uint16_t cr = 0;
    Ucode ucode = Ucode::None;
    uint16_t cpu_hi = 0;                 // mitad alta del correo que la CPU está componiendo
    std::deque<Mail> to_cpu;
    uint16_t last_hi = 0, last_lo = 0;   // último correo leído, lo que se ve con el buzón vacío
    uint32_t empty_polls = 0;
    // ROM
    uint32_t rom_tag = 0, rom_ram = 0, rom_len = 0;
    // AX
    bool want_list = false;
    uint32_t list_size = 0;
    bool uploading = false;
    uint32_t upload[7] = {};
    unsigned upload_n = 0;
    uint32_t crc = 0, frames = 0;
    // ARAM
    uint32_t ar_mm = 0, ar_ar = 0, ar_cnt = 0;
    std::vector<uint8_t> aram;
} s;

#define TRACE(...) do { if (s.trace) { std::fprintf(stderr, "[dsp] " __VA_ARGS__); std::fputc('\n', stderr); } } while (0)

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void update_irq() {
    const bool on = ((s.cr & CR_AIINT) && (s.cr & CR_AIMSK)) || ((s.cr & CR_ARINT) && (s.cr & CR_ARMSK)) ||
                    ((s.cr & CR_DSPINT) && (s.cr & CR_DSPMSK));
    if (s.host.irq_line) s.host.irq_line(on);
}

// El correo del frente del buzón DSP->CPU es el visible; si pide interrupción, se levanta ahora.
void present() {
    if (!s.to_cpu.empty() && s.to_cpu.front().irq) {
        s.cr |= CR_DSPINT;
        update_irq();
    }
}

void push(uint32_t value, bool irq) {
    TRACE("DSP -> CPU %08X%s", value, irq ? " +irq" : "");
    s.to_cpu.push_back({value, irq});
    s.empty_polls = 0;
    if (s.to_cpu.size() == 1) present();
}

void flush_mail() {
    s.to_cpu.clear();
    s.empty_polls = 0;
}

// ---------------------------------------------------------------- arranque de microcódigos
void start_rom() {
    TRACE("reinicio: ROM");
    s.ucode = Ucode::Rom;
    s.rom_tag = s.rom_ram = s.rom_len = 0;
    s.want_list = s.uploading = false;
    flush_mail();
    push(ROM_READY, false);
}

void start_init_audio() {
    TRACE("arranque desde ARAM (__OSInitAudioSystem)");
    s.ucode = Ucode::InitAudio;
    flush_mail();
    push(INITAUDIO_READY, false);
}

void start_ax(uint32_t image, uint32_t length) {
    s.crc = 0;
    if (length && length <= 0x10000)
        if (const uint8_t* p = s.host.span ? s.host.span(image, length) : nullptr) s.crc = crc32(p, length);
    TRACE("microcódigo en %08X, %u bytes, CRC32 %08X: se trata como AX", image, length, s.crc);
    s.ucode = Ucode::Ax;
    s.want_list = s.uploading = false;
    flush_mail();
    push(DSP_INIT, true);
}

void rom_mail(uint32_t m) {
    if (s.rom_tag) {
        switch (s.rom_tag & 0xFFFF) {
        case 0xA001: s.rom_ram = m; break;
        case 0xA002: s.rom_len = m; break;
        case 0xD001: s.rom_tag = 0; start_ax(s.rom_ram, s.rom_len); return;
        default: break;  // C002 (destino en IRAM) y B002 (DRAM): no hacen falta
        }
        s.rom_tag = 0;
        return;
    }
    if ((m & 0xFFFF0000u) == 0x80F30000u) { s.rom_tag = m; return; }
    TRACE("ROM: correo inesperado %08X", m);
}

// ---------------------------------------------------------------- AX
// Aquí se mezclaría el audio. La lista (AX: CMD_SETUP 0x00, PBADDR 0x02, PROCESS 0x03, MIX_AUX* 0x04-0x06,
// OUTPUT 0x0E, END 0x0F) recorre la cadena de PBs de las voces y deja L/R en el búfer del DMA de audio.
// Por ahora solo se cuenta: el búfer de salida del juego queda como lo dejó (cero tras AXInit) y suena silencio.
void ax_list(uint32_t addr, uint32_t size) {
    ++s.frames;
    if (s.trace && s.host.span) {
        if (const uint8_t* p = s.host.span(addr, 4))
            TRACE("lista AX #%u en %08X (tamaño %u), primer comando %02X%02X", s.frames, addr, size, p[0], p[1]);
    }
}

void ax_mail(uint32_t m) {
    if (s.uploading) {
        s.upload[s.upload_n++] = m;
        if (s.upload_n == 7) {  // dram addr/len/dst, iram addr/len/dst, vector de inicio
            s.uploading = false;
            start_ax(s.upload[3], s.upload[4]);
        }
        return;
    }
    if (s.want_list) {
        s.want_list = false;
        ax_list(m, s.list_size);
        push(DSP_YIELD, true);
        return;
    }
    if (m == MAIL_RESUME) { push(DSP_RESUME, true); return; }
    if (m == MAIL_NEW_UCODE) { s.uploading = true; s.upload_n = 0; return; }
    if (m == MAIL_RESET) { start_rom(); return; }
    if (m == MAIL_CONTINUE) return;
    if ((m & MAIL_CMDLIST_MASK) == MAIL_CMDLIST) { s.want_list = true; s.list_size = m & 0xFFFF; return; }
    TRACE("AX: correo desconocido %08X", m);
}

void cpu_mail(uint32_t m) {
    TRACE("CPU -> DSP %08X", m);
    switch (s.ucode) {
    case Ucode::Rom: rom_mail(m); break;
    case Ucode::Ax: ax_mail(m); break;
    default: TRACE("correo ignorado (sin microcódigo)"); break;
    }
}

// ---------------------------------------------------------------- ARAM
void aram_dma() {
    if (s.aram.empty()) s.aram.assign(ARAM_SIZE, 0);
    const bool to_main = (s.ar_cnt & 0x80000000u) != 0;
    uint32_t n = s.ar_cnt & 0x7FFFFFE0u;
    const uint32_t main = s.ar_mm & 0x03FFFFE0u, ar = s.ar_ar;
    TRACE("DMA ARAM %s: memoria %08X, ARAM %08X, %u bytes", to_main ? "->MEM" : "MEM->", main, ar, n);
    if (n && ar < ARAM_SIZE && n <= ARAM_SIZE - ar && s.host.span) {
        if (uint8_t* m = s.host.span(main, n)) {
            if (to_main) {
                std::copy(s.aram.begin() + ar, s.aram.begin() + ar + n, m);
                if (s.host.mem_written) s.host.mem_written(main | 0x80000000u, n);
            } else {
                std::copy(m, m + n, s.aram.begin() + ar);
            }
        }
    }
    s.ar_cnt = 0;
    s.cr |= CR_ARINT;  // el DMA termina al instante
    update_irq();
}

}  // namespace

// ---------------------------------------------------------------- interfaz
void init(const Host& host) {
    s = State{};
    s.host = host;
    s.trace = std::getenv("CACTUS_TRACE_DSP") != nullptr;
    update_irq();
}

void refresh_irq() { update_irq(); }

bool handles(uint32_t reg) {
    return (reg >= 0x5000 && reg <= 0x5006) || reg == 0x500A || (reg >= 0x5020 && reg <= 0x502A);
}

uint16_t read(uint32_t reg) {
    switch (reg) {
    case 0x5000: return s.cpu_hi & 0x7FFF;  // la CPU ya no tiene correo pendiente: el DSP lo lee al instante
    case 0x5002: return 0;
    case 0x5004:
        if (s.to_cpu.empty()) {
            if (++s.empty_polls == POLL_WARN)
                std::fprintf(stderr, "[dsp] aviso: la CPU lleva mucho esperando un correo del DSP (microcódigo %d). "
                                     "Ejecuta con CACTUS_TRACE_DSP=1 para ver el protocolo.\n", int(s.ucode));
            return s.last_hi & 0x7FFF;
        }
        return uint16_t((s.to_cpu.front().value >> 16) | 0x8000u);
    case 0x5006:
        if (s.to_cpu.empty()) return s.last_lo;
        s.last_hi = uint16_t(s.to_cpu.front().value >> 16);
        s.last_lo = uint16_t(s.to_cpu.front().value);
        s.to_cpu.pop_front();
        s.empty_polls = 0;
        present();
        return s.last_lo;
    case 0x500A: return s.cr;
    case 0x5020: return uint16_t(s.ar_mm >> 16);
    case 0x5022: return uint16_t(s.ar_mm);
    case 0x5024: return uint16_t(s.ar_ar >> 16);
    case 0x5026: return uint16_t(s.ar_ar);
    case 0x5028: return uint16_t(s.ar_cnt >> 16);
    case 0x502A: return uint16_t(s.ar_cnt);
    default: return 0;
    }
}

void write(uint32_t reg, uint16_t v) {
    switch (reg) {
    case 0x5000: s.cpu_hi = v; break;
    case 0x5002: cpu_mail((uint32_t(s.cpu_hi) << 16) | v); break;
    case 0x500A: {
        const uint16_t prev = s.cr;
        s.cr = uint16_t(s.cr & ~(v & CR_W1C));
        s.cr = uint16_t((s.cr & ~CR_RW) | (v & CR_RW));
        if (v & CR_PIINT) TRACE("la CPU interrumpe al DSP");
        if (v & CR_RES) start_rom();
        if ((prev & CR_INIT) && !(v & CR_INIT)) start_init_audio();
        update_irq();
        break;
    }
    case 0x5020: s.ar_mm = (s.ar_mm & 0x0000FFFFu) | (uint32_t(v) << 16); break;
    case 0x5022: s.ar_mm = (s.ar_mm & 0xFFFF0000u) | (v & 0xFFE0u); break;
    case 0x5024: s.ar_ar = (s.ar_ar & 0x0000FFFFu) | (uint32_t(v) << 16); break;
    case 0x5026: s.ar_ar = (s.ar_ar & 0xFFFF0000u) | (v & 0xFFE0u); break;
    case 0x5028: s.ar_cnt = (s.ar_cnt & 0x0000FFFFu) | (uint32_t(v) << 16); break;
    case 0x502A:
        s.ar_cnt = (s.ar_cnt & 0xFFFF0000u) | (v & 0xFFE0u);
        aram_dma();
        break;
    default: break;
    }
}

void raise_status(uint16_t bits) {
    s.cr |= (bits & CR_W1C);
    update_irq();
}

Ucode current_ucode() { return s.ucode; }
uint32_t ucode_crc() { return s.crc; }
uint32_t frames_processed() { return s.frames; }

}  // namespace dsp
