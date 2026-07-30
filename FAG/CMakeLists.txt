cmake_minimum_required(VERSION 3.20)

# SSH/non-login shells on the B200 do not always expose nvcc via PATH.
# Discover the installed CUDA toolkit before CUDA language detection.
if(NOT DEFINED CMAKE_CUDA_COMPILER)
  find_program(FAG_NVCC NAMES nvcc
    HINTS /usr/local/cuda-13.3/bin /usr/local/cuda/bin)
  if(FAG_NVCC)
    set(CMAKE_CUDA_COMPILER "${FAG_NVCC}" CACHE FILEPATH
      "CUDA compiler selected by FAG" FORCE)
  endif()
endif()

project(FAG LANGUAGES CXX CUDA)

# FAG is independent from CUTLASS's examples tree.  It consumes the sibling
# Benchmark/cutlass checkout as a third-party, header-oriented dependency.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../cutlass/CMakeLists.txt")
  message(FATAL_ERROR
    "FAG requires the sibling CUTLASS checkout at ../cutlass. "
    "Expected: <Benchmark>/FAG next to <Benchmark>/cutlass.")
endif()

if(NOT DEFINED CUTLASS_NVCC_ARCHS)
  set(CUTLASS_NVCC_ARCHS "100a" CACHE STRING "CUTLASS CUDA architectures")
endif()

set(CUTLASS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(CUTLASS_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUTLASS_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
option(FAG_ENABLE_LINEINFO "Embed CUDA line information for profiling" OFF)

add_subdirectory(
  "${CMAKE_CURRENT_SOURCE_DIR}/../cutlass"
  "${CMAKE_BINARY_DIR}/cutlass_build"
  EXCLUDE_FROM_ALL)

set(FAG_CUDA_SOURCE_FLAGS "--use_fast_math -ftemplate-backtrace-limit=0")
if(FAG_ENABLE_LINEINFO)
  string(APPEND FAG_CUDA_SOURCE_FLAGS " -lineinfo")
endif()
set_property(
  SOURCE 77_blackwell_fmha_bwd.cu
  PROPERTY COMPILE_FLAGS "${FAG_CUDA_SOURCE_FLAGS}")

function(add_fag_bwd_target TARGET_NAME)
  cutlass_add_executable(${TARGET_NAME} 77_blackwell_fmha_bwd.cu)

  target_link_libraries(${TARGET_NAME} PRIVATE
    CUTLASS
    cuda)

  target_include_directories(${TARGET_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    # CUTLASS tools are excluded from the standalone build, but the benchmark
    # harness still uses its header-only command-line and distribution helpers.
    "${CMAKE_CURRENT_SOURCE_DIR}/../cutlass/tools/util/include")

  target_compile_definitions(${TARGET_NAME} PRIVATE
    FP8
    CUTLASS_VERSIONS_GENERATED
    ${ARGN})
  target_compile_options(${TARGET_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-O3>
    $<$<COMPILE_LANGUAGE:CUDA>:-DNDEBUG>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fPIE>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-v>)
endfunction()

# Primary Blackwell FP8 backward kernel: two cooperative CTAs spanning two SMs.
add_fag_bwd_target(fag_bwd_fp8_2sm BWD_2SM)

# Kept as a standalone correctness/reference baseline for direct comparisons.
add_fag_bwd_target(fag_bwd_fp8_1sm)

message(STATUS "FAG targets: fag_bwd_fp8_2sm, fag_bwd_fp8_1sm")
