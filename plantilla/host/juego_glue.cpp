// juego_glue.cpp — integración de la CPU y los servicios específicos del juego.
#include "wii_gx.h"
namespace cactus {
void game_init(CPUState* cpu, uint32_t entry) {
  cpu->pc = entry;
  // TODO: llamar al entry point del módulo generado (ver generated.h / ModernGekko-Template)
}
void game_frame(CPUState* cpu) {
  // TODO: ejecutar un slice del código recompilado (dispatch de funciones generadas)
}
}
