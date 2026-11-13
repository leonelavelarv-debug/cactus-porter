cmake_minimum_required(VERSION 3.22)
project(cactus_wii_host C CXX)

# ---------------- rutas requeridas ----------------
set(CACTUS_GALAXY_ROOT "" CACHE PATH "Checkout de cactus-galaxy (el fork)")
set(DOLRECOMP_DIR      "" CACHE PATH "Checkout de DolRecomp")
set(GENERATED_DIR      "" CACHE PATH "C generado por DolRecomp (<ID>_generated)")
set(CACTUS_SDL2_PREFIX "" CACHE PATH "SDL2 aarch64 (include/SDL2, lib/libSDL2-2.0.so)")
set(CACTUS_SLUG        "wii" CACHE STRING "Nombre del ejecutable del port")
foreach(_req CACTUS_GALAXY_ROOT DOLRECOMP_DIR GENERATED_DIR CACTUS_SDL2_PREFIX)
    if("${${_req}}" STREQUAL "")
        message(FATAL_ERROR "Falta -D${_req}=...")
    endif()
endforeach()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------- la librería GX GLES del fork ----------------
# El host recompilado construye las dependencias que enlaza.
add_subdirectory("${CACTUS_GALAXY_ROOT}/platform/knulli/game"
                 "${CMAKE_BINARY_DIR}/galaxy" EXCLUDE_FROM_ALL)

set(CACTUS_GX_LIB "" CACHE STRING "Target GX GLES del fork (vacío: autodetectar)")
if(NOT CACTUS_GX_LIB)
    foreach(_cand galaxy_gx_gles nebula_gx_gles gx_gles galaxy_gx)
        if(TARGET "${_cand}")
            set(CACTUS_GX_LIB "${_cand}")
            break()
        endif()
    endforeach()
endif()
if(NOT TARGET "${CACTUS_GX_LIB}")
    message(FATAL_ERROR
        "No encontré la librería GX GLES del fork. Confírmala con:\n"
        "  grep -nE 'add_library|add_executable' ${CACTUS_GALAXY_ROOT}/platform/knulli/game/CMakeLists.txt\n"
        "y pásala con -DCACTUS_GX_LIB=<nombre>.")
endif()
message(STATUS "cactus-porter: librería GX del fork = ${CACTUS_GX_LIB}")

# ---------------- fuentes del port ----------------
get_filename_component(game_dir "${GENERATED_DIR}" NAME)
string(REGEX REPLACE "_generated$" "" game_id "${game_dir}")
set(manifest "${GENERATED_DIR}/${game_id}.c")
if(NOT EXISTS "${manifest}" OR NOT EXISTS "${GENERATED_DIR}/${game_id}.h")
    message(FATAL_ERROR "Falta el C generado para ${game_id}. Ejecuta recomp.")
endif()
# El manifiesto identifica los chunks vigentes, incluso después de regenerar.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
file(STRINGS "${manifest}" chunks REGEX "^// [a-zA-Z0-9_]+/[^ ]+\\.c$")
if(NOT chunks)
    message(FATAL_ERROR "El manifiesto no contiene chunks de DolRecomp")
endif()
set(GEN_SOURCES "${manifest}")
foreach(chunk IN LISTS chunks)
    string(SUBSTRING "${chunk}" 3 -1 chunk_path)
    list(APPEND GEN_SOURCES "${GENERATED_DIR}/${chunk_path}")
endforeach()

# El renderer exporta una cabecera de compatibilidad de C++ por -include.
# Las unidades C de DolRecomp se compilan con sus propias opciones.
# Módulos REL recompilados (cactus-porter recomp los deja en cactus_rels.cmake); sin REL, tabla vacía.
if(EXISTS "${GENERATED_DIR}/cactus_rels.cmake")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GENERATED_DIR}/cactus_rels.cmake")
    include("${GENERATED_DIR}/cactus_rels.cmake")
