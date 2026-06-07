# Copyright (c) 2014 - 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
# list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
# this list of conditions and the following disclaimer in the documentation
# and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# =============================================================
# 独立编译的 CMakeLists.txt
# cutlass 作为第三方库, 通过 add_subdirectory(../cutlass ...)引入
#
# 使用方式:
#   mkdir build && cd build
#   cmake .. -DCUTLASS_NVCC_ARCHS="100a"
#   make b200_blackwell_fmha_mxfp8 -j
# =============================================================

set_property(
  SOURCE
      b200_blackwell_fmha.cu
  PROPERTY
      COMPILE_FLAGS "--use_fast_math -lineinfo -ftemplate-backtrace-limit=0"
)

# --- 测试参数 (供手动运行参考) --------------------------------
set(TEST_BASIC --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=no)
set(TEST_CAUSAL_00 --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=causal)
set(TEST_CAUSAL_01 --verify --iterations=0 --b=1 --h=1 --h_k=1 --q=1013 --k=1024 --d=128 --mask=causal --causal-type=qend)
set(TEST_VARLEN --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=residual --varlen)
set(TEST_HDIM64 --b=2 --h=4 --q=512 --k=512 --d=64 --verify)
set(TEST_GQA --b=2 --h=4 --h_k=2 --q=512 --k=512 --d=64 --verify)

set(TEST_VARLEN_00 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_VARLEN_01 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_VARLEN_02 --verify --varlen --mask=causal,residual --d=128 --h=4 --h_k=2 --varlen-q=128 --varlen-k=128)
set(TEST_VARLEN_03 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=8 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_VARLEN_04 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=4 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_VARLEN_05 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=1 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_VARLEN_06 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=2 --varlen-q=256:256:256:256 --varlen-k=256:768:512:512)
set(TEST_VARLEN_07 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=2 --varlen-q=256:256:256:256 --varlen-k=256:0:1280:512)
set(TEST_VARLEN_08 --verify --varlen --mask=causal,residual --d=128 --h=8 --h_k=2 --varlen-q=256:0:512:256 --varlen-k=256:256:1024:512)
set(TEST_VARLEN_09 --verify --varlen --mask=causal,residual --d=64 --h=16 --h_k=16 --varlen-q=100:300 --varlen-k=100:300)
set(TEST_VARLEN_10 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=4 --varlen-q=3:2 --varlen-k=2:5)
set(TEST_VARLEN_11 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=2 --varlen-q=17:10 --varlen-k=13:10)
set(TEST_VARLEN_12 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=4 --varlen-q=177:845 --varlen-k=257:766)
set(TEST_VARLEN_13 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=2 --varlen-q=177:366:479 --varlen-k=257:0:766)
set(TEST_VARLEN_14 --verify --varlen --mask=causal,residual --d=64 --h=4 --h_k=4 --varlen-q=1 --varlen-k=1)
set(TEST_VARLEN_15 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_VARLEN_16 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=257)
set(TEST_VARLEN_17 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=25)
set(TEST_VARLEN_18 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_VARLEN_19 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=257)
set(TEST_VARLEN_20 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=25)
set(TEST_VARLEN_21 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=1013 --varlen-k=1024)
set(TEST_VARLEN_22 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=1024 --varlen-k=1035)



set(TEST_MLA_FWD_VARLEN_00 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_MLA_FWD_VARLEN_01 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_MLA_FWD_VARLEN_02 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=2 --varlen-q=128 --varlen-k=128)
set(TEST_MLA_FWD_VARLEN_03 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=8 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_MLA_FWD_VARLEN_04 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=4 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_MLA_FWD_VARLEN_05 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=1 --varlen-q=256:256 --varlen-k=512:512)
set(TEST_MLA_FWD_VARLEN_06 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=2 --varlen-q=256:256:256:256 --varlen-k=256:768:512:512)
set(TEST_MLA_FWD_VARLEN_07 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=2 --varlen-q=256:256:256:256 --varlen-k=256:0:1280:512)
set(TEST_MLA_FWD_VARLEN_08 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=8 --h_k=2 --varlen-q=256:0:512:256 --varlen-k=256:256:1024:512)
set(TEST_MLA_FWD_VARLEN_09 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=16 --h_k=16 --varlen-q=100:300 --varlen-k=100:300)
set(TEST_MLA_FWD_VARLEN_10 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=4 --varlen-q=2:3 --varlen-k=2:5)
set(TEST_MLA_FWD_VARLEN_11 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=2 --varlen-q=11:10 --varlen-k=13:10)
set(TEST_MLA_FWD_VARLEN_12 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=4 --varlen-q=177:766 --varlen-k=257:845)
set(TEST_MLA_FWD_VARLEN_13 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=2 --varlen-q=177:0:479 --varlen-k=257:0:766)
set(TEST_MLA_FWD_VARLEN_14 --verify --varlen --mask=causal,residual --dl=128 --dr=64 --h=4 --h_k=4 --varlen-q=1 --varlen-k=1)
set(TEST_MLA_FWD_VARLEN_15 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_MLA_FWD_VARLEN_16 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=257)
set(TEST_MLA_FWD_VARLEN_17 --verify --varlen --mask=causal --causal-type=qbegin --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=25)
set(TEST_MLA_FWD_VARLEN_18 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=128 --varlen-k=128)
set(TEST_MLA_FWD_VARLEN_19 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=257)
set(TEST_MLA_FWD_VARLEN_20 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=17 --varlen-k=25)
set(TEST_MLA_FWD_VARLEN_21 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=1013 --varlen-k=1024)
set(TEST_MLA_FWD_VARLEN_22 --verify --varlen --mask=causal --causal-type=qend --d=128 --h=4 --h_k=4 --varlen-q=1024 --varlen-k=1035)


