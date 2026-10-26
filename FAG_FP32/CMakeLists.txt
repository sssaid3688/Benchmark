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
# Per-component limiting-study ablations (each stubs ONE component, keeps all
# pipeline/mbarrier handshakes -> no deadlock; inputs kept live to prevent DCE).
add_fag_bwd_target(fag_abl_s       BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_S)
add_fag_bwd_target(fag_abl_dp      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DP)
add_fag_bwd_target(fag_abl_dv      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DV)
add_fag_bwd_target(fag_abl_dk      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DK)
add_fag_bwd_target(fag_abl_dq      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DQ)
add_fag_bwd_target(fag_abl_quant   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_QUANT)
# Rigorous quant ablation: opaque never-taken-branch liveness pin survives ptxas
# (the empty-asm pin in FAG_ABLATE_QUANT does not; see kernel header notes).
# fag_ctrl_quantpin = paired control (pin + converter kept) so that
# (ctrl - quant_v2) isolates the converter cost with pin overhead cancelled.
add_fag_bwd_target(fag_abl_quant_v2 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_QUANT_V2)
add_fag_bwd_target(fag_ctrl_quantpin BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_CTRL_QUANTPIN)
# v3: minimal instrument (3-XOR bit-consume) - no branches/constants/bloat.
add_fag_bwd_target(fag_abl_quant_v3 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_QUANT_V3)
# Clean-combo rebuilds for the 1.6x analysis (no contaminated quant guard).
add_fag_bwd_target(fag_abl_pack_reduce   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_PACK FAG_ABLATE_REDUCE)
add_fag_bwd_target(fag_abl_pack_red_soft BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_PACK FAG_ABLATE_REDUCE FAG_ABLATE_SOFTMAX)
add_fag_bwd_target(fag_abl_pack_quantv3  BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_PACK FAG_ABLATE_QUANT_V3)
# Adversarial control: does a SECOND proxy fence add a second ~10%?
add_fag_bwd_target(fag_adv_doublefence BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ADVERSARIAL_DOUBLE_FENCE)
add_fag_bwd_target(fag_opt_fence_cta BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA)
# All-hardware-MUFU exp2 variant of the optimal: NO FAG_EX2_EMU_STRIDE, so every
# exp2 is a hardware MUFU.EX2 and the software-polynomial (packed f32x2 FMA)
# emulation disappears.  dQ TMA reduce stays FP32 (default path, untouched).
add_fag_bwd_target(fag_opt_fence_cta_hwex2 BWD_2SM FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA)
# Instruction-level limiting study on the optimal config.  Each target removes
# exactly one instruction class from the compute/reduce/corner data paths while
# keeping every pipeline handshake and the liveness of upstream loads.
add_fag_bwd_target(fag_abl_sload     BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_S_LOAD)
add_fag_bwd_target(fag_abl_dpload    BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DP_LOAD)
add_fag_bwd_target(fag_abl_pstore    BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_P_STORE)
add_fag_bwd_target(fag_abl_dsstore   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_DS_TMEM_STORE)
add_fag_bwd_target(fag_abl_scale_lse BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_SCALE_LSE)
add_fag_bwd_target(fag_abl_pack      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_PACK)
add_fag_bwd_target(fag_abl_cwait     BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_CORNER_WAIT)
add_fag_bwd_target(fag_abl_reduce    BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_REDUCE)
# Combined limiting studies: all MMAs, all non-MMA compute data work, and the
# full pipeline floor (only TMA loads + barriers + epilogue stores remain).
add_fag_bwd_target(fag_abl_mma_all BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_S FAG_ABLATE_DP FAG_ABLATE_DV FAG_ABLATE_DK FAG_ABLATE_DQ)
add_fag_bwd_target(fag_abl_compute_all BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT)
add_fag_bwd_target(fag_abl_floor BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_S FAG_ABLATE_DP FAG_ABLATE_DV FAG_ABLATE_DK FAG_ABLATE_DQ
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT FAG_ABLATE_REDUCE
  FAG_ABLATE_S_LOAD FAG_ABLATE_DP_LOAD FAG_ABLATE_P_STORE FAG_ABLATE_DS_TMEM_STORE)
