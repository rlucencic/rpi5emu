# rpi5emu: the shared runtime for the rpi5* emulators (Raspberry Pi 5,
# 64-bit Linux, SDL 3). Not built on its own: a machine project includes it
#
#   add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../rpi5emu rpi5emu)
#   add_executable(rpi5zx ...)
#   rpi5emu_add_cores(rpi5zx z80 upd765 diskspace)
#   target_link_libraries(rpi5zx PRIVATE rpi5emu)
#
# The CPU cores and shared chips are esp32emu's (../esp32emu/components),
# compiled into each machine with its own defines (CONFIG_Z80_CONTENTION,
# ...): one copy of each core for the boards and the Pi.
cmake_minimum_required(VERSION 3.20)
project(rpi5emu C)

set(ESP32EMU_DIR "${CMAKE_CURRENT_LIST_DIR}/../esp32emu" CACHE PATH
    "esp32emu checkout: the CPU cores and shared chips")
if(NOT EXISTS "${ESP32EMU_DIR}/components/emudbg/emudbg.c")
    message(FATAL_ERROR "No esp32emu at ${ESP32EMU_DIR}: clone github.com/rlucencic/esp32emu beside rpi5emu "
                        "(or -DESP32EMU_DIR=...)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3>=3.2)
# The setup screen's text: SDL3_ttf (libsdl3-ttf-dev) if it's there, else SDL's 8 x 8 font
pkg_check_modules(SDL3_TTF IMPORTED_TARGET sdl3-ttf)
find_package(PNG REQUIRED)
find_package(Threads REQUIRED)

# The web page: the boards' own (esp32emu's web/index.html), built in
set(WEB_PAGE "${ESP32EMU_DIR}/components/emu/web/index.html")
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/web_page.c"
    COMMAND ${CMAKE_COMMAND} -DIN=${WEB_PAGE} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/web_page.c
            -DNAME=rpi5emu_index_html -P ${CMAKE_CURRENT_LIST_DIR}/cmake/embed.cmake
    DEPENDS "${WEB_PAGE}" "${CMAKE_CURRENT_LIST_DIR}/cmake/embed.cmake"
    COMMENT "The web page from esp32emu")

# The debugger's protocol and its server (esp32emu's emudbg): every machine's
# cores call into it
add_library(rpi5emu STATIC
    src/emu.c
    src/ui.c
    src/web.c
    src/console.c
    src/disks.c
    src/settings.c
    src/json.c
    src/png.c
    "${CMAKE_CURRENT_BINARY_DIR}/web_page.c"
    ${ESP32EMU_DIR}/components/emudbg/emudbg.c
    ${ESP32EMU_DIR}/components/emudbg/emudbg_posix.c)
target_include_directories(rpi5emu
    PUBLIC include ${ESP32EMU_DIR}/components/emudbg/include
    PRIVATE src)
target_link_libraries(rpi5emu PUBLIC PkgConfig::SDL3 PNG::PNG Threads::Threads m)
if(SDL3_TTF_FOUND)
    target_link_libraries(rpi5emu PRIVATE PkgConfig::SDL3_TTF)
    target_compile_definitions(rpi5emu PRIVATE RPI5EMU_TTF=1)
else()
    message(STATUS "No SDL3_ttf (libsdl3-ttf-dev): the setup screen in SDL's 8 x 8 font")
endif()
target_compile_options(rpi5emu PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Optimised with debug information unless asked otherwise (gdb on the Pi,
# or gdb-multiarch through gdbserver)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# rpi5emu_add_cores(<target> <core> ...): esp32emu's components into the
# machine's target (its own defines apply: a core's header may depend on
# them). Cores: z80 m6502 m6809 w65816 musashi arm2 x86 upd765 diskspace
# emu8950
function(rpi5emu_add_cores target)
    foreach(core ${ARGN})
        set(dir "${ESP32EMU_DIR}/components/${core}")
        if(NOT EXISTS "${dir}")
            message(FATAL_ERROR "esp32emu has no component ${core}")
        endif()
        file(GLOB sources "${dir}/*.c")
        # the board's own files
        list(FILTER sources EXCLUDE REGEX "_freertos\\.c$|_alloc\\.c$")
        target_sources(${target} PRIVATE ${sources})
        target_include_directories(${target} PRIVATE "${dir}/include")
    endforeach()
endfunction()
