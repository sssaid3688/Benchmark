file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_bf16_moe_gemm_f8.a"
  "libcutlass_gemm_sm100_bf16_moe_gemm_f8.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_bf16_moe_gemm_f8_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