# 2200-TFLOPS reachability combos: which minimal op set must be removed.
add_fag_bwd_target(fag_abl_pack_quant BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_QUANT)
add_fag_bwd_target(fag_abl_pack_quant_softmax BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_QUANT FAG_ABLATE_SOFTMAX)
# 1.6x-FP16 reachability ladder per shape.
add_fag_bwd_target(fag_abl_compute_reduce BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT FAG_ABLATE_REDUCE)
add_fag_bwd_target(fag_abl_compute_mma BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT
  FAG_ABLATE_S FAG_ABLATE_DP FAG_ABLATE_DV FAG_ABLATE_DK FAG_ABLATE_DQ)
add_fag_bwd_target(fag_abl_compute_reduce_mma BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT FAG_ABLATE_REDUCE
  FAG_ABLATE_S FAG_ABLATE_DP FAG_ABLATE_DV FAG_ABLATE_DK FAG_ABLATE_DQ)
add_fag_bwd_target(fag_abl_pq_reduce BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT
  FAG_ABLATE_PACK FAG_ABLATE_QUANT FAG_ABLATE_REDUCE)
# Corner-cost discrimination: split the exchange into STS vs bulk-copy+tx.
add_fag_bwd_target(fag_abl_bulk      BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_BULK)
add_fag_bwd_target(fag_abl_xchgstore BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_XCHG_STORE)
add_fag_bwd_target(fag_abl_fence     BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_FENCE)
add_fag_bwd_target(fag_abl_ownstore  BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_ABLATE_OWN_STORE)
# Pack-path optimization candidates (correctness preserving, optimal base).
add_fag_bwd_target(fag_opt_earlycopy BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_EARLY_COPY)
add_fag_bwd_target(fag_opt_stasync   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_STASYNC_XCHG)
add_fag_bwd_target(fag_opt_ownlate   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_OWN_LATE)

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

# fence-cta + clean removals: corrected 1.6x reachability.
add_fag_bwd_target(fag_est_cta_pack BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_ABLATE_PACK)
add_fag_bwd_target(fag_est_cta_pack_reduce BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_ABLATE_PACK FAG_ABLATE_REDUCE)

# Combined adopted optimizations (correctness preserving): fence scope + own-late stores.
add_fag_bwd_target(fag_opt_v2 BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_OPT_OWN_LATE)

# Pure 2SM configuration: BWD_2SM only (intrinsic TMEM reuse), NO exp2 MUFU split,
# NO regalloc tuning. +/- fence_cta for isolation.
add_fag_bwd_target(fag_pure_2sm     BWD_2SM)
add_fag_bwd_target(fag_pure_2sm_cta BWD_2SM FAG_OPT_FENCE_CTA)

