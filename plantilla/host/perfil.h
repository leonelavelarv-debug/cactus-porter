// perfil.h — instrumentación con Tracy (https://github.com/wolfpld/tracy) para medir los ports en la consola.
// Con -DCACTUS_TRACY=ON el host enlaza el cliente de Tracy (TRACY_ENABLE, modo bajo demanda): escucha en el
// puerto 8086 y solo guarda datos mientras el visor o tracy-capture del PC estén conectados. Sin esa opción
// todas las macros desaparecen y el port queda igual que siempre.
//
//   CACTUS_ZONA("nombre")        zona con nombre fijo hasta el final del bloque
//   CACTUS_CUADRO()              fin de cuadro (un retrace del juego)
//   CACTUS_GRAFICA("nombre", v)  punto de una gráfica (número)
//   CACTUS_MENSAJE(fmt, ...)     mensaje en la línea de tiempo, estilo printf
//   CACTUS_HILO("nombre")        nombre del hilo actual
#pragma once

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#include <cstdio>
#include <cstring>

#define CACTUS_ZONA(nombre) ZoneScopedN(nombre)
#define CACTUS_CUADRO() FrameMark
#define CACTUS_GRAFICA(nombre, valor) TracyPlot(nombre, static_cast<double>(valor))
#define CACTUS_GRAFICA_PORCENTAJE(nombre) TracyPlotConfig(nombre, tracy::PlotFormatType::Percentage, false, true, 0)
#define CACTUS_HILO(nombre) tracy::SetThreadName(nombre)
#define CACTUS_MENSAJE(...)                                              \
    do {                                                                 \
        char cactus_msg_[256];                                           \
        const int cactus_n_ = std::snprintf(cactus_msg_, sizeof cactus_msg_, __VA_ARGS__); \
        if (cactus_n_ > 0)                                               \
            TracyMessage(cactus_msg_, std::strlen(cactus_msg_));         \
    } while (0)
#define CACTUS_PERFIL_ACTIVO 1
#else
#define CACTUS_ZONA(nombre) ((void)0)
#define CACTUS_CUADRO() ((void)0)
#define CACTUS_GRAFICA(nombre, valor) ((void)(valor))
#define CACTUS_GRAFICA_PORCENTAJE(nombre) ((void)0)
#define CACTUS_HILO(nombre) ((void)0)
#define CACTUS_MENSAJE(...) ((void)0)
#define CACTUS_PERFIL_ACTIVO 0
#endif
