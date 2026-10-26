// ablation_guards.hpp — component-removal ablation macros.
//
// Each ABL_NO_* macro, when defined, short-circuits ONE functional component's
// WORK while leaving every pipeline barrier (acquire/commit/wait/release) intact.
// The kernel still runs to completion; only timing is measured (output is garbage).
//
//   ABL_NO_TMA_LOAD      — skip the copy(tma_load_*) issues in Load::load()
//   ABL_NO_QK_MMA        — skip the gemm_bs QK UMMA in do_qk (keep load_sfb UTCCP)
//   ABL_NO_TMEM_LOAD_S   — skip the S TMEM->reg copy in softmax_step
//   ABL_NO_SOFTMAX       — skip row_max/exp2/P-quant arithmetic (constant P)
//   ABL_NO_TMEM_STORE_P  — skip the P reg->TMEM copy + fence in softmax_step
//   ABL_NO_PV_MMA        — skip the issue_pv_mxfp8_blockscaled_ts in do_pv
//   ABL_NO_CORRECTION    — skip the correction_rescale per-tile O RMW
//   ABL_NO_EPILOGUE_STORE— skip the TMA store in Epilogue::store
//
// Helper: ABL_DO(call) expands to `call` unless ABL_NO_TMA_LOAD is set.
#pragma once

// C1 — TMA load skip. When set, the load warp still does producer_acquire/
// producer_get_barrier/++state/producer_commit machinery, but the actual
// copy(tma...) issues are elided (TMA hardware idle).
#ifdef ABL_NO_TMA_LOAD
  #define ABL_TMA_COPY(...) do {} while (0)
#else
  #define ABL_TMA_COPY(...) do { __VA_ARGS__; } while (0)
#endif
