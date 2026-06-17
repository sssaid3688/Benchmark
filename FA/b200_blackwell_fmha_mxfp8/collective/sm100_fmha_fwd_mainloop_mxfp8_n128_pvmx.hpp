/***************************************************************************************************
 * ===== CtaN=128 single-stage variant (M3) — MAINLOOP TRANSFORMED =====
 * Single-softmax-warp, M=128, CtaN=128 rewrite of the verified CtaN=64
 * mainloop (sm100_fmha_fwd_mainloop_mxfp8.hpp, untouched). See
 * docs/OP6_M3_CTAN128_PLAN.md.
 * DONE here: ThreadShape<1,1,1>; single-stage TmemAllocation (single-buffered
 * S0 + O0 + SFA0 + SFB0, kEnd=320); single-stage mma() (one QK, one PV per KV
 * tile; pipeline op counts verified n+1 on s0, n on corr); correction() and
 * correction_empty() single-stage (one O, one epilogue signal); softmax_step
 * order_s removed (single softmax warp group).
 * STILL NEEDED to build/run (see plan): single-stage epilogue collective
 * (store() o0_index=blk, 1 wait/store/release); single-Q load; driver
 * -DMXFP8_N128 path. NOT YET BUILT/VERIFIED.
 * =====================================================================
 *
 * Operator 6 — Milestone 2 / Route C : MXFP8 FlashAttention
 * File 2 of 3 : block-scaled QK mainloop collective.
 *
 * Derived from CUTLASS example 77
 *   examples/77_blackwell_fmha/collective/sm100_fmha_fwd_mainloop_tma_warpspecialized.hpp
 * (pristine CUTLASS is never edited; this is a standalone modified copy).
 *
 * Route C changes vs the pristine collective — all marked `// [MXFP8]`:
 *   - QK CollectiveBuilder : OpClassTensorOp -> OpClassBlockScaledTensorOp
 *       Q,K become mx_float8_t<float_e4m3_t> = (e4m3 data, ue8m0 scale factor).
 *   - PV stays plain-FP8 (unchanged) — P is generated on-chip by softmax.
 *   - QK N-tile shrunk 128 -> 64 (driver passes TileShape with mode-1 == 64)
 *       so the SFA/SFB TMEM operands fit in the 512-column TMEM budget:
 *         S0,S1 = 64 cols each ; O0,O1 = 128 cols each ; SF region = 128 cols.
 *   - SF rides the existing Q / KV TMA pipelines (no new pipeline): each Q-slot
 *     also carries SFQ, each KV-slot also carries an SF-sized payload (real K
 *     scale factors on K-slots, ignored filler on V-slots) so the pipeline
 *     transaction-byte counts stay uniform.
 *   - mma(): before every QK UMMA, the relevant SF tile is copied smem->TMEM
 *     via tcgen05.cp (UTCCP), and the UMMA is issued as a block-scaled MMA
 *     `mma_qk.with(scaleC, tCtSFA, tCtSFB)`.
 *
 * The example-77 forward kernel header is generic over the mainloop collective
 * and needs no modification: the SF pointers/layouts are encapsulated inside
 * Load::Arguments and thread through automatically.
 ***************************************************************************************************/
#pragma once

// ── [PSTATIC] static P quantization (client-requested evaluation) ──
// P's e8m0 scale factor becomes a COMPILE-TIME CONSTANT exponent instead of
// the online per-32-block amax: the whole amax chain (fused fmax pass, e8m0
// ceil-log2 math), the per-tile SFP smem store and the per-tile SFP UTCCP are
// all compiled out. SFP TMEM is filled ONCE (constant byte) at first_pv.
// The fixed scale is 2^floor(log2(1/448)) = 2^-9, so softmax writes
// E4M3(P * 512) and PV dequantizes with UE8M0 byte 118.
// Numerics: P > 448*2^EXP saturates (e4m3 satfinite); P < ~2^(EXP-9) flushes
// to zero — accuracy vs the dynamic release is part of the evaluation.
#ifdef MXFP8_PSTATIC
#ifndef MXFP8_PSTATIC_EXP
#define MXFP8_PSTATIC_EXP -9
#endif
#if (MXFP8_PSTATIC_EXP) != -9
#error "MXFP8_PSTATIC uses fixed P scale 2^-9; set MXFP8_PSTATIC_EXP=-9."
#endif
static constexpr int kMXFP8PStaticExp = MXFP8_PSTATIC_EXP;
static constexpr int kMXFP8PStaticSfpByte = 127 + kMXFP8PStaticExp;
#endif

// ── [刀15 SM12] 2SM 12-warp softmax (correction 并入 softmax G0) ──
// 设计文档 docs/projects/op6_2sm/SM12_DESIGN.md。默认关; 宏关 = 现 8+4 模式
// 逐字保留 (SASS bit-exact 卫生闸门)。骨架形态 MXFP8_SM12_SKELETON: G0/G1 =
// 今日 64/64 列, G2 = 0 列纯陪跑 (验证 schedule/计数/reg 不死锁)。
#ifdef MXFP8_SM12
#ifndef MXFP8_PSTATIC
#error "MXFP8_SM12 requires MXFP8_PSTATIC (16-col granule slices cut the dynamic 32-col SF amax blocks)"
#endif
#ifndef MXFP8_E2RSF
#error "MXFP8_SM12 requires MXFP8_E2RSF (only the fused row_sum path is implemented in softmax_step12)"
#endif
#if defined(MXFP8_OSPLIT) || defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2OFFLOAD_V2) || \
    defined(MXFP8_2SM_DECOUPLE_PERF) || defined(MXFP8_LCFUSE) || defined(MXFP8_G1NOMAX)
#error "MXFP8_SM12 incompatible with OSPLIT/E2OFFLOAD/DECOUPLE_PERF/LCFUSE/G1NOMAX"
#endif
#endif

// ── [刀21 WHYST] w 滞回懒链: w 只在 g0 局部 max 超出 w_{k-1} 超过 T binade
// (P 域) 时推进 — correction old==new ⟹ scale==1 精确 ⟹ O rescale 跳过率趋
// 100% (刀20 noinit 探针: 该税 2SM SOW ~54)。阈值 MXFP8_WHYST_T (binade),
// 过冲账: 懒链原生滞后 + T 必须 < log2(448)≈8.8 (PSTATIC e0 satfinite)。
#ifdef MXFP8_WHYST
#ifndef MXFP8_WHYST_T
#define MXFP8_WHYST_T 4
#endif
#if defined(MXFP8_LCFUSE) || defined(MXFP8_SM12)
#error "MXFP8_WHYST: hysteresis only wired into the default (non-LCFUSE, non-SM12) chain publish"
#endif
#endif

// ── [刀13 E2OFFLOAD_V2] 刀10 复活变体: 同一套 slice 机器, 只改放置点 ──
// ★★ TOMBSTONE 2026-06-13 (刀13 终判, 默认关; 宏关 SASS bit-exact vs px8a 源) ★★
// 数值全过 (金标准 0.01721 + 50-shape sweep 50/50, max_abs 带 0.0063-0.0323 同
// 基线带) 但 perf 比刀10 更深负, 3 复测极稳 (探针 819.9/820.3 干净窗):
//   px8a 1149.7/1094.9 → px13a(V2,C=16) 786.3/743.4 = -363.4/-351.5
//   (刀10 同 C=16 是 -138.6 — V2 放置反而 ×2.6 倍恶化)
// 死因 = 影子时间律失效的精确边界: "release 后工作免费" ⟺ 该工作不在任何
// consumer 的 gate 链上 (E2RSF 的 row_sum 谁也不 gate, 故免费). 而 E2OFFLOAD
// 的 slice 本质是 PV(t) 的 producer 输入 — 放哪都在 PV 的 gate 链上:
//   刀10 位置 (stats 读后/O wait 前): slice 与 "等 O"(= PV(t-1) 尚在跑) 部分
//     重叠, 净伤只是 O release 反应链拖长 → -138;
//   V2 位置 (O release 后): PV(t) 的 P-done gate (s0/s1 empty 512-count 含
//     correction 的 arrive) 改为严格排在 [PV(t-1) done → O wait 返回 →
//     rescale → release → slice(t) 全算完] 之后 — slice 全延迟 (2×LDTM +
//     32 exp2 + STS + 2 fence) 每 tile 100% 落在 PV(t-1)→PV(t) 发射间隙,
//     零重叠 (softmax 48 列从 QK(t) done 即开算, 早已完成, correction 是唯一
//     尾巴) → -363. 连重叠都丢了, 比刀10 更糟.
// GATEPROBE ~9 的误导性澄清: 它量的是 "刀10 位置上 PV 不等 slice" 的闸门税
// (slice 仍与 O-wait 重叠地算), 不是 "slice 起点推迟到 PV(t-1) 完成后" 的
// 串行成本. 第三放置点不存在 (stats 读之前 S 未 ready) ⇒ E2OFFLOAD 族
// (correction 接 P slice, 任意放置) 正式全族判死, 与刀10 推论闭环.
// 原 V2 设计要点 (存档):
//   * S(t) 有效性 — 本组尚未在 s0/s1 empty barrier 上 arrive, QK(t+2) 对同
//     parity slot 的覆写需要 512-count 全到 (含本组的 256), 不可能先发生;
//   * new_max 已在寄存器 (stats slot 早释放无碍);
//   * PV(t) 的 O-gate 在 release 即开, P-done gate 等 slice 后的 arrive.
#if defined(MXFP8_E2OFFLOAD_V2) && !defined(MXFP8_E2OFFLOAD)
#define MXFP8_E2OFFLOAD
#endif
#if defined(MXFP8_E2OFFLOAD_V2) && defined(MXFP8_E2OFFLOAD_GATEPROBE)
#error "MXFP8_E2OFFLOAD_V2 and MXFP8_E2OFFLOAD_GATEPROBE are mutually exclusive slice placements"
#endif
#if defined(MXFP8_E2OFFLOAD_V2) && defined(MXFP8_OSPLIT)
#error "MXFP8_E2OFFLOAD_V2 + MXFP8_OSPLIT placement not implemented"
#endif

#ifdef MXFP8_E2OFFLOAD
// ★★ TOMBSTONE 2026-06-12 (刀10 终判, 默认关; 宏关 SASS bit-exact 双验) ★★
// 数值全过 (金标准 + 50-shape sweep 100/100, max_abs 同带 0.009-0.032) 但 perf 深负:
//   px8a 1149.2/1093.8 → px10a(C=16) 1010.6/966.2 (-138.6/-127.6)
//                       → px10b(C=32) 974.6/930.3 (单调更差)
// 归因探针 (同窗, 探针 820.1/820.5):
//   GATEPROBE (arrive 先于 slice, PV 不等) 1019.6/974.3 → 闸门税仅 ~9 — 主税不在 PV 等 slice;
//   E2NULL  (softmax 纯砍 16 列, 零结构)   1178.5/1114.2 → 纯减负上限 +29.3/+20.4.
// 死因 = 等待链律的隐蔽反向: correction 并非真闲 — O 是单缓冲 (PipelineO depth-1),
// correction 的 O consumer_release 直接 gate 下一个 PV 的 producer_acquire. slice 计算
// (2×LDTM + 32 exp2 + STS + fences, 96-reg 配额) 插在 stats 读与 O wait 之间, 把
// "O ready → rescale → release" 的反应链每 tile 拖长 → PV 节拍整体退速 ≈ -130.
// 即: 把工作塞进"等待者"的指令流 = 塞进 PV 发射链 (刀2/LCFUSE 族, 换了张脸).
// 推论: exp2 搬运族 (搬给任何同 SM 警组) 全族死 — 要么在 PV 链上 (correction/MMA),
// 要么共享同 SMSP 槽位池 (softmax 内部分流 = 刀3 墓碑). 残余 exp2 池 +123 只能靠
// "消灭计算" — 而路线A bit-trick 直造 e4m3 的槽位账 ≥3.75-5.5 slot/elem (判据 ≤3,
// EXP2FMA-630/SOFTEXP-503 实证同族) 同样死. 1400 在当前 kernel 结构下不可达.
// ── [刀10 E2OFFLOAD] exp2 work redistribution: the tail MXFP8_E2OFFLOAD_C
// columns of EACH softmax group's 64-col half are computed by the CORRECTION
// warpgroup (exp2 + e4m3 convert + P STS + partial row_sum) instead of the
// softmax warps. Rationale (刀9 终判): 2SM residual exp2 pool = +123 and the
// stall is WAIT-latency-type (ncu 10k/14.8k wait, XU pipe only 46%) — more
// warps issuing EX2 concurrently raises XU util; softmax's burst shrinks by
// C/64. Zero NEW sync on the softmax side:
//   * S-ready for correction = g0's pipeline_c stats commit (happens-before
//     transitivity through the mbarrier: g0 consumed S before committing).
//   * P-done gate = correction JOINS the S sub-pipelines' consumer arrival
//     count (the PV MMA's producer_acquire on s0+s1 already gates PV on those
//     empty barriers — correction's arrive slots in for free).
//   * row_max for the slice = kIdxNewRowMax from the stats it already reads.
//   * row_sum: correction keeps a per-row partial (same per-tile rescale
//     chain via the stats old/new pair) and folds it into the final
//     normalization scale (1/(sum+corr_sum)) — no extra barrier.
// Chain safety: the standalone rowmax pass (no LCFUSE in the release flag
// set) still covers ALL 64 raw columns, so g0's lazy-chain w_k is UNCHANGED.
// Masked steps are NOT offloaded (kE2Keep folds back to 64; correction skips
// t >= unmasked_trip_count) — the ResidualMask logic stays softmax-only.
#ifndef MXFP8_PSTATIC
#error "MXFP8_E2OFFLOAD requires MXFP8_PSTATIC (P SF must be a compile-time constant; correction cannot participate in the dynamic per-32 amax)"
#endif
#ifdef MXFP8_2SM_DECOUPLE_PERF
#error "MXFP8_E2OFFLOAD is incompatible with MXFP8_2SM_DECOUPLE_PERF (it relies on the pipeline_c stats commit as the S-ready signal)"
#endif
#ifndef MXFP8_E2OFFLOAD_C
#define MXFP8_E2OFFLOAD_C 16
#endif
#if (MXFP8_E2OFFLOAD_C % 16) != 0 || (MXFP8_E2OFFLOAD_C) < 16 || (MXFP8_E2OFFLOAD_C) > 32
#error "MXFP8_E2OFFLOAD_C must be 16 or 32 (STS.128 = 16-col granules; >32 starves the softmax side)"
#endif
#endif

// ── Fast exp2 approximation using PTX ex2.approx instruction ──
__device__ __forceinline__ float fast_exp2f(float x) {
#if defined(MXFP8_EXP2FMA_PROBE)
    // [T3 PROBE — NUMERICS WRONG, FLOPs representative] pure 4-FFMA poly, no
    // range reduction. ncu: exp2 (ex2.approx) = 8.7% samples, XU pipe 46% vs
    // FMA 15% — quantifies the upper bound of moving exp2 off the XU pipe.
    float p = fmaf(x, 0.0558f, 0.2401f);
    p = fmaf(p, x, 0.6931f);
    p = fmaf(p, x, 1.0f);
    p = fmaf(p, x, 0.0f);   // 4th FFMA to keep the op count representative
    return p;
#elif defined(MXFP8_EXP2FMA)
    // [T3 REAL] FMA-pipe exp2: clamp + round-to-int (magic number) + deg-3
    // minimax poly on the fraction + exact 2^i scale. Rel err ~2e-4 over
    // f∈[-0.5,0.5] — far below e4m3 P quantization noise (~6%). x<=0 here
    // (softmax: scale*(s - row_max)); clamp at -120 keeps the 2^i scale
    // normal (e>=7) and exp2(-120) is exactly 0 after P e4m3 quantization.
    // Upper clamp +124: under the lagged chain g1's P half can exceed 1.0
    // (x>0, absorbed by the e8m0 P-SF); +124 keeps (i+127)<<23 from wrapping
    // (documented chain margin is ~120 binades, so the clamp never bites
    // real data — it only guards the bit-trick's validity).
    x = ::fmaxf(::fminf(x, 124.0f), -120.0f);
    float t = x + 12582912.0f;                       // 1.5*2^23: RN round-to-int
    int   i = __float_as_int(t) - __float_as_int(12582912.0f);
    float f = x - (t - 12582912.0f);                 // f in [-0.5, 0.5]
    float p = fmaf(f, 0.0792043f, 0.2241708f);
    p = fmaf(p, f, 0.6931513f);
    p = fmaf(p, f, 1.0f);
    return p * __int_as_float((i + 127) << 23);      // * 2^i (exact)
#else
    float result;
    asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(result) : "f"(x));
    return result;
#endif
}

#if defined(MXFP8_EXP2NOP_PERF)
// [2SM E2NOP PROBE — NUMERICS WRONG, FLOPs/issue-slots reduced] identity in
// place of the softmax P exp2 — quantifies the exp2-chain ceiling in the 2SM
// kernel (base measured +47 = 1001.6; 2SM's exp2 stall is wait-latency-type
// vs base's mio-queue-full — the headroom may differ). Calibration only.
#define EXP2_SOFTMAX(x) (x)
#else
#define EXP2_SOFTMAX(x) fast_exp2f(x)
#endif

#if defined(MXFP8_EXP2MIX)
// ── [EXP2MIX] partial exp2 offload: MUFU ex2.approx -> FMA polynomial ──
// ncu (2SM pstatic): exp2 stall 14.8k dominated by wait 10k (LATENCY-type, on
// the critical chain) + XU(MUFU) 51.9% vs FMA pipe 9.8% (slots idle). The two
// historical 100% moves died (SOFTEXP 503 instr blowup / EXP2FMA serial-FMA
// chain longer than MUFU latency) — both made the SERIAL chain longer. Here
// only a compile-time 1/RATIO subset of element PAIRS takes the FMA poly; the
// rest stay on ex2.approx. Unrolled loop => `(i/2) % RATIO` folds at compile
// time (no data-dependent branch), and the poly's FMAs fill the idle FMA pipe
// IN PARALLEL with neighboring iterations' MUFU ops — breaking the MUFU
// back-to-back dependency cadence without lengthening any single chain.
// RATIO semantics: every RATIO-th pair (pair index % RATIO == 0) goes FMA,
// i.e. fraction 1/RATIO of all elements. 8 -> 1/8, 4 -> 1/4, 3 -> 1/3.
// NUMERICS: poly rel err ~2e-4 (vs ex2.approx ulp-level) — NOT bit-identical
// to baseline; far below e4m3 P quantization noise (~6%). Gate via sweep.
// ★ TOMBSTONE 2026-06-12 (probe-clean 820.3/820.3 window, 3-rep best):
//   2SM pstatic base 1128.4 / 1074.8 (b1h8s32k / b1h16s16k)
//   RATIO=8 (1/8): 1055.7 / 1004.6  (-72.7 / -70.2)
//   RATIO=4 (1/4):  999.6 /  945.2  (-128.8 / -129.6)
//   Loss ~LINEAR in mixed fraction ⟹ no ratio wins; limit ratio->0 is +0.
//   Mechanism: the poly's ~8 extra instrs per mixed pair are issued by the
//   SAME softmax warp — issue slots + the poly's serial dep chain (longer than
//   one MUFU latency) are the binding resource, not the idle FMA *pipe*
//   (pipe-utilization headroom ≠ issue-slot headroom). Mixed elements finish
//   LATER than ex2 elements, so the downstream amax/convert consumers wait on
//   the poly tail — same death mechanism as the 100% moves (SOFTEXP instr
//   blowup / EXP2FMA serial chain), just scaled by the fraction. Partial
//   offload direction SEALED at all ratios.
#ifndef MXFP8_EXP2MIX_RATIO
#define MXFP8_EXP2MIX_RATIO 4
#endif
__device__ __forceinline__ float fast_exp2f_fma(float x) {
    // Same poly as the MXFP8_EXP2FMA tombstone: clamp + magic-number
    // round-to-int + deg-3 minimax on f∈[-0.5,0.5] + exact 2^i bit-scale.
    x = ::fmaxf(::fminf(x, 124.0f), -120.0f);
    float t = x + 12582912.0f;                       // 1.5*2^23: RN round-to-int
    int   i = __float_as_int(t) - __float_as_int(12582912.0f);
    float f = x - (t - 12582912.0f);                 // f in [-0.5, 0.5]
    float p = fmaf(f, 0.0792043f, 0.2241708f);
    p = fmaf(p, f, 0.6931513f);
    p = fmaf(p, f, 1.0f);
    return p * __int_as_float((i + 127) << 23);      // * 2^i (exact)
}
// idx = element index of the pair head (loop steps by 2); condition is a
// compile-time constant per unrolled iteration.
#define EXP2_SOFTMAX_MIX(idx, x) \
    (((((idx) / 2) % (MXFP8_EXP2MIX_RATIO)) == 0) ? fast_exp2f_fma(x) : EXP2_SOFTMAX(x))
#else
#define EXP2_SOFTMAX_MIX(idx, x) EXP2_SOFTMAX(x)
#endif

#if defined(MXFP8_E2POLY)
// ── [刀17 E2POLY — oyhj 移植刀 #1] 25% FMA-pipe poly exp2 分流 ──
// oyhj (同源 fork, SOW 1351.6 vs 我方 1290.5) SASS 实锤: lean burst (只有
// bias-FMA + exp2, row_sum 在 post-release shadow pass) 里把 1/4 的元素对
// ((j&7)==6) 分流到 FMA 管 poly = 净赚; 它 sweep 过 25% 甜点 (37.5%/50% 更差)。
// 与刀3 EXP2MIX 墓碑 (-72.7/-128.8) 的死因区别 = burst 载荷条件: E2RSF 把
// ~37 条 row_sum FADD2 织进 burst 后 issue 槽位已满, poly 的 ~10 条/对是纯
// 加法; lean burst (本刀配 E2RSF 关) 里才有槽位可填。**必须与 lean burst
// (去 -DMXFP8_E2RSF) 配对使用** — 单独开 = 重蹈刀3。
// poly 形态 = oyhj 原版 deg-4 (c4..c1) + magic-FADD round + 指数位加法拼接
// (省掉我方 EXP2FMA 的尾乘 FMUL); 上 clamp +124 是我方懒链特有 (g1 P 半可
// 过冲 x>0, oyhj 真 rowmax 恒 x<=0 故无此项), 只保 bit-trick 合法性。
// |rel err| ~4e-5 << e4m3 量化噪声 ~6%; 求和序不变但 poly 值 != MUFU 值
// -> 数值刀, 走 sweep 闸门。
// 分流谱可调: 元素对 head 索引 idx 满足 (idx & MOD) == SEL 走 poly。
// MOD=7/SEL=6 -> 25% (oyhj 甜点); MOD=15/SEL=14 -> 12.5%。
#ifndef MXFP8_E2POLY_MOD
#define MXFP8_E2POLY_MOD 7
#endif
#ifndef MXFP8_E2POLY_SEL
#define MXFP8_E2POLY_SEL 6
#endif
__device__ __forceinline__ float fast_exp2f_poly(float x) {
    x = ::fminf(x, 124.0f);                      // [懒链过冲] 保 (e2<<23) 不回绕
    x = ::fmaxf(x, -125.0f);                     // keep 2^x normal; true value ~1e-38 ~ 0
    float t  = x + 12582912.0f;                  // 2^23 + 2^22 magic: round-to-nearest int
    float xi = t - 12582912.0f;                  // integer part round(x)
    float xf = x - xi;                           // fractional part in [-0.5, 0.5]
    float p  = fmaf(xf, 0x1.3b2c9cp-7f, 0x1.c6b08ep-5f);   // c4*xf + c3
    p = fmaf(xf, p, 0x1.ebfbe0p-3f);             // c2
    p = fmaf(xf, p, 0x1.62e430p-1f);             // c1 = ln2
    p = fmaf(xf, p, 1.0f);                       // 2^xf
    int e2 = __float_as_int(t) - 0x4B400000;     // low mantissa bits of t = (int)xi
    return __int_as_float(__float_as_int(p) + (e2 << 23));   // p * 2^xi
}
#define EXP2_SOFTMAX_PSEL(idx, x) \
    ((((idx) & (MXFP8_E2POLY_MOD)) == (MXFP8_E2POLY_SEL)) ? fast_exp2f_poly(x) : EXP2_SOFTMAX_MIX(idx, x))
#else
#define EXP2_SOFTMAX_PSEL(idx, x) EXP2_SOFTMAX_MIX(idx, x)
#endif

#if defined(MXFP8_2SM_DECOUPLE_PERF)
// [2SM DECOUPLE PROBE — NUMERICS WRONG, FLOPs identical] the 2SM mirror of
// the 1SM MXFP8_DUALTILE_PERF decouple ceiling (1047 there, vs coupled 976).
// Compiles out ALL remaining mma<->softmax<->correction handshakes that the
// m33b lazy chain left standing:
//   1. PipelineS sub-slot rotation (s0+s1): mma producer acquire/commit +
//      softmax consumer wait/release (incl. final re-wait + tail balancing) —
//      QK free-runs ahead, softmax reads stale S, PV reads stale P. ++state
//      kept everywhere so buffer indices stay in lockstep.
//   2. order_s one-way w-chain: all wait()/arrive() dropped; the smem
//      publish/read STS/LDS stay (racy chain values — FLOPs parity).
//   3. PipelineC (softmax g0 -> correction stats): producer acquire/commit +
//      consumer wait/release dropped; correction reads racy stats. The stats
//      values remain monotone fmax-chain outputs, so the old==new rescale-skip
//      frequency stays release-like (FLOPs profile comparable).
// KEPT (any real design keeps them): PipelineO mma->corr (O single-buffered),
// pipeline_kv/q, B_FINAL once-per-kernel merge, pipeline_epi. Same keep-set
// as the 1SM dtprobe MINUS its pipeline_c (this probe is strictly more
// aggressive — it measures the FULL structural decouple ceiling).
// Calibration only; default OFF.
#if defined(MXFP8_LCFUSE) || defined(MXFP8_2SM_BEACON)
#error "MXFP8_2SM_DECOUPLE_PERF excludes MXFP8_LCFUSE / MXFP8_2SM_BEACON (tombstone paths not maintained under the probe)"
#endif
#endif

#include "cutlass/cutlass.h"
#include "cutlass/arch/memory_sm80.h"
#include "cutlass/arch/barrier.h"                            // [MXFP8 N128 M3b] NamedBarrier
#include "cutlass/float8.h"                                  // [MXFP8] mx_float8_t / float_ue8m0_t
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cutlass/detail/sm100_blockscaled_layout.hpp"       // [MXFP8] Sm1xxBlockScaledConfig
#include "cutlass/detail/sm100_tmem_helper.hpp"              // [MXFP8] tmem helpers
#include "cute/arch/simd_sm100.hpp"
#include "cute/tensor.hpp"
#include "cute/layout.hpp"

#include "collective/fmha_common.hpp"
#include "collective/fmha_fusion.hpp"
#include "sm100_fmha_load_tma_mxfp8_n128.hpp"                // [MXFP8 N128] single-Q SF-aware load

#ifdef MXFP8_DBG
// Debug: capture the SF smem the MMA warp sees after the TMA load.
__device__ uint8_t g_dbg_sfk[2048];
__device__ uint8_t g_dbg_sfq[2048];
// Debug: capture raw QK scores for Q-row 0, first KV tile. [续19d] widened to 128
// so g1's half (kv 64-127) is captured too (it was NEVER verified — only g0's 0-63).
__device__ float   g_dbg_S[256];
__device__ int     g_dbg_S_got;
// Debug: capture raw QK score S[q,kv=0] for Q-rows 0..63.
__device__ float   g_dbg_Srow[64];
// [续19c] capture final O for q-row 0 (all 128 d), post-scale, in correction_epilogue.
__device__ float   g_dbg_O[128];
__device__ int     g_dbg_O_got;
// [续19d] capture raw exp softmax weights P for q-row 0 (all 128 kv), pre-quant,
// + the final combined row_sum. Splits softmax-vs-PV: O_ref_from_P should match
// the true ref O iff the kernel's P (softmax) is correct.
__device__ float   g_dbg_P[256];
__device__ int     g_dbg_P_got;
__device__ float   g_dbg_rowsum;
// [续19g] SFP (P-scale) exponent per global kv-block (row 0), both tiles.
__device__ int     g_dbg_sfp_exp[8];
// [续19g] dequantized smem_p (e4m3 × 2^exp_b) for row 0 at global kv — the P data the PV reads.
__device__ float   g_dbg_Pdq[256];
// [续19h] O accumulator after PV(0) (before tile-1 rescale/accumulate), row 0, per d.
__device__ float   g_dbg_OafterPV0[128];
// [续19h] O before final normalize (= full PV(0)+PV(1) accumulator), row 0, per d.
__device__ float   g_dbg_Obn[128];
// [续19h] O re-read after correction copy_out (round-trip check), row 0, per d.
__device__ float   g_dbg_Orr[128];
// [续19i] per-PV-call: which V KV-stage (vidx) and SF buffer (buf) each PV uses.
__device__ int     g_dbg_pv_vidx[8];
__device__ int     g_dbg_pv_buf[8];
__device__ int     g_dbg_pv_first[8];
__device__ int     g_dbg_npv;
// [续19j] smem_sfv (V-SF) capture (2048 bytes), the input to the PV's SFV UTCCP.
__device__ uint8_t g_dbg_sfv[2048];
// [续19k] PEER (block 1) S for local row 0 (= Q row 128), to verify peer's QK.
__device__ float   g_dbg_Sp[256];
__device__ int     g_dbg_Sp_row;
__device__ int     g_dbg_qnz;   // peer smem_q nonzero byte count
__device__ int     g_dbg_qsz;
__device__ int     g_dbg_sbuf_blk[8];  // [续19m] per-CTA sbuf at tile0 (detect S-buffer desync)
__device__ int     g_dbg_sbuf_val[8];
__device__ int     g_dbg_sbuf_n;
__device__ int     g_dbg_sfq_peer_nz;   // [续19p] peer smem_sfq (Q-scale) nonzero
__device__ int     g_dbg_sfq_peer_sz;
__device__ int     g_dbg_sfk_peer_nz;
__device__ unsigned long long g_dbg_qsum[2];  // [续19r] smem_q weighted byte-sum per CTA
__device__ unsigned int g_dbg_tmem_base[2];   // [续19t] tmem_base_ptr per CTA (leader/peer)
// [续19f] correction cross-tile rescale: per corr_tile, capture scale + old/new max (row 0).
__device__ float   g_dbg_corr_scale[8];
__device__ float   g_dbg_corr_old[8];
__device__ float   g_dbg_corr_new[8];
__device__ int     g_dbg_corr_n;
#endif

