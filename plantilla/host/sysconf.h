// sysconf.h — /shared2/sys/SYSCONF mínimo con un Wiimote "sincronizado" (BT.DINF), que es lo que mira WPAD al arrancar.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wiisys {

// Idioma del sistema (IPL.LNG) desde CACTUS_LANG (es, en, fr, de, it, nl, ja, ko, zh): 0 japonés, 1 inglés,
// 2 alemán, 3 francés, 4 español, 5 italiano, 6 neerlandés, 7 chino, 9 coreano. Sin variable: inglés.
uint8_t language_from_env();
// El archivo de 0x4000 bytes (cabecera SCv0, tabla de desplazamientos, elementos y pie SCed).
std::vector<uint8_t> build(uint8_t language = 1);
// Lo crea en <nand>/shared2/sys/SYSCONF si no existe, con el idioma de CACTUS_LANG. Un SYSCONF ya presente se
// respeta (puede traer más ajustes).
bool ensure(const std::string& nand_dir);

}  // namespace wiisys