set(TEST_GEN_BASIC --b=1 --h=4 --k=512 --d=128 --verify)
set(TEST_GEN_VARLEN --b=1 --h=4 --k=512 --d=128 --verify  --varlen)
set(TEST_GEN_HDIM64 --b=2 --h=4 --k=512 --d=64 --verify)
set(TEST_GEN_GQA --b=2 --h=4 --h_k=2 --k=512 --d=128 --verify)
set(TEST_GEN_REMAP --b=2 --h=4 --h_k=2 --k=512 --d=128 --verify --remap)
set(TEST_GEN_CACHEONLY --b=2 --h=4 --h_k=2 --k=512 --d=128 --verify --cache-only)

set(TEST_MLA_BASIC --b=1 --k=512 --page=128 --verify)
set(TEST_BWD_MLA_BASIC --b=1 --h=4 --q=512 --k=512 --d=192 --d_vo=128 --verify --mask=no)
set(TEST_BWD_MLA_VARLEN --b=1 --h=4 --q=512 --k=512 --d=192 --d_vo=128 --verify --mask=residual --varlen)

set(TEST_MLA_SEP_REDUCTION  --b=1 --k=4096 --split_kv=8 --page=128 --verify)
set(TEST_MLA_FUSE_REDUCTION --b=1 --k=4096 --split_kv=8 --page=128 --fuse_reduction --verify)

set(TEST_MLA_LARGE_SPLIT_KV --verify --split_kv=20 --page=128)
# -------------------------------------------------------------


if(NOT WIN32 AND (NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang")) AND (CUTLASS_NVCC_ARCHS MATCHES 100a OR CUTLASS_NVCC_ARCHS MATCHES 103a))

  foreach(PREC mxfp8)
    string(TOUPPER "${PREC}" PREC_MACRO)

    # 使用 cutlass 提供的函数创建可执行文件
    # （会自动设置 CUDA 编译选项和 gencode 标志）
    cutlass_add_executable(
        b200_blackwell_fmha_${PREC}
        b200_blackwell_fmha.cu
    )

    # [重要] 链接 CUTLASS (header-only)、工具库和 CUDA 运行时
    # cutlass_add_executable 只创建 target, 需要手动链接
    # cutlass_tools_util_includes 提供 cutlass/util/command_line.h 等工具头文件
    target_link_libraries(b200_blackwell_fmha_${PREC} PRIVATE
      CUTLASS
      cutlass_tools_util_includes
      cuda
    )

    # 添加自己的头文件路径 (collective/, kernel/, reference/, ...)
    target_include_directories(b200_blackwell_fmha_${PREC} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # 设置精度宏 (MXFP8)
    target_compile_definitions(b200_blackwell_fmha_${PREC} PRIVATE ${PREC_MACRO})

    # 可选: 打印寄存器/共享内存使用情况 (去掉 -v 可静默 ptxas 输出)
    # target_compile_options(b200_blackwell_fmha_${PREC} PRIVATE -Xptxas -v)
    target_compile_options(b200_blackwell_fmha_${PREC} PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>)

  endforeach()

  # 一键编译所有 target
  add_custom_target(b200_blackwell_fmha_all
    DEPENDS
      b200_blackwell_fmha_mxfp8
  )

  # 打印使用说明
  message(STATUS "========================================")
  message(STATUS "构建完成! 运行方式:")
  message(STATUS "  ./b200_blackwell_fmha_mxfp8/b200_blackwell_fmha_mxfp8 --b=1 --h=4 --q=512 --k=512 --d=128 --verify --mask=no")
  message(STATUS "========================================")

endif()