# ============================================================================
# V2 ablation matrix on the ADOPTED optimal base (fence_cta): all targets add
# FAG_OPT_FENCE_CTA on top of BWD_2SM + EX2 split + regalloc.
# Entry format "name:FLAG+FLAG" ("+" separates flags inside one entry).
# ============================================================================
set(FAG2_BASE BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA)
set(FAG2_CFGS
  "s:FAG_ABLATE_S"
  "dp:FAG_ABLATE_DP"
  "dv:FAG_ABLATE_DV"
  "dk:FAG_ABLATE_DK"
  "dq:FAG_ABLATE_DQ"
  "mma_all:FAG_ABLATE_S+FAG_ABLATE_DP+FAG_ABLATE_DV+FAG_ABLATE_DK+FAG_ABLATE_DQ"
  "quantv3:FAG_ABLATE_QUANT_V3"
  "exp2:FAG_ABLATE_EXP2"
  "softmax:FAG_ABLATE_SOFTMAX"
  "scale_lse:FAG_ABLATE_SCALE_LSE"
  "corner:FAG_ABLATE_CORNER"
  "pack:FAG_ABLATE_PACK"
  "sload:FAG_ABLATE_S_LOAD"
  "dpload:FAG_ABLATE_DP_LOAD"
  "pstore:FAG_ABLATE_P_STORE"
  "dsstore:FAG_ABLATE_DS_TMEM_STORE"
  "cwait:FAG_ABLATE_CORNER_WAIT"
  "reduce:FAG_ABLATE_REDUCE"
  "bulk:FAG_ABLATE_BULK"
  "xchgstore:FAG_ABLATE_XCHG_STORE"
  "ownstore:FAG_ABLATE_OWN_STORE"
  "fence:FAG_ABLATE_FENCE"
  "pack_reduce:FAG_ABLATE_PACK+FAG_ABLATE_REDUCE"
  "pack_red_soft:FAG_ABLATE_PACK+FAG_ABLATE_REDUCE+FAG_ABLATE_SOFTMAX"
  "floor:FAG_ABLATE_S+FAG_ABLATE_DP+FAG_ABLATE_DV+FAG_ABLATE_DK+FAG_ABLATE_DQ+FAG_ABLATE_PACK+FAG_ABLATE_SCALE_LSE+FAG_ABLATE_SOFTMAX+FAG_ABLATE_QUANT_V3+FAG_ABLATE_REDUCE+FAG_ABLATE_S_LOAD+FAG_ABLATE_DP_LOAD+FAG_ABLATE_P_STORE+FAG_ABLATE_DS_TMEM_STORE"
)
foreach(cfg ${FAG2_CFGS})
  string(REPLACE ":" ";" parts "${cfg}")
  list(GET parts 0 cname)
  list(GET parts 1 cflags_plus)
  string(REPLACE "+" ";" cflags "${cflags_plus}")
  add_fag_bwd_target(fag2_${cname} ${FAG2_BASE} ${cflags})
endforeach()

# ----------------------------------------------------------------------------
# FA4-style (FP16-equivalent) dS corner-turn restructure + remote st.async
# exchange probe.  All on the adopted optimal base (FAG2_BASE).
#   colrun     : FAG_DS_RUN_STORES -- x32 TMEM load runs (2x32 consecutive Q
#                per thread, fragment order matched by the 8x TMEM stores);
#                STS.128 at loop-invariant precomputed bases (base^16), zero
#                per-strip swizzle addressing inside the mainloop.
#   colloop    : colrun + FAG_DS_COLUMN_SPLIT -- the full FA4 two-chunk dS
#                phase (chunked dP load / math / quantize), halving transient
#                P+dP register pressure; chunk-1 load overlaps chunk-0 math.
#   stasync_cl : FAG_DS_STASYNC_CLUSTER -- per-store remote
#                st.async.shared::cluster exchange into the peer's smem_ds
#                with mbarrier tx accounting on the peer's ds_full; removes
#                the staging STS, the proxy fence AND the cp.async.bulk in
#                one shot (only untested exchange variant; prior evidence:
#                local st.async +12%, direct remote store +10~11%).
#   colloop_st : colloop + stasync_cl combined.
# ----------------------------------------------------------------------------
add_fag_bwd_target(fag_cand_colrun     ${FAG2_BASE} FAG_DS_RUN_STORES)
add_fag_bwd_target(fag_cand_colloop    ${FAG2_BASE} FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT)
add_fag_bwd_target(fag_cand_stasync_cl ${FAG2_BASE} FAG_DS_STASYNC_CLUSTER)
add_fag_bwd_target(fag_cand_colloop_st ${FAG2_BASE} FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT FAG_DS_STASYNC_CLUSTER)
# Bit-exact variants: same restructure on the CLUSTER-scope proxy fence base
# (no FAG_OPT_FENCE_CTA) for deployments requiring run-to-run bit stability.
add_fag_bwd_target(fag_cand_colrun_exact  BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DS_RUN_STORES)
add_fag_bwd_target(fag_cand_colloop_exact BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT)

