// gx_bridge.cpp — alimenta el renderer GLES de Nebula con el stream GX que produce
// el código recompilado por DolRecomp. Sustituye al stub GX de juego_glue.cpp.
#include "wii_gx.h"
#include "galaxy/gx_d3d12.h"     // API pública galaxy::gx (GxBackend compilada GLES)
#include "galaxy/native_api.h"   // galaxy::GuestMemoryV1, galaxy::NativeServicesV1

#include <atomic>
#include <cstdio>
#include <deque>
#include <vector>

namespace gx = galaxy::gx;

namespace {
galaxy::GuestMemoryRegionV1 g_regions[5];
galaxy::GuestMemoryV1       g_memory;
galaxy::NativeServicesV1    g_services;
bool                        g_listo = false;

std::vector<std::byte>      g_ring;      // bytes GX acumulados en el retrace
std::deque<gx::FramePeCompletionToken> g_tokens;  // receipts por consumir (una vez cada uno)

std::atomic<uint16_t> g_pe_token{0};
std::atomic<uint32_t> g_pi_cause{0};     // causas PI pendientes (el juego lee 0xCC003000)

void servicio_log(void*, galaxy::LogLevelV1, const char* msg) {
    std::fprintf(stderr, "[gx] %s\n", msg ? msg : "");
}
void servicio_fatal(void*, uint32_t pc, const char* msg) {
    std::fprintf(stderr, "[gx FATAL @ %08x] %s\n", pc, msg ? msg : "");
}
// El renderer los llama desde el hilo de render: solo atómicos.
void pe_finish(void*) {
    g_pi_cause.fetch_or(0x4000u, std::memory_order_release);   // INT_CAUSE_PE_FINISH
}
void pe_token(void*, uint16_t token, bool interrupt) {
    g_pe_token.store(token, std::memory_order_release);
    if (interrupt) g_pi_cause.fetch_or(0x2000u, std::memory_order_release);  // INT_CAUSE_PE_TOKEN
}
}  // namespace

namespace cactus {

bool gx_init(CPUState* cpu, int width, int height) {
    auto set = [](int i, uint32_t base, uint32_t size, uint8_t* host) {
        g_regions[i] = galaxy::GuestMemoryRegionV1{base, size, reinterpret_cast<std::byte*>(host)};
    };
    // MEM1 (24 MiB) y MEM2 (64 MiB) con sus espejos, tal como los resuelve el renderer.
    set(0, 0x00000000u, cpu->ram_size,  cpu->ram);
    set(1, 0x80000000u, cpu->ram_size,  cpu->ram);
    set(2, 0xC0000000u, cpu->ram_size,  cpu->ram);
    set(3, 0x90000000u, cpu->mem2_size, cpu->mem2);
    set(4, 0xD0000000u, cpu->mem2_size, cpu->mem2);
    g_memory.struct_size  = sizeof(galaxy::GuestMemoryV1);
    g_memory.region_count = 5;
    g_memory.regions      = g_regions;
    g_memory.fast_regions[0x0] = {cpu->ram_size,  reinterpret_cast<std::byte*>(cpu->ram)};
    g_memory.fast_regions[0x8] = {cpu->ram_size,  reinterpret_cast<std::byte*>(cpu->ram)};
    g_memory.fast_regions[0xC] = {cpu->ram_size,  reinterpret_cast<std::byte*>(cpu->ram)};
    g_memory.fast_regions[0x9] = {cpu->mem2_size, reinterpret_cast<std::byte*>(cpu->mem2)};
    g_memory.fast_regions[0xD] = {cpu->mem2_size, reinterpret_cast<std::byte*>(cpu->mem2)};

    g_services.struct_size = sizeof(galaxy::NativeServicesV1);
    g_services.log         = servicio_log;
    g_services.fatal       = servicio_fatal;
    g_services.gx_pe_finish = pe_finish;
    g_services.gx_pe_token  = pe_token;

    g_ring.reserve(4 * 1024 * 1024);

    if (!gx::initialize(width, height)) {
        std::fprintf(stderr, "gx initialize: %s\n", gx::startup_failure().c_str());
        return false;
    }
    gx::install_guest_dirty_tracker(&g_memory);  // el renderer rastrea páginas sucias
    gx::set_indexed_load_host_order(false);      // ¡DolRecomp deja los datos big-endian!
    g_listo = true;
    return true;
}

void gx_fifo_write(const uint8_t* data, uint32_t size) {
    const auto* b = reinterpret_cast<const std::byte*>(data);
    g_ring.insert(g_ring.end(), b, b + size);
}

void gx_frame(bool present, uint32_t xfb) {
    if (!g_listo) return;
    gx::pump_messages();
    // Consumir los receipts ya completados (cada token exactamente una vez, sin bloquear).
    while (!g_tokens.empty() && gx::wait_for_frame_pe_completion_for(g_tokens.front(), 0))
        g_tokens.pop_front();
    if (!g_ring.empty()) {
        auto token = gx::render_frame(g_ring.data(), g_ring.size(), &g_memory, &g_services, present, xfb);
        g_ring.clear();
        g_tokens.push_back(token);
    } else if (present) {
        gx::present_cached_xfb(xfb, true);   // retrace sin dibujo nuevo: presenta el XFB cacheado
    }
}

uint32_t gx_pi_cause_exchange(uint32_t clear_mask) {
    return g_pi_cause.fetch_and(~clear_mask, std::memory_order_acq_rel);
}
void     gx_pi_raise(uint32_t bits) { g_pi_cause.fetch_or(bits, std::memory_order_release); }
uint16_t gx_pe_token_value()        { return g_pe_token.load(std::memory_order_acquire); }
bool     gx_quit_requested()        { return gx::quit_requested(); }

void gx_shutdown() {
    if (!g_listo) return;
    gx::wait_for_render_idle();
    gx::shutdown();
    g_listo = false;
}

}  // namespace cactus
