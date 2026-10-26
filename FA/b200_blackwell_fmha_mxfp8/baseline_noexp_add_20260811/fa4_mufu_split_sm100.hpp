/***************************************************************************************************
 * FlashAttention-4 style partial EX2 emulation for Blackwell.
 *
 * The policy is entirely compile-time: selected float2 pairs use a packed
 * Cody-Waite range reduction plus a degree-3 polynomial, while all other pairs
 * issue the native MUFU.EX2 instruction.  There is no runtime branch, modulo,
 * synchronization, or memory traffic in the dispatcher.
 **************************************************************************************************/
#pragma once

namespace cutlass::fmha::collective::fa4_mufu_split {

#ifndef MXFP8_FA4_MUFU_PERIOD
#define MXFP8_FA4_MUFU_PERIOD 5
#endif

#ifndef MXFP8_FA4_MUFU_SLOT
#define MXFP8_FA4_MUFU_SLOT (MXFP8_FA4_MUFU_PERIOD - 1)
#endif

#ifndef MXFP8_FA4_MUFU_BEGIN_PAIR
#define MXFP8_FA4_MUFU_BEGIN_PAIR 0
#endif

#ifndef MXFP8_FA4_MUFU_END_PAIR
#define MXFP8_FA4_MUFU_END_PAIR 32
#endif

static_assert(MXFP8_FA4_MUFU_PERIOD > 1, "FA4 MUFU split period must exceed one pair");
static_assert(MXFP8_FA4_MUFU_SLOT >= 0 && MXFP8_FA4_MUFU_SLOT < MXFP8_FA4_MUFU_PERIOD,
              "FA4 MUFU split slot must be inside its period");
static_assert(MXFP8_FA4_MUFU_BEGIN_PAIR >= 0 &&
              MXFP8_FA4_MUFU_BEGIN_PAIR < MXFP8_FA4_MUFU_END_PAIR,
              "FA4 MUFU split window must be non-empty");

__device__ __forceinline__ float2 native_ex2_pair(float2 x) {
  float2 out;
  asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(out.x) : "f"(x.x));
  asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(out.y) : "f"(x.y));
  return out;
}

// Approximate 2^x for a pair of FP32 values.  Softmax inputs satisfy x < 127.
// The lower clamp keeps the reconstructed exponent normal; values below this
// threshold are already zero after the E4M3 P conversion.
__device__ __forceinline__ float2 emulated_ex2_pair(float2 x) {
  float2 out;
  asm(
      "{\n\t"
      ".reg .f32 x0, x1, magic, c3, c2, c1, one;\n\t"
      ".reg .b64 xy, magic2, rounded, rounded_back, frac;\n\t"
      ".reg .b64 c3x2, c2x2, c1x2, onex2, poly;\n\t"
      ".reg .s32 ri0, ri1, pi0, pi1, exp0, exp1, bits0, bits1;\n\t"
      "max.ftz.f32 x0, %2, 0fC2FE0000;\n\t"  // -127.0f
      "max.ftz.f32 x1, %3, 0fC2FE0000;\n\t"
      "mov.b64 xy, {x0, x1};\n\t"
      "mov.f32 magic, 0f4B400000;\n\t"     // 2^23 + 2^22
      "mov.b64 magic2, {magic, magic};\n\t"
      "add.rm.ftz.f32x2 rounded, xy, magic2;\n\t"
      "sub.rn.ftz.f32x2 rounded_back, rounded, magic2;\n\t"
      "sub.rn.ftz.f32x2 frac, xy, rounded_back;\n\t"
      "mov.f32 c3, 0f3D9DF09D;\n\t"
      "mov.f32 c2, 0f3E6906A4;\n\t"
      "mov.f32 c1, 0f3F31F519;\n\t"
      "mov.f32 one, 0f3F800000;\n\t"
      "mov.b64 c3x2, {c3, c3};\n\t"
      "mov.b64 c2x2, {c2, c2};\n\t"
      "mov.b64 c1x2, {c1, c1};\n\t"
      "mov.b64 onex2, {one, one};\n\t"
      "fma.rn.ftz.f32x2 poly, frac, c3x2, c2x2;\n\t"
      "fma.rn.ftz.f32x2 poly, poly, frac, c1x2;\n\t"
      "fma.rn.ftz.f32x2 poly, poly, frac, onex2;\n\t"
      "mov.b64 {ri0, ri1}, rounded;\n\t"
      "mov.b64 {pi0, pi1}, poly;\n\t"
      "shl.b32 exp0, ri0, 23;\n\t"
      "shl.b32 exp1, ri1, 23;\n\t"
      "add.s32 bits0, exp0, pi0;\n\t"
      "add.s32 bits1, exp1, pi1;\n\t"
      "mov.b32 %0, bits0;\n\t"
      "mov.b32 %1, bits1;\n\t"
      "}\n"
      : "=f"(out.x), "=f"(out.y)
      : "f"(x.x), "f"(x.y));
  return out;
}

__device__ __forceinline__ float2 dispatch_ex2_pair(int pair_index, float2 x) {
  // The caller is a fully unrolled fixed-trip loop, so pair_index is folded by
  // ptxas for every call site. SASS validation enforces that this emits neither
  // control flow nor index arithmetic.
  bool const use_emulation =
      pair_index >= MXFP8_FA4_MUFU_BEGIN_PAIR &&
      pair_index < MXFP8_FA4_MUFU_END_PAIR &&
      ((pair_index - MXFP8_FA4_MUFU_BEGIN_PAIR) % MXFP8_FA4_MUFU_PERIOD) ==
          MXFP8_FA4_MUFU_SLOT;
  if (use_emulation) {
    return emulated_ex2_pair(x);
  } else {
    return native_ex2_pair(x);
  }
}

} // namespace cutlass::fmha::collective::fa4_mufu_split
