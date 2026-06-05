file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_f16_gemm_f6_f4.pdb"
  "libcutlass_gemm_sm100_f16_gemm_f6_f4.so"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_f16_gemm_f6_f4.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
