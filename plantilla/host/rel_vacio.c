// Tabla de REL vacía: el juego no trae módulos REL (o no se recompilaron). Ver wii_rel.h.
#include <stdint.h>
typedef struct CPUState CPUState;
typedef void (*CactusRelFunction)(CPUState*);
typedef struct { uint32_t id, base; const char* disc_path; CactusRelFunction (*find)(uint32_t); } CactusRelModule;
const CactusRelModule cactus_rel_modules[] = {{0u, 0u, 0, 0}};
const unsigned cactus_rel_module_count = 0;