namespace cutlass::fmha::collective {

using namespace cute;

// [2SM BEACON] host-mapped progress beacon to localize cluster deadlocks (printf
// can't flush on a hang). g_bcn points at zero-copy memory; host polls it live.
#if defined(MXFP8_2SM_BEACON)
// g_bcn is defined in the load header (sm100_fmha_load_tma_mxfp8_n128.hpp), which is
// included above at line ~64 — same namespace, already in scope here. No redeclare.
#define BCN(val) do { if (cute::elect_one_sync()) { unsigned _r = cute::block_rank_in_cluster(); if (_r < 4) { atomicMax(&g_bcn[_r*16], (unsigned)(val)); __threadfence_system(); } } } while(0)
#else
#define BCN(val) do {} while(0)
#endif

// [MXFP8] block-scaled QK gemm helper — like fmha_common.hpp gemm_zero_acc, but
// every k-block UMMA is issued as a block-scaled MMA referencing SF in TMEM.
// [2SM BEACON] per-k_block beacon helper (slot0/MMA). bcn_pv!=0 marks PV gemm so
// we can localize WHICH cute::gemm issue stalls. 83=entered, 84+2k=before gemm(k),
// 85+2k=after gemm(k). QK calls pass 0 (no beacon).
#if defined(MXFP8_2SM_BEACON)
#define BCN_PV(val) do { if (cute::elect_one_sync()) { unsigned _r = cute::block_rank_in_cluster(); if (_r < 4) { atomicMax(&g_bcn[_r*16], (unsigned)(val)); __threadfence_system(); } } } while(0)
#else
#define BCN_PV(val) do {} while(0)
#endif
template<class Atom, class TA, class TB, class TC, class TSFA, class TSFB>
CUTE_DEVICE void gemm_bs(Atom& atom, bool zero_acc,
                         TA const& tA, TB const& tB, TC&& tC,
                         TSFA const& tSFA, TSFB const& tSFB,
                         bool bcn_pv = false) {
  using ScaleOut = decltype(atom.accumulate_);
  atom.accumulate_ = zero_acc ? ScaleOut::Zero : ScaleOut::One;
  if (bcn_pv) BCN_PV(83);
  CUTLASS_PRAGMA_UNROLL
  for (int k_block = 0; k_block < size<2>(tA); k_block++) {
    if (bcn_pv) BCN_PV(84 + 2 * k_block);
    cute::gemm(atom.with(atom.accumulate_, tSFA(_,_,k_block), tSFB(_,_,k_block)),
               tA(_,_,k_block), tB(_,_,k_block), tC);
    if (bcn_pv) BCN_PV(85 + 2 * k_block);
    atom.accumulate_ = ScaleOut::One;
  }
}

// PV variant for MXFP8 block-scaled MMA where A=P is already in TMEM and B=V
// remains the normal SMEM descriptor. CUTLASS wraps the SS spelling, but this
// TS spelling is needed to avoid staging P through smem_p.
template<bool kIs2Sm>
CUTE_DEVICE void tcgen05_mma_pv_mxfp8_block_scale_ts(
    uint32_t d_tmem,
    uint32_t p_tmem,
    uint64_t v_desc,
    uint32_t idesc_hi,
    uint32_t scaleC,
    uint32_t sfp_tmem,
    uint32_t sfv_tmem) {
#if defined(CUTE_ARCH_TCGEN05_MXF8F6F4_MMA_ENABLED)
  if (cute::elect_one_sync()) {
    if constexpr (kIs2Sm) {
      asm volatile(
        "{\n\t"
        ".reg .pred p;\n\t"
        "setp.ne.b32 p, %4, 0;\n\t"
        "tcgen05.mma.cta_group::2.kind::mxf8f6f4.block_scale [%0], [%1], %2, %3, [%5], [%6], p; \n\t"
        "}\n"
        :
        : "r"(d_tmem), "r"(p_tmem), "l"(v_desc), "r"(idesc_hi),
          "r"(scaleC), "r"(sfp_tmem), "r"(sfv_tmem));
    }
    else {
      asm volatile(
        "{\n\t"
        ".reg .pred p;\n\t"
        "setp.ne.b32 p, %4, 0;\n\t"
        "tcgen05.mma.cta_group::1.kind::mxf8f6f4.block_scale [%0], [%1], %2, %3, [%5], [%6], p; \n\t"
        "}\n"
        :
        : "r"(d_tmem), "r"(p_tmem), "l"(v_desc), "r"(idesc_hi),
          "r"(scaleC), "r"(sfp_tmem), "r"(sfv_tmem));
    }
  }
#else
  CUTE_INVALID_CONTROL_PATH("Attempting to use SM100 MXFP8 PV MMA without CUTE_ARCH_TCGEN05_MXF8F6F4_MMA_ENABLED");
#endif
}

template<bool kIs2Sm, class Atom, class TV, class TO, class TSFP, class TSFV>
CUTE_DEVICE void issue_pv_mxfp8_blockscaled_ts(
    Atom& atom,
    bool zero_acc,
    uint32_t p_tmem_base,
    TV const& tV,
    TO&& tO,
    TSFP const& tSFP,
    TSFV const& tSFV,
    bool bcn_pv = false) {
  using TOBase = std::remove_reference_t<TO>;
  static_assert(cute::is_rmem<typename TV::engine_type>::value,
                "SM100 MXFP8 PV expects V as an SMEM descriptor fragment.");
  static_assert(cute::is_tmem<typename TOBase::engine_type>::value,
                "SM100 MXFP8 PV accumulates O in TMEM.");
  static_assert(cute::is_tmem<typename TSFP::engine_type>::value,
                "SM100 MXFP8 PV expects SFP in TMEM.");
  static_assert(cute::is_tmem<typename TSFV::engine_type>::value,
                "SM100 MXFP8 PV expects SFV in TMEM.");

  using ScaleOut = decltype(atom.accumulate_);
  atom.accumulate_ = zero_acc ? ScaleOut::Zero : ScaleOut::One;
  if (bcn_pv) BCN_PV(83);

  CUTLASS_PRAGMA_UNROLL
  for (int k_block = 0; k_block < size<2>(tV); k_block++) {
    if (bcn_pv) BCN_PV(84 + 2 * k_block);
    auto tVk = tV(_,_,k_block);
    auto tSFPk = tSFP(_,_,k_block);
    auto tSFVk = tSFV(_,_,k_block);

    auto atom_k = atom.with(atom.accumulate_, tSFPk, tSFVk);
    uint64_t idesc = UMMA::make_runtime_instr_desc_block_scaled<>(
        atom_k.idesc_, atom_k.tsfa_addr_, atom_k.tsfb_addr_);
    uint32_t idesc_hi = uint32_t(idesc >> 32);
    // Each block-scaled MXFP8 MMA consumes K=32 FP8 P elements, packed as
    // 8 TMEM columns of 32-bit words.
    uint32_t p_tmem_k = p_tmem_base + uint32_t(k_block * 8);

    tcgen05_mma_pv_mxfp8_block_scale_ts<kIs2Sm>(
        raw_pointer_cast(tO.data()),
        p_tmem_k,
        tVk(_0{}),
        idesc_hi,
        uint32_t(atom.accumulate_),
        atom_k.tsfa_addr_,
        atom_k.tsfb_addr_);

    if (bcn_pv) BCN_PV(85 + 2 * k_block);
    atom.accumulate_ = ScaleOut::One;
  }
}

template<
  class Element_,
  class ElementQK_,
  class ElementPV_,
  class TileShape_,
  class StrideQ_,
  class StrideK_,
  class StrideV_,
  class Mask_,
  class ThreadShape = Shape<_1, _1, _1>,   // [MXFP8 N128] single-stage: M not split
  class OrderLoadEpilogue = cute::false_type
>
struct Sm100FmhaFwdMainloopTmaWarpspecializedMxfp8 {

  using Element = Element_;          // storage element for Q/K/V smem (float_e4m3_t)
  using ElementQK = ElementQK_;
  using ElementPV = ElementPV_;
  using TileShape = TileShape_;
  using StrideQ = StrideQ_;
  using StrideK = StrideK_;
  using StrideV = StrideV_;
  using Mask = Mask_;

#ifdef MXFP8_Q_STAGES
  // [续19an-perf] Q stage count override. With IndividualTileScheduler each CTA runs
  // ONE tile = ONE Q load, so the 2nd Q stage (16.5KB smem) is dead weight better
  // spent on deeper KV staging (we are register-walled to 1 CTA/SM anyway).
  static constexpr int StageCountQ = MXFP8_Q_STAGES;
#else
  static constexpr int StageCountQ = 2;
#endif
#ifdef MXFP8_KV_STAGES
  static constexpr int StageCountKV = MXFP8_KV_STAGES;
#else
  static constexpr int StageCountKV = sizeof(Element_) == 1 ? 4 : 3;
#endif

  using StagesQ = cutlass::gemm::collective::StageCount<StageCountQ>;
  using StagesKV = cutlass::gemm::collective::StageCount<StageCountKV>;

  using ClusterShape = Shape<_2, _1, _1>;   // [2SM M1a] cluster launch (was <_1,_1,_1>)

  static const int Alignment = 128 / sizeof_bits_v<Element>;

  using TileShapeQK = decltype(shape_div(TileShape{}, ThreadShape{}));

  using TileShapePV = decltype(select<0,2,1>(TileShapeQK{}));

  // [2SM M2] The 2Sm MMA atom requires the logical M-tile == 256 (each of the 2
  // CTAs in the cluster holds M=128 via AtomThrShape=2). DECOUPLE the MMA tile
  // (M=256, fed ONLY to the CollectiveBuilder) from the PER-CTA TileShape (M=128,
  // used for ALL load/smem/softmax/O indexing — unchanged from base A). The
  // scheduler tiles gmem by the per-CTA M=128, so cluster {2c,2c+1} covers
  // adjacent 128-row halves = one 256 MMA tile the 2Sm atom cooperates on.
  using MmaTileShapeQK = cute::Shape<cute::_256,
        decltype(cute::get<1>(TileShapeQK{})), decltype(cute::get<2>(TileShapeQK{}))>;
  using MmaTileShapePV = decltype(cute::select<0,2,1>(MmaTileShapeQK{}));

  // [MXFP8] block-scaled element pair for QK: (e4m3 data, ue8m0 scale factor).
  using ElementBlockScaled = cutlass::mx_float8_t<cutlass::float_e4m3_t>;

  // [MXFP8] QK collective is now block-scaled MXFP8.
  using CollectiveMmaQK = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideQ, Alignment,
      ElementBlockScaled, StrideK, Alignment,
      ElementQK,
      MmaTileShapeQK, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized2SmMxf8f6f4Sm100>::CollectiveOp;   // [2SM M2] MMA tile M=256

  // [M3 sub-tile] N=64 sub-MMA QK collective. The KV pipeline keeps 128-row
  // stages; each stage is consumed as TWO N=64 sub-MMAs (sub0 = kv cols 0-63,
  // sub1 = 64-127) that commit independently (sub0 -> PipelineS s0, sub1 -> s1)
  // so softmax g0/g1 start as soon as THEIR half of S lands — the FA4-style
  // sub-tile S pipeline (docs/projects/op6_2sm/M3_subtile_pipeline.md §3).
  // Per-CTA B (K) smem under the 2-SM N-split = 32 rows per sub-tile.
  using MmaTileShapeQK64 = cute::Shape<cute::_256, cute::_64,
        decltype(cute::get<2>(TileShapeQK{}))>;
  using CollectiveMmaQK64 = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideQ, Alignment,
      ElementBlockScaled, StrideK, Alignment,
      ElementQK,
      MmaTileShapeQK64, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized2SmMxf8f6f4Sm100>::CollectiveOp;
  // [M3 smoke] force instantiation + sanity: still a 2-SM atom.
  static_assert(cute::size(typename CollectiveMmaQK64::TiledMma::AtomThrID{}) == 2,
                "[M3] N=64 QK collective must remain a 2-SM (cta_group::2) atom");

  // [PVMX 2a.1] PV is now block-scaled MXFP8 SS: P,V = e4m3 + ue8m0 SF along seqlen_kv.
  using CollectiveMmaPV = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideK, Alignment,
      ElementBlockScaled, decltype(select<1,0,2>(StrideV{})), Alignment,
      ElementPV,
      MmaTileShapePV, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized2SmMxf8f6f4Sm100>::CollectiveOp;   // [2SM M2] MMA tile M=256
  static constexpr bool kPVIs2Sm =
      int(cute::size(typename CollectiveMmaPV::TiledMma::AtomThrID{})) == 2;

#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] N=64 sub-MMA PV collective — the EXACT mirror of CollectiveMmaQK64,
  // applied to PV's N (= head dim D). PV(t) issues as 2 x (M256xN64) sub-MMAs writing
  // two INDEPENDENT 64-col O halves (O0 / O0+64), each on its own PipelineO stage, so
  // correction's "O ready -> rescale -> release" reaction chain interleaves at half-tile
  // granularity with the PV stream (刀10 死因 = O depth-1 release gates the next PV).
  // TMEM total unchanged (the halves tile the original 128-col O footprint exactly).
  // Per-CTA B (V) smem under the 2-SM N-split = 32 D-rows per sub-slot (K precedent).
  using MmaTileShapePV64 = cute::Shape<cute::_256, cute::_64,
        decltype(cute::get<2>(MmaTileShapePV{}))>;
  using CollectiveMmaPV64 = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm100, cutlass::arch::OpClassBlockScaledTensorOp,
      ElementBlockScaled, StrideK, Alignment,
      ElementBlockScaled, decltype(select<1,0,2>(StrideV{})), Alignment,
      ElementPV,
      MmaTileShapePV64, ClusterShape, cutlass::gemm::collective::StageCount<3> /* changed later */,
      cutlass::gemm::KernelTmaWarpSpecialized2SmMxf8f6f4Sm100>::CollectiveOp;
  static constexpr bool kPV64Is2Sm =
      int(cute::size(typename CollectiveMmaPV64::TiledMma::AtomThrID{})) == 2;
  static_assert(cute::size(typename CollectiveMmaPV64::TiledMma::AtomThrID{}) == 2,
                "[OSPLIT] N=64 PV collective must remain a 2-SM (cta_group::2) atom");
#endif

  using SmemLayoutQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutA{}, Int<StageCountQ>{}));
  // [M3 sub-tile] K smem is laid out for the N=64 sub-MMA: 2 sub-slots per KV
  // pipeline stage (sub0 = kv 0-63, sub1 = 64-127 of the 128-row stage; per-CTA
  // N-split = 32 rows per sub-slot). TOTAL bytes per stage are unchanged (2 x
  // 4KB = 8KB) so the smem_k/smem_v union stays intact. Sub-slot index = 2*stage+sub.
#if defined(MXFP8_2SM_N128SINGLE)
  // [刀27 N128SINGLE] single N128 QK MMA: K smem = ONE slot per KV stage
  // (per-CTA B = 64-N from the 2-SM atom halving N=128). Same total bytes as the
  // QK64 2-sub layout (2 x 32-N = 64-N), so the smem_k/smem_v union is unchanged.
  // (oyhj mainloop:193 target form.)
  using SmemLayoutK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutB{}, Int<StageCountKV>{}));
#else
  using SmemLayoutK = decltype(unstageSmemLayout(typename CollectiveMmaQK64::SmemLayoutB{}, Int<2*StageCountKV>{}));
#endif
#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] V smem laid out for the N=64 sub-MMA: 2 sub-slots per KV stage
  // (sub0 = D cols 0-63, sub1 = 64-127; per-CTA 2-SM N-split = 32 D-rows per
  // sub-slot). Same total bytes per stage — the smem_k/smem_v union is intact.
  // Sub-slot index = 2*stage+sub (exact mirror of SmemLayoutK).
  using SmemLayoutV = decltype(unstageSmemLayout(typename CollectiveMmaPV64::SmemLayoutB{}, Int<2*StageCountKV>{}));
#else
  using SmemLayoutV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutB{}, Int<StageCountKV>{}));
#endif
  // [PVMX 2a.0] P operand smem layout (block-scaled SS PV reads P from smem, not TMEM).
  // 2 stages, matching the double-buffered S/PV pipeline.
  static constexpr int StageCountP = 2;
  using SmemLayoutP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutA{}, Int<StageCountP>{}));

  // [MXFP8] scale-factor types/layouts exposed by the block-scaled QK collective.
  // [M3 sub-tile] QK-side SF types come from the N=64 sub-MMA collective so the
  // SFB TMEM fragment / UTCCP match the sub-MMA. A-side (SFA: M=256, K=128) is
  // N-independent — identical layouts either way.
  using ElementSF = typename CollectiveMmaQK::ElementSF;                 // float_ue8m0_t
#if defined(MXFP8_2SM_N128SINGLE)
  // [刀27 N128SINGLE] SF atoms from the N128 collective (oyhj mainloop:202-203).
  // SFB0/SFB1 TMEM slots + load_sfb are unchanged; only the atom source + the
  // do_qk SFB read (no sfb_parity +2) differ.
  using SmemLayoutAtomSFA = typename CollectiveMmaQK::SmemLayoutAtomSFA;
  using SmemLayoutAtomSFB = typename CollectiveMmaQK::SmemLayoutAtomSFB;
#else
  using SmemLayoutAtomSFA = typename CollectiveMmaQK64::SmemLayoutAtomSFA;
  using SmemLayoutAtomSFB = typename CollectiveMmaQK64::SmemLayoutAtomSFB;
#endif
  using TiledMmaQK        = typename CollectiveMmaQK::TiledMma;
  using TiledMmaQK64      = typename CollectiveMmaQK64::TiledMma;        // [M3 sub-tile]
  // [PVMX 2a.1] block-scaled PV SF types (P=SFA, V=SFB; per-32 along seqlen_kv).
  using TiledMmaPV        = typename CollectiveMmaPV::TiledMma;
#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] PV-side SF types come from the N=64 sub-MMA collective so the
  // SFV TMEM fragment / UTCCP match the sub-MMA (exact mirror of the QK64 SFB
  // treatment). A-side (SFP: M=256, K=128) is N-independent — identical layouts.
  // The SFV smem slot still holds the FULL 128-D-row SF atom (IsCtaN64 stride-0
  // pair in the load); the odd sub-MMA reads the SAME TMEM slot at +2 columns
  // (the proven sfb_parity trick).
  using TiledMmaPV64      = typename CollectiveMmaPV64::TiledMma;
  using SmemLayoutAtomSFP = typename CollectiveMmaPV64::SmemLayoutAtomSFA;  // P-SF atom (N-indep)
  using SmemLayoutAtomSFV = typename CollectiveMmaPV64::SmemLayoutAtomSFB;  // V-SF atom (64-N)
  using SmemLayoutSFP = decltype(unstageSmemLayout(typename CollectiveMmaPV64::SmemLayoutSFA{}, Int<2>{}));
  using SmemLayoutSFV = decltype(unstageSmemLayout(typename CollectiveMmaPV64::SmemLayoutSFB{}, Int<StageCountKV>{}));
  // A-side layouts must be N-independent (softmax's P/SFP smem write recipes assume so).
  static_assert(cute::is_same_v<typename CollectiveMmaPV64::SmemLayoutA, typename CollectiveMmaPV::SmemLayoutA>,
                "[OSPLIT] PV64 A (P) smem layout must equal PV's");
  static_assert(cute::is_same_v<typename CollectiveMmaPV64::SmemLayoutSFA, typename CollectiveMmaPV::SmemLayoutSFA>,
                "[OSPLIT] PV64 SFA (P-SF) smem layout must equal PV's");
#else
  using SmemLayoutAtomSFP = typename CollectiveMmaPV::SmemLayoutAtomSFA;  // P-SF atom
  using SmemLayoutAtomSFV = typename CollectiveMmaPV::SmemLayoutAtomSFB;  // V-SF atom
  // [PVMX 2b] P-SF smem is double-buffered (softmax(k) writes while UTCCP(k-1) reads).
  using SmemLayoutSFP = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFA{}, Int<2>{}));
  // [PVMX 2a.1b] V-SF smem, staged on KV pipeline (loaded by TMA on V-slots).
  using SmemLayoutSFV = decltype(unstageSmemLayout(typename CollectiveMmaPV::SmemLayoutSFB{}, Int<StageCountKV>{}));
#endif

  // [MXFP8] SF smem layouts, re-staged onto the FMHA Q / KV pipelines.
  // [M3 sub-tile] ONE SFK slot per KV stage: the IsCtaN64 gmem view is a
  // stride-0 pair (sub0/sub1 boxes are IDENTICAL — each carries the FULL
  // 128-row SF atom, 512B). One TMA copy + one UTCCP per tile serves BOTH
  // sub-MMAs; the odd sub reads the SAME TMEM slot at +2 columns (the proven
  // `sfb_parity` trick from the original verified Route-C N=64 mainloop).
#if defined(MXFP8_2SM_N128SINGLE)
  // [刀27 N128SINGLE] SFQ/SFK smem from the N128 collective (oyhj mainloop:217-218).
  // The SFK smem per-stage size is unchanged (full 128-row SF atom); only the
  // collective source differs (the QK64 stride-0 sub0/sub1 pair collapses to the
  // single N128 box). ONE TMA copy + ONE UTCCP per tile, no sfb_parity.
  using SmemLayoutSFQ = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFA{}, Int<StageCountQ>{}));
  using SmemLayoutSFK = decltype(unstageSmemLayout(typename CollectiveMmaQK::SmemLayoutSFB{}, Int<StageCountKV>{}));
#else
  using SmemLayoutSFQ = decltype(unstageSmemLayout(typename CollectiveMmaQK64::SmemLayoutSFA{}, Int<StageCountQ>{}));
  using SmemLayoutSFK = decltype(unstageSmemLayout(typename CollectiveMmaQK64::SmemLayoutSFB{}, Int<StageCountKV>{}));
#endif

  // Reuse shared memory for V and O.
  static constexpr bool IsOrderLoadEpilogue = std::is_same_v<OrderLoadEpilogue, cute::true_type>;
  struct TensorStorage {
    cute::array_aligned<Element, cute::cosize_v<SmemLayoutQ>> smem_q;
    union {
      cute::array_aligned<Element, cute::cosize_v<SmemLayoutK>> smem_k;
      cute::array_aligned<Element, cute::cosize_v<SmemLayoutV>> smem_v;
    };
    // [MXFP8] scale-factor smem — one slot per Q / KV pipeline stage.
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFQ>> smem_sfq;
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFK>> smem_sfk;
    // [PVMX 2a.0] P data buffer in smem (e4m3), written by softmax, read by PV SS MMA.
    cute::array_aligned<Element, cute::cosize_v<SmemLayoutP>> smem_p;
    // [PVMX 2b] P-SF smem, double-buffered (softmax writes per tile, UTCCP stages per tile).
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFP>> smem_sfp;
    // [PVMX 2a.1b] V-SF smem, staged on KV pipeline (TMA loads per V-slot).
    cute::array_aligned<ElementSF, cute::cosize_v<SmemLayoutSFV>> smem_sfv;
    // [M3-2] cross-group softmax exchange: [0][row] = the running global
    // online-max chain m_j; [1][row] + both rows reused by the final (m,l)
    // merge. MUST live here (one allocation): a function-static __shared__
    // inside softmax_step is PER-TEMPLATE-INSTANCE (mask/no-mask are two
    // instantiations) — the chain state would break crossing into the masked
    // tail (the partial-tile FAIL bug found 2026-06-10).
#ifdef MXFP8_SM12
    // [SM12] 3 slots × 128 rows: [0] = lazy-chain publish (G0), [1]/[2] =
    // G1/G2 per-step slice-max publishes (full-width lag-1 chain coverage)
    // doubling as the 3-way final row_sum exchange slots. +512 B smem.
    cute::array_aligned<float, 384> smem_softmax_exch;
#else
    cute::array_aligned<float, 256> smem_softmax_exch;
#endif
  };

  // [MXFP8 N128 M2] single-stage, DOUBLE-buffered S TMEM map — QK N-tile 128.
  // Two QK accumulators S0/S1 (128 cols each) so QK(k+1)→S[(k+1)%2] can run
  // ahead while softmax reads S[k%2] — recovers the QK/softmax overlap that
  // single-buffered milestone-1 lost. One PV accumulator O (single M=128
  // subtile). One SFA (Q is fixed for all KV tiles → one shared slot); two SFB
  // (K changes per tile and QK(k)/QK(k+1) overlap, so the K scale-factor TMEM
  // operand must ping-pong too).
  //   S0=0..127  S1=128..255  O0=256..383
  //   SFA0=384..415  SFB0=416..447  SFB1=448..479   kEnd=480  (<=512)
  // P / V-stats are embedded in the S buffers: P0=S0+32, P1=S1+32 (e4m3 P tile
  // for the PV GEMM); V0=S0, V1=S1 (softmax→correction row stats, first 2 cols).
  // O1 aliases O0 — the *_1 name is still referenced inside the static
  // `stage==_0 ? _0 : _1` ternary in correction_epilogue, never the live branch
  // (there is only one O).
  // [M3 sub-tile] S is now written as 4 x 64-col sub-slots by the 2 x (M256xN64)
  // sub-MMAs — but the COLUMN MAP IS UNCHANGED: tile-buffer b's sub0 lands at
  // b*128 (g0's read half) and sub1 at b*128+64 (g1's +nHalf half). SFB also
  // unchanged (2 buffers): each holds the FULL 128-row K-SF atom; the odd
  // sub-MMA reads the same buffer at +2 columns (original Route-C sfb_parity).
  enum class TmemAllocation : uint32_t {
    kSizeS = 128,
    kSizeO = 128,
    kSizeP = 32,                 // tilePlikeFP32 = N(128)/sizeof(float)*sizeof(Element)
    kSizeSF = 16,                // [PVMX 2a.1] tightened 32->16 (real SF footprint=4) to fit PV SF slots
    kSizeSub = 64,               // [M3 sub-tile] 64-col S sub-slot stride
    S0 = 0,
    S1 = S0 + kSizeS,            // [M2] second QK accumulator buffer
    V0 = S0,                     // stats storage from softmax to correction
    V1 = S1,
    P0 = S0 + kSizeP,
    P1 = S1 + kSizeP,
    O0 = S1 + kSizeS,
    SFA0 = O0 + kSizeO,          // [MXFP8] SFA (Q scale factors, shared)
    SFB0 = SFA0 + kSizeSF,       // [MXFP8] SFB (K scale), buffer 0 — holds the FULL
                                 //   128-row SF atom; sub1 reads +2 cols (sfb_parity)
    SFB1 = SFB0 + kSizeSF,       // [MXFP8] SFB, buffer 1
    SFP0 = SFB1 + kSizeSF,       // [PVMX 2b] P-SF (PV SFA), buffer 0 — double-buffered like SFB(K)
    SFP1 = SFP0 + kSizeSF,       // [PVMX 2b] P-SF, buffer 1
    SFV0 = SFP1 + kSizeSF,       // [PVMX 2a.1b] V-SF (PV SFB), buffer 0 — double-buffered per-tile
    SFV1 = SFV0 + kSizeSF,       // [PVMX 2a.1b] V-SF, buffer 1
    kEnd = SFV1 + kSizeSF,       // = 496 <= 512
    O1 = O0,                     // [N128] alias (single O; *_1 never live)
    SFA1 = SFA0
  };
  static_assert(uint32_t(TmemAllocation::kEnd) <= 512, "TMEM 512-col budget exceeded");

  // indices for V0 / V1
  enum : int {
    kIdxOldRowMax = 0,
    kIdxNewRowMax = 1,
    kIdxFinalRowSum = 0,
    kIdxFinalRowMax = 1
  };

  // from load to mma warp, protects q in smem
  // [2SM M1a] pass ClusterShape explicitly (3-arg form); previously the 2-arg
  // form put AtomThrShape<1,1,1> into the ClusterShape slot — fine only while
  // both were <1,1,1>. With ClusterShape<2,1,1> the slot must be the real cluster.
  using PipelineQ = cutlass::PipelineTmaUmmaAsync<
    StageCountQ,
    ClusterShape,
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;

  // from load to mma warp, protects k/v in smem
  using PipelineKV = cutlass::PipelineTmaUmmaAsync<
    StageCountKV,
    ClusterShape,
    typename CollectiveMmaQK::AtomThrShapeMNK
  >;

  // from mma to softmax0/1 warp, protects S in tmem
  // [MXFP8 N128 M2] depth 2 — double-buffered S. The single softmax warp group
  // consumes ONE pipeline (pipeline_mma_s0); depth 2 maps its two stages onto
  // the two TMEM buffers S0/S1, selected by PipelineState::index(). QK(k)
  // commits stage k%2; softmax_step(k) / PV read the same buffer via index().
  // [2SM M2] pass the MMA AtomThrShape so the UMMA→softmax pipeline is 2-SM-aware:
  // the leader's cta_group::2 QK MMA producer_commit then arrives on BOTH CTAs'
  // consumer barriers (calculate_umma_peer_mask) → BOTH CTAs' softmax get S and
  // produce their P half → leader's PV MMA (needs both P) no longer hangs.
  // 1-SM: AtomThrShapeMNK==<1,1,1> => default behavior (unchanged).
  using PipelineS = cutlass::PipelineUmmaAsync<2, typename CollectiveMmaQK::AtomThrShapeMNK>;

  // from softmax0/1/ to correction wg
  using PipelineC = cutlass::PipelineAsync<1>;

  // from mma to correction
  // [MXFP8 N128] depth 1, NOT 2. The dual-stage design has TWO O accumulators
  // (O0/O1, one per M=128 subtile) so depth-2 PipelineO ⟺ 2 physical buffers.
  // The single-stage variant has ONE O accumulator (TmemAllocation::O0, with O1
  // aliased to it). With depth 2 the mma producer runs 2 PVs ahead: PV(i+1)'s
  // producer_acquire only waits for correction to release O #(i-1), so PV(i+1)
  // accumulates into the single O before correction has rescaled/consumed
  // PV(i)'s result — a WAR race on O (~1% corrupted outputs). Depth 1 forces
  // PV(i+1) to wait until correction has consumed PV(i)'s O.
#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] depth 2, ONE pipeline: stage index == O half index. Per tile the
  // producer (PV) does acquire/commit TWICE (h0 then h1), the consumer (correction)
  // wait/rescale/release TWICE — the state sequence 0,1,0,1,... maps even states to
  // O[0:64) and odd states to O[64:128). Each half is still effectively depth-1
  // double-buffer-free (acquire(h,t) waits release(h,t-1)) so the WAR race that
  // killed plain depth-2 (single shared O buffer) CANNOT occur: the two stages
  // protect two PHYSICALLY DISJOINT 64-col halves.
  using PipelineO = cutlass::PipelineUmmaAsync<2, typename CollectiveMmaQK::AtomThrShapeMNK>;
#else
  using PipelineO = cutlass::PipelineUmmaAsync<1, typename CollectiveMmaQK::AtomThrShapeMNK>;  // [2SM M2] 2-SM-aware UMMA pipeline
#endif

  // from corr to epilogue
  using PipelineE = cutlass::PipelineAsync<2>;

#ifdef MXFP8_SM12
  // [SM12] 3-group ordered chain: g0 (publish w_k, read m1/m2) -> g1 (read
  // w_k, publish m1) -> g2 (read w_k, publish m2) -> g0(next round). Each
  // group does EXACTLY one wait/arrive pair per tile (count discipline
  // identical to the 2-group barrier; group 0 still starts opposite-phase).
  using OrderBarrierSoftmax = cutlass::OrderedSequenceBarrier<
    /*stages*/ 1, /*groups*/ 3>;
#else
  using OrderBarrierSoftmax = cutlass::OrderedSequenceBarrier<
    /*stages*/ 1, /*groups*/ 2>;
#endif

  // [MXFP8] Q-pipeline transaction = Q tile bytes + SFQ tile bytes.
  static const int TransactionBytesLoadQ_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutQ{})) * cute::sizeof_bits_v<Element>);
  static const int TransactionBytesLoadSFQ   = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutSFQ{})) * cute::sizeof_bits_v<ElementSF>);
  static const int TransactionBytesLoadQ     = TransactionBytesLoadQ_data + TransactionBytesLoadSFQ;

  // [MXFP8] KV-pipeline transaction = K|V tile bytes + SF-sized payload (real
  // K scale factors on K-slots, real V-SF on V-slots) — kept equal.
  // [M3 sub-tile] a K stage delivers TWO 32-row K sub-tiles + ONE 512B SF atom
  // (= the full 128-row SF, shared by both sub-MMAs); V stage = V + SFV. Equal.
