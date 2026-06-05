file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_gemm_f8_f4.a"
  "libcutlass_gemm_sm100_gemm_f8_f4.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_gemm_f8_f4_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
