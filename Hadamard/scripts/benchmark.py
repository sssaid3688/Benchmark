#!/usr/bin/env python3
"""doublewarp Hadamard+NVFP4 融合量化 benchmark

用法:
  # 基本：跑所有 shape，打印 fusion kernel 时间
  python3 benchmark.py

  # 指定 shape index（0-6）
  python3 benchmark.py --shape-idx 4

  # 指定 fast_math（0 或 1）
  python3 benchmark.py --fm 1

  # nsys 采集模式（输出 .nsys-rep）
  python3 benchmark.py --nsys --shape-idx 4

  # warmup/iters 可调
  python3 benchmark.py --warmup 100 --iters 300

环境变量:
  NVTE_USE_FAST_MATH=1  开启 fast_math（等价于 --fm 1）

输出:
  打印 fusion kernel 的 min/mean/median/max（us）
  --nsys 模式额外生成 nsys 报告
"""
import argparse, math, os, torch
import transformer_engine.pytorch as te  # noqa: F401
import transformer_engine_torch as tex
from transformer_engine.pytorch import NVFP4Quantizer

# ============ Shape 定义 ============
SHAPES = [
    ("shape8", 8192,   2048),   # idx 0
    ("shape5", 8192,   4096),   # idx 1
    ("shape9", 16384,  2048),   # idx 2
    ("shape6", 16384,  4096),   # idx 3
    ("shape7", 32768,  4096),   # idx 4
    ("shape1", 170112, 5120),   # idx 5 (padded from 170100)
    ("shape2", 510336, 5120),   # idx 6 (padded from 510300)
]

def make_quantizer():
    """创建 NVFP4 列量化 quantizer（Hadamard+NVFP4 路径）"""
    q = NVFP4Quantizer(
        fp4_dtype=tex.DType.kFloat4E2M1,
        rowwise=False,          # 不做行量化
        columnwise=True,         # 只做列量化
        with_amax_reduction=False,
        amax_reduction_group=None,
        with_rht=True,           # 开启 Hadamard 变换
        with_post_rht_amax=True,
        with_random_sign_mask=True,
        stochastic_rounding=False,
    )
    q.optimize_for_gemm = False
    return q

def main():
    ap = argparse.ArgumentParser(description="doublewarp Hadamard+NVFP4 benchmark")
    ap.add_argument("--shape-idx", type=int, default=-1,
                    help="Shape index (0-6). -1 = run all shapes")
    ap.add_argument("--fm", type=int, default=None, choices=[0, 1],
                    help="NVTE_USE_FAST_MATH (0=off, 1=on). None=use env var")
    ap.add_argument("--warmup", type=int, default=100)
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--nsys", action="store_true",
                    help="Run in nsys profile mode (single shape, generates .nsys-rep)")
    args = ap.parse_args()

    # 设置 fast_math 环境变量
    if args.fm is not None:
        os.environ["NVTE_USE_FAST_MATH"] = str(args.fm)
    fm_val = os.environ.get("NVTE_USE_FAST_MATH", "0")

    torch.cuda.set_device(0)

    # nsys 模式：只跑一个 shape，不做 event 计时（nsys 自己测）
    if args.nsys:
        assert args.shape_idx >= 0, "--nsys requires --shape-idx"
        name, M, K = SHAPES[args.shape_idx]
        print(f"[nsys] {name} M={M} K={K} fast_math={fm_val}", flush=True)
        x = torch.randn((M, K), dtype=torch.bfloat16, device="cuda")
        q = make_quantizer()
        # warmup
        for _ in range(args.warmup):
            q(x)
        torch.cuda.synchronize()
        # 正式采集区间（用 nvtx range 标记）
        torch.cuda.nvtx.range_push(f"{name}_fm{fm_val}")
        for _ in range(args.iters):
            q(x)
        torch.cuda.synchronize()
        torch.cuda.nvtx.range_pop()
        print(f"[nsys] DONE {name}", flush=True)
        return

    # 普通 CUDA event 计时模式
    if args.shape_idx >= 0:
        indices = [args.shape_idx]
    else:
        indices = list(range(len(SHAPES)))

    print(f"doublewarp Hadamard+NVFP4 benchmark")
    print(f"  fast_math (NVTE_USE_FAST_MATH): {fm_val}")
    print(f"  warmup: {args.warmup}, iters: {args.iters}")
    print()

    header = f"{'idx':>3} {'shape':<8} {'M':>8} {'K':>6} {'min_us':>10} {'mean_us':>10} {'median_us':>10} {'max_us':>10}"
    print(header)
    print("-" * len(header))

    results = []
    for idx in indices:
        name, M, K = SHAPES[idx]
        x = torch.randn((M, K), dtype=torch.bfloat16, device="cuda")
        q = make_quantizer()

        # warmup
        for _ in range(args.warmup):
            q(x)
        torch.cuda.synchronize()

        # batch 计时（连续跑 iters 次，总时间除以次数——最稳定）
        # 注意：CUDA event 计时会包含 kernel launch 间隙（~3-5us），
        # 对于短 kernel（<20us）会偏高。精确测量请用 nsys 模式。
        # 多轮 batch 取统计
        times = []
        num_rounds = 30
        for _ in range(num_rounds):
            si = torch.cuda.Event(enable_timing=True)
            ei = torch.cuda.Event(enable_timing=True)
            si.record()
            for _ in range(args.iters):
                q(x)
            ei.record()
            torch.cuda.synchronize()
            times.append(si.elapsed_time(ei) * 1e3 / args.iters)

        times.sort()
        import statistics
        med = statistics.median(times)
        mn = times[0]
        mx = times[-1]
        mean = statistics.mean(times)

        print(f"{idx:>3} {name:<8} {M:>8} {K:>6} {mn:>10.3f} {mean:>10.3f} {med:>10.3f} {mx:>10.3f}")
        results.append({"idx": idx, "name": name, "M": M, "K": K,
                        "min": mn, "mean": mean, "median": med, "max": mx})

    print()
    print("注：大 shape（shape1/shape2）会触发 DVFS 降频（18-21%），")
    print("    实际满频性能约为测量值 × (满载频率/1965) ≈ 测量值 × 0.82。")

if __name__ == "__main__":
    main()
