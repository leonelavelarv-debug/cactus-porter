// wii_host.cpp — host del port: CPUState de DolRecomp + MMIO de Wii sobre SDL2.
// GX/DSP/WPAD se completan por juego en juego_glue.cpp (renderer GLES de Nebula).
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include "dev_di.h"
#include "dev_fs.h"
#include "guest_memory.h"
#include "wii_dvd.h"
#include "wii_gx.h"
#include "galaxy/gx_d3d12.h"

static wii::PakDisc g_pak;
static CPUState g_cpu;
static SDL_AudioDeviceID g_audio = 0;
static uint32_t g_ai_rate = 32000;
static std::atomic<bool> g_corriendo{true};

// Estado GX/VI del host. end es exclusivo en este anillo.
static struct { uint32_t base=0, end=0, wp=0, rp=0; bool started=false; } g_fifo;
static uint16_t g_vi[0x40] = {};

static void fifo_drain() {
    const auto base = g_fifo.base, end = g_fifo.end, wp = g_fifo.wp;
    if (end <= base || !g_cpu.ram || wp < base || wp >= end) return;
    const uint32_t physical = base & 0x3FFFFFFFu;
    if (physical >= g_cpu.ram_size || end - base > g_cpu.ram_size - physical) return;
    uint32_t rp = g_fifo.started ? g_fifo.rp : base;
    if (rp < base || rp >= end || rp == wp) return;
    uint32_t count = wp >= rp ? wp - rp : (end - rp) + (wp - base);
    while (count) {
        uint32_t span = std::min({count, end - rp, 4096u});
        cactus::gx_fifo_write(g_cpu.ram + physical + (rp - base), span);
        rp += span;
        if (rp == end) rp = base;
        count -= span;
    }
    g_fifo.rp = wp;
    g_fifo.started = true;
    cactus::gx_pi_raise(0x800u);
}

static void cp_write(uint32_t reg, uint32_t v) {
    auto lo = [](uint32_t& r, uint32_t x){ r = (r & 0xFFFF0000u) | (x & 0xFFFFu); };
    auto hi = [](uint32_t& r, uint32_t x){ r = (r & 0x0000FFFFu) | ((x & 0xFFFFu) << 16); };
    switch (reg) {
        case 0x20: lo(g_fifo.base, v); g_fifo.started = false; break;
        case 0x22: hi(g_fifo.base, v); g_fifo.started = false; break;
        case 0x24: lo(g_fifo.end, v); break;
        case 0x26: hi(g_fifo.end, v); break;
        case 0x34: lo(g_fifo.wp, v); fifo_drain(); break;
        case 0x36: hi(g_fifo.wp, v); fifo_drain(); break;
        default: break;
    }
}

static uint32_t g_pi_mask;
static uint8_t* guest_span(uint32_t addr, size_t length) {
    return wii::resolve(&g_cpu, addr, length);
}

static uint32_t xfb_addr() {
    uint32_t info = (uint32_t(g_vi[0x1c/2]) << 16) | g_vi[0x1e/2];
    return (info & 0xffffffu) << ((info & (1u<<28)) ? 5 : 0);
}

// DI de lectura directa sobre el espacio lógico descifrado del game.pak.
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
    }
}

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

static uint64_t mmio_read(CPUState* cpu, uint32_t ea, uint8_t size) {
    if (!io_page(ea)) return 0;
    if (ipc_register(ea)) return size == 4 ? ios::kernel().reg_read(ea) : 0;
    const uint32_t reg = ea & 0xffffu;
    if (reg >= 0x2000 && reg < 0x2080 && gx_page(ea)) {
        if (size == 4 && reg <= 0x207c)
            return (mmio_read(cpu, ea, 2) << 16) | mmio_read(cpu, ea+2, 2);
        return g_vi[(reg-0x2000)/2];
    }
    switch (reg) {
    case 0x100e: return cactus::gx_pe_token_value();
    case 0x3000: return cactus::gx_pi_cause_exchange(0);
    case 0x3004: return g_pi_mask;
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
    if (gx_page(ea) && reg < 0x100) { cp_write(reg, uint32_t(value)); return; }
    if (gx_page(ea) && reg >= 0x2000 && reg < 0x2080) {
        if (size == 4 && reg <= 0x207c) {
            mmio_write(cpu, ea, value >> 16, 2);
            mmio_write(cpu, ea+2, value, 2);
        } else if (size == 2) {
            g_vi[(reg-0x2000)/2] = uint16_t(value);
            if (reg >= 0x2030 && reg <= 0x203e) vi_update(false);
        }
        return;
    }
    uint32_t v = uint32_t(value);
    switch (reg) {
    case 0x100a: // PE_CONTROL: acuses del token y de fin de dibujo.
        cactus::gx_pi_cause_exchange(((v & 4) ? 0x200u : 0u) | ((v & 8) ? 0x400u : 0u));
        break;
    case 0x3000: cactus::gx_pi_cause_exchange(v); di_irq(); vi_update(false); ipc_irq(cpu); break;
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

  auto fs = ios::make_dev_fs(nand_path);
  if (!fs) { perror(nand_path); return 1; }
  if (!cpu_init(&g_cpu)) return 1;
  if (!cpu_alloc_mem2(&g_cpu, WII_MEM2_SIZE)) return 1;   // MEM1 24 MiB + MEM2 64 MiB
  ios::kernel().init(&g_cpu, ipc_irq);
  ios::kernel().register_device("/dev/fs", fs);
  ios::kernel().register_device("/dev/di", std::make_shared<ios::DevDI>(g_pak));
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

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    cpu_free(&g_cpu);
    return 1;
  }
  // El backend KNULLI crea la única ventana fbdev y su contexto EGL.
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
  while (ok && g_corriendo.load() && !cactus::gx_quit_requested()) {
    const Uint64 t0 = SDL_GetPerformanceCounter();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) g_corriendo = false;
      if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)
        g_corriendo = false;  // PUNTO DE INTEGRACIÓN: combo de salida del fork (NEBULA_QUIT_COMBO)
    }
    if (!g_corriendo.load()) break;
    ok = cactus::game_frame(&g_cpu, g_pi_mask);
    if (!ok) break;
    pump_audio();
    vi_update(true);
    cactus::gx_frame(true, xfb_addr());
    while ((SDL_GetPerformanceCounter() - t0) / freq < 1.0/60.0) SDL_Delay(1);
  }
  ppc_set_mem_write_journal(nullptr, nullptr);
  cactus::gx_shutdown();
  if (g_audio) SDL_CloseAudioDevice(g_audio);
  SDL_Quit();
  cpu_free(&g_cpu);
  return ok ? 0 : 1;
}
