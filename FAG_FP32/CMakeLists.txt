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

# ----------------------------------------------------------------------------
# Ablation ladder (FP8 backward).  Each target isolates one optimization layer
# relative to its predecessor so the per-step performance contribution can be
# measured.  All share the same 77_blackwell_fmha_bwd.cu translation unit; the
# macros below select the active code path at compile time.
#
#   A. fag_abl_baseline      : 1-SM CUTLASS reference (no optimizations).
#   B. fag_abl_2sm_overlapoff: full 2-SM FA4 structure (cooperative MMA +
#                              dS corner-turn + TMEM reuse + fused-zero) with
#                              the dP/dQ prologue/main/tail overlap DISABLED
#                              (BWD_2SM_SERIAL_BASELINE).
#   C. fag_abl_2sm_overlapon : B + overlap ENABLED (= the final main kernel).
#   D. fag_abl_2sm_nofuse    : C with SumOdO fused dQ_acc zeroing DISABLED
#                              (FAG_ABLATION_NO_FUSED_ZERO) so a separate
#                              cudaMemsetAsync is used instead.  Isolates the
#                              kernel-fusion contribution.
# ----------------------------------------------------------------------------
add_fag_bwd_target(fag_abl_baseline)
add_fag_bwd_target(fag_abl_2sm_overlapoff BWD_2SM BWD_2SM_SERIAL_BASELINE)
add_fag_bwd_target(fag_abl_2sm_overlapon BWD_2SM)
add_fag_bwd_target(fag_abl_2sm_nofuse    BWD_2SM FAG_ABLATION_NO_FUSED_ZERO)

# ----------------------------------------------------------------------------
# Stage-2 ladder: staticQuant micro-optimizations ported on top of the full
# 2-SM FA4 kernel (overlap ON = S0 baseline).  ex2 first (software exp2 split
# across MUFU and packed-FMA pipelines), then regalloc.
#   S0   = fag_abl_2sm_overlapon (already defined above)
#   ex2  = S0 + FAG_EX2_EMU_STRIDE={1,2,4,8}  (software-ex2 fraction sweep)
#   final= best ex2 + FAG_REGALLOC_FA4_CURRENT (register rebalancing)
# ----------------------------------------------------------------------------
add_fag_bwd_target(fag_stage_ex2_s1 BWD_2SM FAG_EX2_EMU_STRIDE=1)
add_fag_bwd_target(fag_stage_ex2_s2 BWD_2SM FAG_EX2_EMU_STRIDE=2)
add_fag_bwd_target(fag_stage_ex2_s4 BWD_2SM FAG_EX2_EMU_STRIDE=4)
add_fag_bwd_target(fag_stage_ex2_s8 BWD_2SM FAG_EX2_EMU_STRIDE=8)
add_fag_bwd_target(fag_stage_regalloc BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT)

# Ablation experiments on the optimal config (ex2_s4 + regalloc).
add_fag_bwd_target(fag_abl_exp2    BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_EXP2)
add_fag_bwd_target(fag_abl_softmax BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_SOFTMAX)
add_fag_bwd_target(fag_abl_corner  BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_CORNER)
add_fag_bwd_target(fag_abl_reduce  BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_REDUCE)