#if defined(MXFP8_2SM_N128SINGLE)
  // [刀27 N128SINGLE] SmemLayoutK is now ONE slot = full per-CTA 64-N K tile,
  // so cosize already covers the whole stage (drop the QK64 2x sub-slot factor).
  // Same byte total as the OFF path (2 x 32-N == 64-N). (oyhj mainloop:351.)
  static const int TransactionBytesLoadK_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<Element>);
#else
  static const int TransactionBytesLoadK_data = 2 * cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutK{})) * cute::sizeof_bits_v<Element>);
#endif
#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] a V stage = TWO 32-D-row V sub-tiles (same total bytes; K mirror).
  static const int TransactionBytesLoadV_data = 2 * cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutV{})) * cute::sizeof_bits_v<Element>);
#else
  static const int TransactionBytesLoadV_data = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutV{})) * cute::sizeof_bits_v<Element>);
#endif
  static const int TransactionBytesLoadSFK   = cutlass::bits_to_bytes(cosize(take<0,3>(SmemLayoutSFK{})) * cute::sizeof_bits_v<ElementSF>);
  static const int TransactionBytesLoadK     = TransactionBytesLoadK_data + TransactionBytesLoadSFK;
  static const int TransactionBytesLoadV     = TransactionBytesLoadV_data + TransactionBytesLoadSFK;

  static_assert(TransactionBytesLoadK == TransactionBytesLoadV, "K and V smem layouts must be of equal size");

  // [MXFP8] SF-aware load collective (file 3).
  // [M3 sub-tile] the load partitions K/SFB (and Q/SFA — N-independent) against
  // the N=64 sub-MMA collective: 2 TMA copies per K stage into sub-slots 2s/2s+1.
  using Load = Sm100FmhaLoadTmaWarpspecializedMxfp8<
    Element, ElementSF, StrideQ, StrideK, StrideV,
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] the load partitions V/SFV against the N=64 PV sub-MMA
    // collective: 2 TMA copies per V stage into sub-slots 2s/2s+1 (K mirror).
    CollectiveMmaQK64, CollectiveMmaPV64,
#elif defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] the load partitions K/SFB against the N128 QK collective:
    // ONE TMA copy per K stage (per-CTA B = 64-N from the 2-SM atom). V is
    // whole-tile (non-OSPLIT). (oyhj load passes its single CollectiveMmaQK.)
    CollectiveMmaQK, CollectiveMmaPV,
#else
    CollectiveMmaQK64, CollectiveMmaPV,
#endif
    SmemLayoutQ, SmemLayoutK, SmemLayoutV,
    SmemLayoutSFQ, SmemLayoutSFK, SmemLayoutSFV,
    TensorStorage, PipelineQ, PipelineKV, Mask, TileShape,
    ClusterShape   // [2SM] for TMA multicast masks in the load
  >;

  struct Arguments {
    typename Load::Arguments load;

    // if zero, defaults to 1/sqrt(D)
    float scale_softmax = 0.0f;

    // scaling factors to dequantize QKV
    float scale_q = 1.0f;
    float scale_k = 1.0f;
    float scale_v = 1.0f;

    // scaling factor to quantize O
    float inv_scale_o = 1.0f;
  };

  struct Params {
    typename Load::Params load;

    float scale_softmax;
    float scale_softmax_log2;

    float scale_output;
  };

  template<class ProblemShape>
  static bool can_implement(ProblemShape const& problem_shape, Arguments const& args) {
    return true;
  }

  template<class ProblemShape>
  static Params to_underlying_arguments(
      ProblemShape const& problem_shape,
      Arguments const& args,
      void* workspace) {

    float scale_softmax = args.scale_softmax;
    if (scale_softmax == 0.0f) {
      scale_softmax = 1.0f / (float) std::sqrt(get<2>(problem_shape));
    }
    float log2_e = static_cast<float>(std::log2(std::exp(1.0)));

    return Params{
        Load::to_underlying_arguments(problem_shape, args.load, workspace),
        args.scale_q * args.scale_k * scale_softmax,
        args.scale_q * args.scale_k * log2_e * scale_softmax,
        args.scale_v * args.inv_scale_o
    };
  }

  CUTLASS_DEVICE
  static void prefetch_tma_descriptors(Params const& params) {
      Load::prefetch_tma_descriptors(params.load);
  }

  template<class BlkCoord, class ProblemShape, class ParamsProblemShape>
  CUTLASS_DEVICE void
  load(
      BlkCoord const& blk_coord, ProblemShape const& problem_shape,
      Params const& params, ParamsProblemShape const& params_problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_producer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_producer_state) {

    Load load;
    load.load(blk_coord, problem_shape, params.load, params_problem_shape,
        storage,
        pipeline_q, pipeline_q_producer_state,
        pipeline_kv, pipeline_kv_producer_state);
  }




  template<class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  mma(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,
      PipelineQ& pipeline_q, typename PipelineQ::PipelineState& pipeline_q_consumer_state,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_consumer_state,
      PipelineS& pipeline_s0, typename PipelineS::PipelineState& pipeline_s0_producer_state,
      PipelineS& pipeline_s1, typename PipelineS::PipelineState& pipeline_s1_producer_state,
      PipelineO& pipeline_corr, typename PipelineO::PipelineState& pipeline_corr_producer_state) {

    BCN(1);   // [2SM] MMA warp entered mma()
#ifdef MXFP8_2SM_CLUSTERCHK
    if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && cute::elect_one_sync()) {
      unsigned nctarank, ctarank, smid;
      asm volatile("mov.u32 %0, %%cluster_nctarank;" : "=r"(nctarank));
      asm volatile("mov.u32 %0, %%cluster_ctarank;" : "=r"(ctarank));
      asm volatile("mov.u32 %0, %%smid;" : "=r"(smid));
      cute::print("[CLCHK] leader blockIdx=%d cluster_nctarank=%u cluster_ctarank=%u smid=%u block_rank_in_cluster=%u AtomThrID=%d\n",
                  (int)blockIdx.x, nctarank, ctarank, smid, (unsigned)cute::block_rank_in_cluster(),
                  (int)cute::size(typename CollectiveMmaQK::TiledMma::AtomThrID{}));
    }
#endif
#if defined(MXFP8_2SM_BEACON)
    { unsigned smid; asm volatile("mov.u32 %0, %%smid;" : "=r"(smid));
      if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+8], smid+1u); __threadfence_system(); } } }
#endif
    auto pipeline_q_release_state = pipeline_q_consumer_state;
    auto pipeline_kv_release_state = pipeline_kv_consumer_state;

    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    // [M3 sub-tile] QK issues as 2 x (M256xN64) sub-MMAs per 128-row KV tile,
    // each committing its own PipelineS (sub0 -> s0/g0, sub1 -> s1/g1).
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] single N128 QK MMA per 128-row KV tile, commits s0 only
    // (oyhj mainloop:464). per-CTA tile = M256 x N64 (2-SM atom halving N=128).
    typename CollectiveMmaQK::TiledMma mma_qk;
#else
    typename CollectiveMmaQK64::TiledMma mma_qk;
#endif
    ThrMMA thr_mma_qk = mma_qk.get_slice(0);

    // [PVMX 2a.0] PV uses the plain SS atom directly (P read from smem, not TMEM).
    // Dropping to_tiled_mma_sm100_ts is the first half of moving toward the
    // block-scaled SS PV (which is SS-only — no TS form exists).
#ifdef MXFP8_OSPLIT
    typename CollectiveMmaPV64::TiledMma mma_pv;   // [刀12] N=64 PV sub-MMA atom
#else
    typename CollectiveMmaPV::TiledMma mma_pv;
#endif
    ThrMMA thr_mma_pv = mma_pv.get_slice(0);

    Tensor sQ = make_tensor(make_smem_ptr(storage.smem_q.data()), SmemLayoutQ{});
    Tensor sK = make_tensor(make_smem_ptr(storage.smem_k.data()), SmemLayoutK{});
    Tensor sV = make_tensor(make_smem_ptr(storage.smem_v.data()), SmemLayoutV{});

    Tensor tSrQ = thr_mma_qk.make_fragment_A(sQ);
#ifdef MXFP8_DBG
    if (false && blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && threadIdx.x==384) {
      cute::print("[QOP] SmemLayoutQ="); cute::print(SmemLayoutQ{}); cute::print("\n");
      cute::print("[QOP] sQ="); cute::print(sQ.layout()); cute::print("\n");
      cute::print("[QOP] tSrQ="); cute::print(tSrQ.layout()); cute::print("\n");
      cute::print("[QOP] QK AtomThrID="); cute::print(typename CollectiveMmaQK::TiledMma::AtomThrID{}); cute::print("\n");
      cute::print("[QOP] QK TiledMma="); cute::print(mma_qk); cute::print("\n");
    }
#endif
    Tensor tSrK = thr_mma_qk.make_fragment_B(sK);
    Tensor tOrV = thr_mma_pv.make_fragment_B(sV);

    // tmem layout: S0 S1 O0, S overlaps with P and V
    // [M3 sub-tile] C fragment for ONE N=64 sub-MMA: per-CTA (128, 64), N
    // contiguous (stride 1). Sub-slot column base = (buf? S1:S0) + sub*64 —
    // EXACTLY the columns softmax g0/g1 already read (s_base / s_base+nHalf).
    // (The 续19x MMATILE_C M=256-partition experiment was removed — proven no-op.)
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] C fragment for the single N128 QK MMA: (256,128) ->
    // per-CTA (128,128). do_qk writes the whole 128-col S buffer at once; the
    // S column base is buf?S1:S0 (no sub offset). (oyhj mainloop:482.)
    Tensor tStS = partition_fragment_C(mma_qk, select<0,1>(TileShapeQK{}));
#else
    Tensor tStS = partition_fragment_C(mma_qk, make_shape(get<0>(TileShapeQK{}), _64{}));
#endif
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] C fragment for ONE N=64 PV sub-MMA: per-CTA (128, 64). Half
    // h's column base = O0 + h*64 — the two halves tile the original O exactly.
    Tensor tOtO = partition_fragment_C(mma_pv, make_shape(get<0>(TileShapePV{}), _64{}));
#else
    Tensor tOtO = partition_fragment_C(mma_pv, select<0,1>(TileShapePV{}));
#endif
#ifdef MXFP8_DBG
    if (false && blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && threadIdx.x==384 && cute::elect_one_sync()) {
      cute::print("[SHP] MmaTileShapeQK="); cute::print(MmaTileShapeQK{}); cute::print("\n");
      cute::print("[SHP] SmemLayoutQ(A)="); cute::print(SmemLayoutQ{}); cute::print("\n");
      cute::print("[SHP] SmemLayoutK(B)="); cute::print(SmemLayoutK{}); cute::print("\n");
      cute::print("[SHP] tStS="); cute::print(tStS.layout()); cute::print("\n");
      cute::print("[SHP] tSrQ(A frag)="); cute::print(tSrQ.layout()); cute::print(" tSrK(B frag)="); cute::print(tSrK.layout()); cute::print("\n");
      cute::print("[SHP] CollectiveMmaQK::SmemLayoutA="); cute::print(typename CollectiveMmaQK::SmemLayoutA{}); cute::print("\n");
      cute::print("[SHP] CollectiveMmaQK::SmemLayoutB="); cute::print(typename CollectiveMmaQK::SmemLayoutB{}); cute::print("\n");
      cute::print("[SHP] QK AtomThrID="); cute::print(typename CollectiveMmaQK::TiledMma::AtomThrID{}); cute::print("\n");
    }
#endif

    // [M3 sub-tile] 4 S sub-slots, addressed per (sub, buf) inside do_qk.
#ifdef MXFP8_DBG_COLOFF
    if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && threadIdx.x==384 && cute::elect_one_sync()) {
      Tensor _sfa = make_tensor<typename TiledMmaQK::FrgTypeSFA>(shape(SmemLayoutAtomSFA{}));
      Tensor _sfb = make_tensor<typename TiledMmaQK::FrgTypeSFB>(shape(SmemLayoutAtomSFB{}));
      Tensor _sfp = make_tensor<typename TiledMmaPV::FrgTypeSFA>(shape(SmemLayoutAtomSFP{}));
      Tensor _sfv = make_tensor<typename TiledMmaPV::FrgTypeSFB>(shape(SmemLayoutAtomSFV{}));
      cute::print("[COLOFF] tStS=%u tOtO=%u | coloff SFA=%u SFB=%u SFP=%u SFV=%u | enum S0=%u S1=%u O0=%u SFA0=%u SFB0=%u SFB1=%u SFP0=%u SFV0=%u kEnd=%u\n",
                  (unsigned)tStS.data().get(), (unsigned)tOtO.data().get(),
                  (unsigned)cutlass::detail::find_tmem_tensor_col_offset(_sfa),
                  (unsigned)cutlass::detail::find_tmem_tensor_col_offset(_sfb),
                  (unsigned)cutlass::detail::find_tmem_tensor_col_offset(_sfp),
                  (unsigned)cutlass::detail::find_tmem_tensor_col_offset(_sfv),
                  (unsigned)TmemAllocation::S0,(unsigned)TmemAllocation::S1,(unsigned)TmemAllocation::O0,
                  (unsigned)TmemAllocation::SFA0,(unsigned)TmemAllocation::SFB0,(unsigned)TmemAllocation::SFB1,
                  (unsigned)TmemAllocation::SFP0,(unsigned)TmemAllocation::SFV0,(unsigned)TmemAllocation::kEnd);
    }
#endif

    Tensor tOtO0 = tOtO;  tOtO0.data() = tOtO.data().get() + uint32_t(TmemAllocation::O0);

    // P is produced by softmax directly into TMEM P0/P1. PV issues the TS
    // block-scaled MMA form so A=P is read from TMEM, while B=V stays in SMEM.

    // ===================================================================
    // [MXFP8] block-scaled QK: scale-factor smem -> TMEM infrastructure.
    // [M2] one SFA operand (Q, fixed/shared), two SFB operands (K) so the
    // K scale-factor TMEM operand ping-pongs while QK(k)/QK(k+1) overlap.
    // ===================================================================
    Tensor sSFQ = make_tensor(make_smem_ptr(storage.smem_sfq.data()), SmemLayoutSFQ{});
    Tensor sSFK = make_tensor(make_smem_ptr(storage.smem_sfk.data()), SmemLayoutSFK{});

    // SF TMEM operands — the block-scaled UMMA reads SFA/SFB from TMEM.
    // [M3 sub-tile] fragments from the N=64 sub-MMA. SFB has 4 slots (sub x buf).
    // (续19ah ACCREL_SF / SFROOT_ACC dead experiments removed — both disproven,
    // see agent-memory op6-2sm-peer-cta-group2-cwrite.)
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] SFA/SFB TMEM fragments from the N128 atom (oyhj 504-505).
    Tensor tCtSFA = make_tensor<typename TiledMmaQK::FrgTypeSFA>(shape(SmemLayoutAtomSFA{}));
    Tensor tCtSFB = make_tensor<typename TiledMmaQK::FrgTypeSFB>(shape(SmemLayoutAtomSFB{}));
#else
    Tensor tCtSFA = make_tensor<typename TiledMmaQK64::FrgTypeSFA>(shape(SmemLayoutAtomSFA{}));
    Tensor tCtSFB = make_tensor<typename TiledMmaQK64::FrgTypeSFB>(shape(SmemLayoutAtomSFB{}));
#endif

    Tensor tCtSFA0 = tCtSFA;  tCtSFA0.data() = tCtSFA0.data().get() + uint32_t(TmemAllocation::SFA0);
    Tensor tCtSFB0 = tCtSFB;  tCtSFB0.data() = tCtSFB0.data().get() + uint32_t(TmemAllocation::SFB0);
    Tensor tCtSFB1 = tCtSFB;  tCtSFB1.data() = tCtSFB1.data().get() + uint32_t(TmemAllocation::SFB1);

    // UTCCP (tcgen05.cp) smem->TMEM copies, mirroring the block-scaled
    // GEMM collective's mma_init().
    // [2SM M2] must pick the 2-CTA UTCCP op when the MMA atom is 2-SM (AtomThrID==2),
    // exactly like the stock collective — a 1-CTA UTCCP under a 2-SM MMA deadlocks.
    using UtccpOp = cute::conditional_t<
        decltype(cute::size(typename CollectiveMmaQK64::TiledMma::AtomThrID{}) == cute::Int<2>{})::value,
        SM100_UTCCP_4x32dp128bit_2cta,
        SM100_UTCCP_4x32dp128bit_1cta>;
    auto tCtSFA0_c = make_tensor(tCtSFA0.data(), filter_zeros(tCtSFA0.layout()));
    auto tCtSFB0_c = make_tensor(tCtSFB0.data(), filter_zeros(tCtSFB0.layout()));
    auto tCtSFB1_c = make_tensor(tCtSFB1.data(), filter_zeros(tCtSFB1.layout()));

    auto utccp_SFA = make_utccp_copy(UtccpOp{}, tCtSFA0_c);
    auto utccp_SFB = make_utccp_copy(UtccpOp{}, tCtSFB0_c);

    auto sSFQ_c = make_tensor(sSFQ.data(), filter_zeros(sSFQ.layout()));
    auto sSFK_c = make_tensor(sSFK.data(), filter_zeros(sSFK.layout()));

    auto thr_utccp_SFA = utccp_SFA.get_slice(0);
    auto thr_utccp_SFB = utccp_SFB.get_slice(0);

    auto sfq_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFA.partition_S(sSFQ_c));
    auto sfk_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFB.partition_S(sSFK_c));
    auto sfa0_s2t_dst = thr_utccp_SFA.partition_D(tCtSFA0_c);
    auto sfb0_s2t_dst = thr_utccp_SFB.partition_D(tCtSFB0_c);
    auto sfb1_s2t_dst = thr_utccp_SFB.partition_D(tCtSFB1_c);
#ifdef MXFP8_DBG
    // [续19ac] SFAGEO cute::print disabled (SLOW -> 60s run timeout). Layouts captured:
    //   SmemLayoutAtomSFA M-mode ((_32,_4),_1)=M=128 per CTA (stock deduce_smem_layoutSFA
    //   MMA_M = TileShape.M/ThrLayoutVMNK.V = 256/2 = 128, SAME as stock). UTCCP src
    //   sfq_s2t_src M=128; dst sfa0_s2t_dst has cta_group::2 TMEM CTA offset 0x800000.
    if (false && blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && threadIdx.x==384 && cute::elect_one_sync()) {
      cute::print("[SFAGEO] SmemLayoutAtomSFA="); cute::print(SmemLayoutAtomSFA{}); cute::print("\n");
      cute::print("[SFAGEO] SmemLayoutSFQ="); cute::print(SmemLayoutSFQ{}); cute::print("\n");
      cute::print("[SFAGEO] tCtSFA(dst frag)="); cute::print(tCtSFA.layout()); cute::print("\n");
      cute::print("[SFAGEO] sSFQ_c(src)="); cute::print(sSFQ_c.layout()); cute::print("\n");
      cute::print("[SFAGEO] sfq_s2t_src="); cute::print(sfq_s2t_src.layout()); cute::print("\n");
      cute::print("[SFAGEO] sfa0_s2t_dst="); cute::print(sfa0_s2t_dst.layout()); cute::print("\n");
      cute::print("[SFAGEO] UtccpOp is 2cta? AtomThrID="); cute::print(typename CollectiveMmaQK::TiledMma::AtomThrID{}); cute::print("\n");
    }
    if (false) {
      cute::print("[SFGEO2] SmemLayoutAtomSFB="); cute::print(SmemLayoutAtomSFB{}); cute::print("\n");
      cute::print("[SFGEO2] tCtSFB.layout="); cute::print(tCtSFB.layout()); cute::print("\n");
      cute::print("[SFGEO2] SmemLayoutSFK="); cute::print(SmemLayoutSFK{}); cute::print("\n");
      cute::print("[SFGEO2] sSFK_c.layout="); cute::print(sSFK_c.layout()); cute::print("\n");
      cute::print("[SFGEO2] sfk_s2t_src.layout="); cute::print(sfk_s2t_src.layout()); cute::print("\n");
      cute::print("[SFGEO2] sfb0_s2t_dst.layout="); cute::print(sfb0_s2t_dst.layout()); cute::print("\n");
      cute::print("[SFGEO2] col_off SFA="); cute::print(cutlass::detail::find_tmem_tensor_col_offset(tCtSFA));
      cute::print(" SFB="); cute::print(cutlass::detail::find_tmem_tensor_col_offset(tCtSFB));
      cute::print("  (our kSizeSF=16)\n");
      cute::print("[SFGEO2] CollMmaQK::SmemLayoutB="); cute::print(typename CollectiveMmaQK::SmemLayoutB{}); cute::print("\n");
      cute::print("[SFGEO2] SmemLayoutK="); cute::print(SmemLayoutK{}); cute::print("\n");
      cute::print("[SFGEO2] sK.layout="); cute::print(sK.layout()); cute::print("\n");
      cute::print("[SFGEO2] tSrK.layout="); cute::print(tSrK.layout()); cute::print("\n");
      cute::print("[SFGEO2] tStS.layout="); cute::print(tStS.layout()); cute::print("\n");
      cute::print("[SFGEO2] SmemLayoutV="); cute::print(SmemLayoutV{}); cute::print("\n");
      cute::print("[SFGEO2] tOrV.layout="); cute::print(tOrV.layout()); cute::print("\n");
    }
#endif
    // ===================================================================

    // ===================================================================
    // [PVMX 2a.1b/2b] block-scaled PV SF: SFP (P, online) and SFV (V, from gmem)
    // both double-buffered + per-tile UTCCP. V-SF is staged by the load TMA into
    // smem_sfv (KV pipeline); SFP is computed by softmax into smem_sfp.
    // ===================================================================
    Tensor sSFP_full = make_tensor(make_smem_ptr(storage.smem_sfp.data()), SmemLayoutSFP{});
    Tensor sSFV_full = make_tensor(make_smem_ptr(storage.smem_sfv.data()), SmemLayoutSFV{});
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] PV SF fragments come from the N=64 sub-MMA (QK64 SFB mirror).
    Tensor tCtSFP = make_tensor<typename TiledMmaPV64::FrgTypeSFA>(shape(SmemLayoutAtomSFP{}));
    Tensor tCtSFV = make_tensor<typename TiledMmaPV64::FrgTypeSFB>(shape(SmemLayoutAtomSFV{}));
#else
    Tensor tCtSFP = make_tensor<typename TiledMmaPV::FrgTypeSFA>(shape(SmemLayoutAtomSFP{}));
    Tensor tCtSFV = make_tensor<typename TiledMmaPV::FrgTypeSFB>(shape(SmemLayoutAtomSFV{}));
#endif
    // (续19ah ACCREL_SF / SFROOT_ACC dead experiments removed.)
    Tensor tCtSFP0 = tCtSFP;  tCtSFP0.data() = tCtSFP0.data().get() + uint32_t(TmemAllocation::SFP0);
    Tensor tCtSFP1 = tCtSFP;  tCtSFP1.data() = tCtSFP1.data().get() + uint32_t(TmemAllocation::SFP1);
    Tensor tCtSFV0 = tCtSFV;  tCtSFV0.data() = tCtSFV0.data().get() + uint32_t(TmemAllocation::SFV0);
    Tensor tCtSFV1 = tCtSFV;  tCtSFV1.data() = tCtSFV1.data().get() + uint32_t(TmemAllocation::SFV1);
    auto tCtSFP0_c = make_tensor(tCtSFP0.data(), filter_zeros(tCtSFP0.layout()));
    auto tCtSFP1_c = make_tensor(tCtSFP1.data(), filter_zeros(tCtSFP1.layout()));
    auto tCtSFV0_c = make_tensor(tCtSFV0.data(), filter_zeros(tCtSFV0.layout()));
    auto tCtSFV1_c = make_tensor(tCtSFV1.data(), filter_zeros(tCtSFV1.layout()));
    auto utccp_SFP = make_utccp_copy(UtccpOp{}, tCtSFP0_c);
    auto utccp_SFV = make_utccp_copy(UtccpOp{}, tCtSFV0_c);
    auto sSFP_c  = make_tensor(sSFP_full.data(), filter_zeros(sSFP_full.layout()));
    auto sSFV_c  = make_tensor(sSFV_full.data(), filter_zeros(sSFV_full.layout()));
    auto thr_utccp_SFP = utccp_SFP.get_slice(0);
    auto thr_utccp_SFV = utccp_SFV.get_slice(0);
    auto sfp_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFP.partition_S(sSFP_c));
    auto sfv_s2t_src = get_utccp_smem_desc_tensor<UtccpOp>(thr_utccp_SFV.partition_S(sSFV_c));
    auto sfp0_s2t_dst = thr_utccp_SFP.partition_D(tCtSFP0_c);
    auto sfp1_s2t_dst = thr_utccp_SFP.partition_D(tCtSFP1_c);
    auto sfv0_s2t_dst = thr_utccp_SFV.partition_D(tCtSFV0_c);
    auto sfv1_s2t_dst = thr_utccp_SFV.partition_D(tCtSFV1_c);
#ifdef MXFP8_DBG
    if (false) {
      cute::print("[SFGEO3] col_off SFP="); cute::print(cutlass::detail::find_tmem_tensor_col_offset(tCtSFP));
      cute::print(" SFV="); cute::print(cutlass::detail::find_tmem_tensor_col_offset(tCtSFV));
      cute::print("  (kSizeSF=16; SFP0..SFP1 spacing=16, SFV0..SFV1 spacing=16)\n");
      cute::print("[SFGEO3] SmemLayoutSFP="); cute::print(SmemLayoutSFP{}); cute::print("\n");
      cute::print("[SFGEO3] sfp_s2t_src.layout="); cute::print(sfp_s2t_src.layout()); cute::print("\n");
      cute::print("[SFGEO3] sfp1_s2t_dst.layout="); cute::print(sfp1_s2t_dst.layout()); cute::print("\n");
      cute::print("[SFGEO3] sfv1_s2t_dst.layout="); cute::print(sfv1_s2t_dst.layout()); cute::print("\n");
    }
#endif
    // [PVMX 2b] per-tile P-SF UTCCP (mirror load_sfb): smem_sfp[buf] -> SFP[buf] TMEM.
    auto load_sfp = [&](int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFP, sfp_s2t_src(_,_,_,_,0), sfp0_s2t_dst);
        else          copy(utccp_SFP, sfp_s2t_src(_,_,_,_,1), sfp1_s2t_dst);
      }
    };
    // [PVMX 2a.1b] per-tile V-SF UTCCP: smem_sfv[vidx] -> SFV[buf] TMEM.
    auto load_sfv = [&](int vidx, int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFV, sfv_s2t_src(_,_,_,_,vidx), sfv0_s2t_dst);
        else          copy(utccp_SFV, sfv_s2t_src(_,_,_,_,vidx), sfv1_s2t_dst);
      }
    };
    // ===================================================================

    int k_index = 0;
    int v_index = 0;
    int q_index = 0;
#if defined(MXFP8_G_QKADDR) || defined(MXFP8_G_QKSFB) || defined(MXFP8_G_COMBO)
    // [G_QKADDR/QKSFB] loop-invariant-scope holders for the QK S-tile and SFB
    // TMEM column offsets. Declared here (before the do_qk lambda that reads).
    // Materialized (warp_uniform) at the TOP of each k-iter BEFORE the K(k)
    // wait + corr/s0 spins. OFF -> these lines vanish (byte-identical).
    uint32_t qk_s_uaddr   = 0u;
    uint32_t qk_sfb_uaddr = 0u;
#endif

    // wait for Q
    q_index = pipeline_q_consumer_state.index();
    pipeline_q.consumer_wait(pipeline_q_consumer_state);
    ++pipeline_q_consumer_state;

    Tensor tSrQ0 = tSrQ(_,_,_,q_index);
    BCN(2);   // [2SM] past Q consumer_wait

    int n = mask_tile_count;   // number of KV tiles

    // ---- helpers -----------------------------------------------------
    // SFA (Q scale factors) is loaded smem->TMEM exactly ONCE: Q is fixed for
    // all KV tiles, so a single SFA0 slot is reused — re-copying it per QK
    // would race the in-flight QK that reads it once QK tiles overlap.
    auto load_sfa = [&]() {
      if (cute::elect_one_sync())
        copy(utccp_SFA, sfq_s2t_src(_,_,_,_,q_index), sfa0_s2t_dst);
    };
    // SFB (K scale factors) ping-pongs by buf — QK(k)/QK(k+1) overlap, so the
    // K scale-factor TMEM operand must double-buffer like S. One UTCCP per
    // tile: the 512B SFK smem slot already holds the FULL 128-row SF atom.
    auto load_sfb = [&](int kidx, int buf) {
      if (cute::elect_one_sync()) {
        if (buf == 0) copy(utccp_SFB, sfk_s2t_src(_,_,_,_,kidx), sfb0_s2t_dst);
        else          copy(utccp_SFB, sfk_s2t_src(_,_,_,_,kidx), sfb1_s2t_dst);
      }
    };
    // [M3 sub-tile] issue QK sub-MMA (M256xN64): KV-stage kidx, sub-tile sub
    // (0 = kv cols 0-63, 1 = 64-127), S buffer buf. S column base lands exactly
    // where softmax g{sub} reads (buf*128 + sub*64). The odd sub reads SFB[buf]
    // at +2 columns (the original Route-C `sfb_parity`: the 128-row SF atom in
    // TMEM holds the odd 64 rows' SF at col offset 2). buf/sub warp-uniform.
#if defined(MXFP8_2SM_N128SINGLE)
    // [刀27 N128SINGLE] single N128 QK MMA: writes the WHOLE 128-col S buffer
    // (no sub offset), reads SFB[buf] at base (no sfb_parity +2), reads K at the
    // single stage slot tSrK(_,_,_,kidx). One UTCCP/tile. (oyhj mainloop:616-621.)
    auto do_qk = [&](int kidx, int buf) {
      Tensor tStSk   = tStS;
      Tensor tCtSFBk = tCtSFB;
#if defined(MXFP8_G_QKADDR) || defined(MXFP8_G_COMBO)
      // [G_QKADDR] use BOTH S+SFB TMEM base offsets precomputed at the loop top
      // (depend only on `buf`). Identical warp_uniform()+ADD multiset; the URF
      // address latency overlapped with the K-wait + corr/s0 spins instead of
      // sitting on the post-K-wait path to the QK MMA. MMA reads the SAME
      // tStSk/tCtSFBk; bit-exact.
      tStSk.data()   = tStS.data().get()   + qk_s_uaddr;
      tCtSFBk.data() = tCtSFB.data().get() + qk_sfb_uaddr;
#elif defined(MXFP8_G_QKSFB)
      // [G_QKSFB] decomposition probe: hoist ONLY the SFB offset (S stays inline)
      // to isolate which operand-address hoist drives any perf delta.
      tStSk.data()   = tStS.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::S1 : TmemAllocation::S0));
      tCtSFBk.data() = tCtSFB.data().get() + qk_sfb_uaddr;
