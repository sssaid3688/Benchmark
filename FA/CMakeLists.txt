cmake_minimum_required(VERSION 3.20)
project(FA LANGUAGES CUDA CXX)

# 将 cutlass 作为第三方库引入
# EXCLUDE_FROM_ALL 避免编译 cutlass 自带的 examples/tools/tests
add_subdirectory(../cutlass cutlass_build EXCLUDE_FROM_ALL)

# 编译 b200 项目
add_subdirectory(b200_blackwell_fmha_mxfp8)