# ============================================================================
# V3 ablation matrix on the SECTION-11 OPTIMAL base (fence_cta + RUN_STORES +
# COLUMN_SPLIT = fag_cand_colloop).  Each target stubs exactly ONE component
# (timing-only, correctness intentionally broken) so the per-component exposed
# cost can be re-ranked after the corner-turn restructure removed the in-loop
# swizzle addressing.  Mirrors the FAG2 ladder naming for direct comparison.
# ============================================================================
set(FAG3_BASE BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT)
set(FAG3_CFGS
  "s:FAG_ABLATE_S"
  "dp:FAG_ABLATE_DP"
  "dv:FAG_ABLATE_DV"
  "dk:FAG_ABLATE_DK"
  "dq:FAG_ABLATE_DQ"
  "mma_all:FAG_ABLATE_S+FAG_ABLATE_DP+FAG_ABLATE_DV+FAG_ABLATE_DK+FAG_ABLATE_DQ"
  "exp2:FAG_ABLATE_EXP2"
  "softmax:FAG_ABLATE_SOFTMAX"
  "scale_lse:FAG_ABLATE_SCALE_LSE"
  "quantv3:FAG_ABLATE_QUANT_V3"
  "corner:FAG_ABLATE_CORNER"
  "pack:FAG_ABLATE_PACK"
  "sload:FAG_ABLATE_S_LOAD"
  "dpload:FAG_ABLATE_DP_LOAD"
  "pstore:FAG_ABLATE_P_STORE"
  "dsstore:FAG_ABLATE_DS_TMEM_STORE"
  "cwait:FAG_ABLATE_CORNER_WAIT"
  "reduce:FAG_ABLATE_REDUCE"
  "bulk:FAG_ABLATE_BULK"
  "xchgstore:FAG_ABLATE_XCHG_STORE"
  "ownstore:FAG_ABLATE_OWN_STORE"
  "fence:FAG_ABLATE_FENCE"
  "floor:FAG_ABLATE_S+FAG_ABLATE_DP+FAG_ABLATE_DV+FAG_ABLATE_DK+FAG_ABLATE_DQ+FAG_ABLATE_PACK+FAG_ABLATE_SCALE_LSE+FAG_ABLATE_SOFTMAX+FAG_ABLATE_QUANT_V3+FAG_ABLATE_REDUCE+FAG_ABLATE_S_LOAD+FAG_ABLATE_DP_LOAD+FAG_ABLATE_P_STORE+FAG_ABLATE_DS_TMEM_STORE"
)
foreach(cfg ${FAG3_CFGS})
  string(REPLACE ":" ";" parts "${cfg}")
  list(GET parts 0 cname)
  list(GET parts 1 cflags_plus)
  string(REPLACE "+" ";" cflags "${cflags_plus}")
  add_fag_bwd_target(fag3_${cname} ${FAG3_BASE} ${cflags})
endforeach()

# dQ-reduce discrimination: keep the TMEM->RMEM drain and SMEM staging, skip
# only the TMA reduce-add issue.  (fag3_reduce - fag3_reduce_tma) isolates the
# atomic reduce-add traffic; fag3_reduce_tma alone is the drain cost.
add_fag_bwd_target(fag3_reduce_tma ${FAG3_BASE} FAG_ABLATE_REDUCE_TMA)

# LSU competition probes: ADD extra instructions to the optimal base.
# If LSU/TMA competition is real: extra STS costs ~6%, extra LDS less, ALU ~0.
add_fag_bwd_target(fag_probe_dbl_sts   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_PROBE_DBL_STS)
add_fag_bwd_target(fag_probe_dbl_lds   BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_PROBE_DBL_LDS)
add_fag_bwd_target(fag_probe_alu_24    BWD_2SM FAG_EX2_EMU_STRIDE=4 FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_PROBE_EXTRA_ALU_24)

# Optimal variant WITHOUT the ex2 MUFU/software split (pure hardware exp2):
# FAG3_BASE minus FAG_EX2_EMU_STRIDE.  Isolates the value of the 3:1
# MUFU/FMA shunting on the post-restructure optimal base.
add_fag_bwd_target(fag3_opt_noex2 BWD_2SM FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT)