#else
      tStSk.data()   = tStS.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::S1 : TmemAllocation::S0));
      tCtSFBk.data() = tCtSFB.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
#endif
      load_sfb(kidx, buf);
      gemm_bs(mma_qk, /*zero_acc=*/true, tSrQ0, tSrK(_,_,_, kidx), tStSk, tCtSFA0, tCtSFBk);
    };
#else
    auto do_qk = [&](int kidx, int sub, int buf) {
      Tensor tStSk   = tStS;
      tStSk.data()   = tStS.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::S1 : TmemAllocation::S0)
                         + uint32_t(sub) * uint32_t(TmemAllocation::kSizeSub));
      Tensor tCtSFBk = tCtSFB;
      tCtSFBk.data() = tCtSFB.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0)
                         + uint32_t(sub) * 2u);
      if (sub == 0) load_sfb(kidx, buf);   // one UTCCP serves both sub-MMAs
      // K data smem sub-slot index (K stays 2 sub-slots per stage; SF does not).
      gemm_bs(mma_qk, /*zero_acc=*/true, tSrQ0, tSrK(_,_,_, 2*kidx + sub), tStSk, tCtSFA0, tCtSFBk);
    };
#endif
#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] PV(t) issues as 2 x (M256xN64) sub-MMAs along D: sub h writes
    // O cols [h*64,(h+1)*64). P (A) + SFP are FULL-tile (N-independent); V (B)
    // reads smem sub-slot 2*vidx+h; SFV: sub1 reads SFV[buf] at +2 columns (the
    // proven sfb_parity trick — the 128-D-row SF atom holds the odd 64 rows' SF
    // at col offset 2). first_pv zero-inits each half's own columns.
    auto do_pv = [&](int vidx, int buf, bool first_pv, int sub) {
#ifdef MXFP8_DBG
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && cute::elect_one_sync() && g_dbg_npv < 8) {
        g_dbg_pv_vidx[g_dbg_npv] = vidx; g_dbg_pv_buf[g_dbg_npv] = buf; g_dbg_pv_first[g_dbg_npv] = first_pv?1:0; g_dbg_npv = g_dbg_npv + 1;
      }
#endif
      uint32_t p_tmem_base = warp_uniform(uint32_t(buf ? TmemAllocation::P1 : TmemAllocation::P0));
      Tensor tCtSFPk = tCtSFP;
      tCtSFPk.data() = tCtSFP.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFP1 : TmemAllocation::SFP0));
      Tensor tCtSFVk = tCtSFV;
      tCtSFVk.data() = tCtSFV.data().get()
          + warp_uniform(uint32_t(buf ? TmemAllocation::SFV1 : TmemAllocation::SFV0)
                         + uint32_t(sub) * 2u);
      Tensor tOtOk   = tOtO;
      tOtOk.data()   = tOtO.data().get()
          + warp_uniform(uint32_t(TmemAllocation::O0) + uint32_t(sub) * 64u);
      issue_pv_mxfp8_blockscaled_ts<kPV64Is2Sm>(
          mma_pv, /*zero_acc=*/first_pv, p_tmem_base,
          tOrV(_,_,_, 2*vidx + sub), tOtOk, tCtSFPk, tCtSFVk,
          /*bcn_pv=*/true);
    };
#else
    // [M3-2 LAZY-MAX] PV is WHOLE-TILE again: both P halves share one scale
    // w_k, so P[buf] x V(tile) issues as one block-scaled MMA with a single
    // per-tile correction rescale. first_pv zero-inits O.
    auto do_pv = [&](int vidx, int buf, bool first_pv) {
#ifdef MXFP8_DBG
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && cute::elect_one_sync() && g_dbg_npv < 8) {
        g_dbg_pv_vidx[g_dbg_npv] = vidx; g_dbg_pv_buf[g_dbg_npv] = buf; g_dbg_pv_first[g_dbg_npv] = first_pv?1:0; g_dbg_npv = g_dbg_npv + 1;
      }
#endif
      uint32_t p_tmem_base = warp_uniform(uint32_t(buf ? TmemAllocation::P1 : TmemAllocation::P0));
      Tensor tCtSFPk = tCtSFP;
      tCtSFPk.data() = tCtSFP.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFP1 : TmemAllocation::SFP0));
      Tensor tCtSFVk = tCtSFV;
      tCtSFVk.data() = tCtSFV.data().get() + warp_uniform(uint32_t(buf ? TmemAllocation::SFV1 : TmemAllocation::SFV0));
      issue_pv_mxfp8_blockscaled_ts<kPVIs2Sm>(
          mma_pv, /*zero_acc=*/first_pv, p_tmem_base,
          tOrV(_,_,_,vidx), tOtO0, tCtSFPk, tCtSFVk,
          /*bcn_pv=*/true);
    };
#endif
    // ------------------------------------------------------------------

    // ---- prologue: QK0 -> S0 -----------------------------------------
    // wait K0
    k_index = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;

#ifdef MXFP8_DBG
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&
        (threadIdx.x % 32) == 0) {
      auto* psfk = reinterpret_cast<uint8_t*>(storage.smem_sfk.data());
      auto* psfq = reinterpret_cast<uint8_t*>(storage.smem_sfq.data());
      for (int i = 0; i < 2048; ++i) { g_dbg_sfk[i] = psfk[i]; g_dbg_sfq[i] = psfq[i]; }
#if defined(MXFP8_DBG_LEADERSFQ)
      // [续19ac] LEADER smem_sfq M-coverage probe. The 2cta UTCCP reads LEADER's
      // smem and writes BOTH TMEMs (leader m0-127 + peer m128-255). If LEADER's
      // smem_sfq only holds m0-127 (self-only SFA TMA), the peer's TMEM half gets
      // zero -> peer S=0. firsthalf vs secondhalf nz distinguishes.
      {
        int total = (int)sizeof(storage.smem_sfq);
        int nz=0, nz_lo=0, nz_hi=0;
        for (int i=0;i<total;i++){ if(psfq[i]!=0){ nz++; if(i<total/2) nz_lo++; else nz_hi++; } }
        printf("[LEADERSFQ] total_bytes=%d nz=%d nz_firsthalf=%d nz_secondhalf=%d\n", total, nz, nz_lo, nz_hi);
      }
#endif
    }
#endif

    BCN(3);   // [2SM] past K0 consumer_wait, before load_sfa (UTCCP)
#if defined(MXFP8_DBG_PEERSMEM_AT_UTCCP)
    // [续19aj JUDGE] leader reads the PEER's smem_sfq[0] via a cluster remote-shared
    // load AT THE EXACT MOMENT before its cta_group::2 SFA UTCCP. Decides 续19ai vs ag:
    //   peer byte == 0x7F (with FORCE_PEERSFA127) -> peer smem READY before UTCCP =>
    //       the UTCCP does NOT read peer smem (续19ag right, timing not the issue).
    //   peer byte == 0x00 -> peer SFA smem NOT yet landed at UTCCP time (续19ai right,
    //       the sync gate is mis-placed -> fix = order leader UTCCP after peer SF land).
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && cute::elect_one_sync()) {
      uint32_t local_addr = cute::cast_smem_ptr_to_uint(storage.smem_sfq.data());
      // map this smem addr into PEER cta (rank 1)'s window via mapa.shared::cluster
      uint32_t peer_addr;
      asm volatile("mapa.shared::cluster.u32 %0, %1, %2;\n"
                   : "=r"(peer_addr) : "r"(local_addr), "r"(1u));
      uint32_t b0, b256;
      asm volatile("ld.shared::cluster.u32 %0, [%1];\n" : "=r"(b0) : "r"(peer_addr));
      asm volatile("ld.shared::cluster.u32 %0, [%1];\n" : "=r"(b256) : "r"(peer_addr + 256u));
      printf("[PEERSMEM@UTCCP] leader sees PEER smem_sfq[0..3]=0x%08x [256..259]=0x%08x\n", b0, b256);
    }
#endif
    // Q scale factors -> SFA0 TMEM, once (Q is fixed for all KV tiles).
    load_sfa();
    BCN(4);   // [2SM] past load_sfa UTCCP

    {
      int buf = pipeline_s0_producer_state.index();          // 0
#if defined(MXFP8_G_QKADDR) || defined(MXFP8_G_COMBO)
      // [G_QKADDR] seed the holders for the prologue QK0 (buf==0 -> S0/SFB0).
      // do_qk below reads them; without this they would be 0 and SFB0 (=416)
      // would be wrong. Same warp_uniform()+ADD as the legacy inline form.
      qk_s_uaddr   = warp_uniform(uint32_t(buf ? TmemAllocation::S1 : TmemAllocation::S0));
      qk_sfb_uaddr = warp_uniform(uint32_t(buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
#elif defined(MXFP8_G_QKSFB)
      qk_sfb_uaddr = warp_uniform(uint32_t(buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
#endif
#if defined(MXFP8_2SM_N128SINGLE)
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
      do_qk(k_index, buf);                                   // QK0 -> S[0:128)
      BCN(5);   // [2SM] past first QK 2-SM MMA (do_qk)
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
#endif
      do_qk(k_index, /*sub=*/0, buf);                        // QK0.sub0 -> S cols [0:64)
      BCN(5);   // [2SM] past first QK 2-SM MMA (do_qk)
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_commit(pipeline_s0_producer_state);  // g0 starts now
#endif
      ++pipeline_s0_producer_state;

#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s1.producer_acquire(pipeline_s1_producer_state);
#endif
      do_qk(k_index, /*sub=*/1, buf);                        // QK0.sub1 -> S cols [64:128)
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s1.producer_commit(pipeline_s1_producer_state);  // g1 starts now
#endif
      ++pipeline_s1_producer_state;
#endif
    }
    BCN(6);   // [2SM] past first QK producer_commit

    // wait V0 (held until PV0 in loop iter 1)
    int v_index_prev = pipeline_kv_consumer_state.index();
    pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
    ++pipeline_kv_consumer_state;

    // release K0  (KV release order == slot order: K0,V0,K1,V1,...)
    pipeline_kv.consumer_release(pipeline_kv_release_state);
    ++pipeline_kv_release_state;

    // acquire S sub-slots (buffer 1) for QK1 (loop iter 1). depth-2 acquire #2
    // is granted immediately (no prior occupant of buffer 1).
#if defined(MXFP8_2SM_N128SINGLE)
    pipeline_s0.producer_acquire(pipeline_s0_producer_state);
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s0.producer_acquire(pipeline_s0_producer_state);
    pipeline_s1.producer_acquire(pipeline_s1_producer_state);
#endif
#endif

    // ---- loop: iter k = 1..n-1 issues QK(k) sub0/sub1 -> S[k%2] and PV(k-1) --
    // [M3 sub-tile] QK(k) = 2 x N=64 sub-MMAs, each committing its own
    // PipelineS the moment its half of S is in flight — softmax g0 starts on
    // sub0 while the MMA is still computing sub1 (the FA4-style overlap).
    // PV(k-1) is gated by acquire #(k+2) on BOTH sub-pipelines, which (depth 2)
    // waits g0/g1 release #k ⟹ both P halves + SFP of tile k-1 are ready.
    bool first_pv = true;
    for (int k = 1; k < n; ++k) {
      BCN(7);   // [2SM] entered KV loop iter
      int qk_buf = pipeline_s0_producer_state.index();       // == k % 2 (s1 lockstep)
      int pv_buf = (k - 1) & 1;                              // == (k-1) % 2
#if defined(MXFP8_G_QKADDR) || defined(MXFP8_G_COMBO)
      // [G_QKADDR] precompute QK(k)'s S-tile + SFB TMEM column offsets HERE
      // (depend only on qk_buf, fixed at loop top) so the warp_uniform() URF
      // latency overlaps the K(k) wait AND the corr/s0 barrier-spins below,
      // rather than sitting on the post-K-wait path into do_qk. Pure reorder:
      // same warp_uniform()+ADD ops, issued earlier. Independent of K(k) data.
      qk_s_uaddr   = warp_uniform(uint32_t(qk_buf ? TmemAllocation::S1 : TmemAllocation::S0));
      qk_sfb_uaddr = warp_uniform(uint32_t(qk_buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
#elif defined(MXFP8_G_QKSFB)
      qk_sfb_uaddr = warp_uniform(uint32_t(qk_buf ? TmemAllocation::SFB1 : TmemAllocation::SFB0));
#endif

      // wait K(k)
      k_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;

#if defined(MXFP8_2SM_N128SINGLE)
      do_qk(k_index, qk_buf);
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
#else
      // QK(k).sub0 -> S[k%2][0:64), commit s0; .sub1 -> [64:128), commit s1
      do_qk(k_index, 0, qk_buf);
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
#endif
      ++pipeline_s0_producer_state;
      do_qk(k_index, 1, qk_buf);
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s1.producer_commit(pipeline_s1_producer_state);
#endif
      ++pipeline_s1_producer_state;
#endif

      // wait V(k) (held until PV(k) next iter)
      v_index = pipeline_kv_consumer_state.index();
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);
      ++pipeline_kv_consumer_state;
#if defined(MXFP8_M2_UTCCP) || defined(MXFP8_M2_COMBO)
      // [M2-A UTCCP-HOIST] issue the per-tile V-SF UTCCP NOW (smem_sfv ready
      // since the V-wait above returned; SFV[pv_buf] TMEM free since tile k-3).
      // Pure reorder: independent of the corr/s0 barrier-spins below, so its
      // tcgen05.cp latency overlaps them. do_pv consumes SFV after this cp via
      // async-pipe program order -> bit-exact. v_index_prev/pv_buf are the
      // SAME values the original site used (set last iter / from k-1).
      load_sfv(v_index_prev, pv_buf);
#endif

      // PV(k-1): P[(k-1)%2] * V(k-1) -> O
      BCN(8);   // [2SM] loop: before pipeline_corr.producer_acquire (PipelineO mma->corr)
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);   // [OSPLIT: h0 of O]
      BCN(80);  // [2SM] loop: past corr acquire, before PipelineS sub-slot acquires
#if defined(MXFP8_2SM_N128SINGLE)
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);   // g0 released sub0(k-1): P half0 ready
      pipeline_s1.producer_acquire(pipeline_s1_producer_state);   // g1 released sub1(k-1): P half1 ready
#endif
#endif
      BCN(81);  // [2SM] loop: P ready, about to PV-SF UTCCP
#ifdef MXFP8_PSTATIC
      // [PSTATIC] one-shot constant SFP: both TMEM slots filled at the first
      // PV (the s0/s1 producer_acquires above prove both CTAs' softmax entry
      // fill + fence are done); per-tile SFP UTCCP is gone.
      if (first_pv) { load_sfp(0); load_sfp(1); }
#else
      load_sfp(pv_buf);                // [PVMX 2b] stage P-SF[pv_buf] -> SFP[pv_buf]
#endif
#if !defined(MXFP8_M2_UTCCP) && !defined(MXFP8_M2_COMBO)
      load_sfv(v_index_prev, pv_buf);  // [PVMX 2a.1b] stage V-SF[v_index_prev] -> SFV[pv_buf]
#endif
      BCN(82);  // [2SM] loop: PV-SF UTCCP done, about to do_pv (PV 2-SM MMA)
#ifdef MXFP8_OSPLIT
      // [刀12 OSPLIT] PV0(k-1) -> O[0:64) on stage h0, PV1(k-1) -> O[64:128) on h1.
      // The acquire(h1) between the two sub-issues waits correction's h1 release of
      // tile k-2, which overlaps PV0's UMMA execution — corr work is NOT serially
      // sandwiched between the sub-PVs (the M3-2 死因), it pipelines half-a-tile ahead.
      do_pv(v_index_prev, pv_buf, first_pv, /*sub=*/0);
      pipeline_corr.producer_commit(pipeline_corr_producer_state);   // h0 in flight
      ++pipeline_corr_producer_state;
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);  // h1 (corr released k-2's h1)
      do_pv(v_index_prev, pv_buf, first_pv, /*sub=*/1);
      BCN(92);   // [2SM] loop: past do_pv return (before corr producer_commit)
      first_pv = false;
      pipeline_corr.producer_commit(pipeline_corr_producer_state);   // h1 in flight
      ++pipeline_corr_producer_state;
#else
      // [M3-2 LAZY-MAX] whole-tile PV (P halves share scale w_k).
      do_pv(v_index_prev, pv_buf, first_pv);
      BCN(92);   // [2SM] loop: past do_pv return (before corr producer_commit)
      first_pv = false;
      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;
#endif
      BCN(100);  // [2SM] loop: past PV producer_commit (PipelineO)

      // release V(k-1) then K(k)  (slot order)
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      v_index_prev = v_index;
    }

    // ---- tail: PV(n-1) -----------------------------------------------
    {
      int pv_buf = (n - 1) & 1;
      BCN(101);  // [2SM] tail: before corr acquire
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);
      // balancing commits for the sub-slots acquired at the end of the last
      // loop iteration (no QK follows); then acquire #(n+2) on each
      // sub-pipeline waits g0/g1 release #n ⟹ both P halves of tile n-1 ready.
#if defined(MXFP8_2SM_N128SINGLE)
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
#endif
      ++pipeline_s0_producer_state;
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s1.producer_commit(pipeline_s1_producer_state);
#endif
      ++pipeline_s1_producer_state;
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_acquire(pipeline_s0_producer_state);
      pipeline_s1.producer_acquire(pipeline_s1_producer_state);
#endif
#endif
#ifdef MXFP8_PSTATIC
      // [PSTATIC] n==1 case: the loop never ran, fill both slots here once.
      if (first_pv) { load_sfp(0); load_sfp(1); }
#else
      load_sfp(pv_buf);                // [PVMX 2b] tail: stage P-SF[pv_buf] -> SFP[pv_buf]
#endif
#ifdef MXFP8_DBG
      // [续19j] capture smem_sfv (V-SF input to the tail PV's UTCCP) — SAFE smem read
      // (not TMEM). v_index_prev=3 (V1 KV stage) for tile 1. Compare to repacked dSFV.
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && (threadIdx.x%32)==0) {
        auto* psfv = reinterpret_cast<uint8_t*>(storage.smem_sfv.data());
        for (int i = 0; i < 2048; ++i) g_dbg_sfv[i] = psfv[i];
      }
#endif
      load_sfv(v_index_prev, pv_buf);  // [PVMX 2a.1b] tail: stage V-SF[v_index_prev] -> SFV[pv_buf]
      BCN(102);  // [2SM] tail: before tail do_pv
#ifdef MXFP8_OSPLIT
      do_pv(v_index_prev, pv_buf, first_pv, /*sub=*/0);
      pipeline_corr.producer_commit(pipeline_corr_producer_state);   // h0
      ++pipeline_corr_producer_state;
      pipeline_corr.producer_acquire(pipeline_corr_producer_state);  // h1
      do_pv(v_index_prev, pv_buf, first_pv, /*sub=*/1);
      pipeline_corr.producer_commit(pipeline_corr_producer_state);   // h1
      ++pipeline_corr_producer_state;
#else
      do_pv(v_index_prev, pv_buf, first_pv);
      pipeline_corr.producer_commit(pipeline_corr_producer_state);
      ++pipeline_corr_producer_state;
#endif
      BCN(103);  // [2SM] tail: past tail PV producer_commit

      // release V(n-1)
      pipeline_kv.consumer_release(pipeline_kv_release_state);
      ++pipeline_kv_release_state;

      // final balancing commits (match the trailing producer_acquires)
#if defined(MXFP8_2SM_N128SINGLE)
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
      ++pipeline_s0_producer_state;
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0.producer_commit(pipeline_s0_producer_state);
#endif
      ++pipeline_s0_producer_state;
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s1.producer_commit(pipeline_s1_producer_state);
#endif
      ++pipeline_s1_producer_state;
#endif
    }

    // release Q
    BCN(104);  // [2SM] before release Q (mma end)
    pipeline_q.consumer_release(pipeline_q_release_state);
    ++pipeline_q_release_state;
    BCN(105);  // [2SM] mma() returning
  }

  // [2SM 续18] PEER-CTA shadow consumer for the KV pipeline.
  //
  // In the cooperative cta_group::2 MMA, ONLY the leader CTA issues mma(); the
  // peer's MMA warp is otherwise idle. But the peer's Load warp SELF-loads K/V
  // into the peer's own smem (required — the 2-SM MMA reads B from BOTH CTAs'
  // smem; gating the peer KV load off hangs, see 续18 / experiment C1), arming
  // the peer's OWN KV full barriers. The leader's consumer_wait only waits the
  // leader's full barriers, so the peer's full barriers are never consumed. On
  // the first KV wrap (n_kv > StageCountKV) the peer's producer_acquire re-arms a
  // never-flipped full barrier -> CUDA_EXCEPTION_4 Warp Illegal Instruction at
  // arrive_and_expect_tx (block 1, Load warp).
  //
  // Fix: the peer MMA warp shadow-consumes its own KV full barriers — one
  // consumer_wait per K slot and per V slot, matching the peer Load's 2n arms.
  // It does NOT release the empty barriers: the leader's consumer_release issues
  // umma_arrive_multicast_2x1SM which already arrives the PEER's empty barrier
  // (a second release here would double-count and free the buffer early).
  template<class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  mma_peer_shadow(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      PipelineKV& pipeline_kv, typename PipelineKV::PipelineState& pipeline_kv_consumer_state) {
    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);
    for (int k = 0; k < mask_tile_count; ++k) {
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);   // K(k) full
      ++pipeline_kv_consumer_state;
      pipeline_kv.consumer_wait(pipeline_kv_consumer_state);   // V(k) full
      ++pipeline_kv_consumer_state;
    }
  }

  template<bool need_apply_mask, class Stage, class BlkCoord, class CoordTensor, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax_step(
      float& row_max, float& row_sum, float& chain_w,
      Stage stage, bool final_call, bool chain_first,
      BlkCoord const& blk_coord, CoordTensor const& cS,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,    // [PVMX 2a.0] write P -> storage.smem_p
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s) {

    // [MXFP8 N128 M3b] split-N cooperative softmax. stage 0 (warps 0-3) owns
    // KV columns [0:64]; stage 1 (warps 4-7) owns [64:128]. Each group reduces
    // its 64-col half; row_max (every step) and final row_sum are combined
    // across groups via a smem exchange + NamedBarrier. Only group 0 (is_g0)
    // drives the mma->softmax / softmax->correction pipelines and writes the
    // V-stats; group 1 stays lockstepped purely through the NamedBarriers.
    const bool is_g0 = (stage == 0);
    const uint32_t nHalf = uint32_t(stage) * 64u;   // TMEM col offset of this group's score half
    const uint32_t pHalf = uint32_t(stage) * 16u;   // TMEM col offset of this group's P half (64 e4m3 = 16 fp32 cols)

    // [M3-2] smem exchange buffer for the cross-group chain / final merge —
    // one float per (group, row). Lives in TensorStorage: a function-static
    // __shared__ here would be PER-TEMPLATE-INSTANCE (mask/no-mask) and the
    // chain state would break at the unmasked->masked boundary. Other warp
    // roles never reach here.
    float (&smem_sm_exch)[2][128] =
        *reinterpret_cast<float(*)[2][128]>(storage.smem_softmax_exch.data());

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    // [M3-3 LAGGED CHAIN v2] tile k's shared P scale v_k = w_{k-1}. g0
    // publishes w_k from its chain section every step (uniform, no tile0
    // special case); g1 keeps v_k in a REGISTER (its chain_w doubles as the
    // cache: the value read at the END of step k-1 was g0's round-(k-1)
    // publish = w_{k-1} = v_k). g1 re-reads at the END of its step (after P
    // is written) to seed the next tile — ZERO order-wait on g1's critical
    // path. Only g1's first step reads in-line (no cache yet; and v_1 = w_0
    // = v_0, so that one read also seeds step 1).

    // [2SM 续19o] NOTE: tried CTA-aware get_slice(mma_v) for the C partition/fragment
    // (tutorial 04_mma_tma_2sm_sm100.cu pattern) — NO effect on peer-S=0 (our s_base
    // override negates any V-offset; M-split C has no per-slice offset here). The peer
    // fix needs the FULL tutorial TMEM setup (Allocator2Sm + .data()=tmem_base_ptr +
    // cluster-visible alloc + cta_group::2 lifecycle), not just the slice. See M2 续19o.
    Tensor tScS_full = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    // [MXFP8 N128 M2] double-buffered S: select the S/P/V buffer from the
    // S-pipeline consumer state index (0/1). softmax_step(k) consumes commit #k
    // → index == k%2 → buffer k%2, exactly the buffer QK(k) wrote. group 1
    // mirrors group 0's pipeline-state ++ so this index stays in lockstep.
    int sbuf = pipeline_s_consumer_state.index();

    // Build the full (128,128) C fragment exactly as the single-group variant,
    // then compose down to this group's 64-col half (the proven sub-view path
    // already used for tStS_v / tStS_P — avoids partitioning the N=128 TiledMma
    // over a 64-wide tile directly).
    Tensor tStS_full = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));
    uint32_t s_base = (sbuf == 0 ? uint32_t(TmemAllocation::S0) : uint32_t(TmemAllocation::S1));

    Tensor tStS = tStS_full.compose(make_layout(make_shape(_128{}, _64{})));
    // [2SM 续16] NOTE: tStS.layout = (128,64):(65536,1) — N is contiguous (stride 1), so
    // the softmax READ is correct. The numeric bug (QK S = exact 0 at kv odd-16-blocks
    // {16-31,48-63}, correct at {0-15,32-47}) is in the cooperative MMA WRITE / SF, NOT
    // the read. Suspect SFB (K scale) zero at odd-16-N-blocks in 2-SM. See 续16.
    tStS.data() = warp_uniform(s_base + nHalf);     // this group's 64-col score half
#ifdef MXFP8_2SM_CLUSTERCHK
    if ((blockIdx.x==0||blockIdx.x==1) && blockIdx.y==0 && blockIdx.z==0 && is_g0 && cute::elect_one_sync()) {
      printf("[ADDRCHK] blockIdx=%d softmax READ tStS.data()=0x%08x  s_base=%u nHalf=%u sbuf=%d\n",
             (int)blockIdx.x, (unsigned)tStS.data().get(), (unsigned)s_base, (unsigned)nHalf, sbuf);
    }
#endif
    // [mask fix] group g owns score cols [g*64 : g*64+64) = kv [tile_base + g*64 : ...).
    // tStS.data() gets +nHalf (above), but the COORDINATE used for masking must too —
    // else ResidualMask::apply_mask (kv >= seqlen_kv) tests the wrong kv for group 1 and
    // leaves its half of the partial last tile unmasked (S%128!=0 -> biased low). Offset
    // the score-coordinate's kv by nHalf so both groups see global kv.
    Tensor tScS = domain_offset(make_coord(_0{}, nHalf),
                                tScS_full.compose(make_layout(make_shape(_128{}, _64{}))));

    // [M3-2] V-stats live at THIS GROUP'S sub-slot base cols [0:2] — per-sub
    // stats slots (4 total: buf x sub), written by EACH group for its own
    // sub-steps. (g0: s_base+0; g1: s_base+64. Final sum/max: g0's slot.)
    Tensor tStS_v = tStS_full.compose(make_layout(make_shape(_128{}, _2{})));
    tStS_v.data() = warp_uniform(s_base + nHalf);
    Tensor tScS_v = tScS_full.compose(make_layout(make_shape(_128{}, _2{})));

    // per-group P half is 64 e4m3 = 16 fp32-words wide.
    auto tilePlikeFP32 = Int<64>{} / Int<sizeof(float)>{} * Int<sizeof(Element)>{};
    uint32_t p_base = (sbuf == 0 ? uint32_t(TmemAllocation::P0) : uint32_t(TmemAllocation::P1));
    Tensor tStS_P = tStS_full.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));
    tStS_P.data() = warp_uniform(p_base + pHalf);
    Tensor tScS_P = tScS_full.compose(make_layout(make_shape(_128{}, tilePlikeFP32)));

    // Each thread owns a single row.
    #if defined CUTE_ARCH_TCGEN05_TMEM_STAT_ENABLED
      using TMEM_LOAD = SM100_TMEM_LOAD_STAT_32dp32b32x;
    #else
      // [MXFP8 N128 M3b] split-N: each group reads only its 64-col score half.
      // Use the 64-wide TMEM-load atom so the whole half lands in ONE tcgen05.ld
      // per thread (vs two with the 32x atom) — fewer MIO-queued instructions,
      // which NCU flagged as a top secondary stall (mio_throttle). STAT is not
      // enabled for sm_100a (SM103 only), so this #else is the live path.
      using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b64x;
    #endif
    // [MXFP8] P-tile width follows TileShapeQK's N. With the Route-C N=64 tile
    // the P-tile is 16 fp32-words wide, so the store atom must be the 16x form
    // (the pristine N=128 collective used 32x).
    using TMEM_STORE = std::conditional_t<
        decltype(tilePlikeFP32)::value >= 32,
        SM100_TMEM_STORE_32dp32b32x, SM100_TMEM_STORE_32dp32b16x>;
    using TMEM_STORE_V = SM100_TMEM_STORE_32dp32b2x;   // 4x32 threads with 2 cols of 32b elem

    // (thread_idx declared at the top of the step, before the early chain publish.)

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tStS);
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);

    Tensor tTMEM_LOADtS = thr_tmem_load.partition_S(tStS);
    Tensor tTMEM_LOADcS = thr_tmem_load.partition_D(tScS);

    auto tiled_tmem_storev = make_tmem_copy(TMEM_STORE_V{}, tStS_v);
    auto thr_tmem_storev  = tiled_tmem_storev.get_slice(thread_idx);

    Tensor tTMEM_STOREVtS = thr_tmem_storev.partition_D(tStS_v);
    Tensor tTMEM_STOREVcS = thr_tmem_storev.partition_S(tScS_v);

    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE{}, tStS_P);
    auto thr_tmem_store  = tiled_tmem_store.get_slice(thread_idx);

    Tensor tTMEM_STOREtS_x4 = thr_tmem_store.partition_D(tStS_P);
    tTMEM_STOREtS_x4.data() = warp_uniform(tTMEM_STOREtS_x4.data().get());
    Tensor tTMEM_STOREcS = thr_tmem_store.partition_S(tScS_P);

    // [M3 sub-tile] each group owns ITS OWN mma->softmax pipeline (g0 <- s0 =
    // QK sub0 = cols [0:64), g1 <- s1 = sub1 = [64:128)) and waits for ITS half
    // of S directly — the B_SREADY cross-group NamedBarrier is GONE. g0 starts
    // on sub0 while the MMA is still computing sub1.
#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s.consumer_wait(pipeline_s_consumer_state);
#endif

    // read all of S from tmem into reg mem
    // [续19u] race test (peer reads S before leader's MMA writes peer TMEM) was NEGATIVE:
    // a 200us peer delay left peer S = exact 0. So the cta_group::2 MMA genuinely does
    // NOT write the peer's TMEM (not a read-before-write race). See M2 续19u.
    Tensor tTMEM_LOADrS = make_tensor<ElementQK>(shape(tTMEM_LOADcS));
    copy(tiled_tmem_load, tTMEM_LOADtS, tTMEM_LOADrS);

