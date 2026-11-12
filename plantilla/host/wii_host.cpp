// wii_host.cpp — host del port: CPUState de DolRecomp + MMIO de Wii sobre SDL2.
// El renderer GLES de Nebula dibuja (gx_bridge.cpp); el CPU corre en juego_glue.cpp.
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "dev_di.h"
#include "dev_fs.h"
#include "dev_misc.h"
#include "dsp_hle.h"
#include "guest_memory.h"
#include "sdl_host.h"
#include "dev_bt.h"
#include "sysconf.h"
#include "galaxy/input_bindings.h"
#include "wii_boot.h"
#include "wii_dvd.h"
#include "wii_gx.h"
#include "galaxy/gx_d3d12.h"

static wii::PakDisc g_pak;
static CPUState g_cpu;
static SDL_AudioDeviceID g_audio = 0;
static uint32_t g_ai_rate = 32000;

static uint32_t g_pi_mask;
static uint8_t* guest_span(uint32_t addr, size_t length) {
    return wii::resolve(&g_cpu, addr, length);
}

// ---------------------------------------------------------------- CP / PE
// Todo lo que el juego manda al GX pasa por el write-gather pipe (0xCC008000) y se entrega al renderer en
// cuanto llega. El FIFO del CP queda siempre vacío y ocioso: sus punteros se guardan para que GX los relea,
// pero no se vuelven a enviar (eso duplicaba el dibujo) ni levantan la interrupción del CP (que nadie limpiaba).
static uint16_t g_cp[0x40] = {};
static uint16_t g_pe_ctrl = 0;

static uint16_t cp_read(uint32_t reg) {
    switch (reg) {
    case 0x00: return 0x000E;              // subdesbordado, lectura ociosa, órdenes ociosas
    case 0x30: case 0x32: return 0;        // distancia lectura-escritura
    case 0x38: return g_cp[0x34 / 2];      // puntero de lectura = de escritura
    case 0x3A: return g_cp[0x36 / 2];
    default: return reg < 0x80 ? g_cp[reg / 2] : 0;
    }
}

static uint16_t pe_ctrl_read() {
    const uint32_t cause = cactus::gx_pi_cause_exchange(0);
    return uint16_t((g_pe_ctrl & ~0xCu) | ((cause & 0x200u) ? 4u : 0u) | ((cause & 0x400u) ? 8u : 0u));
}

static uint32_t xfb_addr(const uint16_t* vi) {
    uint32_t info = (uint32_t(vi[0x1c/2]) << 16) | vi[0x1e/2];
    return (info & 0xffffffu) << ((info & (1u<<28)) ? 5 : 0);
}

// ---------------------------------------------------------------- VI
static uint16_t g_vi[0x40] = {};

static void vi_update(bool retrace) {
    bool irq = false;
    for (unsigned reg = 0x30; reg <= 0x3c; reg += 4) {
        uint16_t& high = g_vi[reg / 2];
        if (retrace && (high & 0x1000)) high |= 0x8000;
        irq |= (high & 0x9000) == 0x9000;
    }
    if (irq) cactus::gx_pi_raise(0x100u);
    else cactus::gx_pi_cause_exchange(0x100u);
}

// ---------------------------------------------------------------- DI directo (estilo GameCube)
static struct {
    uint32_t status, cover, cmd[3], address, length, control, immediate;
} di;

static void di_irq() {
    if (di.status & (di.status << 1) & 0x54u) cactus::gx_pi_raise(0x4);
    else cactus::gx_pi_cause_exchange(0x4);
}

static void di_transfer() {
    uint32_t n = di.cmd[2];
    uint8_t* dst = guest_span(di.address, n);
    bool ok = (di.cmd[0] >> 24) == 0xa8 && n <= di.length &&
              (di.control & 7u) == 3u && dst &&
              g_pak.read(uint64_t(di.cmd[1]) << 2, n, dst);
    if (ok) {
        galaxy::gx::notify_guest_memory_write(di.address | 0x80000000u, n);
        di.address += n;
        di.length -= n;
    }
    di.status |= ok ? 0x10u : 0x4u;
    di.control &= ~1u;
    di_irq();
}

