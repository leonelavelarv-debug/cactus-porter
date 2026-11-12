// Despacho del backend C de DolRecomp. Cada retrace concede un presupuesto finito.
#include "wii_gx.h"
#include <cstdio>
extern "C" {
#include CACTUS_GENERATED_HEADER
}

namespace cactus {
bool game_init(CPUState* cpu, uint32_t entry) {
    if (entry != DOLRECOMP_ENTRY_POINT) {
        std::fprintf(stderr, "DOL incompatible: entrada %08x, esperada %08x\n", entry, DOLRECOMP_ENTRY_POINT);
        return false;
    }
    cpu->pc = entry;
    std::fprintf(stderr, "Inicio del DOL en %08x\n", entry);
    return true;
}

bool game_frame(CPUState* cpu, const uint32_t& pi_mask) {
    // Presupuesto de trabajo inicial, ajustable tras perfilar un juego real.
    cpu->downcount = 65536;
    do {
        if ((cpu->msr & 0x8000u) && (gx_pi_cause_exchange(0) & pi_mask)) {
            ppc_take_exception(cpu, 0, 0x500u, cpu->pc, 0);
        }
        const uint32_t pc = cpu->pc;
        const auto budget = cpu->downcount;
        if (!dolrecomp_call(cpu, pc) || (cpu->pc == pc && cpu->downcount == budget)) {
            std::fprintf(stderr, "Sin bloque recompilado para PC=%08x\n", pc);
            return false;
        }
        if (cpu->exception) {
            std::fprintf(stderr, "Excepción PPC %08x desde %08x, vector %08x\n",
                         cpu->exception, cpu->srr0, cpu->pc);
            return false;
        }
    } while (cpu->downcount > 0);
    return true;
}
}