#ifdef MXFP8_DBG
    // [续19d] Dump raw QK scores for q-row 0, first KV tile — BOTH groups.
    // g0 owns kv 0-63 (tScS kv base 0), g1 owns kv 64-127 (domain_offset nHalf=64).
    // Store at the kv coordinate so g_dbg_S[0..127] = full row-0 S. g1's half was
    // never verified before and is the suspected scramble (P vs refP wrong @ kv>=64).
    // [续19m] per-CTA sbuf at FIRST tile (g0, kvbase==0, thread0) -> slot blockIdx.x.
    // Detects leader-vs-peer S-buffer desync (peer reading empty buffer -> peer S=0).
    if (blockIdx.y==0 && blockIdx.z==0 && is_g0 && thread_idx==0 && get<1>(tTMEM_LOADcS(0))==0 && blockIdx.x<2) {
      g_dbg_sbuf_val[blockIdx.x] = sbuf;
    }
    if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {
      int row    = get<0>(tTMEM_LOADcS(0));
      int kvbase = get<1>(tTMEM_LOADcS(0));
      if (row == 0) {                              // S[0, kv=kvbase..] — kvbase is GLOBAL (cS advances per tile)
        for (int i = 0; i < size(tTMEM_LOADrS) && (kvbase + i) < 256; ++i)
          g_dbg_S[kvbase + i] = tTMEM_LOADrS(i);
        g_dbg_S_got = (int)size(tTMEM_LOADrS);
      }
      if (kvbase == 0 && row >= 0 && row < 64)     // S[q=row, kv=0]
        g_dbg_Srow[row] = tTMEM_LOADrS(0);
    }
    // [续19k] PEER (block 1) S for its local row 0 (thread_idx==0). Verify peer's QK.
    if (blockIdx.x == 1 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx == 0) {
      int kvbase = get<1>(tTMEM_LOADcS(0));
      g_dbg_Sp_row = get<0>(tTMEM_LOADcS(0));   // record the coordinate-row the peer sees
      for (int i = 0; i < size(tTMEM_LOADrS) && (kvbase + i) < 256; ++i) g_dbg_Sp[kvbase + i] = tTMEM_LOADrS(i);
      // [续19l] is the peer's Q even LOADED? count nonzero smem_q bytes (safe smem read).
      auto* pq = reinterpret_cast<uint8_t*>(storage.smem_q.data());
      int nzq = 0; for (int i = 0; i < (int)sizeof(storage.smem_q); ++i) if (pq[i]) ++nzq;
      g_dbg_qnz = nzq; g_dbg_qsz = (int)sizeof(storage.smem_q);
      // [续19p] peer smem_sfq(Q-scale) loaded? If 0 -> peer's m128-255 SFA never reaches
      // TMEM -> cooperative block-scaled MMA scales peer S by ue8m0 0 = 2^-127 ~ 0.
      auto* psfq = reinterpret_cast<uint8_t*>(storage.smem_sfq.data());
      int nzsfq=0, szsfq=(int)sizeof(storage.smem_sfq); for (int i=0;i<szsfq;++i) if(psfq[i]) ++nzsfq;
      g_dbg_sfq_peer_nz = nzsfq; g_dbg_sfq_peer_sz = szsfq;
#if defined(MXFP8_DBG_LEADERSFQ)
      // [续19ac] PEER smem_sfq half-split. The 2cta UTCCP (cta_group::2.cp) src CTA-stride=_0
      // = both CTAs read SAME-offset smem. If peer needs its m128-255 SF at PEER smem offset 0
      // (firsthalf), and peer firsthalf is nonzero (续19s loaded peer's own half), then a
      // truly-cooperative 2cta UTCCP run by the leader SHOULD fill peer TMEM from PEER smem.
      // It doesn't (peer S=0) -> peer never RUNS the UTCCP (leader-only gate) AND the leader's
      // cp does not reach peer. Compare to leader (firsthalf=nz, secondhalf=0).
      { int lo=0,hi=0; for(int i=0;i<szsfq;i++){ if(psfq[i]){ if(i<szsfq/2)lo++; else hi++; } }
        printf("[PEERSFQ] total=%d nz=%d firsthalf=%d secondhalf=%d\n", szsfq, nzsfq, lo, hi); }
#endif
      // [续19w] PEER TMEM column-scan attempt -> DEADLOCKS (any extra tcgen05.ld in the
      // peer softmax hangs the cooperative pipeline; even the read-only SFATILE print did).
      // Route-shift hypothesis ruled out by REASONING: tStS=((128,128)):((65536,1)) maps
      // M->lanes(0-127), N->cols(0-127); 2-SM M-split puts peer m128-255 in peer lanes
      // 0-127 / cols 0-127 = exactly S0 (no column shift). So peer S=0 = genuinely
      // never written to peer TMEM, not a read-offset bug. Next: cuda-gdb. See M2 续19w.
    }
    // [续19r] smem_q byte-sum per CTA (both blocks) -> peer==leader means peer Q is a
    // m0-127 DUPLICATE (A/M-split broken in load); differ means peer Q = m128-255 (OK).
    if (blockIdx.x < 2 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx == 0 && get<1>(tTMEM_LOADcS(0))==0) {
      auto* pq = reinterpret_cast<uint8_t*>(storage.smem_q.data());
      unsigned long long s = 0; for (int i = 0; i < (int)sizeof(storage.smem_q); ++i) s += (unsigned)pq[i]*(unsigned)(i+1);
      g_dbg_qsum[blockIdx.x] = s;
#if defined(MXFP8_DBG_PEERQ_LOC)
      // [续19an DECISIVE] WHERE is each CTA's Q data inside smem_q? FORCE_PEERQ proved the
      // peer-half MMA READS peer smem fine (S nonzero when forced 1.0) -> with real data
      // S=exact 0 means the real Q bytes are NOT at the offsets the MMA reads (e.g. wrong
      // stage: Q TMA atom is 2CTA, self-only tma_partition may route the box elsewhere).
      // stage0 = bytes [0, half), stage1 = [half, 2*half).
      { int half = (int)sizeof(storage.smem_q) / 2; int lo = 0, hi = 0;
        for (int i = 0; i < (int)sizeof(storage.smem_q); ++i) { if (pq[i]) { if (i < half) lo++; else hi++; } }
        printf("[PEERQ_LOC] bx=%d rank=%d total=%d stage0_nz=%d stage1_nz=%d | st0[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x st1[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x\n",
               (int)blockIdx.x, (int)cute::block_rank_in_cluster(), (int)sizeof(storage.smem_q), lo, hi,
               pq[0], pq[1], pq[2], pq[3], pq[4], pq[5], pq[6], pq[7],
               pq[half+0], pq[half+1], pq[half+2], pq[half+3], pq[half+4], pq[half+5], pq[half+6], pq[half+7]);
      }
#endif
    }
    // [续19d] g0-side FULL 128-col read of the leader's S accumulator (base s_base,
    // NO +nHalf). Tells whether the MMA WROTE cols 64-127 correctly (=> g1's +nHalf
    // READ is the bug) or wrote them garbage (=> MMA write bug). Overwrites g_dbg_S
    // (got=999) so the driver's full-128 QK compare judges the physical accumulator.
    if (false && is_g0 && blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0) {  // [续19d] disabled: was overwriting g_dbg_S with local-coord; per-group dump now captures global-kv across tiles
      Tensor tFull = tStS_full.compose(make_layout(make_shape(_128{}, _128{})));
      tFull.data() = warp_uniform(s_base);
      Tensor cFull = tScS_full.compose(make_layout(make_shape(_128{}, _128{})));
      auto tl  = make_tmem_copy(TMEM_LOAD{}, tFull);
      auto thl = tl.get_slice(thread_idx);
      Tensor tFs = thl.partition_S(tFull);
      Tensor cFs = thl.partition_D(cFull);
      Tensor rFs = make_tensor<ElementQK>(shape(cFs));
      copy(tl, tFs, rFs);
      if (get<0>(cFs(0)) == 0) {                   // row 0
        for (int i = 0; i < size(rFs); ++i) { int n = get<1>(cFs(i)); if (n >= 0 && n < 128) g_dbg_S[n] = rFs(i); }
        g_dbg_S_got = 999;
      }
    }
#endif

    if constexpr (need_apply_mask) {
      Mask{}.apply_mask(tTMEM_LOADrS, tTMEM_LOADcS, problem_shape);
    }

    ElementQK old_row_max = row_max;
#ifdef MXFP8_AMAXSCORE
    // [op5 trick] per-32-col SCORE block maxes, captured in the rowmax pass below
    // (always runs: TMEM_STAT hardware-max is SM103-only, sm_100a uses manual).
    // Reused to derive the P amax as exp2(scale*(blk_max-row_max)) on the idle SFU.
    float blk_max_score[2] = { -INFINITY, -INFINITY };
#endif
    #if defined CUTE_ARCH_TCGEN05_TMEM_STAT_ENABLED
      auto pos = tTMEM_LOADcS(0);
      if (!need_apply_mask || (need_apply_mask && (get<0>(pos) >= get<1>(pos) + 12) && (get<1>(pos) < get<1>(problem_shape)))) {
        float curr_max = tiled_tmem_load.get_max();
        row_max = ::fmax(row_max, curr_max);
      }
      else
    #endif
#ifdef MXFP8_LCFUSE
    // [2SM-LC v3 FUSED PASS, ported from base lc3b (+5.1 there)] tile k>=1:
    // P's scale v_k = w_{k-1} already sits in chain_w (known BEFORE this
    // tile's S arrives) — the standalone rowmax pass is only needed by the
    // tile-0 seed. For k>=1 the local max is accumulated INSIDE the fused
    // exp2 loop below (raw value read just before the in-place overwrite),
    // interleaving the MUFU burst 1:2 with ALU/FMA. Chain math bit-identical:
    // chain_w monotone ⟹ fmax(w_{k-1}, max(v_{k-1}-seeded local)) ==
    // fmax(w_{k-1}, max(raw elements)). g0's order_s publish moves to right
    // after the fused loop (vs G1NOMAX which kept the pre-exp2 cadence).
    if (chain_first)
#elif defined(MXFP8_G1NOMAX)
    // [G1-NOMAX] g1's local rowmax is DEAD WORK under the lazy chain: the
    // chain section below discards it unconditionally (row_max = chain_w;
    // only g0's local max feeds w_k). Skip the whole pass on g1 — saves 64
    // fmax/thread/tile of ALU and lets g1's exp2 (MUFU) start immediately.
    // g0 keeps the pass (its publish w_k sits inside the order_s ping-pong
    // BEFORE the exp2 loop — do NOT move it; that cadence is load-bearing).
    if (is_g0)
#endif
    {
      // compute rowmax
      float row_max_0 = row_max;
      float row_max_1 = row_max;
      float row_max_2 = row_max;
      float row_max_3 = row_max;
#ifdef MXFP8_AMAXSCORE
      // [op5 trick, v2 RESTRUCTURED] the per-32-col SCORE block maxes fall out of
      // the SAME 64 fmax — split the single 64-wide reduction into two 32-wide
      // blocked reductions (each 4 ILP accumulators), then row_max = max(blk0,blk1).
      // SAME 64 fmax as the original strided pass (NOT +8 like v1); the 2 block
      // maxes are a free byproduct. Downstream derives the P amax as
      // exp2(scale*(blk_max-row_max_scale)) on the idle SFU, REPLACING the 64-wide
      // in-loop fmax over the exp2'd P (which serialized on the MUFU exp2 latency).
      // Block accumulators seed at -INF (PURE per-block score max — the P amax
      // must be this block's own max, not inflated by the prior-tile row_max).
      // The incoming online row_max is folded back into row_max at the end.
      float b0_0 = -INFINITY, b0_1 = -INFINITY, b0_2 = -INFINITY, b0_3 = -INFINITY;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < 32; i += 4) {                  // block 0: cols [0,32)
        b0_0 = ::fmax(b0_0, tTMEM_LOADrS(i));
        b0_1 = ::fmax(b0_1, tTMEM_LOADrS(i+1));
        b0_2 = ::fmax(b0_2, tTMEM_LOADrS(i+2));
        b0_3 = ::fmax(b0_3, tTMEM_LOADrS(i+3));
      }
      blk_max_score[0] = ::fmax(::fmax(b0_0, b0_1), ::fmax(b0_2, b0_3));
      float b1_0 = -INFINITY, b1_1 = -INFINITY, b1_2 = -INFINITY, b1_3 = -INFINITY;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 32; i < size(tTMEM_LOADrS); i += 4) { // block 1: cols [32,64)
        b1_0 = ::fmax(b1_0, tTMEM_LOADrS(i));
        b1_1 = ::fmax(b1_1, tTMEM_LOADrS(i+1));
        b1_2 = ::fmax(b1_2, tTMEM_LOADrS(i+2));
        b1_3 = ::fmax(b1_3, tTMEM_LOADrS(i+3));
      }
      blk_max_score[1] = ::fmax(::fmax(b1_0, b1_1), ::fmax(b1_2, b1_3));
      // row_max = max(incoming online row_max, both block maxes).
      row_max = ::fmax(row_max, ::fmax(blk_max_score[0], blk_max_score[1]));
      (void)row_max_0; (void)row_max_1; (void)row_max_2; (void)row_max_3;
#else
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(tTMEM_LOADrS); i += 4) {
        row_max_0  = ::fmax(row_max_0, tTMEM_LOADrS(i));
        row_max_1 = ::fmax(row_max_1, tTMEM_LOADrS(i+1));
        row_max_2 = ::fmax(row_max_2, tTMEM_LOADrS(i+2));
        row_max_3 = ::fmax(row_max_3, tTMEM_LOADrS(i+3));
      }
      row_max = ::fmax(row_max_0, row_max_1);
      row_max = ::fmax(row_max, row_max_2);
      row_max = ::fmax(row_max, row_max_3);
#endif
    }

    // [MXFP8 N128 M3b] combine the per-group partial row_max across the two
    // N-halves so both groups hold the true global row_max before computing P.
    // (Both groups already finished reading their score halves into registers,
    // so it is now safe for group 1's P write to land in group 0's old score
    // region — see the P-in-S note below.)
    // [M3-2 LAZY-MAX] the whole tile's P is written in ONE shared scale w_k =
    // max(w_{k-1}, g0's local 64-col max) — the "lazy" chain that g0 ALONE
    // advances. g1 just READS w_k (one-way order_s handoff, no rendezvous, no
    // contribution): its P half may exceed 1.0, which the P block-SF (e8m0 up
    // to 2^127) absorbs exactly — softmax is scale-invariant, so any monotone
    // w sequence is mathematically EXACT. This keeps PV whole-tile and
    // correction at ONE rescale per tile (stats/c0 from g0 only).
    // (Theoretical tail risk: a >~120-binade score jump inside one 64-col
    // sub-tile would overflow P1 in fp32 — impossible for random/real data;
    // TODO optional THRESH fallback if adversarial inputs ever matter.)
    // [M3-3 LAGGED CHAIN v2] chain section. g0: w_k = max(w_{k-1}, local0)
    // (row_max entered as v_{k-1}, local merged above; v_{k-1} <= w_{k-1} so
    // max(chain_w, row_max) == w_k), publish w_k (= v_{k+1}; tile0's publish
    // doubles as v_0 since v_1 = w_0 = v_0), adopt v_k = entry chain_w.
    // g1: adopt the cached v_k; first step reads in-line instead.
#ifdef MXFP8_LCFUSE
    // [2SM-LC v3] k>=1: both groups adopt v_k = w_{k-1} immediately (no local
    // pass ran); g0's w_k = fmax(w_{k-1}, local max) update + order_s publish
    // move to right after the fused exp2 loop (local max accumulated there).
    // Tile 0 keeps the original seed publish (standalone pass ran above).
    if (is_g0) {
      if (chain_first) {
        float w_k = ::fmax(chain_w, row_max);
        order_s.wait();                      // g1's inline step-0 read pairs with this
        smem_sm_exch[0][thread_idx] = w_k;   // publish (g1 reads it as v_0 = w_0)
        order_s.arrive();
        chain_w = w_k;
        row_max = w_k;                       // v_0 = w_0
      } else {
        row_max = chain_w;                   // v_k = w_{k-1} drives P/stats/l downstream
      }
    } else {
      if (chain_first) {
        order_s.wait();                      // g0's first publish (w_0)
        chain_w = smem_sm_exch[0][thread_idx];
        order_s.arrive();                    // (this round's pair — no end-of-step read on step 0)
      }
      row_max = chain_w;                     // cached v_k (discard local max)
    }
#else
    if (is_g0) {
      float v_k = chain_first ? ::fmax(chain_w, row_max) : chain_w;
#ifdef MXFP8_WHYST
      // [刀21 WHYST] w 滞回: 只有当本 tile 局部 max 超出 w_{k-1} 超过 T 个
      // binade (P 域; w 是 raw-S 域 ⟹ 乘 scale 换算) 才推进链 — 否则 w 原地。
      // softmax 刻度不变性 ⟹ 任何单调 w 序列精确; 代价 = P 过冲上限从懒链
      // 原生滞后再 ×2^T, 必须 < e4m3 satfinite 448 (sweep 闸门裁决)。w 不动
      // ⟹ v_k 链恒定 ⟹ correction old==new, scale==1 精确, O rescale 跳过
      // (刀20 noinit 探针: 该税 2SM SOW ~54)。tile-0 种子保持精确 fmax。
      float w_k = (chain_first ||
                   (row_max - chain_w) * params.scale_softmax_log2 > (float)MXFP8_WHYST_T)
                      ? ::fmax(chain_w, row_max) : chain_w;
#else
      float w_k = ::fmax(chain_w, row_max);
#endif
#ifndef MXFP8_2SM_DECOUPLE_PERF
      order_s.wait();                        // g1's previous end-of-step read done
#endif
      smem_sm_exch[0][thread_idx] = w_k;     // publish (g1 reads it as v_{k+1})
#ifndef MXFP8_2SM_DECOUPLE_PERF
      order_s.arrive();
#endif
      chain_w = w_k;
      row_max = v_k;                         // v_k drives P/stats/l downstream
    } else {
      if (chain_first) {
#ifndef MXFP8_2SM_DECOUPLE_PERF
        order_s.wait();                      // g0's first publish (w_0)
#endif
        chain_w = smem_sm_exch[0][thread_idx];
#ifndef MXFP8_2SM_DECOUPLE_PERF
        order_s.arrive();                    // (this round's pair — no end-of-step read on step 0)
#endif
      }
      row_max = chain_w;                     // cached v_k (discard local max)
    }
#endif

    ElementQK row_max_safe = row_max == -INFINITY ? 0 : row_max;

    // V-stats (w_{k-1}, w_k) -> correction's per-TILE O rescale; g0-only
    // (g1's s1_corr stays inert — correction consumes c0 once per tile).
    if (is_g0) {
      ElementQK old_safe = (old_row_max == -INFINITY) ? row_max_safe : old_row_max;
      Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));
      tTMEM_STOREVrS(kIdxOldRowMax) = old_safe;
      tTMEM_STOREVrS(kIdxNewRowMax) = row_max_safe;
      copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);

#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_c.producer_commit(pipeline_c_producer_state);
#endif
      ++pipeline_c_producer_state;
    }

    ElementQK scale = params.scale_softmax_log2;
    ElementQK row_max_scale = row_max_safe * scale;

    float2 scale_fp32x2 = make_float2(scale, scale);
    float2 minus_row_max_scale_fp32x2 = make_float2(-row_max_scale, -row_max_scale);

    Tensor tTMEM_STORErS_x4 = make_tensor<uint32_t>(shape(tTMEM_STOREcS));

    constexpr int kExpConversionsPerStep = 2;

    // [M3-2] order_s now serializes the global online-max chain (above).

    // [PVMX 2b] Phase 1: scale + exp -> P (fp32) in tTMEM_LOADrS (don't convert yet).
#if defined(MXFP8_AMAXFUSE) && !defined(MXFP8_PSTATIC)
    // [v3b AMAX-FUSE] P = exp2(..) >= 0 so fabs is free; fmax exactly
    // associative/commutative -> bit-identical to the standalone Phase-2 pass.
    constexpr int kSFVecF = 32;
#ifdef MXFP8_AMAXSCORE
    // [op5 trick] derive the per-32 P amax from the SCORE block max captured for
    // free in the rowmax pass: amax = max_i exp2(scale*(S_i - row_max)) =
    // exp2(scale*blk_max_score - row_max_scale) (exp2 monotone — exact, no abs
    // since P>=0). 2 exp2 on the idle SFU REPLACE the 64-wide in-loop fmax over
    // the exp2'd P (which serialized on the MUFU exp2 latency). row_max here is
    // already the lazy-chain v_k = w_{k-1}, so a block holding a NEW global max
    // gives amax>1; the e8m0 P-SF (2^127 range) absorbs that exactly (unlike
    // op5's ue4m3 448 cap which needed a clamp). scale/row_max_scale are set
    // just above; fast_exp2f matches the P loop's EXP2_SOFTMAX (plain dynking has
    // no E2POLY/E2MIX) ⟹ amax == max(P) bit-identical.
    float amax_f[2] = {
        fast_exp2f(scale * blk_max_score[0] - row_max_scale),
        fast_exp2f(scale * blk_max_score[1] - row_max_scale)
    };
#else
    float amax_f[2] = {0.0f, 0.0f};
#endif
#endif
#ifdef MXFP8_LCFUSE
    // [2SM-LC v3 FUSED PASS] accumulate the local max on the raw value just
    // before the in-place overwrite: same fmax count as the deleted standalone
    // pass, but the MUFU exp2 now interleaves 1:2 with ALU/FMA instead of
    // bursting after a pure-fmax pass. (chain_first keeps the standalone pass
    // above; its lc_lmax here is redundant and unused.)
    float lc_lmax0 = -INFINITY;
    float lc_lmax1 = -INFINITY;
#endif
#ifdef MXFP8_E2RSF
    // [刀8 E2RSF — LAGGED row-sum fuse] move the post-release row_sum pass
    // into the exp2 burst, consuming EX2 outputs MXFP8_E2RSF_LAG elements
    // behind (lag>=8 ⟹ the consumed MUFU issued >=4 iterations ago — a READY
    // operand, pure hole-filler). Differs from the v3c RSFUSE tombstone (-15,
    // 1SM dynamic) on all three kill conditions: (1) v3c consumed i+0/i+1
    // IMMEDIATELY (each FADD2 stalls on the just-issued MUFU, like the F2FP);
    // (2) 1SM exp2 stall is mio-queue-full (no holes to fill) vs 2SM wait-
    // latency-type (ncu: 10k/14.8k wait); (3) PSTATIC compiled the amax-fuse
    // fillers out — the dense MUFU region (SASS px7a e600-f010) has only
    // dependent F2FP at distance 4-6. Bit-exact: per-accumulator dispatch
    // (j&7) and ascending-j order identical to the standalone pass; the seed
    // row_sum*acc_scale is the same expression evaluated earlier (all inputs
    // — old_row_max/row_max_safe/scale — are fixed before Phase 1).
#ifndef MXFP8_E2RSF_LAG
#define MXFP8_E2RSF_LAG 8
#endif
    static_assert((MXFP8_E2RSF_LAG % 8) == 0 && MXFP8_E2RSF_LAG >= 8,
                  "E2RSF lag must be a positive multiple of 8 (keeps (j&7) accumulator dispatch identical)");
    {
      ElementQK rsf_acc_scale = (old_row_max == row_max_safe) ? 0.5f : 0.5f * ::exp2f(scale * (old_row_max - row_max_safe));
      row_sum *= rsf_acc_scale;
    }
    float2 rsf_acc0 = make_float2(row_sum, row_sum);
    float2 rsf_acc1 = make_float2(0, 0);
    float2 rsf_acc2 = make_float2(0, 0);
    float2 rsf_acc3 = make_float2(0, 0);
#endif
#if defined(MXFP8_E2NULL_PERF) && !defined(MXFP8_E2OFFLOAD)
// [刀10 NULL-PROBE] softmax sheds the tail C columns but NOBODY computes them
// (numerics WRONG: stale/garbage P tail, short row_sum) — quantifies the pure
// softmax-deload upper bound with ZERO structural cost.
#ifndef MXFP8_E2OFFLOAD_C
#define MXFP8_E2OFFLOAD_C 16
#endif
#endif
#if defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)
    // [刀10 E2OFFLOAD] unmasked steps: this group only computes exp2/cvt/STS
    // for its first kE2Keep columns; the tail C columns belong to correction.
    // The raw tail values stay untouched in tTMEM_LOADrS (the standalone
    // rowmax pass above already covered them — chain math unchanged).
    constexpr int kE2Keep = need_apply_mask
        ? int(decltype(size(tTMEM_LOADrS))::value)
        : int(decltype(size(tTMEM_LOADrS))::value) - (MXFP8_E2OFFLOAD_C);
    static_assert((kE2Keep % 8) == 0 && kE2Keep > 0, "E2OFFLOAD keep-range must stay a multiple of 8 (E2RSF accumulator dispatch)");
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < kE2Keep; i += 2) {
#else
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += kExpConversionsPerStep) {
#endif
      float2 in = make_float2(tTMEM_LOADrS(i+0), tTMEM_LOADrS(i+1));
#ifdef MXFP8_LCFUSE
      lc_lmax0 = ::fmax(lc_lmax0, in.x);
      lc_lmax1 = ::fmax(lc_lmax1, in.y);
#endif
      float2 out;
      cute::fma(out, scale_fp32x2, in, minus_row_max_scale_fp32x2);
      tTMEM_LOADrS(i+0) = EXP2_SOFTMAX_PSEL(i, out.x);
      tTMEM_LOADrS(i+1) = EXP2_SOFTMAX_PSEL(i, out.y);
#if defined(MXFP8_AMAXFUSE) && defined(MXFP8_AMAXLAG) && !defined(MXFP8_PSTATIC)
      // [2SM AMAXLAG, ported from base v5] consume the EX2 outputs 8 elements
      // (4 iters) behind — breaks the MUFU-latency dependent stall on the fmax
      // that immediately follows the exp2 writes. base measured ±0 (ptxas had
      // already scheduled it); 2SM's exp2 stall is WAIT-latency-type (vs base
      // mio-queue-full), so the dependent-stall break may actually bite here.
      // Same fmax element set per block (block index follows the LAGGED
      // element) ⟹ bit-exact. Tail drained after the loop.
      if (i >= 8) {
        amax_f[(i-8) / kSFVecF] = ::fmaxf(amax_f[(i-8) / kSFVecF],
                                     ::fmaxf(tTMEM_LOADrS(i-8), tTMEM_LOADrS(i-7)));
      }
#elif defined(MXFP8_AMAXFUSE) && !defined(MXFP8_PSTATIC) && !defined(MXFP8_AMAXSCORE)
      amax_f[i / kSFVecF] = ::fmaxf(amax_f[i / kSFVecF],
                                    ::fmaxf(tTMEM_LOADrS(i+0), tTMEM_LOADrS(i+1)));
#endif
      // [MXFP8_AMAXSCORE] in-loop fmax-over-P SKIPPED — amax_f already derived
      // from the SCORE block max (exp2 on the idle SFU above).
#ifdef MXFP8_E2RSF
      // [刀8 E2RSF] lagged accumulate: element pair (j, j+1) was exp2'd
      // LAG/2 iterations ago — operand ready, fills the MUFU wait hole.
      if (i >= MXFP8_E2RSF_LAG) {
        const int j = i - MXFP8_E2RSF_LAG;
        float2 rsf_in = make_float2(tTMEM_LOADrS(j+0), tTMEM_LOADrS(j+1));
        if      ((j & 7) == 0) cute::add(rsf_acc0, rsf_acc0, rsf_in);
        else if ((j & 7) == 2) cute::add(rsf_acc1, rsf_acc1, rsf_in);
        else if ((j & 7) == 4) cute::add(rsf_acc2, rsf_acc2, rsf_in);
        else                   cute::add(rsf_acc3, rsf_acc3, rsf_in);
      }
#endif
    }
#if (defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)) && defined(MXFP8_LCFUSE)
    // [刀10 E2OFFLOAD × LCFUSE] the fused loop no longer covers the offloaded
    // tail — accumulate its RAW values here so g0's chain w_k stays exact.
    CUTLASS_PRAGMA_UNROLL
    for (int i = kE2Keep; i < size(tTMEM_LOADrS); i += 2) {
      lc_lmax0 = ::fmax(lc_lmax0, tTMEM_LOADrS(i));
      lc_lmax1 = ::fmax(lc_lmax1, tTMEM_LOADrS(i+1));
    }
#endif
#ifdef MXFP8_E2RSF
    // [刀8 E2RSF] drain the last LAG lagged elements (source-placed before the
    // cvt/STS phases; same basic block — ptxas schedules them into the tail of
    // the burst alongside the equally-dependent final F2FPs).
#if defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)
    CUTLASS_PRAGMA_UNROLL
    for (int j = kE2Keep - MXFP8_E2RSF_LAG; j < kE2Keep; j += 2) {
#else
    CUTLASS_PRAGMA_UNROLL
    for (int j = size(tTMEM_LOADrS) - MXFP8_E2RSF_LAG; j < size(tTMEM_LOADrS); j += 2) {
#endif
      float2 rsf_in = make_float2(tTMEM_LOADrS(j+0), tTMEM_LOADrS(j+1));
      if      ((j & 7) == 0) cute::add(rsf_acc0, rsf_acc0, rsf_in);
      else if ((j & 7) == 2) cute::add(rsf_acc1, rsf_acc1, rsf_in);
      else if ((j & 7) == 4) cute::add(rsf_acc2, rsf_acc2, rsf_in);
      else                   cute::add(rsf_acc3, rsf_acc3, rsf_in);
    }
#endif
#if defined(MXFP8_AMAXFUSE) && defined(MXFP8_AMAXLAG) && !defined(MXFP8_PSTATIC)
    // [2SM AMAXLAG] drain the last 8 lagged elements.
    CUTLASS_PRAGMA_UNROLL
    for (int i = size(tTMEM_LOADrS) - 8; i < size(tTMEM_LOADrS); i += 2) {
      amax_f[i / kSFVecF] = ::fmaxf(amax_f[i / kSFVecF],
                                    ::fmaxf(tTMEM_LOADrS(i+0), tTMEM_LOADrS(i+1)));
    }
#endif
#ifdef MXFP8_LCFUSE
    // [2SM-LC v3] g0 publishes w_k = fmax(w_{k-1}, local max) right after the
    // fused loop (replaces the pre-exp2 chain-section publish; bit-identical
    // by chain monotonicity); g1 discards its local max as before. order_s
    // alternation count/sequence per step is unchanged — only the position of
    // g0's (wait, publish, arrive) within its own instruction stream moves.
    if (!chain_first && is_g0) {
      float w_k = ::fmax(chain_w, ::fmax(lc_lmax0, lc_lmax1));
      order_s.wait();                        // g1's previous end-of-step read done
      smem_sm_exch[0][thread_idx] = w_k;     // publish (g1 reads it as v_{k+1})
      order_s.arrive();
      chain_w = w_k;
    }
#endif
#ifdef MXFP8_DBG
    // [续19e] capture raw exp P for row 0 at GLOBAL kv (kvbase from tScS advances per
    // tile) so BOTH tiles are captured (was overwriting at local nHalf). Lets the
    // driver verify the softmax P for tile>=1 against the (exact) S.
    if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && thread_idx==0) {
      int kvb = get<1>(tTMEM_LOADcS(0));   // global kv base of this group's half
      for (int i = 0; i < size(tTMEM_LOADrS) && (kvb + i) < 256; ++i)
        g_dbg_P[kvb + i] = tTMEM_LOADrS(i);
      g_dbg_P_got = 1;
    }
