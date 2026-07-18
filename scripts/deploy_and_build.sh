#!/bin/bash
# 部署 doublewarp 源码到容器并编译
#
# 用法:
#   bash deploy_and_build.sh
#
# 前提:
#   - docker 容器 te-base 运行中
#   - 容器内 /workspace/hadamard_opti/TransformerEngine 已有 build 目录
#
# 部署后:
#   - 源码覆盖到 TE 的 fusion.cu
#   - 增量编译（只重编 fusion.cu + relink .so）
#   - md5 验证

set -e
CONTAINER=te-base
SRC_HOST="$(dirname "$(dirname "$(readlink -f "$0")")")/src/doublewarp_fusion.cu"
TE_ROOT=/workspace/hadamard_opti/TransformerEngine
SRC_REL=transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
SRC_PATH="$TE_ROOT/$SRC_REL"

echo "=== doublewarp 部署+编译 ==="
echo "源码: $SRC_HOST"
echo "目标: $SRC_PATH"
echo ""

# 1. 部署源码
echo "[1] 部署源码..."
docker cp "$SRC_HOST" "$CONTAINER:$SRC_PATH"

# 验证 md5
DEPLOYED_MD5=$(docker exec "$CONTAINER" md5sum "$SRC_PATH" | cut -d' ' -f1)
HOST_MD5=$(md5sum "$SRC_HOST" | cut -d' ' -f1)
if [ "$DEPLOYED_MD5" != "$HOST_MD5" ]; then
    echo "ERROR: md5 mismatch after deploy!"
    echo "  host:     $HOST_MD5"
    echo "  deployed: $DEPLOYED_MD5"
    exit 1
fi
echo "  md5: $DEPLOYED_MD5 (expected e6ff5d9c1896c34dbbbafbe02bdea0ca)"

# 2. 增量编译
echo ""
echo "[2] 增量编译..."
BUILD_DIR="$TE_ROOT/build/cmake"
OBJ="CMakeFiles/transformer_engine.dir/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu.o"

docker exec "$CONTAINER" bash -c "
    cd $BUILD_DIR
    rm -f $OBJ
    ninja -j30 transformer_engine 2>&1 | tail -3
    if [ \$? -ne 0 ]; then
        echo 'BUILD FAILED'
        exit 1
    fi
    cp libtransformer_engine.so $TE_ROOT/transformer_engine/libtransformer_engine.so
"

echo "  编译成功"

# 3. 验证
echo ""
echo "[3] 验证部署..."
SO_MD5=$(docker exec "$CONTAINER" md5sum "$TE_ROOT/transformer_engine/libtransformer_engine.so" | cut -d' ' -f1)
echo "  .so md5: $SO_MD5"

echo ""
echo "=== 部署完成 ==="
echo "运行 benchmark:"
echo "  cd /home/ubuntu/workspace/oyhj/hadamard_final"
echo "  bash run.sh"