# Evidence-driven fixed-shape experiments.  These retain the current best
# exp2/register allocation and alter only bitwise dS packing or FP32 dQ store
# staging granularity.
add_fag_bwd_target(fag_candidate_pack BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DS_RECAST_PACK)
add_fag_bwd_target(fag_candidate_dq64 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DQ_STORE_TILE_64)
add_fag_bwd_target(fag_candidate_dq128 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DQ_STORE_TILE_128)
add_fag_bwd_target(fag_candidate_tmem8 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_TMEM_STORE_8X)
add_fag_bwd_target(fag_candidate_tmem16 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_TMEM_STORE_16X)
add_fag_bwd_target(fag_candidate_ex2_2phase BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_EX2_TWO_PHASE)
add_fag_bwd_target(fag_candidate_ex2_hwfirst BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_EX2_HW_FIRST)
add_fag_bwd_target(fag_candidate_dq_batched BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DQ_BATCHED_TMA)
add_fag_bwd_target(fag_candidate_prefetch_dp BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_PREFETCH_DP)
add_fag_bwd_target(fag_candidate_prefetch_dp_after_p BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_PREFETCH_DP_AFTER_P)
add_fag_bwd_target(fag_candidate_reg_red144 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_RED144_COMP136)
add_fag_bwd_target(fag_candidate_reg_red148 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_RED148_COMP134)
add_fag_bwd_target(fag_candidate_reg_red152 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_RED152_COMP132)
add_fag_bwd_target(fag_candidate_reg_comp140 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_COMP140)
add_fag_bwd_target(fag_candidate_reg_p144 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_P144)
add_fag_bwd_target(fag_candidate_direct_ds BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DS_DIRECT_DSMEM)
add_fag_bwd_target(fag_candidate_direct_ds_p144 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_P144 FAG_DS_DIRECT_DSMEM)
add_fag_bwd_target(fag_diag_tmem_map BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DEBUG_TMEM_MAP)
add_fag_bwd_target(fag_candidate_p_self_owned BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_P_SELF_OWNED_TMEM)
add_fag_bwd_target(fag_candidate_p_self_owned_p144 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_P144 FAG_P_SELF_OWNED_TMEM)
add_fag_bwd_target(fag_candidate_p_wg_barriers BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_P_WG_BARRIERS)
add_fag_bwd_target(fag_candidate_split_dk BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_SPLIT_DK_DQ_READY)
add_fag_bwd_target(fag_candidate_split_dk_p144 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_P144 FAG_SPLIT_DK_DQ_READY)
add_fag_bwd_target(fag_candidate_tmem_load32 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_TMEM_LOAD_32X)
add_fag_bwd_target(fag_candidate_tmem_load32_p144 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_P144 FAG_TMEM_LOAD_32X)
add_fag_bwd_target(fag_diag_tmem_map_load32 BWD_2SM FAG_FIXED_D128_FULL FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_TMEM_LOAD_32X FAG_DEBUG_TMEM_MAP)
add_fag_bwd_target(fag_candidate_1sm FAG_FIXED_D128_FULL)

message(STATUS "FAG targets: fag_bwd_fp8_2sm, fag_bwd_fp8_1sm")

function(add_probe TARGET_NAME)
  cutlass_add_executable(${TARGET_NAME} 77_blackwell_fmha_bwd_probe.cu)
  target_link_libraries(${TARGET_NAME} PRIVATE CUTLASS cuda)
  target_include_directories(${TARGET_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../cutlass/tools/util/include")
  target_compile_definitions(${TARGET_NAME} PRIVATE FP8 CUTLASS_VERSIONS_GENERATED BWD_2SM ${ARGN})
  target_compile_options(${TARGET_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-O3> $<$<COMPILE_LANGUAGE:CUDA>:-DNDEBUG> $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fPIE>)
endfunction()
add_probe(probe_allgemm FAG_PROBE_STUB_ALLGEMM)
add_probe(probe_everything FAG_PROBE_STUB_EVERYTHING)
add_probe(probe_softmax    FAG_PROBE_STUB_SOFTMAX)
add_probe(probe_corner     FAG_PROBE_STUB_CORNER)
add_probe(probe_reduce     FAG_PROBE_STUB_REDUCE)
add_probe(probe_ev_novdo FAG_PROBE_STUB_EVERYTHING FAG_PROBE_STUB_LOADVDO)
add_probe(probe_ev_doubletma FAG_PROBE_STUB_EVERYTHING FAG_PROBE_DOUBLETMA)
add_probe(probe_ev_halftx FAG_PROBE_STUB_EVERYTHING FAG_PROBE_HALFTX)