#endif
    // [PVMX 2b] Phase 2: per-32 amax of P. Thread owns its row's 64 cols (group half)
    // = 2 SF blocks (size(tTMEM_LOADrS)=64; 64/32=2).
    constexpr int kSFVec   = 32;
    constexpr int kNSFBlk  = 2;
    (void)kSFVec;
#ifdef MXFP8_PSTATIC
    // [PSTATIC] Phases 2-3 compiled out: the e8m0 exponent is the compile-time
    // constant MXFP8_PSTATIC_EXP. exp_b/scale_inv_b keep their names so the
    // DBG dequant paths still compile; the constants fold at compile time.
    int   exp_b[kNSFBlk];
    float scale_inv_b[kNSFBlk];
    CUTLASS_PRAGMA_UNROLL
    for (int b = 0; b < kNSFBlk; ++b) {
      exp_b[b] = MXFP8_PSTATIC_EXP;
      scale_inv_b[b] = __uint_as_float((uint32_t)(127 - (MXFP8_PSTATIC_EXP)) << 23); // 2^-EXP
    }
#else
#ifdef MXFP8_AMAXFUSE
    float amax_b[kNSFBlk] = {amax_f[0], amax_f[1]};   // fused in Phase 1
#else
    float amax_b[kNSFBlk] = {0.0f, 0.0f};
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); ++i) {
      float v = ::fabsf(tTMEM_LOADrS(i));
      amax_b[i / kSFVec] = ::fmaxf(amax_b[i / kSFVec], v);
    }
#endif
    // Phase 3: per-block e8m0 = ceil(log2(amax/E4M3_MAX)) + 127, clamped.
    constexpr float E4M3_MAX_F = 448.0f;
    int   exp_b[kNSFBlk];
    float scale_inv_b[kNSFBlk];
    CUTLASS_PRAGMA_UNROLL
    for (int b = 0; b < kNSFBlk; ++b) {
      float a = amax_b[b];
      // [opt] ceil(log2(a/448)) via float exponent bits (no SFU log2/ceil)
      uint32_t _xb = __float_as_uint(a * (1.0f/E4M3_MAX_F));
      int e = (a == 0.0f) ? -127
              : ((int)((_xb >> 23) & 0xFFu) - 127 + (((_xb & 0x7FFFFFu) != 0u) ? 1 : 0));
      if (e < -127) e = -127;
      if (e >  127) e =  127;
      exp_b[b] = e;
      scale_inv_b[b] = __uint_as_float((uint32_t)(127 - e) << 23); // [opt] 2^-e, no SFU exp2
    }
#endif
    // Phase 4: scale P by 2^-exp_b, convert to e4m3, and pack four FP8
    // values per uint32 TMEM word. The row_sum loop downstream still consumes
    // the unscaled FP32 values in tTMEM_LOADrS.
    constexpr int kPConversionsPerWord = 4;
    Tensor tTMEM_STORErS_x4_e = recast<Array<Element, kPConversionsPerWord>>(tTMEM_STORErS_x4);
    NumericArrayConverter<Element, ElementQK, kPConversionsPerWord> convert_p;

#if defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < kE2Keep; i += kPConversionsPerWord) {
#else
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += kPConversionsPerWord) {
#endif
      Array<ElementQK, kPConversionsPerWord> in_conv;
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < kPConversionsPerWord; ++j) {
#if defined(MXFP8_PSTATIC) && ((MXFP8_PSTATIC_EXP) == 0)
        in_conv[j] = tTMEM_LOADrS(i+j);
#else
        int b = (i + j) / kSFVec;
        in_conv[j] = tTMEM_LOADrS(i+j) * scale_inv_b[b];
#endif
      }
      tTMEM_STORErS_x4_e[i / kPConversionsPerWord] = convert_p(in_conv);
    }
    // Phase 5: write e8m0 P-SF to smem_sfp[sbuf]. Group g owns kblocks [g*2, g*2+1].
    // Derived from SmemLayoutAtomSFP probe ((((_32,_4),_1),(_32,_1)),_1,(_4,_1)) with
    // strides (((16,4),512),(0,1)),_0,(1,512):
    //   byte_offset(row, kblock) = (row%32)*16 + (row/32)*4 + kblock; per stage = +sbuf*512.
#ifndef MXFP8_PSTATIC
    {
      uint8_t* sf_buf = reinterpret_cast<uint8_t*>(storage.smem_sfp.data());
      const int stage_off = sbuf * int(cute::cosize_v<SmemLayoutAtomSFP>);
      const int kblk_base = int(stage) * kNSFBlk;
      const int row = thread_idx;
      const int row_off = (row & 31) * 16 + (row >> 5) * 4;
#if defined(MXFP8_PSF_VEC16)
      // [perf, ported from base lc3b] the two e8m0 blocks (b=0,1) land in
      // ADJACENT, 2-byte-aligned bytes (kblk_base+0,+1); base + row_off +
      // kblk_base is even (row_off mult of 4, kblk_base = stage*2). Coalesce
      // the 2 single-byte stores into one uint16 store -> halves the P-SF
      // store instructions and their bank-conflict wavefronts. Same bytes to
      // the same addresses -> UTCCP read-end (SmemLayoutSFP) + numerics exact.
      {
        static_assert(kNSFBlk == 2, "MXFP8_PSF_VEC16 packs exactly 2 e8m0 blocks");
        uint16_t packed = (uint16_t)((uint8_t)(exp_b[0] + 127)) |
                          (uint16_t)((uint8_t)(exp_b[1] + 127)) << 8;
        *reinterpret_cast<uint16_t*>(&sf_buf[stage_off + row_off + kblk_base]) = packed;
      }
#else
      CUTLASS_PRAGMA_UNROLL
      for (int b = 0; b < kNSFBlk; ++b) {
        uint8_t e8m0 = (uint8_t)(exp_b[b] + 127);
        sf_buf[stage_off + row_off + (kblk_base + b)] = e8m0;
      }
#endif
    }
#endif  // !MXFP8_PSTATIC (Phase 5 compiled out: SFP TMEM is constant-filled once)
#ifdef MXFP8_DBG
    // [续19g] capture SFP exponent per GLOBAL kv-block for row 0 (both tiles).
    if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && thread_idx==0) {
      int kvb = get<1>(tTMEM_LOADcS(0));   // global kv base of this group's half
      for (int b = 0; b < kNSFBlk; ++b) { int gblk = kvb/32 + b; if (gblk < 8) g_dbg_sfp_exp[gblk] = exp_b[b]; }
    }
#endif

    // [PVMX] write packed e4m3 P directly to TMEM P0/P1. The PV MMA reads this
    // TMEM tile as operand A, so the former smem_p STS path is no longer used.
#if defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)
#error "MXFP8_E2OFFLOAD/E2NULL still write their P tail to smem_p; port that tail to TMEM before using TMEM-P PV."
#endif
    copy(tiled_tmem_store, tTMEM_STORErS_x4, tTMEM_STOREtS_x4);
#ifdef MXFP8_DBG
    // smem_p no longer mirrors P; keep g_dbg_Pdq disabled rather than reporting
    // stale smem data.
#endif

    cutlass::arch::fence_view_async_tmem_store();
    // [M3 sub-tile] each group releases ITS OWN S sub-slot once its half of P
    // (+SFP) is written and fenced. The MMA's next QK sub-MMA only reuses THIS
    // group's 64 cols (gated by this release), and PV acquires BOTH
    // sub-pipelines so it still sees the full 128-wide P + SFP.
#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s.consumer_release(pipeline_s_consumer_state);
#endif
#if defined(MXFP8_2SM_BEACON)
    if (is_g0 && cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+5], 3u); __threadfence_system(); } }   // [2SM] softmax did consumer_release(S)
#endif
    ++pipeline_s_consumer_state;

#ifdef MXFP8_E2RSF
    // [刀8 E2RSF] only the final cross-accumulator reduction remains after the
    // release (the per-element pass ran fused in the exp2 burst). Reduction
    // order identical to the standalone pass: acc0+=acc1; acc2+=acc3;
    // acc0+=acc2; sum = x + y.
    cute::add(rsf_acc0, rsf_acc0, rsf_acc1);
    cute::add(rsf_acc2, rsf_acc2, rsf_acc3);
    cute::add(rsf_acc0, rsf_acc0, rsf_acc2);
    row_sum = rsf_acc0.x + rsf_acc0.y;
#else
    ElementQK acc_scale = (old_row_max == row_max_safe) ? 0.5f : 0.5f * ::exp2f(scale * (old_row_max - row_max_safe));
    row_sum *= acc_scale;
    // row_sum = sum(reg_S)
    float2 local_row_sum_f32x2 = make_float2(row_sum, row_sum);
    float2 local_row_sum_1 = make_float2(0, 0);
    float2 local_row_sum_2 = make_float2(0, 0);
    float2 local_row_sum_3 = make_float2(0, 0);

#if defined(MXFP8_E2OFFLOAD) || defined(MXFP8_E2NULL_PERF)
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < kE2Keep; i += 8) {
#else
    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < size(tTMEM_LOADrS); i += 8) {
#endif
      float2 in = make_float2(tTMEM_LOADrS(i), tTMEM_LOADrS(i+1));
      cute::add(local_row_sum_f32x2, local_row_sum_f32x2, in);

      in = make_float2(tTMEM_LOADrS(i+2), tTMEM_LOADrS(i+2+1));
      cute::add(local_row_sum_1, local_row_sum_1, in);

      in = make_float2(tTMEM_LOADrS(i+4), tTMEM_LOADrS(i+4+1));
      cute::add(local_row_sum_2, local_row_sum_2, in);

      in = make_float2(tTMEM_LOADrS(i+6), tTMEM_LOADrS(i+6+1));
      cute::add(local_row_sum_3, local_row_sum_3, in);
    }

    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_1);
    cute::add(local_row_sum_2, local_row_sum_2, local_row_sum_3);
    cute::add(local_row_sum_f32x2, local_row_sum_f32x2, local_row_sum_2);
    float local_row_sum = local_row_sum_f32x2.x + local_row_sum_f32x2.y;

    row_sum = local_row_sum;
#endif

    // [M3-3 LAGGED CHAIN v2] g1's end-of-step chain read (rounds k>=1): pick
    // up g0's round-k publish (w_k = v_{k+1}) for the NEXT tile — off g1's
    // critical path (P for this tile is already written and released).
    if (!is_g0 && !chain_first) {
#ifndef MXFP8_2SM_DECOUPLE_PERF
      order_s.wait();
#endif
      chain_w = smem_sm_exch[0][thread_idx];
#ifndef MXFP8_2SM_DECOUPLE_PERF
      order_s.arrive();
#endif
    }

    // [M3-2 LAZY-MAX] c-pipeline is g0-only again (one corr signal per tile).
#ifndef MXFP8_2SM_DECOUPLE_PERF
    if (is_g0) {
      pipeline_c.producer_acquire(pipeline_c_producer_state);
    }
#endif

    if (final_call) {
      // [M3-2 LAZY-MAX] both groups' row_sum is in the SAME scale (w_{n-1},
      // both hold it in row_max) — the final merge is a plain sum exchange,
      // ONCE per kernel (the per-tile B_REDUCE is GONE).
      smem_sm_exch[stage][thread_idx] = row_sum;
      cutlass::arch::NamedBarrier::arrive_and_wait(256, /*B_FINAL*/ 1u);
      row_sum += smem_sm_exch[stage ^ 1][thread_idx];
#ifdef MXFP8_DBG
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && thread_idx==0 && is_g0)
        g_dbg_rowsum = row_sum;
#endif

      // [M3 sub-tile] both groups re-wait their OWN pipeline in the final step
      // (op-count symmetry with the mma's n+2 commits per sub-pipeline); only
      // g0 writes the final V-stats (to ITS slot s_base+0 — corr tail reads there).
#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s.consumer_wait(pipeline_s_consumer_state);
#endif
      if (is_g0) {
        Tensor tTMEM_STOREVrS = make_tensor<ElementQK>(shape(tTMEM_STOREVcS));
        tTMEM_STOREVrS(kIdxFinalRowMax) = row_max;
        tTMEM_STOREVrS(kIdxFinalRowSum) = row_sum;
        copy(tiled_tmem_storev, tTMEM_STOREVrS, tTMEM_STOREVtS);
      }
    }
  }

  template<class Stage, class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE auto
  softmax(
      Stage stage,
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,    // [PVMX 2a.0] softmax writes P -> storage.smem_p
      PipelineS& pipeline_s, typename PipelineS::PipelineState& pipeline_s_consumer_state,
      PipelineC& pipeline_c, typename PipelineC::PipelineState& pipeline_c_producer_state,
      OrderBarrierSoftmax& order_s) {

    int mask_tile_count = Mask{}.get_unmasked_trip_count(blk_coord, TileShape{}, problem_shape);

    ElementQK row_max = -INFINITY;
    ElementQK row_sum = 0;
    ElementQK chain_w = -INFINITY;   // [M3-3] g0's true chain register (w_k)
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+5], 1u); __threadfence_system(); } }   // [2SM] softmax entered
#endif

    Tensor cS_base = make_identity_tensor(select<0,1>(TileShapeQK{}));
    auto logical_offset = make_coord(
        get<0>(blk_coord) * get<0>(TileShape{}) + (stage % get<0>(ThreadShape{})) * get<0>(TileShapeQK{}),
        0 + (stage % get<1>(ThreadShape{})) * get<1>(TileShapeQK{})
    );
    Tensor cS = domain_offset(logical_offset, cS_base);

    // [M3-2 LAZY-MAX] g0 drives s0 + s0_corr; g1 drives s1 only (s1_corr
    // inert). Cross-group coupling = the order_s one-way w-chain handoff
    // inside softmax_step + one final sum merge.
    const bool is_g0 = (stage == 0);

#ifdef MXFP8_PSTATIC
    // [PSTATIC] fill THIS CTA's smem_sfp ONCE with the constant e8m0 byte
    // (127 + MXFP8_PSTATIC_EXP). Layout-agnostic: every byte is the same
    // constant. g0's 128 threads fill stage 0 (512 B), g1's fill stage 1.
    // Ordering vs the MMA's one-shot SFP UTCCP (first_pv site): this fill
    // happens-before this thread's tile-0 fence_view_async_shared() + S
    // release in softmax_step, which is exactly what gates the UTCCP today
    // (strictly earlier in program order than the old per-tile Phase-5 store).
    {
      int tfill = int(threadIdx.x) % (4 * cutlass::NumThreadsPerWarp);
      constexpr uint32_t kSFByte = uint32_t(kMXFP8PStaticSfpByte) & 0xFFu;
      constexpr uint32_t kSFWord = 0x01010101u * kSFByte;
      uint32_t* sf_w = reinterpret_cast<uint32_t*>(storage.smem_sfp.data());
      sf_w[(is_g0 ? 0 : 128) + tfill] = kSFWord;
    }
#endif

#ifndef MXFP8_2SM_DECOUPLE_PERF
    if (is_g0) {
      pipeline_c.producer_acquire(pipeline_c_producer_state);
    }
#endif

    // chain_first: g0's first sub-step of THIS tile-loop starts the global
    // online-max chain (skips the smem chain read).
    bool first_step = true;

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step<false /* need_apply_mask */>(
          row_max, row_sum, chain_w, stage,
          (mask_tile_count == 1) &&
              (Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape) == 0),
          /*chain_first=*/first_step,
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s
      );
      first_step = false;
#if defined(MXFP8_2SM_BEACON)
      if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+5], 2u); __threadfence_system(); } }   // [2SM] softmax completed a step (consumed S, produced P)
#endif

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    // Masked iterations
    mask_tile_count = Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape);

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step<true /* need_apply_mask */>(
          row_max, row_sum, chain_w, stage, mask_tile_count == 1,
          /*chain_first=*/first_step,
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s, pipeline_s_consumer_state,
          pipeline_c, pipeline_c_producer_state,
          order_s
      );
      first_step = false;

      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    // [M3 sub-tile] correction signalling stays g0-only (s1_corr inert), but
    // BOTH groups now run the trailing S-pipeline balancing on their OWN
    // sub-pipeline: the mma() issues n+2 commits per sub-pipeline (n QK subs +
    // 2 tail balancing): n consumed by softmax_step, one by the final_call
    // re-wait, so TWO trailing empty wait/release pairs balance each pipeline.
#ifndef MXFP8_2SM_DECOUPLE_PERF
    if (is_g0) {
      pipeline_c.producer_commit(pipeline_c_producer_state);
      ++pipeline_c_producer_state;

      pipeline_c.producer_acquire(pipeline_c_producer_state);
    }
    pipeline_s.consumer_release(pipeline_s_consumer_state);
    ++pipeline_s_consumer_state;
    pipeline_s.consumer_wait(pipeline_s_consumer_state);
    pipeline_s.consumer_release(pipeline_s_consumer_state);
    ++pipeline_s_consumer_state;
#else
    // [DECOUPLE PROBE] advance the states through the trailing ops so the
    // (unused) phase bookkeeping stays consistent; no waits, no releases.
    if (is_g0) {
      ++pipeline_c_producer_state;
    }
    ++pipeline_s_consumer_state;
    ++pipeline_s_consumer_state;
#endif
  }

#ifdef MXFP8_SM12
  // ════════════════════════════════════════════════════════════════════
  // [刀15 SM12] 12-warp softmax, correction merged into G0.
  // Column piece table per group: {colA, widthA, colB, widthB} in ABSOLUTE
  // S columns (0-127; >=64 = the s1 sub-tile half). Widths are multiples of
  // 16 (STS.128 granule; 16-col TMEM ld offsets proven by today's pHalf=16).
  // 32-wide pieces sit at 32-aligned offsets, 16-wide at 16-aligned.
  // ════════════════════════════════════════════════════════════════════
#ifdef MXFP8_SM12_SKELETON
  // Skeleton: G0/G1 = today's 64/64 split (G0<-s0, G1<-s1), G2 = 0 columns
  // (escort-only: order_s + B_FINAL participation, no S/P/pipeline work).
  // Validates the 12-warp schedule / 160-reg quota / 3-group chain /
  // PipelineC retirement / correction-in-G0 without touching the column map.
  static constexpr int kSm12Col[3][4] = {{0,64, 0,0}, {64,64, 0,0}, {0,0, 0,0}};
#else
  // Real three-split (granules of 16): G0 = s0[0:32) — lightest, it also owns
  // the chain + correction segment + tail epilogue; G1 = s0[32:64) + s1[64:80);
  // G2 = s1[96:128) + s1[80:96). Critical exp2 chain per warp: 64 -> 48.
  static constexpr int kSm12Col[3][4] = {{0,32, 0,0}, {32,32, 64,16}, {96,32, 80,16}};
#endif
  static constexpr bool sm12_uses_s0(int g) {
    return (kSm12Col[g][1] > 0 && kSm12Col[g][0] < 64) || (kSm12Col[g][3] > 0 && kSm12Col[g][2] < 64);
  }
  static constexpr bool sm12_uses_s1(int g) {
    return (kSm12Col[g][1] > 0 && kSm12Col[g][0] >= 64) || (kSm12Col[g][3] > 0 && kSm12Col[g][2] >= 64);
  }

#ifdef MXFP8_SM12_OWAIT
  // [SM12 OWAIT] w15's dedicated O-waiter loop. Per tile t (1..n-1):
  //   wait full(t-1)  [absorbs the PV completion latency IN PARALLEL]
  //   publish full_seq = t  (G0's rescale path spins on this)
  //   spin each G0 warp's ring-4 flag for tile t; if a warp flagged need,
  //   spin its done counter (its 32-row rescale finished)
  //   consumer_release(t-1)  -> the next PV's producer_acquire opens.
  // Tail: wait full(n-1), publish, spin epi_done==4 (all G0 warps' tail
  // epilogue TMEM reads retired), release.
  // Deadlock DAG: full(t-1) <- PV(t-1) <- [release(t-2) by w15 (prior iter),
  // P-done(t-1)]; flags <- G0 step t (gated only by QK); done <- G0's rescale
  // <- full_seq (this warp, already published). No cycle through G0's
  // final_call re-wait: w15's releases never depend on the MMA tail.
  template<class BlkCoord, class ProblemShape>
  CUTLASS_DEVICE void
  sm12_owaiter(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      PipelineO& pipeline_o, typename PipelineO::PipelineState& o_state,
      uint32_t* osync_raw) {

    volatile uint32_t* osync = osync_raw;
    int n = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    CUTLASS_PRAGMA_NO_UNROLL
    for (int t = 1; t < n; ++t) {
      pipeline_o.consumer_wait(o_state);              // O(t-1) full
      if (cute::elect_one_sync()) { osync[0] = uint32_t(t); }   // full_seq
      // gather the 4 G0 warps' (seq, need) flags for tile t (ring-4 slots).
      uint32_t need_mask = 0;
      CUTLASS_PRAGMA_UNROLL
      for (int w = 0; w < 4; ++w) {
        uint32_t v;
        do { v = osync[2 + w * 4 + (t & 3)]; } while ((int)(v >> 1) < t);
        need_mask |= (v & 1u) << w;
      }
      if (need_mask) {
        CUTLASS_PRAGMA_UNROLL
        for (int w = 0; w < 4; ++w) {
          if (need_mask & (1u << w)) {
            while ((int)osync[18 + w] < t) { }        // warp w's rescale done
          }
        }
        __threadfence_block();
      }
      pipeline_o.consumer_release(o_state);           // opens PV(t)'s acquire
      ++o_state;
    }
    // tail: O(n-1) -> G0's epilogue reads it; release after all 4 G0 warps
    // signalled epi_done.
    pipeline_o.consumer_wait(o_state);
    if (cute::elect_one_sync()) { osync[0] = uint32_t(n); }
    while (osync[1] < 4u) { }
    __threadfence_block();
    pipeline_o.consumer_release(o_state);
    ++o_state;
  }
#endif  // MXFP8_SM12_OWAIT

  template<bool need_apply_mask, int kGroup, class BlkCoord, class CoordTensor, class ProblemShape>
  CUTLASS_DEVICE void
  softmax_step12(
      float& row_max, float& row_sum, float& chain_w,
      float& olag_old, float& olag_new, bool& olag_valid,   // [OLAG] G0's lagged rescale pair (v_{t-1}, v_t)
      int step_t,                                           // tile index (== chain_first ? 0 : t)
      bool final_call, bool chain_first,
      BlkCoord const& blk_coord, CoordTensor const& cS,
      Params const& params, ProblemShape const& problem_shape,
      TensorStorage& storage,
      PipelineS& pipeline_s0, typename PipelineS::PipelineState& s0_state,
      PipelineS& pipeline_s1, typename PipelineS::PipelineState& s1_state,
      PipelineO& pipeline_o, typename PipelineO::PipelineState& o_state,
      OrderBarrierSoftmax& order_s
#ifdef MXFP8_SM12_OWAIT
      , uint32_t* osync_raw
#endif
      ) {

    constexpr int  kColA  = kSm12Col[kGroup][0];
    constexpr int  kWA    = kSm12Col[kGroup][1];
    constexpr int  kColB  = kSm12Col[kGroup][2];
    constexpr int  kWB    = kSm12Col[kGroup][3];
    constexpr bool kUsesS0 = sm12_uses_s0(kGroup);
    constexpr bool kUsesS1 = sm12_uses_s1(kGroup);
    constexpr bool kHasWork = (kWA + kWB) > 0;

    // [SM12] 3-slot exchange: [0] chain publish, [1]/[2] G1/G2 slice maxes
    // (per step) doubling as the final row_sum slots.
    float (&smem_sm_exch)[3][128] =
        *reinterpret_cast<float(*)[3][128]>(storage.smem_softmax_exch.data());

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    Tensor tScS_full = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);
    Tensor tStS_full = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));
    // buffer index: this group's member pipelines advance in lockstep with the
    // QK commits, so any member state's index() selects the right S buffer.
    int sbuf = kUsesS0 ? s0_state.index() : s1_state.index();
    uint32_t s_base = (sbuf == 0 ? uint32_t(TmemAllocation::S0) : uint32_t(TmemAllocation::S1));

    // per-piece TMEM ld: build the (128, W) sub-view at absolute column C,
    // coordinate-offset so ResidualMask sees global kv (the [mask fix] rule).
    // `livec` == false_type for a zero-width group's dummy piece: NO tcgen05.ld
    // may be issued then (续19w: any extra tcgen05.ld in a softmax warp that
    // did not consumer_wait hangs the cooperative pipeline).
    auto load_piece = [&](auto colc, auto wc, auto livec) {
      constexpr int W = decltype(wc)::value;
      using ATOM = std::conditional_t<W == 64, SM100_TMEM_LOAD_32dp32b64x,
                   std::conditional_t<W == 32, SM100_TMEM_LOAD_32dp32b32x,
                                               SM100_TMEM_LOAD_32dp32b16x>>;
      Tensor tStS_p = tStS_full.compose(make_layout(make_shape(_128{}, wc)));
      tStS_p.data() = warp_uniform(s_base + uint32_t(decltype(colc)::value));
      Tensor tScS_p = domain_offset(make_coord(_0{}, colc),
                                    tScS_full.compose(make_layout(make_shape(_128{}, wc))));
      auto tld  = make_tmem_copy(ATOM{}, tStS_p);
      auto thr  = tld.get_slice(thread_idx);
      Tensor tS  = thr.partition_S(tStS_p);
      Tensor cS_ = thr.partition_D(tScS_p);
      Tensor rS  = make_tensor<ElementQK>(shape(cS_));
      if constexpr (decltype(livec)::value) {
        copy(tld, tS, rS);
        if constexpr (need_apply_mask) {
          Mask{}.apply_mask(rS, cS_, problem_shape);
        }
      }
      return rS;
    };
    auto piece_max = [&](auto const& rS) {
      float m0 = -INFINITY, m1 = -INFINITY, m2 = -INFINITY, m3 = -INFINITY;
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < size(rS); i += 4) {
        m0 = ::fmax(m0, rS(i));
        m1 = ::fmax(m1, rS(i+1));
        m2 = ::fmax(m2, rS(i+2));
        m3 = ::fmax(m3, rS(i+3));
      }
      return ::fmax(::fmax(m0, m1), ::fmax(m2, m3));
    };

    ElementQK old_row_max = row_max;   // v_{k-1} — G0's correction stats source

    // ---- load + local max (raw S must be maxed before in-place exp2) ----
    if constexpr (kUsesS0) { pipeline_s0.consumer_wait(s0_state); }
    if constexpr (!kUsesS0 && kUsesS1) { pipeline_s1.consumer_wait(s1_state); }
    auto rA = load_piece(Int<kColA>{}, Int<(kWA > 0 ? kWA : 16)>{}, cute::bool_constant<(kWA > 0)>{});
    if constexpr (kUsesS0 && kUsesS1) { pipeline_s1.consumer_wait(s1_state); }
    auto rB = load_piece(Int<kColB>{}, Int<(kWB > 0 ? kWB : 16)>{}, cute::bool_constant<(kWB > 0)>{});
    float localM = -INFINITY;
    if constexpr (kWA > 0) { localM = ::fmax(localM, piece_max(rA)); }
    if constexpr (kWB > 0) { localM = ::fmax(localM, piece_max(rB)); }

    // ---- chain section (lazy w-chain, full-width lag-1 coverage) ----
    // w_k = max(w_{k-1}, G0 local(k), m1(k-1), m2(k-1)); P scale v_k = w_{k-1}
    // (v_0 = w_0). Monotone ⟹ mathematically exact; PSTATIC saturation risk
    // is judged by the sweep gate (coverage ⊃ today's 64-col-only chain).
    if constexpr (kGroup == 0) {
      order_s.wait();                      // g2's previous-round arrive
      float w_k;
      if (chain_first) {
        w_k = ::fmax(chain_w, localM);
      } else {
        float m1 = smem_sm_exch[1][thread_idx];
        float m2 = smem_sm_exch[2][thread_idx];
        w_k = ::fmax(::fmax(chain_w, localM), ::fmax(m1, m2));
      }
      smem_sm_exch[0][thread_idx] = w_k;   // publish (G1/G2 read as v_{k+1})
      order_s.arrive();
      row_max = chain_first ? w_k : chain_w;   // v_k drives P/row_sum downstream
      chain_w = w_k;
    } else {
      if (chain_first) {
        // inline pair (no cache yet): read v_0 = w_0, publish own tile-0 max.
        order_s.wait();
        chain_w = smem_sm_exch[0][thread_idx];
        smem_sm_exch[kGroup][thread_idx] = localM;
        order_s.arrive();
      }
      row_max = chain_w;                   // cached v_k
    }

    ElementQK row_max_safe = row_max == -INFINITY ? 0 : row_max;

    ElementQK scale = params.scale_softmax_log2;
    ElementQK row_max_scale = row_max_safe * scale;
    float2 scale_fp32x2 = make_float2(scale, scale);
    float2 minus_row_max_scale_fp32x2 = make_float2(-row_max_scale, -row_max_scale);

#ifdef MXFP8_SM12_OWAIT
    // [OWAIT v4b] EARLY flag publish — right after the chain section, BEFORE
    // the exp2 burst. w15's release(t-1) gates on this flag; publishing it at
    // the step END (v4a) re-serialized the release behind G0's whole step
    // (px15d 957/893 — worse than v1). Early publish lets w15 finish its
    // [full(t-1) + flags(t)] gathering DURING G0's exp2, so release(t-1)
    // tracks PV(t-1) completion (the dedicated-waiter ideal). The (rare)
    // rescale work itself stays late (after the S release, off the P path).
    float sm12_o_scale = 1.0f;
    bool  sm12_o_need  = false;
    if constexpr (kGroup == 0) {
      volatile uint32_t* osync = osync_raw;
      if (!chain_first) {
        ElementQK old_safe = (old_row_max == -INFINITY) ? row_max_safe : old_row_max;
        sm12_o_scale = ::exp2f(scale * (old_safe - row_max_safe));
        sm12_o_need  = __any_sync(0xffffffffu, sm12_o_scale != 1.0f);
        const int w15w = (int(threadIdx.x) >> 5) & 3;
        __syncwarp();
        if (cute::elect_one_sync()) {
          osync[2 + w15w * 4 + (step_t & 3)] = (uint32_t(step_t) << 1) | (sm12_o_need ? 1u : 0u);
        }
      }
    }
