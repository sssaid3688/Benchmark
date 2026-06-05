file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_bf16_moe_gemm_f8.pdb"
  "libcutlass_gemm_sm100_bf16_moe_gemm_f8.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_bf16_moe_gemm_f8.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
