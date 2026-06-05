file(REMOVE_RECURSE
  "libcutlass_gemm_sm89_bf16_s16832gemm_e4m3.a"
  "libcutlass_gemm_sm89_bf16_s16832gemm_e4m3.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm89_bf16_s16832gemm_e4m3_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
