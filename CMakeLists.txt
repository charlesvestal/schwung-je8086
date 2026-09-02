cmake_minimum_required(VERSION 3.15)
project(schwung-jp8000 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Build position-independent code (required for shared library)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# No STB_GNU_UNIQUE symbols.
#
# glibc marks any DSO that DEFINES a unique symbol as NODELETE at relocation
# time, so dlclose() on it is a no-op: the chain host unloads the slot, dlopen()
# hands back the same mapping, and the process keeps running the OLD dsp.so
# until MoveOriginal is restarted. Gearmulator's templates gave us 254 of them,
# which cost a whole debugging session -- ui_hierarchy kept answering with a
# build two commits behind the file on disk, at an identical load base.
#
# Set before the add_subdirectory() calls: the unique symbols come from jeLib /
# dsp56300 / synthLib linked into dsp.so, not from our own translation unit.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique>)
endif()

# ---- Gearmulator dependencies ----
if(NOT GEARMULATOR_SOURCE)
    set(GEARMULATOR_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/libs/gearmulator/source)
endif()

# DSP56300 emulator (includes asmjit JIT compiler — needed by ESP JIT)
set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${GEARMULATOR_SOURCE}/dsp56300/source ${CMAKE_BINARY_DIR}/dsp56300)

# Base library
add_subdirectory(${GEARMULATOR_SOURCE}/baseLib ${CMAKE_BINARY_DIR}/baseLib)

# Synth library (core device abstraction)
add_subdirectory(${GEARMULATOR_SOURCE}/synthLib ${CMAKE_BINARY_DIR}/synthLib)

# Libresample (sample rate conversion — transitive dep of synthLib)
add_subdirectory(${GEARMULATOR_SOURCE}/libresample ${CMAKE_BINARY_DIR}/libresample)

# Hardware library (needed by jeLib for H8S emulation)
add_subdirectory(${GEARMULATOR_SOURCE}/hardwareLib ${CMAKE_BINARY_DIR}/hardwareLib)

# MC68k emulator (needed by hardwareLib)
add_subdirectory(${GEARMULATOR_SOURCE}/mc68k ${CMAKE_BINARY_DIR}/mc68k)

# Ronaldo / JE-8086 (JP-8000) dependencies
add_subdirectory(${GEARMULATOR_SOURCE}/ronaldo/common ${CMAKE_BINARY_DIR}/rLib)
add_subdirectory(${GEARMULATOR_SOURCE}/ronaldo/esp ${CMAKE_BINARY_DIR}/esp)
add_subdirectory(${GEARMULATOR_SOURCE}/ronaldo/je8086/jeLib ${CMAKE_BINARY_DIR}/jeLib)

# ---- Move plugin ----

add_library(jp8000-move-plugin SHARED
    src/dsp/jp8000_plugin.cpp
)

target_include_directories(jp8000-move-plugin PRIVATE
    ${GEARMULATOR_SOURCE}
)

target_link_libraries(jp8000-move-plugin PRIVATE
    jeLib
    synthLib
    baseLib
)

# Statically link libstdc++ - the Move device may have an older GLIBCXX
#
# --exclude-libs,ALL hides every symbol that came out of a static archive, which
# is the other half of -fno-gnu-unique above: libstdc++.a's locale facet `::id`
# statics are STB_GNU_UNIQUE, and a unique symbol ANYWHERE in .dynsym is enough
# for glibc to pin the whole DSO NODELETE, whatever our own code compiles to.
# Hidden symbols never reach .dynsym, so they cannot be looked up as unique.
# move_plugin_init_v2 lives in our own object file, not an archive, so it is
# still exported.
if(NOT APPLE)
    target_link_options(jp8000-move-plugin PRIVATE
        -static-libstdc++ -static-libgcc
        -Wl,--exclude-libs,ALL)
endif()

# Output as dsp.so
set_target_properties(jp8000-move-plugin PROPERTIES
    OUTPUT_NAME "dsp"
    PREFIX ""
    SUFFIX ".so"
)

