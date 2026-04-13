cmake_minimum_required(VERSION 3.15)
project(schwung-jp8000 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Build position-independent code (required for shared library)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---- Gearmulator dependencies ----
set(GEARMULATOR_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/libs/gearmulator/source)

# DSP56300 emulator (includes asmjit JIT compiler — needed by ESP JIT)
set(ASMJIT_STATIC TRUE)
set(ASMJIT_NO_INSTALL TRUE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${GEARMULATOR_SOURCE}/dsp56300/source ${CMAKE_BINARY_DIR}/dsp56300)

# Base library
add_subdirectory(${GEARMULATOR_SOURCE}/baseLib ${CMAKE_BINARY_DIR}/baseLib)

# Synth library (core device abstraction)
add_subdirectory(${GEARMULATOR_SOURCE}/synthLib ${CMAKE_BINARY_DIR}/synthLib)

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
target_link_options(jp8000-move-plugin PRIVATE -static-libstdc++ -static-libgcc)

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