else()
    set(CACTUS_REL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/rel_vacio.c")
endif()

add_library(cactus_game_cpu OBJECT
    "${DOLRECOMP_DIR}/src/cpu/cpu.c"
    ${GEN_SOURCES}
    ${CACTUS_REL_SOURCES})
target_include_directories(cactus_game_cpu PRIVATE "${GENERATED_DIR}" "${DOLRECOMP_DIR}/src")
target_compile_options(cactus_game_cpu PRIVATE -O2 -Wall)

add_executable(${CACTUS_SLUG}-host
    wii_host.cpp wii_boot.cpp ai_dma.cpp dsp_hle.cpp ax_hle.cpp zelda_hle.cpp wii_rel.cpp wiimote.cpp dev_bt.cpp sysconf.cpp ios.cpp dev_di.cpp dev_fs.cpp dev_misc.cpp wii_dvd.cpp gx_bridge.cpp juego_glue.cpp
    "${CACTUS_GALAXY_ROOT}/platform/knulli/game/host_window_knulli.cpp"
    "${CACTUS_GALAXY_ROOT}/platform/sdl/display_clock.cpp"
    $<TARGET_OBJECTS:cactus_game_cpu>)

target_compile_definitions(${CACTUS_SLUG}-host PRIVATE CACTUS_GENERATED_HEADER="${game_id}.h")

# ---------------- Tracy (pruebas de rendimiento, perfil.h) ----------------
# cactus-porter build --tracy: cliente de Tracy en modo bajo demanda (solo mide con el visor conectado) y con
# muestreo de pila, que en KNULLI (root) funciona. El visor del PC debe ser de la misma versión (TRACY_DIR).
option(CACTUS_TRACY "Enlazar el cliente de Tracy para medir el rendimiento" OFF)
set(TRACY_DIR "" CACHE PATH "Checkout de Tracy (misma versión que el visor del PC)")
if(CACTUS_TRACY)
    if(NOT EXISTS "${TRACY_DIR}/public/TracyClient.cpp")
        message(FATAL_ERROR "CACTUS_TRACY=ON necesita -DTRACY_DIR=<checkout de Tracy> (cactus-porter fetch lo trae)")
    endif()
    target_sources(${CACTUS_SLUG}-host PRIVATE "${TRACY_DIR}/public/TracyClient.cpp")
    target_include_directories(${CACTUS_SLUG}-host PRIVATE "${TRACY_DIR}/public")
    target_compile_definitions(${CACTUS_SLUG}-host PRIVATE TRACY_ENABLE TRACY_ON_DEMAND TRACY_NO_FRAME_IMAGE)
    message(STATUS "cactus-porter: Tracy activado (${TRACY_DIR})")
endif()
target_include_directories(${CACTUS_SLUG}-host PRIVATE
    "${GENERATED_DIR}"
    "${DOLRECOMP_DIR}/src"
    "${CACTUS_GALAXY_ROOT}/runtime/include"
    "${CACTUS_GALAXY_ROOT}/native/include"
    "${CACTUS_GALAXY_ROOT}/platform/sdl"
    "${CACTUS_GALAXY_ROOT}/platform/knulli"
    "${CACTUS_SDL2_PREFIX}/include/SDL2")

find_library(CACTUS_EGL    NAMES EGL)
find_library(CACTUS_GLESv2 NAMES GLESv2)
set(CACTUS_GL_LIBS "")
if(CACTUS_EGL)
    list(APPEND CACTUS_GL_LIBS "${CACTUS_EGL}")
endif()
if(CACTUS_GLESv2)
    list(APPEND CACTUS_GL_LIBS "${CACTUS_GLESv2}")
endif()

target_link_libraries(${CACTUS_SLUG}-host PRIVATE
    ${CACTUS_GX_LIB}
    "${CACTUS_SDL2_PREFIX}/lib/libSDL2-2.0.so"
    ${CACTUS_GL_LIBS}
    pthread m ${CMAKE_DL_LIBS})

target_compile_options(${CACTUS_SLUG}-host PRIVATE -O2 -Wall)
target_link_options(${CACTUS_SLUG}-host PRIVATE
    -static-libstdc++ -static-libgcc -Wl,--gc-sections)
set_target_properties(${CACTUS_SLUG}-host PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
