file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_bf16_gemm_e2m1.a"
  "libcutlass_gemm_sm100_bf16_gemm_e2m1.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_bf16_gemm_e2m1_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
