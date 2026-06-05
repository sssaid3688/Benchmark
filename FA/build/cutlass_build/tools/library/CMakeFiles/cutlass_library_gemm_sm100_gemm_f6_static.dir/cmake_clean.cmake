file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_gemm_f6.a"
  "libcutlass_gemm_sm100_gemm_f6.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_gemm_f6_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
