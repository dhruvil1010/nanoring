# ---------------------------------------------------------------------------
# nanoring
#
# Every benchmark is built twice from the same source: once with the cache-line
# padding in spsc_ring.hpp enabled (`_padded`) and once with it compiled out via
# -DSPSC_DISABLE_PADDING (`_unpadded`). That pair is the whole experiment - the
# two binaries differ by nothing except the offset of two integers, so any delta
# you measure between them is attributable to false sharing and nothing else.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(nanoring LANGUAGES CXX)

# RelWithDebInfo is "-O2 -g -DNDEBUG" for GCC/Clang, which is exactly what we
# want: the spec asks for -O2 (not -O3, whose extra unrolling makes the assembly
# harder to read), and -g keeps the symbols that `perf report` needs to attribute
# cache misses to source lines. A benchmark you cannot profile is half useless.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Off => -std=c++20 rather than -std=gnu++20, so the "compiles clean under
# -Wpedantic" claim is actually about ISO C++ and not about GNU C++.
set(CMAKE_CXX_EXTENSIONS OFF)

option(NANORING_NATIVE   "Compile with -march=native (results become machine-specific)" OFF)
option(NANORING_SANITIZE "Compile with ThreadSanitizer (do NOT use for timing runs)"    OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Shared settings, carried by an INTERFACE target so every executable picks up
# the same include path, warnings and link libraries without repetition.
# ---------------------------------------------------------------------------
add_library(nanoring_core INTERFACE)
target_include_directories(nanoring_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nanoring_core INTERFACE Threads::Threads)

if(MSVC)
  set(NANORING_FLAGS /W4 /permissive-)
else()
  # -O2 is repeated here on purpose: target options are emitted after
  # CMAKE_CXX_FLAGS_<CONFIG>, so this pins the level even if someone configures
  # with -DCMAKE_BUILD_TYPE=Release (which would otherwise hand us -O3).
  set(NANORING_FLAGS -Wall -Wextra -Wpedantic -O2)
endif()

if(NANORING_NATIVE AND NOT MSVC)
  # Opt-in only. -march=native lets the compiler emit instructions your CPU has
  # and mine does not, so a binary built this way is not comparable across
  # machines - which is the whole reason it is not the default.
  list(APPEND NANORING_FLAGS -march=native -mtune=native)
endif()

if(NANORING_SANITIZE AND NOT MSVC)
  # ThreadSanitizer instruments every memory access and shadow-maps the heap; it
  # slows the program down by roughly an order of magnitude and rewrites the
  # timing behaviour completely. Correctness build only - never quote numbers
  # from a TSan binary.
  list(APPEND NANORING_FLAGS -fsanitize=thread -fno-omit-frame-pointer -g)
  target_link_options(nanoring_core INTERFACE -fsanitize=thread)
endif()

target_compile_options(nanoring_core INTERFACE ${NANORING_FLAGS})

# Bake the build configuration into the binaries so every result block printed
# by a benchmark carries the flags that produced it. Copy-pasting output into a
# report then cannot lose the context that makes it reproducible.
string(REPLACE ";" " " NANORING_FLAGS_STR "${NANORING_FLAGS}")
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" NANORING_CONFIG_UC)
  set(NANORING_FLAGS_STR "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${NANORING_CONFIG_UC}} ${NANORING_FLAGS_STR}")
endif()
string(STRIP "${NANORING_FLAGS_STR}" NANORING_FLAGS_STR)

target_compile_definitions(nanoring_core INTERFACE
  NANORING_BUILD_FLAGS="${NANORING_FLAGS_STR}"
  NANORING_COMPILER_ID="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
  NANORING_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# ---------------------------------------------------------------------------
# One source -> two executables. The only difference is SPSC_DISABLE_PADDING.
# ---------------------------------------------------------------------------
function(nanoring_add_padded_pair base_name source)
  foreach(variant padded unpadded)
    set(target "${base_name}_${variant}")
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE nanoring_core)
    if(variant STREQUAL "unpadded")
      target_compile_definitions(${target} PRIVATE SPSC_DISABLE_PADDING)
    endif()
  endforeach()
endfunction()

nanoring_add_padded_pair(bench_throughput bench/bench_throughput.cpp)
nanoring_add_padded_pair(bench_latency    bench/bench_latency.cpp)
# The test is built both ways too: collapsing the layout must change timing and
# nothing else, so a correctness failure in the unpadded build would mean the
# A/B knob is doing more than it claims.
nanoring_add_padded_pair(test_spsc        tests/test_spsc.cpp)

enable_testing()
add_test(NAME spsc_padded   COMMAND test_spsc_padded)
add_test(NAME spsc_unpadded COMMAND test_spsc_unpadded)

message(STATUS "nanoring:")
message(STATUS "  build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  flags         : ${NANORING_FLAGS_STR}")
message(STATUS "  -march=native : ${NANORING_NATIVE}")
message(STATUS "  ThreadSanitizer: ${NANORING_SANITIZE}")
