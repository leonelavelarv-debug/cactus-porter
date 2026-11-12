// sysconf.h — /shared2/sys/SYSCONF mínimo con un Wiimote "sincronizado" (BT.DINF), que es lo que mira WPAD al arrancar.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wiisys {

// El archivo de 0x4000 bytes (cabecera SCv0, tabla de desplazamientos, elementos y pie SCed).
std::vector<uint8_t> build();
// Lo crea en <nand>/shared2/sys/SYSCONF si no existe. Un SYSCONF ya presente se respeta (puede traer más ajustes).
bool ensure(const std::string& nand_dir);

}  // namespace wiisys
