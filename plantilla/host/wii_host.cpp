// wii_host.cpp — host del port: CPUState de DolRecomp + MMIO de Wii sobre SDL2.
// GX/DSP/WPAD se completan por juego en juego_glue.cpp (renderer GLES de Nebula).
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
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

static uint32_t xfb_addr() {
    return ((uint32_t(g_vi[0x12/2]) & 0x1FFFu) << 16) | uint32_t(g_vi[0x10/2]);
}

static uint64_t mmio_read(CPUState*, uint32_t ea, uint8_t) {
    const uint32_t reg = ea & 0xFFFF, block = reg & 0xFF00;
    if (reg == 0x100E) return cactus::gx_pe_token_value();
    if (reg == 0x3000) return cactus::gx_pi_cause_exchange(0);
    if (block == 0x2000) return g_vi[(reg & 0x7F) / 2];
    return 0;
}

static void mmio_write(CPUState*, uint32_t ea, uint64_t value, uint8_t size) {
    const uint32_t reg = ea & 0xFFFF, block = reg & 0xFF00;
    if (reg >= 0x8000) {
        uint8_t be[8];
        if (!size || size > sizeof be) return;
        for (int i = 0; i < size; ++i) be[i] = uint8_t(value >> (8 * (size - 1 - i)));
        cactus::gx_fifo_write(be, size);
        return;
    }
    switch (block) {
        case 0x0000: cp_write(reg & 0xFF, uint32_t(value)); break;
        case 0x2000: g_vi[(reg & 0x7F) / 2] = uint16_t(value); break;
        case 0x3000: if (reg == 0x3000) cactus::gx_pi_cause_exchange(uint32_t(value)); break;
    case 0x6000: {                                                         // DI
      static uint32_t di_buf = 0, di_len = 0, di_off = 0;
      if (reg == 0x6008) di_buf = (uint32_t)value;        // DIMAR: buffer DMA
      if (reg == 0x600C) di_len = (uint32_t)value;        // DILENGTH
      if (reg == 0x6000 && (value & 0x2)) {               // DISR: ack
      }
      if (reg == 0x6004 && (value & 1)) {                 // DTCR: arrancar DMA
        uint8_t* dst = g_cpu.ram + (di_buf & 0x3FFFFFFF);
        g_pak.read(uint64_t(di_off) << 2, di_len, dst);   // lee del game.pak
        // PUNTO DE INTEGRACIÓN: señalar DI completion + causa PI cuando el juego lo espere
      }
      // PUNTO DE INTEGRACIÓN: comandos DI (0xA8 READ, 0xAB SEEK...) desde el buffer de comandos
      break;
    }
    case 0x6C00: {                                        // AI: DMA de audio PCM BE16
      static uint32_t ai_buf = 0, ai_len = 0;
      if (reg == 0x6C00) ai_buf = (uint32_t)value;
      if (reg == 0x6C04) ai_len = (uint32_t)value & 0xFFFF;
      if (reg == 0x6C08) { /* AIS: start/stop */ }
      if (g_audio && ai_len) {
        uint8_t tmp[4096];
        for (uint32_t at = 0; at < ai_len; at += sizeof tmp) {
          uint32_t n = std::min<uint32_t>(sizeof tmp, ai_len - at);
          memcpy(tmp, g_cpu.ram + ((ai_buf + at) & 0x3FFFFFFF), n);
          for (uint32_t i = 0; i + 1 < n; i += 2) { uint8_t t = tmp[i]; tmp[i] = tmp[i+1]; tmp[i+1] = t; }
          SDL_QueueAudio(g_audio, tmp, n);
        }
      }
      break;
    }
    default: break;
  }
}
static void mmio_write32(CPUState* c, uint32_t ea, uint32_t v, uint8_t) { mmio_write(c, ea, v, 4); }

int main(int argc, char** argv) {
  const char* pak_path = nullptr;
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], "--pak") && i + 1 < argc) pak_path = argv[++i];
  if (!pak_path) { fprintf(stderr, "uso: %s --pak game.pak [--nand dir]\n", argv[0]); return 2; }
  if (!g_pak.open(pak_path)) { fprintf(stderr, "no se pudo abrir %s\n", pak_path); return 1; }

  if (!cpu_init(&g_cpu)) return 1;
  if (!cpu_alloc_mem2(&g_cpu, WII_MEM2_SIZE)) return 1;   // MEM1 24 MiB + MEM2 64 MiB
  g_cpu.external_read = mmio_read;
  g_cpu.external_write = mmio_write;
  g_cpu.external_write32 = mmio_write32;

  // Cargar las secciones del main.dol (datos inicializados) desde el pak a la RAM del juego.
  std::vector<uint8_t> dol(g_pak.dol_size());
  g_pak.read(g_pak.dol_offset(), dol.size(), dol.data());
  auto be32 = [](const uint8_t* p){ return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; };
  for (int i = 0; i < 18; ++i) {
    uint32_t off = be32(dol.data() + 4*i), addr = be32(dol.data() + 0x48 + 4*i), n = be32(dol.data() + 0x90 + 4*i);
    if (!n) continue;
    uint8_t* dst = (addr >= 0x90000000 && addr < 0x94000000) ? g_cpu.mem2 + (addr - 0x90000000)
                 : g_cpu.ram + (addr & 0x3FFFFFFF);
    memcpy(dst, dol.data() + off, n);
  }
  const uint32_t entry = be32(dol.data() + 0xE0);

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
  SDL_Window* win = SDL_CreateWindow("cactus-porter", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     640, 480, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
  SDL_AudioSpec want{}, have{};
  want.freq = g_ai_rate; want.format = AUDIO_S16MSB; want.channels = 2; want.samples = 1024;
  g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (g_audio) SDL_PauseAudioDevice(g_audio, 0);
  if (!cactus::gx_init(&g_cpu, 640, 480)) return 1;   // renderer GLES de Nebula (gx_bridge.cpp)
  // El journal de DolRecomp informa desplazamientos relativos a MEM1.
  ppc_set_mem_write_journal([](uint32_t offset, uint32_t size, void*) {
    galaxy::gx::notify_guest_memory_write(0x80000000u + offset, size);
  }, nullptr);
  cactus::game_init(&g_cpu, entry);

  Uint64 t0 = SDL_GetPerformanceCounter();
  const double freq = double(SDL_GetPerformanceFrequency());
  while (g_corriendo.load() && !cactus::gx_quit_requested()) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) g_corriendo = false;
      if (ev.type == SDL_CONTROLLERBUTTONDOWN && ev.cbutton.button == SDL_CONTROLLER_BUTTON_START)
        g_corriendo = false;  // PUNTO DE INTEGRACIÓN: combo de salida del fork (NEBULA_QUIT_COMBO)
    }
    cactus::game_frame(&g_cpu);   // slice de CPU recompilada + GX
    cactus::gx_pi_raise(0x20u);  // Retrace VI pendiente en PI.
    cactus::gx_frame(true, xfb_addr());
    // Ritmo de retrace: 60 Hz (el perfil bajo del fork salta frames si hace falta)
    double objetivo = (SDL_GetPerformanceCounter() - t0) / freq + 1.0/60.0;
    while ((SDL_GetPerformanceCounter() - t0) / freq < objetivo) SDL_Delay(1);
    t0 = SDL_GetPerformanceCounter();
  }
  ppc_set_mem_write_journal(nullptr, nullptr);
  cactus::gx_shutdown();
  if (g_audio) SDL_CloseAudioDevice(g_audio);
  SDL_DestroyWindow(win);
  SDL_Quit();
  cpu_free(&g_cpu);
  return 0;
}
