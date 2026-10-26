#!/bin/bash
# Swap fusion.cu version, incremental rebuild, redeploy .so
# usage: bash swap_and_build.sh <path-to-cu>
set -e
SRC="$1"
TE=/workspace/hadamard_base/TransformerEngine
FUS=$TE/transformer_engine/common/hadamard_transform/row_cast_col_hadamard_transform_cast_fusion.cu
cp "$SRC" "$FUS"
echo "deployed: $(md5sum "$FUS")"
cd $TE/build/cmake
ninja transformer_engine 2>&1 | tail -1
cp libtransformer_engine.so $TE/transformer_engine/libtransformer_engine.so
echo "redeployed .so"
