file(REMOVE_RECURSE
  "libcutlass_gemm_sm100_cbf16gemm.a"
  "libcutlass_gemm_sm100_cbf16gemm.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CUDA)
  include(CMakeFiles/cutlass_library_gemm_sm100_cbf16gemm_static.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