#endif

    // [刀8 E2RSF] lagged fused row_sum (accumulators shared across pieces;
    // per-piece local lag indices — new association, gated by the sweep).
    {
      ElementQK rsf_acc_scale = (old_row_max == row_max_safe) ? 0.5f : 0.5f * ::exp2f(scale * (old_row_max - row_max_safe));
      row_sum *= rsf_acc_scale;
    }
    float2 rsf_acc0 = make_float2(row_sum, row_sum);
    float2 rsf_acc1 = make_float2(0, 0);
    float2 rsf_acc2 = make_float2(0, 0);
    float2 rsf_acc3 = make_float2(0, 0);

    NumericArrayConverter<Element, ElementQK, 2> convert;
    Tensor sP_st = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{})(_, _, _, sbuf);
    const int row = thread_idx;

    // exp2 (in place) + E2RSF lagged sum + e4m3 convert + STS.128 granules.
    auto exp2_sts_piece = [&](auto colc, auto& rS) {
      constexpr int C = decltype(colc)::value;
      constexpr int W = decltype(size(rS))::value;
      static_assert((W % 16) == 0 && W >= MXFP8_E2RSF_LAG, "SM12 piece width: 16-col granules, >= E2RSF lag");
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < W; i += 2) {
        float2 in = make_float2(rS(i+0), rS(i+1));
        float2 out;
        cute::fma(out, scale_fp32x2, in, minus_row_max_scale_fp32x2);
        rS(i+0) = EXP2_SOFTMAX_MIX(i, out.x);
        rS(i+1) = EXP2_SOFTMAX_MIX(i, out.y);
        if (i >= MXFP8_E2RSF_LAG) {
          const int j = i - MXFP8_E2RSF_LAG;
          float2 rsf_in = make_float2(rS(j+0), rS(j+1));
          if      ((j & 7) == 0) cute::add(rsf_acc0, rsf_acc0, rsf_in);
          else if ((j & 7) == 2) cute::add(rsf_acc1, rsf_acc1, rsf_in);
          else if ((j & 7) == 4) cute::add(rsf_acc2, rsf_acc2, rsf_in);
          else                   cute::add(rsf_acc3, rsf_acc3, rsf_in);
        }
      }
      CUTLASS_PRAGMA_UNROLL
      for (int j = W - MXFP8_E2RSF_LAG; j < W; j += 2) {
        float2 rsf_in = make_float2(rS(j+0), rS(j+1));
        if      ((j & 7) == 0) cute::add(rsf_acc0, rsf_acc0, rsf_in);
        else if ((j & 7) == 2) cute::add(rsf_acc1, rsf_acc1, rsf_in);
        else if ((j & 7) == 4) cute::add(rsf_acc2, rsf_acc2, rsf_in);
        else                   cute::add(rsf_acc3, rsf_acc3, rsf_in);
      }
      // convert (PSTATIC: constant SF — EXP==0 means no per-element rescale)
      Tensor pw = make_tensor<uint32_t>(Int<W / 4>{});
      Tensor pw_e = recast<Array<Element, 2>>(pw);
#if (MXFP8_PSTATIC_EXP) != 0
      const float sinv = __uint_as_float((uint32_t)(127 - (MXFP8_PSTATIC_EXP)) << 23);
#endif
      CUTLASS_PRAGMA_UNROLL
      for (int i = 0; i < W; i += 2) {
        Array<ElementQK, 2> in_conv;
#if (MXFP8_PSTATIC_EXP) != 0
        in_conv[0] = rS(i+0) * sinv;
        in_conv[1] = rS(i+1) * sinv;
#else
        in_conv[0] = rS(i+0);
        in_conv[1] = rS(i+1);
#endif
        pw_e[i / 2] = convert(in_conv);
      }
      // STS.128 per 16-col granule at absolute byte col kg0 (same swizzle
      // recipe as the 8-warp path — kbase generalized to the piece base).
      CUTLASS_PRAGMA_UNROLL
      for (int g = 0; g < W / 16; ++g) {
        const int kg0 = C + 16 * g;
        Element& dst_byte0 = sP_st(make_coord(row, kg0 % 32), _0{}, kg0 / 32);
        const uint32_t dst_addr = cute::cast_smem_ptr_to_uint(&dst_byte0);
        asm volatile("st.shared.v4.u32 [%0], {%1, %2, %3, %4};" ::
                     "r"(dst_addr),
                     "r"(pw(4*g+0)), "r"(pw(4*g+1)),
                     "r"(pw(4*g+2)), "r"(pw(4*g+3)));
      }
    };

    if constexpr (kWA > 0) { exp2_sts_piece(Int<kColA>{}, rA); }
    if constexpr (kWB > 0) { exp2_sts_piece(Int<kColB>{}, rB); }

    if constexpr (kHasWork) {
      cutlass::arch::fence_view_async_shared();
    }

    // ---- release this group's S sub-slots (P-done arrivals for the PV) ----
    if constexpr (kUsesS0) {
      pipeline_s0.consumer_release(s0_state);
      ++s0_state;
    }
    if constexpr (kUsesS1) {
      pipeline_s1.consumer_release(s1_state);
      ++s1_state;
    }

    // ---- G1/G2 end-of-step chain pair (k>=1): publish slice max, cache w_k ----
    if constexpr (kGroup != 0) {
      if (!chain_first) {
        order_s.wait();
        smem_sm_exch[kGroup][thread_idx] = localM;   // m_g(k) for G0's w_{k+1}
        chain_w = smem_sm_exch[0][thread_idx];       // v_{k+1} cache
        order_s.arrive();
      }
    }

    // ---- E2RSF final reduce (post-release: shadow-time-free) ----
    cute::add(rsf_acc0, rsf_acc0, rsf_acc1);
    cute::add(rsf_acc2, rsf_acc2, rsf_acc3);
    cute::add(rsf_acc0, rsf_acc0, rsf_acc2);
    row_sum = rsf_acc0.x + rsf_acc0.y;

    // ---- [SM12 correction segment, G0 only, softmax-first ruling] ----
    // O rescale from REGISTER stats (PipelineC + V-stats retired).
#ifdef MXFP8_SM12_OWAIT
    // [SM12 v4 OWAIT] w15 owns the O pipeline wait/release; G0 only (a)
    // publishes a per-warp (seq, need) flag each tile and (b) on the RARE
    // rescale tiles spins w15's full_seq, rescales its own 32 rows, then
    // signals done. The lazy-chain common case (scale==1) costs G0 one smem
    // store — the PV-completion wait lives entirely in w15 (the dedicated-
    // waiter semantics the pre-SM12 correction group provided, restored at
    // a cost of ONE warp instead of four).
    if constexpr (kGroup == 0) {
      volatile uint32_t* osync = osync_raw;
      // flag was published EARLY (pre-exp2, v4b); only the rare rescale here.
      if (!chain_first && sm12_o_need) {
        const int w15w = (int(threadIdx.x) >> 5) & 3;   // G0 warp id 0-3
        while ((int)osync[0] < step_t) { }              // O(t-1) full (w15's full_seq)
        correction_rescale(sm12_o_scale, uint32_t(TmemAllocation::O0));
        cutlass::arch::fence_view_async_tmem_store();
        __syncwarp();
        if (cute::elect_one_sync()) {
          __threadfence_block();
          osync[18 + w15w] = uint32_t(step_t);          // done -> w15 releases
        }
      }
    }
#else
    // ★ OSKIP TOMBSTONE (v2, 2026-06-13): "scale==1 ⟹ release O without
    // waiting full" DEADLOCKS (beacon: MMA stuck at BCN 8 = the FIRST
    // pipeline_corr.producer_acquire). Root cause: severing the
    // [release#k happens-after commit#k] edge lets G0's release#0 overtake
    // the producer's acquire#0 — the early empty-barrier flip consumes the
    // producer start-state's virtual credit, so acquire#0 waits a flip that
    // now requires release#1 ⟸ PV(0) ⟸ acquire#0: cycle. The skip is
    // PROTOCOL-unsound for any mbarrier pipeline (not fixable by phasing).
#ifdef MXFP8_SM12_OLAG
    // [SM12 v3 OLAG] lag-1 correction: step t handles O(t-2) (not O(t-1)).
    // PV(t-2) was issued ~one full step earlier, so the consumer_wait is WARM
    // — the PV-completion latency that v1 serialized into G0's loop (px15a
    // -89/-101 vs px8a; skeleton -108) is absorbed by the lag instead of a
    // dedicated warp. All protocol edges are STANDARD (wait-then-release);
    // the only changes are which stage is handled (lag) and the lagged scale
    // pair: O at step t holds PV(<= t-2) in scale v_{t-2}; rescale by
    // exp2(sl·(v_{t-2} − v_{t-1})) = the pair SAVED at step t-1 — before
    // PV(t-1) (gated by this release) accumulates in scale v_{t-1}.
    // Net cadence: each PV issues ~half a step later (acquire granted at the
    // end of the NEXT softmax step); steady-state 1 release/step unchanged.
    // The leftover O(n-2) is handled in the softmax12 tail.
    if constexpr (kGroup == 0) {
      auto olag_handle = [&]() {
        float o_scale = ::exp2f(scale * (olag_old - olag_new));
        pipeline_o.consumer_wait(o_state);
        if (__any_sync(0xffffffffu, o_scale != 1.0f)) {
          correction_rescale(o_scale, uint32_t(TmemAllocation::O0));
          cutlass::arch::fence_view_async_tmem_store();
        }
        pipeline_o.consumer_release(o_state);
        ++o_state;
      };
      if (olag_valid) { olag_handle(); }              // lagged: O(t-2)
      // save this step's pair for the NEXT step's (or the flush's) rescale.
      olag_old = (old_row_max == -INFINITY) ? row_max_safe : old_row_max;
      olag_new = row_max_safe;
      olag_valid = !chain_first;   // pair (v_{t-1} -> v_t) exists from t>=1
      // [OLAG FLUSH — MUST be BEFORE the final_call block] the leftover
      // O(n-2) (fresh pair v_{n-2}->v_{n-1}). Deadlock triangle if deferred
      // to the softmax12 tail (px15c hang, 2026-06-13): G0's final_call
      // re-wait consumes the MMA tail's BALANCING commit, which the MMA
      // issues only AFTER its tail corr.producer_acquire#(n-1) — and that
      // acquire waits exactly this release#(n-2). Flushing here keeps
      // release#(n-2) upstream of the re-wait: cycle broken.
      if (final_call && olag_valid) {
        olag_handle();                                // flush: O(n-1 - 1)
        olag_valid = false;                           // tail: only O(n-1) epilogue
      }
    }
#else
    // [SM12 v1] in-step O(t-1) wait: P(t) is already written and released by
    // now, so O-release lands ≈ P-done(t) — the 刀13 placement tax (P gated
    // behind wait-O) cannot occur. But the PV(t-1)-completion WAIT itself
    // serializes into G0's loop (the dedicated correction warp used to absorb
    // it in parallel) — measured -89/-101/-73 vs px8a; superseded by OLAG.
    if constexpr (kGroup == 0) {
      if (!chain_first) {
        ElementQK old_safe = (old_row_max == -INFINITY) ? row_max_safe : old_row_max;
        float o_scale = ::exp2f(scale * (old_safe - row_max_safe));
        pipeline_o.consumer_wait(o_state);
        if (__any_sync(0xffffffffu, o_scale != 1.0f)) {
          correction_rescale(o_scale, uint32_t(TmemAllocation::O0));
          cutlass::arch::fence_view_async_tmem_store();
        }
        pipeline_o.consumer_release(o_state);
        ++o_state;
      }
    }
#endif  // MXFP8_SM12_OLAG / v1
#endif  // MXFP8_SM12_OWAIT

    if (final_call) {
      // 3-way row_sum merge (all in the shared scale v_{n-1}); only G0 needs
      // the total (tail normalize + LSE). Slot reuse after the barrier is the
      // same benign pattern as today's [0]-slot final write.
      smem_sm_exch[kGroup][thread_idx] = row_sum;
      cutlass::arch::NamedBarrier::arrive_and_wait(384, /*B_FINAL*/ 1u);   // [SM12] 256 -> 384 (12 warps)
      if constexpr (kGroup == 0) {
        row_sum += smem_sm_exch[1][thread_idx] + smem_sm_exch[2][thread_idx];
      }
      // op-count symmetry re-wait on member pipelines (producer commits n+2).
      if constexpr (kUsesS0) { pipeline_s0.consumer_wait(s0_state); }
      if constexpr (kUsesS1) { pipeline_s1.consumer_wait(s1_state); }
    }
  }

  template<
    int kGroup, class BlkCoord, class ProblemShape, class ParamsProblemShape,
    class TensorStorageEpi, class CollectiveEpilogue
  >
  CUTLASS_DEVICE void
  softmax12(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      ParamsProblemShape const& params_problem_shape,
      TensorStorage& storage,
      TensorStorageEpi& shared_storage_epi,
      PipelineS& pipeline_s0, typename PipelineS::PipelineState& s0_state,
      PipelineS& pipeline_s1, typename PipelineS::PipelineState& s1_state,
      PipelineO& pipeline_o, typename PipelineO::PipelineState& o_state,
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& epi_state,
      OrderBarrierSoftmax& order_s,
      CollectiveEpilogue& epilogue
#ifdef MXFP8_SM12_OWAIT
      , uint32_t* osync_raw
#endif
      ) {

#if defined(MXFP8_SM12_OWAIT) && defined(MXFP8_SM12_OLAG)
#error "MXFP8_SM12_OWAIT and MXFP8_SM12_OLAG are mutually exclusive correction placements"
#endif

    constexpr bool kUsesS0 = sm12_uses_s0(kGroup);
    constexpr bool kUsesS1 = sm12_uses_s1(kGroup);

    int mask_tile_count = Mask{}.get_unmasked_trip_count(blk_coord, TileShape{}, problem_shape);

    ElementQK row_max = -INFINITY;
    ElementQK row_sum = 0;
    ElementQK chain_w = -INFINITY;
    // [OLAG] G0's lagged rescale pair: saved at step t, applied to O(t-1) at
    // step t+1 (or the tail). valid from step >= 1's save.
    float olag_old = 0.0f, olag_new = 0.0f;
    bool  olag_valid = false;

    Tensor cS_base = make_identity_tensor(select<0,1>(TileShapeQK{}));
    // single-stage ThreadShape<1,1,1>: all groups share the same tile coords.
    auto logical_offset = make_coord(get<0>(blk_coord) * get<0>(TileShape{}), 0);
    Tensor cS = domain_offset(logical_offset, cS_base);

    // [PSTATIC] constant smem_sfp fill: G0 -> stage 0, G1 -> stage 1 (same
    // happens-before chain as the 8-warp path: fill precedes this group's
    // tile-0 fence + S release, which gates the MMA's one-shot SFP UTCCP).
    if constexpr (kGroup < 2) {
      int tfill = int(threadIdx.x) % (4 * cutlass::NumThreadsPerWarp);
      constexpr uint32_t kSFByte = uint32_t(kMXFP8PStaticSfpByte) & 0xFFu;
      constexpr uint32_t kSFWord = 0x01010101u * kSFByte;
      uint32_t* sf_w = reinterpret_cast<uint32_t*>(storage.smem_sfp.data());
      sf_w[(kGroup == 0 ? 0 : 128) + tfill] = kSFWord;
    }

    bool first_step = true;
    int step_t = 0;
#ifdef MXFP8_SM12_OWAIT
#define SM12_STEP_EXTRA , osync_raw
#else
#define SM12_STEP_EXTRA
#endif

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step12<false /* need_apply_mask */, kGroup>(
          row_max, row_sum, chain_w,
          olag_old, olag_new, olag_valid, step_t,
          (mask_tile_count == 1) &&
              (Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape) == 0),
          /*chain_first=*/first_step,
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s0, s0_state, pipeline_s1, s1_state,
          pipeline_o, o_state, order_s SM12_STEP_EXTRA);
      first_step = false;
      ++step_t;
      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }

    mask_tile_count = Mask{}.get_masked_trip_count(blk_coord, TileShape{}, problem_shape);

    CUTLASS_PRAGMA_NO_UNROLL
    for (; mask_tile_count > 0; mask_tile_count -= 1) {
      softmax_step12<true /* need_apply_mask */, kGroup>(
          row_max, row_sum, chain_w,
          olag_old, olag_new, olag_valid, step_t,
          mask_tile_count == 1,
          /*chain_first=*/first_step,
          blk_coord, cS, params, problem_shape, storage,
          pipeline_s0, s0_state, pipeline_s1, s1_state,
          pipeline_o, o_state, order_s SM12_STEP_EXTRA);
      first_step = false;
      ++step_t;
      cS.data() = cS.data() + E<1>{} * get<1>(ThreadShape{}) * get<1>(TileShapeQK{});
    }
#undef SM12_STEP_EXTRA

    // trailing S-pipeline balance per membership (producer commits n+2 per
    // sub-pipeline: n steps + final re-wait consumed n+1 — two trailing
    // empty wait/release pairs balance each member pipeline, exactly as the
    // 8-warp path).
    if constexpr (kUsesS0) {
      pipeline_s0.consumer_release(s0_state);
      ++s0_state;
      pipeline_s0.consumer_wait(s0_state);
      pipeline_s0.consumer_release(s0_state);
      ++s0_state;
    }
    if constexpr (kUsesS1) {
      pipeline_s1.consumer_release(s1_state);
      ++s1_state;
      pipeline_s1.consumer_wait(s1_state);
      pipeline_s1.consumer_release(s1_state);
      ++s1_state;
    }

    // ---- [SM12] G0 tail: ex-correction final normalize + LSE + epilogue ----
    // row_sum was 3-way merged at final_call; row_max = v_{n-1} — both in
    // registers (the V-stats TMEM round-trip is gone).
    if constexpr (kGroup == 0) {
      // [OLAG] the O(n-2) flush moved INTO the final softmax_step12 (before
      // its final_call block) — deferring it here deadlocked: the final_call
      // re-wait <- MMA balancing commit <- tail corr.acquire <- this release.
      (void)olag_old; (void)olag_new; (void)olag_valid;
#ifdef MXFP8_SM12_OWAIT
      // [OWAIT] w15 owns the O pipeline: spin its full_seq for O(n-1), do the
      // epilogue, then signal epi_done (w15 releases O(n-1) at count 4).
      volatile uint32_t* osync = osync_raw;
      const int sm12_n = step_t;                      // total tiles processed
      while ((int)osync[0] < sm12_n) { }              // O(n-1) full
#else
      pipeline_o.consumer_wait(o_state);              // O(n-1) full
#endif
      pipeline_epi.producer_acquire(epi_state);

      Tensor sO = make_tensor(make_smem_ptr(shared_storage_epi.smem_o.data()), typename TensorStorageEpi::SmemLayoutO{});
      correction_epilogue(params.scale_output / row_sum, _0{}, sO);

      if (epilogue.params.ptr_LSE != nullptr) {
        Tensor gLSE = make_tensor(make_gmem_ptr(epilogue.params.ptr_LSE), select<0,3>(problem_shape), epilogue.params.dLSE);
        int thread_idx = int(threadIdx.x) % (4 * cutlass::NumThreadsPerWarp);
        int row_idx = thread_idx + get<0>(TileShape{}) * get<0>(blk_coord);
        int row_offset = 0;
        if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
          row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
        }
        ElementPV lse = cutlass::fast_log(row_sum) + params.scale_softmax * row_max;
        if (row_idx < get<0>(problem_shape)) {
          gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
        }
      }

      cutlass::arch::fence_view_async_tmem_load();
#ifdef MXFP8_SM12_OWAIT
      __syncwarp();
      if (cute::elect_one_sync()) {
        __threadfence_block();
        atomicAdd(const_cast<uint32_t*>(&osync_raw[1]), 1u);   // 4 warps -> w15 releases O(n-1)
      }
#else
      pipeline_o.consumer_release(o_state);
      ++o_state;
#endif
      pipeline_epi.producer_commit(epi_state);
      ++epi_state;
    }
  }
#endif  // MXFP8_SM12

  template<class Stage, class TensorO>
  CUTLASS_DEVICE auto
  correction_epilogue(
      float scale,
      Stage stage,
      TensorO const& sO_01) {

    using ElementOut = typename TensorO::value_type;

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    Tensor sO = sO_01(_,_,stage);

    const int kCorrectionTileSize = 32 / sizeof(ElementOut);

    using TMEM_LOAD = std::conditional_t<kCorrectionTileSize == 32, SM100_TMEM_LOAD_32dp32b32x, SM100_TMEM_LOAD_32dp32b16x>;  // 4x32 threads with 64 cols of 32b elem

    typename CollectiveMmaPV::TiledMma mma;
    Tensor cO = make_identity_tensor(select<0,1>(TileShapePV{}));
    Tensor tOtO = partition_fragment_C(mma, select<0,1>(TileShapePV{}));
    Tensor tOcO = mma.get_slice(0).partition_C(cO);
    Tensor tOsO = mma.get_slice(0).partition_C(sO);

    Tensor tOtO_i = logical_divide(tOtO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOcO_i = logical_divide(tOcO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOsO_i = logical_divide(tOsO, make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));

    if constexpr (decltype(stage == _0{})::value) {
      tOtO_i.data() = tOtO_i.data().get() + uint32_t(TmemAllocation::O0);
    }
    else {
      static_assert(decltype(stage == _1{})::value, "stage is either 0 or 1");
      tOtO_i.data() = tOtO_i.data().get() + uint32_t(TmemAllocation::O1);
    }

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tOtO_i(make_coord(_, _), _0{}));
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);

    Tensor tTMEM_LOADtO = thr_tmem_load.partition_S(tOtO_i(make_coord(_, _), _));
    Tensor tTMEM_LOADcO = thr_tmem_load.partition_D(tOcO_i(make_coord(_, _), _));
    Tensor tTMEM_LOADsO = thr_tmem_load.partition_D(tOsO_i(make_coord(_, _), _));

    float2 scale_f32x2 = make_float2(scale, scale);

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < get<2>(TileShape{}) / kCorrectionTileSize; i++) {
      Tensor tTMEM_LOADtO_i = tTMEM_LOADtO(_, _0{}, _0{}, i);
      Tensor tTMEM_LOADsO_i = tTMEM_LOADsO(_, _0{}, _0{}, i);

      Tensor tTMrO = make_tensor<ElementPV>(shape(tTMEM_LOADcO(_, _0{}, _0{}, i)));

      copy(tiled_tmem_load, tTMEM_LOADtO_i, tTMrO);
#ifdef MXFP8_DBG
      // [续19h] capture O BEFORE normalize (= full PV(0)+PV(1) accumulator), row 0.
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && decltype(stage == _0{})::value && thread_idx==0) {
        Tensor cOi2 = tTMEM_LOADcO(_, _0{}, _0{}, i);
        for (int j = 0; j < size(tTMrO); ++j) { int d = get<1>(cOi2(j)); if (d>=0 && d<128) g_dbg_Obn[d] = tTMrO(j); }
      }
#endif

#ifndef ONLY_SOFTMAX
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tTMrO); j += 2) {
        float2 in = make_float2(tTMrO(j), tTMrO(j+1));
        float2 out;
        cute::mul(out, scale_f32x2, in);
        tTMrO(j) = out.x;
        tTMrO(j+1) = out.y;
      }
#endif

#ifdef MXFP8_DBG
      // [续19c] capture final O for (block 0, q-row 0) across all d. thread_idx==0
      // owns row 0; tile i covers d in [i*kCorrectionTileSize, ...). Use the d
      // coordinate so the dump is layout-correct regardless of the TMEM atom.
      if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 &&
          decltype(stage == _0{})::value && thread_idx == 0) {
        Tensor cOi = tTMEM_LOADcO(_, _0{}, _0{}, i);
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < size(tTMrO); ++j) {
          int d = get<1>(cOi(j));
          if (d >= 0 && d < 128) g_dbg_O[d] = tTMrO(j);
        }
        g_dbg_O_got = 128;
      }
#endif

      constexpr int N = 4 / sizeof(ElementOut);
      NumericArrayConverter<ElementOut, ElementPV, N> convert;

      Tensor tSMrO = make_tensor_like<ElementOut>(tTMrO);

      Tensor tCs = recast<decltype(convert)::source_type>(tTMrO);
      Tensor tCd = recast<decltype(convert)::result_type>(tSMrO);

      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tCs); j++) {
        tCd(j) = convert.convert(tCs(j));
      }

      Tensor tSMsO_i = recast<uint32_t>(tTMEM_LOADsO_i);
      Tensor tSMrO_i = recast<uint32_t>(tSMrO);

      copy(AutoVectorizingCopyWithAssumedAlignment<128>{}, tSMrO_i, tSMsO_i);
    }

    cutlass::arch::fence_view_async_shared();
  }

#ifdef MXFP8_OSPLIT
  // [刀12 OSPLIT] kRsCols = number of O columns to rescale starting at tmem_O
  // (64 for one half; 0 = legacy full width from TileShape).
  template<int kRsCols = 0>
#endif
#if defined(MXFP8_G_RSADDR) || defined(MXFP8_G_COMBO)
  // [G_RSADDR] correction_rescale variant whose scale_f32x2 is precomputed by
  // the caller BEFORE the pipeline_o.consumer_wait spin, so the FMUL.F32X2
  // pack (data-dependent on `scale`, INDEPENDENT of the O accumulator the wait
  // gates) overlaps the barrier-spin instead of sitting on the post-wait path.
  // Body is byte-for-byte the legacy correction_rescale with the single line
  // `float2 scale_f32x2 = make_float2(scale,scale);` lifted into the arg list.
  // Same op multiset; only the pack's issue point moved earlier. The O-touching
  // copy_in/mul/copy_out are unchanged and still strictly after the wait.
  CUTLASS_DEVICE auto
  correction_rescale_pp(
      float2 scale_f32x2,
      uint32_t tmem_O) {

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

    const int kCorrectionTileSize = 16;

    using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b16x;
    using TMEM_STORE = SM100_TMEM_STORE_32dp32b16x;

    typename CollectiveMmaPV::TiledMma mma;
    Tensor cO = make_identity_tensor(select<0,1>(TileShapePV{}));
    Tensor tOtO = partition_fragment_C(mma, select<0,1>(TileShapePV{}));
    Tensor tOcO = mma.get_slice(0).partition_C(cO);

    Tensor tOtO_i = tOtO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOcO_i = tOcO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));

    tOtO_i.data() = tOtO_i.data().get() + tmem_O;

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tOtO_i);
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);
    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE{}, tOtO_i);
    auto thr_tmem_store   = tiled_tmem_store.get_slice(thread_idx);

    Tensor tTMEM_LOADtO = thr_tmem_load.partition_S(tOtO_i);
    Tensor tTMEM_LOADcO = thr_tmem_load.partition_D(tOcO_i);
    Tensor tTMEM_STOREtO = thr_tmem_store.partition_D(tOtO_i);
    Tensor tTMEM_STOREcO = thr_tmem_store.partition_S(tOcO_i);
    static_assert(shape(tTMEM_STOREcO) == shape(tTMEM_LOADcO));

    Tensor tTMrO = make_tensor<ElementPV>(make_shape(shape(tTMEM_LOADcO), Int<128 / kCorrectionTileSize>{}));

    auto copy_in = [&](int i) {
      Tensor tTMEM_LOADtO_i = tTMEM_LOADtO;
      tTMEM_LOADtO_i.data() = tTMEM_LOADtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_load, tTMEM_LOADtO_i, tTMrO_i);
    };

    auto copy_out = [&](int i) {
      Tensor tTMEM_STOREtO_i = tTMEM_STOREtO;
      tTMEM_STOREtO_i.data() = tTMEM_STOREtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_store, tTMrO_i, tTMEM_STOREtO_i);
    };

    copy_in(0);

    int count = get<2>(TileShape{}) / kCorrectionTileSize;

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < count; i++) {
      if (i != count - 1) {
        copy_in(i+1);
      }
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tTMrO_i); j += 2) {
        float2 in = make_float2(tTMrO_i(j), tTMrO_i(j+1));
        float2 out;
        cute::mul(out, scale_f32x2, in);
        tTMrO_i(j) = out.x;
        tTMrO_i(j+1) = out.y;
      }
      copy_out(i);
    }
  }
#endif

  CUTLASS_DEVICE auto
  correction_rescale(
      float scale,
      uint32_t tmem_O) {

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);

#ifdef MXFP8_2SM_RSTS8
    // ★ TOMBSTONE 2026-06-13 (刀20b, 默认关; 宏关 SASS bit-exact vs px18a 源) ★
    // [刀20b RSTS8 — oyhj correction_rescale 形态差 #2] LDTM/STTM 粒度 16→8 列
    // (32dp32b8x): 16 段 × 8 列流水 vs 我方 8 段 × 16 列。oyhj 2CTA 用 8;
    // 更细粒度 = 首段 LDTM 返回更早 / load-mul-store 重叠更深, 代价 = 2× 的
    // tcgen05 指令数。数值恒等 (同一组 fp32 mul, 仅分段不同; dump bit-exact 实证)。
    // MEASURED (px20b vs px18a 同窗, probe 820-823 干净): b1h8s32k 898.7 vs
    // 1156.6 (-258), b1h16s16k 847.6 vs 1101.7 (-254), SOW 170100h40 1128.4
    // vs 1295.9 (-167.5, ×2 复测一致) — 深负判死。死因: 2×
    // tcgen05.ld/st 指令数打在 depth-1 O pipeline 关键路径 (rescale 执行时
    // PV(t+1) 等本 release), 且 8x atom 的单位字节固定开销翻倍; rescale 在
    // 真数据下执行频率不低 (噪声数据 rowmax 常动), 每次执行都全额付税。
    // 佐证: 全零数据 (noinit, rescale 永跳过) SOW px18a 1295.9→1349.6 (+53.7)
    // — O-rescale 执行路径在 SOW 上的总税 ~54 TFLOPS, 这条路径是热路径。
    const int kCorrectionTileSize = 8;

    using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b8x;
    using TMEM_STORE = SM100_TMEM_STORE_32dp32b8x;
#else
    const int kCorrectionTileSize = 16;

    using TMEM_LOAD = SM100_TMEM_LOAD_32dp32b16x;  // 4x32 threads with 64 cols of 32b elem
    using TMEM_STORE = SM100_TMEM_STORE_32dp32b16x;  // 4x32 threads with 64 cols of 32b elem
#endif

    typename CollectiveMmaPV::TiledMma mma;
    Tensor cO = make_identity_tensor(select<0,1>(TileShapePV{}));
    Tensor tOtO = partition_fragment_C(mma, select<0,1>(TileShapePV{}));
    Tensor tOcO = mma.get_slice(0).partition_C(cO);

    Tensor tOtO_i = tOtO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));
    Tensor tOcO_i = tOcO.compose(make_layout(make_shape(_128{}, Int<kCorrectionTileSize>{})));

    tOtO_i.data() = tOtO_i.data().get() + tmem_O;

    auto tiled_tmem_load = make_tmem_copy(TMEM_LOAD{}, tOtO_i);
    auto thr_tmem_load   = tiled_tmem_load.get_slice(thread_idx);
    auto tiled_tmem_store = make_tmem_copy(TMEM_STORE{}, tOtO_i);
    auto thr_tmem_store   = tiled_tmem_store.get_slice(thread_idx);

    Tensor tTMEM_LOADtO = thr_tmem_load.partition_S(tOtO_i);
    Tensor tTMEM_LOADcO = thr_tmem_load.partition_D(tOcO_i);
    Tensor tTMEM_STOREtO = thr_tmem_store.partition_D(tOtO_i);
    Tensor tTMEM_STOREcO = thr_tmem_store.partition_S(tOcO_i);
    static_assert(shape(tTMEM_STOREcO) == shape(tTMEM_LOADcO));

    float2 scale_f32x2 = make_float2(scale, scale);

#ifdef MXFP8_OSPLIT
    constexpr int kRsWidth = (kRsCols > 0) ? kRsCols : 128;
    Tensor tTMrO = make_tensor<ElementPV>(make_shape(shape(tTMEM_LOADcO), Int<kRsWidth / kCorrectionTileSize>{}));
#else
    Tensor tTMrO = make_tensor<ElementPV>(make_shape(shape(tTMEM_LOADcO), Int<128 / kCorrectionTileSize>{}));
#endif

    auto copy_in = [&](int i) {
      Tensor tTMEM_LOADtO_i = tTMEM_LOADtO;
      tTMEM_LOADtO_i.data() = tTMEM_LOADtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_load, tTMEM_LOADtO_i, tTMrO_i);
    };

    auto copy_out = [&](int i) {
      Tensor tTMEM_STOREtO_i = tTMEM_STOREtO;
      tTMEM_STOREtO_i.data() = tTMEM_STOREtO_i.data().get() + uint32_t(i * kCorrectionTileSize);
      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
      copy(tiled_tmem_store, tTMrO_i, tTMEM_STOREtO_i);
    };

    copy_in(0);

