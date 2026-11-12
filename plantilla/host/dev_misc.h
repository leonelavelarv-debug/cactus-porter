// /dev/stm/* (eventos de encendido y reinicio) y /dev/es (identidad del título) mínimos para el SDK.
#pragma once
#include "ios.h"
#include <cstdint>

namespace ios {
// Registra /dev/stm/immediate, /dev/stm/eventhook y /dev/es en el kernel.
void register_misc_devices(Kernel& kernel, std::uint64_t title_id);
}