// ---------------------------------------------------------------- DSP / AI
// DSPCR, buzones, arranque de microcódigo, AX y DMA de ARAM viven en dsp_hle.cpp. Aquí solo quedan el DMA de
// audio hacia SDL (0x5030-0x503A) y la conexión con PI y la memoria del invitado.
static void dsp_irq_line(bool on) {
    if (on) cactus::gx_pi_raise(0x40u);
    else cactus::gx_pi_cause_exchange(0x40u);
}
static void dsp_mem_written(uint32_t addr, size_t length) {
    galaxy::gx::notify_guest_memory_write(addr, uint32_t(length));
}

// DMA PCM del DSP. SDL recibe estéreo big-endian en bloques de 32 bytes.
static struct {
    uint32_t source, cursor, control, left, ai_control;
} audio_dma;

static void open_audio(uint32_t rate) {
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    g_ai_rate = rate;
    SDL_AudioSpec want{};
    want.freq = int(rate);
    want.format = AUDIO_S16MSB;
    want.channels = 2;
    want.samples = 1024;
    g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
    else std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
}

static void pump_audio() {
    if (!g_audio || !(audio_dma.control & 0x8000) || !(audio_dma.control & 0x7fff)) return;
    const uint32_t target = (g_ai_rate / 30) * 4;
    uint32_t queued = SDL_GetQueuedAudioSize(g_audio);
    bool finished = false;
    while (queued < target) {
        if (!audio_dma.left) {
            audio_dma.left = audio_dma.control & 0x7fff;
            audio_dma.cursor = audio_dma.source;
        }
        uint32_t blocks = std::min(audio_dma.left, (target - queued + 31) / 32);
        uint32_t n = blocks * 32;
        auto* src = guest_span(audio_dma.cursor, n);
        if (!src || SDL_QueueAudio(g_audio, src, n) != 0) break;
        audio_dma.cursor += n;
        audio_dma.left -= blocks;
        queued += n;
        finished |= audio_dma.left == 0;
    }
    if (finished) {  // fin de bloque: el juego encola el siguiente en su interrupción AID
        dsp::raise_status(0x08);  // AIDINT
    }
}

// ---------------------------------------------------------------- MMIO
static bool gx_page(uint32_t ea) {
    const uint32_t page = ea & 0xffff0000u;
    return page == 0xcc000000u || page == 0x0c000000u;
}
static bool io_page(uint32_t ea) {
    const uint32_t page = ea & 0xffff0000u;
    return gx_page(ea) || page == 0xcd000000u || page == 0x0d000000u ||
           page == 0xcd800000u || page == 0x0d800000u;
}

static bool ipc_register(uint32_t ea) {
    const uint32_t base = ea & 0x3fffffc0u;
    return base == 0x0d000000u || base == 0x0d800000u;
}

static void ipc_irq(CPUState*) {
    auto& ipc = ios::kernel();
    if (ipc.reg_read(0x30) & ipc.reg_read(0x34))
        cactus::gx_pi_raise(1u << 14);
    else
        cactus::gx_pi_cause_exchange(1u << 14);
}

static uint64_t mmio_read(CPUState* cpu, uint32_t ea, uint8_t size) {
    if (uint8_t* lc = cactus::locked_cache(ea, size)) {
        uint64_t v = 0;
        for (unsigned i = 0; i < size; ++i) v = (v << 8) | lc[i];
        return v;
    }
    if (!io_page(ea)) return 0;
    if (ipc_register(ea)) return size == 4 ? ios::kernel().reg_read(ea) : 0;
    const uint32_t reg = ea & 0xffffu;
    if (gx_page(ea) && size == 4 && ((reg < 0x80) || (reg >= 0x2000 && reg < 0x2080) || (reg >= 0x5000 && reg < 0x5040)))
        return (mmio_read(cpu, ea, 2) << 16) | mmio_read(cpu, ea + 2, 2);
    if (gx_page(ea) && dsp::handles(reg)) return dsp::read(reg);
    if (gx_page(ea) && reg < 0x80) return cp_read(reg);
    if (gx_page(ea) && reg >= 0x2000 && reg < 0x2080) return g_vi[(reg - 0x2000) / 2];
    switch (reg) {
    case 0x100a: return pe_ctrl_read();
    case 0x100e: return cactus::gx_pe_token_value();
    case 0x3000: return cactus::gx_pi_cause_exchange(0) | 0x10000u;  // RSWST: botón RESET suelto
    case 0x3004: return g_pi_mask;
    case 0x302c: return 0x246500B1u;  // revisión de Flipper/Hollywood
    case 0x6000: return di.status;
    case 0x6004: return di.cover;
    case 0x6008: case 0x600c: case 0x6010: return di.cmd[(reg-0x6008)/4];
    case 0x6014: return di.address;
    case 0x6018: return di.length;
    case 0x601c: return di.control;
    case 0x6020: return di.immediate;
    case 0x5030: return audio_dma.source >> 16;
    case 0x5032: return audio_dma.source & 0xffff;
    case 0x5036: return audio_dma.control;
    case 0x503a: return audio_dma.left ? audio_dma.left-1 : 0;
    case 0x6c00: return audio_dma.ai_control;
    default: return 0;
    }
}

