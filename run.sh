#!/bin/bash
# 一键运行 doublewarp benchmark
#
# 用法:
#   bash run.sh                              # 跑所有 shape，fm0 和 fm1（CUDA event 计时）
#   bash run.sh --fm 1                       # 只跑 fast_math=on
#   bash run.sh --shape-idx 1                # 只跑 shape5 (idx=1)
#   bash run.sh --nsys --shape-idx 1 --fm 1  # nsys 采集（生成 .nsys-rep 到 ./nsys_reports/）
#
# 前提: 容器 te-base 运行中，且已部署编译（deploy_and_build.sh）

set -e
CONTAINER=te-base
TE_ROOT=/workspace/hadamard_opti/TransformerEngine
SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
BENCH_SCRIPT="$SCRIPT_DIR/scripts/benchmark.py"
NSYS_OUT_DIR="$SCRIPT_DIR/nsys_reports"

echo "=== doublewarp Hadamard+NVFP4 benchmark ==="

# 检查是否是 nsys 模式
IS_NSYS=false
SHAPE_IDX=""
FM_VAL=""
for arg in "$@"; do
    case $arg in
        --nsys) IS_NSYS=true ;;
        --shape-idx) shift_next=true ;;
        --fm) shift_next_fm=true ;;
    esac
    if [ "$shift_next" = "true" ] && [ "$arg" != "--shape-idx" ]; then
        SHAPE_IDX="$arg"; shift_next=false
    fi
    if [ "$shift_next_fm" = "true" ] && [ "$arg" != "--fm" ]; then
        FM_VAL="$arg"; shift_next_fm=false
    fi
done

if [ "$IS_NSYS" = "true" ]; then
    # === nsys 采集模式 ===
    if [ -z "$SHAPE_IDX" ]; then
        echo "ERROR: --nsys 需要 --shape-idx 参数"
        exit 1
    fi

    mkdir -p "$NSYS_OUT_DIR"

    # shape 名字
    SHAPE_NAMES=("shape8" "shape5" "shape9" "shape6" "shape7" "shape1" "shape2")
    NAME="${SHAPE_NAMES[$SHAPE_IDX]}"
    [ -z "$FM_VAL" ] && FM_VAL=0
    TAG="${NAME}_fm${FM_VAL}"

    echo "nsys 采集: $TAG"
    echo "输出: $NSYS_OUT_DIR/$TAG.nsys-rep"
    echo ""

    # 把 benchmark.py 拷进容器
    docker cp "$BENCH_SCRIPT" "$CONTAINER:/tmp/benchmark.py"

    # 在容器内用 nsys profile 运行
    docker exec -e PYTHONPATH="$TE_ROOT:$PYTHONPATH" -e NVTE_USE_FAST_MATH="$FM_VAL" \
        "$CONTAINER" bash -c "
        nsys profile -t cuda,nvtx --force-overwrite=true -o /tmp/$TAG \
            python3 /tmp/benchmark.py --nsys --shape-idx $SHAPE_IDX --fm $FM_VAL 2>/dev/null
    "

    # 拷回 host
    docker cp "$CONTAINER:/tmp/$TAG.nsys-rep" "$NSYS_OUT_DIR/$TAG.nsys-rep"

    # 提取 fusion kernel 时间
    NSYS_OUT_HOST="$NSYS_OUT_DIR"
    docker exec -e NSYS_OUT_HOST="$NSYS_OUT_HOST" "$CONTAINER" bash -c "
        nsys stats --force-export=true --report none /tmp/$TAG.nsys-rep > /dev/null 2>&1
        python3 -c \"
import sqlite3, statistics
db = sqlite3.connect('/tmp/$TAG.sqlite')
c = db.cursor()
pat = chr(37) + 'row_col_rht' + chr(37)
ids = c.execute('SELECT id FROM StringIds WHERE value LIKE ?', (pat,)).fetchall()
if ids:
    id_list = ','.join(str(i[0]) for i in ids)
    durs = [x[0]/1e3 for x in c.execute(f'SELECT (end-start) FROM CUPTI_ACTIVITY_KIND_KERNEL WHERE shortName IN ({id_list}) ORDER BY start').fetchall()]
    d = durs[5:] if len(durs)>10 else durs
    r = c.execute('SELECT MIN(start), MAX(end), COUNT(*) FROM CUPTI_ACTIVITY_KIND_KERNEL').fetchone()
    span = (r[1]-r[0])/1e6

    print()
    print('=' * 60)
    print('  RESULT: $TAG')
    print('=' * 60)
    print(f'  fusion kernel (row_col_rht_gemm_device):')
    print(f'    samples : {len(d)}')
    print(f'    min     : {min(d):.3f} us')
    print(f'    median  : {statistics.median(d):.3f} us')
    print(f'    mean    : {statistics.mean(d):.3f} us')
    print(f'    max     : {max(d):.3f} us')
    print(f'    stdev   : {statistics.stdev(d) if len(d)>1 else 0:.3f} us')
    print()
    print(f'  nsys session:')
    print(f'    total kernels : {r[2]}')
    print(f'    total span    : {span:.2f} ms')
    print()
    print(f'  report: $NSYS_OUT_HOST/$TAG.nsys-rep')
    print('=' * 60)
else:
    print('ERROR: fusion kernel not found')
\"
        rm -f /tmp/$TAG.sqlite
    "

else
    # === 普通 CUDA event 计时模式 ===
    echo ""
    if [ "$1" == "--fm" ] || [ "$1" == "--shape-idx" ]; then
        # 直接透传参数
        docker exec -e PYTHONPATH="$TE_ROOT:$PYTHONPATH" "$CONTAINER" \
            python3 -c "$(cat "$BENCH_SCRIPT")" "$@"
    else
        # 默认：fm0 和 fm1 各跑一遍
        echo "--- fast_math = OFF (fm0) ---"
        docker exec -e PYTHONPATH="$TE_ROOT:$PYTHONPATH" -e NVTE_USE_FAST_MATH=0 "$CONTAINER" \
            python3 -c "$(cat "$BENCH_SCRIPT")" --fm 0

        echo ""
        echo "--- fast_math = ON (fm1) ---"
        docker exec -e PYTHONPATH="$TE_ROOT:$PYTHONPATH" -e NVTE_USE_FAST_MATH=1 "$CONTAINER" \
            python3 -c "$(cat "$BENCH_SCRIPT")" --fm 1
    fi
fi