#ifdef MXFP8_OSPLIT
    constexpr int count = kRsWidth / kCorrectionTileSize;
#else
    int count = get<2>(TileShape{}) / kCorrectionTileSize;
#endif

    CUTLASS_PRAGMA_UNROLL
    for (int i = 0; i < count; i++) {
      if (i != count - 1) {
        copy_in(i+1);
      }

      Tensor tTMrO_i = tTMrO(_, i).compose(make_layout(shape<0>(tTMrO)));
#ifdef MXFP8_DBG
      // [续19h] capture O (after PV(0), BEFORE rescale-multiply) for row 0 at its d's.
      // tmem_O==O0 only (single O). For s=256 this fires once (corr_tile=1), giving
      // the un-normalized tile-0 PV accumulator. Compare to Σ_kv0-127 rawP·V_ref.
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && thread_idx==0) {
        for (int j = 0; j < size(tTMrO_i); ++j) { int d = i * kCorrectionTileSize + get<1>(tTMEM_LOADcO(j)); if (d>=0 && d<128) g_dbg_OafterPV0[d] = tTMrO_i(j); }
      }
#endif

#ifdef MXFP8_2SM_RSGUARD
      // ★ TOMBSTONE 2026-06-13 (刀20a, 默认关; 宏关 SASS bit-exact vs px18a 源) ★
      // [刀20a RSGUARD — oyhj correction_rescale 形态差 #1] per-thread (per-row)
      // `scale != 1.0f` 内层 guard: 循环级 __any_sync 只挡"全 warp 32 行全稳"的
      // tile; 进到这里说明至少 1 行动了, 但其余稳定行的 mul 仍是 ×1.0 恒等 —
      // oyhj 保留 guard 跳过这些行的 FFMA (LDTM/STTM 往返不可跳, warp op)。
      // 我方 M3-2 判过 branchless 更优 (AVO L1), 但那是 rescale 常开时代;
      // 懒链下 rescale 触发本就稀疏, 触发时 per-row 分布未知 — 隔离重测。
      // 数值恒等 (跳过 = ×1.0; dump bit-exact 实证)。
      // MEASURED (px20a vs px18a 同窗, probe 820-823 干净): b1h8s32k 1136.0 vs
      // 1156.6 (-20.7), b1h16s16k 1081.2 vs 1101.7 (-20.5), SOW 170100h40
      // 1281.0 vs 1295.9 (-14.9, ×2 复测一致) — 判死, AVO L1 在
      // 懒链时代复验成立: guard 分支挡住编译器把 FFMA 软流水进 LDTM 阴影,
      // 执行态 rescale 的关键路径反而变长; 省下的 ×1.0 FFMA 本就藏在 LDTM
      // 延迟里, 是免费的。oyhj 扛得住此形态是其骨架 O 链有冗余, 非此刀本身好。
      if (scale != 1.0f) {
        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < size(tTMrO_i); j += 2) {
          float2 in = make_float2(tTMrO_i(j), tTMrO_i(j+1));
          float2 out;
          cute::mul(out, scale_f32x2, in);
          tTMrO_i(j) = out.x;
          tTMrO_i(j+1) = out.y;
        }
      }
#else
      // [MXFP8 L1] branchless rescale — always multiply. scale==1.0 is an exact
      // fp32 no-op, so dropping the `if (scale != 1.0f)` guard removes per-thread
      // (per-row) control divergence in the correction warp without changing the
      // result. (AVO L1: the divergence/sync cost outweighs the skipped mul.)
      CUTLASS_PRAGMA_UNROLL
      for (int j = 0; j < size(tTMrO_i); j += 2) {
        float2 in = make_float2(tTMrO_i(j), tTMrO_i(j+1));
        float2 out;
        cute::mul(out, scale_f32x2, in);
        tTMrO_i(j) = out.x;
        tTMrO_i(j+1) = out.y;
      }
#endif

      copy_out(i);
    }
  }

  template<
    class BlkCoord, class ProblemShape, class ParamsProblemShape,
    class TensorStorageEpi, class CollectiveEpilogue
  >
  CUTLASS_DEVICE auto
  correction(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      ParamsProblemShape const& params_problem_shape,
      TensorStorageEpi& shared_storage_epi,
#ifdef MXFP8_E2OFFLOAD
      TensorStorage& storage,                    // [刀10] correction writes its P slice -> storage.smem_p
      PipelineS& pipeline_s0_rel, typename PipelineS::PipelineState& pipeline_s0_rel_state,
      PipelineS& pipeline_s1_rel, typename PipelineS::PipelineState& pipeline_s1_rel_state,
#endif
      PipelineC& pipeline_s0_c, typename PipelineC::PipelineState& pipeline_s0_c_consumer_state,
      PipelineC& pipeline_s1_c, typename PipelineC::PipelineState& pipeline_s1_c_consumer_state,
      PipelineO& pipeline_o, typename PipelineO::PipelineState& pipeline_o_consumer_state,
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& pipeline_epi_producer_state,
      CollectiveEpilogue& epilogue) {

    int mask_tile_count = Mask{}.get_trip_count(blk_coord, TileShape{}, problem_shape);

    int thread_idx = threadIdx.x % (4 * cutlass::NumThreadsPerWarp);
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 1u); __threadfence_system(); } }   // [2SM] correction entered
#endif

    Tensor tStS = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));

    Tensor cS = make_identity_tensor(select<0,1>(TileShapeQK{}));
    Tensor tScS = typename CollectiveMmaQK::TiledMma{}.get_slice(0).partition_C(cS);

    Tensor tStS_v = tStS.compose(make_layout(make_shape(_128{}, _2{})));
    Tensor tScS_v = tScS.compose(make_layout(make_shape(_128{}, _2{})));

    using TMEM_LOAD_V = SM100_TMEM_LOAD_32dp32b2x;   // 4x32 threads with 2 cols of 32b elem

    auto tiled_tmem_loadv = make_tmem_copy(TMEM_LOAD_V{}, tStS_v);
    auto thr_tmem_loadv  = tiled_tmem_loadv.get_slice(thread_idx);

    Tensor tTMEM_LOADVtS = thr_tmem_loadv.partition_S(tStS_v);
    Tensor tTMEM_LOADVcS = thr_tmem_loadv.partition_D(tScS_v);

    // [M3-2 LAZY-MAX] back to ONE stats slot + one rescale per TILE (the lazy
    // w-chain makes the whole tile share one scale): stats slot for tile t =
    // S col (t%2)*128 + [0:2), g0-only signals (s1_corr inert again).
    auto loadv_stats_col = [&](uint32_t col, auto& dst) {
      Tensor tv = tTMEM_LOADVtS;
      tv.data() = tTMEM_LOADVtS.data().get() + warp_uniform(col);
      copy(tiled_tmem_loadv, tv, dst);
    };
    (void) pipeline_s1_c; (void) pipeline_s1_c_consumer_state;

#ifdef MXFP8_E2OFFLOAD
    // ── [刀10 E2OFFLOAD] correction-side P slice machinery ──
    // For unmasked tile t this warpgroup computes P for the tail kOffC columns
    // of BOTH softmax halves: cols [kKeepC,64) (g0 half) and [64+kKeepC,128)
    // (g1 half). Input gating: the stats wait (this loop already does it) —
    // g0 committed stats AFTER consuming S(t), so S(t) TMEM is valid here by
    // mbarrier transitivity. Output gating: this warpgroup joins the s0/s1
    // sub-pipelines' consumer arrival count, which is exactly what the PV
    // MMA's producer_acquire waits on.
    constexpr int kOffC  = MXFP8_E2OFFLOAD_C;
    constexpr int kKeepC = 64 - kOffC;
    const int e2_unmasked = Mask{}.get_unmasked_trip_count(blk_coord, TileShape{}, problem_shape);
    float corr_sum = 0.0f;                       // this row's offloaded partial row_sum (final-scale chain)
    const float e2_scale = params.scale_softmax_log2;
    NumericArrayConverter<Element, ElementQK, 2> e2_convert;

    // slice TMEM load: 128 rows x kOffC fp32 cols, 32dp32b atom family (same
    // thread->row mapping as the softmax P STS recipe: thread_idx == row).
    Tensor tE2tS_full = partition_fragment_C(typename CollectiveMmaQK::TiledMma{}, select<0,1>(TileShapeQK{}));
    Tensor tE2tS_base = tE2tS_full.compose(make_layout(make_shape(_128{}, Int<kOffC>{})));
    tE2tS_base.data() = warp_uniform(uint32_t(TmemAllocation::S0) + uint32_t(kKeepC));   // (t=0, h=0) base
    using TMEM_LOAD_E2 = std::conditional_t<kOffC == 16, SM100_TMEM_LOAD_32dp32b16x, SM100_TMEM_LOAD_32dp32b32x>;
    auto tiled_tmem_load_e2 = make_tmem_copy(TMEM_LOAD_E2{}, tE2tS_base);
    auto thr_tmem_load_e2   = tiled_tmem_load_e2.get_slice(thread_idx);
    Tensor tE2tS = thr_tmem_load_e2.partition_S(tE2tS_base);
    Tensor cE2   = make_identity_tensor(make_shape(_128{}, Int<kOffC>{}));
    Tensor tE2cS = thr_tmem_load_e2.partition_D(cE2);
    const int e2_row = get<0>(tE2cS(_0{}));      // this thread's row (== thread_idx for 32dp atoms)

    auto e2_pslice = [&](int t, float new_max) {
      const float row_max_scale = new_max * e2_scale;
      float2 e2s2  = make_float2(e2_scale, e2_scale);
      float2 nrms2 = make_float2(-row_max_scale, -row_max_scale);
#if (MXFP8_PSTATIC_EXP) != 0
      const float e2_sinv = __uint_as_float((uint32_t)(127 - (MXFP8_PSTATIC_EXP)) << 23); // 2^-EXP
#endif
      Tensor sP_st = make_tensor(make_smem_ptr(storage.smem_p.data()), SmemLayoutP{})(_, _, _, (t & 1));
      CUTLASS_PRAGMA_UNROLL
      for (int h = 0; h < 2; ++h) {
        // S slice load (tile parity picks S0/S1 = +128 cols; half picks +64)
        Tensor tE2tS_i = tE2tS;
        tE2tS_i.data() = tE2tS_i.data().get() + uint32_t((t & 1) * 128 + h * 64);
        Tensor tE2rS = make_tensor<ElementQK>(shape(tE2cS));
        copy(tiled_tmem_load_e2, tE2tS_i, tE2rS);
        // exp2 + e4m3 convert (identical expressions/constants to softmax's
        // Phase 1/4 -> per-element results bit-identical to the non-offloaded
        // path; only the row_sum association differs).
        Tensor pOut = make_tensor<uint32_t>(make_shape(Int<kOffC / 4>{}));
        Tensor pOut_e = recast<Array<Element, 2>>(pOut);
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < kOffC; i += 2) {
          float2 in = make_float2(tE2rS(i), tE2rS(i+1));
          float2 out;
          cute::fma(out, e2s2, in, nrms2);
          float p0 = EXP2_SOFTMAX(out.x);
          float p1 = EXP2_SOFTMAX(out.y);
          corr_sum += p0;
          corr_sum += p1;
          Array<ElementQK, 2> in_conv;
#if (MXFP8_PSTATIC_EXP) != 0
          in_conv[0] = p0 * e2_sinv;
          in_conv[1] = p1 * e2_sinv;
#else
          in_conv[0] = p0;
          in_conv[1] = p1;
#endif
          pOut_e[i / 2] = e2_convert(in_conv);
        }
        // P STS.128 — same address recipe as softmax (row, 16-col granules).
        CUTLASS_PRAGMA_UNROLL
        for (int g = 0; g < kOffC / 16; ++g) {
          const int kg0 = h * 64 + kKeepC + 16 * g;
          Element& dst_byte0 = sP_st(make_coord(e2_row, kg0 % 32), _0{}, kg0 / 32);
          const uint32_t dst_addr = cute::cast_smem_ptr_to_uint(&dst_byte0);
          asm volatile("st.shared.v4.u32 [%0], {%1, %2, %3, %4};" ::
                       "r"(dst_addr),
                       "r"(pOut(4*g+0)), "r"(pOut(4*g+1)),
                       "r"(pOut(4*g+2)), "r"(pOut(4*g+3)));
        }
      }
      // generic->async publication of the P slice (MMA reads smem_p via
      // tcgen05) + pin the slice tcgen05.ld retirement before the S release.
      cutlass::arch::fence_view_async_shared();
      cutlass::arch::fence_view_async_tmem_load();
    };

    // per-tile S sub-pipeline release arrives (this warpgroup is part of the
    // 512-count consumer set; arrive EVERY tile — masked tiles too, just
    // without slice work).
    auto e2_release_s = [&]() {
      pipeline_s0_rel.consumer_release(pipeline_s0_rel_state);
      ++pipeline_s0_rel_state;
      pipeline_s1_rel.consumer_release(pipeline_s1_rel_state);
      ++pipeline_s1_rel_state;
    };
#endif

    // ignore first signal (tile 0): no O to rescale before the first PV.
#ifdef MXFP8_E2OFFLOAD
    // [刀10] tile 0 DOES need its P slice (PV(0) waits for it): read the tile-0
    // stats (slot col 0) for the shared scale w_0, slice, then release.
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
#ifdef MXFP8_E2OFFLOAD_GATEPROBE
    e2_release_s();
#endif
    if (0 < e2_unmasked) {
      Tensor tE2VrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));
      loadv_stats_col(0u, tE2VrS);
      e2_pslice(0, tE2VrS(kIdxNewRowMax));
    }
#ifndef MXFP8_E2OFFLOAD_GATEPROBE
    e2_release_s();
#endif
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#else
#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#endif
#endif
    ++pipeline_s0_c_consumer_state;
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 2u); __threadfence_system(); } }   // [2SM] corr: past first ignore-wait
#endif

    CUTLASS_PRAGMA_NO_UNROLL
    for (int t = 1; t < mask_tile_count; ++t) {

#ifndef MXFP8_2SM_DECOUPLE_PERF
      pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
#endif

      Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));
      loadv_stats_col(uint32_t((t & 1) ? 128u : 0u), tTMEM_LOADVrS);

      // [MXFP8 L1] rescale factor — exp2(0)=1 exactly when the running max
      // is unchanged (the common case under the lazy chain).
#if defined(MXFP8_R15_ORVLOG)
      // [r15 ORVLOG ported to the STATIC (PSTATIC) g_combo king] the O-rescale
      // exp2 ran UNCONDITIONALLY only because the warp-vote read its result
      // (scale != 1.0f). But exp2 is injective and ex2.approx(0)==1.0f EXACTLY,
      // so {scale != 1.0f} <=> {old != new} in the SCORE/log domain — derive
      // the vote from the raw maxes (one FSETP, no MUFU) and SINK the exp2 +
      // float2 pack INSIDE the rescale guard (below). Under the lazy-max chain
      // old==new is the common case, so the MUFU.EX2 is DYNAMICALLY skipped on
      // most tiles (off the saturated XU), while the rescale-taken path runs
      // the identical exp2 -> O byte-identical. PSTATIC-SAFE: PSTATIC only
      // changes the P-quant SF path in the SOFTMAX warp; this O-rescale lives
      // in the CORRECTION warp and is bit-identical to the dynamic corner
      // (no PSTATIC guard anywhere in this region). The vote inputs are the
      // SAME stats (kIdxOldRowMax/kIdxNewRowMax) the unconditional exp2 read.
      // GOLD-SAFE: if ex2.approx rounded a tiny nonzero arg to exactly 1.0f the
      // stock vote would have voted "no rescale" too (it tested scale==1.0f);
      // here old!=new votes "rescale", the guard recomputes the identical exp2
      // and multiplies O by that factor (=1.0f if it really rounds to 1) ->
      // bit-exact (O never wrong). `scale` stays declared (DBG / OSPLIT refs).
      const float m2_old_max = tTMEM_LOADVrS(kIdxOldRowMax);
      const float m2_new_max = tTMEM_LOADVrS(kIdxNewRowMax);
      const bool m2_need_rescale = __any_sync(0xffffffffu, m2_old_max != m2_new_max);
      float scale = 1.0f;          // assigned in the guard when rescale taken
      float2 g_scale_f32x2;        // packed in the guard when rescale taken
#else
      float scale = ::exp2f(params.scale_softmax_log2 * (tTMEM_LOADVrS(kIdxOldRowMax) - tTMEM_LOADVrS(kIdxNewRowMax)));
#if defined(MXFP8_M2_ORVOTE) || defined(MXFP8_M2_COMBO)
      // [M2-B ORVOTE-HOIST] precompute the rescale warp-vote here (scale is
      // final). Overlaps the VOTE latency with the O-wait barrier-spin below.
      const bool m2_need_rescale = __any_sync(0xffffffffu, scale != 1.0f);
#endif
#if defined(MXFP8_G_RSADDR) || defined(MXFP8_G_COMBO)
      // [G_RSADDR] pack scale into the FMUL.F32X2 operand HERE, before the
      // O-wait. Pure data-pack on `scale` (already final from the line above);
      // INDEPENDENT of the O accumulator the wait gates. The _pp rescale variant
      // consumes this exact float2, so the pack's latency overlaps the spin.
      float2 g_scale_f32x2 = make_float2(scale, scale);
#endif
#endif
#ifdef MXFP8_DBG
      if (blockIdx.x==0 && blockIdx.y==0 && blockIdx.z==0 && (threadIdx.x % (4*cutlass::NumThreadsPerWarp))==0 && t < 8) {
        g_dbg_corr_scale[t] = scale;
        g_dbg_corr_old[t]   = tTMEM_LOADVrS(kIdxOldRowMax);
        g_dbg_corr_new[t]   = tTMEM_LOADVrS(kIdxNewRowMax);
        g_dbg_corr_n = t + 1;
      }
#endif

#if defined(MXFP8_2SM_CREL) && !defined(MXFP8_2SM_DECOUPLE_PERF)
      // [PX7 2SM-CREL] early release: stats already in registers (scale FADD/
      // MUFU consumed them; the fence pins tcgen05.ld retirement) — release
      // the softmax->correction slot BEFORE the O wait, cutting "PV(t) UMMA
      // done" (pipeline_o.consumer_wait) out of g0-softmax's post-B_PDONE
      // producer_acquire chain (ncu hotspot #4, L1880, 4.6-4.7% samples).
      // Same knife as 1SM PX2a CRELEASE (TOMBSTONE there, -21.8) but three
      // conditions invert on 2SM: (a) softmax has NO issue slack (eligible
      // 0.537 vs 0.75; EXP2NOP +47.6 vs +0.8 — softmax-side latency IS the
      // critical path); (b) the de-pacing collision surface is mostly absent:
      // 2SM rescale is conditionally SKIPPED (lazy-max => scale==1 common),
      // so no always-on 128-col LDTM/STTM burst to collide with; (c) DECOUPLE
      // probe leaves ~+38 structural margin in exactly this c-pipeline chain.
      // V-slot safety identical to 1SM argument: read(t) < release(t) <
      // release(t+1) gates the col-(t%2)*128 stats rewrite at t+2 (depth-1
      // back-pressure semantics preserved).
      cutlass::arch::fence_view_async_tmem_load();
      pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#endif

#ifdef MXFP8_E2OFFLOAD
      // [刀10] P slice for tile t — fills this warpgroup's idle window between
      // the stats read and the O wait (ncu: 43.6k barrier-idle samples). The
      // partial-sum chain advances EVERY tile (masked ones too: rescale
      // continues even when no elements are added), mirroring softmax's
      // rsf_acc_scale cascade exactly (same stats inputs, same expression).
      corr_sum *= scale;
#if defined(MXFP8_E2OFFLOAD_V2)
      // [刀13 V2] slice deferred to AFTER the O consumer_release (see below) —
      // keep only the partial-sum rescale here (needs `scale`, trivially cheap).
#elif defined(MXFP8_E2OFFLOAD_GATEPROBE)
      // [刀10 GATE-PROBE — NUMERICS RACE] arrive BEFORE computing the slice:
      // the PV MMA no longer waits for the slice (reads a stale P tail) —
      // isolates the PV-gate latency tax from the correction-load tax.
      e2_release_s();
      if (t < e2_unmasked) {
        e2_pslice(t, tTMEM_LOADVrS(kIdxNewRowMax));
      }
#else
      if (t < e2_unmasked) {
        e2_pslice(t, tTMEM_LOADVrS(kIdxNewRowMax));
      }
      e2_release_s();
#endif
#endif

#ifdef MXFP8_OSPLIT
      // [刀12 OSPLIT] half-column interleave: rescale h0 and release it BEFORE
      // waiting h1 — the moment h0 is released the MMA's acquire(h0) unblocks
      // PV0(t) while this warpgroup still rescales h1. The O gating drops from
      // whole-tile serial to half-tile interleaved.
      bool need_rescale = __any_sync(0xffffffffu, scale != 1.0f);

      pipeline_o.consumer_wait(pipeline_o_consumer_state);            // h0 full
#if defined(MXFP8_2SM_BEACON)
      if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 3u); __threadfence_system(); } }   // [2SM] corr: past loop PipelineO wait
#endif
      if (need_rescale) {
        correction_rescale<64>(scale, uint32_t(TmemAllocation::O0));
        cutlass::arch::fence_view_async_tmem_store();
      }
      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;

      pipeline_o.consumer_wait(pipeline_o_consumer_state);            // h1 full
      if (need_rescale) {
        correction_rescale<64>(scale, uint32_t(TmemAllocation::O0) + 64u);
        cutlass::arch::fence_view_async_tmem_store();
      }

#if !defined(MXFP8_2SM_DECOUPLE_PERF) && !defined(MXFP8_2SM_CREL)
      pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#endif
      ++pipeline_s0_c_consumer_state;

      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;
    }
#else
      pipeline_o.consumer_wait(pipeline_o_consumer_state);
#if defined(MXFP8_2SM_BEACON)
      if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 3u); __threadfence_system(); } }   // [2SM] corr: past loop PipelineO wait
#endif

      // [M3-2 perf] conditional rescale: tiles that don't move the row max
      // (old==new -> scale==1 exactly) skip the 128-col O TMEM read-modify-
      // write. Per-WARP uniform (each warp owns its own 32 rows; the 32dp
      // TMEM atoms are warp-independent).
#if defined(MXFP8_M2_ORVOTE) || defined(MXFP8_M2_COMBO)
      if (m2_need_rescale) {
#else
      if (__any_sync(0xffffffffu, scale != 1.0f)) {
#endif
#if defined(MXFP8_R15_ORVLOG)
        // [r15 ORVLOG] rescale IS taken — NOW run the exp2 (off the skip-tile
        // common path) and pack. IDENTICAL expression/operands to the stock
        // unconditional site (m2_old_max/m2_new_max are the same kIdxOld/New
        // stats) => the taken-path rescale factor is bit-identical to g_combo.
        scale = ::exp2f(params.scale_softmax_log2 * (m2_old_max - m2_new_max));
        g_scale_f32x2 = make_float2(scale, scale);
#endif
#if defined(MXFP8_G_RSADDR) || defined(MXFP8_G_COMBO)
        correction_rescale_pp(g_scale_f32x2, uint32_t(TmemAllocation::O0));
#else
        correction_rescale(scale, uint32_t(TmemAllocation::O0));
#endif
        cutlass::arch::fence_view_async_tmem_store();
      }

#if !defined(MXFP8_2SM_DECOUPLE_PERF) && !defined(MXFP8_2SM_CREL)
      pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#endif
      ++pipeline_s0_c_consumer_state;

      pipeline_o.consumer_release(pipeline_o_consumer_state);
      ++pipeline_o_consumer_state;

#ifdef MXFP8_E2OFFLOAD_V2
      // [刀13 V2] post-release shadow slot: O(t-1) is released (PV(t)'s O-gate
      // already open), softmax is concurrently exp2-ing its own kKeepC columns
      // of tile t. Compute the tail slice(t) NOW, then arrive on s0/s1 —
      // the PV(t) P-done gate. S(t) TMEM is still valid: this very arrival is
      // what lets QK(t+2) rewrite the parity slot.
      if (t < e2_unmasked) {
        e2_pslice(t, tTMEM_LOADVrS(kIdxNewRowMax));
      }
      e2_release_s();
#endif
    }
#endif

    // tail: final normalize + epilogue for the single M=128 tile.
    // The final (sum, max) were written by g0's final_call into ITS slot of
    // the last tile: col ((n-1)%2)*128, signalled by g0's trailing c0 commit.
#ifdef MXFP8_E2OFFLOAD
    // [刀10] trailing arrival balance: the mma() commits n+2 per sub-pipeline
    // (n QK subs + 2 tail balancing); this warpgroup arrived n times in the
    // tile loop — two more blind arrives per pipeline keep every empty-barrier
    // phase at the full 512 count (mirrors softmax's two trailing releases).
    e2_release_s();
    e2_release_s();
#endif
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 4u); __threadfence_system(); } }   // [2SM] corr: past loop, at tail
#endif
#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s0_c.consumer_wait(pipeline_s0_c_consumer_state);
#endif

    Tensor tTMEM_LOADVrS = make_tensor<ElementQK>(shape(tTMEM_LOADVcS));
    loadv_stats_col(uint32_t(((mask_tile_count - 1) & 1) ? 128u : 0u), tTMEM_LOADVrS);   // g0's final slot

#ifndef MXFP8_2SM_DECOUPLE_PERF
    pipeline_s0_c.consumer_release(pipeline_s0_c_consumer_state);
#endif
    ++pipeline_s0_c_consumer_state;

#ifdef MXFP8_OSPLIT
    // [刀12 OSPLIT] the epilogue reads the FULL 128-col O — wait BOTH halves
    // (the two halves tile [O0, O0+128) contiguously; read path unchanged).
    auto pipeline_o_state_h0 = pipeline_o_consumer_state;
    pipeline_o.consumer_wait(pipeline_o_consumer_state);              // h0 full
    ++pipeline_o_consumer_state;
    pipeline_o.consumer_wait(pipeline_o_consumer_state);              // h1 full
#else
    pipeline_o.consumer_wait(pipeline_o_consumer_state);
#endif
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 5u); __threadfence_system(); } }   // [2SM] corr: past tail PipelineO wait
#endif
    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    Tensor sO = make_tensor(make_smem_ptr(shared_storage_epi.smem_o.data()), typename TensorStorageEpi::SmemLayoutO{});
    Tensor gLSE = make_tensor(make_gmem_ptr(epilogue.params.ptr_LSE), select<0,3>(problem_shape), epilogue.params.dLSE);

#ifdef MXFP8_E2OFFLOAD
    // [刀10] fold this row's offloaded partial row_sum into the final
    // normalization (g0's kIdxFinalRowSum excludes the slice columns; both
    // values are already in the same scale w_{n-1}). Same thread == same row
    // on both sides (32dp atom family), so this is barrier-free.
    const float e2_final_sum = tTMEM_LOADVrS(kIdxFinalRowSum) + corr_sum;
    correction_epilogue(params.scale_output / e2_final_sum, _0{}, sO);
#else
    correction_epilogue(params.scale_output / tTMEM_LOADVrS(kIdxFinalRowSum), _0{}, sO);
#endif

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = get<0>(tTMEM_LOADVcS(_0{})) + get<0>(TileShape{}) * get<0>(blk_coord);

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

#ifdef MXFP8_E2OFFLOAD
      ElementPV lse = cutlass::fast_log(e2_final_sum) + params.scale_softmax * tTMEM_LOADVrS(kIdxFinalRowMax);
#else
      ElementPV lse = cutlass::fast_log(tTMEM_LOADVrS(kIdxFinalRowSum)) + params.scale_softmax * tTMEM_LOADVrS(kIdxFinalRowMax);
#endif

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    cutlass::arch::fence_view_async_tmem_load();

#ifdef MXFP8_OSPLIT
    pipeline_o.consumer_release(pipeline_o_state_h0);                 // h0
    pipeline_o.consumer_release(pipeline_o_consumer_state);           // h1
    ++pipeline_o_consumer_state;
#else
    pipeline_o.consumer_release(pipeline_o_consumer_state);
    ++pipeline_o_consumer_state;
#endif

    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;
#if defined(MXFP8_2SM_BEACON)
    if (cute::elect_one_sync()) { unsigned _r=cute::block_rank_in_cluster(); if(_r<4){ atomicMax(&g_bcn[_r*16+7], 6u); __threadfence_system(); } }   // [2SM] corr: DONE (past epilogue commit)
#endif
  }


  template<
    class BlkCoord, class ProblemShape, class ParamsProblemShape,
    class TensorStorageEpi, class CollectiveEpilogue
  >
  CUTLASS_DEVICE auto
  correction_empty(
      BlkCoord const& blk_coord,
      Params const& params, ProblemShape const& problem_shape,
      ParamsProblemShape const& params_problem_shape,
      TensorStorageEpi& shared_storage_epi,
      PipelineE& pipeline_epi, typename PipelineE::PipelineState& pipeline_epi_producer_state,
      CollectiveEpilogue& epilogue) {

    pipeline_epi.producer_acquire(pipeline_epi_producer_state);

    Tensor sO = make_tensor(make_smem_ptr(shared_storage_epi.smem_o.data()), typename TensorStorageEpi::SmemLayoutO{});
    Tensor gLSE = make_tensor(make_gmem_ptr(epilogue.params.ptr_LSE), select<0,3>(problem_shape), epilogue.params.dLSE);
    float lse = -INFINITY;
    int thread_idx = threadIdx.x % (4 * NumThreadsPerWarp);

#define DSHOW(x) print(#x ": "); print(x); print("\n")
    if (threadIdx.x % 128 == 0 && block0()) {
      DSHOW(sO);
    }
#if 1

    using ElementOut = typename CollectiveEpilogue::ElementOut;
    auto tiled_copy = make_cotiled_copy(
        Copy_Atom<UniversalCopy<uint32_t>, ElementOut>{},
        make_ordered_layout(make_shape(_128{}, Int<sizeof(uint32_t) / sizeof(ElementOut)>{}), Step<_1, _0>{}),
        sO.layout());

    auto thr_copy = tiled_copy.get_slice(thread_idx);
    auto tOgO = thr_copy.partition_D(sO);
    auto tOrO = make_tensor<ElementOut>(shape(tOgO(_,_,_,_0{})));
    clear(tOrO);

    copy(tiled_copy, tOrO, tOgO(_,_,_,_0{}));
#endif

    if (epilogue.params.ptr_LSE != nullptr) {
      int row_idx = thread_idx + get<0>(TileShape{}) * get<0>(blk_coord);

      int row_offset = 0;
      if constexpr (is_variable_length_v<tuple_element_t<0, ParamsProblemShape>>) {
        row_offset = get<0>(params_problem_shape).cumulative_length[get<2,1>(blk_coord)];
      }

      if (row_idx < get<0>(problem_shape)) {
        gLSE(row_idx + row_offset, get<2>(blk_coord)) = lse;
      }
    }

    // [MXFP8 N128] single-stage: one O sub-tile, one epilogue signal.
    cutlass::arch::fence_view_async_shared();
    pipeline_epi.producer_commit(pipeline_epi_producer_state);
    ++pipeline_epi_producer_state;
  }

};

}  // namespace cutlass::fmha::collective