# ---- Benchmarks ----

add_executable(bench_je src/benchmark/dsp_bench.cpp)
target_include_directories(bench_je PRIVATE ${GEARMULATOR_SOURCE})
target_compile_definitions(bench_je PRIVATE BENCH_JE=1)
target_link_libraries(bench_je PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(bench_je PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(bench_je_fork src/benchmark/dsp_bench.cpp)
target_include_directories(bench_je_fork PRIVATE ${GEARMULATOR_SOURCE})
target_compile_definitions(bench_je_fork PRIVATE BENCH_JE_FORK=1)
target_link_libraries(bench_je_fork PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(bench_je_fork PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- Snapshot tool ----

add_executable(je_snapshot src/tools/je_snapshot.cpp)
target_include_directories(je_snapshot PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(je_snapshot PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(je_snapshot PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- whine_compare diagnostic harness ----

add_executable(whine_compare src/tools/whine_compare.cpp)
target_include_directories(whine_compare PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(whine_compare PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(whine_compare PRIVATE -static-libstdc++ -static-libgcc)
endif()

add_executable(audio_probe src/tools/audio_probe.cpp)
target_include_directories(audio_probe PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(audio_probe PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(audio_probe PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- spike_protocol: H8S→ASIC protocol characterization tool ----
add_executable(spike_protocol src/tools/spike_protocol.cpp)
target_include_directories(spike_protocol PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(spike_protocol PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(spike_protocol PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- jp8000_render: scripted MIDI → WAV for A/B testing ----
add_executable(jp8000_render src/tools/jp8000_render.cpp)
target_include_directories(jp8000_render PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(jp8000_render PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(jp8000_render PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- plugin_drive: load dsp.so through the plugin API and render ----
add_executable(plugin_drive src/tools/plugin_drive.cpp)
target_link_libraries(plugin_drive PRIVATE ${CMAKE_DL_LIBS})

# ---- nusaw_render: drive schwung-nusaw with the same script format ----
set(NUSAW_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../schwung-nusaw/src/dsp")
if(EXISTS "${NUSAW_SRC}/nusaw_engine.cpp")
    add_executable(nusaw_render
        src/tools/nusaw_render.cpp
        ${NUSAW_SRC}/nusaw_engine.cpp)
    target_include_directories(nusaw_render PRIVATE ${NUSAW_SRC})
    if(NOT APPLE)
        target_link_options(nusaw_render PRIVATE -static-libstdc++ -static-libgcc)
    endif()
else()
    message(STATUS "schwung-nusaw not found at ${NUSAW_SRC}; skipping nusaw_render")
endif()

# ---- snap_bisect: isolate snapshot-boot mute (serial mode, raw Je8086) ----
add_executable(snap_bisect src/tools/snap_bisect.cpp)
target_include_directories(snap_bisect PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(snap_bisect PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(snap_bisect PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- plugin_host_test: minimal v2 host that dlopens dsp.so for autonomous testing ----
add_executable(plugin_host_test src/tools/plugin_host_test.cpp)
target_link_libraries(plugin_host_test PRIVATE dl pthread)

add_executable(plugin_params src/tools/plugin_params.cpp)
target_link_libraries(plugin_params PRIVATE dl pthread)
if(NOT APPLE)
    target_link_options(plugin_host_test PRIVATE -static-libstdc++ -static-libgcc)
endif()

# ---- jeTestConsole (built from our source) ----
add_executable(je_test_console
    ${GEARMULATOR_SOURCE}/ronaldo/je8086/jeTestConsole/jeTestConsole.cpp)
target_include_directories(je_test_console PRIVATE ${GEARMULATOR_SOURCE})
target_link_libraries(je_test_console PRIVATE jeLib synthLib baseLib)
if(NOT APPLE)
    target_link_options(je_test_console PRIVATE -static-libstdc++ -static-libgcc)
endif()
