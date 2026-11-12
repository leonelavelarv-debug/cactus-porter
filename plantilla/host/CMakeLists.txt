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
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${GENERATED_DIR}/*.c")
if(NOT GEN_SOURCES)
    message(FATAL_ERROR "Sin código generado en ${GENERATED_DIR} (ejecuta 'recomp' antes)")
endif()

# El renderer exporta una cabecera de compatibilidad de C++ por -include.
# Las unidades C de DolRecomp se compilan con sus propias opciones.
add_library(cactus_game_cpu OBJECT
    "${DOLRECOMP_DIR}/src/cpu/cpu.c"
    ${GEN_SOURCES})
target_include_directories(cactus_game_cpu PRIVATE "${GENERATED_DIR}" "${DOLRECOMP_DIR}/src")
target_compile_options(cactus_game_cpu PRIVATE -O2 -Wall)

add_executable(${CACTUS_SLUG}-host
    wii_host.cpp wii_dvd.cpp gx_bridge.cpp juego_glue.cpp
    "${CACTUS_GALAXY_ROOT}/platform/knulli/game/host_window_knulli.cpp"
    "${CACTUS_GALAXY_ROOT}/platform/sdl/display_clock.cpp"
    $<TARGET_OBJECTS:cactus_game_cpu>)

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
