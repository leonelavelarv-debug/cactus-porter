// Runtime del CPU sobre el backend C de DolRecomp.
//
// El C generado delega en el host lo que no traduce:
//  - mfspr/mtspr de SPR fuera de su tabla (HID0, HID4, DEC, SPRG, BAT, WPAR, DMA, L2CR...) e icbi llegan a
//    instruction_fallback; sin él, cada uno es una excepción de programa y el juego muere en __start.
//  - Las excepciones saltan a 0x500, 0x800, 0x900... Ahí el OS de Revolution copia en tiempo de ejecución su
//    __OSExceptionVector, que no está recompilado. host_call lo ejecuta en C (mismo efecto que el stub).
//  - Los saltos a código de módulos REL (en el heap del juego o en su dirección recompilada) van a wii_rel.cpp.
// Cada retrace concede un presupuesto de ciclos de Broadway y un tope de tiempo real.
#include "wii_gx.h"
#include "wii_rel.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
#include CACTUS_GENERATED_HEADER
}

namespace {
constexpr uint32_t kMsrEE = 0x8000, kMsrFP = 0x2000, kMsrIR = 0x20, kMsrDR = 0x10, kMsrRI = 0x2;
constexpr uint32_t kRfiMask = 0x87C0FFFFu;
constexpr int64_t kCyclesPerRetrace = 729000000 / 60;  // Broadway a 729 MHz, 60 Hz
constexpr int64_t kSliceCycles = 2048;                   // latencia máxima de interrupción por despacho
constexpr uint32_t kCyclesPerTick = 12;                  // timebase = bus (243 MHz) / 4
constexpr uint32_t kBroadwayPvr = 0x00087102u;

// SPR por número.
enum : uint32_t {
    SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22, SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287,
    SPR_WPAR = 921, SPR_DMAU = 922, SPR_DMAL = 923, SPR_HID0 = 1008, SPR_HID4 = 1011, SPR_L2CR = 1017,
};

struct Runtime {
    uint32_t spr[1024] = {};
    uint64_t tick_rest = 0;
    uint32_t dec_value = 0;
    uint64_t dec_tb = 0;
    bool dec_armed = false;
    int slice_ms = 14;
    bool trace = false;
    uint64_t frames = 0;
    uint32_t logged_exceptions = 0;
    uint64_t unknown_logged = 0;
} rt;

alignas(32) uint8_t g_locked_cache[16 * 1024];

uint32_t dec_now(const CPUState* cpu) {
    return rt.dec_value - uint32_t(cpu->timebase - rt.dec_tb);
}

bool dec_pending(const CPUState* cpu) {
    return rt.dec_armed && int32_t(dec_now(cpu)) < 0;
}

void advance_timebase(CPUState* cpu, int64_t cycles) {
    if (cycles <= 0) return;
    const uint64_t total = rt.tick_rest + uint64_t(cycles);
    cpu->timebase += total / kCyclesPerTick;
    rt.tick_rest = total % kCyclesPerTick;
}

// DMA del locked cache (Gekko/Broadway UM, DMAU/DMAL).
void locked_cache_dma(CPUState* cpu, uint32_t dmau, uint32_t dmal) {
    uint32_t lines = ((dmau & 0x1Fu) << 2) | ((dmal >> 2) & 3u);
    if (!lines) lines = 128;
    const uint32_t mem = dmau & ~0x1Fu, lc = dmal & ~0x1Fu;
    const bool load = (dmal >> 4) & 1u;  // 1: memoria -> locked cache
    for (uint32_t i = 0; i < lines * 32; i += 4) {
        uint8_t* cache = cactus::locked_cache(lc + i, 4);
        if (!cache) break;
        if (load) {
            const uint32_t v = mem_read32(cpu, mem + i);
            cache[0] = uint8_t(v >> 24); cache[1] = uint8_t(v >> 16); cache[2] = uint8_t(v >> 8); cache[3] = uint8_t(v);
        } else {
            mem_write32(cpu, mem + i, (uint32_t(cache[0]) << 24) | (uint32_t(cache[1]) << 16) |
                                      (uint32_t(cache[2]) << 8) | cache[3]);
        }
    }
}

uint32_t spr_read(CPUState* cpu, uint32_t spr) {
    switch (spr) {
    case SPR_DSISR: return cpu->dsisr;
    case SPR_DAR: return cpu->dar;
    case SPR_DEC: return dec_now(cpu);
    case 268: return uint32_t(cpu->timebase);
    case 269: return uint32_t(cpu->timebase >> 32);
    case SPR_PVR: return kBroadwayPvr;
    case SPR_WPAR: return rt.spr[SPR_WPAR] & ~1u;  // BNE=0: el write-gather pipe ya se vació
    case SPR_L2CR: return rt.spr[SPR_L2CR] & ~1u;  // L2IP=0: invalidación terminada
    default: return rt.spr[spr & 1023];
    }
}

void spr_write(CPUState* cpu, uint32_t spr, uint32_t value) {
    switch (spr) {
    case SPR_DSISR: cpu->dsisr = value; return;
    case SPR_DAR: cpu->dar = value; return;
    case SPR_DEC:
        rt.dec_value = value;
        rt.dec_tb = cpu->timebase;
        rt.dec_armed = true;
        return;
    case SPR_TBL_W: cpu->timebase = (cpu->timebase & 0xFFFFFFFF00000000ull) | value; return;
    case SPR_TBU_W: cpu->timebase = (uint64_t(value) << 32) | (cpu->timebase & 0xFFFFFFFFull); return;
    case SPR_HID0: rt.spr[SPR_HID0] = value & ~0xC00u; return;  // ICFI/DCFI se limpian solos
    case SPR_DMAL:
        if (value & 2u) locked_cache_dma(cpu, rt.spr[SPR_DMAU], value);
        rt.spr[SPR_DMAL] = value & ~3u;  // T y F: transferencia terminada
        return;
    default: rt.spr[spr & 1023] = value; return;
    }
}

// Instrucciones que el backend C deja al host. ctx->pc debe quedar en la siguiente.
void fallback(CPUState* cpu, uint32_t raw, uint32_t cia) {
    const uint32_t op = raw >> 26, xo = (raw >> 1) & 0x3FFu;
    if (op == 31 && (xo == 339 || xo == 467)) {  // mfspr / mtspr
        const uint32_t spr = ((raw >> 16) & 31u) | (((raw >> 11) & 31u) << 5);
        const uint32_t reg = (raw >> 21) & 31u;
        if (xo == 339) cpu->gpr[reg] = spr_read(cpu, spr);
        else spr_write(cpu, spr, cpu->gpr[reg]);
        cpu->pc = cia + 4;
        cpu->downcount -= 1;
        return;
    }
    if (op == 31 && xo == 982) {  // icbi: el código recompilado no cambia
        cpu->pc = cia + 4;
        cpu->downcount -= 1;
        return;
    }
    if (rt.unknown_logged++ < 32)
        std::fprintf(stderr, "[cpu] instrucción sin traducir %08x en %08x\n", raw, cia);
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

uint32_t r32(CPUState* cpu, uint32_t a) { return mem_read32(cpu, a); }
void w32(CPUState* cpu, uint32_t a, uint32_t v) { mem_write32(cpu, a, v); }

// __OSExceptionVector (OS de Revolution), copiado por __OSInitExceptions a cada vector:
//   mtsprg0 r4; lwz r4,0xC0(0); stw r3/r4/r5, CR, LR, CTR, XER, SRR0, SRR1 en el contexto; estado |= EXC;
//   mfmsr r3; ori r3,r3,IR|DR; mtsrr1 r3; li r3,<número>; lwz r4,0xD4(0);
//   si SRR1[RI]: r5 = __OSExceptionTable[número] (0x80003000) si no: OSDefaultExceptionHandler; mtsrr0 r5; rfi
bool os_vector(CPUState* cpu, uint32_t vector) {
    const uint32_t base = 0x80000000u | vector;
    const uint32_t first = r32(cpu, base);
    if (first == 0x7D30FAA6u) {  // __OSSystemCallVector: mfspr r9,HID0 ... rfi (solo sincroniza)
        cpu->msr = (cpu->msr & ~kRfiMask) | (cpu->srr1 & kRfiMask);
        cpu->pc = cpu->srr0 & ~3u;
        cpu->downcount -= 8;
        return true;
    }
    if (first != 0x7C9043A6u) {  // mtsprg0 r4
        std::fprintf(stderr, "[cpu] vector %04x sin manejador del OS (%08x), desde %08x\n", vector, first, cpu->srr0);
        return false;
    }
    int32_t number = -1;
    uint32_t hi = 0, fallback_handler = 0;
    bool have_hi = false;
    for (uint32_t i = 1; i < 64; ++i) {
        const uint32_t w = r32(cpu, base + 4 * i);
        if (number < 0 && (w & 0xFFFF0000u) == 0x38600000u) number = int32_t(w & 0xFFFFu);  // li r3,N
        else if ((w & 0xFFFF0000u) == 0x3CA00000u) { hi = w & 0xFFFFu; have_hi = true; }       // lis r5,hi
        else if (have_hi && (w & 0xFFFF0000u) == 0x38A50000u)                                    // addi r5,r5,lo
            fallback_handler = (hi << 16) + uint32_t(int32_t(int16_t(w & 0xFFFFu)));
        if (w == 0x4C000064u && fallback_handler) break;  // rfi
    }
    const uint32_t context = r32(cpu, 0x800000C0u);  // OSContext actual (física)
    if (number < 0 || !context) {
        std::fprintf(stderr, "[cpu] stub del vector %04x no reconocido\n", vector);
        return false;
    }
    const uint32_t ctx = context | 0x80000000u;
    rt.spr[272] = cpu->gpr[4];  // SPRG0
    w32(cpu, ctx + 0x0C, cpu->gpr[3]);
    w32(cpu, ctx + 0x10, cpu->gpr[4]);
    w32(cpu, ctx + 0x14, cpu->gpr[5]);
    mem_write16(cpu, ctx + 0x1A2, uint16_t(mem_read16(cpu, ctx + 0x1A2) | 2u));  // OS_CONTEXT_STATE_EXC
    w32(cpu, ctx + 0x80, cpu->cr);
    w32(cpu, ctx + 0x84, cpu->lr);
    w32(cpu, ctx + 0x88, cpu->ctr);
    w32(cpu, ctx + 0x8C, cpu->xer);
    w32(cpu, ctx + 0x198, cpu->srr0);
    w32(cpu, ctx + 0x19C, cpu->srr1);
    const uint32_t saved_srr1 = cpu->srr1;
    cpu->srr1 = cpu->msr | kMsrIR | kMsrDR;
    cpu->gpr[3] = uint32_t(number);
    cpu->gpr[4] = r32(cpu, 0x800000D4u);
    const uint32_t handler = (saved_srr1 & kMsrRI) ? r32(cpu, 0x80003000u + ((uint32_t(number) & 0xFFu) << 2))
                                                   : fallback_handler;
    if (!handler) {
        std::fprintf(stderr, "[cpu] excepción %d sin manejador (SRR0 %08x)\n", number, cpu->srr0);
        return false;
    }
    cpu->gpr[5] = handler;
    cpu->srr0 = handler;
    cpu->msr = (cpu->msr & ~kRfiMask) | (cpu->srr1 & kRfiMask);
    cpu->pc = handler & ~3u;
    cpu->downcount -= 40;
    return true;
}

bool host_call(CPUState* cpu, uint32_t address) {
    if (rel::dispatch(cpu, address)) return true;  // código de un módulo REL
    const uint32_t segment = address >> 28, vector = address & 0x0FFFFFFFu;  // 0x00000500, 0x80000500, 0xC0000500
    if ((segment != 0 && segment != 8 && segment != 0xC) || vector < 0x100 || vector >= 0x3000 || (vector & 0xFF))
        return false;
    if (dolrecomp_find_original(0x80000000u | vector)) return false;  // vector recompilado (pruebas)
    return os_vector(cpu, vector);
}

const char* exception_name(uint32_t e) {
    if (e & PPC_EXC_MACHINE_CHECK) return "machine check";
    if (e & PPC_EXC_DSI) return "DSI";
    if (e & PPC_EXC_ALIGNMENT) return "alineación";
    if (e & PPC_EXC_PROGRAM) return "programa";
    if (e & PPC_EXC_SYSTEM_CALL) return "llamada al sistema";
    if (e & PPC_EXC_FP_UNAVAILABLE) return "FPU no disponible";
    return "desconocida";
}
}  // namespace

namespace cactus {

uint8_t* locked_cache(uint32_t ea, std::size_t size) {
    if (ea < 0xE0000000u || size > sizeof(g_locked_cache) || ea - 0xE0000000u > sizeof(g_locked_cache) - size)
        return nullptr;
    return g_locked_cache + (ea - 0xE0000000u);
}

bool game_init(CPUState* cpu, uint32_t entry) {
    if (entry != DOLRECOMP_ENTRY_POINT) {
        std::fprintf(stderr, "DOL incompatible: entrada %08x, esperada %08x\n", entry, DOLRECOMP_ENTRY_POINT);
        return false;
    }
    rt = Runtime{};
    if (const char* ms = std::getenv("CACTUS_CPU_MS")) rt.slice_ms = std::clamp(std::atoi(ms), 1, 1000);
    rt.trace = std::getenv("CACTUS_TRACE") != nullptr;
    rt.spr[SPR_HID0] = 0x0011C464u;  // como lo deja el IPL
    rt.spr[SPR_HID4] = 0x83900000u;
    cpu->instruction_fallback = fallback;
    cpu->host_call = host_call;
    cpu->msr = kMsrFP | kMsrIR | kMsrDR | kMsrRI;
    cpu->gpr[1] = 0x816FFFF0u;
    cpu->pc = entry;
    std::fprintf(stderr, "Inicio del DOL en %08x\n", entry);
    return true;
}

bool game_frame(CPUState* cpu, const uint32_t& pi_mask) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(rt.slice_ms);
    int64_t remaining = kCyclesPerRetrace;
    for (uint32_t n = 1; remaining > 0; ++n) {
        if (cpu->msr & kMsrEE) {
            if (gx_pi_cause_exchange(0) & pi_mask) {
                ppc_take_exception(cpu, 0, 0x500u, cpu->pc, 0);
            } else if (dec_pending(cpu)) {
                rt.dec_armed = false;  // la siguiente la arma el próximo mtdec
                ppc_take_exception(cpu, 0, 0x900u, cpu->pc, 0);
            }
        }
        const uint32_t pc = cpu->pc;
        const int64_t slice = std::min(remaining, kSliceCycles);
        cpu->downcount = slice;
        const bool ran = dolrecomp_call(cpu, pc);
        const int64_t used = slice - cpu->downcount;
        if (!ran || (cpu->pc == pc && used == 0 && !cpu->exception)) {
            std::fprintf(stderr, "Sin bloque recompilado para PC=%08x (LR %08x)\n", pc, cpu->lr);
            return false;
        }
        remaining -= std::max<int64_t>(used, 1);
        advance_timebase(cpu, std::max<int64_t>(used, 1));
        if (cpu->exception) {
            const uint32_t e = cpu->exception;
            if ((e & PPC_EXC_MACHINE_CHECK) || (rt.trace && rt.logged_exceptions < 64) ||
                (!(e & PPC_EXC_FP_UNAVAILABLE) && rt.logged_exceptions < 16)) {
                ++rt.logged_exceptions;
                std::fprintf(stderr, "[cpu] excepción %s en %08x (DAR %08x), al vector %08x\n",
                             exception_name(e), cpu->srr0, cpu->dar, cpu->pc);
            }
            // El vector ya está en ctx->pc: el siguiente despacho entra al manejador del OS.
            cpu->exception = 0;
            cpu->program_exception = 0;
        }
        if ((n & 63) == 0 && clock::now() >= deadline) break;
    }
    if (rt.trace && (++rt.frames % 60) == 0)
        std::fprintf(stderr, "[cpu] retrace %llu: PC %08x LR %08x MSR %08x TB %llu\n",
                     (unsigned long long)rt.frames, cpu->pc, cpu->lr, cpu->msr,
                     (unsigned long long)cpu->timebase);
    return true;
}

}  // namespace cactus