static void mmio_write(CPUState* cpu, uint32_t ea, uint64_t value, uint8_t size) {
    if (uint8_t* lc = cactus::locked_cache(ea, size)) {
        for (unsigned i = 0; i < size; ++i) lc[i] = uint8_t(value >> (8 * (size - 1 - i)));
        return;
    }
    if (!io_page(ea)) return;
    if (ipc_register(ea)) {
        if (size == 4) ios::kernel().reg_write(ea, uint32_t(value));
        return;
    }
    const uint32_t reg = ea & 0xffffu;
    if (gx_page(ea) && reg >= 0x8000 && reg < 0x8020) {
        if (!size || size > 8 || reg + size > 0x8020) return;
        uint8_t bytes[8];
        for (unsigned i=0; i<size; ++i) bytes[i] = uint8_t(value >> (8*(size-1-i)));
        cactus::gx_fifo_write(bytes, size);
        return;
    }
    if (gx_page(ea) && size == 4 && ((reg < 0x80) || (reg >= 0x2000 && reg < 0x2080) || (reg >= 0x5000 && reg < 0x5040))) {
        mmio_write(cpu, ea, value >> 16, 2);
        mmio_write(cpu, ea + 2, value & 0xffff, 2);
        return;
    }
    if (gx_page(ea) && reg < 0x80) {
        if (size == 2) g_cp[reg / 2] = uint16_t(value);
        return;
    }
    if (gx_page(ea) && reg >= 0x2000 && reg < 0x2080) {
        if (size == 2) {
            g_vi[(reg-0x2000)/2] = uint16_t(value);
            if (reg >= 0x2030 && reg <= 0x203e) vi_update(false);
        }
        return;
    }
    if (gx_page(ea) && dsp::handles(reg)) {
        if (size == 2) dsp::write(reg, uint16_t(value));
        return;
    }
    uint32_t v = uint32_t(value);
    switch (reg) {
    case 0x100a: // PE_CONTROL: habilitaciones y acuses del token y de fin de dibujo.
        g_pe_ctrl = uint16_t(v & ~0xCu);
        cactus::gx_pi_cause_exchange(((v & 4) ? 0x200u : 0u) | ((v & 8) ? 0x400u : 0u));
        break;
    case 0x3000: cactus::gx_pi_cause_exchange(v); di_irq(); vi_update(false); ipc_irq(cpu); dsp::refresh_irq(); break;
    case 0x3004: g_pi_mask = v; break;
    case 0x6000: di.status = (di.status & 0x54u & ~v) | (v & 0x2au); di_irq(); break;
    case 0x6008: case 0x600c: case 0x6010: di.cmd[(reg-0x6008)/4] = v; break;
    case 0x6014: di.address = v & ~31u; break;
    case 0x6018: di.length = v & ~31u; break;
    case 0x601c: di.control = v & 7u; if (di.control & 1u) di_transfer(); break;
    case 0x6020: di.immediate = v; break;
    case 0x5030:
        if (size == 4) audio_dma.source = v & 0x1fffffe0u;
        else audio_dma.source = ((v & 0x1fff) << 16) | (audio_dma.source & 0xffff);
        break;
    case 0x5032: audio_dma.source = (audio_dma.source & 0xffff0000u) | (v & 0xffe0); break;
    case 0x5036:
        if (!(audio_dma.control & 0x8000) || !(v & 0x8000)) audio_dma.left = 0;
        audio_dma.control = v & 0xffff;
        if (!(v & 0x8000) && g_audio) SDL_ClearQueuedAudio(g_audio);
        break;
    case 0x6c00: {
        audio_dma.ai_control = v;
        uint32_t rate = (v & 0x40) ? 32000 : 48000;
        if (rate != g_ai_rate) open_audio(rate);
        break;
    }
    default: break;
    }
}
static void mmio_write32(CPUState* c, uint32_t ea, uint32_t v, uint8_t) { mmio_write(c, ea, v, 4); }