# ---------------------------------------------------------------------------
# V3 optimization candidates: dQ-reduce drain (the -9% #1) + ex2 stride rescan.
#   fag3_dq_batched : FAG_DQ_BATCHED_TMA (8 named barriers/iter -> 2, 4 TMAs
#                     back-to-back, 4 SMEM stages)
#   fag3_dq64       : FAG_DQ_STORE_TILE_64 (2x64-col stages: 4 barriers/iter)
#   fag3_dq128      : FAG_DQ_STORE_TILE_128 (1x128-col stage: 2 barriers/iter)
#   fag3_ex2_s2/s8  : EX2_EMU_STRIDE rescan {2,8} on the new optimal base
# ---------------------------------------------------------------------------
set(FAG3_NOEX_BASE BWD_2SM FAG_REGALLOC_FA4_CURRENT FAG_OPT_FENCE_CTA FAG_DS_RUN_STORES FAG_DS_COLUMN_SPLIT)
add_fag_bwd_target(fag3_dq_batched ${FAG3_BASE} FAG_DQ_BATCHED_TMA FAG_FIXED_D128_FULL)
add_fag_bwd_target(fag3_dq64      ${FAG3_BASE} FAG_DQ_STORE_TILE_64)
add_fag_bwd_target(fag3_dq128     ${FAG3_BASE} FAG_DQ_STORE_TILE_128 FAG_FIXED_D128_FULL)
add_fag_bwd_target(fag3_ex2_s2    ${FAG3_NOEX_BASE} FAG_EX2_EMU_STRIDE=2)
add_fag_bwd_target(fag3_ex2_s8    ${FAG3_NOEX_BASE} FAG_EX2_EMU_STRIDE=8)
add_fag_bwd_target(fag3_dq_deep4 ${FAG3_BASE} FAG_DQ_DEEP_SERIAL)
add_fag_bwd_target(fag3_reduce_sts ${FAG3_BASE} FAG_ABLATE_REDUCE_STS)
add_fag_bwd_target(fag3_dq_bf16 ${FAG3_BASE} FAG_DQ_BF16_STAGING)
add_fag_bwd_target(fag3_dq_bf16_deep4 ${FAG3_BASE} FAG_DQ_BF16_STAGING FAG_DQ_DEEP_SERIAL)

# V3.2: additional component probes + dQ half-split structural candidate.
add_fag_bwd_target(fag3_dq_half      ${FAG3_BASE} FAG_DQ_HALF_SPLIT)
add_fag_bwd_target(fag3_lse_load     ${FAG3_BASE} FAG_ABLATE_LSE_LOAD)
add_fag_bwd_target(fag3_red_fence    ${FAG3_BASE} FAG_ABLATE_REDUCE_FENCE)
add_fag_bwd_target(fag3_comp_bar     ${FAG3_BASE} FAG_ABLATE_COMPUTE_BAR)
add_fag_bwd_target(fag3_dq_half_dbg ${FAG3_BASE} FAG_DQ_HALF_SPLIT BWD_2SM_DEBUG)
add_fag_bwd_target(fag3_dq_half_p1 ${FAG3_BASE} FAG_DQ_HALF_SPLIT FAG_ABLATE_REDUCE_TMA)

# Skeleton + pure-MMA probe: floor (all compute/data-path stubs) but WITH all
# five MMAs kept (no ABLATE_S/DP/DV/DK/DQ).  Measures the empirical
# TMA+pipeline+MMA time; (this - floor) = marginal MMA cost on the skeleton.
add_fag_bwd_target(fag3_floor_mma ${FAG3_BASE}
  FAG_ABLATE_PACK FAG_ABLATE_SCALE_LSE FAG_ABLATE_SOFTMAX FAG_ABLATE_QUANT_V3
  FAG_ABLATE_REDUCE FAG_ABLATE_S_LOAD FAG_ABLATE_DP_LOAD
  FAG_ABLATE_P_STORE FAG_ABLATE_DS_TMEM_STORE)