// mkdir -p dentro de la NAND del juego.
static bool make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); ++at) {
        if (at != path.size() && path[at] != '/') continue;
        const std::string part = path.substr(0, at);
        if (mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}


// Mando del handheld -> Wiimote + Nunchuk. Stick izquierdo = Nunchuk, cruz = cruz, sur = A, este = B (también RT),
// oeste = C, LB/LT = Z, RB = agitar, norte = 1, captura = 2. El stick derecho mueve el puntero IR (acumulado).
static wpad::Input read_pad(std::shared_ptr<ios::DevBt>& bt, float dt) {
  using namespace galaxy::input;
  static float px = 0, py = 0;
  static bool pointer_on = false;
  wpad::Input in;
  const auto st = galaxy::sdl::input_state();
  const PadSample* pad = nullptr;
  for (const PadSample& p : st.pads) if (p.connected) { pad = &p; break; }
  if (!pad) return in;
  auto down = [&](unsigned bit) { return (pad->buttons & pad::bit(bit)) != 0; };
  auto axis = [&](PadAxis a) { return pad->axes[size_t(a)]; };
  if (down(pad::kDpadLeftBit)) in.buttons |= wpad::Left;
  if (down(pad::kDpadRightBit)) in.buttons |= wpad::Right;
  if (down(pad::kDpadUpBit)) in.buttons |= wpad::Up;
  if (down(pad::kDpadDownBit)) in.buttons |= wpad::Down;
  if (down(pad::kStartBit)) in.buttons |= wpad::Plus;
  if (down(pad::kBackBit)) in.buttons |= wpad::Minus;
  if (down(pad::kHomeBit)) in.buttons |= wpad::Home;
  if (down(pad::kSouthBit)) in.buttons |= wpad::A;
  if (down(pad::kEastBit) || axis(PadAxis::RightTrigger) > 0.5f) in.buttons |= wpad::B;
  if (down(pad::kNorthBit)) in.buttons |= wpad::One;
  if (down(pad::kCaptureBit)) in.buttons |= wpad::Two;
  in.c = down(pad::kWestBit);
  in.z = down(pad::kLeftShoulderBit) || axis(PadAxis::LeftTrigger) > 0.5f;
  in.shake = down(pad::kRightShoulderBit);
  in.stick_x = axis(PadAxis::LeftX);
  in.stick_y = axis(PadAxis::LeftY);
  const float rx = axis(PadAxis::RightX), ry = -axis(PadAxis::RightY);  // y del puntero: abajo positivo
  if (rx != 0 || ry != 0) pointer_on = true;
  px = std::fmax(-1.f, std::fmin(1.f, px + rx * 1.6f * dt));
  py = std::fmax(-1.f, std::fmin(1.f, py + ry * 1.6f * dt));
  in.pointer = pointer_on;
  in.pointer_x = px;
  in.pointer_y = py;
  (void)bt;
  return in;
}

int main(int argc, char** argv) {
  const char* pak_path = nullptr;
  const char* nand_path = "nand";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--pak") && i + 1 < argc) pak_path = argv[++i];
    else if (!strcmp(argv[i], "--nand")) {
      if (++i == argc) { fprintf(stderr, "Falta el directorio de --nand\n"); return 2; }
      nand_path = argv[i];
    }
  }
  if (!pak_path) { fprintf(stderr, "uso: %s --pak game.pak [--nand dir]\n", argv[0]); return 2; }
  if (!g_pak.open(pak_path)) { fprintf(stderr, "no se pudo abrir %s\n", pak_path); return 1; }

  if (!cpu_init(&g_cpu)) return 1;
  if (!cpu_alloc_mem2(&g_cpu, WII_MEM2_SIZE)) return 1;   // MEM1 24 MiB + MEM2 64 MiB
  g_cpu.external_read32 = [](CPUState* cpu, uint32_t ea, uint8_t) -> uint32_t {
    return uint32_t(mmio_read(cpu, ea, 4));
  };
  g_cpu.external_read = mmio_read;
  g_cpu.external_write = mmio_write;
  g_cpu.external_write32 = mmio_write32;

  // Cargar las secciones del main.dol (datos inicializados) desde el pak a la RAM del juego.
  std::vector<uint8_t> dol(g_pak.dol_size());
  if (!g_pak.read(g_pak.dol_offset(), dol.size(), dol.data())) return 1;
  auto be32 = [](const uint8_t* p){ return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be32(dol.data() + 4*i), addr = be32(dol.data() + 0x48 + 4*i), n = be32(dol.data() + 0x90 + 4*i);
    if (!n) continue;
    uint8_t* dst = guest_span(addr, n);
    if (!dst || off > dol.size() || n > dol.size() - off) {
        std::fprintf(stderr, "Sección DOL %d fuera de rango\n", i);
        return 1;
    }
    memcpy(dst, dol.data() + off, n);
  }
  const uint32_t entry = be32(dol.data() + 0xE0);

  // Lo que el IPL, IOS y el apploader dejan en RAM antes de main.dol.
  wii::BootInfo boot;
  if (!wii::setup_boot_memory(&g_cpu, g_pak, boot)) {
    std::fprintf(stderr, "No se pudo preparar el arranque desde %s\n", pak_path);
    return 1;
  }

  // NAND del juego: su carpeta de datos (partidas) y las del sistema que el SDK espera.
  char title_dir[64];
  std::snprintf(title_dir, sizeof title_dir, "/title/%08x/%08x/data", uint32_t(boot.title_id >> 32),
                uint32_t(boot.title_id));
  for (const std::string& sub : {std::string(title_dir), std::string("/shared2/sys"), std::string("/tmp"),
                                std::string("/sys")}) {
    if (!make_dirs(std::string(nand_path) + sub)) { perror((std::string(nand_path) + sub).c_str()); return 1; }
  }
  auto fs = ios::make_dev_fs(nand_path);
  if (!fs) { perror(nand_path); return 1; }
  ios::kernel().init(&g_cpu, ipc_irq);
  ios::kernel().register_device("/dev/fs", fs);
  ios::kernel().register_device("/dev/di", std::make_shared<ios::DevDI>(g_pak));
  ios::register_misc_devices(ios::kernel(), boot.title_id);
  if (!wiisys::ensure(nand_path)) std::fprintf(stderr, "Aviso: no se pudo escribir SYSCONF en la NAND\n");
  auto bt = ios::make_bt_device();
  ios::kernel().register_device("/dev/usb/oh1/57e/305", bt);
  dsp::init({guest_span, dsp_irq_line, dsp_mem_written});

  // La capa SDL del fork: ventana, mandos (base de PortMaster), combinación de salida y audio.
  if (!galaxy::sdl::start()) {
    cpu_free(&g_cpu);
    return 1;
  }
  audio_dma.ai_control = 0x40;
  open_audio(g_ai_rate);
  if (!cactus::gx_init(&g_cpu, 640, 480)) {
    if (g_audio) SDL_CloseAudioDevice(g_audio);
    SDL_Quit();
    cpu_free(&g_cpu);
    return 1;
  }
  // El journal de DolRecomp informa desplazamientos relativos a MEM1.
  ppc_set_mem_write_journal([](uint32_t offset, uint32_t size, void*) {
    galaxy::gx::notify_guest_memory_write(0x80000000u + offset, size);
  }, nullptr);
  bool ok = cactus::game_init(&g_cpu, entry);

  const double freq = double(SDL_GetPerformanceFrequency());
  const double period = boot.pal ? 1.0 / 50.0 : 1.0 / 60.0;
  while (ok) {
    const Uint64 t0 = SDL_GetPerformanceCounter();
    // Eventos y mandos los atiende la capa del fork; MENU/SELECT + Start pide salir.
    galaxy::sdl::pump_events(0);
    if (cactus::gx_quit_requested() || ios::kernel().shutdown_requested()) break;
    bt->tick(read_pad(bt, float(period)));
    {
      const auto pads = galaxy::sdl::input_state();
      for (size_t i = 0; i < pads.pads.size(); ++i)
        if (pads.pads[i].connected) { galaxy::sdl::rumble(pads.pad_ids[i], bt->rumble() ? 1.0f : 0.0f); break; }
    }
    ok = cactus::game_frame(&g_cpu, g_pi_mask);
    if (!ok) break;
    pump_audio();
    vi_update(true);
    cactus::gx_frame(true, xfb_addr(g_vi));
    const double left = period - double(SDL_GetPerformanceCounter() - t0) / freq;
    if (left > 0.001) SDL_Delay(Uint32(left * 1000.0));
  }
  ppc_set_mem_write_journal(nullptr, nullptr);
  cactus::gx_shutdown();
  if (g_audio) SDL_CloseAudioDevice(g_audio);
  SDL_Quit();
  cpu_free(&g_cpu);
  return ok ? 0 : 1;
}
